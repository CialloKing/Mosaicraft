#include "LibraryVisuals.h"
#include "ColorOrder.h"
#include "Database.h"
#include "FeatureIndex.h"
#include "MatchInspection.h"
#include "UnicodeIO.h"
#include <numeric>

namespace mosaicraft
{
ServiceResult measureMosaicCoverage(const MosaicRequest &request, MosaicCoverage &coverage)
{
    cv::Mat target = imreadUnicode(request.inputPath, cv::IMREAD_COLOR);
    if (target.empty())
    {
        return ServiceResult::failure(1, "cannot read target image", "input_unreadable", "inputPath");
    }
    TargetGeometry geometry;
    std::string error;
    const auto &cfg = request.config;
    if (!resolveTargetGeometry(target.cols, target.rows, cfg, geometry, error))
    {
        return ServiceResult::failure(1, error, "invalid_argument", "inputPath");
    }
    std::vector<std::array<double, 3>> colors;
    {
        Database db(resolveDbPathForService(request.dbPath), Database::OpenMode::ReadOnly);
        if (!db.isOpen())
        {
            return ServiceResult::failure(2, db.error(), db.errorCode(), "dbPath");
        }
        colors = db.averageColors();
    }
    coverage.library = summarizeColors(colors);
    std::vector<double> lightness;
    lightness.reserve(colors.size());
    for (const auto &color : colors)
    {
        if (std::isfinite(color[0]))
        {
            lightness.push_back(color[0]);
        }
    }
    std::sort(lightness.begin(), lightness.end());
    resizeTarget(target, geometry, cfg);
    padTarget(target, cfg, geometry.columns, geometry.rows);
    coverage.columns = geometry.columns;
    coverage.rows = geometry.rows;
    coverage.mapColumns = std::min(256, coverage.columns);
    coverage.mapRows = std::min(256, coverage.rows);
    coverage.threshold = std::min(10, cfg.candidates);
    coverage.minimumCandidates.assign(size_t(coverage.mapColumns) * coverage.mapRows, std::numeric_limits<int>::max());
    // 按匹配行转换 LAB，避免为整张工作图再分配一份 LAB 图像。
    cv::Mat lab;
    for (int row = 0; row < coverage.rows; ++row)
    {
        cv::cvtColor(target.rowRange(row * cfg.tileH, (row + 1) * cfg.tileH), lab, cv::COLOR_BGR2Lab);
        for (int col = 0; col < coverage.columns; ++col)
        {
            const auto mean = cv::mean(lab.colRange(col * cfg.tileW, (col + 1) * cfg.tileW));
            coverage.target.add(mean[0], mean[1], mean[2]);
            const auto low = std::lower_bound(lightness.begin(), lightness.end(), mean[0] - cfg.lRange);
            const auto high = std::upper_bound(low, lightness.end(), mean[0] + cfg.lRange);
            const int count = static_cast<int>(high - low);
            coverage.noCandidates += count == 0;
            coverage.fewCandidates += count > 0 && count < coverage.threshold;
            const auto slot = int64_t(row) * coverage.mapRows / coverage.rows * coverage.mapColumns +
                              int64_t(col) * coverage.mapColumns / coverage.columns;
            coverage.minimumCandidates[slot] = std::min(coverage.minimumCandidates[slot], count);
        }
    }
    return ServiceResult::success();
}

ServiceResult browseLibrary(const std::string &dbPath, const std::string &sort, int anchor, LibraryBrowser &browser)
{
    Database db(resolveDbPathForService(dbPath), Database::OpenMode::ReadOnly);
    if (!db.isOpen())
    {
        return ServiceResult::failure(2, db.error(), db.errorCode(), "dbPath");
    }
    const auto records = db.allRecords();
    int width = 0, height = 0;
    readLibraryDimensions(db, width, height);
    browser.version = previewLibraryVersion(records, width, height);
    std::vector<int> order(records.size());
    std::iota(order.begin(), order.end(), 0);
    if (sort == "similarity")
    {
        const auto found = std::find_if(records.begin(), records.end(), [anchor](const ImageRecord &item)
        {
            return item.id == anchor;
        });
        if (found == records.end())
        {
            return ServiceResult::failure(1, "select a library image as the reference", "invalid_argument", "anchor");
        }
        const auto featureDir = pathToUtf8(u8path(found->tinyPath).parent_path());
        FeatureIndex index;
        if (!index.load(featureDir + "/lib.ann196", FeatureIndex::kDimension, records) && !index.build(records))
        {
            return ServiceResult::failure(2, "cannot prepare similarity candidates", "library_unavailable");
        }
        FeaturePack::Reader reader(featureDir, records, 32ULL * 1024 * 1024);
        const auto row = reader.get(static_cast<int>(found - records.begin()));
        std::vector<float> query;
        buildTileVector(found->avgL, found->avgA, found->avgB, found->gridLab, row.tiny, found->edgeDensity, row.lbp, query);
        order.clear();
        for (int id : index.query(query.data(), std::min<int>(200, static_cast<int>(records.size()))))
        {
            const int position = index.idToAllRecordsIndex(id);
            if (position >= 0)
            {
                order.push_back(position);
            }
        }
        for (int position : order)
        {
            const auto &record = records[position];
            browser.distances[record.id] = inspectionScore(featureDistances(found->avgL, found->avgA, found->avgB,
                found->gridLab, found->edgeDensity, row, record, reader.get(position)), {});
        }
        std::sort(order.begin(), order.end(), [&](int left, int right)
        {
            return std::make_pair(browser.distances.at(records[left].id), records[left].id) <
                   std::make_pair(browser.distances.at(records[right].id), records[right].id);
        });
    }
    else if (sort == "usage")
    {
        for (const auto &[id, runs, tiles] : db.topUsedImages(std::max(1, db.totalCount())))
        {
            browser.tiles.emplace(id, tiles);
        }
        const auto count = [&](int id)
        {
            const auto found = browser.tiles.find(id);
            return found == browser.tiles.end() ? 0 : found->second;
        };
        std::sort(order.begin(), order.end(), [&](int left, int right)
        {
            return std::make_pair(-count(records[left].id), records[left].id) <
                   std::make_pair(-count(records[right].id), records[right].id);
        });
    }
    else
    {
        const auto cached = db.imageColorOrders();
        std::vector<int64_t> keys;
        keys.reserve(records.size());
        for (const auto &record : records)
        {
            const auto found = cached.find(record.id);
            keys.push_back(found != cached.end() && found->second.fileHash == record.fileHash ? found->second.key
                           : meanLabColorOrderKey(record.avgL, record.avgA, record.avgB));
        }
        std::sort(order.begin(), order.end(), [&](int left, int right)
        {
            return std::make_pair(keys[left], records[left].id) < std::make_pair(keys[right], records[right].id);
        });
    }
    browser.ordered.reserve(order.size());
    for (int position : order)
    {
        const auto &record = records[position];
        browser.positions.emplace(record.id, browser.ordered.size());
        browser.ordered.push_back({record.id, position, pathToUtf8(u8path(record.filePath).filename()), record.filePath, record.fileHash});
    }
    return ServiceResult::success();
}
} // namespace mosaicraft
