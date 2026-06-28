#pragma once

#include "MosaicService.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mosaicraft
{

struct DatabaseRequest
{
    std::string dbPath = "library/mosaicraft.db";
};

struct HistogramBin
{
    int lo = 0;
    int hi = 0;
    int count = 0;
};

struct DatabaseStats
{
    bool empty = true;
    int total = 0;
    std::string featureWidth;
    std::string featureHeight;
    int gridDim = 0;
    double minL = 0.0, maxL = 0.0, avgL = 0.0;
    double minA = 0.0, maxA = 0.0, avgA = 0.0;
    double minB = 0.0, maxB = 0.0, avgB = 0.0;
    int dark = 0, mid = 0, bright = 0;
    std::vector<HistogramBin> lHistogram;
    std::vector<std::string> coverageGaps;
};

struct DatabaseHealth
{
    bool empty = true;
    int total = 0;
    int dark = 0, mid = 0, bright = 0;
    double darkPct = 0.0, midPct = 0.0, brightPct = 0.0;
    double minA = 0.0, maxA = 0.0, minB = 0.0, maxB = 0.0;
    int usedCount = 0;
    int unusedCount = 0;
    double usedPct = 0.0;
    double unusedPct = 0.0;
    int topHotspotCount = 0;
    double topHotspotTilePct = 0.0;
    std::vector<std::string> warnings;
    std::vector<std::string> recommendations;
};

struct DatabaseStatsResult
{
    ServiceResult status;
    DatabaseStats stats;
};

struct DatabaseHealthResult
{
    ServiceResult status;
    DatabaseHealth health;
};

class DatabaseService
{
public:
    DatabaseStatsResult stats(const DatabaseRequest& request) const;
    DatabaseHealthResult health(const DatabaseRequest& request) const;
};

} // namespace mosaicraft
