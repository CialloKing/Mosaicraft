#include "../cuda4dnn/kernels/cutlass_convolution.hpp"
#include "../cuda4dnn/csl/error.hpp"

#include <algorithm>
#include <cstdint>
#include <cutlass/conv/conv2d_problem_size.h>
#include <cutlass/conv/device/implicit_gemm_convolution.h>
#include <cutlass/conv/kernel/default_conv2d_fprop.h>
#include <cutlass/gemm/device/gemm.h>

namespace cv::dnn::cuda4dnn::kernels
{
namespace
{
using NHWC = cutlass::layout::TensorNHWC;
// SIMT uses full-precision FP32 multiply-adds; do not use TF32 or reduced-precision accumulators.
template <cutlass::conv::IteratorAlgorithm Iterator>
using Kernel = typename cutlass::conv::kernel::DefaultConv2dFprop<
    float, NHWC, float, NHWC, float, NHWC, float, cutlass::arch::OpClassSimt, cutlass::arch::Sm50,
    cutlass::gemm::GemmShape<64, 64, 8>, cutlass::gemm::GemmShape<32, 32, 8>,
    cutlass::gemm::GemmShape<1, 1, 1>, cutlass::epilogue::thread::LinearCombination<float, 1>,
    cutlass::gemm::threadblock::GemmIdentityThreadblockSwizzle<>, 2,
    cutlass::arch::OpMultiplyAdd, Iterator>::Kernel;

template <class Op>
void launch(Op &operation, const typename Op::Arguments &args, cudaStream_t stream)
{
    const auto status = operation(args, nullptr, stream);
    if (status != cutlass::Status::kSuccess)
    {
        // CUTLASS consumes the launch status; a sticky CUDA fault must still reach recovery.
        const auto error = cudaStreamQuery(stream);
        if (error != cudaErrorNotReady)
        {
            CUDA4DNN_CHECK_CUDA(error);
        }
        CV_Error(Error::GpuApiCallError, cutlassGetStatusString(status));
    }
}

template <cutlass::conv::IteratorAlgorithm Iterator>
bool implicitConv(const cutlass::conv::Conv2dProblemSize &shape, const cutlass::Tensor4DCoord &in,
                  const cutlass::Tensor4DCoord &weights, const cutlass::Tensor4DCoord &out,
                  const float *input, const float *filter, float *output, cudaStream_t stream)
{
    using Operation = cutlass::conv::device::ImplicitGemmConvolution<Kernel<Iterator>>;
    const auto layout = NHWC::packed(out);
    // CUTLASS 2.x convolution's TensorRef is not const-qualified for the read-only A/B operands.
    const typename Operation::Arguments args(shape, {const_cast<float *>(input), NHWC::packed(in)},
                                             {const_cast<float *>(filter), NHWC::packed(weights)},
                                             {output, layout}, {output, layout}, {1.f, 0.f});
    Operation operation;
    if (operation.can_implement(args) != cutlass::Status::kSuccess)
    {
        return false; // Unhandled shape only; a launch failure must reach the session's recovery logic.
    }
    launch(operation, args, stream);
    return true;
}

template <bool Depthwise>
__global__ void directConv(CropConv2d s, const float *input, const float *filter, float *output, int count)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count)
    {
        return;
    }
    const int x = i % s.outW, y = i / s.outW % s.outH;
    const int k = i / (s.outW * s.outH) % s.outC;
    const int n = i / (s.outW * s.outH * s.outC);
    const int channels = Depthwise ? 1 : s.inC;
    const int firstChannel = Depthwise ? k : 0;
    const int baseY = y * s.strideY - s.top, baseX = x * s.strideX - s.left;
    float sum = 0.f;
    for (int r = 0; r < s.kernel; ++r)
    {
        const int sy = baseY + r * s.dilationY;
        if (sy < 0 || sy >= s.inH)
        {
            continue;
        }
        for (int t = 0; t < s.kernel; ++t)
        {
            const int sx = baseX + t * s.dilationX;
            if (sx < 0 || sx >= s.inW)
            {
                continue;
            }
            const auto *w = filter + ((k * s.kernel + r) * s.kernel + t) * channels;
            for (int c = 0; c < channels; ++c)
            {
                const int offset = ((n * s.inC + firstChannel + c) * s.inH + sy) * s.inW + sx;
                sum = fmaf(input[offset], w[c], sum);
            }
        }
    }
    output[i] = sum;
}

bool implicitCropConv(const CropConv2d &s, const float *input, const float *filter,
                      float *output, cudaStream_t stream)
{
    const cutlass::Tensor4DCoord in(s.batches, s.inH, s.inW, s.inC);
    const cutlass::Tensor4DCoord weights(s.outC, s.kernel, s.kernel, s.inC);
    const cutlass::Tensor4DCoord out(s.batches, s.outH, s.outW, s.outC);
    const cutlass::conv::Conv2dProblemSize shape(in, weights, {s.top, s.bottom, s.left, s.right},
                                                {s.strideY, s.strideX}, {s.dilationY, s.dilationX}, out);
    // Use the precomputed iterator for the common case and keep analytic dilation on the same stream.
    if (s.dilationY == 1 && s.dilationX == 1 &&
        implicitConv<cutlass::conv::IteratorAlgorithm::kOptimized>(shape, in, weights, out, input, filter, output, stream))
    {
        return true;
    }
    return implicitConv<cutlass::conv::IteratorAlgorithm::kAnalytic>(shape, in, weights, out, input, filter, output, stream);
}

constexpr int transposeWidth = 32, transposeWarpRows = 8;

// Both passes coalesce global reads and writes; the extra column avoids shared-memory bank conflicts.
__global__ void transposeCropTile(const float *src, float *dst, int width, int height, int srcStride, int dstStride)
{
    __shared__ float values[transposeWidth][transposeWidth + 1];
    const int x = blockIdx.x * transposeWidth + threadIdx.x;
    const int y = blockIdx.y * transposeWidth + threadIdx.y;
    for (int row = 0; row < transposeWidth; row += transposeWarpRows)
    {
        if (x < width && y + row < height)
        {
            values[threadIdx.y + row][threadIdx.x] = src[(y + row) * srcStride + x];
        }
    }
    __syncthreads();
    const int outX = blockIdx.y * transposeWidth + threadIdx.x;
    const int outY = blockIdx.x * transposeWidth + threadIdx.y;
    for (int row = 0; row < transposeWidth; row += transposeWarpRows)
    {
        if (outX < height && outY + row < width)
        {
            dst[(outY + row) * dstStride + outX] = values[threadIdx.x][threadIdx.y + row];
        }
    }
}

void transposeCrop(const float *src, float *dst, int width, int height, int srcStride, int dstStride,
                   cudaStream_t stream)
{
    // A wide image after transposition can exceed grid.y's limit.
    const int xTiles = (width - 1) / transposeWidth + 1;
    const int yTiles = (height - 1) / transposeWidth + 1;
    const auto block = dim3(transposeWidth, transposeWarpRows);
    for (int y = 0; y < yTiles;)
    {
        const int rows = std::min(yTiles - y, 65535);
        const int offset = y * transposeWidth;
        const int tileHeight = yTiles - y == rows ? height - offset : rows * transposeWidth;
        transposeCropTile<<<dim3(xTiles, rows), block, 0, stream>>>(
            src + std::size_t(offset) * srcStride, dst + offset, width, tileHeight, srcStride, dstStride);
        CUDA4DNN_CHECK_CUDA(cudaGetLastError());
        y += rows;
    }
}
}

CropConvScratch crop_conv_scratch(const CropConv2d &s)
{
    // Bound each graph's layout workspace independently of the source image resolution.
    constexpr std::size_t maxElements = (64u * 1024u * 1024u) / sizeof(float) - 2 * (256u / sizeof(float));
    const std::size_t inRow = std::size_t(s.inW) * s.inC;
    const std::size_t outRow = std::size_t(s.outW) * s.outC;
    const std::size_t receptive = 1u + std::size_t(s.kernel - 1) * s.dilationY;
    const auto forRows = [&](int rows)
    {
        const auto needed = std::size_t(rows - 1) * s.strideY + receptive;
        return CropConvScratch{rows, std::min(std::size_t(s.inH), needed) * inRow, rows * outRow};
    };
    if (const auto full = forRows(s.outH); full.inputElements + full.outputElements <= maxElements)
    {
        return full;
    }
    const auto first = forRows(1);
    const auto firstSize = first.inputElements + first.outputElements;
    if (firstSize > maxElements)
    {
        return {}; // One full-width receptive field exceeds the budget; use the direct kernel.
    }
    const auto extraRows = (maxElements - firstSize) / (inRow * s.strideY + outRow);
    return forRows(1 + static_cast<int>(extraRows));
}

bool cutlass_crop_conv(const CropConv2d &s, const CropConvScratch &scratch, const float *input,
                       const float *filter, float *output, float *inputTile, float *outputTile, cudaStream_t stream)
{
    const int inPlane = s.inH * s.inW, outPlane = s.outH * s.outW;
    const std::int64_t receptive = 1 + std::int64_t(s.kernel - 1) * s.dilationY;
    for (int n = 0; n < s.batches; ++n)
    {
        const auto *src = input + n * s.inC * inPlane;
        auto *dst = output + n * s.outC * outPlane;
        for (int y = 0; y < s.outH;)
        {
            auto tile = s;
            tile.batches = 1;
            tile.outH = std::min(scratch.outputRows, s.outH - y);
            const std::int64_t first = std::int64_t(y) * s.strideY - s.top;
            const std::int64_t end = first + std::int64_t(tile.outH - 1) * s.strideY + receptive;
            const auto begin = std::max<std::int64_t>(0, first);
            const auto limit = std::min<std::int64_t>(s.inH, end);
            if (begin >= limit)
            {
                return false;
            }
            // Keep the original receptive fields, including dilation and only the padding beyond the image.
            tile.inH = static_cast<int>(limit - begin);
            tile.top = static_cast<int>(begin - first);
            tile.bottom = static_cast<int>(end - limit);
            const int inPixels = tile.inH * s.inW, outPixels = tile.outH * s.outW;
            transposeCrop(src + begin * s.inW, inputTile, inPixels, s.inC, inPlane, s.inC, stream);
            if (!implicitCropConv(tile, inputTile, filter, outputTile, stream))
            {
                return false;
            }
            transposeCrop(outputTile, dst + y * s.outW, s.outC, outPixels, s.outC, outPlane, stream);
            y += tile.outH;
        }
    }
    return true;
}

bool pointwise_crop_conv(const CropConv2d &s, const float *input, const float *filter,
                         float *output, cudaStream_t stream)
{
    // Stride-one, unpadded 1x1 implicit GEMM needs no gather or layout conversions.
    using RowMajor = cutlass::layout::RowMajor;
    using Gemm = cutlass::gemm::device::Gemm<float, RowMajor, float, RowMajor, float, RowMajor, float,
        cutlass::arch::OpClassSimt, cutlass::arch::Sm50,
        cutlass::gemm::GemmShape<32, 128, 8>, cutlass::gemm::GemmShape<16, 64, 8>>;
    const int hw = s.outH * s.outW;
    for (int n = 0; n < s.batches; ++n)
    {
        float *dest = output + n * s.outC * hw;
        const Gemm::Arguments args({s.outC, hw, s.inC}, {filter, s.inC},
                                   {input + n * s.inC * hw, hw}, {dest, hw}, {dest, hw}, {1.f, 0.f});
        Gemm operation;
        if (operation.can_implement(args) != cutlass::Status::kSuccess)
        {
            return false;
        }
        launch(operation, args, stream);
    }
    return true;
}

void direct_crop_conv(const CropConv2d &s, const float *input, const float *filter, float *output,
                      cudaStream_t stream)
{
    const int count = s.batches * s.outC * s.outH * s.outW;
    constexpr int threads = 256;
    const int blocks = (count - 1) / threads + 1;
    if (s.groups != 1)
    {
        directConv<true><<<blocks, threads, 0, stream>>>(s, input, filter, output, count);
    }
    else
    {
        directConv<false><<<blocks, threads, 0, stream>>>(s, input, filter, output, count);
    }
    CUDA4DNN_CHECK_CUDA(cudaGetLastError());
}
}
