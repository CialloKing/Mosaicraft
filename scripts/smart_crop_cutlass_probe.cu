#include <cuda_runtime.h>
#include <cutlass/conv/conv2d_problem_size.h>
#include <cutlass/conv/device/implicit_gemm_convolution.h>
#include <cutlass/conv/kernel/default_conv2d_fprop.h>
#include <cutlass/gemm/device/gemm.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
void check(cudaError_t status, const char *step)
{
    if (status != cudaSuccess)
    {
        std::fprintf(stderr, "%s: %s\n", step, cudaGetErrorString(status));
        std::exit(EXIT_FAILURE);
    }
}

void check(cutlass::Status status, const char *step)
{
    if (status != cutlass::Status::kSuccess)
    {
        std::fprintf(stderr, "%s: %s\n", step, cutlassGetStatusString(status));
        std::exit(EXIT_FAILURE);
    }
}

class Buffer
{
public:
    Buffer(int count, float value) : m_count(count)
    {
        check(cudaMalloc(&m_data, count * sizeof(float)), "cudaMalloc");
        const std::vector<float> host(count, value);
        check(cudaMemcpy(m_data, host.data(), count * sizeof(float), cudaMemcpyHostToDevice), "copy input");
    }

    ~Buffer()
    {
        cudaFree(m_data);
    }

    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;

    float *data() const
    {
        return m_data;
    }

    void expect(float value) const
    {
        std::vector<float> host(m_count);
        check(cudaMemcpy(host.data(), m_data, m_count * sizeof(float), cudaMemcpyDeviceToHost), "read output");
        if (!std::all_of(host.begin(), host.end(), [value](float x)
        {
            return x == value;
        }))
        {
            std::fprintf(stderr, "wrong result: expected %g\n", value);
            std::exit(EXIT_FAILURE);
        }
    }

private:
    int m_count;
    float *m_data = nullptr;
};

using RowMajor = cutlass::layout::RowMajor;
using Gemm = cutlass::gemm::device::Gemm<float, RowMajor, float, RowMajor, float, RowMajor, float,
                                        cutlass::arch::OpClassSimt, cutlass::arch::Sm50>;

using NHWC = cutlass::layout::TensorNHWC;
using ConvolutionKernel = cutlass::conv::kernel::DefaultConv2dFprop<
    float, NHWC, float, NHWC, float, NHWC, float, cutlass::arch::OpClassSimt, cutlass::arch::Sm50,
    cutlass::gemm::GemmShape<128, 128, 8>, cutlass::gemm::GemmShape<64, 64, 8>,
    cutlass::gemm::GemmShape<1, 1, 1>, cutlass::epilogue::thread::LinearCombination<float, 1>,
    cutlass::gemm::threadblock::GemmIdentityThreadblockSwizzle<>, 2,
    cutlass::arch::OpMultiplyAdd, cutlass::conv::IteratorAlgorithm::kAnalytic>::Kernel;
using Convolution = cutlass::conv::device::ImplicitGemmConvolution<ConvolutionKernel>;

void gemm(cudaStream_t stream)
{
    const Buffer a(5 * 7, 2.f), b(7 * 11, 3.f), c(5 * 11, -1.f);
    Gemm::Arguments args({5, 11, 7}, {a.data(), 7}, {b.data(), 11}, {c.data(), 11}, {c.data(), 11}, {1.f, 0.f});
    Gemm operation;
    check(operation.can_implement(args), "GEMM plan");
    check(operation(args, nullptr, stream), "GEMM launch");
    check(cudaStreamSynchronize(stream), "GEMM execution");
    c.expect(42.f);
}

void convolution(int dilation, cudaStream_t stream)
{
    const cutlass::Tensor4DCoord in(1, 8, 9, 3), filter(5, 3, 3, 3);
    const cutlass::Tensor4DCoord out(1, 8 - 2 * dilation, 9 - 2 * dilation, 5);
    const Buffer a(in.product(), 2.f), b(filter.product(), 3.f), c(out.product(), -1.f);
    const auto layout = NHWC::packed(out);
    const cutlass::conv::Conv2dProblemSize shape(in, filter, {0, 0, 0, 0}, {1, 1},
                                                {dilation, dilation}, out);
    const Convolution::Arguments args(shape, {a.data(), NHWC::packed(in)}, {b.data(), NHWC::packed(filter)},
                                      {c.data(), layout}, {c.data(), layout}, {1.f, 0.f});
    Convolution operation;
    check(operation.can_implement(args), "convolution plan");
    check(operation(args, nullptr, stream), "convolution launch");
    check(cudaStreamSynchronize(stream), "convolution execution");
    c.expect(162.f);
}
}

int main()
{
    cudaDeviceProp device{};
    check(cudaGetDeviceProperties(&device, 0), "select device");
    if (device.major != 8 || device.minor != 9)
    {
        std::fprintf(stderr, "expected sm_89, got sm_%d%d\n", device.major, device.minor);
        return EXIT_FAILURE;
    }
    cudaStream_t stream{};
    check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "create stream");
    gemm(stream);
    convolution(1, stream);
    convolution(2, stream);
    check(cudaStreamDestroy(stream), "destroy stream");
    std::printf("PASS: FP32 GEMM, 3x3 convolution and dilated convolution on %s (sm_89)\n", device.name);
}
