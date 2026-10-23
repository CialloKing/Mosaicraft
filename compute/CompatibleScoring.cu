#include "CompatibleScoring.h"
#include "DeviceBuffer.cuh"
#include <algorithm>
#include <cfloat>
#include <cstring>
#include <limits>
#include <numeric>

namespace mosaicraft
{
namespace cuda
{
namespace
{
constexpr size_t libraryRowBytes = 3 * sizeof(double) + 192 * sizeof(float) + 256 + sizeof(double) +
                                    256 * sizeof(float) + sizeof(int) + 2;
constexpr size_t tileFeatureBytes = 9 * sizeof(double) + 192 * sizeof(float) + 256 + 256 * sizeof(float);

template <class T> T *take(uint8_t *&cursor, size_t count)
{
    auto *result = reinterpret_cast<T *>(cursor);
    cursor += count * sizeof(T);
    return result;
}

GpuLibrary libraryLayout(uint8_t *cursor, int count)
{
    GpuLibrary result;
    result.count = count;
    result.d_lab = take<double>(cursor, size_t(count) * 3);
    result.d_grid = take<float>(cursor, size_t(count) * 192);
    result.d_tiny = take<uint8_t>(cursor, size_t(count) * 256);
    result.d_edge = take<double>(cursor, count);
    result.d_lbp = take<float>(cursor, size_t(count) * 256);
    result.d_use = take<int>(cursor, count);
    result.d_tinyValid = take<uint8_t>(cursor, count);
    result.d_lbpValid = take<uint8_t>(cursor, count);
    return result;
}

struct ScoreInput
{
    double *l, *a, *b, *edge, *labW, *gridW, *tinyW, *edgeW, *lbpW;
    float *grid, *lbp;
    uint8_t *tiny;
    int *indices;
    double *scores;
};

size_t scoreBytes(size_t count, int candidates)
{
    const size_t inputs = checkedMatchProduct(count, tileFeatureBytes +
                                                     checkedMatchProduct(candidates, sizeof(int)));
    return ((inputs + 7) & ~size_t(7)) + checkedMatchProduct(count, size_t(candidates) * sizeof(double));
}

ScoreInput scoreLayout(uint8_t *start, int count, int candidates)
{
    auto *cursor = start;
    ScoreInput result;
    for (auto **field : {&result.l, &result.a, &result.b, &result.edge, &result.labW,
                         &result.gridW, &result.tinyW, &result.edgeW, &result.lbpW})
    {
        *field = take<double>(cursor, count);
    }
    result.grid = take<float>(cursor, size_t(count) * 192);
    result.lbp = take<float>(cursor, size_t(count) * 256);
    result.tiny = take<uint8_t>(cursor, size_t(count) * 256);
    result.indices = take<int>(cursor, size_t(count) * candidates);
    const size_t offset = (size_t(cursor - start) + 7) & ~size_t(7);
    result.scores = reinterpret_cast<double *>(start + offset);
    return result;
}

__device__ __constant__ double gridWeights[64] =
{
#include "core/GridWeights.inc"
};

__global__ void compatibleScoreKernel(ScoreInput tile, int count, int candidates, GpuLibrary library,
                                      double usePenalty)
{
    const size_t position = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (position >= size_t(count) * candidates)
    {
        return;
    }
    const int row = static_cast<int>(position / candidates);
    const int index = tile.indices[position];
    if (index < 0 || index >= library.count)
    {
        tile.scores[position] = DBL_MAX;
        return;
    }
    const double dl = tile.l[row] - library.d_lab[size_t(index) * 3];
    const double da = tile.a[row] - library.d_lab[size_t(index) * 3 + 1];
    const double db = tile.b[row] - library.d_lab[size_t(index) * 3 + 2];
    const double lab = sqrt(dl * dl + da * da + db * db) / 100.0;
    double grid = 0;
    for (int cell = 0; cell < 64; ++cell)
    {
        const auto *left = tile.grid + size_t(row) * 192 + cell * 3;
        const auto *right = library.d_grid + size_t(index) * 192 + cell * 3;
        // CPU 的两个 float 操作数先相减，再提升为 double。直接转换操作数会改变平分结果。
        const double l = __fsub_rn(left[0], right[0]);
        const double a = __fsub_rn(left[1], right[1]);
        const double b = __fsub_rn(left[2], right[2]);
        grid += sqrt(l * l + a * a + b * b) * gridWeights[cell];
    }
    grid = grid / 64.0 / 100.0;
    double tiny = 1;
    if (library.d_tinyValid[index])
    {
        double sum = 0;
        for (int bin = 0; bin < 256; ++bin)
        {
            const double diff = double(tile.tiny[size_t(row) * 256 + bin]) -
                                double(library.d_tiny[size_t(index) * 256 + bin]);
            sum += diff * diff;
        }
        tiny = sum / (256.0 * 255.0 * 255.0);
    }
    const double edge = fabs(tile.edge[row] - library.d_edge[index]);
    double lbp = 1;
    if (library.d_lbpValid[index])
    {
        double sum = 0;
        for (int bin = 0; bin < 256; ++bin)
        {
            sum += fabsf(__fsub_rn(tile.lbp[size_t(row) * 256 + bin],
                                 library.d_lbp[size_t(index) * 256 + bin]));
        }
        lbp = sum / 2.0;
    }
    // 本编译单元关闭 FMA；保持 CPU 原表达式的左结合顺序，包括末尾的历史使用惩罚。
    tile.scores[position] = tile.labW[row] * lab + tile.gridW[row] * grid + tile.tinyW[row] * tiny +
        tile.edgeW[row] * edge + tile.lbpW[row] * lbp + library.d_use[index] * usePenalty;
}
} // namespace

struct CompatibleScoreWorkspace::Impl
{
    int count = 0, candidates = 0, chunk = 0;
    bool ready = false, failed = false;
    CompatibleScoreFault fault = CompatibleScoreFault::None;
    GpuLibrary library;
    DeviceBuffer<uint8_t> resident, temporary;
    PinnedBuffer staging;
    std::vector<int> needed;
    // 先收束 stream，再释放其引用的缓冲。
    TransferStream transfer;
};

CompatibleScoreWorkspace::CompatibleScoreWorkspace() = default;
CompatibleScoreWorkspace::~CompatibleScoreWorkspace() = default;

bool CompatibleScoreWorkspace::prepare(int maxTiles, int candidates, int libraryCount, size_t hostBudget,
                                       size_t deviceBudget, CompatibleScoreFault fault)
{
    m_impl.reset();
    m_failure = "CUDA resource allocation failed";
    if (maxTiles < 1 || candidates < 1 || libraryCount < 1)
    {
        throw std::invalid_argument("invalid compatible CUDA scoring dimensions");
    }
    const size_t residentBytes = checkedMatchProduct(libraryCount, libraryRowBytes);
    size_t free = 0, total = 0;
    if (!optionalCudaAllocation(cudaMemGetInfo(&free, &total)))
    {
        return false;
    }
    if (residentBytes >= free / 2)
    {
        m_failure = "resident library exceeds available VRAM reserve";
        return false;
    }
    // free 是分配前的余量，只扣一次本次常驻图库；已准备的提取缓冲已反映在 free 中。
    const size_t budget = std::min({deviceBudget, size_t(256ULL * 1024 * 1024), free / 2 - residentBytes});
    int rows = std::min(maxTiles, 256);
    while (rows > 0 && (scoreBytes(rows, candidates) > budget ||
                       scoreBytes(rows, candidates) + 3 * sizeof(int) > hostBudget))
    {
        rows /= 2;
    }
    int chunk = static_cast<int>(std::min({size_t(libraryCount), size_t(4096),
                                          hostBudget / (libraryRowBytes + 3 * sizeof(int))}));
    if (rows == 0 || chunk == 0 || fault == CompatibleScoreFault::LibraryAllocation)
    {
        m_failure = fault == CompatibleScoreFault::LibraryAllocation ? "injected library allocation failure"
            : "host/device budget cannot hold one score row";
        return false;
    }
    try
    {
        auto impl = std::make_unique<Impl>();
        if (!optionalCudaAllocation(impl->resident.allocate(residentBytes)))
        {
            return false;
        }
        while (rows > 0)
        {
            const size_t temporary = scoreBytes(rows, candidates);
            // 一个上传索引数组，另为 Reader 的去重临时索引预留两倍容量。
            while (chunk > 1 && std::max(temporary, size_t(chunk) * libraryRowBytes) +
                                    size_t(chunk) * 3 * sizeof(int) > hostBudget)
            {
                chunk /= 2;
            }
            const size_t host = std::max(temporary, size_t(chunk) * libraryRowBytes);
            if (fault == CompatibleScoreFault::TemporaryAllocation || fault == CompatibleScoreFault::PinnedAllocation)
            {
                m_failure = "injected scoring buffer allocation failure";
                return false;
            }
            if (optionalCudaAllocation(impl->temporary.allocate(temporary)) &&
                optionalCudaAllocation(impl->staging.allocate(host)))
            {
                break;
            }
            impl->temporary.reset();
            impl->staging.reset();
            rows /= 2;
            chunk = std::max(1, chunk / 2);
        }
        if (rows == 0 || !impl->transfer.create())
        {
            return false;
        }
        impl->count = rows;
        impl->candidates = candidates;
        impl->chunk = chunk;
        impl->needed.resize(chunk);
        impl->fault = fault;
        impl->library = libraryLayout(impl->resident.get(), libraryCount);
        m_impl = std::move(impl);
        m_failure = "";
        return true;
    }
    catch (const std::bad_alloc &)
    {
        m_failure = "host allocation failed";
        return false;
    }
}

int CompatibleScoreWorkspace::capacity() const
{
    return m_impl ? m_impl->count : 0;
}
size_t CompatibleScoreWorkspace::hostBytes() const
{
    return m_impl ? m_impl->staging.capacityBytes() + m_impl->needed.capacity() * sizeof(int) : 0;
}
size_t CompatibleScoreWorkspace::deviceBytes() const
{
    return m_impl ? m_impl->temporary.capacityBytes() : 0;
}
size_t CompatibleScoreWorkspace::libraryBytes() const
{
    return m_impl ? m_impl->resident.capacityBytes() : 0;
}
const char *CompatibleScoreWorkspace::preparationFailure() const
{
    return m_failure;
}

void CompatibleScoreWorkspace::uploadLibrary(const std::vector<ImageRecord> &records, FeaturePack::Reader &reader,
                                             GpuTimings *timings)
{
    if (!m_impl || m_impl->failed || records.size() != size_t(m_impl->library.count))
    {
        throw std::invalid_argument("invalid compatible CUDA library");
    }
    auto &p = *m_impl;
    p.ready = false;
    PhaseTimer timer(timings ? &timings->uploadMs : nullptr);
    StreamDrain drain(p.transfer.stream);
    try
    {
        for (int first = 0; first < p.library.count; first += p.chunk)
        {
            const int count = std::min(p.chunk, p.library.count - first);
            p.needed.resize(count);
            std::iota(p.needed.begin(), p.needed.end(), first);
            // 批量来源校验会复用目录/卷元数据采样；逐行 get 会退化为重复文件系统查询。
            reader.prepare(p.needed);
            auto host = libraryLayout(p.staging.get(), count);
            for (int i = 0; i < count; ++i)
            {
                const auto &record = records[first + i];
                if (record.grid4x4.size() != 192)
                {
                    throw std::invalid_argument("compatible CUDA library requires v6 Grid features");
                }
                const auto &features = reader.get(first + i);
                host.d_lab[i * 3] = record.avgL;
                host.d_lab[i * 3 + 1] = record.avgA;
                host.d_lab[i * 3 + 2] = record.avgB;
                std::copy_n(record.grid4x4.data(), 192, host.d_grid + i * 192);
                std::copy(features.tiny.begin(), features.tiny.end(), host.d_tiny + i * 256);
                std::copy(features.lbp.begin(), features.lbp.end(), host.d_lbp + i * 256);
                host.d_edge[i] = record.edgeDensity;
                host.d_use[i] = record.useCount;
                host.d_tinyValid[i] = features.tinyValid;
                host.d_lbpValid[i] = features.lbpValid;
            }
            p.transfer.mark(0);
            auto upload = [&](auto *device, const auto *source, size_t columns)
            {
                requireCuda(cudaMemcpyAsync(device + size_t(first) * columns, source,
                    size_t(count) * columns * sizeof(*source), cudaMemcpyHostToDevice, p.transfer.stream));
            };
            upload(p.library.d_lab, host.d_lab, 3);
            upload(p.library.d_grid, host.d_grid, 192);
            upload(p.library.d_tiny, host.d_tiny, 256);
            upload(p.library.d_edge, host.d_edge, 1);
            upload(p.library.d_lbp, host.d_lbp, 256);
            upload(p.library.d_use, host.d_use, 1);
            upload(p.library.d_tinyValid, host.d_tinyValid, 1);
            upload(p.library.d_lbpValid, host.d_lbpValid, 1);
            if (p.fault == CompatibleScoreFault::LibraryUpload)
            {
                throw std::runtime_error("injected compatible library upload failure");
            }
            p.transfer.mark(1);
            p.transfer.mark(2);
            p.transfer.mark(3);
            p.transfer.finish(timings);
        }
        drain.complete();
        p.ready = true;
    }
    catch (...)
    {
        p.failed = true;
        throw;
    }
    if (timings)
    {
        timings->uploadBytes += libraryBytes();
        timings->residentBytes = libraryBytes();
        timings->pinnedBytes = p.staging.capacityBytes();
    }
}

void CompatibleScoreWorkspace::score(const MatchingBuffers &tiles, int count, double usePenalty, double *scores,
                                     GpuTimings *timings)
{
    if (!m_impl || !m_impl->ready || m_impl->failed || count < 1 || count > capacity() || !scores ||
        tiles.l.size() < size_t(count) || tiles.indices.size() < size_t(count) * m_impl->candidates)
    {
        throw std::invalid_argument("invalid compatible CUDA score batch");
    }
    auto &p = *m_impl;
    const auto host = scoreLayout(p.staging.get(), count, p.candidates);
    const auto device = scoreLayout(p.temporary.get(), count, p.candidates);
    const size_t inputBytes = reinterpret_cast<uint8_t *>(host.scores) - p.staging.get();
    const size_t outputBytes = size_t(count) * p.candidates * sizeof(double);
    {
        PhaseTimer timer(timings ? &timings->uploadMs : nullptr);
        const std::vector<double> *sources[] = {&tiles.l, &tiles.a, &tiles.b, &tiles.edge, &tiles.labW,
                                                &tiles.gridW, &tiles.tinyW, &tiles.edgeW, &tiles.lbpW};
        double *destinations[] = {host.l, host.a, host.b, host.edge, host.labW, host.gridW,
                                  host.tinyW, host.edgeW, host.lbpW};
        for (int i = 0; i < 9; ++i)
        {
            std::copy_n(sources[i]->data(), count, destinations[i]);
        }
        std::copy_n(tiles.grid[0].data(), size_t(count) * 192, host.grid);
        std::copy_n(tiles.tiny[0].data(), size_t(count) * 256, host.tiny);
        std::copy_n(tiles.lbp[0].data(), size_t(count) * 256, host.lbp);
        std::copy_n(tiles.indices.data(), size_t(count) * p.candidates, host.indices);
    }
    StreamDrain drain(p.transfer.stream);
    try
    {
        p.transfer.mark(0);
        requireCuda(cudaMemcpyAsync(p.temporary.get(), p.staging.get(), inputBytes,
                                    cudaMemcpyHostToDevice, p.transfer.stream));
        if (p.fault == CompatibleScoreFault::ScoreUpload)
        {
            throw std::runtime_error("injected compatible score upload failure");
        }
        p.transfer.mark(1);
        const unsigned blocks = static_cast<unsigned>((size_t(count) * p.candidates + 255) / 256);
        compatibleScoreKernel<<<blocks, 256, 0, p.transfer.stream>>>(device, count, p.candidates, p.library, usePenalty);
        requireCuda(cudaGetLastError());
        if (p.fault == CompatibleScoreFault::Kernel)
        {
            throw std::runtime_error("injected compatible score kernel failure");
        }
        p.transfer.mark(2);
        requireCuda(cudaMemcpyAsync(host.scores, device.scores, outputBytes, cudaMemcpyDeviceToHost, p.transfer.stream));
        p.transfer.mark(3);
        if (p.fault == CompatibleScoreFault::Synchronize)
        {
            throw std::runtime_error("injected compatible score synchronization failure");
        }
        p.transfer.finish(timings);
        drain.complete();
    }
    catch (...)
    {
        p.failed = true;
        throw;
    }
    // 只在整批成功后公布分数，异常时调用方看不到部分下载结果。
    {
        PhaseTimer timer(timings ? &timings->downloadMs : nullptr);
        std::memcpy(scores, host.scores, outputBytes);
    }
    if (timings)
    {
        timings->uploadBytes += inputBytes;
        timings->downloadBytes += outputBytes;
        timings->pinnedBytes = p.staging.capacityBytes();
        timings->peakTemporaryBytes = deviceBytes();
    }
}
} // namespace cuda
} // namespace mosaicraft
