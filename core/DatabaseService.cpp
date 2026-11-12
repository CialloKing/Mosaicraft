#include "DatabaseService.h"

#include "Database.h"
#include "FeatureIO.h"
#include "FeaturePack.h"
#include "LibraryVisuals.h"
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
    Database db(resolveDbPathForService(request.dbPath), Database::OpenMode::Existing);
    if (!db.isOpen())
    {
        out.status = ServiceResult::failure(2, db.error(), db.errorCode(), "dbPath");
        return out;
    }

    const auto all = db.averageColors();
    out.stats.total = static_cast<int>(all.size());
    out.stats.empty = out.stats.total == 0;
    if (out.stats.empty)
    {
        out.status = ServiceResult::success("database is empty");
        return out;
    }

    out.stats.featureWidth = db.getMeta("feature_w");
    out.stats.featureHeight = db.getMeta("feature_h");
    out.stats.gridDim = 8 * 8 * 3;

    double sumL = 0.0, sumA = 0.0, sumB = 0.0;
    out.stats.minL = out.stats.minA = out.stats.minB = 255.0;
    out.stats.maxL = out.stats.maxA = out.stats.maxB = 0.0;

    out.stats.colors = summarizeColors(all);
    for (const auto &[l, a, b] : all)
    {
        out.stats.minL = std::min(out.stats.minL, l);
        out.stats.maxL = std::max(out.stats.maxL, l);
        out.stats.minA = std::min(out.stats.minA, a);
        out.stats.maxA = std::max(out.stats.maxA, a);
        out.stats.minB = std::min(out.stats.minB, b);
        out.stats.maxB = std::max(out.stats.maxB, b);
        sumL += l;
        sumA += a;
        sumB += b;
        if (l < 50)
        {
            ++out.stats.dark;
        }
        else if (l < 150)
        {
            ++out.stats.mid;
        }
        else
        {
            ++out.stats.bright;
        }
    }

    out.stats.avgL = sumL / out.stats.total;
    out.stats.avgA = sumA / out.stats.total;
    out.stats.avgB = sumB / out.stats.total;

    for (int i = 0; i < 8; ++i)
    {
        out.stats.lHistogram.push_back({i * 32, i == 7 ? 255 : (i + 1) * 32 - 1,
                                        static_cast<int>(out.stats.colors.lightness[i])});
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
    Database db(resolveDbPathForService(request.dbPath), Database::OpenMode::Existing);
    if (!db.isOpen())
    {
        out.status = ServiceResult::failure(2, db.error(), db.errorCode(), "dbPath");
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
        std::error_code error;
        const auto status = std::filesystem::status(u8path(r.filePath), error);
        if (status.type() == std::filesystem::file_type::not_found)
        {
            ++out.health.missingFiles;
        }
        else if (error || !std::filesystem::is_regular_file(status))
        {
            ++out.health.unreadableFiles;
        }
        std::array<uint8_t, 256> tiny{};
        std::array<float, 256> lbp{};
        if (r.gridLab.size() != 192 ||
            !readFeature(r.tinyPath, FeatureKind::Tiny, tiny.data()) ||
            !readFeature(r.lbpPath, FeatureKind::Lbp, lbp.data()))
        {
            ++out.health.invalidFeatures;
        }
        if (request.deep && std::filesystem::is_regular_file(status))
        {
            const auto image = imreadUnicode(r.filePath);
            uint64_t hash = 14695981039346656037ULL;
            if (!image.empty())
            {
                for (size_t i = 0; i < image.total() * image.elemSize(); ++i)
                {
                    hash = (hash ^ image.data[i]) * 1099511628211ULL;
                }
            }
            if (image.empty() || std::to_string(hash) != r.fileHash)
            {
                ++out.health.corruptImages;
            }
        }
        if (r.avgL < 50) out.health.dark++;
        else if (r.avgL < 150) out.health.mid++;
        else out.health.bright++;
        out.health.minA = std::min(out.health.minA, r.avgA);
        out.health.maxA = std::max(out.health.maxA, r.avgA);
        out.health.minB = std::min(out.health.minB, r.avgB);
        out.health.maxB = std::max(out.health.maxB, r.avgB);
    }

    if (out.health.missingFiles > 0)
    {
        out.health.warnings.push_back("Missing image files: " + std::to_string(out.health.missingFiles));
        out.health.recommendations.push_back("Restore missing image files or run library purge");
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

    auto used = db.topUsedImages(0, true);
    out.health.usedCount = static_cast<int>(used.size());
    out.health.unusedCount = out.health.total - out.health.usedCount;
    out.health.usedPct = 100.0 * out.health.usedCount / out.health.total;
    out.health.unusedPct = 100.0 * out.health.unusedCount / out.health.total;
    if (out.health.unusedCount > out.health.total / 2)
        out.health.warnings.push_back(">50% images absent from recorded usage (historical usage may be incomplete)");

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
        out.health.recommendations.push_back("Use library usage to inspect recorded targets and tile counts");

    const int failures = out.health.missingFiles + out.health.unreadableFiles + out.health.invalidFeatures + out.health.corruptImages;
    out.status = failures ? ServiceResult::failure(1, "library integrity check failed", "library_integrity_failed")
                          : ServiceResult::success("database health generated");
    return out;
}

DatabaseUsageResult DatabaseService::usage(const DatabaseUsageRequest& request) const
{
    DatabaseUsageResult out;
    Database db(resolveDbPathForService(request.dbPath), Database::OpenMode::Existing);
    if (!db.isOpen())
    {
        out.status = ServiceResult::failure(2, db.error(), db.errorCode(), "dbPath");
        return out;
    }

    auto all = db.allRecords();
    out.usage.total = static_cast<int>(all.size());
    auto top = db.topUsedImages(request.limit, request.sortByTiles);
    out.usage.empty = top.empty();
    for (const auto& item : top)
    {
        out.usage.top.push_back({std::get<0>(item), std::get<1>(item), std::get<2>(item)});
    }

    if (request.showUnused)
    {
        auto used = db.topUsedImages(0, true);
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

    Database db(resolveDbPathForService(request.dbPath), Database::OpenMode::Existing);
    if (!db.isOpen())
    {
        out.status = ServiceResult::failure(2, db.error(), db.errorCode(), "dbPath");
        return out;
    }

    auto allRecs = db.allRecords();
    std::unordered_map<int, std::string> pathMap;
    for (const auto& rec : allRecs)
    {
        pathMap[rec.id] = rec.filePath;
    }

    if (request.selection != "used" && request.selection != "unused" && request.selection != "all")
    {
        out.status = ServiceResult::failure(1, "selection must be used, unused or all");
        return out;
    }
    auto allUsed = db.topUsedImages(0, true);
    std::unordered_set<int> usedIds;
    for (const auto &[id, runs, tiles] : allUsed)
    {
        usedIds.insert(id);
    }
    if (request.selection != "used")
    {
        allUsed.clear();
        for (const auto &rec : allRecs)
        {
            if (request.selection == "all" || !usedIds.count(rec.id))
            {
                allUsed.emplace_back(rec.id, 0, 0);
            }
        }
    }
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

    Database db(resolveDbPathForService(request.dbPath), Database::OpenMode::Build);
    if (!db.isOpen())
    {
        out.status = ServiceResult::failure(2, db.error(), db.errorCode(), "dbPath");
        return out;
    }

    auto all = db.allRecords();
    out.purge.total = static_cast<int>(all.size());

    std::vector<OrphanRecord> orphans;
    for (const auto& rec : all)
    {
        std::error_code error;
        const auto status = std::filesystem::status(u8path(rec.filePath), error);
        if (!rec.filePath.empty() && status.type() == std::filesystem::file_type::not_found)
        {
            orphans.push_back({rec.id, rec.filePath, rec.tinyPath, rec.lbpPath});
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

    out.purge.recommendations.push_back("Only confirmed missing image records are eligible for removal.");

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

    std::vector<int> ids;
    for (const auto &orphan : orphans)
    {
        ids.push_back(orphan.id);
    }
    if (!db.removeImages(ids))
    {
        out.status = ServiceResult::failure(2, "purge transaction failed; files preserved");
        return out;
    }
    out.purge.removedCount = static_cast<int>(ids.size());
    std::unordered_set<std::string> referenced;
    for (const auto &record : db.allRecords())
    {
        referenced.insert(record.tinyPath);
        referenced.insert(record.lbpPath);
    }
    for (const auto &orphan : orphans)
    {
        for (const auto &path : {orphan.tinyPath, orphan.lbpPath})
        {
            if (!path.empty() && !referenced.count(path))
            {
                std::error_code error;
                std::filesystem::remove(u8path(path), error);
            }
        }
    }
    // 缓存的内容指纹已随记录变更失效；仅移除无引用的旧代，不删除有效源特征。
    FeaturePack::removeObsolete(pathToUtf8(u8path(db.path()).parent_path() / "features"));
    if (out.purge.failedCount > 0)
    {
        out.status = ServiceResult::failure(1, "purge completed with errors");
        return out;
    }

    out.status = ServiceResult::success("orphan records purged");
    return out;
}

} // namespace mosaicraft
