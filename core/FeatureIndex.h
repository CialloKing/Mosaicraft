#pragma once

#include "Database.h"
#include "WorkerPool.h"
#include "UnicodeIO.h"
#include <array>
#include <memory>
#include <limits>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <queue>
#include <unordered_map>
#include <vector>

#include "hnswlib.h"

namespace mosaicraft
{

// ============================================================
// FeatureIndex — HNSW 近似最近邻索引（支持持久化）
//
// HNSW label 使用稳定的 image_id（而非 allRecords 下标），
// 这样索引可在 build 时保存、mosaic 时加载，无需每次重建。
// ============================================================
class FeatureIndex
{
public:
    static constexpr int kDimension = 196;

    FeatureIndex() : m_index(nullptr), m_space(nullptr) {}

    // 禁止拷贝（管理原始指针，拷贝会导致double-free）
    FeatureIndex(const FeatureIndex&) = delete;
    FeatureIndex& operator=(const FeatureIndex&) = delete;

    ~FeatureIndex()
    {
        delete m_index; m_index = nullptr;
        delete m_space; m_space = nullptr;
    }

    // 从 records 构建索引（label = image_id）
    bool build(const std::vector<ImageRecord>& records)
    {
        int count = static_cast<int>(records.size());
        if (count == 0) return false;

        // 释放旧索引
        delete m_index; m_index = nullptr;
        delete m_space; m_space = nullptr;
        m_idToIndex.clear();

        constexpr int dim = kDimension;
        m_dim = dim;

        // 构建 id → index 映射（供查询后用）
        m_idToIndex.reserve(count);
        for (int i = 0; i < count; ++i)
            m_idToIndex[records[i].id] = i;

        // 构建特征矩阵 + 收集 image_id
        std::vector<float> data(static_cast<size_t>(count) * dim, 0.0f);
        for (int i = 0; i < count; ++i)
        {
            const auto& rec = records[i];
            float* vec = &data[i * dim];
            int off = 0;

            vec[off++] = static_cast<float>(rec.avgL / 255.0);
            vec[off++] = static_cast<float>(rec.avgA / 255.0);
            vec[off++] = static_cast<float>(rec.avgB / 255.0);

            for (int j = 0; j < 192 && j < static_cast<int>(rec.grid4x4.size()); ++j)
                vec[off++] = rec.grid4x4[j] / 255.0f;
            for (int j = static_cast<int>(rec.grid4x4.size()); j < 192; ++j)
                vec[off++] = 0.0f;

            // 粗筛仅保留有区分度的特征，Tiny/LBP 的真实值仍用于精排。
            vec[off++] = static_cast<float>(rec.edgeDensity);

        }

        m_space = new hnswlib::L2Space(dim);
        m_index = new hnswlib::HierarchicalNSW<float>(m_space, count, 16, 200);
        // label = image_id（稳定，不随 use_count 排序变化）
        for (int i = 0; i < count; ++i)
            m_index->addPoint(&data[i * dim], records[i].id);

        m_count = count;
        m_fingerprint = fingerprint(records);
        return true;
    }

    // 保存索引到文件
    bool save(const std::string& path)
    {
        if (!m_index) return false;
        const auto temporary = path + ".tmp";
        try
        {
            m_index->saveIndex(temporary);
            const auto digest = fileDigest(temporary);
            std::ofstream meta(u8path(path + ".meta.tmp"));
            meta << "MOSAICRAFT_ANN 1 " << kDimension << ' ' << m_count << ' '
                 << m_fingerprint << ' ' << digest << '\n';
            meta.close();
            if (!meta)
            {
                return false;
            }
            // 元数据最后提交；中途退出时校验失败会重建，不会接受半写缓存。
            std::error_code ec;
            std::filesystem::remove(u8path(path), ec);
            std::filesystem::rename(u8path(temporary), u8path(path));
            std::filesystem::remove(u8path(path + ".meta"), ec);
            std::filesystem::rename(u8path(path + ".meta.tmp"), u8path(path + ".meta"));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // 从文件加载索引
    bool load(const std::string& path, int dim,
              const std::vector<ImageRecord>& records)
    {
        constexpr int kDim = kDimension;
        if (dim != kDim) return false;

        int count = static_cast<int>(records.size());
        if (count == 0) return false;

        std::string magic;
        int version = 0, storedDimension = 0, storedCount = 0;
        uint64_t storedFingerprint = 0, digest = 0;
        std::ifstream meta(u8path(path + ".meta"));
        if (!(meta >> magic >> version >> storedDimension >> storedCount >> storedFingerprint >> digest)
            || magic != "MOSAICRAFT_ANN" || version != 1 || storedDimension != kDimension
            || storedCount != count || storedFingerprint != fingerprint(records)
            || !digest || digest != fileDigest(path))
        {
            return false;
        }

        // 释放旧索引
        delete m_index; m_index = nullptr;
        delete m_space; m_space = nullptr;
        m_idToIndex.clear();

        // 构建 id→index 映射
        m_idToIndex.reserve(count);
        for (int i = 0; i < count; ++i)
            m_idToIndex[records[i].id] = i;

        m_space = new hnswlib::L2Space(kDim);
        m_index = new hnswlib::HierarchicalNSW<float>(m_space, count, 16, 200);
        try
        {
            m_index->loadIndex(path, m_space, count);
        }
        catch (...)
        {
            delete m_index; m_index = nullptr;
            delete m_space; m_space = nullptr;
            return false;
        }

        m_fingerprint = storedFingerprint;
        m_dim = kDim;
        m_count = count;

        // 校验：索引元素数须与当前 records 一致
        if (static_cast<int>(m_index->cur_element_count) != count)
        {
            delete m_index; m_index = nullptr;
            delete m_space; m_space = nullptr;
            m_idToIndex.clear();
            return false;  // 缓存不同步→触发重建
        }

        return true;
    }

    // 查询（返回 image_id 列表，按距离升序）
    // 调用方自行通过 idToAllRecordsIndex() 转换为 allRecords 下标
    std::vector<int> query(const float* tileVec, int k) const
    {
        std::vector<int> result;
        if (!m_index || !tileVec || k <= 0 || m_count <= 0) return result;
        k = std::min(k, m_count);

        std::priority_queue<std::pair<float, hnswlib::labeltype>> pq;
        try
        {
            pq = m_index->searchKnn(tileVec, static_cast<size_t>(k));
        }
        catch (...)
        {
            return result;
        }
        while (!pq.empty())
        {
            result.push_back(static_cast<int>(pq.top().second));
            pq.pop();
        }
        std::reverse(result.begin(), result.end());
        return result;
    }

    // 将 image_id 转换为 allRecords 数组下标
    int idToAllRecordsIndex(int imageId) const
    {
        auto it = m_idToIndex.find(imageId);
        return (it != m_idToIndex.end()) ? it->second : -1;
    }

    // 每个查询的堆和访问表独立，结果写入固定区间，选图次序不受调度影响。
    std::vector<int> queryBatch(const std::vector<float>& queries, int k, unsigned threads = 0) const
    {
        if (k <= 0 || queries.size() % kDimension != 0)
        {
            throw std::invalid_argument("invalid ANN batch");
        }
        const size_t count = queries.size() / kDimension;
        if (count > std::numeric_limits<size_t>::max() / static_cast<size_t>(k))
        {
            throw std::overflow_error("ANN batch size overflow");
        }
        std::vector<int> indices(count * k, -1);
        if (count == 0)
        {
            return indices;
        }
        unsigned workers = threads ? threads : std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
        WorkerPool pool(static_cast<unsigned>(std::min<size_t>(workers, count)));
        pool.run(count, [&](size_t i)
        {
            auto ids = query(queries.data() + i * kDimension, k);
            for (size_t j = 0; j < ids.size(); ++j)
            {
                indices[i * k + j] = idToAllRecordsIndex(ids[j]);
            }
        });
        return indices;
    }

    int dimension() const { return m_dim; }
    int count() const { return m_count; }

private:
    static uint64_t appendHash(uint64_t hash, uint32_t value)
    {
        for (unsigned shift = 0; shift < 32; shift += 8)
        {
            hash = (hash ^ ((value >> shift) & 255)) * 1099511628211ULL;
        }
        return hash;
    }

    static uint64_t fingerprint(const std::vector<ImageRecord>& records)
    {
        std::vector<const ImageRecord*> sorted;
        sorted.reserve(records.size());
        for (const auto& record : records)
        {
            sorted.push_back(&record);
        }
        std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return a->id < b->id; });
        uint64_t hash = 14695981039346656037ULL;
        for (const auto* record : sorted)
        {
            hash = appendHash(hash, static_cast<uint32_t>(record->id));
            std::array<float, kDimension> values{};
            values[0] = static_cast<float>(record->avgL / 255.0);
            values[1] = static_cast<float>(record->avgA / 255.0);
            values[2] = static_cast<float>(record->avgB / 255.0);
            for (size_t i = 0; i < std::min<size_t>(192, record->grid4x4.size()); ++i)
            {
                values[3 + i] = record->grid4x4[i] / 255.0f;
            }
            values.back() = static_cast<float>(record->edgeDensity);
            for (float value : values)
            {
                uint32_t bits;
                std::memcpy(&bits, &value, sizeof(bits));
                hash = appendHash(hash, bits);
            }
        }
        return hash;
    }

    static uint64_t fileDigest(const std::string& path)
    {
        std::ifstream input(u8path(path), std::ios::binary);
        if (!input)
        {
            return 0;
        }
        std::array<char, 65536> buffer;
        uint64_t hash = 14695981039346656037ULL;
        while (input.read(buffer.data(), buffer.size()) || input.gcount())
        {
            for (std::streamsize i = 0; i < input.gcount(); ++i)
            {
                hash = (hash ^ static_cast<unsigned char>(buffer[i])) * 1099511628211ULL;
            }
        }
        return input.bad() ? 0 : hash;
    }

    uint64_t m_fingerprint = 0;
    hnswlib::HierarchicalNSW<float>* m_index;
    hnswlib::SpaceInterface<float>* m_space;
    std::unordered_map<int, int> m_idToIndex;  // image_id → allRecords index
    int m_dim = 0;
    int m_count = 0;
};

// ============================================================
// 构建粗筛向量 (196 维)，精排仍使用全部五特征
// ============================================================
inline void buildTileVector(
    double tL, double tA, double tB,
    const std::vector<float>& grid,
    const std::vector<uint8_t>& tiny,
    double edge,
    const std::vector<float>& lbp,
    std::vector<float>& out)
{
    out.resize(FeatureIndex::kDimension);
    int off = 0;

    out[off++] = static_cast<float>(tL / 255.0);
    out[off++] = static_cast<float>(tA / 255.0);
    out[off++] = static_cast<float>(tB / 255.0);

    for (int j = 0; j < 192; ++j)
        out[off++] = (j < static_cast<int>(grid.size())) ? grid[j] / 255.0f : 0.0f;

    (void)tiny;
    (void)lbp;

    out[off++] = static_cast<float>(edge);

}

} // namespace mosaicraft
