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

struct DatabaseUsageRequest
{
    std::string dbPath = "library/mosaicraft.db";
    int limit = 50;
    bool showUnused = false;
};

struct DatabaseUsageExportRequest
{
    std::string dbPath = "library/mosaicraft.db";
    std::string outputDir;
    bool confirm = false;
};

struct DatabasePurgeRequest
{
    std::string dbPath = "library/mosaicraft.db";
    bool dryRun = true;
    bool confirm = false;
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

struct UsageItem
{
    int id = 0;
    int runs = 0;
    int tiles = 0;
};

struct UnusedItem
{
    int id = 0;
    std::string filePath;
};

struct DatabaseUsage
{
    bool empty = true;
    int total = 0;
    std::vector<UsageItem> top;
    int unusedCount = 0;
    std::vector<UnusedItem> unusedPreview;
};

struct ExportedUsageItem
{
    int id = 0;
    int runs = 0;
    int tiles = 0;
    std::string sourcePath;
    std::string outputPath;
};

struct DatabaseUsageExport
{
    std::string outputDir;
    int usedCount = 0;
    int exportedCount = 0;
    int skippedCount = 0;
    int failedCount = 0;
    std::vector<ExportedUsageItem> exportedPreview;
    std::vector<std::string> errors;
};

struct OrphanRecord
{
    int id = 0;
    std::string filePath;
    std::string tinyPath;
    std::string histPath;
};

struct DatabasePurge
{
    bool dryRun = true;
    int total = 0;
    int orphanCount = 0;
    int removedCount = 0;
    int failedCount = 0;
    std::vector<OrphanRecord> orphanPreview;
    std::vector<std::string> errors;
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

struct DatabaseUsageResult
{
    ServiceResult status;
    DatabaseUsage usage;
};

struct DatabaseUsageExportResult
{
    ServiceResult status;
    DatabaseUsageExport exportInfo;
};

struct DatabasePurgeResult
{
    ServiceResult status;
    DatabasePurge purge;
};

class DatabaseService
{
public:
    DatabaseStatsResult stats(const DatabaseRequest& request) const;
    DatabaseHealthResult health(const DatabaseRequest& request) const;
    DatabaseUsageResult usage(const DatabaseUsageRequest& request) const;
    DatabaseUsageExportResult exportUsage(const DatabaseUsageExportRequest& request) const;
    DatabasePurgeResult purge(const DatabasePurgeRequest& request) const;
};

} // namespace mosaicraft
