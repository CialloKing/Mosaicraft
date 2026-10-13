#include "FeaturePack.h"
#include "UnicodeIO.h"
#include "WorkerPool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <list>
#include <memory>
#include <numeric>
#include <stdexcept>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mosaicraft
{
namespace
{
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using File = std::unique_ptr<FILE, decltype(&fclose)>;
constexpr uint64_t kHashStart = UINT64_C(14695981039346656037);

uint64_t hashBytes(const void *data, size_t bytes, uint64_t hash = kHashStart)
{
    const auto *values = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < bytes; ++i)
    {
        hash = (hash ^ values[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

File openFile(const fs::path &path, const char *mode)
{
#ifdef _WIN32
    const std::wstring wideMode(mode, mode + std::strlen(mode));
    return File(_wfopen(path.c_str(), wideMode.c_str()), fclose);
#else
    return File(fopen(path.c_str(), mode), fclose);
#endif
}

struct Stamp
{
    uint64_t path = 0, size = UINT64_MAX, time = 0;
    bool operator==(const Stamp &other) const
    {
        return path == other.path && size == other.size && time == other.time;
    }
};

Stamp stamp(const std::string &path)
{
    Stamp result;
    result.path = hashBytes(path.data(), path.size());
    if (path.empty())
    {
        return result;
    }
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExW(u8path(path).c_str(), GetFileExInfoStandard, &data) &&
        !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        result.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        result.time =
            (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    }
#else
    std::error_code error;
    const auto file = u8path(path);
    const auto size = fs::file_size(file, error);
    if (!error)
    {
        const auto time = fs::last_write_time(file, error);
        if (!error)
        {
            result.size = size;
            result.time = static_cast<uint64_t>(time.time_since_epoch().count());
        }
    }
#endif
    return result;
}

// 固定宽度的小端元数据与 v2 数据布局配套，避免数万条路径的文本解析。
struct Metadata
{
    int32_t id = 0, version = 0;
    uint64_t imageHash = 0;
    Stamp tiny, lbp;
    uint64_t tinyHash = 0, lbpHash = 0;
    uint32_t tinyValid = 0, lbpValid = 0;
};
static_assert(sizeof(Metadata) == 88, "feature metadata layout changed");

// 大图库的逐文件属性查询会盖过 GPU 收益；目录枚举提供相同的大小和修改时间。
// 枚举失败、大小写别名或未找到的文件仍走原属性查询，不将不可读目录当成空目录。
class StampSampler
{
#ifdef _WIN32
    struct Directory
    {
        bool available = false;
        std::unordered_map<std::wstring, Stamp> entries;
    };
    std::unordered_map<fs::path, Directory> m_directories;
#endif
  public:
    StampSampler(const std::vector<ImageRecord> &records, const std::vector<int> &needed)
    {
#ifdef _WIN32
        if (needed.size() < 512)
        {
            return;
        }
        for (int index : needed)
        {
            for (const auto *source : {&records[index].tinyPath, &records[index].histPath})
            {
                if (!source->empty())
                {
                    const auto path = u8path(*source);
                    m_directories[path.parent_path()].entries.emplace(path.filename().wstring(), Stamp{});
                }
            }
        }
        for (auto &[path, directory] : m_directories)
        {
            WIN32_FIND_DATAW data{};
            const auto handle = FindFirstFileExW((path / L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                                 nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (handle == INVALID_HANDLE_VALUE)
            {
                continue;
            }
            do
            {
                auto entry = directory.entries.find(data.cFileName);
                if (entry != directory.entries.end() && !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    entry->second = {1, (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
                                     (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                                         data.ftLastWriteTime.dwLowDateTime};
                }
            } while (FindNextFileW(handle, &data));
            directory.available = GetLastError() == ERROR_NO_MORE_FILES;
            FindClose(handle);
        }
#endif
    }

    Stamp get(const std::string &source) const
    {
#ifdef _WIN32
        if (!source.empty() && !m_directories.empty())
        {
            const auto path = u8path(source);
            const auto directory = m_directories.find(path.parent_path());
            if (directory != m_directories.end() && directory->second.available)
            {
                const auto entry = directory->second.entries.find(path.filename().wstring());
                if (entry != directory->second.entries.end() && entry->second.path)
                {
                    auto result = entry->second;
                    result.path = hashBytes(source.data(), source.size());
                    return result;
                }
            }
        }
#endif
        return stamp(source);
    }
};

struct Manifest
{
    std::string generation;
    uint32_t count = 0;
    uint64_t metadataHash = kHashStart;
};

fs::path generationPath(const std::string &directory, const Manifest &manifest, const char *suffix)
{
    return u8path(directory) / ("features-" + manifest.generation + suffix);
}

bool readManifest(const std::string &directory, Manifest &manifest)
{
    std::ifstream input(u8path(directory) / "features.pack");
    std::string magic;
    int version = 0;
    if (!(input >> magic >> version >> manifest.generation >> manifest.count >> manifest.metadataHash) ||
        magic != "MOSAICRAFT_PACK" || version != 3 || manifest.generation.empty() ||
        manifest.generation.find_first_not_of("0123456789-") != std::string::npos)
    {
        return false;
    }
    input >> std::ws;
    return input.eof();
}

bool readMetadata(const std::string &directory, const Manifest &manifest, size_t count, std::vector<Metadata> &metadata)
{
    auto file = openFile(generationPath(directory, manifest, ".meta"), "rb");
    if (!file || manifest.count != count)
    {
        return false;
    }
    metadata.resize(count);
    return fread(metadata.data(), sizeof(Metadata), count, file.get()) == count && fgetc(file.get()) == EOF &&
           !ferror(file.get()) && hashBytes(metadata.data(), count * sizeof(Metadata)) == manifest.metadataHash;
}

bool readSource(const std::string &path, const Stamp &source, void *data, size_t bytes)
{
    if (path.empty() || source.size != bytes)
    {
        return false;
    }
    auto file = openFile(u8path(path), "rb");
    return file && fread(data, 1, bytes, file.get()) == bytes && fgetc(file.get()) == EOF && !ferror(file.get()) &&
           source == stamp(path);
}

bool loadFeatures(const std::string &directory, const std::vector<ImageRecord> &records, const std::vector<int> &needed,
                  std::vector<uint8_t> &tiny, std::vector<float> &lbp, std::vector<uint8_t> &tinyValid,
                  std::vector<uint8_t> &lbpValid, std::vector<Metadata> &current)
{
    const size_t count = needed.size();
    std::vector<int> slots(records.size(), -1);
    std::unordered_map<int, size_t> ids;
    ids.reserve(records.size());
    for (size_t i = 0; i < records.size(); ++i)
    {
        if (records[i].id < 0 || !ids.emplace(records[i].id, i).second)
        {
            throw std::invalid_argument("invalid feature record IDs");
        }
    }
    for (size_t i = 0; i < count; ++i)
    {
        if (needed[i] < 0 || static_cast<size_t>(needed[i]) >= records.size() || slots[needed[i]] >= 0)
        {
            throw std::invalid_argument("invalid feature candidate indices");
        }
        slots[needed[i]] = static_cast<int>(i);
    }
    tiny.assign(count * 256, 0);
    lbp.assign(count * 256, 0);
    tinyValid.assign(count, 0);
    lbpValid.assign(count, 0);
    current.resize(count);
    const StampSampler sampler(records, needed);
    WorkerPool workers(static_cast<unsigned>(std::max<size_t>(1, std::min<size_t>(count, 8))));
    workers.run(count,
                [&](size_t i)
                {
                    const auto &record = records[needed[i]];
                    auto &metadata = current[i];
                    metadata.id = record.id;
                    metadata.version = record.featureVersion;
                    metadata.imageHash = hashBytes(record.fileHash.data(), record.fileHash.size());
                    metadata.tiny = sampler.get(record.tinyPath);
                    metadata.lbp = sampler.get(record.histPath);
                });

    Manifest manifest;
    std::vector<Metadata> saved;
    bool packed = !directory.empty() && readManifest(directory, manifest) &&
                  readMetadata(directory, manifest, records.size(), saved);
    std::vector<uint8_t> matched(count, 0);
    if (packed)
    {
        auto tinyFile = openFile(generationPath(directory, manifest, ".tiny.bin"), "rb");
        auto lbpFile = openFile(generationPath(directory, manifest, ".lbp.bin"), "rb");
        if (tinyFile)
        {
            setvbuf(tinyFile.get(), nullptr, _IOFBF, 2 * 1024 * 1024);
        }
        if (lbpFile)
        {
            setvbuf(lbpFile.get(), nullptr, _IOFBF, 2 * 1024 * 1024);
        }
        uint32_t tinyCount = 0, lbpCount = 0;
        packed = tinyFile && lbpFile && fread(&tinyCount, 4, 1, tinyFile.get()) == 1 &&
                 fread(&lbpCount, 4, 1, lbpFile.get()) == 1 && tinyCount == records.size() && lbpCount == tinyCount;
        if (packed)
        {
            int previous = -1;
            std::array<uint8_t, 256> t;
            std::array<float, 256> l;
            for (const auto &metadata : saved)
            {
                int32_t ti = -1, li = -1;
                auto record = ids.find(metadata.id);
                if (metadata.id <= previous || record == ids.end() || metadata.tinyValid > 1 || metadata.lbpValid > 1 ||
                    fread(&ti, 4, 1, tinyFile.get()) != 1 || fread(t.data(), 1, 256, tinyFile.get()) != 256 ||
                    fread(&li, 4, 1, lbpFile.get()) != 1 || fread(l.data(), 4, 256, lbpFile.get()) != 256 ||
                    ti != metadata.id || li != ti)
                {
                    packed = false;
                    break;
                }
                previous = ti;
                const int slot = slots[record->second];
                if (slot < 0)
                {
                    continue;
                }
                if (hashBytes(t.data(), 256) != metadata.tinyHash || hashBytes(l.data(), 1024) != metadata.lbpHash)
                {
                    packed = false;
                    break;
                }
                auto &source = current[slot];
                if (source.version == metadata.version && source.imageHash == metadata.imageHash &&
                    source.tiny == metadata.tiny && source.lbp == metadata.lbp)
                {
                    std::copy(t.begin(), t.end(), tiny.begin() + static_cast<size_t>(slot) * 256);
                    std::copy(l.begin(), l.end(), lbp.begin() + static_cast<size_t>(slot) * 256);
                    tinyValid[slot] = static_cast<uint8_t>(metadata.tinyValid);
                    lbpValid[slot] = static_cast<uint8_t>(metadata.lbpValid);
                    matched[slot] = 1;
                    source = metadata;
                }
            }
            packed = packed && fgetc(tinyFile.get()) == EOF && fgetc(lbpFile.get()) == EOF && !ferror(tinyFile.get()) &&
                     !ferror(lbpFile.get());
        }
    }
    if (!packed)
    {
        std::fill(matched.begin(), matched.end(), 0);
    }
    workers.run(count,
                [&](size_t i)
                {
                    if (matched[i])
                    {
                        return;
                    }
                    const auto &record = records[needed[i]];
                    auto &metadata = current[i];
                    tinyValid[i] = readSource(record.tinyPath, metadata.tiny, tiny.data() + i * 256, 256);
                    lbpValid[i] = readSource(record.histPath, metadata.lbp, lbp.data() + i * 256, 1024);
                    if (!tinyValid[i])
                    {
                        std::fill_n(tiny.data() + i * 256, 256, 0);
                    }
                    if (!lbpValid[i] || !std::all_of(lbp.data() + i * 256, lbp.data() + (i + 1) * 256,
                                                     [](float value)
                                                     {
                                                         return std::isfinite(value);
                                                     }))
                    {
                        lbpValid[i] = 0;
                        std::fill_n(lbp.data() + i * 256, 256, 0.0f);
                    }
                    metadata.tinyValid = tinyValid[i];
                    metadata.lbpValid = lbpValid[i];
                    metadata.tinyHash = hashBytes(tiny.data() + i * 256, 256);
                    metadata.lbpHash = hashBytes(lbp.data() + i * 256, 1024);
                });
    return packed && std::all_of(matched.begin(), matched.end(),
                                 [](uint8_t value)
                                 {
                                     return value != 0;
                                 });
}

bool persist(File &file)
{
    if (!file || fflush(file.get()) != 0)
    {
        return false;
    }
#ifdef _WIN32
    if (!FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file.get())))))
    {
        return false;
    }
#else
    if (fsync(fileno(file.get())) != 0)
    {
        return false;
    }
#endif
    return fclose(file.release()) == 0;
}
} // namespace

// 一个会话持有同一代次；包可以预载，来源状态在第一次使用前才验证。
struct FeaturePack::Reader::Impl
{
    using Row = Reader::Row;
    using ReadStats = Reader::ReadStats;
    const std::vector<ImageRecord> &records;
    size_t budget;
    ReadStats statistics;
    Manifest manifest;
    std::vector<Metadata> metadata;
    std::vector<int> positions;
    std::vector<uint8_t> checked;
    std::vector<unsigned> seen;
    unsigned generation = 0;
    File tiny{nullptr, fclose}, lbp{nullptr, fclose};
    std::vector<Row> rows;
    std::list<int> order;
    struct Cached
    {
        Row row;
        std::list<int>::iterator position;
    };
    std::unordered_map<int, Cached> cache;
    Row uncached;

    static bool seek(FILE *file, uint64_t offset)
    {
#ifdef _WIN32
        return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
        return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
    }

    Impl(const std::string &directory, const std::vector<ImageRecord> &input, size_t bytes)
        : records(input), budget(bytes), positions(input.size(), -1), checked(input.size(), 0), seen(input.size(), 0)
    {
        std::unordered_map<int, int> ids;
        for (size_t i = 0; i < records.size(); ++i)
        {
            if (records[i].id < 0 || !ids.emplace(records[i].id, static_cast<int>(i)).second)
            {
                throw std::invalid_argument("invalid feature record IDs");
            }
        }
        if (directory.empty() || !readManifest(directory, manifest) ||
            !readMetadata(directory, manifest, records.size(), metadata))
        {
            return;
        }
        int previous = -1;
        for (size_t i = 0; i < metadata.size(); ++i)
        {
            const auto &entry = metadata[i];
            const auto index = ids.find(entry.id);
            if (entry.id <= previous || index == ids.end() || entry.tinyValid > 1 || entry.lbpValid > 1)
            {
                return;
            }
            previous = entry.id;
            positions[index->second] = static_cast<int>(i);
        }
        const auto tinyPath = generationPath(directory, manifest, ".tiny.bin");
        const auto lbpPath = generationPath(directory, manifest, ".lbp.bin");
        std::error_code error;
        const auto tinySize = fs::file_size(tinyPath, error);
        if (error || tinySize != 4 + static_cast<uint64_t>(records.size()) * 260)
        {
            return;
        }
        const auto lbpSize = fs::file_size(lbpPath, error);
        if (error || lbpSize != 4 + static_cast<uint64_t>(records.size()) * 1028)
        {
            return;
        }
        tiny = openFile(tinyPath, "rb");
        lbp = openFile(lbpPath, "rb");
        if (tiny)
        {
            setvbuf(tiny.get(), nullptr, _IOFBF, 64 * 1024);
        }
        if (lbp)
        {
            setvbuf(lbp.get(), nullptr, _IOFBF, 64 * 1024);
        }
        uint32_t tc = 0, lc = 0;
        if (!tiny || !lbp || fread(&tc, 4, 1, tiny.get()) != 1 || fread(&lc, 4, 1, lbp.get()) != 1 ||
            tc != records.size() || lc != tc)
        {
            tiny.reset();
            lbp.reset();
            return;
        }
        if (records.size() <= budget / sizeof(Row))
        {
            rows.resize(records.size());
            for (size_t i = 0; i < metadata.size(); ++i)
            {
                if (!readPacked(static_cast<int>(i), rows[ids.at(metadata[i].id)], false))
                {
                    // 预载发现半包/损坏时不接受该代次，来源按需读取，避免全库回退 I/O。
                    std::vector<Row>().swap(rows);
                    tiny.reset();
                    lbp.reset();
                    return;
                }
            }
            statistics.preloaded = true;
            statistics.peakBytes = rows.size() * sizeof(Row);
        }
    }

    bool readPacked(int position, Row &row, bool reposition)
    {
        if (!tiny || !lbp || position < 0)
        {
            return false;
        }
        if (reposition && (!seek(tiny.get(), 4 + static_cast<uint64_t>(position) * 260) ||
                           !seek(lbp.get(), 4 + static_cast<uint64_t>(position) * 1028)))
        {
            return false;
        }
        int32_t ti = -1, li = -1;
        const auto &entry = metadata[position];
        if (fread(&ti, 4, 1, tiny.get()) != 1 || fread(row.tiny.data(), 1, 256, tiny.get()) != 256 ||
            fread(&li, 4, 1, lbp.get()) != 1 || fread(row.lbp.data(), 4, 256, lbp.get()) != 256 || ti != entry.id ||
            li != ti || hashBytes(row.tiny.data(), 256) != entry.tinyHash ||
            hashBytes(row.lbp.data(), 1024) != entry.lbpHash)
        {
            return false;
        }
        row.tinyValid = entry.tinyValid != 0;
        row.lbpValid = entry.lbpValid != 0;
        return true;
    }

    void load(int index, Row &row, const StampSampler *sampler)
    {
        const auto &record = records[index];
        const auto ts = sampler ? sampler->get(record.tinyPath) : stamp(record.tinyPath);
        const auto ls = sampler ? sampler->get(record.histPath) : stamp(record.histPath);
        const int position = positions[index];
        if (tiny && lbp && position >= 0)
        {
            const auto &saved = metadata[position];
            if (saved.version == record.featureVersion &&
                saved.imageHash == hashBytes(record.fileHash.data(), record.fileHash.size()) && saved.tiny == ts &&
                saved.lbp == ls && (statistics.preloaded || readPacked(position, row, true)))
            {
                ++statistics.packedRows;
                return;
            }
        }
        if (!record.tinyPath.empty() && ts.size == 256)
        {
            ++statistics.sourceReads;
        }
        row.tinyValid = readSource(record.tinyPath, ts, row.tiny.data(), 256);
        if (!row.tinyValid)
        {
            row.tiny.fill(0);
        }
        if (!record.histPath.empty() && ls.size == 1024)
        {
            ++statistics.sourceReads;
        }
        row.lbpValid =
            readSource(record.histPath, ls, row.lbp.data(), 1024) && std::all_of(row.lbp.begin(), row.lbp.end(),
                                                                                 [](float value)
                                                                                 {
                                                                                     return std::isfinite(value);
                                                                                 });
        if (!row.lbpValid)
        {
            row.lbp.fill(0);
        }
    }

    const Row &get(int index, const StampSampler *sampler = nullptr)
    {
        if (index < 0 || static_cast<size_t>(index) >= records.size())
        {
            throw std::out_of_range("invalid feature index");
        }
        if (statistics.preloaded)
        {
            if (!checked[index])
            {
                ++statistics.misses;
                load(index, rows[index], sampler);
                checked[index] = 1;
            }
            else
            {
                ++statistics.hits;
            }
            return rows[index];
        }
        auto existing = cache.find(index);
        if (existing != cache.end())
        {
            ++statistics.hits;
            order.splice(order.begin(), order, existing->second.position);
            return existing->second.row;
        }
        ++statistics.misses;
        Row row;
        load(index, row, sampler);
        if (budget < sizeof(Row))
        {
            uncached = std::move(row);
            statistics.peakBytes = sizeof(Row); // 一个最小特征行独占，避免非法缩减特征。
            return uncached;
        }
        while (cache.size() >= budget / sizeof(Row))
        {
            cache.erase(order.back());
            order.pop_back();
            ++statistics.evictions;
        }
        order.push_front(index);
        const auto inserted = cache.emplace(index, Cached{std::move(row), order.begin()});
        statistics.peakBytes = std::max(statistics.peakBytes, cache.size() * sizeof(Row));
        return inserted.first->second.row;
    }

    void prepare(const std::vector<int> &candidates)
    {
        if (++generation == 0)
        {
            std::fill(seen.begin(), seen.end(), 0);
            ++generation;
        }
        std::vector<int> needed;
        for (int index : candidates)
        {
            if (index < 0)
            {
                continue;
            }
            if (static_cast<size_t>(index) >= records.size())
            {
                throw std::out_of_range("invalid feature index");
            }
            if (seen[index] == generation)
            {
                continue;
            }
            seen[index] = generation;
            if (statistics.preloaded)
            {
                if (!checked[index])
                {
                    needed.push_back(index);
                }
            }
            else
            {
                auto existing = cache.find(index);
                if (existing == cache.end())
                {
                    needed.push_back(index);
                }
                else
                {
                    // 先固定本批已缓存的访问次序，新条目才能优先淘汰批次外的数据。
                    order.splice(order.begin(), order, existing->second.position);
                }
            }
        }
        if (needed.empty())
        {
            return;
        }
        const StampSampler sampler(records, needed);
        std::sort(needed.begin(), needed.end(),
                  [&](int left, int right)
                  {
                      return positions[left] < positions[right];
                  });
        for (int index : needed)
        {
            get(index, &sampler);
        }
    }
};

FeaturePack::Reader::Reader(const std::string &directory, const std::vector<ImageRecord> &records, size_t budget)
    : m_impl(std::make_unique<Impl>(directory, records, budget))
{
}
FeaturePack::Reader::~Reader() = default;
void FeaturePack::Reader::prepare(const std::vector<int> &candidates)
{
    m_impl->prepare(candidates);
}
const FeaturePack::Reader::Row &FeaturePack::Reader::get(int index)
{
    return m_impl->get(index);
}
const FeaturePack::Reader::ReadStats &FeaturePack::Reader::stats() const
{
    return m_impl->statistics;
}

bool FeaturePack::load(const std::string &directory, const std::vector<ImageRecord> &records,
                       const std::vector<int> &needed, std::vector<uint8_t> &tiny, std::vector<float> &lbp,
                       std::vector<uint8_t> &tinyValid, std::vector<uint8_t> &lbpValid)
{
    std::vector<Metadata> metadata;
    return loadFeatures(directory, records, needed, tiny, lbp, tinyValid, lbpValid, metadata);
}

bool FeaturePack::buildCache(const std::string &directory, const std::vector<ImageRecord> &records, Stats *stats)
{
    const auto start = Clock::now();
    std::vector<uint8_t> tiny, tinyValid, lbpValid;
    std::vector<float> lbp;
    std::vector<Metadata> metadata;
    std::vector<int> needed(records.size());
    std::iota(needed.begin(), needed.end(), 0);
    const bool reused = loadFeatures(directory, records, needed, tiny, lbp, tinyValid, lbpValid, metadata);
    const auto writeStart = Clock::now();
    if (stats)
    {
        *stats = {};
        stats->validationMs = std::chrono::duration<double, std::milli>(writeStart - start).count();
    }
    if (reused)
    {
        if (stats)
        {
            stats->reused = true;
        }
        return true;
    }
    static std::atomic<unsigned> sequence{0};
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    const auto process = getpid();
#endif
    Manifest manifest{std::to_string(process) + "-" + std::to_string(Clock::now().time_since_epoch().count()) + "-" +
                          std::to_string(sequence++),
                      static_cast<uint32_t>(records.size()), kHashStart};
    const auto tinyPath = generationPath(directory, manifest, ".tiny.bin");
    const auto lbpPath = generationPath(directory, manifest, ".lbp.bin");
    const auto metadataPath = generationPath(directory, manifest, ".meta");
    const auto temporary = generationPath(directory, manifest, ".manifest.tmp");
    struct Cleanup
    {
        std::array<fs::path, 4> paths;
        bool published = false;
        ~Cleanup()
        {
            if (!published)
            {
                std::error_code error;
                for (const auto &path : paths)
                {
                    fs::remove(path, error);
                }
            }
        }
    } cleanup{{tinyPath, lbpPath, metadataPath, temporary}};
    auto t = openFile(tinyPath, "wb");
    auto l = openFile(lbpPath, "wb");
    auto m = openFile(metadataPath, "wb");
    if (!t || !l || !m || fwrite(&manifest.count, 4, 1, t.get()) != 1 || fwrite(&manifest.count, 4, 1, l.get()) != 1)
    {
        return false;
    }
    std::sort(needed.begin(), needed.end(),
              [&](int a, int b)
              {
                  return records[a].id < records[b].id;
              });
    for (int index : needed)
    {
        const auto &entry = metadata[index];
        if (fwrite(&entry.id, 4, 1, t.get()) != 1 ||
            fwrite(tiny.data() + static_cast<size_t>(index) * 256, 1, 256, t.get()) != 256 ||
            fwrite(&entry.id, 4, 1, l.get()) != 1 ||
            fwrite(lbp.data() + static_cast<size_t>(index) * 256, 4, 256, l.get()) != 256 ||
            fwrite(&entry, sizeof(entry), 1, m.get()) != 1)
        {
            return false;
        }
        manifest.metadataHash = hashBytes(&entry, sizeof(entry), manifest.metadataHash);
    }
    if (!persist(t) || !persist(l) || !persist(m))
    {
        return false;
    }
    auto publication = openFile(temporary, "wb");
    if (!publication ||
        fprintf(publication.get(), "MOSAICRAFT_PACK 3 %s %u %llu\n", manifest.generation.c_str(), manifest.count,
                static_cast<unsigned long long>(manifest.metadataHash)) < 0 ||
        !persist(publication))
    {
        return false;
    }
    // 只替换一个小清单，读者不会看到 tiny/LBP 来自不同代次的半成品。
    const auto destination = u8path(directory) / "features.pack";
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        return false;
    }
#else
    std::error_code error;
    fs::rename(temporary, destination, error);
    if (error)
    {
        return false;
    }
#endif
    cleanup.published = true;
#ifndef _WIN32
    const int descriptor = open(u8path(directory).c_str(), O_RDONLY);
    const bool flushed = descriptor >= 0 && fsync(descriptor) == 0;
    if (descriptor >= 0)
    {
        close(descriptor);
    }
    if (!flushed)
    {
        return false;
    }
#endif
    if (stats)
    {
        stats->writingMs = std::chrono::duration<double, std::milli>(Clock::now() - writeStart).count();
    }
    return true;
}
void FeaturePack::removeObsolete(const std::string &directory)
{
    Manifest manifest;
    if (!readManifest(directory, manifest))
    {
        return;
    }
    const std::string current = "features-" + manifest.generation;
    std::error_code error;
    fs::directory_iterator iterator(u8path(directory), error), end;
    while (!error && iterator != end)
    {
        const auto entry = *iterator;
        const auto name = pathToUtf8(entry.path().filename());
        if (name.rfind("features-", 0) == 0 && name.rfind(current + ".", 0) != 0)
        {
            for (const std::string suffix : {".tiny.bin", ".lbp.bin", ".meta", ".manifest.tmp"})
            {
                if (name.size() > 9 + suffix.size() &&
                    name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
                {
                    const auto token = name.substr(9, name.size() - 9 - suffix.size());
                    if (token.find_first_not_of("0123456789-") == std::string::npos)
                    {
                        std::error_code removeError;
                        fs::remove(entry.path(), removeError);
                    }
                }
            }
        }
        iterator.increment(error);
    }
}
} // namespace mosaicraft
