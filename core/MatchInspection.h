#pragma once

#include "FeaturePack.h"
#include "FeatureUtils.h"
#include "MosaicEngine.h"

namespace mosaicraft
{
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
