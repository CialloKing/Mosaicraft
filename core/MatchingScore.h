#pragma once
#include "FeatureUtils.h"
#include "MatchingBatch.h"

namespace mosaicraft
{
// 这是自定义尺寸原 CPU 表达式。顺序决定舍入和平分结果，不能重排成点积或融合乘加。
inline double cpuBaseScore(const MatchingBuffers &tiles, size_t tile, const ImageRecord &record,
                           const FeaturePack::Reader::Row &features, double usePenalty)
{
    const double lab = tiles.labW[tile] *
        labDistance(tiles.l[tile], tiles.a[tile], tiles.b[tile], record.avgL, record.avgA, record.avgB);
    const double grid = tiles.gridW[tile] * gridDistance8x8(tiles.grid[tile], record.gridLab, true);
    const double edge = tiles.edgeW[tile] * std::abs(tiles.edge[tile] - record.edgeDensity);
    return lab + grid +
        tiles.tinyW[tile] * (features.tinyValid ? tinyMSE(tiles.tiny[tile], features.tiny) : 1.0) + edge +
        tiles.lbpW[tile] * (features.lbpValid ? lbpDistance(tiles.lbp[tile], features.lbp) : 1.0) +
        record.useCount * usePenalty;
}
// 同一格减去公共使用历史不改变排序。先用整数相减，避免 FP32 抹去相邻的大计数。
struct RowHistory
{
    int minimum = 0;
    bool wide = false;

    RowHistory(const int *indices, int count, const std::vector<ImageRecord> &records, double penalty)
    {
        int low = std::numeric_limits<int>::max(), high = 0;
        for (int i = 0; i < count; ++i)
        {
            if (indices[i] >= 0)
            {
                const int uses = records[indices[i]].useCount;
                low = std::min(low, uses);
                high = std::max(high, uses);
            }
        }
        minimum = low;
        // 以 2^-16 为快路径的绝对精度预算，超出时仅提升历史合成的精度。
        wide = penalty > std::numeric_limits<float>::max() || (double(high) - low) * penalty > 128.0;
    }
    double combine(float distance, int uses, double penalty) const
    {
        const auto delta = int64_t(uses) - minimum;
        if (wide)
        {
            return std::min(double(distance) + double(delta) * penalty, std::numeric_limits<double>::max() / 2);
        }
        return distance + float(delta) * float(penalty);
    }
};
} // namespace mosaicraft
