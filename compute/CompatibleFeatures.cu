#include "CompatibleFeatures.h"
#include "DeviceBuffer.cuh"
#include "core/FeatureExtractor.h"
#include "core/FeatureUtils.h"
#include <algorithm>
#include <cstring>

namespace mosaicraft
{
namespace cuda
{
namespace
{
constexpr int threads = 256;
constexpr int pixelsPerBlock = 4096;
struct IntegerFeatures
{
    unsigned long long lab[3];
    unsigned long long grid[192];
    unsigned int lbp[256];
};

__global__ void compatibleKernel(const uint8_t *input, int width, int height, int blocks,
                                 IntegerFeatures *partials)
{
    const int thread = threadIdx.x, pixels = width * height;
    const auto *lab = input + static_cast<size_t>(blockIdx.y) * pixels * 4;
    const auto *gray = lab + static_cast<size_t>(pixels) * 3;
    __shared__ unsigned int sums[195];
    __shared__ unsigned int histogram[256];
    if (thread < 195)
    {
        sums[thread] = 0;
    }
    histogram[thread] = 0;
    __syncthreads();
    unsigned int totalL = 0, totalA = 0, totalB = 0;
    const int first = blockIdx.x * pixelsPerBlock;
    const int end = min(first + pixelsPerBlock, pixels);
    const int cellW = (width + 7) / 8, cellH = (height + 7) / 8;
    for (int pixel = first + thread; pixel < end; pixel += threads)
    {
        const int x = pixel % width, y = pixel / width;
        const auto *color = lab + pixel * 3;
        totalL += color[0];
        totalA += color[1];
        totalB += color[2];
        if (width >= 8 && height >= 8)
        {
            const int cell = (y / cellH * 8 + x / cellW) * 3;
            atomicAdd(sums + 3 + cell, static_cast<unsigned int>(color[0]));
            atomicAdd(sums + 4 + cell, static_cast<unsigned int>(color[1]));
            atomicAdd(sums + 5 + cell, static_cast<unsigned int>(color[2]));
        }
        if (x > 0 && y > 0 && x + 1 < width && y + 1 < height)
        {
            const uint8_t center = gray[pixel];
            unsigned int code = (gray[pixel - width - 1] >= center) |
                                ((gray[pixel - width] >= center) << 1) |
                                ((gray[pixel - width + 1] >= center) << 2) |
                                ((gray[pixel + 1] >= center) << 3) |
                                ((gray[pixel + width + 1] >= center) << 4) |
                                ((gray[pixel + width] >= center) << 5) |
                                ((gray[pixel + width - 1] >= center) << 6) |
                                ((gray[pixel - 1] >= center) << 7);
            atomicAdd(histogram + code, 1u);
        }
    }
    // 每块最多 4096 像素，块内 uint32 不溢出；跨块 LAB 必须使用 uint64。
    atomicAdd(sums, totalL);
    atomicAdd(sums + 1, totalA);
    atomicAdd(sums + 2, totalB);
    __syncthreads();
    auto &out = partials[static_cast<size_t>(blockIdx.y) * blocks + blockIdx.x];
    if (thread < 3)
    {
        out.lab[thread] = sums[thread];
    }
    if (thread < 192)
    {
        out.grid[thread] = sums[thread + 3];
    }
    out.lbp[thread] = histogram[thread];
}

__global__ void reduceCompatible(const IntegerFeatures *partials, int blocks, IntegerFeatures *output)
{
    const int index = threadIdx.x;
    const auto *source = partials + static_cast<size_t>(blockIdx.x) * blocks;
    unsigned long long lab = 0, grid = 0;
    unsigned int histogram = 0;
    for (int block = 0; block < blocks; ++block)
    {
        if (index < 3)
        {
            lab += source[block].lab[index];
        }
        if (index < 192)
        {
            grid += source[block].grid[index];
        }
        histogram += source[block].lbp[index];
    }
    auto &out = output[blockIdx.x];
    if (index < 3)
    {
        out.lab[index] = lab;
    }
    if (index < 192)
    {
        out.grid[index] = grid;
    }
    out.lbp[index] = histogram;
}
} // namespace

struct CompatibleFeatureWorkspace::Impl
{
    int width = 0, height = 0, count = 0, blocks = 0;
    size_t host = 0, device = 0;
    bool statistics = false;
    PipelineFault fault = PipelineFault::None;
    PinnedBuffer input, output;
    DeviceBuffer<uint8_t> pixels;
    DeviceBuffer<IntegerFeatures> partials, result;
    TransferStream transfer;
};

CompatibleFeatureWorkspace::CompatibleFeatureWorkspace() = default;
CompatibleFeatureWorkspace::~CompatibleFeatureWorkspace() = default;

bool CompatibleFeatureWorkspace::prepare(int width, int height, int maxImages, size_t hostBudget,
                                         bool statistics, size_t deviceBudget, PipelineFault fault)
{
    m_impl.reset();
    if (width < 1 || height < 1 || width > 8192 || height > 8192 || maxImages < 1)
    {
        throw std::invalid_argument("invalid compatible CUDA feature dimensions");
    }
    const size_t pixels = static_cast<size_t>(width) * height;
    const size_t blocks = (pixels + pixelsPerBlock - 1) / pixelsPerBlock;
    // BGR + 锁页 LAB/gray，另预留 Canny 输出或统计中的 HSV/分通道临时图。
    // OpenCV 内部的算子工作区不属于可直接计量的像素缓冲。
    const size_t hostRow = pixels * (statistics ? 17 : 8) + sizeof(IntegerFeatures) +
                           sizeof(CompatibleFeature) + sizeof(cv::Mat) + sizeof(ImageRecord *);
    const size_t deviceRow = pixels * 4 + (blocks + 1) * sizeof(IntegerFeatures);
    size_t free = 0, total = 0;
    requireCuda(cudaMemGetInfo(&free, &total));
    const size_t budget = std::min({deviceBudget, size_t(256ULL * 1024 * 1024), free / 2});
    const size_t count = std::min({size_t(maxImages), size_t(256), hostBudget / hostRow, budget / deviceRow});
    if (count == 0)
    {
        return false;
    }
    try
    {
        auto impl = std::make_unique<Impl>();
        if (!optionalCudaAllocation(impl->input.allocate(count * pixels * 4)) ||
            fault == PipelineFault::PinnedAllocation ||
            !optionalCudaAllocation(impl->output.allocate(count * sizeof(IntegerFeatures))) ||
            !optionalCudaAllocation(impl->pixels.allocate(count * pixels * 4)) ||
            fault == PipelineFault::DeviceAllocation ||
            !optionalCudaAllocation(impl->partials.allocate(count * blocks * sizeof(IntegerFeatures))) ||
            !optionalCudaAllocation(impl->result.allocate(count * sizeof(IntegerFeatures))) ||
            !impl->transfer.create())
        {
            return false;
        }
        impl->width = width;
        impl->height = height;
        impl->blocks = static_cast<int>(blocks);
        impl->count = static_cast<int>(count);
        impl->host = count * hostRow;
        impl->device = count * deviceRow;
        impl->statistics = statistics;
        impl->fault = fault;
        m_impl = std::move(impl);
        return true;
    }
    catch (const std::bad_alloc &)
    {
        return false;
    }
}

int CompatibleFeatureWorkspace::capacity() const
{
    return m_impl ? m_impl->count : 0;
}
size_t CompatibleFeatureWorkspace::hostBytes() const
{
    return m_impl ? m_impl->host : 0;
}
size_t CompatibleFeatureWorkspace::deviceBytes() const
{
    return m_impl ? m_impl->device : 0;
}

void CompatibleFeatureWorkspace::extract(const std::vector<cv::Mat> &images,
                                         std::vector<CompatibleFeature> &features, WorkerPool &workers,
                                         const std::vector<ImageRecord *> &statistics, CompatibleTimings *timings)
{
    if (!m_impl || images.empty() || images.size() > static_cast<size_t>(capacity()) ||
        (!statistics.empty() && (!m_impl->statistics || statistics.size() != images.size())))
    {
        throw std::invalid_argument("invalid compatible CUDA batch");
    }
    auto &p = *m_impl;
    for (size_t i = 0; i < images.size(); ++i)
    {
        if (images[i].type() != CV_8UC3 || images[i].cols != p.width || images[i].rows != p.height ||
            (!statistics.empty() && !statistics[i]))
        {
            throw std::invalid_argument("compatible CUDA expects equally sized 8-bit BGR images");
        }
    }
    features.resize(images.size());
    const size_t pixels = static_cast<size_t>(p.width) * p.height;
    {
        PhaseTimer timer(timings ? &timings->prepareMs : nullptr);
        workers.run(images.size(), [&](size_t i)
        {
            auto *input = p.input.get() + i * pixels * 4;
            cv::Mat lab(p.height, p.width, CV_8UC3, input);
            cv::Mat gray(p.height, p.width, CV_8UC1, input + pixels * 3);
            cv::cvtColor(images[i], lab, cv::COLOR_BGR2Lab);
            cv::cvtColor(images[i], gray, cv::COLOR_BGR2GRAY);
        });
    }
    StreamDrain drain(p.transfer.stream);
    try
    {
        p.transfer.mark(0);
        requireCuda(cudaMemcpyAsync(p.pixels.get(), p.input.get(), images.size() * pixels * 4,
                                    cudaMemcpyHostToDevice, p.transfer.stream));
        if (p.fault == PipelineFault::AfterUpload)
        {
            throw std::runtime_error("injected compatible CUDA failure after upload");
        }
        p.transfer.mark(1);
        compatibleKernel<<<dim3(p.blocks, static_cast<unsigned>(images.size())), threads, 0, p.transfer.stream>>>(
            p.pixels.get(), p.width, p.height, p.blocks, p.partials.get());
        requireCuda(cudaGetLastError());
        reduceCompatible<<<static_cast<unsigned>(images.size()), threads, 0, p.transfer.stream>>>(
            p.partials.get(), p.blocks, p.result.get());
        requireCuda(cudaGetLastError());
        p.transfer.mark(2);
        requireCuda(cudaMemcpyAsync(p.output.get(), p.result.get(), images.size() * sizeof(IntegerFeatures),
                                    cudaMemcpyDeviceToHost, p.transfer.stream));
        p.transfer.mark(3);
        {
            PhaseTimer timer(timings ? &timings->cpuMs : nullptr);
            workers.run(images.size(), [&](size_t i)
            {
                if (p.fault == PipelineFault::CpuTask && i == 0)
                {
                    throw std::runtime_error("injected compatible CPU task failure");
                }
                auto *input = p.input.get() + i * pixels * 4;
                cv::Mat lab(p.height, p.width, CV_8UC3, input);
                cv::Mat gray(p.height, p.width, CV_8UC1, input + pixels * 3);
                cv::Mat tiny(16, 16, CV_8UC1, features[i].tiny.data());
                cv::resize(gray, tiny, tiny.size(), 0, 0, cv::INTER_AREA);
                if (statistics.empty())
                {
                    features[i].edge = computeEdgeDensityFromGray(gray);
                }
                else
                {
                    FeatureExtractor::computeStatistics(images[i], *statistics[i], lab, gray);
                    features[i].edge = statistics[i]->edgeDensity;
                }
            });
        }
        if (p.fault == PipelineFault::BeforeWait)
        {
            throw std::runtime_error("injected compatible CUDA failure before completion");
        }
        p.transfer.finish(timings ? &timings->gpu : nullptr);
        drain.complete();
    }
    catch (...)
    {
        // StreamDrain 先等待本批 DMA，调用方不得在失败后消费部分输出。
        features.clear();
        throw;
    }
    const auto *integers = reinterpret_cast<const IntegerFeatures *>(p.output.get());
    for (size_t i = 0; i < images.size(); ++i)
    {
        auto &feature = features[i];
        for (int channel = 0; channel < 3; ++channel)
        {
            // cv::mean 使用整数和乘以 double 倒数，不能换成直接除法或 GPU float 均值。
            feature.lab[channel] = static_cast<double>(integers[i].lab[channel]) * (1.0 / pixels);
        }
        const int cellW = (p.width + 7) / 8, cellH = (p.height + 7) / 8;
        for (int cell = 0; cell < 64; ++cell)
        {
            const int width = std::min(cellW, p.width - cell % 8 * cellW);
            const int height = std::min(cellH, p.height - cell / 8 * cellH);
            const double scale = p.width >= 8 && p.height >= 8 && width > 0 && height > 0
                                     ? 1.0 / (width * height) : 0.0;
            for (int channel = 0; channel < 3; ++channel)
            {
                const int index = cell * 3 + channel;
                feature.grid[index] = static_cast<float>(static_cast<double>(integers[i].grid[index]) * scale);
            }
        }
        for (int bin = 0; bin < 256; ++bin)
        {
            feature.lbp[bin] = compatibleLbpCount(integers[i].lbp[bin]);
        }
        normalizeLbpHistogram(feature.lbp);
    }
    if (timings)
    {
        timings->images += images.size();
        ++timings->batches;
        timings->gpu.uploadBytes += images.size() * pixels * 4;
        timings->gpu.downloadBytes += images.size() * sizeof(IntegerFeatures);
        timings->gpu.pinnedBytes = p.input.capacityBytes() + p.output.capacityBytes();
        timings->gpu.peakTemporaryBytes = p.device;
    }
}
} // namespace cuda
} // namespace mosaicraft
