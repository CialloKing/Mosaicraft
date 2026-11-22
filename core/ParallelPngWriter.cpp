#include "ParallelPngWriter.h"
#include "WorkerPool.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <vector>
#include <zlib.h>

namespace mosaicraft
{
namespace
{
constexpr size_t MiB = 1024 * 1024;
constexpr size_t dictionaryBytes = 32 * 1024;
constexpr size_t minBlockBytes = 64 * 1024;
constexpr size_t targetBlockBytes = 4 * MiB;
// 为压缩上界及块尾保留余量，保证 IDAT 长度不超过 PNG 的 31 位限制。
constexpr size_t maxBlockInputBytes = 1024 * MiB;
constexpr size_t stateBudget = 512 * 1024; // zlib 的滑窗、哈希表和 pending buffer。
constexpr std::array<uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
constexpr std::array<uint8_t, 4> idat{'I', 'D', 'A', 'T'};

void saveUint32(uint8_t *p, uint32_t value)
{
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
}

size_t compressedCapacity(size_t inputBytes)
{
    // 默认窗口与内存级别的 zlib 上界，再为非最终块的 SYNC_FLUSH 留余量。
    return compressBound(static_cast<uLong>(inputBytes)) + 64;
}

uint64_t bytesPerWorker(size_t inputBytes)
{
    return inputBytes + compressedCapacity(inputBytes) + stateBudget;
}

using File = std::unique_ptr<FILE, decltype(&std::fclose)>;

File openPng(const std::string &path)
{
#ifdef _WIN32
    return File(_wfopen(std::filesystem::u8path(path).c_str(), L"wb"), &std::fclose);
#else
    return File(std::fopen(path.c_str(), "wb"), &std::fclose);
#endif
}

int paeth(int left, int above, int upperLeft)
{
    const int a = above - upperLeft, b = left - upperLeft;
    const int da = std::abs(a), db = std::abs(b), dc = std::abs(a + b);
    return da <= db && da <= dc ? left : (db <= dc ? above : upperLeft);
}

template<int Filter>
uint8_t residual(const uint8_t *row, const uint8_t *above, size_t i, int left, int upperLeft)
{
    if constexpr (Filter == 0)
    {
        return row[i];
    }
    if constexpr (Filter == 1)
    {
        return static_cast<uint8_t>(row[i] - left);
    }
    if constexpr (Filter == 2)
    {
        return static_cast<uint8_t>(row[i] - above[i]);
    }
    if constexpr (Filter == 3)
    {
        return static_cast<uint8_t>(row[i] - (left + above[i]) / 2);
    }
    return static_cast<uint8_t>(row[i] - paeth(left, above[i], upperLeft));
}

template<int Filter>
uint64_t scoreFilter(const uint8_t *row, const uint8_t *above, size_t size, uint64_t best)
{
    uint64_t score = 0;
    for (size_t first = 0; first < size && score < best; first += 256)
    {
        const size_t end = std::min(first + 256, size);
        for (size_t i = first; i < end; ++i)
        {
            const int v = residual<Filter>(row, above, i, i < 3 ? 0 : row[i - 3], i < 3 ? 0 : above[i - 3]);
            score += std::min(v, 256 - v);
        }
    }
    return score;
}

template<int Filter>
void writeFilter(const uint8_t *row, const uint8_t *above, size_t size, uint8_t *out)
{
    out[0] = Filter;
    if constexpr (Filter == 0)
    {
        std::memcpy(out + 1, row, size);
        return;
    }
    for (size_t i = 0; i < 3; ++i)
    {
        out[i + 1] = residual<Filter>(row, above, i, 0, 0);
    }
    for (size_t i = 3; i < size; ++i)
    {
        out[i + 1] = residual<Filter>(row, above, i, row[i - 3], Filter >= 3 ? above[i - 3] : 0);
    }
}

void filterRow(const uint8_t *row, const uint8_t *above, size_t size, uint8_t *out)
{
    using Score = uint64_t (*)(const uint8_t *, const uint8_t *, size_t, uint64_t);
    using Write = void (*)(const uint8_t *, const uint8_t *, size_t, uint8_t *);
    constexpr std::array<Score, 5> score{scoreFilter<0>, scoreFilter<1>, scoreFilter<2>, scoreFilter<3>, scoreFilter<4>};
    constexpr std::array<Write, 5> write{writeFilter<0>, writeFilter<1>, writeFilter<2>, writeFilter<3>, writeFilter<4>};
    // 同 libpng 的默认启发式：选择残差绝对值和最小的滤波，平分时保留靠前的类型。
    uint64_t best = UINT64_MAX;
    size_t selected = 0;
    for (size_t i = 0; i < score.size() && best; ++i)
    {
        const auto value = score[i](row, above, size, best);
        if (value < best)
        {
            best = value;
            selected = i;
        }
    }
    write[selected](row, above, size, out);
}

class Deflater
{
  public:
    Deflater() = default;
    Deflater(const Deflater &) = delete;
    Deflater &operator=(const Deflater &) = delete;

    void init(int level)
    {
        const int rc = deflateInit2(&stream, level, Z_DEFLATED, -15, 8, Z_FILTERED);
        if (rc == Z_MEM_ERROR)
        {
            throw std::bad_alloc();
        }
        if (rc != Z_OK)
        {
            throw std::runtime_error("PNG deflate initialization failed");
        }
    }

    ~Deflater()
    {
        if (stream.state)
        {
            deflateEnd(&stream);
        }
    }

    z_stream stream{};
};
} // namespace

ParallelPngPlan planParallelPng(int width, int height, unsigned workers, uint64_t memoryBudget)
{
    if (width <= 0 || height < 2 || workers < 2)
    {
        return {};
    }
    const uint64_t rowBytes = uint64_t(width) * 3 + 1;
    const uint64_t fixedBytes = dictionaryBytes + rowBytes - 1; // 包含自适应滤波的上一行。
    if (rowBytes > maxBlockInputBytes || memoryBudget <= fixedBytes)
    {
        return {};
    }
    // 先按较小的工作粒度分配最多的槽位，不用整图大小挡住中小图。
    const auto minimumRows = std::max<size_t>(1, minBlockBytes / rowBytes);
    const uint64_t budget = memoryBudget - fixedBytes;
    const auto slots = static_cast<size_t>(std::min<uint64_t>(
        {workers, uint64_t(height) / minimumRows, budget / bytesPerWorker(minimumRows * rowBytes)}));
    if (slots < 2)
    {
        return {};
    }

    // 固定并行度后再增大块；极宽行按整行切分，只要预算能容纳即可。
    const uint64_t perSlot = budget / slots;
    const auto targetRows = std::max<size_t>(1, targetBlockBytes / rowBytes);
    size_t lo = minimumRows, hi = std::min(targetRows, static_cast<size_t>(height) / slots);
    while (lo < hi)
    {
        const size_t mid = lo + (hi - lo + 1) / 2;
        if (bytesPerWorker(mid * rowBytes) <= perSlot)
        {
            lo = mid;
        }
        else
        {
            hi = mid - 1;
        }
    }
    return {slots, lo, slots * bytesPerWorker(lo * rowBytes) + fixedBytes};
}

struct ParallelPngWriter::Impl
{
    struct Block
    {
        std::vector<uint8_t> compressed;
        size_t size = 0;
        uLong adler = 1;
        uLong crc = 0;
        Deflater encoder;
    };

    Impl(const std::string &path, int width, int height, int level, bool fixedSub, const ParallelPngPlan &plan,
         WorkerPool &pool)
        : height(height), rowBytes(static_cast<size_t>(width) * 3 + 1), blockRows(plan.blockRows),
          pool(pool), filtered(rowBytes * blockRows * plan.workers), precedingRow(fixedSub ? 0 : rowBytes - 1),
          blocks(plan.workers), file(nullptr, &std::fclose)
    {
        // 先分配有界缓冲；内存不足时调用方仍可使用旧写入器，尚未碰输出文件。
        for (auto &block : blocks)
        {
            block.compressed.resize(compressedCapacity(blockRows * rowBytes));
            block.encoder.init(level);
        }
        file = openPng(path);
        if (!file)
        {
            throw std::runtime_error("PNG open failed");
        }
        std::array<uint8_t, 13> ihdr{};
        saveUint32(ihdr.data(), static_cast<uint32_t>(width));
        saveUint32(ihdr.data() + 4, static_cast<uint32_t>(height));
        ihdr[8] = 8;
        ihdr[9] = 2; // RGB，不交错。
        write(signature.data(), signature.size());
        chunk({'I', 'H', 'D', 'R'}, ihdr.data(), ihdr.size());
        // CMF/FLG：32 KiB 滑窗、压缩等级提示、没有外部字典。所有 IDAT 属于同一条流。
        const unsigned flags = (level < 2 ? 0 : (level < 6 ? 1 : (level == 6 ? 2 : 3))) << 6;
        const std::array<uint8_t, 2> zlibHeader{0x78, static_cast<uint8_t>(flags + (31 - (0x7800 + flags) % 31) % 31)};
        chunk(idat, zlibHeader.data(), zlibHeader.size());
    }

    void write(const uint8_t *data, size_t size)
    {
        if (size && std::fwrite(data, 1, size, file.get()) != size)
        {
            throw std::runtime_error("PNG write failed");
        }
    }

    static uLong checksum(const std::array<uint8_t, 4> &type, const uint8_t *data, size_t size)
    {
        const auto crc = crc32(0, type.data(), static_cast<uInt>(type.size()));
        return size ? crc32(crc, data, static_cast<uInt>(size)) : crc;
    }

    void chunk(const std::array<uint8_t, 4> &type, const uint8_t *data, size_t size, uLong crc)
    {
        std::array<uint8_t, 8> header{};
        std::copy(type.begin(), type.end(), header.begin() + 4);
        saveUint32(header.data(), static_cast<uint32_t>(size));
        std::array<uint8_t, 4> footer{};
        saveUint32(footer.data(), static_cast<uint32_t>(crc));
        write(header.data(), header.size());
        write(data, size);
        write(footer.data(), footer.size());
    }

    void chunk(const std::array<uint8_t, 4> &type, const uint8_t *data, size_t size)
    {
        chunk(type, data, size, checksum(type, data, size));
    }

    void drain(bool final)
    {
        const size_t bytes = bufferedRows * rowBytes;
        const size_t blockBytes = blockRows * rowBytes;
        const size_t count = (bufferedRows + blockRows - 1) / blockRows;
        pool.run(count, [&](size_t i)
        {
            auto &block = blocks[i];
            const size_t offset = i * blockBytes, size = std::min(blockBytes, bytes - offset);
            const auto input = filtered.data() + offset;
            const size_t history = i ? std::min(offset, dictionary.size()) : dictionarySize;
            const auto previous = i ? input - history : dictionary.data();
            auto &z = block.encoder.stream;
            // 重置独立块的压缩状态，但复用 zlib 的内部分配。
            if (deflateReset(&z) != Z_OK)
            {
                throw std::runtime_error("PNG deflate reset failed");
            }
            // 字典来自前块的原始滤波数据，压缩不互相等待。SYNC_FLUSH 对齐非尾块，供顺序拼接。
            if (history && deflateSetDictionary(&z, previous, static_cast<uInt>(history)) != Z_OK)
            {
                throw std::runtime_error("PNG deflate dictionary failed");
            }
            z.next_in = const_cast<Bytef *>(input);
            z.avail_in = static_cast<uInt>(size);
            z.next_out = block.compressed.data();
            z.avail_out = static_cast<uInt>(block.compressed.size());
            const bool last = final && i + 1 == count;
            if (deflate(&z, last ? Z_FINISH : Z_SYNC_FLUSH) != (last ? Z_STREAM_END : Z_OK) ||
                z.avail_in != 0 || z.avail_out == 0)
            {
                throw std::runtime_error("PNG deflate block failed");
            }
            block.size = z.total_out;
            block.adler = adler32(1, input, static_cast<uInt>(size));
            block.crc = checksum(idat, block.compressed.data(), block.size);
        });
        for (size_t i = 0; i < count; ++i)
        {
            const auto &block = blocks[i];
            chunk(idat, block.compressed.data(), block.size, block.crc);
            adler = adler32_combine(adler, block.adler, static_cast<z_off_t>(std::min(blockBytes, bytes - i * blockBytes)));
        }
        // 只有整批写完才复用缓冲；最后一批可能不足一块，无需保留字典。
        if (!final)
        {
            dictionarySize = std::min(bytes, dictionary.size());
            std::memcpy(dictionary.data(), filtered.data() + bytes - dictionarySize, dictionarySize);
        }
        bufferedRows = 0;
    }

    void writeRows(const uint8_t *rgb, size_t stride, int count)
    {
        if (!file || !rgb || stride < rowBytes - 1 || count < 0 || count > height - rowsWritten)
        {
            throw std::invalid_argument("invalid parallel PNG rows");
        }
        const size_t capacity = filtered.size() / rowBytes;
        for (int first = 0; first < count;)
        {
            const size_t take = std::min<size_t>(count - first, capacity - bufferedRows);
            auto dest = filtered.data() + bufferedRows * rowBytes;
            auto src = rgb + static_cast<size_t>(first) * stride;
            pool.runChunks(take, 4, [&](size_t i)
            {
                const auto row = src + i * stride;
                auto out = dest + i * rowBytes;
                if (precedingRow.empty())
                {
                    writeFilter<1>(row, nullptr, rowBytes - 1, out);
                }
                else
                {
                    // 上方像素来自未修改的 RGB 行；无需等前一行完成滤波。
                    filterRow(row, i ? row - stride : precedingRow.data(), rowBytes - 1, out);
                }
            });
            if (!precedingRow.empty())
            {
                std::memcpy(precedingRow.data(), src + (take - 1) * stride, rowBytes - 1);
            }
            first += static_cast<int>(take);
            rowsWritten += static_cast<int>(take);
            bufferedRows += take;
            if (bufferedRows == capacity || rowsWritten == height)
            {
                drain(rowsWritten == height);
            }
        }
    }

    bool close()
    {
        if (file)
        {
            const bool complete = rowsWritten == height && bufferedRows == 0;
            if (complete)
            {
                std::array<uint8_t, 4> trailer{};
                saveUint32(trailer.data(), static_cast<uint32_t>(adler));
                chunk(idat, trailer.data(), trailer.size());
                chunk({'I', 'E', 'N', 'D'}, nullptr, 0);
            }
            closed = std::fclose(file.release()) == 0 && complete;
        }
        return closed;
    }

    const int height;
    const size_t rowBytes, blockRows;
    WorkerPool &pool;
    std::vector<uint8_t> filtered, precedingRow;
    std::vector<Block> blocks;
    File file;
    std::array<uint8_t, dictionaryBytes> dictionary;
    size_t bufferedRows = 0, dictionarySize = 0;
    int rowsWritten = 0;
    uLong adler = 1;
    bool closed = false;
};

ParallelPngWriter::ParallelPngWriter(const std::string &path, int width, int height, int level, bool fixedSub,
                                     const ParallelPngPlan &plan, WorkerPool &pool)
    : m_impl(std::make_unique<Impl>(path, width, height, level, fixedSub, plan, pool))
{
}

ParallelPngWriter::~ParallelPngWriter() = default;

void ParallelPngWriter::writeRows(const uint8_t *rgb, size_t stride, int count)
{
    m_impl->writeRows(rgb, stride, count);
}

bool ParallelPngWriter::close()
{
    return m_impl->close();
}

} // namespace mosaicraft
