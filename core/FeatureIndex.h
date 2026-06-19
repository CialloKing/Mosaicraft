#pragma once

#include "Database.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
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
    FeatureIndex() : m_index(nullptr), m_space(nullptr) {}

    ~FeatureIndex()
    {
        if (m_index) { delete m_index; }
        if (m_space) { delete m_space; }
    }

    // 从 records 构建索引（label = image_id）
    bool build(const std::vector<ImageRecord>& records)
    {
        int count = static_cast<int>(records.size());
        if (count == 0) return false;

        constexpr int dim = 564;
        m_dim = dim;

        // 构建 id → index 映射（供查询后用）
        m_idToIndex.reserve(count);
        for (int i = 0; i < count; ++i)
            m_idToIndex[records[i].id] = i;

        // 构建特征矩阵 + 收集 image_id
        std::vector<float> data(count * dim, 0.0f);
        for (int i = 0; i < count; ++i)
        {
            const auto& rec = records[i];
            float* vec = &data[i * dim];
            int off = 0;

            vec[off++] = static_cast<float>(rec.avgL / 255.0);
            vec[off++] = static_cast<float>(rec.avgA / 255.0);
            vec[off++] = static_cast<float>(rec.avgB / 255.0);

            for (int j = 0; j < 48 && j < static_cast<int>(rec.grid4x4.size()); ++j)
                vec[off++] = rec.grid4x4[j] / 255.0f;
            for (int j = static_cast<int>(rec.grid4x4.size()); j < 48; ++j)
                vec[off++] = 0.0f;

            // Tiny/LBP 在 ANN 阶段用 0 填充，完整特征由 GPU 精排使用
            for (int j = 0; j < 256; ++j) vec[off++] = 0.0f;

            vec[off++] = static_cast<float>(rec.edgeDensity);

            for (int j = 0; j < 256; ++j) vec[off++] = 0.0f;
        }

        m_space = new hnswlib::L2Space(dim);
        m_index = new hnswlib::HierarchicalNSW<float>(m_space, count, 16, 200);
        // label = image_id（稳定，不随 use_count 排序变化）
        for (int i = 0; i < count; ++i)
            m_index->addPoint(&data[i * dim], records[i].id);

        m_count = count;
        return true;
    }

    // 保存索引到文件
    bool save(const std::string& path)
    {
        if (!m_index) return false;
        try { m_index->saveIndex(path); return true; }
        catch (...) { return false; }
    }

    // 从文件加载索引
    bool load(const std::string& path, int dim,
              const std::vector<ImageRecord>& records)
    {
        constexpr int kDim = 564;
        if (dim != kDim) return false;

        int count = static_cast<int>(records.size());
        if (count == 0) return false;

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

        m_dim = kDim;
        m_count = count;
        return true;
    }

    // 查询（返回 image_id 列表，按距离升序）
    // 调用方自行通过 idToAllRecordsIndex() 转换为 allRecords 下标
    std::vector<int> query(const float* tileVec, int k)
    {
        std::vector<int> result;
        if (!m_index) return result;

        auto pq = m_index->searchKnn(tileVec, k);
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

    int dimension() const { return m_dim; }
    int count() const { return m_count; }

private:
    hnswlib::HierarchicalNSW<float>* m_index;
    hnswlib::SpaceInterface<float>* m_space;
    std::unordered_map<int, int> m_idToIndex;  // image_id → allRecords index
    int m_dim = 0;
    int m_count = 0;
};

// ============================================================
// 构建 tile 特征向量 (564 维)
// ============================================================
inline void buildTileVector(
    double tL, double tA, double tB,
    const std::vector<float>& grid,
    const std::vector<uint8_t>& tiny,
    double edge,
    const std::vector<float>& lbp,
    std::vector<float>& out)
{
    out.resize(564);
    int off = 0;

    out[off++] = static_cast<float>(tL / 255.0);
    out[off++] = static_cast<float>(tA / 255.0);
    out[off++] = static_cast<float>(tB / 255.0);

    for (int j = 0; j < 48; ++j)
        out[off++] = (j < static_cast<int>(grid.size())) ? grid[j] / 255.0f : 0.0f;

    for (int j = 0; j < 256; ++j)
        out[off++] = (j < static_cast<int>(tiny.size())) ? tiny[j] / 255.0f : 0.0f;

    out[off++] = static_cast<float>(edge);

    for (int j = 0; j < 256; ++j)
        out[off++] = (j < static_cast<int>(lbp.size())) ? lbp[j] : 0.0f;
}

} // namespace mosaicraft
