#pragma once

#include "Database.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// hnswlib headers (vendored in core/)
#include "hnswlib.h"

namespace mosaicraft
{

// ============================================================
// FeatureIndex — HNSW 近似最近邻索引
// 将图库特征向量组织为可快速检索的图结构
// 用于替代 L-range 候选筛选，支持 10 万+ 规模图库
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

    // 从 allRecords 构建索引
    // 特征维度: 3(LAB) + 48(Grid) + 256(Tiny) + 1(Edge) + 256(LBP) = 564
    bool build(const std::vector<ImageRecord>& records)
    {
        int count = static_cast<int>(records.size());
        if (count == 0) return false;

        constexpr int dim = 564;
        m_dim = dim;

        // 构建特征矩阵
        std::vector<float> data(count * dim, 0.0f);
        for (int i = 0; i < count; ++i)
        {
            const auto& rec = records[i];
            float* vec = &data[i * dim];
            int off = 0;

            // LAB (3): /255 归一化
            vec[off++] = static_cast<float>(rec.avgL / 255.0);
            vec[off++] = static_cast<float>(rec.avgA / 255.0);
            vec[off++] = static_cast<float>(rec.avgB / 255.0);

            // Grid4x4 (48): /255
            for (int j = 0; j < 48 && j < static_cast<int>(rec.grid4x4.size()); ++j)
                vec[off++] = rec.grid4x4[j] / 255.0f;
            for (int j = static_cast<int>(rec.grid4x4.size()); j < 48; ++j)
                vec[off++] = 0.0f;

            // TinyImage (256): /255
            // Tiny 数据在外部文件中，此处用 0 填充（仅用于快速筛选）
            // 完整的 tiny 特征在 GPU 端评分时使用
            for (int j = 0; j < 256; ++j)
                vec[off++] = 0.0f;

            // Edge density (1): already 0-1
            vec[off++] = static_cast<float>(rec.edgeDensity);

            // LBP histogram (256): /255 (LBP 数据在外部文件)
            for (int j = 0; j < 256; ++j)
                vec[off++] = 0.0f;
        }

        // 创建 L2 空间索引
        m_space = new hnswlib::L2Space(dim);
        m_index = new hnswlib::HierarchicalNSW<float>(m_space, count, 16, 200);
        // 逐点添加：hnswlib::addPoint 每次只加一个向量，label 即 0-based 库索引
        for (int i = 0; i < count; ++i) {
            m_index->addPoint(&data[i * dim], i);
        }

        return true;
    }

    // 查询最近邻，返回库中索引 (0-based)
    // tileFeatures: [LAB(3), Grid(48), Tiny(256), Edge(1), LBP(256)]
    std::vector<int> query(const float* tileVec, int k)
    {
        std::vector<int> result;
        if (!m_index) return result;

        auto pq = m_index->searchKnn(tileVec, k);
        while (!pq.empty())
        {
            result.push_back(pq.top().second);
            pq.pop();
        }
        // HNSW 返回距离升序，保持原序
        std::reverse(result.begin(), result.end());
        return result;
    }

    int dimension() const { return m_dim; }

private:
    hnswlib::HierarchicalNSW<float>* m_index;
    hnswlib::SpaceInterface<float>* m_space;
    int m_dim = 0;
};

// ============================================================
// 构建 tile 特征向量 (564 维)
// 用于 ANN 查询
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
