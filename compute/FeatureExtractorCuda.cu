#include "FeatureExtractorCuda.h"

#include "DeviceBuffer.cuh"
#include "core/FeatureExtractor.h"
#include "core/FeatureUtils.h"
#include "core/UnicodeIO.h"
#include "core/WorkerPool.h"
#include <cuda_runtime.h>
#include <opencv2/imgproc.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace mosaicraft
{
namespace cuda
{

namespace
{

} // namespace

struct FeaturePipeline
{
    FeaturePipelineConfig config;
    PinnedBuffer input, download;
    DeviceBuffer<uint8_t> images, tiny;
    DeviceBuffer<float> grid, lbp;
    DeviceBuffer<double> lab, bright, contrast, edge;
    std::shared_ptr<DeviceFeatureState> state = std::make_shared<DeviceFeatureState>();
    std::unique_ptr<WorkerPool> workers;
    TransferStream transfer;
};

struct FeatureWorkspace::Impl
{
    DeviceBuffer<uint8_t> images, tiny;
    DeviceBuffer<float> grid, lbp;
    DeviceBuffer<double> lab, bright, contrast, edge;
    std::vector<uint8_t> h_images, h_tiny;
    std::vector<float> h_grid, h_lbp;
    std::vector<double> h_lab, h_bright, h_contrast, h_edge;
    std::unique_ptr<WorkerPool> cpuWorkers;
    std::unique_ptr<FeaturePipeline> pipeline;
};
FeatureWorkspace::FeatureWorkspace() : buffers(std::make_unique<Impl>())
{
}
FeatureWorkspace::~FeatureWorkspace() = default;

bool FeatureWorkspace::preparePipeline(const FeaturePipelineConfig &config, GpuTimings *timings)
{
    resetPipeline();
    const bool supported = (config.width == 180 && config.height == 320) ||
                           (config.width == 320 && config.height == 180) ||
                           (config.width == 360 && config.height == 640) ||
                           (config.width == 640 && config.height == 360);
    if (!supported || config.maxTiles <= 0 || config.maxTiles > 1024)
    {
        throw std::invalid_argument("invalid CUDA feature pipeline dimensions");
    }
    PhaseTimer timer(timings ? &timings->allocationMs : nullptr);
    const size_t rows = config.maxTiles, chunk = std::min<size_t>(rows, 256);
    const size_t pixels = chunk * config.width * config.height * 3;
    const size_t download = chunk * (3 * sizeof(double) + 192 * sizeof(float) +
                                     (config.downloadTiny ? 256 : 0) +
                                     (config.downloadLbp ? 256 * sizeof(float) : 0));
    const size_t resident = rows * (448 * sizeof(float) + 256);
    size_t freeBytes = 0, totalBytes = 0;
    requireCuda(cudaMemGetInfo(&freeBytes, &totalBytes));
    if (pixels + download > config.hostBudget || resident > config.deviceBudget ||
        pixels + resident + chunk * 6 * sizeof(double) > freeBytes / 2)
    {
        return false;
    }
    try
    {
        auto p = std::make_unique<FeaturePipeline>();
        p->config = config;
        if (!optionalCudaAllocation(p->input.allocate(pixels)) ||
            config.testFault == PipelineFault::PinnedAllocation ||
            !optionalCudaAllocation(p->download.allocate(download)) ||
            !optionalCudaAllocation(p->images.allocate(pixels)) ||
            config.testFault == PipelineFault::DeviceAllocation ||
            !optionalCudaAllocation(p->grid.allocate(rows * 192 * sizeof(float))) ||
            !optionalCudaAllocation(p->tiny.allocate(rows * 256)) ||
            !optionalCudaAllocation(p->lbp.allocate(rows * 256 * sizeof(float))) ||
            !optionalCudaAllocation(p->lab.allocate(chunk * 3 * sizeof(double))) ||
            !optionalCudaAllocation(p->bright.allocate(chunk * sizeof(double))) ||
            !optionalCudaAllocation(p->contrast.allocate(chunk * sizeof(double))) ||
            !optionalCudaAllocation(p->edge.allocate(chunk * sizeof(double))) || !p->transfer.create())
        {
            return false;
        }
        p->workers = std::make_unique<WorkerPool>();
        p->state->grid = p->grid.get();
        p->state->lbp = p->lbp.get();
        p->state->tiny = p->tiny.get();
        p->state->bytes = resident;
        buffers->pipeline = std::move(p);
        if (timings)
        {
            timings->pinnedBytes = pinnedBytes();
            timings->residentBytes = residentBytes();
            timings->pixelBytes = pixels;
        }
        return true;
    }
    catch (const std::bad_alloc &)
    {
        return false;
    }
}

void FeatureWorkspace::resetPipeline()
{
    buffers->pipeline.reset();
}

uint8_t *FeatureWorkspace::pipelineInput()
{
    return buffers->pipeline ? buffers->pipeline->input.get() : nullptr;
}

size_t FeatureWorkspace::pinnedBytes() const
{
    const auto *p = buffers->pipeline.get();
    return p ? p->input.capacityBytes() + p->download.capacityBytes() : 0;
}

size_t FeatureWorkspace::residentBytes() const
{
    return buffers->pipeline ? buffers->pipeline->state->bytes : 0;
}

void FeatureWorkspace::beginBatch(int count)
{
    auto *p = buffers->pipeline.get();
    if (!p || count <= 0 || count > p->config.maxTiles)
    {
        throw std::invalid_argument("invalid CUDA feature batch");
    }
    ++p->state->generation;
    p->state->count = count;
    p->state->ready = 0;
}

DeviceFeatureView FeatureWorkspace::deviceView(int first, int count) const
{
    const auto *p = buffers->pipeline.get();
    if (!p || first < 0 || count <= 0 || first > p->state->ready || count > p->state->ready - first)
    {
        throw std::invalid_argument("CUDA device features are not ready");
    }
    return {p->state, p->state->generation, first, count};
}

// ============================================================
// GPU 常量：归一化图尺寸 — 模板化以支持常见分辨率
// ============================================================
template <int W, int H> struct GpuParams
{
    static constexpr int IMG_W = W;
    static constexpr int IMG_H = H;
    static constexpr int IMG_PIX = W * H;
    static constexpr int GRID_CW = (W + 7) / 8;
    static constexpr int GRID_CH = (H + 7) / 8;
    static constexpr int GRID_CELLS = 64;
    static constexpr int TINY_SX = W / 16; // TinyImage 采样步长 X
    static constexpr int TINY_SY = H / 16; // TinyImage 采样步长 Y
};
static constexpr int LBP_BINS = 256;

// ============================================================
// 工具：RGB→LAB（GPU device function）
// ============================================================
__device__ void rgb2lab(float r, float g, float b, float &l, float &a, float &bb)
{
    // sRGB → XYZ
    float varR = r / 255.0f, varG = g / 255.0f, varB = b / 255.0f;
    varR = (varR > 0.04045f) ? powf((varR + 0.055f) / 1.055f, 2.4f) : varR / 12.92f;
    varG = (varG > 0.04045f) ? powf((varG + 0.055f) / 1.055f, 2.4f) : varG / 12.92f;
    varB = (varB > 0.04045f) ? powf((varB + 0.055f) / 1.055f, 2.4f) : varB / 12.92f;
    varR *= 100.0f;
    varG *= 100.0f;
    varB *= 100.0f;

    float x = varR * 0.4124f + varG * 0.3576f + varB * 0.1805f;
    float y = varR * 0.2126f + varG * 0.7152f + varB * 0.0722f;
    float z = varR * 0.0193f + varG * 0.1192f + varB * 0.9505f;

    float refX = 95.047f, refY = 100.0f, refZ = 108.883f;
    float vx = x / refX, vy = y / refY, vz = z / refZ;
    vx = (vx > 0.008856f) ? powf(vx, 1.0f / 3.0f) : (7.787f * vx + 16.0f / 116.0f);
    vy = (vy > 0.008856f) ? powf(vy, 1.0f / 3.0f) : (7.787f * vy + 16.0f / 116.0f);
    vz = (vz > 0.008856f) ? powf(vz, 1.0f / 3.0f) : (7.787f * vz + 16.0f / 116.0f);

    l = (116.0f * vy - 16.0f) * 2.55f; // OpenCV scale
    a = 500.0f * (vx - vy) + 128.0f;
    bb = 200.0f * (vy - vz) + 128.0f;
}

__device__ float rgb2gray(float r, float g, float b)
{
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

// ============================================================
// 主 kernel：模板化，每个 block 处理一张图片
// 模板参数 W, H = 图像宽高
// ============================================================
template <int W, int H>
__global__ void featureKernel(const uint8_t *__restrict__ d_images, float *__restrict__ d_grid,
                              uint8_t *__restrict__ d_tiny, float *__restrict__ d_lbp, double *__restrict__ d_avgLAB,
                              double *__restrict__ d_brightness, double *__restrict__ d_contrast,
                              double *__restrict__ d_edgeDensity, int batchSize)
{
    constexpr int IMG_W = W;
    constexpr int IMG_H = H;
    constexpr int IMG_PIX = W * H;
    constexpr int GRID_CW = (W + 7) / 8;
    constexpr int GRID_CH = (H + 7) / 8;
    constexpr int GRID_CELLS = 64;
    constexpr int TINY_SX = W / 16;
    constexpr int TINY_SY = H / 16;

    int imgIdx = blockIdx.x;
    if (imgIdx >= batchSize)
    {
        return;
    }

    int tx = threadIdx.x;
    const uint8_t *img = d_images + imgIdx * IMG_PIX * 3;

    __shared__ float s_gridLab[GRID_CELLS * 3];
    __shared__ float s_grayAcc;
    __shared__ float s_graySqAcc;
    __shared__ int s_edgeCount;
    __shared__ uint32_t s_lbpHist[LBP_BINS];

    // 跨步清零全部 192 个 Grid cell LAB 累加器（block 可能少于 192 线程）
    for (int i = tx; i < GRID_CELLS * 3; i += blockDim.x)
    {
        s_gridLab[i] = 0.0f;
    }
    if (tx == 0)
    {
        s_grayAcc = 0;
        s_graySqAcc = 0;
        s_edgeCount = 0;
    }
    // 跨步清零全部256个LBP bin（block可能少于256线程）
    for (int i = tx; i < LBP_BINS; i += blockDim.x)
    {
        s_lbpHist[i] = 0;
    }
    __syncthreads();

    float localGrayAcc = 0, localGraySq = 0;
    int localEdge = 0;

    for (int y = 0; y < IMG_H; ++y)
    {
        int x = tx;
        if (x >= IMG_W)
        {
            continue;
        }

        int idx = (y * IMG_W + x) * 3;
        float b = img[idx];
        float g = img[idx + 1];
        float r = img[idx + 2];

        float lv, av, bv;
        rgb2lab(r, g, b, lv, av, bv);

        int cellX = x / GRID_CW;
        int cellY = y / GRID_CH;
        if (cellX >= 8)
        {
            cellX = 7;
        }
        if (cellY >= 8)
        {
            cellY = 7;
        }
        int cellIdx = cellY * 8 + cellX;
        atomicAdd(&s_gridLab[cellIdx * 3 + 0], lv);
        atomicAdd(&s_gridLab[cellIdx * 3 + 1], av);
        atomicAdd(&s_gridLab[cellIdx * 3 + 2], bv);

        float gray = rgb2gray(r, g, b);
        localGrayAcc += gray;
        localGraySq += gray * gray;

        if (x > 0 && y > 0)
        {
            float center = gray;
            float right = rgb2gray(img[(y * IMG_W + x - 1) * 3 + 2], img[(y * IMG_W + x - 1) * 3 + 1],
                                   img[(y * IMG_W + x - 1) * 3]);
            float down = rgb2gray(img[((y - 1) * IMG_W + x) * 3 + 2], img[((y - 1) * IMG_W + x) * 3 + 1],
                                  img[((y - 1) * IMG_W + x) * 3]);
            float grad = fabsf(center - right) + fabsf(center - down);
            if (grad > 30.0f)
            {
                localEdge++;
            }
        }

        if (x > 0 && x < IMG_W - 1 && y > 0 && y < IMG_H - 1)
        {
            uint8_t code = 0;
            float c = rgb2gray(img[((y)*IMG_W + x) * 3 + 2], img[((y)*IMG_W + x) * 3 + 1], img[((y)*IMG_W + x) * 3]);
            auto gv = [&](int dy, int dx)
            {
                float v = rgb2gray(img[((y + dy) * IMG_W + (x + dx)) * 3 + 2],
                                   img[((y + dy) * IMG_W + (x + dx)) * 3 + 1], img[((y + dy) * IMG_W + (x + dx)) * 3]);
                return v >= c ? 1.0f : 0.0f;
            };
            if (gv(-1, -1))
            {
                code |= 1;
            }
            if (gv(-1, 0))
            {
                code |= 2;
            }
            if (gv(-1, 1))
            {
                code |= 4;
            }
            if (gv(0, 1))
            {
                code |= 8;
            }
            if (gv(1, 1))
            {
                code |= 16;
            }
            if (gv(1, 0))
            {
                code |= 32;
            }
            if (gv(1, -1))
            {
                code |= 64;
            }
            if (gv(0, -1))
            {
                code |= 128;
            }
            atomicAdd(&s_lbpHist[code], 1u);
        }
    }

    atomicAdd(&s_grayAcc, localGrayAcc);
    atomicAdd(&s_graySqAcc, localGraySq);
    atomicAdd(&s_edgeCount, localEdge);
    __syncthreads();

    if (tx == 0)
    {
        float nPix = IMG_PIX;
        float grayMean = s_grayAcc / nPix;
        float grayVar = s_graySqAcc / nPix - grayMean * grayMean;

        d_brightness[imgIdx] = grayMean;
        d_contrast[imgIdx] = (grayVar > 0 ? sqrtf(grayVar) : 0.0f) / 255.0f;
        d_edgeDensity[imgIdx] = static_cast<double>(s_edgeCount) / nPix;

        int outOff = imgIdx * 192;
        for (int c = 0; c < GRID_CELLS; ++c)
        {
            int cellCol = c % 8;
            int cellRow = c / 8;
            int cw = (cellCol == 7) ? (W - 7 * GRID_CW) : GRID_CW;
            int ch = (cellRow == 7) ? (H - 7 * GRID_CH) : GRID_CH;
            float cnt = cw * ch;
            d_grid[outOff + c * 3 + 0] = s_gridLab[c * 3 + 0] / cnt;
            d_grid[outOff + c * 3 + 1] = s_gridLab[c * 3 + 1] / cnt;
            d_grid[outOff + c * 3 + 2] = s_gridLab[c * 3 + 2] / cnt;
        }

        float totalL = 0, totalA = 0, totalB = 0;
        for (int c = 0; c < GRID_CELLS; ++c)
        {
            totalL += s_gridLab[c * 3 + 0];
            totalA += s_gridLab[c * 3 + 1];
            totalB += s_gridLab[c * 3 + 2];
        }
        d_avgLAB[imgIdx * 3 + 0] = totalL / nPix;
        d_avgLAB[imgIdx * 3 + 1] = totalA / nPix;
        d_avgLAB[imgIdx * 3 + 2] = totalB / nPix;

        int lbpOff = imgIdx * LBP_BINS;
        float lbpSum = 0;
        for (int i = 0; i < LBP_BINS; ++i)
        {
            lbpSum += s_lbpHist[i];
        }
        if (lbpSum > 0)
        {
            for (int i = 0; i < LBP_BINS; ++i)
            {
                d_lbp[lbpOff + i] = s_lbpHist[i] / lbpSum;
            }
        }
        else
        {
            for (int i = 0; i < LBP_BINS; ++i)
            {
                d_lbp[lbpOff + i] = 0;
            }
        }
    }

    // TinyImage: loop over all 256 pixels, works with any blockDim
    for (int ti = tx; ti < 256; ti += blockDim.x)
    {
        int ty = ti / 16;
        int ttx = ti % 16;
        int srcX0 = ttx * TINY_SX;
        int srcY0 = ty * TINY_SY;
        float sum = 0;
        int cnt = 0;
        for (int dy = 0; dy < TINY_SY && (srcY0 + dy) < IMG_H; ++dy)
        {
            for (int dx = 0; dx < TINY_SX && (srcX0 + dx) < IMG_W; ++dx)
            {
                int idx2 = ((srcY0 + dy) * IMG_W + (srcX0 + dx)) * 3;
                sum += rgb2gray(img[idx2 + 2], img[idx2 + 1], img[idx2]);
                cnt++;
            }
        }
        d_tiny[imgIdx * 256 + ti] = static_cast<uint8_t>(sum / cnt);
    }
}

// 显式实例化：支持的四种分辨率

// ============================================================
// 主机接口
// ============================================================
int extractBatch(const std::vector<cv::Mat> &images, std::vector<ImageRecord> &records, const std::string &featDir,
                 const std::vector<std::string> &stems, FeatureWorkspace *workspace)
{
    int N = static_cast<int>(images.size());
    if (N <= 0)
    {
        return 0;
    }
    if (images[0].empty())
    {
        return 0;
    }

    // 从实际图像读取尺寸
    int imgW = images[0].cols;
    int imgH = images[0].rows;
    if (!((imgW == 180 && imgH == 320) || (imgW == 320 && imgH == 180) ||
          (imgW == 360 && imgH == 640) || (imgW == 640 && imgH == 360)))
    {
        return 0;
    }
    size_t imgBytes = static_cast<size_t>(imgW) * imgH * 3;

    // 上传图像到 GPU
    FeatureWorkspace local;
    auto &buffers = *(workspace ? workspace : &local)->buffers;
    auto &h_images = buffers.h_images;
    h_images.resize(N * imgBytes);
    for (int i = 0; i < N; ++i)
    {
        cv::Mat bgr;
        if (images[i].channels() == 4)
        {
            cv::cvtColor(images[i], bgr, cv::COLOR_BGRA2BGR);
        }
        else if (images[i].channels() == 1)
        {
            cv::cvtColor(images[i], bgr, cv::COLOR_GRAY2BGR);
        }
        else
        {
            bgr = images[i];
        }
        if (bgr.empty() || bgr.cols != imgW || bgr.rows != imgH || bgr.channels() != 3)
        {
            fprintf(stderr, "GPU build: image mismatch at index %d (%dx%d ch=%d vs %dx%d ch=3)\n", i, bgr.cols,
                    bgr.rows, bgr.channels(), imgW, imgH);
            return 0;
        }
        std::memcpy(&h_images[i * imgBytes], bgr.data, imgBytes);
    }

    auto &d_images = buffers.images;
    auto &d_grid = buffers.grid;
    auto &d_tiny = buffers.tiny;
    auto &d_lbp = buffers.lbp;
    auto &d_avgLAB = buffers.lab;
    auto &d_bright = buffers.bright;
    auto &d_contrast = buffers.contrast;
    auto &d_edge = buffers.edge;

    auto allocOrFail = [&](auto &buf, size_t bytes, const char *label)
    {
        if (buf.allocate(bytes) == cudaSuccess)
        {
            return true;
        }
        fprintf(stderr, "GPU OOM (%dx%d): %s\n", imgW, imgH, label);
        return false;
    };
    if (!allocOrFail(d_images, N * imgBytes, "image"))
    {
        return 0;
    }
    if (!allocOrFail(d_grid, N * 192 * sizeof(float), "grid"))
    {
        return 0;
    }
    if (!allocOrFail(d_tiny, N * 256, "tiny"))
    {
        return 0;
    }
    if (!allocOrFail(d_lbp, N * 256 * sizeof(float), "lbp"))
    {
        return 0;
    }
    if (!allocOrFail(d_avgLAB, N * 3 * sizeof(double), "lab"))
    {
        return 0;
    }
    if (!allocOrFail(d_bright, N * sizeof(double), "bright"))
    {
        return 0;
    }
    if (!allocOrFail(d_contrast, N * sizeof(double), "contrast"))
    {
        return 0;
    }
    if (!allocOrFail(d_edge, N * sizeof(double), "edge"))
    {
        return 0;
    }

    if (cudaMemcpy(d_images.get(), h_images.data(), N * imgBytes, cudaMemcpyHostToDevice) != cudaSuccess)
    {
        return 0;
    }
    if (imgW == 180 && imgH == 320)
    {
        featureKernel<180, 320><<<N, 180>>>(d_images.get(), d_grid.get(), d_tiny.get(), d_lbp.get(), d_avgLAB.get(),
                                            d_bright.get(), d_contrast.get(), d_edge.get(), N);
    }
    else if (imgW == 320 && imgH == 180)
    {
        featureKernel<320, 180><<<N, 320>>>(d_images.get(), d_grid.get(), d_tiny.get(), d_lbp.get(), d_avgLAB.get(),
                                            d_bright.get(), d_contrast.get(), d_edge.get(), N);
    }
    else if (imgW == 360 && imgH == 640)
    {
        featureKernel<360, 640><<<N, 360>>>(d_images.get(), d_grid.get(), d_tiny.get(), d_lbp.get(), d_avgLAB.get(),
                                            d_bright.get(), d_contrast.get(), d_edge.get(), N);
    }
    else if (imgW == 640 && imgH == 360)
    {
        featureKernel<640, 360><<<N, 640>>>(d_images.get(), d_grid.get(), d_tiny.get(), d_lbp.get(), d_avgLAB.get(),
                                            d_bright.get(), d_contrast.get(), d_edge.get(), N);
    }
    else
    {
        fprintf(stderr, "GPU build: unsupported size %dx%d\n", imgW, imgH);
        return 0;
    }
    if (cudaGetLastError() != cudaSuccess || cudaDeviceSynchronize() != cudaSuccess)
    {
        fprintf(stderr, "GPU feature error: kernel launch failed\n");
        return 0;
    }
    auto &h_grid = buffers.h_grid;
    h_grid.resize(N * 192);
    auto &h_tiny = buffers.h_tiny;
    h_tiny.resize(N * 256);
    auto &h_lbp = buffers.h_lbp;
    h_lbp.resize(N * 256);
    auto &h_avgLAB = buffers.h_lab;
    h_avgLAB.resize(N * 3);
    auto &h_bright = buffers.h_bright;
    h_bright.resize(N);
    auto &h_contrast = buffers.h_contrast;
    h_contrast.resize(N);
    auto &h_edge = buffers.h_edge;
    h_edge.resize(N);

    if (cudaMemcpy(h_grid.data(), d_grid.get(), N * 192 * sizeof(float), cudaMemcpyDeviceToHost) != cudaSuccess)
    {
        return 0;
    }
    if (cudaMemcpy(h_tiny.data(), d_tiny.get(), N * 256, cudaMemcpyDeviceToHost) != cudaSuccess)
    {
        return 0;
    }
    if (cudaMemcpy(h_lbp.data(), d_lbp.get(), N * 256 * sizeof(float), cudaMemcpyDeviceToHost) != cudaSuccess)
    {
        return 0;
    }
    if (cudaMemcpy(h_avgLAB.data(), d_avgLAB.get(), N * 3 * sizeof(double), cudaMemcpyDeviceToHost) != cudaSuccess)
    {
        return 0;
    }
    if (cudaMemcpy(h_bright.data(), d_bright.get(), N * sizeof(double), cudaMemcpyDeviceToHost) != cudaSuccess)
    {
        return 0;
    }
    if (cudaMemcpy(h_contrast.data(), d_contrast.get(), N * sizeof(double), cudaMemcpyDeviceToHost) != cudaSuccess)
    {
        return 0;
    }
    if (cudaMemcpy(h_edge.data(), d_edge.get(), N * sizeof(double), cudaMemcpyDeviceToHost) != cudaSuccess)
    {
        return 0;
    }

    // 填充 ImageRecord
    for (int i = 0; i < N; ++i)
    {
        auto &rec = records[i];
        rec.grid4x4.assign(&h_grid[i * 192], &h_grid[i * 192] + 192);
        rec.avgL = h_avgLAB[i * 3 + 0];
        rec.avgA = h_avgLAB[i * 3 + 1];
        rec.avgB = h_avgLAB[i * 3 + 2];
        rec.meanBrightness = h_bright[i];
        rec.contrast = h_contrast[i];
        rec.edgeDensity = h_edge[i];

        FeatureExtractor::computeStatistics(images[i], rec);

        // Save TinyImage
        if (!featDir.empty() && i < static_cast<int>(stems.size()))
        {
            std::string tinyPath = featDir + "/" + stems[i] + ".tiny";
            std::ofstream ofs(u8path(tinyPath), std::ios::binary);
            if (!ofs)
            {
                throw std::runtime_error("cannot write tiny feature: " + tinyPath);
            }
            if (ofs.is_open())
            {
                ofs.write(reinterpret_cast<const char *>(&h_tiny[i * 256]), 256);
                ofs.close();
                if (!ofs)
                {
                    throw std::runtime_error("tiny feature write failed: " + tinyPath);
                }
                rec.tinyPath = tinyPath;
            }

            // Save LBP histogram
            std::string lbpPath = featDir + "/" + stems[i] + ".hist";
            std::ofstream ofs2(u8path(lbpPath), std::ios::binary);
            if (!ofs2)
            {
                throw std::runtime_error("cannot write histogram feature: " + lbpPath);
            }
            if (ofs2.is_open())
            {
                ofs2.write(reinterpret_cast<const char *>(&h_lbp[i * 256]), 256 * sizeof(float));
                ofs2.close();
                if (!ofs2)
                {
                    throw std::runtime_error("histogram feature write failed: " + lbpPath);
                }
                rec.histPath = lbpPath;
            }
        }

        rec.featureVersion = FeatureExtractor::featureVersion();
    }

    return N;
}

// ============================================================
// 调度器宏：根据尺寸选择模板实例
// ============================================================
#define LAUNCH_FEATURE(W, H)                                                                                           \
    if (imgW == W && imgH == H)                                                                                        \
    {                                                                                                                  \
        return launchKernel<W, H>(h_images, N, h_avgLAB, h_grid, h_tiny, h_edge, h_lbp, workspace, timings);           \
    }

namespace
{
template <int W, int H>
int launchKernel(const uint8_t *h_images, int N, double *h_avgLAB, float *h_grid, uint8_t *h_tiny, double *h_edge,
                 float *h_lbp, FeatureWorkspace *workspace, GpuTimings *timings)
{
    constexpr int PIX = W * H;
    size_t imgBytes = PIX * 3;
    FeatureWorkspace local;
    auto &buffers = *(workspace ? workspace : &local)->buffers;
    auto &d_img = buffers.images;
    auto &d_grid = buffers.grid;
    auto &d_tiny = buffers.tiny;
    auto &d_lbp = buffers.lbp;
    auto &d_lab = buffers.lab;
    auto &d_bright = buffers.bright;
    auto &d_contrast = buffers.contrast;
    auto &d_edge = buffers.edge;

    auto cudaCheck = [](cudaError_t e, const char *name, int W, int H)
    {
        if (e != cudaSuccess)
        {
            fprintf(stderr, "GPU OOM (%dx%d): %s for %s\n", W, H, cudaGetErrorString(e), name);
            return false;
        }
        return true;
    };
    {
        PhaseTimer timer(timings ? &timings->allocationMs : nullptr);
        if (!cudaCheck(d_img.allocate(N * imgBytes), "image", W, H))
        {
            return -1;
        }
        if (!cudaCheck(d_grid.allocate(N * 192 * sizeof(float)), "grid", W, H))
        {
            return -1;
        }
        if (!cudaCheck(d_tiny.allocate(N * 256), "tiny", W, H))
        {
            return -1;
        }
        if (!cudaCheck(d_lbp.allocate(N * 256 * sizeof(float)), "lbp", W, H))
        {
            return -1;
        }
        if (!cudaCheck(d_lab.allocate(N * 3 * sizeof(double)), "lab", W, H))
        {
            return -1;
        }
        if (!cudaCheck(d_bright.allocate(N * sizeof(double)), "bright", W, H))
        {
            return -1;
        }
        if (!cudaCheck(d_contrast.allocate(N * sizeof(double)), "contrast", W, H))
        {
            return -1;
        }
        if (!cudaCheck(d_edge.allocate(N * sizeof(double)), "edge", W, H))
        {
            return -1;
        }
    }
    {
        PhaseTimer timer(timings ? &timings->uploadMs : nullptr);
        if (cudaMemcpy(d_img.get(), h_images, N * imgBytes, cudaMemcpyHostToDevice) != cudaSuccess)
        {
            return -1;
        }
    }
    {
        PhaseTimer timer(timings ? &timings->computeMs : nullptr);
        featureKernel<W, H><<<N, W>>>(d_img.get(), d_grid.get(), d_tiny.get(), d_lbp.get(), d_lab.get(), d_bright.get(),
                                      d_contrast.get(), d_edge.get(), N);
        if (cudaGetLastError() != cudaSuccess || cudaDeviceSynchronize() != cudaSuccess)
        {
            fprintf(stderr, "GPU kernel error (%dx%d)\n", W, H);
            return -1;
        }
    }
    {
        PhaseTimer timer(timings ? &timings->downloadMs : nullptr);
        if (cudaMemcpy(h_avgLAB, d_lab.get(), N * 3 * sizeof(double), cudaMemcpyDeviceToHost) != cudaSuccess)
        {
            return -1;
        }
        if (cudaMemcpy(h_grid, d_grid.get(), N * 192 * sizeof(float), cudaMemcpyDeviceToHost) != cudaSuccess)
        {
            return -1;
        }
        if (cudaMemcpy(h_tiny, d_tiny.get(), N * 256, cudaMemcpyDeviceToHost) != cudaSuccess)
        {
            return -1;
        }
        if (cudaMemcpy(h_lbp, d_lbp.get(), N * 256 * sizeof(float), cudaMemcpyDeviceToHost) != cudaSuccess)
        {
            return -1;
        }
        if (cudaMemcpy(h_edge, d_edge.get(), N * sizeof(double), cudaMemcpyDeviceToHost) != cudaSuccess)
        {
            return -1;
        }
    }
    if (timings)
    {
        timings->uploadBytes += static_cast<size_t>(N) * imgBytes;
        timings->downloadBytes += static_cast<size_t>(N) * (4 * sizeof(double) + 448 * sizeof(float) + 256);
        timings->pixelBytes = std::max(timings->pixelBytes, d_img.capacityBytes());
    }
    // 查询特征必须与建库使用相同的 Canny 双阈值，不能把近似梯度计数混入同一列。
    PhaseTimer cannyTimer(timings ? &timings->cannyMs : nullptr);
    if (!buffers.cpuWorkers)
    {
        buffers.cpuWorkers = std::make_unique<WorkerPool>();
    }
    buffers.cpuWorkers->run(N,
                            [&](size_t i)
                            {
                                cv::Mat bgr(H, W, CV_8UC3,
                                            const_cast<uint8_t *>(h_images) + static_cast<size_t>(i) * imgBytes);
                                cv::Mat gray;
                                cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
                                h_edge[i] = computeEdgeDensityFromGray(gray);
                            });
    return 0;
}
} // anonymous namespace

namespace
{
template <int W, int H>
void extractPipelined(FeaturePipeline &p, int first, int count, double *lab, float *grid, uint8_t *tiny,
                      double *edge, float *lbp, GpuTimings *timings)
{
    const size_t rows = static_cast<size_t>(count), imgBytes = W * H * 3;
    auto &transfer = p.transfer;
    StreamDrain drain(transfer.stream);
    // ready 仅在 CPU 和 GPU 均成功完成后推进；失败批次不能生成可评分的视图。
    transfer.mark(0);
    requireCuda(cudaMemcpyAsync(p.images.get(), p.input.get(), rows * imgBytes, cudaMemcpyHostToDevice,
                                transfer.stream));
    if (p.config.testFault == PipelineFault::AfterUpload)
    {
        throw std::runtime_error("injected CUDA failure after upload");
    }
    transfer.mark(1);
    featureKernel<W, H><<<count, W, 0, transfer.stream>>>(
        p.images.get(), p.grid.get() + first * 192, p.tiny.get() + first * 256,
        p.lbp.get() + first * 256, p.lab.get(), p.bright.get(), p.contrast.get(), p.edge.get(), count);
    requireCuda(cudaGetLastError());
    transfer.mark(2);
    auto *hostLab = reinterpret_cast<double *>(p.download.get());
    auto *hostGrid = reinterpret_cast<float *>(hostLab + rows * 3);
    auto *hostLbp = hostGrid + rows * 192;
    auto *hostTiny = reinterpret_cast<uint8_t *>(hostLbp + (p.config.downloadLbp ? rows * 256 : 0));
    requireCuda(cudaMemcpyAsync(hostLab, p.lab.get(), rows * 3 * sizeof(double), cudaMemcpyDeviceToHost,
                                transfer.stream));
    requireCuda(cudaMemcpyAsync(hostGrid, p.grid.get() + first * 192, rows * 192 * sizeof(float),
                                cudaMemcpyDeviceToHost, transfer.stream));
    if (p.config.downloadLbp)
    {
        requireCuda(cudaMemcpyAsync(hostLbp, p.lbp.get() + first * 256, rows * 256 * sizeof(float),
                                    cudaMemcpyDeviceToHost, transfer.stream));
    }
    if (p.config.downloadTiny)
    {
        requireCuda(cudaMemcpyAsync(hostTiny, p.tiny.get() + first * 256, rows * 256,
                                    cudaMemcpyDeviceToHost, transfer.stream));
    }
    transfer.mark(3);
    {
        PhaseTimer timer(timings ? &timings->cannyMs : nullptr);
        // CPU 与 DMA 只读同一份锁页 BGR。run 收束所有任务（包括异常），随后再等待 GPU。
        p.workers->run(rows,
                       [&](size_t i)
                       {
                           if (p.config.testFault == PipelineFault::CpuTask && i == 0)
                           {
                               throw std::runtime_error("injected Canny worker failure");
                           }
                           cv::Mat bgr(H, W, CV_8UC3, p.input.get() + i * imgBytes), gray;
                           cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
                           edge[i] = computeEdgeDensityFromGray(gray);
                       });
    }
    if (p.config.testFault == PipelineFault::BeforeWait)
    {
        throw std::runtime_error("injected CUDA failure before completion");
    }
    transfer.finish(timings);
    drain.complete();
    std::memcpy(lab, hostLab, rows * 3 * sizeof(double));
    std::memcpy(grid, hostGrid, rows * 192 * sizeof(float));
    if (p.config.downloadLbp)
    {
        std::memcpy(lbp, hostLbp, rows * 256 * sizeof(float));
    }
    if (p.config.downloadTiny)
    {
        std::memcpy(tiny, hostTiny, rows * 256);
    }
    p.state->ready += count;
    if (timings)
    {
        timings->uploadBytes += rows * imgBytes;
        timings->downloadBytes += rows * (3 * sizeof(double) + 192 * sizeof(float) +
                                          (p.config.downloadLbp ? 256 * sizeof(float) : 0) +
                                          (p.config.downloadTiny ? 256 : 0));
    }
}
} // namespace

void FeatureWorkspace::extract(int first, int count, double *lab, float *grid, uint8_t *tiny, double *edge,
                               float *lbp, GpuTimings *timings)
{
    auto *p = buffers->pipeline.get();
    if (!p || count <= 0 || count > 256 || first != p->state->ready ||
        count > p->state->count - first || !lab || !grid || !edge ||
        (p->config.downloadTiny && !tiny) || (p->config.downloadLbp && !lbp))
    {
        throw std::invalid_argument("invalid CUDA feature extraction range");
    }
#define EXTRACT_PIPELINE(W, H)                                                                                  \
    if (p->config.width == W && p->config.height == H)                                                           \
    {                                                                                                          \
        extractPipelined<W, H>(*p, first, count, lab, grid, tiny, edge, lbp, timings);                            \
        return;                                                                                                \
    }
    EXTRACT_PIPELINE(180, 320)
    EXTRACT_PIPELINE(320, 180)
    EXTRACT_PIPELINE(360, 640)
    EXTRACT_PIPELINE(640, 360)
#undef EXTRACT_PIPELINE
}

// 带尺寸参数的版本
int extractFeaturesRaw(const uint8_t *h_images, int N, int imgW, int imgH, double *h_avgLAB, float *h_grid,
                       uint8_t *h_tiny, double *h_edge, float *h_lbp, FeatureWorkspace *workspace, GpuTimings *timings)
{
    if (N <= 0)
    {
        return 0;
    }
    LAUNCH_FEATURE(180, 320);
    LAUNCH_FEATURE(320, 180);
    LAUNCH_FEATURE(360, 640);
    LAUNCH_FEATURE(640, 360);
    fprintf(stderr, "GPU: unsupported feature size %dx%d, use CPU fallback\n", imgW, imgH);
    return -1;
}

// 向后兼容：默认 180×320
int extractFeaturesRaw(const uint8_t *h_images, int N, double *h_avgLAB, float *h_grid, uint8_t *h_tiny, double *h_edge,
                       float *h_lbp)
{
    return extractFeaturesRaw(h_images, N, 180, 320, h_avgLAB, h_grid, h_tiny, h_edge, h_lbp);
}

} // namespace cuda
} // namespace mosaicraft
