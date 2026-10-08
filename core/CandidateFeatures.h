#pragma once
#include "FeatureMatrix.h"
#include "FeaturePack.h"
#include "UnicodeIO.h"
#include "WorkerPool.h"
#include <chrono>
#include <fstream>

namespace mosaicraft
{
// 缓存包仅是加速层；源文件有效性与 CPU 原来的缺失惩罚分别保存。
class CandidateFeatures
{
    struct Stamp
    {
        uint64_t path = 0, size = UINT64_MAX, time = 0;
        bool operator==(const Stamp &other) const
        {
            return path == other.path && size == other.size && time == other.time;
        }
    };
    struct Metadata
    {
        Stamp tiny, lbp;
        uint64_t tinyHash = 0, lbpHash = 0;
        bool tinyValid = false, lbpValid = false;
    };
    std::vector<int> m_slots;
    std::vector<uint8_t> m_tiny;
    std::vector<float> m_lbp;
    std::vector<uint8_t> m_tinyValid, m_lbpValid;

    static uint64_t hash(const void *data, size_t bytes)
    {
        auto p = static_cast<const uint8_t *>(data);
        uint64_t result = UINT64_C(14695981039346656037);
        for (size_t i = 0; i < bytes; ++i)
        {
            result = (result ^ p[i]) * UINT64_C(1099511628211);
        }
        return result;
    }
    static Stamp stamp(const std::string &path)
    {
        Stamp result;
        result.path = hash(path.data(), path.size());
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
        auto source = u8path(path);
        auto size = std::filesystem::file_size(source, error);
        if (!error)
        {
            auto time = std::filesystem::last_write_time(source, error);
            if (!error)
            {
                result.size = size;
                result.time = static_cast<uint64_t>(time.time_since_epoch().count());
            }
        }
#endif
        return result;
    }
    static bool read(const std::string &path, void *data, size_t bytes)
    {
        std::unique_ptr<FILE, decltype(&fclose)> file(u8fopen(path, "rb"), fclose);
        return file && fread(data, 1, bytes, file.get()) == bytes;
    }
    static std::unordered_map<int, Metadata> readMetadata(const std::string &path)
    {
        std::ifstream input(u8path(path));
        std::string magic;
        size_t count = 0;
        if (!(input >> magic >> count) || magic != "MOSAICRAFT_FEATURES_1" || count > 500000)
        {
            return {};
        }
        std::unordered_map<int, Metadata> records;
        for (size_t i = 0; i < count; ++i)
        {
            int id;
            Metadata m;
            if (!(input >> id >> m.tiny.path >> m.tiny.size >> m.tiny.time >> m.tinyHash >> m.tinyValid >> m.lbp.path >>
                  m.lbp.size >> m.lbp.time >> m.lbpHash >> m.lbpValid) ||
                id < 0 || !records.emplace(id, m).second)
            {
                return {};
            }
        }
        return records;
    }
    static void saveMetadata(const std::string &path, const std::unordered_map<int, Metadata> &records)
    {
        const auto temporary =
            u8path(path + ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::ofstream output(temporary, std::ios::trunc);
        output << "MOSAICRAFT_FEATURES_1 " << records.size() << '\n';
        for (const auto &[id, m] : records)
        {
            output << id << ' ' << m.tiny.path << ' ' << m.tiny.size << ' ' << m.tiny.time << ' ' << m.tinyHash << ' '
                   << m.tinyValid << ' ' << m.lbp.path << ' ' << m.lbp.size << ' ' << m.lbp.time << ' ' << m.lbpHash
                   << ' ' << m.lbpValid << '\n';
        }
        output.close();
        std::error_code error;
        if (output)
        {
            std::filesystem::rename(temporary, u8path(path), error);
        }
        // 缓存只读或更新失败不影响本次生成；不修改现有 v2 包。
        std::filesystem::remove(temporary, error);
    }

  public:
    CandidateFeatures(const std::vector<ImageRecord> &records, const std::vector<int> &candidates,
                      const std::string &directory)
        : m_slots(records.size(), -1)
    {
        std::vector<int> needed;
        for (int index : candidates)
        {
            if (index >= 0 && m_slots[index] < 0)
            {
                m_slots[index] = static_cast<int>(needed.size());
                needed.push_back(index);
            }
        }
        const size_t count = needed.size();
        m_tiny.resize(count * 256);
        m_lbp.resize(count * 256);
        m_tinyValid.resize(count);
        m_lbpValid.resize(count);
        std::vector<Metadata> current(count);
        const auto metaPath = directory + "/features.meta";
        auto metadata = directory.empty() ? std::unordered_map<int, Metadata>{} : readMetadata(metaPath);
        std::unordered_map<int, int> ids;
        for (size_t i = 0; i < records.size(); ++i)
        {
            ids.emplace(records[i].id, static_cast<int>(i));
        }
        WorkerPool workers(static_cast<unsigned>(std::max<size_t>(
            1, std::min<size_t>(count, std::min(8u, std::max(1u, std::thread::hardware_concurrency()))))));
        workers.run(count,
                    [&](size_t i)
                    {
                        current[i].tiny = stamp(records[needed[i]].tinyPath);
                        current[i].lbp = stamp(records[needed[i]].histPath);
                    });
        const bool validPack =
            !directory.empty() &&
            FeaturePack::visit(
                directory, records.size(),
                [&](int id, const auto &tiny, const auto &lbp)
                {
                    auto record = ids.find(id);
                    if (record == ids.end())
                    {
                        return false;
                    }
                    const int slot = m_slots[record->second];
                    auto saved = metadata.find(id);
                    if (slot >= 0 && saved != metadata.end())
                    {
                        const auto &m = saved->second;
                        if (m.tinyValid && m.tiny == current[slot].tiny && m.tinyHash == hash(tiny.data(), 256))
                        {
                            std::memcpy(m_tiny.data() + slot * 256, tiny.data(), 256);
                            m_tinyValid[slot] = true;
                        }
                        if (m.lbpValid && m.lbp == current[slot].lbp && m.lbpHash == hash(lbp.data(), 1024))
                        {
                            std::memcpy(m_lbp.data() + slot * 256, lbp.data(), 1024);
                            m_lbpValid[slot] = true;
                        }
                    }
                    return true;
                });
        if (!validPack)
        {
            std::fill(m_tinyValid.begin(), m_tinyValid.end(), false);
            std::fill(m_lbpValid.begin(), m_lbpValid.end(), false);
        }
        std::atomic<bool> changed{!validPack};
        workers.run(count,
                    [&](size_t i)
                    {
                        const auto &record = records[needed[i]];
                        auto &m = current[i];
                        if (!m_tinyValid[i])
                        {
                            m_tinyValid[i] =
                                !record.tinyPath.empty() && read(record.tinyPath, m_tiny.data() + i * 256, 256);
                            changed = true;
                        }
                        if (!m_lbpValid[i])
                        {
                            m_lbpValid[i] =
                                !record.histPath.empty() && read(record.histPath, m_lbp.data() + i * 256, 1024);
                            changed = true;
                        }
                        m.tinyValid = m_tinyValid[i];
                        m.lbpValid = m_lbpValid[i];
                        m.tinyHash = hash(m_tiny.data() + i * 256, 256);
                        m.lbpHash = hash(m_lbp.data() + i * 256, 1024);
                        // 源文件在读取期间发生变化时不认证本次值，下次仍回退读取。
                        if (!(m.tiny == stamp(record.tinyPath)))
                        {
                            m.tinyValid = false;
                        }
                        if (!(m.lbp == stamp(record.histPath)))
                        {
                            m.lbpValid = false;
                        }
                    });
        for (size_t i = 0; i < count; ++i)
        {
            metadata[records[needed[i]].id] = current[i];
        }
        for (auto it = metadata.begin(); it != metadata.end();)
        {
            if (ids.find(it->first) == ids.end())
            {
                it = metadata.erase(it);
            }
            else
            {
                ++it;
            }
        }
        if (changed && !directory.empty())
        {
            saveMetadata(metaPath, metadata);
        }
    }
    bool hasTiny(int index) const
    {
        return m_tinyValid[m_slots[index]];
    }
    bool hasLbp(int index) const
    {
        return m_lbpValid[m_slots[index]];
    }
    auto tiny(int index) const
    {
        return FeatureMatrix<uint8_t, 256>::Row<const uint8_t>(m_tiny.data() + m_slots[index] * 256);
    }
    auto lbp(int index) const
    {
        return FeatureMatrix<float, 256>::Row<const float>(m_lbp.data() + m_slots[index] * 256);
    }
};
} // namespace mosaicraft
