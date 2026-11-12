#pragma once

#include "FeaturePack.h"
#include "FeatureUtils.h"
#include "MosaicEngine.h"

namespace mosaicraft
{
enum class TileClass { Smooth, Edge, Texture, Normal };

template <typename Grid, typename Lbp>
TileClass classifyTile(double edge, const Grid &grid, const Lbp &lbp)
{
    if (edge > 0.01)
    {
        return TileClass::Edge;
    }
    if (edge < 0.005)
    {
        double sum = 0, squares = 0;
        for (int cell = 0; cell < 64; ++cell)
        {
            const double lightness = grid[cell * 3];
            sum += lightness;
            squares += lightness * lightness;
        }
        if (squares / 64.0 - (sum / 64.0) * (sum / 64.0) < 100.0)
        {
            return TileClass::Smooth;
        }
    }
    double entropy = 0;
    for (int bin = 0; bin < 256; ++bin)
    {
        const float value = lbp[bin];
        if (value > 0.0f)
        {
            entropy -= value * std::log2(value);
        }
    }
    return entropy > 3.0 ? TileClass::Texture : TileClass::Normal;
}

using MatchWeights = std::array<double, 5>;

inline MatchWeights adaptiveTileWeights(TileClass type, const MatchWeights &defaults)
{
    switch (type)
    {
    case TileClass::Smooth: return {0.25, 0.45, 0.20, 0.05, 0.05};
    case TileClass::Edge: return {0.15, 0.40, 0.25, 0.15, 0.05};
    case TileClass::Texture: return {0.15, 0.40, 0.20, 0.05, 0.20};
    case TileClass::Normal: return defaults;
    }
    return defaults;
}

using FeatureDistances = std::array<double, 5>;

template <class Grid>
FeatureDistances featureDistances(double l, double a, double b, const Grid &grid, double edge,
                                  const FeaturePack::Reader::Row &query, const ImageRecord &record,
                                  const FeaturePack::Reader::Row &candidate)
{
    return {labDistance(l, a, b, record.avgL, record.avgA, record.avgB),
            gridDistance8x8(grid, record.gridLab, true),
            query.tinyValid && candidate.tinyValid ? tinyMSE(query.tiny, candidate.tiny) : 1.0,
            std::abs(edge - record.edgeDensity),
            query.lbpValid && candidate.lbpValid ? lbpDistance(query.lbp, candidate.lbp) : 1.0};
}

inline double inspectionScore(const FeatureDistances &distances, const MosaicEngine::Config &config)
{
    const FeatureDistances weights{config.labWeight, config.gridWeight, config.tinyWeight, config.edgeWeight, config.lbpWeight};
    double sum = 0, weighted = 0;
    for (size_t i = 0; i < weights.size(); ++i)
    {
        sum += weights[i];
        weighted += weights[i] * distances[i];
    }
    return weighted / sum;
}
} // namespace mosaicraft
