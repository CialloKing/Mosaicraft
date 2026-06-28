#include "DatabaseService.h"

#include "Database.h"
#include "MosaicService.h"
#include "UnicodeIO.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <tuple>

namespace mosaicraft
{

DatabaseStatsResult DatabaseService::stats(const DatabaseRequest& request) const
{
    DatabaseStatsResult out;
    Database db(resolveDbPathForService(request.dbPath));
    if (!db.isOpen())
    {
        out.status = ServiceResult::failure(2, "cannot open database: " + request.dbPath);
        return out;
    }

    auto all = db.allRecords();
    out.stats.total = static_cast<int>(all.size());
    out.stats.empty = out.stats.total == 0;
    if (out.stats.empty)
    {
        out.status = ServiceResult::success("database is empty");
        return out;
    }

    out.stats.featureWidth = db.getMeta("feature_w");
    out.stats.featureHeight = db.getMeta("feature_h");
    out.stats.gridDim = static_cast<int>(all[0].grid4x4.size());

    double sumL = 0.0, sumA = 0.0, sumB = 0.0;
    out.stats.minL = out.stats.minA = out.stats.minB = 255.0;
    out.stats.maxL = out.stats.maxA = out.stats.maxB = 0.0;

    int hist[8] = {0};
    for (const auto& r : all)
    {
        out.stats.minL = std::min(out.stats.minL, r.avgL);
        out.stats.maxL = std::max(out.stats.maxL, r.avgL);
        out.stats.minA = std::min(out.stats.minA, r.avgA);
        out.stats.maxA = std::max(out.stats.maxA, r.avgA);
        out.stats.minB = std::min(out.stats.minB, r.avgB);
        out.stats.maxB = std::max(out.stats.maxB, r.avgB);
        sumL += r.avgL;
        sumA += r.avgA;
        sumB += r.avgB;

        if (r.avgL < 30) out.stats.dark++;
        else if (r.avgL < 70) out.stats.mid++;
        else out.stats.bright++;

        int b = static_cast<int>(r.avgL) / 32;
        if (b < 0) b = 0;
        if (b > 7) b = 7;
        hist[b]++;
    }

    out.stats.avgL = sumL / out.stats.total;
    out.stats.avgA = sumA / out.stats.total;
    out.stats.avgB = sumB / out.stats.total;

    for (int i = 0; i < 8; ++i)
    {
        out.stats.lHistogram.push_back({i * 32, i == 7 ? 255 : (i + 1) * 32 - 1, hist[i]});
    }

    if (out.stats.dark + out.stats.mid < out.stats.total / 100) out.stats.coverageGaps.push_back("dark");
    if (out.stats.minA > 110.0) out.stats.coverageGaps.push_back("green-biased");
    if (out.stats.maxA < 145.0) out.stats.coverageGaps.push_back("red-deficient");
    if (out.stats.minB > 110.0) out.stats.coverageGaps.push_back("blue-biased");
    if (out.stats.maxB < 145.0) out.stats.coverageGaps.push_back("yellow-deficient");

    out.status = ServiceResult::success("database stats generated");
    return out;
}

DatabaseHealthResult DatabaseService::health(const DatabaseRequest& request) const
{
    DatabaseHealthResult out;
    Database db(resolveDbPathForService(request.dbPath));
    if (!db.isOpen())
    {
        out.status = ServiceResult::failure(2, "cannot open database: " + request.dbPath);
        return out;
    }

    auto all = db.allRecords();
    out.health.total = static_cast<int>(all.size());
    out.health.empty = out.health.total == 0;
    if (out.health.empty)
    {
        out.status = ServiceResult::success("database is empty");
        return out;
    }

    out.health.minA = out.health.minB = 255.0;
    out.health.maxA = out.health.maxB = 0.0;
    for (const auto& r : all)
    {
        if (r.avgL < 50) out.health.dark++;
        else if (r.avgL < 150) out.health.mid++;
        else out.health.bright++;
        out.health.minA = std::min(out.health.minA, r.avgA);
        out.health.maxA = std::max(out.health.maxA, r.avgA);
        out.health.minB = std::min(out.health.minB, r.avgB);
        out.health.maxB = std::max(out.health.maxB, r.avgB);
    }

    out.health.darkPct = 100.0 * out.health.dark / out.health.total;
    out.health.midPct = 100.0 * out.health.mid / out.health.total;
    out.health.brightPct = 100.0 * out.health.bright / out.health.total;

    if (out.health.darkPct < 5.0) out.health.warnings.push_back("Dark images severely underrepresented");
    if (out.health.midPct < 15.0) out.health.warnings.push_back("Mid-tone images underrepresented");
    if (out.health.minA > 115) out.health.warnings.push_back("Lacking green-toned images");
    if (out.health.maxA < 140) out.health.warnings.push_back("Lacking red/warm-toned images");
    if (out.health.minB > 115) out.health.warnings.push_back("Lacking blue/cool-toned images");
    if (out.health.maxB < 140) out.health.warnings.push_back("Lacking yellow-toned images");

    auto used = db.topUsedImages(999999);
    out.health.usedCount = static_cast<int>(used.size());
    out.health.unusedCount = out.health.total - out.health.usedCount;
    out.health.usedPct = 100.0 * out.health.usedCount / out.health.total;
    out.health.unusedPct = 100.0 * out.health.unusedCount / out.health.total;
    if (out.health.unusedCount > out.health.total / 2)
        out.health.warnings.push_back(">50% images never used - consider pruning");

    if (!used.empty())
    {
        int64_t totalTiles = 0;
        for (const auto& item : used) totalTiles += std::get<2>(item);
        out.health.topHotspotCount = std::max(1, out.health.usedCount / 100);
        int64_t topTiles = 0;
        for (int i = 0; i < out.health.topHotspotCount && i < out.health.usedCount; ++i)
            topTiles += std::get<2>(used[i]);
        if (totalTiles > 0)
            out.health.topHotspotTilePct = 100.0 * topTiles / totalTiles;
        if (out.health.topHotspotTilePct > 20.0)
            out.health.warnings.push_back("Small subset dominates matching");
    }

    if (out.health.darkPct < 5.0) out.health.recommendations.push_back("Add night / indoor / low-light photos");
    if (out.health.midPct < 15.0) out.health.recommendations.push_back("Add overcast / shadow / twilight scenes");
    if (out.health.minA > 115 || out.health.maxA < 140)
        out.health.recommendations.push_back("Diversify green-red color range");
    if (out.health.minB > 115 || out.health.maxB < 140)
        out.health.recommendations.push_back("Diversify blue-yellow color range");
    if (out.health.unusedCount > out.health.total / 2)
        out.health.recommendations.push_back("Run db-usage to identify dead weight");

    out.status = ServiceResult::success("database health generated");
    return out;
}

DatabaseUsageResult DatabaseService::usage(const DatabaseUsageRequest& request) const
{
    DatabaseUsageResult out;
    Database db(resolveDbPathForService(request.dbPath));
    if (!db.isOpen())
    {
        out.status = ServiceResult::failure(2, "cannot open database: " + request.dbPath);
        return out;
    }

    auto all = db.allRecords();
    out.usage.total = static_cast<int>(all.size());
    auto top = db.topUsedImages(std::max(1, request.limit));
    out.usage.empty = top.empty();
    for (const auto& item : top)
    {
        out.usage.top.push_back({std::get<0>(item), std::get<1>(item), std::get<2>(item)});
    }

    if (request.showUnused)
    {
        auto used = db.topUsedImages(999999);
        std::unordered_set<int> usedIds;
        for (const auto& item : used) usedIds.insert(std::get<0>(item));

        for (const auto& rec : all)
        {
            if (usedIds.count(rec.id)) continue;
            out.usage.unusedCount++;
            if (out.usage.unusedPreview.size() < 50)
            {
                out.usage.unusedPreview.push_back({rec.id, rec.filePath});
            }
        }
    }

    out.status = ServiceResult::success(out.usage.empty
        ? "no usage data yet"
        : "database usage generated");
    return out;
}

DatabaseUsageExportResult DatabaseService::exportUsage(const DatabaseUsageExportRequest& request) const
{
    DatabaseUsageExportResult out;
    out.exportInfo.outputDir = request.outputDir;
    if (request.outputDir.empty())
    {
        out.status = ServiceResult::failure(1, "outputDir is required");
        return out;
    }
    if (!request.confirm)
    {
        out.status = ServiceResult::failure(1, "confirm is required to export used images");
        return out;
    }

    Database db(resolveDbPathForService(request.dbPath));
    if (!db.isOpen())
    {
        out.status = ServiceResult::failure(2, "cannot open database: " + request.dbPath);
        return out;
    }

    auto allRecs = db.allRecords();
    std::unordered_map<int, std::string> pathMap;
    for (const auto& rec : allRecs)
    {
        pathMap[rec.id] = rec.filePath;
    }

    auto allUsed = db.topUsedImages(999999);
    std::sort(allUsed.begin(), allUsed.end(),
        [](const auto& a, const auto& b) {
            return std::get<2>(a) > std::get<2>(b);
        });
    out.exportInfo.usedCount = static_cast<int>(allUsed.size());

    try
    {
        std::filesystem::create_directories(u8path(request.outputDir));
    }
    catch (const std::exception& e)
    {
        out.status = ServiceResult::failure(1, "cannot create output directory: " + std::string(e.what()));
        return out;
    }

    for (size_t i = 0; i < allUsed.size(); ++i)
    {
        auto [id, runs, tiles] = allUsed[i];
        auto it = pathMap.find(id);
        if (it == pathMap.end())
        {
            out.exportInfo.skippedCount++;
            out.exportInfo.errors.push_back("missing database path for id=" + std::to_string(id));
            continue;
        }

        const std::string& srcPath = it->second;
        if (!std::filesystem::exists(u8path(srcPath)))
        {
            out.exportInfo.skippedCount++;
            out.exportInfo.errors.push_back("source file missing for id=" + std::to_string(id) + ": " + srcPath);
            continue;
        }

        auto dotPos = srcPath.find_last_of('.');
        std::string ext = (dotPos != std::string::npos) ? srcPath.substr(dotPos) : "";
        char fileName[512];
        std::snprintf(fileName, sizeof(fileName), "rank%04zu_%druns_%dtiles_id%d%s",
                      i + 1, runs, tiles, id, ext.c_str());

        auto dstPath = u8path(request.outputDir) / u8path(fileName);
        try
        {
            std::filesystem::copy_file(u8path(srcPath), dstPath,
                std::filesystem::copy_options::overwrite_existing);
            out.exportInfo.exportedCount++;
            if (out.exportInfo.exportedPreview.size() < 50)
            {
                out.exportInfo.exportedPreview.push_back({
                    id,
                    runs,
                    tiles,
                    srcPath,
                    pathToUtf8(dstPath)
                });
            }
        }
        catch (const std::exception& e)
        {
            out.exportInfo.failedCount++;
            out.exportInfo.errors.push_back("failed to export id=" + std::to_string(id) + ": " + e.what());
        }
    }

    if (out.exportInfo.failedCount > 0)
    {
        out.status = ServiceResult::failure(1, "usage export completed with errors");
        return out;
    }

    out.status = ServiceResult::success("used images exported");
    return out;
}

DatabasePurgeResult DatabaseService::purge(const DatabasePurgeRequest& request) const
{
    DatabasePurgeResult out;
    out.purge.dryRun = request.dryRun;

    Database db(resolveDbPathForService(request.dbPath));
    if (!db.isOpen())
    {
        out.status = ServiceResult::failure(2, "cannot open database: " + request.dbPath);
        return out;
    }

    auto all = db.allRecords();
    out.purge.total = static_cast<int>(all.size());

    std::vector<OrphanRecord> orphans;
    for (const auto& rec : all)
    {
        if (!rec.filePath.empty() && !std::filesystem::exists(u8path(rec.filePath)))
        {
            orphans.push_back({rec.id, rec.filePath, rec.tinyPath, rec.histPath});
        }
    }

    out.purge.orphanCount = static_cast<int>(orphans.size());
    for (const auto& orphan : orphans)
    {
        if (out.purge.orphanPreview.size() >= 50) break;
        out.purge.orphanPreview.push_back(orphan);
    }

    if (orphans.empty())
    {
        out.status = ServiceResult::success("no orphan records found");
        return out;
    }

    out.purge.recommendations.push_back("delete normalized/features/*.bin and lib.ann, then re-run mosaic to rebuild caches");

    if (request.dryRun)
    {
        out.status = ServiceResult::success("orphan records found");
        return out;
    }

    if (!request.confirm)
    {
        out.status = ServiceResult::failure(1, "confirm is required to purge orphan records");
        return out;
    }

    for (const auto& orphan : orphans)
    {
        try
        {
            if (!orphan.tinyPath.empty() && std::filesystem::exists(u8path(orphan.tinyPath)))
            {
                std::filesystem::remove(u8path(orphan.tinyPath));
            }
            if (!orphan.histPath.empty() && std::filesystem::exists(u8path(orphan.histPath)))
            {
                std::filesystem::remove(u8path(orphan.histPath));
            }

            if (db.removeImage(orphan.id))
            {
                out.purge.removedCount++;
            }
            else
            {
                out.purge.failedCount++;
                out.purge.errors.push_back("failed to remove database record id=" + std::to_string(orphan.id));
            }
        }
        catch (const std::exception& e)
        {
            out.purge.failedCount++;
            out.purge.errors.push_back("failed to purge id=" + std::to_string(orphan.id) + ": " + e.what());
        }
    }

    if (out.purge.failedCount > 0)
    {
        out.status = ServiceResult::failure(1, "purge completed with errors");
        return out;
    }

    out.status = ServiceResult::success("orphan records purged");
    return out;
}

} // namespace mosaicraft
