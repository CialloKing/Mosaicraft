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
    const double grid = tiles.gridW[tile] * gridDistance8x8(tiles.grid[tile], record.grid4x4, true);
    const double edge = tiles.edgeW[tile] * std::abs(tiles.edge[tile] - record.edgeDensity);
    return lab + grid +
        tiles.tinyW[tile] * (features.tinyValid ? tinyMSE(tiles.tiny[tile], features.tiny) : 1.0) + edge +
        tiles.lbpW[tile] * (features.lbpValid ? lbpDistance(tiles.lbp[tile], features.lbp) : 1.0) +
        record.useCount * usePenalty;
}
} // namespace mosaicraft
