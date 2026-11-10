#include "../compute/CudaBackend.h"
#include "../compute/CompatibleScoring.h"
#include "../core/MatchingScore.h"
#include "../core/AnalysisTiles.h"
#include "../core/BrightnessAdjustment.h"
#include "../core/CandidateFeatures.h"
#include "../core/FeatureExtractor.h"
#include "../core/FeatureUtils.h"
#include "../core/FeatureIO.h"
#include "../core/LibraryPaths.h"
#include "../core/GridDuplicateCache.h"
#include "../core/ImageCache.h"
#include "../core/ImageNormalizer.h"
#include "../core/MatchingAccess.h"
#include "../core/MosaicOutput.h"
#include "../core/PngStreamWriter.h"
#include "../core/PreviewAccumulator.h"
#include "../core/TileCompositor.h"
#include "../core/WorkerPool.h"
#include <deque>
#include <limits>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../core/BuildService.h"
#include "../core/ApiHandlers.h"
#include "../core/DatabaseService.h"
#include "../core/InspectService.h"
#include "../core/MosaicService.h"
#include "../core/UnicodeIO.h"
#include "../core/ColorOrder.h"
#include "../core/LibraryColorOrder.h"
#include "../core/LibraryAccess.h"
#include "../core/json.hpp"
#include <sqlite3.h>
#include <cmath>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cctype>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include <unordered_map>

using namespace mosaicraft;

namespace
{

namespace fs = std::filesystem;

class TempWorkspace
{
public:
    TempWorkspace()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("mosaicraft_regression_" + std::to_string(stamp));
        std::error_code ec;
        fs::create_directories(inputDir(), ec);
        fs::create_directories(outputDir(), ec);
        fs::create_directories(emptyInputDir(), ec);
    }

    ~TempWorkspace()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path inputDir() const { return root / "input"; }
    fs::path emptyInputDir() const { return root / "empty_input"; }
    fs::path libraryDir() const { return root / "library"; }
    fs::path outputDir() const { return root / "output"; }
    fs::path dbPath() const { return libraryDir() / "mosaicraft.db"; }
    fs::path targetPath() const { return root / "target.png"; }
    fs::path mosaicPath() const { return outputDir() / "mosaic.png"; }

    fs::path root;
};

std::string utf8(const fs::path& path)
{
    return pathToUtf8(path);
}

void requireWriteImage(const fs::path& path, const cv::Mat& image)
{
    REQUIRE_FALSE(image.empty());
    REQUIRE(imwriteUnicode(utf8(path), image, {cv::IMWRITE_PNG_COMPRESSION, 1}));
}

void writeLibraryImages(const fs::path& inputDir)
{
    const std::vector<cv::Scalar> colors = {
        {20, 20, 20},
        {230, 230, 230},
        {40, 40, 220},
        {40, 190, 40},
        {220, 70, 40},
        {30, 210, 210},
        {210, 40, 210},
        {210, 160, 30},
        {120, 60, 200},
        {80, 180, 150},
    };

    for (size_t i = 0; i < colors.size(); ++i)
    {
        cv::Mat image(128, 72, CV_8UC3, colors[i]);
        const int shift = static_cast<int>(i);
        cv::rectangle(image,
                      cv::Rect(8 + (shift % 5), 12 + (shift % 7), 28, 42),
                      cv::Scalar(255 - colors[i][0], 255 - colors[i][1], 255 - colors[i][2]),
                      cv::FILLED);
        cv::circle(image,
                   cv::Point(36, 76),
                   10 + static_cast<int>(i % 6),
                   cv::Scalar((30 * shift) % 255, (80 + 17 * shift) % 255, (140 + 29 * shift) % 255),
                   cv::FILLED);
        cv::line(image,
                 cv::Point(0, 20 + static_cast<int>(i * 7 % 80)),
                 cv::Point(71, 110 - static_cast<int>(i * 5 % 70)),
                 cv::Scalar(255, 255 - static_cast<int>(i * 20 % 255), static_cast<int>(i * 25 % 255)),
                 2);

        char name[32];
        std::snprintf(name, sizeof(name), "tile_%02zu.png", i);
        requireWriteImage(inputDir / name, image);
    }
}

void writeTargetImage(const fs::path& path)
{
    cv::Mat target(64, 36, CV_8UC3, cv::Scalar(30, 30, 30));
    target(cv::Rect(0, 0, 18, 32)).setTo(cv::Scalar(40, 40, 220));
    target(cv::Rect(18, 0, 18, 32)).setTo(cv::Scalar(40, 190, 40));
    target(cv::Rect(0, 32, 18, 32)).setTo(cv::Scalar(220, 70, 40));
    target(cv::Rect(18, 32, 18, 32)).setTo(cv::Scalar(220, 220, 220));
    cv::line(target, cv::Point(0, 0), cv::Point(35, 63), cv::Scalar(255, 255, 255), 1);
    cv::line(target, cv::Point(35, 0), cv::Point(0, 63), cv::Scalar(0, 0, 0), 1);
    requireWriteImage(path, target);
}

BuildRequest makeBuildRequest(const TempWorkspace& workspace)
{
    BuildRequest request;
    request.inputDir = utf8(workspace.inputDir());
    request.outputDir = utf8(workspace.libraryDir());
    request.dbPath = utf8(workspace.dbPath());
    request.threads = 1;
    request.forceMode = true;
    request.allowPrompt = false;
    request.normalizeWidth = 36;
    request.normalizeHeight = 64;
    return request;
}

MosaicRequest makeMosaicRequest(const TempWorkspace& workspace)
{
    MosaicRequest request;
    request.inputPath = utf8(workspace.targetPath());
    request.dbPath = utf8(workspace.dbPath());
    request.outputPath = utf8(workspace.mosaicPath());
    request.config.useGpu = false;
    request.config.outputFormat = "png";
    request.config.formatExplicit = true;
    request.config.pngCompressionLevel = 1;
    request.config.outW = 18;
    request.config.outH = 32;
    request.config.tileW = 9;
    request.config.tileH = 16;
    request.config.nativeTileW = 18;
    request.config.nativeTileH = 32;
    request.config.candidates = 4;
    request.config.topNRandom = 1;
    return request;
}

int fixtureSql(const fs::path &path, const std::string &sql)
{
    sqlite3 *raw = nullptr;
    if (sqlite3_open(utf8(path).c_str(), &raw) != SQLITE_OK)
    {
        sqlite3_close(raw);
        return SQLITE_CANTOPEN;
    }
    const int result = sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(raw);
    return result;
}

} // namespace

TEST_CASE("new libraries reject incompatible structures and remain portable")
{
    TempWorkspace workspace;
    writeLibraryImages(workspace.inputDir());
    writeTargetImage(workspace.targetPath());
    auto build = makeBuildRequest(workspace);
    REQUIRE(BuildService().run(build).ok);
    bool incompatible = false;
    SUBCASE("unversioned old library")
    {
        REQUIRE(fixtureSql(workspace.dbPath(), "PRAGMA user_version=0") == SQLITE_OK);
        incompatible = true;
    }
    SUBCASE("future library")
    {
        REQUIRE(fixtureSql(workspace.dbPath(), "PRAGMA user_version=99") == SQLITE_OK);
        incompatible = true;
    }
    SUBCASE("missing required table")
    {
        REQUIRE(fixtureSql(workspace.dbPath(), "DROP TABLE target_runs") == SQLITE_OK);
        incompatible = true;
    }
    if (incompatible)
    {
        for (auto mode : {Database::OpenMode::Existing, Database::OpenMode::Build})
        {
            Database blocked(build.dbPath, mode);
            CHECK_FALSE(blocked.isOpen());
            CHECK(std::string(blocked.errorCode()) == "library_incompatible");
        }
        build.forceMode = true;
        CHECK_FALSE(BuildService().run(build).ok);
        CHECK_FALSE(fs::exists(workspace.libraryDir() / ".mosaicraft-backups"));
        return;
    }
    const auto moved = workspace.root / fs::u8path("移动 后图库");
    fs::rename(workspace.libraryDir(), moved);
    const auto previous = fs::current_path();
    struct RestoreCwd
    {
        fs::path path;
        ~RestoreCwd()
        {
            fs::current_path(path);
        }
    } restore{previous};
    fs::current_path(workspace.outputDir());
    const auto database = utf8(moved / "mosaicraft.db");
    REQUIRE(DatabaseService().health({database}).status.ok);
    auto mosaic = makeMosaicRequest(workspace);
    mosaic.dbPath = database;
    mosaic.config.lRange = 255;
    REQUIRE(MosaicService().run(mosaic).ok);
    DatabaseUsageExportRequest exportRequest;
    exportRequest.dbPath = database;
    exportRequest.outputDir = utf8(workspace.root / "export");
    REQUIRE(DatabaseService().exportUsage(exportRequest).status.ok);
    build.dbPath = database;
    build.outputDir = utf8(moved);
    build.appendMode = true;
    REQUIRE(BuildService().run(build).ok);
}

TEST_CASE("feature payloads validate type version count finite values and exact length")
{
    TempWorkspace workspace;
    const auto tinyPath = utf8(workspace.outputDir() / "fixture.tiny");
    const auto lbpPath = utf8(workspace.outputDir() / "fixture.lbp");
    std::array<uint8_t, 256> tiny{};
    std::array<float, 256> lbp{};
    lbp[0] = 1.0f;
    writeFeature(tinyPath, FeatureKind::Tiny, tiny.data());
    writeFeature(lbpPath, FeatureKind::Lbp, lbp.data());
    CHECK(fs::file_size(u8path(tinyPath)) == 272);
    CHECK(fs::file_size(u8path(lbpPath)) == 1040);
    CHECK(readFeature(tinyPath, FeatureKind::Tiny, tiny.data()));
    CHECK(readFeature(lbpPath, FeatureKind::Lbp, lbp.data()));
    std::ifstream payload(u8path(lbpPath), std::ios::binary);
    const std::vector<char> encoded(std::istreambuf_iterator<char>(payload), {});
    payload.close();
    REQUIRE(encoded.size() == 1040);
    CHECK(encoded[16] == 0);
    CHECK(encoded[17] == 0);
    CHECK(static_cast<uint8_t>(encoded[18]) == 128);
    CHECK(encoded[19] == 63);
    CHECK_FALSE(readFeature(tinyPath, FeatureKind::Lbp, lbp.data()));
    {
        std::fstream corrupt(u8path(tinyPath), std::ios::binary | std::ios::in | std::ios::out);
        corrupt.seekp(8);
        corrupt.put(99);
    }
    CHECK_FALSE(readFeature(tinyPath, FeatureKind::Tiny, tiny.data()));
    writeFeature(tinyPath, FeatureKind::Tiny, tiny.data());
    {
        std::ofstream extra(u8path(tinyPath), std::ios::binary | std::ios::app);
        extra.put(0);
    }
    CHECK_FALSE(readFeature(tinyPath, FeatureKind::Tiny, tiny.data()));
    fs::resize_file(u8path(lbpPath), 10);
    CHECK_FALSE(readFeature(lbpPath, FeatureKind::Lbp, lbp.data()));
    lbp[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_AS(writeFeature(lbpPath, FeatureKind::Lbp, lbp.data()), std::runtime_error);
}

TEST_CASE("match-only uses feature payloads without photos output or usage writes")
{
    TempWorkspace workspace;
    writeLibraryImages(workspace.inputDir());
    writeTargetImage(workspace.targetPath());
    REQUIRE(BuildService().run(makeBuildRequest(workspace)).ok);
    auto request = makeMosaicRequest(workspace);
    request.config.lRange = 255;
    Database db(request.dbPath, Database::OpenMode::Existing);
    REQUIRE(db.isOpen());
    // 不存在的素材路径可证明匹配入口没有偷偷走贴图或图像解码分支。
    sqlite3 *raw = nullptr;
    REQUIRE(sqlite3_open(request.dbPath.c_str(), &raw) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, "UPDATE images SET file_path='missing-photo-' || id || '.png'", nullptr, nullptr, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_close(raw) == SQLITE_OK);
    const auto before = db.allRecords();
    MosaicEngine engine;
    MatchingAccess::configure(engine, MatchingLimits{}, true, 42);
    MatchingAccess::matchOnly(engine);
    std::vector<int> selected;
    MatchingAccess::captureSelections(engine, selected);
    RuntimeContext runtime(false);
    REQUIRE(engine.generate(request.inputPath, request.dbPath, request.outputPath, request.config, runtime));
    CHECK(selected.size() == 4);
    CHECK(std::all_of(selected.begin(), selected.end(), [](int index)
    {
        return index >= 0;
    }));
    CHECK_FALSE(fs::exists(workspace.mosaicPath()));
    CHECK(runtime.snapshot().format == "selection");
    CHECK(runtime.snapshot().artifacts.empty());
    for (int mode = 1; mode <= 4; ++mode)
    {
        SelectionOptions options;
        options.quality = mode != 2;
        options.spatial = mode != 1;
        options.expand = mode == 4;
        std::vector<int> reference;
        for (const size_t batch : {4, 1})
        {
            MatchingLimits limits;
            limits.maxTiles = batch;
            MatchingAccess::configure(engine, limits, true, 42);
            std::vector<SelectionDiagnostic> diagnostics;
            MatchingAccess::selectionPolicy(engine, options, &diagnostics);
            REQUIRE(engine.generate(request.inputPath, request.dbPath, request.outputPath, request.config, runtime));
            REQUIRE(diagnostics.size() == selected.size());
            for (const auto &diagnostic : diagnostics)
            {
                CHECK(diagnostic.selected >= 0);
                if (options.quality)
                {
                    CHECK(diagnostic.error <= diagnostic.limit);
                }
            }
            if (reference.empty())
            {
                reference = selected;
            }
            else
            {
                CHECK(selected == reference);
            }
        }
    }
    const auto after = db.allRecords();
    REQUIRE(after.size() == before.size());
    for (size_t i = 0; i < before.size(); ++i)
    {
        CHECK(after[i].id == before[i].id);
        CHECK(after[i].useCount == before[i].useCount);
    }
}

TEST_CASE("oversized single images use the planned format tile and actual path")
{
    TempWorkspace workspace;
    writeLibraryImages(workspace.inputDir());
    requireWriteImage(workspace.targetPath(), cv::Mat(4, 8, CV_8UC3, cv::Scalar(70, 110, 150)));
    REQUIRE(BuildService().run(makeBuildRequest(workspace)).ok);
    for (const auto &format : {std::string("jpg"), std::string("webp")})
    {
        for (bool explicitFormat : {false, true})
        {
            auto request = makeMosaicRequest(workspace);
            request.config.outW = request.config.outH = 0;
            request.config.tileW = request.config.tileH = 4;
            request.config.nativeTileW = format == "jpg" ? 33000 : 8200;
            request.config.nativeTileH = 1;
            request.config.outputFormat = format;
            request.config.formatExplicit = explicitFormat;
            request.config.generatePreview = true;
            request.config.overlayStrength = 0.25;
            request.config.analyze = true;
            const auto stem = workspace.outputDir() / (format + (explicitFormat ? "-explicit" : "-auto"));
            request.outputPath = utf8(stem) + "." + format;
            RuntimeContext runtime(false);
            REQUIRE(MosaicService().run(request, runtime).ok);
            const auto snapshot = runtime.snapshot();
            const std::string actualFormat = explicitFormat ? format : "png";
            REQUIRE(snapshot.outputPlan.has_value());
            CHECK(snapshot.outputPlan->formatSource == (explicitFormat ? "explicit" : "output_extension"));
            CHECK(snapshot.outputPlan->requestedFormat == format);
            CHECK(snapshot.outputPlan->format == actualFormat);
            CHECK(snapshot.warnings.at(explicitFormat ? "output_scaled" : "output_format_changed").count == 1);
            const auto actual = utf8(stem) + "." + actualFormat;
            CHECK(snapshot.format == actualFormat);
            CHECK(snapshot.width == (explicitFormat ? (format == "jpg" ? 65500 : 16382)
                                                   : 2 * request.config.nativeTileW));
            CHECK(snapshot.height == 1);
            const auto image = imreadUnicode(actual);
            REQUIRE_FALSE(image.empty());
            CHECK(image.cols == snapshot.width);
            CHECK(image.rows == snapshot.height);
            CHECK(fs::exists(u8path(utf8(stem) + "_preview.jpg")));
            CHECK(fs::exists(u8path(utf8(stem) + "_analysis/report.html")));
            if (!explicitFormat)
            {
                CHECK_FALSE(fs::exists(u8path(request.outputPath)));
            }
            const auto main = std::find_if(snapshot.artifacts.begin(), snapshot.artifacts.end(),
                                          [](const Artifact &item)
                                          {
                                              return item.id == "main";
                                          });
            REQUIRE(main != snapshot.artifacts.end());
            CHECK(u8path(main->path).extension() == "." + actualFormat);
        }
    }
}

TEST_CASE("output adaptation preserves fixed seed selections and committed usage")
{
    TempWorkspace workspace;
    writeLibraryImages(workspace.inputDir());
    requireWriteImage(workspace.targetPath(), cv::Mat(4, 8, CV_8UC3, cv::Scalar(70, 110, 150)));
    REQUIRE(BuildService().run(makeBuildRequest(workspace)).ok);
    const auto snapshot = workspace.root / "snapshot.db";
    fs::copy_file(workspace.dbPath(), snapshot);
    for (bool gpu : {false, true})
    {
        if (gpu && !cuda::isCudaAvailable())
        {
            continue;
        }
        std::vector<int> reference, referenceUsage;
        for (const auto &format : {std::string("png"), std::string("jpg"), std::string("webp")})
        {
            fs::copy_file(snapshot, workspace.dbPath(), fs::copy_options::overwrite_existing);
            auto request = makeMosaicRequest(workspace);
            request.outputPath = utf8(workspace.outputDir() / (format + "." + format));
            request.config.outputFormat = format;
            request.config.useGpu = gpu;
            request.config.outW = request.config.outH = 0;
            request.config.tileW = request.config.tileH = 4;
            request.config.nativeTileW = 33000;
            request.config.nativeTileH = 1;
            request.config.topNRandom = 3;
            request.config.lRange = 255;
            request.config.analyze = true;
            MosaicEngine engine;
            MatchingAccess::configure(engine, MatchingLimits{}, true, 123);
            std::vector<int> selected, usage;
            MatchingAccess::captureSelections(engine, selected);
            RuntimeContext runtime(false);
            REQUIRE(engine.generate(request.inputPath, request.dbPath, request.outputPath, request.config, runtime));
            for (const auto &record : Database(request.dbPath).allRecords())
            {
                usage.push_back(record.useCount);
            }
            for (const auto &item : DatabaseService().usage({request.dbPath}).usage.top)
            {
                usage.insert(usage.end(), {item.id, item.runs, item.tiles});
            }
            if (format == "png")
            {
                reference = selected;
                referenceUsage = usage;
            }
            else
            {
                CHECK(selected == reference);
                CHECK(usage == referenceUsage);
            }
        }
    }
    auto failed = makeMosaicRequest(workspace);
    const auto blocker = workspace.root / "blocked";
    std::ofstream(blocker) << "not a directory";
    failed.outputPath = utf8(blocker / "result.jpg");
    RuntimeContext runtime(false);
    CHECK_FALSE(MosaicService().run(failed, runtime).ok);
    const auto result = runtime.snapshot();
    REQUIRE(result.outputPlan.has_value());
    CHECK(result.outputPlan->format == "png");
    CHECK(result.format.empty());
    CHECK(result.artifacts.empty());
    CHECK(result.diagnostic.code == "output_write_failed");
}

TEST_CASE("compatible CUDA scores are exact and failures never publish partial results")
{
    using namespace mosaicraft::cuda;
    CompatibleScoreWorkspace scoring;
    if (!isCudaAvailable())
    {
        CHECK_FALSE(scoring.prepare(1, 1, 1, 1024 * 1024));
        return;
    }
    TempWorkspace workspace;
    std::vector<ImageRecord> records(67);
    cv::RNG rng(5739);
    for (size_t i = 0; i < records.size(); ++i)
    {
        cv::Mat image(17, 31, CV_8UC3);
        rng.fill(image, cv::RNG::UNIFORM, 0, 256);
        FeatureExtractor().compute(image, records[i], utf8(workspace.outputDir()), std::to_string(i));
        records[i].id = static_cast<int>(i);
        records[i].useCount = static_cast<int>(i * 37);
        if (i % 2)
        {
            for (float &value : records[i].gridLab)
            {
                value = rng.uniform(0.0f, 255.0f);
            }
        }
    }
    records[3] = records[2];
    records[3].id = 3; // 完全平分的两个图库索引。
    REQUIRE(FeaturePack::buildCache(utf8(workspace.outputDir()), records));
    FeaturePack::Reader reader(utf8(workspace.outputDir()), records);
    const int candidates = 73;
    MatchingBuffers tiles;
    tiles.resize(256, 0, candidates, false, false);
    for (int row = 0; row < 256; ++row)
    {
        const auto &record = records[row % records.size()];
        const auto &features = reader.get(row % records.size());
        tiles.l[row] = record.avgL;
        tiles.a[row] = record.avgA;
        tiles.b[row] = record.avgB;
        tiles.edge[row] = record.edgeDensity;
        tiles.grid[row] = record.gridLab;
        std::copy(features.tiny.begin(), features.tiny.end(), tiles.tiny[row].data());
        std::copy(features.lbp.begin(), features.lbp.end(), tiles.lbp[row].data());
        if (row % 3)
        {
            for (int bin = 0; bin < 256; ++bin)
            {
                tiles.lbp[row][bin] = std::nextafter(tiles.lbp[row][bin], 1.0f);
            }
            for (int cell = 0; cell < 192; ++cell)
            {
                tiles.grid[row][cell] = std::nextafter(tiles.grid[row][cell], 0.0f);
            }
        }
        tiles.labW[row] = row % 7 == 0 ? 0 : rng.uniform(0.0, 1.0);
        tiles.gridW[row] = rng.uniform(0.0, 1.0);
        tiles.tinyW[row] = rng.uniform(0.0, 1.0);
        tiles.edgeW[row] = rng.uniform(0.0, 1.0);
        tiles.lbpW[row] = rng.uniform(0.0, 1.0);
        for (int j = 0; j < candidates; ++j)
        {
            tiles.indices[size_t(row) * candidates + j] = j < 67 ? j : (j % 2 ? -1 : 67);
        }
    }
    constexpr size_t budget = 1024 * 1024;
    REQUIRE(scoring.prepare(256, candidates, 67, budget, budget));
    REQUIRE(scoring.capacity() == 256);
    GpuTimings upload, timing;
    scoring.uploadLibrary(records, reader, &upload);
    CHECK(upload.uploadBytes == scoring.libraryBytes());
    for (int count : {256, 1, 7})
    {
        for (double penalty : {0.0, 0.01, 0.123456789})
        {
            std::vector<double> result(size_t(count) * candidates);
            scoring.score(tiles, count, penalty, result.data(), &timing);
            for (int row = 0; row < count; ++row)
            {
                for (int j = 0; j < candidates; ++j)
                {
                    const double expected = j < 67 ? cpuBaseScore(tiles, row, records[j], reader.get(j), penalty)
                                                    : std::numeric_limits<double>::max();
                    CAPTURE(row);
                    CAPTURE(j);
                    CAPTURE(penalty);
                    REQUIRE(result[size_t(row) * candidates + j] == expected);
                }
            }
        }
    }
    CHECK(timing.eventComputeMs > 0);
    CHECK(timing.downloadBytes > 0);
    CHECK(scoring.hostBytes() <= budget);
    CHECK(scoring.deviceBytes() <= budget);
    std::vector<double> result(candidates, -1234);
    CHECK_THROWS_AS(scoring.score(tiles, 257, 0, result.data()), std::invalid_argument);
    CHECK_FALSE(scoring.prepare(1, candidates, 67, 1));
    CHECK_FALSE(scoring.prepare(1, candidates, 67, budget, 1));
    for (auto fault : {CompatibleScoreFault::LibraryAllocation, CompatibleScoreFault::TemporaryAllocation,
                       CompatibleScoreFault::PinnedAllocation})
    {
        CHECK_FALSE(scoring.prepare(1, candidates, 67, budget, budget, fault));
        CHECK(scoring.hostBytes() == 0);
        CHECK(scoring.deviceBytes() == 0);
        CHECK(scoring.libraryBytes() == 0);
    }
    for (auto fault : {CompatibleScoreFault::LibraryUpload, CompatibleScoreFault::ScoreUpload,
                       CompatibleScoreFault::Kernel, CompatibleScoreFault::Synchronize})
    {
        REQUIRE(scoring.prepare(1, candidates, 67, budget, budget, fault));
        if (fault == CompatibleScoreFault::LibraryUpload)
        {
            CHECK_THROWS_AS(scoring.uploadLibrary(records, reader), std::runtime_error);
        }
        else
        {
            scoring.uploadLibrary(records, reader);
            CHECK_THROWS_AS(scoring.score(tiles, 1, 0, result.data()), std::runtime_error);
        }
        CHECK(std::all_of(result.begin(), result.end(), [](double value)
        {
            return value == -1234;
        }));
        CHECK_THROWS_AS(scoring.score(tiles, 1, 0, result.data()), std::invalid_argument);
    }
    REQUIRE(scoring.prepare(256, candidates, 67, 16384, 16384));
    CHECK(scoring.capacity() < 256);
    scoring.uploadLibrary(records, reader);
    scoring.score(tiles, 1, 0, result.data());
    CHECK(result[2] == result[3]);
}

TEST_CASE("dynamic CUDA keeps CPU library records selections analysis and encoded pixels")
{
    TempWorkspace workspace;
    writeLibraryImages(workspace.inputDir());
    writeTargetImage(workspace.targetPath());
    auto build = makeBuildRequest(workspace);
    build.normalizeWidth = 255;
    build.normalizeHeight = 257;
    build.threads = 3;
    REQUIRE(BuildService().run(build).ok);
    auto bytes = [](const fs::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    std::vector<ImageRecord> original;
    {
        Database db(build.dbPath);
        original = db.allRecords();
        REQUIRE(original.size() == 10);
        for (const auto &record : original)
        {
            ImageRecord expected;
            FeatureExtractor().compute(imreadUnicode(record.filePath), expected, utf8(workspace.outputDir()), "cpu");
            CHECK(record.avgL == expected.avgL);
            CHECK(record.avgA == expected.avgA);
            CHECK(record.avgB == expected.avgB);
            CHECK(record.gridLab == expected.gridLab);
            CHECK(record.edgeDensity == expected.edgeDensity);
            CHECK(bytes(u8path(record.tinyPath)) == bytes(u8path(expected.tinyPath)));
            CHECK(bytes(u8path(record.lbpPath)) == bytes(u8path(expected.lbpPath)));
        }
    }
    build.appendMode = true;
    REQUIRE(BuildService().run(build).ok);
    {
        Database db(build.dbPath);
        const auto repeated = db.allRecords();
        REQUIRE(repeated.size() == original.size());
        for (size_t i = 0; i < original.size(); ++i)
        {
            CHECK(repeated[i].id == original[i].id);
            CHECK(repeated[i].filePath == original[i].filePath);
            CHECK(repeated[i].fileHash == original[i].fileHash);
        }
    }
    auto request = makeMosaicRequest(workspace);
    request.config.outW = 41;
    request.config.outH = 73;
    request.config.lRange = 255;
    request.config.topNRandom = 3;
    request.config.adaptiveWeights = true;
    request.config.overlayStrength = 0.35;
    for (bool analyze : {false, true})
    {
        request.config.analyze = analyze;
        request.config.lRange = analyze ? 20 : 255;
        std::vector<int> reference;
        std::vector<int> referenceUsage;
        std::vector<char> encoded, report;
        // 四种后端组合、小/大批次、联合预算及两种能力的独立分配回退。
        for (int profile = 0; profile < 10; ++profile)
        {
            sqlite3 *connection = nullptr;
            REQUIRE(sqlite3_open(build.dbPath.c_str(), &connection) == SQLITE_OK);
            REQUIRE(sqlite3_exec(connection, "UPDATE images SET use_count=0", nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(connection);
            MosaicEngine engine;
            request.config.useGpu = profile != 0;
            MatchingLimits limits;
            limits.maxTiles = profile == 1 ? 3 : 256;
            if (profile == 3)
            {
                limits.hostBytes = 64 * 1024;
            }
            limits.compatibleFeatures = profile != 4;
            limits.compatibleScoring = profile != 5;
            if (profile == 6)
            {
                limits.featureFault = cuda::PipelineFault::DeviceAllocation;
            }
            if (profile == 7)
            {
                limits.scoreFault = cuda::CompatibleScoreFault::LibraryAllocation;
            }
            if (profile == 8)
            {
                limits.scoreFault = cuda::CompatibleScoreFault::PinnedAllocation;
            }
            if (profile == 9)
            {
                limits.deviceBytes = 1024 * 1024;
            }
            MatchingAccess::configure(engine, limits, true, 123);
            std::vector<int> selected;
            MatchingAccess::captureSelections(engine, selected);
            REQUIRE(engine.generate(request.inputPath, request.dbPath, request.outputPath, request.config));
            CHECK(MatchingAccess::compatibleFeatureImages(engine) ==
                  ((profile == 1 || profile == 2 || profile == 5 || profile == 7 || profile == 8 || profile == 9) &&
                    cuda::isCudaAvailable() ? selected.size() : 0));
            CHECK(MatchingAccess::compatibleScoreTiles(engine) ==
                  ((profile == 1 || profile == 2 || profile == 3 || profile == 4 || profile == 6 || profile == 9) &&
                    cuda::isCudaAvailable() ? selected.size() : 0));
            CHECK(MatchingAccess::compatibleTemporaryBytes(engine) <= limits.deviceBytes);
            const auto output = bytes(u8path(request.outputPath));
            std::vector<int> usage;
            for (const auto &record : Database(build.dbPath).allRecords())
            {
                usage.push_back(record.useCount);
            }
            const auto analysis = analyze ? bytes(workspace.outputDir() / "mosaic_analysis" / "worst_report.txt")
                                         : std::vector<char>();
            if (profile == 0)
            {
                reference = selected;
                referenceUsage = usage;
                encoded = output;
                report = analysis;
            }
            else
            {
                CHECK(selected == reference);
                CHECK(usage == referenceUsage);
                CHECK(output == encoded);
                CHECK(analysis == report);
            }
        }
    }
    if (cuda::isCudaAvailable())
    {
        request.config.useGpu = true;
        for (auto fault : {cuda::CompatibleScoreFault::LibraryUpload, cuda::CompatibleScoreFault::ScoreUpload,
                           cuda::CompatibleScoreFault::Kernel, cuda::CompatibleScoreFault::Synchronize})
        {
            const auto before = Database(build.dbPath).allRecords();
            const auto output = bytes(u8path(request.outputPath));
            MosaicEngine engine;
            MatchingLimits limits;
            limits.maxTiles = 3;
            limits.scoreFault = fault;
            MatchingAccess::configure(engine, limits, true, 123);
            CHECK_THROWS_AS(engine.generate(request.inputPath, request.dbPath, request.outputPath, request.config),
                            std::runtime_error);
            const auto after = Database(build.dbPath).allRecords();
            REQUIRE(after.size() == before.size());
            for (size_t i = 0; i < before.size(); ++i)
            {
                CHECK(after[i].useCount == before[i].useCount);
            }
            CHECK(bytes(u8path(request.outputPath)) == output);
        }
    }

}

TEST_CASE("regression: build, inspect, and render a small mosaic")
{
    TempWorkspace workspace;
    REQUIRE_FALSE(workspace.root.empty());

    writeLibraryImages(workspace.inputDir());
    writeTargetImage(workspace.targetPath());

    const auto build = BuildService().run(makeBuildRequest(workspace));
    REQUIRE(build.ok);

    const auto stats = DatabaseService().stats({utf8(workspace.dbPath())});
    REQUIRE(stats.status.ok);
    CHECK(stats.stats.total == 10);
    CHECK_FALSE(stats.stats.empty);
    CHECK(stats.stats.featureWidth == "36");
    CHECK(stats.stats.featureHeight == "64");
    CHECK(stats.stats.gridDim == 192);

    const auto health = DatabaseService().health({utf8(workspace.dbPath())});
    REQUIRE(health.status.ok);
    CHECK(health.health.total == 10);
    CHECK_FALSE(health.health.empty);

    const auto inspect = InspectService().inspect({utf8(workspace.targetPath()), utf8(workspace.dbPath())});
    REQUIRE(inspect.status.ok);
    CHECK(inspect.width == 36);
    CHECK(inspect.height == 64);
    CHECK(inspect.databaseAvailable);
    CHECK(inspect.databaseTotal == 10);

    const auto mosaic = MosaicService().run(makeMosaicRequest(workspace));
    REQUIRE(mosaic.ok);
    CHECK(fs::exists(workspace.mosaicPath()));

    const cv::Mat output = imreadUnicode(utf8(workspace.mosaicPath()), cv::IMREAD_COLOR);
    REQUIRE_FALSE(output.empty());
    CHECK(output.cols == 36);
    CHECK(output.rows == 64);

    const auto usage = DatabaseService().usage({utf8(workspace.dbPath()), 10, true});
    REQUIRE(usage.status.ok);
    CHECK(usage.usage.total == 10);
}

TEST_CASE("job artifacts preserve requested paths and reject changed files")
{
    TempWorkspace workspace;
    writeLibraryImages(workspace.inputDir());
    writeTargetImage(workspace.targetPath());
    REQUIRE(BuildService().run(makeBuildRequest(workspace)).ok);
    auto request = makeMosaicRequest(workspace);
    request.outputPath = utf8(workspace.outputDir() / "changed-extension.jpg");
    request.config.generatePreview = true;
    JobManager jobs;
    const auto id = jobs.submitMosaic(request);
    JobSnapshot job;
    REQUIRE(jobs.waitJob(id, job));
    REQUIRE(job.state == JobState::Succeeded);
    CHECK(job.outputPath == request.outputPath);
    CHECK(job.result.runtime.progress.stage == "finalizing");
    REQUIRE(job.result.runtime.artifacts.size() == 2);
    auto artifactRequest = apiJobRequest(ApiOperation::GetJobArtifact, id);
    artifactRequest.artifactId = "main";
    const auto response = handleApiRequest(artifactRequest, jobs);
    REQUIRE(response.status == 200);
    REQUIRE(response.file.has_value());
    CHECK(u8path(response.file->path).extension() == ".png");
    EventPage events;
    REQUIRE(jobs.getEvents(id, 0, 100, events));
    CHECK_FALSE(events.events.empty());
    std::ofstream(u8path(response.file->path), std::ios::app).put('x');
    CHECK(handleApiRequest(artifactRequest, jobs).status == 409);
    fs::remove(u8path(response.file->path));
    CHECK(handleApiRequest(artifactRequest, jobs).status == 404);
}

TEST_CASE("analysis preserves usage commit boundaries after saving the mosaic")
{
    TempWorkspace workspace;
    writeLibraryImages(workspace.inputDir());
    writeTargetImage(workspace.targetPath());
    REQUIRE(BuildService().run(makeBuildRequest(workspace)).ok);
    auto request = makeMosaicRequest(workspace);
    request.config.lRange = 255;
    request.config.analyze = true;
    const auto reportDir = workspace.outputDir() / "mosaic_analysis";
    bool commits = true;
    bool succeeds = true;
    std::string failureCode;

    SUBCASE("analysis disabled")
    {
        request.config.analyze = false;
        commits = false;
    }
    SUBCASE("report directory blocked")
    {
        std::ofstream(reportDir).put('x');
        commits = false;
    }
    SUBCASE("frequency directory blocked")
    {
        fs::create_directories(reportDir);
        std::ofstream(reportDir / "freq_rank").put('x');
        commits = false;
    }
    SUBCASE("HTML cannot open but usage still commits")
    {
        fs::create_directories(reportDir / "report.html");
    }
    SUBCASE("preview fails after the main file closes")
    {
        request.config.generatePreview = true;
        fs::create_directory(workspace.outputDir() / "mosaic_preview.jpg");
        commits = succeeds = false;
        failureCode = "preview_write_failed";
    }
    SUBCASE("usage transaction fails after output is saved")
    {
        REQUIRE(Database(request.dbPath).initUsageStats());
        sqlite3 *connection = nullptr;
        REQUIRE(sqlite3_open(request.dbPath.c_str(), &connection) == SQLITE_OK);
        const auto result = sqlite3_exec(connection,
                                         "CREATE TRIGGER reject_usage BEFORE INSERT ON usage_stats "
                                         "BEGIN SELECT RAISE(ABORT, 'blocked usage'); END",
                                         nullptr, nullptr, nullptr);
        sqlite3_close(connection);
        REQUIRE(result == SQLITE_OK);
        commits = false;
        succeeds = false;
        failureCode = "usage_commit_failed";
    }

    const auto result = MosaicService().run(request);
    CHECK(result.ok == succeeds);
    CHECK(fs::exists(workspace.mosaicPath()));
    REQUIRE_FALSE(result.runtime.artifacts.empty());
    CHECK(result.runtime.artifacts.front().id == "main");
    if (fs::is_directory(reportDir / "report.html"))
    {
        CHECK(result.runtime.warnings.count("analysis_write_failed") == 1);
    }
    int used = 0;
    for (const auto &[id, runs, tiles] : Database(request.dbPath).topUsedImages(100))
    {
        used += tiles;
    }
    CHECK(used == (commits ? 4 : 0));
    if (!succeeds)
    {
        CHECK(result.runtime.diagnostic.code == failureCode);
    }
}

TEST_CASE("rebuilding preserves indexed files and source files")
{
    TempWorkspace workspace;
    writeLibraryImages(workspace.inputDir());
    auto request = makeBuildRequest(workspace);
    REQUIRE(BuildService().run(request).ok);
    REQUIRE(BuildService().run(request).ok);
    Database database(request.dbPath);
    REQUIRE(database.totalCount() == 10);
    for (const auto& record : database.allRecords())
    {
        CHECK(fs::exists(u8path(record.filePath)));
    }
    request.outputDir = request.inputDir;
    request.dbPath = utf8(workspace.root / "same.db");
    const auto nested = workspace.inputDir() / u8path("递归子目录");
    fs::create_directory(nested);
    fs::rename(workspace.inputDir() / "tile_00.png", nested / "tile_00.png");
    request.recursive = true;
    REQUIRE(BuildService().run(request).ok);
    REQUIRE(BuildService().run(request).ok);
    CHECK_FALSE(fs::exists(workspace.inputDir() / "tile_00.png"));
    CHECK(fs::exists(workspace.inputDir() / "000000.png"));
    Database sourceDatabase(request.dbPath);
    CHECK(sourceDatabase.totalCount() == 10);
    for (const auto& record : sourceDatabase.allRecords())
    {
        // Windows 临时目录可能使用 8.3 短路径；验证目录身份而非路径拼写。
        CHECK(fs::equivalent(u8path(record.filePath).parent_path(), workspace.inputDir()));
    }
}

TEST_CASE("color cache rejects old structures and retains fractional LAB sources")
{
    TempWorkspace workspace;
    writeLibraryImages(workspace.inputDir());
    REQUIRE(BuildService().run(makeBuildRequest(workspace)).ok);
    SUBCASE("incomplete cache structure is not upgraded")
    {
        REQUIRE(fixtureSql(workspace.dbPath(), "ALTER TABLE image_color_order DROP COLUMN source_l") == SQLITE_OK);
        Database blocked(utf8(workspace.dbPath()));
        CHECK_FALSE(blocked.isOpen());
        return;
    }
    Database database(utf8(workspace.dbPath()));
    REQUIRE(database.isOpen());
    const auto record = database.allRecords().front();
    ImageColorOrder color{record.fileHash, kColorOrderVersion, meanLabColorOrderKey(100.25,128.6,128.2),100.25,128.6,128.2};
    REQUIRE(database.saveImageColorOrder(record.id,color));
    const auto cached=database.imageColorOrders().at(record.id);
    CHECK(cached.version == kColorOrderVersion);
    CHECK(cached.key == color.key);
    CHECK(cached.sourceL == color.sourceL);
    CHECK(cached.sourceA == color.sourceA);
    CHECK(cached.sourceB == color.sourceB);
}

TEST_CASE("database maintenance rejects missing files and preserves usage integrity")
{
    TempWorkspace workspace;
    auto missing = utf8(workspace.root / "missing.db");
    CHECK_FALSE(DatabaseService().stats({missing}).status.ok);
    CHECK_FALSE(fs::exists(u8path(missing)));
    writeLibraryImages(workspace.inputDir());
    REQUIRE(BuildService().run(makeBuildRequest(workspace)).ok);
    Database database(utf8(workspace.dbPath()));
    auto records = database.allRecords();
    REQUIRE(database.recordRunUsage({{records[0].id, 1}}, "one", "target"));
    fs::remove(u8path(records[0].filePath));
    auto health = DatabaseService().health({utf8(workspace.dbPath())});
    CHECK(health.health.missingFiles == 1);
    REQUIRE(DatabaseService().purge({utf8(workspace.dbPath()), false, true}).status.ok);
    CHECK(database.topUsedImages().empty());
    CHECK(DatabaseService().health({utf8(workspace.dbPath())}).health.usedPct == 0);

    // 同一个目标同时提交时，累计两轮 tile，但独立目标数只增加一次。
    Database other(utf8(workspace.dbPath()), Database::OpenMode::Existing);
    std::atomic<int> ready{0};
    bool firstOk = false, secondOk = false;
    auto record = [&](Database& connection, bool& ok)
    {
        ++ready;
        while (ready.load() < 2)
        {
            std::this_thread::yield();
        }
        ok = connection.recordRunUsage({{records[1].id, 2}}, "same", "concurrent-target");
    };
    std::thread first(record, std::ref(database), std::ref(firstOk));
    std::thread second(record, std::ref(other), std::ref(secondOk));
    first.join();
    second.join();
    REQUIRE(firstOk);
    REQUIRE(secondOk);
    const auto usage = database.topUsedImages();
    REQUIRE(usage.size() == 1);
    CHECK(std::get<1>(usage[0]) == 1);
    CHECK(std::get<2>(usage[0]) == 4);
}

TEST_CASE("color reorder preserves records and recovers interrupted name swaps")
{
    TempWorkspace workspace;
    writeLibraryImages(workspace.inputDir());
    const auto request = makeBuildRequest(workspace);
    REQUIRE(BuildService().run(request).ok);
    std::vector<ImageRecord> records;
    {
        Database database(request.dbPath);
        records = database.allRecords();
    }
    const std::string token = "112233";
    const auto staging = workspace.libraryDir() / (".mosaicraft-color-order-" + token);
    fs::create_directory(staging);
    nlohmann::json journal = {{"version", 1}, {"database", utf8(fs::weakly_canonical(workspace.dbPath()))},
        {"token", token}, {"output", utf8(workspace.libraryDir())}, {"staging", utf8(staging)},
        {"phase", "staging"}, {"entries", nlohmann::json::array()}};
    for (int i = 0; i < 2; ++i)
    {
        journal["entries"].push_back({{"id", records[i].id}, {"old_db_path", records[i].filePath},
            {"original", records[i].filePath}, {"final", records[1 - i].filePath},
            {"temporary", utf8(staging / (std::to_string(records[i].id) + ".stage"))},
            {"hash", records[i].fileHash}, {"size", fs::file_size(u8path(records[i].filePath))}});
    }
    bool committed = false;
    SUBCASE("interrupted staging")
    {
        fs::rename(u8path(records[0].filePath), u8path(journal["entries"][0]["temporary"].get<std::string>()));
    }
#ifdef _WIN32
    SUBCASE("Windows database path spelling is case insensitive")
    {
        auto path = journal["database"].get<std::string>();
        std::transform(path.begin(), path.end(), path.begin(),
            [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        journal["database"] = path;
    }
#endif
    SUBCASE("interrupted publishing")
    {
        for (const auto& entry : journal["entries"])
        {
            fs::rename(u8path(entry["original"].get<std::string>()), u8path(entry["temporary"].get<std::string>()));
        }
        const auto& first = journal["entries"][0];
        fs::rename(u8path(first["temporary"].get<std::string>()), u8path(first["final"].get<std::string>()));
        journal["phase"] = "publishing";
    }
    SUBCASE("interrupted rollback")
    {
        const auto& second = journal["entries"][1];
        fs::rename(u8path(second["original"].get<std::string>()), u8path(second["temporary"].get<std::string>()));
        journal["phase"] = "restoring";
    }
    SUBCASE("committed before journal cleanup")
    {
        Database database(request.dbPath, Database::OpenMode::Build);
        REQUIRE(database.isOpen());
        REQUIRE(database.beginImmediateTransaction());
        for (const auto& entry : journal["entries"])
        {
            fs::rename(u8path(entry["original"].get<std::string>()), u8path(entry["temporary"].get<std::string>()));
            REQUIRE(database.updateImagePath(entry["id"], entry["old_db_path"], entry["temporary"]));
        }
        for (const auto& entry : journal["entries"])
        {
            fs::rename(u8path(entry["temporary"].get<std::string>()), u8path(entry["final"].get<std::string>()));
            REQUIRE(database.updateImagePath(entry["id"], entry["temporary"], entry["final"]));
        }
        REQUIRE(database.setMeta("color_order_batch", token));
        REQUIRE(database.commitTransaction());
        journal["phase"] = "publishing";
        committed = true;
    }
    {
        std::ofstream output(libraryJournalPath(request.dbPath));
        auto stored = journal;
        stored["version"] = kJournalVersion;
        stored["library_version"] = kLibraryVersion;
        stored["feature_version"] = kFeatureVersion;
        const LibraryPaths paths(utf8(workspace.dbPath()));
        stored["database"] = paths.store(stored["database"].get<std::string>());
        stored["output"] = ".";
        stored["staging"] = paths.store(stored["staging"].get<std::string>());
        for (auto &entry : stored["entries"])
        {
            for (const auto *key : {"old_db_path","original","final","temporary"})
            {
                entry[key] = paths.store(entry[key].get<std::string>());
            }
        }
        output << stored;
    }
    {
        Database blocked(request.dbPath, Database::OpenMode::Existing);
        CHECK_FALSE(blocked.isOpen());
        CHECK(blocked.error().find("unfinished color reorder") != std::string::npos);
    }
    std::string error;
    {
        Database database(request.dbPath, Database::OpenMode::Build);
        REQUIRE(database.isOpen());
        REQUIRE_MESSAGE(recoverLibraryColorOrder(database, error), error);
        const auto restored = database.allRecords();
        REQUIRE(restored.size() == records.size());
        for (size_t i = 0; i < restored.size(); ++i)
        {
            CHECK(restored[i].id == records[i].id);
            CHECK(restored[i].fileHash == records[i].fileHash);
            CHECK(restored[i].tinyPath == records[i].tinyPath);
            const auto imagePath = u8path(restored[i].filePath);
            CHECK(fs::exists(imagePath.is_absolute() ? imagePath : workspace.root / imagePath));
            const auto originalIndex = committed && i < 2 ? 1 - i : i;
            CHECK(restored[i].filePath == records[originalIndex].filePath);
        }
    }
    CHECK_FALSE(fs::exists(staging));
    CHECK_FALSE(fs::exists(libraryJournalPath(request.dbPath)));
}

TEST_CASE("color reorder handles occupied paths and releases library locks")
{
    TempWorkspace workspace;
    writeLibraryImages(workspace.inputDir());
    const auto request = makeBuildRequest(workspace);
    REQUIRE(BuildService().run(request).ok);
    Database database(request.dbPath, Database::OpenMode::Build);
    REQUIRE(database.isOpen());
    Database blocked(request.dbPath, Database::OpenMode::Existing);
    CHECK_FALSE(blocked.isOpen());
    auto records = database.allRecords();
    ColorOrderResult result;
    std::string error;
    SUBCASE("unindexed file is preserved")
    {
        const auto destination = workspace.outputDir();
        requireWriteImage(destination / "000000.png", cv::Mat(32, 32, CV_8UC3, cv::Scalar(1, 2, 3)));
        CHECK_FALSE(reorderLibraryByColor(database, utf8(destination), result, error));
        CHECK(error.find("unindexed file") != std::string::npos);
        CHECK(fs::exists(destination / "000000.png"));
    }
    SUBCASE("color cache and stable reordering")
    {
        REQUIRE_MESSAGE(reorderLibraryByColor(database, request.outputDir, result, error), error);
        CHECK(result.computed == 0);
        ColorOrderResult repeated;
        REQUIRE_MESSAGE(reorderLibraryByColor(database, request.outputDir, repeated, error), error);
        CHECK(repeated.computed == 0);
        CHECK(repeated.renamed == 0);
        CHECK(database.imageColorOrders().size() == 10);
    }
    SUBCASE("invalid color cache is recomputed without modifying matching features")
    {
        auto cached = database.imageColorOrders().at(records[0].id);
        cached.version = 0;
        REQUIRE(database.saveImageColorOrder(records[0].id, cached));
        REQUIRE_MESSAGE(reorderLibraryByColor(database, request.outputDir, result, error), error);
        CHECK(result.computed == 1);
        CHECK(result.renamed == 0);
        CHECK(database.imageColorOrders().at(records[0].id).version == kColorOrderVersion);
    }
    SUBCASE("average changes invalidate the cache even with the same content hash")
    {
        auto changed = records[0];
        changed.avgA += 0.1;
        REQUIRE(database.insertImage(changed));
        REQUIRE_MESSAGE(reorderLibraryByColor(database, request.outputDir, result, error), error);
        CHECK(result.computed == 1);
        CHECK(database.imageColorOrders().at(changed.id).sourceA == changed.avgA);
        ColorOrderResult repeated;
        REQUIRE(reorderLibraryByColor(database, request.outputDir, repeated, error));
        CHECK(repeated.computed == 0);
        CHECK(repeated.renamed == 0);
    }
    SUBCASE("missing average fails before moving indexed files")
    {
        sqlite3* raw = nullptr;
        REQUIRE(sqlite3_open(request.dbPath.c_str(), &raw) == SQLITE_OK);
        const auto sql = "UPDATE images SET avg_a=NULL WHERE id=" + std::to_string(records[0].id);
        const int updated = sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr);
        sqlite3_close(raw);
        REQUIRE(updated == SQLITE_OK);
        CHECK_FALSE(reorderLibraryByColor(database, utf8(workspace.outputDir()), result, error));
        CHECK(error.find("invalid average color") != std::string::npos);
        CHECK(result.renamed == 0);
        CHECK_FALSE(fs::exists(libraryJournalPath(request.dbPath)));
    }
#ifdef _WIN32
    SUBCASE("locked source rolls back already staged files")
    {
        const auto handle = CreateFileW(u8path(records[3].filePath).c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        REQUIRE(handle != INVALID_HANDLE_VALUE);
        const bool reordered = reorderLibraryByColor(database, utf8(workspace.outputDir()), result, error);
        CloseHandle(handle);
        CHECK_FALSE(reordered);
        CHECK_FALSE(fs::exists(libraryJournalPath(request.dbPath)));
    }
#endif
    for (const auto& record : database.allRecords())
    {
        CHECK(fs::exists(u8path(record.filePath)));
    }
}

TEST_CASE("same-size builds deduplicate saved pixels and preserve existing records")
{
    auto pixelHash = [](const cv::Mat& image)
    {
        uint64_t hash = 14695981039346656037ULL;
        for (size_t i = 0; i < image.total() * image.elemSize(); ++i)
        {
            hash = (hash ^ image.data[i]) * 1099511628211ULL;
        }
        return std::to_string(hash);
    };
    for (const cv::Size size : {cv::Size(180, 320), cv::Size(72, 128)})
    {
        TempWorkspace workspace;
        auto request = makeBuildRequest(workspace);
        request.normalizeWidth = size.width;
        request.normalizeHeight = size.height;
        request.threads = 2;
        cv::Mat exact(size, CV_8UC3), larger(size.height + 3, size.width + 2, CV_8UC3);
        cv::RNG(12).fill(exact, cv::RNG::UNIFORM, 0, 256);
        cv::RNG(34).fill(larger, cv::RNG::UNIFORM, 0, 256);
        const auto jpeg = utf8(workspace.inputDir() / u8path(u8"原图.jpg"));
        const auto png = utf8(workspace.inputDir() / u8path(u8"原图.png"));
        const auto resized = utf8(workspace.inputDir() / (size.width == 180 ? "resize.jpg" : "resize.JPEG"));
        REQUIRE(imwriteUnicode(jpeg, exact, {cv::IMWRITE_JPEG_QUALITY, 97}));
        REQUIRE(imwriteUnicode(png, exact, {cv::IMWRITE_PNG_COMPRESSION, 1}));
        REQUIRE(imwriteUnicode(resized, larger, {cv::IMWRITE_JPEG_QUALITY, 97}));
        REQUIRE(writeBytesUnicode(utf8(workspace.inputDir() / "duplicate.jpeg"), readBytesUnicode(jpeg)));
        REQUIRE(writeBytesUnicode(utf8(workspace.inputDir() / "broken.jpg"), {1, 2, 3}));
        ImageNormalizer normalizer(size.width, size.height);
        const cv::Mat oldPixels = normalizer.centerCrop(normalizer.resizeToCover(imreadUnicode(resized)));
        std::vector<uint8_t> resizedBytes;
        REQUIRE(cv::imencode(".jpg", oldPixels, resizedBytes, {cv::IMWRITE_JPEG_QUALITY, 100}));
        std::unordered_map<std::string, std::vector<uint8_t>> expected;
        for (const auto& bytes : {readBytesUnicode(jpeg), readBytesUnicode(png), resizedBytes})
        {
            expected.emplace(pixelHash(cv::imdecode(bytes, cv::IMREAD_COLOR)), bytes);
        }
        REQUIRE(expected.size() == 3);
        const auto build = BuildService().run(request);
        REQUIRE_MESSAGE(build.ok, build.message);
        std::vector<ImageRecord> before;
        std::unordered_map<std::string, fs::file_time_type> featureTimes;
        {
            Database database(request.dbPath);
            before = database.allRecords();
            REQUIRE(before.size() == expected.size());
            for (const auto& record : before)
            {
                const auto pixels = imreadUnicode(record.filePath);
                CHECK(record.fileHash == pixelHash(pixels));
                CHECK(pixels.size() == size);
                CHECK(record.featureVersion == FeatureExtractor::featureVersion());
                REQUIRE(expected.count(record.fileHash) == 1);
                CHECK(readBytesUnicode(record.filePath) == expected.at(record.fileHash));
                CHECK(fs::exists(u8path(record.tinyPath)));
                CHECK(fs::exists(u8path(record.lbpPath)));
                for (const auto& path : {record.tinyPath, record.lbpPath})
                {
                    // 固定旧时间，能识别追加时即使内容相同的无谓重写。
                    const auto stamp = fs::file_time_type::clock::now() - std::chrono::hours(1);
                    fs::last_write_time(u8path(path), stamp);
                    featureTimes.emplace(path, fs::last_write_time(u8path(path)));
                }
            }
            REQUIRE(database.recordRunUsage({{before[0].id, 3}}, "target", "target.png"));
        }
        request.appendMode = true;
        REQUIRE(BuildService().run(request).ok);
        auto inPlace = request;
        inPlace.inputDir = inPlace.outputDir;
        REQUIRE(writeBytesUnicode(utf8(u8path(inPlace.inputDir) / "broken.jpg"), {1, 2, 3}));
        REQUIRE(BuildService().run(inPlace).ok);
        {
            Database database(request.dbPath);
            const auto records = database.allRecords();
            REQUIRE(records.size() == before.size());
            const auto usage = database.topUsedImages();
            REQUIRE(usage.size() == 1);
            CHECK(std::get<0>(usage[0]) == before[0].id);
            CHECK(std::get<2>(usage[0]) == 3);
            for (const auto& record : records)
            {
                const auto old = std::find_if(before.begin(), before.end(),
                                             [&](const auto& previous)
                                             {
                                                 return previous.id == record.id;
                                             });
                REQUIRE(old != before.end());
                CHECK(record.fileHash == old->fileHash);
                CHECK(record.gridLab == old->gridLab);
                CHECK(record.tinyPath == old->tinyPath);
                CHECK(record.lbpPath == old->lbpPath);
                CHECK(fs::last_write_time(u8path(record.tinyPath)) == featureTimes.at(record.tinyPath));
                CHECK(fs::last_write_time(u8path(record.lbpPath)) == featureTimes.at(record.lbpPath));
                CHECK(readBytesUnicode(record.filePath) == expected.at(record.fileHash));
            }
        }
        auto normalizeOnly = request;
        normalizeOnly.outputDir = utf8(workspace.root / "normalize-only");
        normalizeOnly.dbPath = utf8(u8path(normalizeOnly.outputDir) / "mosaicraft.db");
        normalizeOnly.normalizeOnly = true;
        REQUIRE(BuildService().run(normalizeOnly).ok);
        int outputs = 0;
        for (const auto& entry : fs::directory_iterator(u8path(normalizeOnly.outputDir)))
        {
            if (entry.path().extension() == ".jpg" || entry.path().extension() == ".jpeg" ||
                entry.path().extension() == ".JPEG" || entry.path().extension() == ".png")
            {
                const auto pixels = imreadUnicode(utf8(entry.path()));
                REQUIRE_FALSE(pixels.empty());
                CHECK(readBytesUnicode(utf8(entry.path())) == expected.at(pixelHash(pixels)));
                ++outputs;
            }
        }
        CHECK(outputs == 4);
        Database emptyDatabase(normalizeOnly.dbPath);
        CHECK(emptyDatabase.totalCount() == 0);
    }
}

TEST_CASE("default color build and append keep bytes IDs usage and feature references")
{
    TempWorkspace workspace;
    auto request = makeBuildRequest(workspace);
    const auto input = workspace.inputDir() / u8path("颜色子目录");
    fs::create_directory(input);
    request.outputDir = utf8(workspace.root / u8path("渐变图库"));
    request.dbPath = utf8(u8path(request.outputDir) / "mosaicraft.db");
    request.recursive = true;
    request.threads = 4;
    // 使用 GPU 支持的尺寸，两种构建都实际覆盖各自的特征提取路径。
    request.normalizeWidth = 180;
    request.normalizeHeight = 320;
    requireWriteImage(input / "a_blue.png", cv::Mat(64, 36, CV_8UC3, cv::Scalar(255, 0, 0)));
    requireWriteImage(input / "b_green.bmp", cv::Mat(64, 36, CV_8UC3, cv::Scalar(0, 255, 0)));
    requireWriteImage(input / "z_red.jpg", cv::Mat(64, 36, CV_8UC3, cv::Scalar(0, 0, 255)));
    REQUIRE(BuildService().run(request).ok);
    auto bytes = [](const std::string& path)
    {
        std::ifstream input(u8path(path), std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    std::vector<ImageRecord> original;
    std::unordered_map<int, std::vector<char>> originalBytes;
    {
        Database database(request.dbPath);
        original = database.allRecords();
        REQUIRE(original.size() == 3);
        CHECK(fs::exists(u8path(request.outputDir) / "000000.jpg"));
        CHECK(fs::exists(u8path(request.outputDir) / "000001.bmp"));
        CHECK(fs::exists(u8path(request.outputDir) / "000002.png"));
        REQUIRE(database.recordRunUsage({{original[0].id, 5}}, "target", "target.png"));
        for (const auto& record : original)
        {
            originalBytes[record.id] = bytes(record.filePath);
        }
    }
    requireWriteImage(input / "new_black.png", cv::Mat(64, 36, CV_8UC3, cv::Scalar(0, 0, 0)));
    request.appendMode = true;
    const auto appended = BuildService().run(request);
    REQUIRE_MESSAGE(appended.ok, appended.message);
    REQUIRE(BuildService().run(request).ok);
    auto emptyAppend = request;
    emptyAppend.inputDir = utf8(workspace.emptyInputDir());
    REQUIRE(BuildService().run(emptyAppend).ok);
    {
        Database database(request.dbPath);
        const auto records = database.allRecords();
        REQUIRE(records.size() == 4);
        CHECK(fs::exists(u8path(request.outputDir) / "000000.png"));
        CHECK(fs::exists(u8path(request.outputDir) / "000001.jpg"));
        CHECK(fs::exists(u8path(request.outputDir) / "000002.bmp"));
        CHECK(fs::exists(u8path(request.outputDir) / "000003.png"));
        CHECK(database.imageColorOrders().size() == 4);
        const auto usage = database.topUsedImages();
        REQUIRE(usage.size() == 1);
        CHECK(std::get<0>(usage[0]) == original[0].id);
        CHECK(std::get<1>(usage[0]) == 1);
        CHECK(std::get<2>(usage[0]) == 5);
        for (const auto& before : original)
        {
            const auto found = std::find_if(records.begin(), records.end(),
                [&](const auto& record)
                {
                    return record.id == before.id;
                });
            REQUIRE(found != records.end());
            CHECK(bytes(found->filePath) == originalBytes.at(before.id));
            CHECK(found->fileHash == before.fileHash);
            CHECK(found->tinyPath == before.tinyPath);
            CHECK(found->lbpPath == before.lbpPath);
            CHECK(found->gridLab == before.gridLab);
            CHECK(found->useCount == before.useCount);
            CHECK(fs::exists(u8path(found->tinyPath)));
            CHECK(fs::exists(u8path(found->lbpPath)));
        }
    }
    CHECK(fs::exists(input / "z_red.jpg"));
    writeTargetImage(workspace.targetPath());
    auto mosaic = makeMosaicRequest(workspace);
    mosaic.dbPath = request.dbPath;
    REQUIRE(MosaicService().run(mosaic).ok);
    REQUIRE_FALSE(imreadUnicode(utf8(workspace.mosaicPath())).empty());
    auto normalized = request;
    normalized.outputDir = utf8(workspace.root / "normalized");
    fs::create_directory(u8path(normalized.outputDir));
    normalized.dbPath = utf8(u8path(normalized.outputDir) / "mosaicraft.db");
    normalized.normalizeOnly = true;
    REQUIRE(BuildService().run(normalized).ok);
    Database normalizedDatabase(normalized.dbPath);
    CHECK(normalizedDatabase.totalCount() == 0);
    CHECK(normalizedDatabase.imageColorOrders().empty());
}

TEST_CASE("mosaic validates dimensions formats and legacy grids before processing")
{
    TempWorkspace workspace;
    writeLibraryImages(workspace.inputDir());
    writeTargetImage(workspace.targetPath());
    REQUIRE(BuildService().run(makeBuildRequest(workspace)).ok);
    auto request = makeMosaicRequest(workspace);
    request.config.upscale = 200000;
    CHECK_FALSE(MosaicService().run(request).ok);
    request.config.upscale = 1;
    request.config.outputFormat = "PNG";
    REQUIRE(MosaicService().run(request).ok);
    request.config.outW = 0;
    request.config.outH = 32;
    REQUIRE(MosaicService().run(request).ok);
    CHECK(imreadUnicode(request.outputPath).cols > 1);
    request.config.usePenalty = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(MosaicService().run(request).ok);
    request.config.usePenalty = 0.01;
    Database database(request.dbPath);
    database.setMeta("feature_w", "2147483647");
    CHECK_FALSE(MosaicService().run(request).ok);
    database.setMeta("feature_w", "180broken");
    CHECK_FALSE(MosaicService().run(request).ok);
}

TEST_CASE("regression: service errors stay stable for invalid paths")
{
    TempWorkspace workspace;

    BuildRequest emptyBuild;
    emptyBuild.inputDir = utf8(workspace.emptyInputDir());
    emptyBuild.outputDir = utf8(workspace.libraryDir());
    emptyBuild.dbPath = utf8(workspace.dbPath());
    emptyBuild.forceMode = true;
    emptyBuild.allowPrompt = false;

    const auto build = BuildService().run(emptyBuild);
    CHECK_FALSE(build.ok);
    CHECK(build.message.find("no supported images found") != std::string::npos);

    MosaicRequest badMosaic;
    badMosaic.inputPath = utf8(workspace.targetPath());
    badMosaic.dbPath = utf8(workspace.dbPath());
    badMosaic.outputPath = utf8(workspace.outputDir());

    const auto mosaic = MosaicService().run(badMosaic);
    CHECK_FALSE(mosaic.ok);
    CHECK(mosaic.message.find("outputPath is a directory") != std::string::npos);
}

TEST_CASE("same-size normalization preserves source bytes and independent saved pixels")
{
    TempWorkspace workspace;
    ImageNormalizer normalizer;
    cv::Mat source(320, 180, CV_8UC3);
    cv::RNG(42).fill(source, cv::RNG::UNIFORM, 0, 256);
    for (const std::string extension : {"jpg", "jpeg", "png", "bmp", "tif", "webp"})
    {
        const auto input = utf8(workspace.inputDir() / u8path(std::string(u8"原图.") + extension));
        const auto output = utf8(workspace.outputDir() / u8path(std::string(u8"副本.") + extension));
        cv::Mat pixels = source;
        if (extension == "png")
        {
            // 高位深和透明通道属于原文件内容，匹配仍使用现有的 8 位 BGR 解码规则。
            pixels.create(320, 180, CV_16UC4);
            cv::RNG(123).fill(pixels, cv::RNG::UNIFORM, 0, 65536);
        }
        REQUIRE(imwriteUnicode(input, pixels, {cv::IMWRITE_JPEG_QUALITY, 97, cv::IMWRITE_PNG_COMPRESSION, 1,
                                               cv::IMWRITE_WEBP_QUALITY, 83}));
        const auto original = readBytesUnicode(input);
        const cv::Mat decoded = imreadUnicode(input);
        cv::Mat saved;
        REQUIRE(normalizer.process(input, output, {cv::IMWRITE_JPEG_QUALITY, 90}, &saved));
        CHECK(readBytesUnicode(output) == original);
        CHECK_FALSE(fs::equivalent(u8path(input), u8path(output)));
        CHECK(cv::norm(saved, decoded, cv::NORM_INF) == 0);
        CHECK(cv::norm(imreadUnicode(output), decoded, cv::NORM_INF) == 0);
        CHECK(cv::norm(normalizer.processToMat(input), decoded, cv::NORM_INF) == 0);
        const auto stamp = fs::file_time_type::clock::now() - std::chrono::hours(1);
        fs::last_write_time(u8path(input), stamp);
        REQUIRE(normalizer.process(input, input));
        CHECK(fs::last_write_time(u8path(input)) == stamp);
        REQUIRE(writeBytesUnicode(input, {1, 2, 3}));
        CHECK(readBytesUnicode(output) == original);
    }
}

TEST_CASE("normalization retains resized encoding conversion and failure behavior")
{
    TempWorkspace workspace;
    ImageNormalizer normalizer;
    cv::Mat source(200, 384, CV_8UC3);
    cv::RNG(17).fill(source, cv::RNG::UNIFORM, 0, 256);
    const auto input = utf8(workspace.inputDir() / "input.png");
    requireWriteImage(u8path(input), source);
    for (const std::string extension : {"jpg", "png"})
    {
        const std::vector<int> params = extension == "jpg" ? std::vector<int>{cv::IMWRITE_JPEG_QUALITY, 90}
                                                           : std::vector<int>{cv::IMWRITE_PNG_COMPRESSION, 9};
        const auto baseline = utf8(workspace.outputDir() / ("baseline." + extension));
        const auto output = utf8(workspace.outputDir() / ("output." + extension));
        const cv::Mat normalized = normalizer.centerCrop(normalizer.resizeToCover(source));
        REQUIRE(imwriteUnicode(baseline, normalized, params));
        cv::Mat saved;
        REQUIRE(normalizer.process(input, output, params, &saved));
        CHECK(readBytesUnicode(output) == readBytesUnicode(baseline));
        CHECK(cv::norm(saved, imreadUnicode(output), cv::NORM_INF) == 0);
        CHECK(cv::norm(normalizer.processToMat(input), normalized, cv::NORM_INF) == 0);
    }
    const cv::Mat exact(320, 180, CV_8UC3, cv::Scalar(20, 80, 160));
    requireWriteImage(u8path(input), exact);
    const auto converted = utf8(workspace.outputDir() / "converted.jpg");
    REQUIRE(normalizer.process(input, converted)); // 用户要求转换格式时，即使同尺寸也仍然编码。
    CHECK(readBytesUnicode(converted)[0] == 0xff);
    CHECK(readBytesUnicode(converted)[1] == 0xd8);
    const auto blocked = workspace.outputDir() / "blocked.png";
    fs::create_directory(blocked);
    cv::Mat saved = source.clone();
    CHECK_FALSE(normalizer.process(input, utf8(blocked), {}, &saved));
    CHECK(saved.empty());
    CHECK(cv::norm(imreadUnicode(input), exact, cv::NORM_INF) == 0);
    const auto invalid = utf8(workspace.inputDir() / "invalid.png");
    REQUIRE(writeBytesUnicode(invalid, {1, 2, 3}));
    CHECK_FALSE(normalizer.process(invalid, converted));
    CHECK(normalizer.processToMat(invalid).empty());
    CHECK_FALSE(normalizer.process(utf8(workspace.root / "missing.png"), converted));
}

TEST_CASE("preview accumulator bounds dimensions and preserves small images")
{
    CHECK(PreviewAccumulator::outputSize({37, 19}) == cv::Size(37, 19));
    CHECK_THROWS_AS(PreviewAccumulator::outputSize({0, 1}), std::invalid_argument);
    for (const cv::Size source : {cv::Size(74880, 141760), cv::Size(115380, 160000),
                                  cv::Size(1400, 30000), cv::Size(1, INT_MAX), cv::Size(INT_MAX, INT_MAX)})
    {
        const auto size = PreviewAccumulator::outputSize(source);
        CHECK(size.width >= 1);
        CHECK(size.height >= 1);
        CHECK(size.width <= std::min(source.width, PreviewAccumulator::kMaxWidth));
        CHECK(size.height <= std::min(source.height, PreviewAccumulator::kJpegLimit));
        CHECK(static_cast<int64_t>(size.width) * size.height <= PreviewAccumulator::kMaxPixels);
    }
    cv::Mat smallImage(19, 37, CV_8UC3);
    cv::RNG(42).fill(smallImage, cv::RNG::UNIFORM, 0, 256);
    PreviewAccumulator preview(smallImage.size());
    CHECK_THROWS_AS(preview.image(), std::logic_error);
    CHECK_THROWS_AS(preview.append(smallImage, 1), std::invalid_argument);
    preview.append(smallImage, 0);
    CHECK(cv::norm(preview.image(), smallImage, cv::NORM_INF) == 0);
    CHECK_THROWS_AS(preview.append(smallImage.row(0), smallImage.rows), std::invalid_argument);
}

TEST_CASE("preview accumulator area averages independently of strip boundaries")
{
    cv::Mat storage(703, 1709, CV_8UC3);
    cv::Mat source = storage(cv::Rect(3, 1, 1703, 701)); // 同时覆盖非连续行和非整数缩放。
    cv::RNG(123).fill(source, cv::RNG::UNIFORM, 0, 256);
    const cv::Mat original = source.clone();
    PreviewAccumulator whole(source.size()), strips(source.size());
    whole.append(source, 0);
    for (int y = 0; y < source.rows;)
    {
        const int count = std::min(1 + y % 71, source.rows - y);
        strips.append(source.rowRange(y, y + count), y);
        y += count;
    }
    cv::Mat expected;
    cv::resize(source, expected, PreviewAccumulator::outputSize(source.size()), 0, 0, cv::INTER_AREA);
    CHECK(cv::norm(whole.image(), expected, cv::NORM_INF) <= 1);
    CHECK(cv::norm(whole.image(), strips.image(), cv::NORM_INF) == 0);
    CHECK(cv::norm(source, original, cv::NORM_INF) == 0);
}

TEST_CASE("optional preview uses final pixels and agrees across formats and output modes")
{
    TempWorkspace workspace;
    cv::Mat source(11, 17, CV_8UC3), target(6, 4, CV_8UC3);
    cv::RNG(42).fill(source, cv::RNG::UNIFORM, 0, 256);
    cv::RNG(123).fill(target, cv::RNG::UNIFORM, 0, 256);
    const auto sourcePath = workspace.inputDir() / "source.png";
    requireWriteImage(sourcePath, source);
    ImageRecord record;
    record.id = 1;
    record.filePath = utf8(sourcePath);
    const std::vector<int> selected{0, 0, 0, -1, 0, 0};
    MosaicEngine::Config config;
    config.formatExplicit = true;
    config.benchmark = true;
    config.colorAdjust = true;
    config.overlayStrength = 0.25;
    auto adjust = [](cv::Mat &image, double)
    {
        cv::add(image, cv::Scalar(5, 7, 9), image);
    };
    cv::Mat referencePreview;
    for (const std::string format : {"png", "jpg", "tiff", "webp"})
    {
        for (const std::string mode : {"batch", "stream"})
        {
            config.outputFormat = format;
            config.writeMode = mode;
            config.generatePreview = false;
            OutputStats stats;
            const auto stem = std::string(u8"中文预览-") + format + "-" + mode;
            const auto path = utf8(workspace.outputDir() / u8path(stem + ".jpg"));
            const auto previewPath = workspace.outputDir() / u8path(stem + "_preview.jpg");
            REQUIRE(writeIndexedMosaicOutput(config, path, 2, 3, 851, 53, {record}, selected, adjust, stats, target));
            const cv::Mat baseline = imreadUnicode(stats.path);
            REQUIRE_FALSE(baseline.empty());
            CHECK(stats.previewPath.empty());
            CHECK(stats.previewMs == 0);
            CHECK_FALSE(fs::exists(previewPath));
            config.generatePreview = true;
            REQUIRE(writeIndexedMosaicOutput(config, path, 2, 3, 851, 53, {record}, selected, adjust, stats, target));
            CHECK(stats.previewPath == utf8(previewPath));
            CHECK(stats.previewMs > 0);
            CHECK(stats.previewMs <= stats.encodingMs);
            CHECK(u8path(stats.path).extension() == "." + format);
            CHECK(cv::norm(imreadUnicode(stats.path), baseline, cv::NORM_INF) == 0);
            const cv::Mat preview = imreadUnicode(stats.previewPath);
            REQUIRE_FALSE(preview.empty());
            CHECK(preview.size() == PreviewAccumulator::outputSize(baseline.size()));
            if (referencePreview.empty())
            {
                // 无损主图独立提供参考像素，检查合成效果进入预览且质量固定为 92。
                PreviewAccumulator expected(baseline.size());
                expected.append(baseline, 0);
                std::vector<uint8_t> encoded;
                REQUIRE(cv::imencode(".jpg", expected.image(), encoded, {cv::IMWRITE_JPEG_QUALITY, 92}));
                referencePreview = cv::imdecode(encoded, cv::IMREAD_COLOR);
            }
            CHECK(cv::norm(preview, referencePreview, cv::NORM_INF) == 0);
            config.generatePreview = false;
            const auto previewTime = fs::last_write_time(previewPath);
            REQUIRE(writeIndexedMosaicOutput(config, path, 2, 3, 851, 53, {record}, selected, adjust, stats, target));
            CHECK(fs::last_write_time(previewPath) == previewTime);
        }
    }
    CHECK(cv::norm(imreadUnicode(record.filePath), source, cv::NORM_INF) == 0);
}

TEST_CASE("preview write failures preserve the closed main image")
{
    TempWorkspace workspace;
    const cv::Mat source(4, 6, CV_8UC3, cv::Scalar(20, 80, 160));
    const auto sourcePath = workspace.inputDir() / "source.png";
    requireWriteImage(sourcePath, source);
    ImageRecord record;
    record.id = 1;
    record.filePath = utf8(sourcePath);
    MosaicEngine::Config config;
    config.outputFormat = "png";
    config.formatExplicit = true;
    config.generatePreview = true;
    const auto path = utf8(workspace.outputDir() / u8path(u8"保存失败.png"));
    const auto previewPath = workspace.outputDir() / u8path(u8"保存失败_preview.jpg");
    fs::create_directory(previewPath); // 用目录模拟预览文件无法写入。
    OutputStats stats;
    std::ostringstream errors;
    auto *previous = std::cerr.rdbuf(errors.rdbuf());
    const bool ok = writeIndexedMosaicOutput(config, path, 1, 1, 6, 4, {record}, {0}, {}, stats);
    std::cerr.rdbuf(previous);
    CHECK_FALSE(ok);
    CHECK(errors.str().find("preview write failed") != std::string::npos);
    CHECK(errors.str().find("main image saved: " + path) != std::string::npos);
    CHECK(stats.previewPath.empty());
    CHECK(cv::norm(imreadUnicode(path), source, cv::NORM_INF) == 0);
    config.generatePreview = false;
    REQUIRE(writeIndexedMosaicOutput(config, path, 1, 1, 6, 4, {record}, {0}, {}, stats));
    CHECK(fs::is_directory(previewPath));
    for (bool deepZoom : {false, true})
    {
        config.generatePreview = true;
        config.tiledOutput = !deepZoom;
        config.deepZoom = deepZoom;
        CHECK_FALSE(writeIndexedMosaicOutput(config, path, 1, 1, 6, 4, {record}, {0}, {}, stats));
    }
}

TEST_CASE("bounded cache shares immutable pixels and output modes agree")
{
    TempWorkspace workspace;
    writeLibraryImages(workspace.inputDir());
    const auto source = utf8(workspace.inputDir() / "tile_00.png");
    ImageCache cache(18 * 32 * 3);
    auto first = cache.getShared(1, source, 18, 32);
    REQUIRE(first);
    CHECK(cache.getShared(1, source, 18, 32) == first);
    cv::Mat modified = cache.getOrLoad(1, source, 18, 32);
    modified.setTo(cv::Scalar(0, 0, 0));
    CHECK(cv::norm(*first, modified, cv::NORM_INF) > 0);
    REQUIRE(cache.getShared(2, source, 18, 32));
    CHECK(cache.cachedBytes() == 18 * 32 * 3);
    REQUIRE(cache.getShared(3, source, 36, 64));
    CHECK(cache.cachedBytes() == 18 * 32 * 3);
    cache.clear();
    CHECK(cache.cachedBytes() == 0);
    CHECK_FALSE(first->empty()); // 淘汰和清空不影响使用中的共享引用。

    MosaicEngine::Config config;
    config.outputFormat = "png";
    config.formatExplicit = true;
    std::vector<ImageRecord> records(6);
    for (auto& record : records)
    {
        record.id = 1;
        record.filePath = source;
    }
    std::vector<int> selected(6, 0);
    selected[2] = -1; // 两种模式对缺失小块采用相同背景。
    OutputStats stats;
    for (const std::string format : {"png", "tiff", "jpg"})
    {
        config.outputFormat = format;
        config.writeMode = "batch";
        const auto batch = utf8(workspace.outputDir() / ("batch." + format));
        REQUIRE(writeMosaicOutput(config, batch, 2, 3, 18, 32, records, selected, {}, stats));
        CHECK(stats.failed == 1);
        config.writeMode = "stream";
        const auto stream = utf8(workspace.outputDir() / ("stream." + format));
        REQUIRE(writeMosaicOutput(config, stream, 2, 3, 18, 32, records, selected, {}, stats));
        CHECK(stats.failed == 1);
        CHECK(cv::norm(imreadUnicode(batch), imreadUnicode(stream), cv::NORM_INF) == 0);
        const auto indexed = utf8(workspace.outputDir() / ("indexed." + format));
        REQUIRE(writeIndexedMosaicOutput(config, indexed, 2, 3, 18, 32, {records[0]}, selected, {}, stats));
        CHECK(stats.failed == 1);
        CHECK(cv::norm(imreadUnicode(stream), imreadUnicode(indexed), cv::NORM_INF) == 0);
    }
    CHECK_FALSE(writeMosaicOutput(config, utf8(workspace.root / "missing" / "out.png"),
        2, 3, 18, 32, records, selected, {}, stats));

    config.colorAdjust = true;
    config.outputFormat = "png";
    auto adjust = [](cv::Mat &image, double strength)
    {
        // 每次使用同一测试种子，使输出模式比较不依赖线程调度。
        std::mt19937 rng(123);
        detail::adjustBrightness(image, strength, rng);
    };
    const cv::Mat sourcePixels = imreadUnicode(source);
    cv::Mat expected = first->clone();
    adjust(expected, config.colorStrength);
    cv::Mat reference;
    const std::vector<ImageRecord> library(1, records[0]);
    const char *modes[] =
    {
        "batch", "stream"
    };
    for (const std::string mode : modes)
    {
        config.writeMode = mode;
        const auto path = utf8(workspace.outputDir() / ("adjusted-" + mode + ".png"));
        REQUIRE(writeIndexedMosaicOutput(config, path, 2, 3, 18, 32, library, selected, adjust, stats));
        const cv::Mat pixels = imreadUnicode(path);
        REQUIRE_FALSE(pixels.empty());
        for (size_t i = 0; i < selected.size(); ++i)
        {
            if (selected[i] >= 0)
            {
                const int x = static_cast<int>(i % 2) * 18, y = static_cast<int>(i / 2) * 32;
                CHECK(cv::norm(pixels(cv::Rect(x, y, 18, 32)), expected, cv::NORM_INF) == 0);
            }
        }
        if (reference.empty())
        {
            reference = pixels;
        }
        else
        {
            CHECK(cv::norm(pixels, reference, cv::NORM_INF) == 0);
        }
    }
    CHECK(cv::norm(imreadUnicode(source), sourcePixels, cv::NORM_INF) == 0);
}

TEST_CASE("overlay samples global pixel centers across tile boundaries and clamps edges")
{
    cv::Mat storage(4, 6, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Mat target = storage(cv::Rect(1, 1, 4, 2)); // 非连续参考图也必须按行读取。
    for (int y = 0; y < target.rows; ++y)
    {
        for (int x = 0; x < target.cols; ++x)
        {
            target.at<cv::Vec3b>(y, x) = cv::Vec3b(x * 80, y * 32, 64);
        }
    }
    TileCompositor sampler(target, 2, 1, cv::Size(4, 2));
    cv::Mat left, right;
    sampler.sample(0, left);
    sampler.sample(1, right);
    const int expected[] = {0, 20, 60, 100, 140, 180, 220, 240};
    for (int x = 0; x < 8; ++x)
    {
        CHECK((x < 4 ? left : right).at<cv::Vec3b>(0, x % 4)[0] == expected[x]);
    }
    CHECK(right.at<cv::Vec3b>(1, 3) == cv::Vec3b(240, 32, 64));
    TileCompositor downsample(target, 2, 1, cv::Size(1, 1));
    downsample.sample(1, right);
    CHECK(right.at<cv::Vec3b>(0, 0) == cv::Vec3b(200, 16, 64));
    CHECK_THROWS_AS(sampler.sample(2, right), std::out_of_range);
    CHECK_THROWS_AS(TileCompositor(cv::Mat(), 2, 1, cv::Size(4, 2)), std::invalid_argument);
}

TEST_CASE("overlay preserves cache pixels adjustment order and all output paths")
{
    TempWorkspace workspace;
    const cv::Mat source(4, 6, CV_8UC3, cv::Scalar(128, 96, 64));
    const auto sourcePath = workspace.inputDir() / "source.png";
    requireWriteImage(sourcePath, source);
    ImageRecord record;
    record.id = 1;
    record.filePath = utf8(sourcePath);
    std::vector<int> selected{0, 0, 0, -1};
    cv::Mat target(4, 4, CV_8UC3);
    for (int y = 0; y < 4; ++y)
    {
        for (int x = 0; x < 4; ++x)
        {
            target.at<cv::Vec3b>(y, x) = cv::Vec3b(x * 64, y * 64, 64);
        }
    }
    cv::Mat enlarged;
    cv::resize(target, enlarged, cv::Size(12, 8), 0, 0, cv::INTER_LINEAR);
    MosaicEngine::Config config;
    config.outputFormat = "png";
    config.formatExplicit = true;
    config.benchmark = true;
    OutputStats stats;
    auto adjust = [](cv::Mat &image, double)
    {
        image.setTo(cv::Scalar(64, 128, 192));
    };
    for (bool brightness : {false, true})
    {
        config.colorAdjust = brightness;
        for (double strength : {0.0, 0.25, 1.0})
        {
            config.overlayStrength = strength;
            cv::Mat material(8, 12, CV_8UC3, brightness ? cv::Scalar(64, 128, 192) : cv::Scalar(128, 96, 64));
            cv::Mat expected;
            cv::addWeighted(material, 1 - strength, enlarged, strength, 0, expected);
            expected(cv::Rect(6, 4, 6, 4)).setTo(cv::Scalar(64, 64, 64));
            cv::Mat reference;
            for (const std::string mode : {"batch", "stream"})
            {
                config.writeMode = mode;
                const auto path = utf8(workspace.outputDir() / (mode + ".png"));
                REQUIRE(writeIndexedMosaicOutput(config, path, 2, 2, 6, 4, {record}, selected, adjust, stats,
                                                strength == 0 ? cv::Mat() : target));
                const auto pixels = imreadUnicode(path);
                CHECK(cv::norm(pixels, expected, cv::NORM_INF) <= 1);
                CHECK(stats.failed == 1);
                CHECK(stats.overlayReferenceBytes == (strength == 0 ? 0 : 48));
                CHECK(stats.overlayWorkingBytes <= 8 * TileCompositor::workingBytes(cv::Size(6, 4)));
                if (reference.empty())
                {
                    reference = pixels;
                }
                else
                {
                    CHECK(cv::norm(pixels, reference, cv::NORM_INF) == 0);
                }
            }
        }
    }
    CHECK(cv::norm(imreadUnicode(record.filePath), source, cv::NORM_INF) == 0);
    config.overlayStrength = 1;
    CHECK_FALSE(writeIndexedMosaicOutput(config, utf8(workspace.mosaicPath()), 2, 2, 6, 4,
                                        {record}, selected, adjust, stats));
    selected.assign(4, 0);
    config.colorAdjust = false;
    config.tiledOutput = true;
    auto jpeg = [&](const cv::Mat &image)
    {
        std::vector<uchar> bytes;
        REQUIRE(cv::imencode(".jpg", image, bytes, {cv::IMWRITE_JPEG_QUALITY, config.jpegQuality}));
        return cv::imdecode(bytes, cv::IMREAD_COLOR);
    };
    cv::Mat encodedTiles(8, 12, CV_8UC3);
    for (int i = 0; i < 4; ++i)
    {
        const cv::Rect region((i % 2) * 6, (i / 2) * 4, 6, 4);
        jpeg(enlarged(region)).copyTo(encodedTiles(region));
    }
    for (bool deepZoom : {false, true})
    {
        config.deepZoom = deepZoom;
        const auto path = utf8(workspace.outputDir() / (deepZoom ? "zoom" : "tiles"));
        REQUIRE(writeIndexedMosaicOutput(config, path, 2, 2, 6, 4, {record}, selected, {}, stats, target));
        CHECK(stats.failed == 0);
        const auto imagePath = deepZoom ? path + "_files/4/0_0.jpg" : path + "_files/0/1_0.jpg";
        const auto pixels = imreadUnicode(imagePath);
        // 分块先编码 JPEG，金字塔最高层再编码；对照相同编码步骤，不能拿原像素限制色度采样误差。
        const cv::Mat expected = deepZoom ? jpeg(encodedTiles) : encodedTiles(cv::Rect(6, 0, 6, 4));
        REQUIRE(pixels.size() == expected.size());
        CHECK(cv::norm(pixels, expected, cv::NORM_INF) <= 4);
        if (deepZoom)
        {
            CHECK(fs::exists(u8path(path + ".dzi")));
        }
    }
}

TEST_CASE("overlay follows resize upscale and reflected padding without changing selections")
{
    TempWorkspace workspace;
    writeLibraryImages(workspace.inputDir());
    writeTargetImage(workspace.targetPath());
    auto build = makeBuildRequest(workspace);
    // 使用 CUDA 支持的特征尺寸，避免 GPU 分支只验证到 CPU 回退。
    build.normalizeWidth = 180;
    build.normalizeHeight = 320;
    REQUIRE(BuildService().run(build).ok);
    auto request = makeMosaicRequest(workspace);
    auto &config = request.config;
    config.outW = 11;
    config.outH = 13;
    config.upscale = 2;
    config.tileW = config.tileH = 4;
    config.nativeTileW = config.nativeTileH = 6;
    config.lRange = 255;
    cv::Mat target, resized, padded, expected;
    cv::resize(imreadUnicode(request.inputPath), resized, cv::Size(11, 13), 0, 0, cv::INTER_AREA);
    cv::resize(resized, target, cv::Size(22, 26), 0, 0, cv::INTER_LINEAR);
    cv::copyMakeBorder(target, padded, 0, 2, 0, 2, cv::BORDER_REFLECT);
    cv::resize(padded, expected, cv::Size(36, 42), 0, 0, cv::INTER_LINEAR);
    for (bool gpu : {false, true})
    {
        if (gpu && !cuda::isCudaAvailable())
        {
            continue;
        }
        config.useGpu = gpu;
        std::vector<int> baseline;
        for (double strength : {0.0, 1.0})
        {
            config.overlayStrength = strength;
            MosaicEngine engine;
            MatchingAccess::configure(engine, MatchingLimits{}, true, 123);
            std::vector<int> selected;
            MatchingAccess::captureSelections(engine, selected);
            REQUIRE(engine.generate(request.inputPath, request.dbPath, request.outputPath, config));
            if (strength == 0)
            {
                baseline = selected;
            }
            else
            {
                CHECK(selected == baseline);
                CHECK(cv::norm(imreadUnicode(request.outputPath), expected, cv::NORM_INF) <= 1);
            }
        }
    }
}

TEST_CASE("worst tile snapshots retain bounded details and original tie order")
{
    WorstTiles tiles;
    std::array<float, 192> grid{};
    for (int i = 0; i < 25; ++i)
    {
        grid.fill(static_cast<float>(i));
        tiles.remember(1.0, i, i * 10, i, 2, 3, 0.1, grid);
    }
    const auto ranks = tiles.ranks();
    REQUIRE(ranks.size() == 20);
    CHECK(ranks.front().second == 24);
    CHECK(ranks.back().second == 5);
    CHECK(tiles.at(24).tile == 240);
    CHECK(tiles.at(24).grid[191] == 24);
    CHECK_THROWS_AS(tiles.at(0), std::out_of_range);
}

TEST_CASE("bounded grid comparisons preserve thresholds and ordered selections")
{
    std::vector<ImageRecord> records(80);
    for (int i = 0; i < 80; ++i)
    {
        records[i].gridLab.assign(192, static_cast<float>(i / 2) * 0.1f);
    }
    records[77].gridLab.clear();
    records[78].gridLab[0] = std::numeric_limits<float>::infinity();
    records[78].gridLab[191] = std::numeric_limits<float>::quiet_NaN();
    records[79].gridLab[191] = std::numeric_limits<float>::quiet_NaN();
    GridDuplicateCache cache(records);
    const double boundary = gridDistance8x8(records[0].gridLab, records[2].gridLab);
    for (double threshold : {0.010, boundary, std::nextafter(boundary, 0.0),
                             std::nextafter(boundary, 1.0)})
    {
        for (int a = 0; a < 80; ++a)
        {
            for (int b = 0; b < 80; ++b)
            {
                CHECK(cache.similar(a, b, threshold) ==
                      (gridDistance8x8(records[a].gridLab, records[b].gridLab) < threshold));
            }
        }
    }
    for (int window : {1, 3, 50})
    {
        std::deque<int> recent;
        for (int tile = 0; tile < 90; ++tile)
        {
            std::vector<std::pair<double, int>> reference, actual;
            for (int i = 0; i < 77; ++i)
            {
                double oldScore = static_cast<double>(i % 7), newScore = oldScore;
                bool oldDuplicate = false, newDuplicate = false;
                for (int r : recent)
                {
                    oldDuplicate |= gridDistance8x8(records[i].gridLab, records[r].gridLab) < 0.010;
                    newDuplicate |= cache.similar(i, r, 0.010);
                }
                reference.emplace_back(oldScore + (oldDuplicate ? 200 : 0), i);
                actual.emplace_back(newScore + (newDuplicate ? 200 : 0), i);
            }
            std::sort(reference.begin(), reference.end());
            const int n = tile % 10 + 1;
            std::partial_sort(actual.begin(), actual.begin() + n, actual.end());
            CHECK(std::equal(actual.begin(), actual.begin() + n, reference.begin()));
            recent.push_back(actual[tile % n].second);
            if (static_cast<int>(recent.size()) > window)
            {
                recent.pop_front();
            }
        }
    }
}

TEST_CASE("usage cache releases final references and shares loading failures")
{
    ImageCache cache(1024, true);
    cache.setUseCounts({{1, 1}, {2, 2}, {3, 2}});
    auto pixels = []()
    {
        return cv::Mat(8, 8, CV_8UC3, cv::Scalar(1, 2, 3));
    };
    REQUIRE(cache.getSharedWithLoader(1, 8, 8, pixels));
    CHECK(cache.cachedBytes() == 0);
    auto first = cache.getSharedWithLoader(2, 8, 8, pixels);
    REQUIRE(first);
    cache.finishUse(2, 8, 8);
    CHECK(cache.getSharedWithLoader(2, 8, 8, pixels) == first);
    cache.finishUse(2, 8, 8);
    CHECK(cache.cachedBytes() == 0);
    CHECK(first->at<cv::Vec3b>(0, 0)[0] == 1);
    for (bool fail : {false, true})
    {
        std::promise<void> started, release;
        auto released = release.get_future().share();
        auto owner = std::async(std::launch::async, [&]()
        {
            return cache.getSharedWithLoader(3, 8, 8, [&]() -> cv::Mat
            {
                started.set_value();
                released.wait();
                if (fail)
                {
                    throw std::runtime_error("decode failure");
                }
                return pixels();
            });
        });
        started.get_future().wait();
        const auto shares = cache.stats().sharedLoads;
        auto waiter = std::async(std::launch::async, [&]()
        {
            return cache.getSharedWithLoader(3, 8, 8, pixels);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (cache.stats().sharedLoads == shares && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::yield();
        }
        release.set_value();
        CHECK(cache.stats().sharedLoads == shares + 1);
        if (fail)
        {
            CHECK_THROWS_AS(owner.get(), std::runtime_error);
            CHECK_THROWS_AS(waiter.get(), std::runtime_error);
        }
        else
        {
            CHECK(owner.get() == waiter.get());
        }
        cache.clear();
    }
}

TEST_CASE("feature generations validate sources legacy caches corruption and publication failure")
{
    TempWorkspace workspace;
    const auto folder = workspace.inputDir() / u8path("中文特征");
    fs::create_directory(folder);
    const auto dir = utf8(folder);
    std::vector<ImageRecord> records(513);
    for (int i = 0; i < static_cast<int>(records.size()); ++i)
    {
        records[i].id = i * 7 + 1;
        records[i].featureVersion = FeatureExtractor::featureVersion();
        records[i].fileHash = "image-" + std::to_string(i);
        records[i].tinyPath = dir + "/" + std::to_string(i) + ".tiny";
        records[i].lbpPath = dir + "/" + std::to_string(i) + ".hist";
        std::vector<uint8_t> tiny(256, static_cast<uint8_t>(i + 1));
        std::vector<float> lbp(256, 0.25f);
        writeFeature(records[i].tinyPath, FeatureKind::Tiny, tiny.data());
        writeFeature(records[i].lbpPath, FeatureKind::Lbp, lbp.data());
    }
    const auto manifestPath = folder / "features.pack";
    auto publishedPath = [&](const char *suffix)
    {
        std::ifstream manifest(manifestPath);
        std::string magic, generation;
        int version;
        int libraryVersion, featureVersion, gridDimension, tinyDimension, lbpDimension;
        manifest >> magic >> version >> libraryVersion >> featureVersion >> gridDimension >> tinyDimension >> lbpDimension >> generation;
        return folder / ("features-" + generation + suffix);
    };
    // Unverified v2 bytes cannot override refreshed source files with the same IDs.
    {
        uint32_t count = static_cast<uint32_t>(records.size());
        std::ofstream old(folder / "tiny.bin", std::ios::binary);
        old.write(reinterpret_cast<const char *>(&count), 4);
        for (const auto &record : records)
        {
            std::array<uint8_t, 256> zero{};
            old.write(reinterpret_cast<const char *>(&record.id), 4);
            old.write(reinterpret_cast<const char *>(zero.data()), 256);
        }
    }
    const std::vector<int> needed{2, 0, 2};
    CandidateFeatures legacy(records, needed, dir);
    CHECK(legacy.tiny(2)[0] == 3);
    FeaturePack::Reader legacyReader(dir, records);
    CHECK_FALSE(legacyReader.stats().preloaded);
    CHECK(legacyReader.get(2).tiny[0] == 3);
    REQUIRE(FeaturePack::buildCache(dir, records));
    const auto oldGeneration = publishedPath(".tiny.bin");
    const auto manifestTime = fs::last_write_time(manifestPath);
    FeaturePack::Stats cacheStats;
    REQUIRE(FeaturePack::buildCache(dir, records, &cacheStats));
    CHECK(cacheStats.reused);
    CHECK(fs::last_write_time(manifestPath) == manifestTime);
    CHECK(publishedPath(".tiny.bin") == oldGeneration);
    CandidateFeatures cached(records, needed, dir);
    CHECK(cached.hasTiny(2));
    CHECK(cached.lbp(0)[0] == 0.25f);
    FeaturePack::Reader preloaded(dir, records);
    REQUIRE(preloaded.stats().preloaded);
    preloaded.prepare(needed);
    CHECK(preloaded.get(2).tiny[0] == cached.tiny(2)[0]);
    CHECK(preloaded.get(0).lbp[0] == cached.lbp(0)[0]);
    CHECK(preloaded.stats().sourceReads == 0);
    FeaturePack::Reader limited(dir, records, 2 * sizeof(FeaturePack::Reader::Row));
    CHECK_FALSE(limited.stats().preloaded);
    CHECK(limited.get(2).tiny[0] == 3);
    CHECK(limited.get(0).tiny[0] == 1);
    limited.prepare({0, 1});
    CHECK(limited.get(0).tiny[0] == 1);
    CHECK(limited.get(1).tiny[0] == 2);
    CHECK(limited.stats().evictions == 1);
    CHECK(limited.stats().sourceReads == 0);
    CHECK(limited.stats().peakBytes <= 2 * sizeof(FeaturePack::Reader::Row));
    {
        std::fstream pack(oldGeneration, std::ios::binary | std::ios::in | std::ios::out);
        pack.seekp(8);
        pack.put(99);
    }
    CandidateFeatures corrupt(records, needed, dir);
    CHECK(corrupt.tiny(0)[0] == 1);
    FeaturePack::Reader corruptReader(dir, records);
    CHECK(corruptReader.get(0).tiny[0] == 1);
    CHECK(corruptReader.stats().sourceReads == 2);
    {
        std::vector<uint8_t> changed(256, 42);
        writeFeature(records[2].tinyPath, FeatureKind::Tiny, changed.data());
    }
    ++records[2].featureVersion;
    REQUIRE(FeaturePack::buildCache(dir, records));
    CandidateFeatures refreshed(records, needed, dir);
    CHECK(refreshed.tiny(2)[0] == 42);
    CHECK(fs::exists(oldGeneration));
    FeaturePack::Reader beforeChange(dir, records);
    REQUIRE(beforeChange.stats().preloaded);
    const auto sourceTime = fs::last_write_time(u8path(records[1].tinyPath));
    {
        std::vector<uint8_t> changed(256, 99);
        writeFeature(records[1].tinyPath, FeatureKind::Tiny, changed.data());
    }
    fs::last_write_time(u8path(records[1].tinyPath), sourceTime + std::chrono::seconds(1));
    beforeChange.prepare({1});
    CHECK(beforeChange.get(1).tiny[0] == 99);
    CHECK(beforeChange.stats().sourceReads == 2);
#ifdef _WIN32
    const auto generationBefore = publishedPath(".tiny.bin");
    ++records[0].featureVersion;
    HANDLE blocker = CreateFileW(manifestPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(blocker != INVALID_HANDLE_VALUE);
    const bool published = FeaturePack::buildCache(dir, records);
    CloseHandle(blocker);
    CHECK_FALSE(published);
    CHECK(publishedPath(".tiny.bin") == generationBefore);
#endif
    // A complete but unpublished generation is ignored after an interrupted writer.
    std::ofstream interrupted(folder / "features-123-456.tiny.bin");
    interrupted << "partial";
    interrupted.close();
    fs::remove(u8path(records[0].lbpPath));
    fs::resize_file(u8path(records[0].tinyPath), 5);
    CHECK_THROWS_AS(CandidateFeatures(records, needed, dir), std::runtime_error);
    FeaturePack::Reader missingReader(dir, records);
    CHECK_THROWS_AS(missingReader.get(0), std::runtime_error);
    fs::resize_file(publishedPath(".lbp.bin"), 5);
    CHECK_THROWS_AS(CandidateFeatures(records, needed, dir), std::runtime_error);
}

TEST_CASE("CPU build rolls back a failing ordered batch")
{
    TempWorkspace workspace;
    writeLibraryImages(workspace.inputDir());
    fs::create_directories(workspace.libraryDir());
    {
        Database database(utf8(workspace.dbPath()));
        REQUIRE(database.createTables());
    }
    sqlite3 *connection = nullptr;
    REQUIRE(sqlite3_open(utf8(workspace.dbPath()).c_str(), &connection) == SQLITE_OK);
    const int triggerResult =
        sqlite3_exec(connection,
                     "CREATE TRIGGER fail_second BEFORE INSERT ON images "
                     "WHEN NEW.file_path LIKE '%000001%' BEGIN SELECT RAISE(ABORT,'batch fault'); END;",
                     nullptr, nullptr, nullptr);
    sqlite3_close(connection);
    REQUIRE(triggerResult == SQLITE_OK);
    auto request = makeBuildRequest(workspace);
    request.normalizeWidth = 72;
    request.normalizeHeight = 128; // 自定义尺寸保留顺序事务，CUDA 提取也必须整批回滚。
    request.threads = 4;
    CHECK_FALSE(BuildService().run(request).ok);
    Database database(utf8(workspace.dbPath()), Database::OpenMode::Existing);
    REQUIRE(database.isOpen());
    CHECK(database.totalCount() == 0);
}

TEST_CASE("output reports incomplete PNG and DeepZoom while empty tiles remain load failures")
{
    TempWorkspace workspace;
    std::ofstream empty(workspace.inputDir() / "empty.jpg");
    empty.close();
    CHECK(imreadUnicode(utf8(workspace.inputDir() / "empty.jpg")).empty());
    cv::Mat image(32, 18, CV_8UC3, cv::Scalar(30, 100, 180));
    requireWriteImage(workspace.inputDir() / "tile.png", image);
    ImageRecord record;
    record.id = 1;
    record.filePath = utf8(workspace.inputDir() / "tile.png");
    auto prefix = utf8(workspace.outputDir() / "pyramid");
    fs::create_directories(u8path(prefix + ".dzi"));
    MosaicEngine::Config config;
    config.deepZoom = config.tiledOutput = true;
    OutputStats stats;
    CHECK_FALSE(writeMosaicOutput(config, prefix, 1, 1, 18, 32, {record}, {0}, {}, stats));
    CHECK(fs::exists(u8path(prefix + "_files/_source_tiles/0_0.jpg")));
}

TEST_CASE("matching batches preserve seeded choices, padding and empty batches")
{
    const MatchingLimits limits;
    WorkerPool prefetch(1);
    std::atomic<bool> entered{false}, release{false};
    prefetch.start(1,
                   [&](size_t)
                   {
                       entered = true;
                       while (!release.load())
                       {
                           std::this_thread::yield();
                       }
                   });
    while (!entered.load())
    {
        std::this_thread::yield();
    }
    CHECK_THROWS_AS(prefetch.start(0,
                                   [](size_t)
                                   {
                                   }),
                    std::logic_error);
    release = true;
    prefetch.wait();
    prefetch.start(1,
                   [](size_t)
                   {
                       throw std::runtime_error("prefetch fault");
                   });
    CHECK_THROWS_AS(prefetch.wait(), std::runtime_error);
    prefetch.run(1,
                 [&](size_t)
                 {
                     entered = false;
                 });
    CHECK_FALSE(entered.load());
    CHECK(matchingBatchRows(limits, 180, 320, 150, 100, true) == 1024);
    MatchingBuffers gpuBuffers;
    gpuBuffers.resize(1024, 180 * 320 * 3, 150, true, true);
    CHECK(gpuBuffers.capacityBytes() <= gpuMatchingBufferBytes(1024, 180 * 320 * 3, 150, true));
    CHECK(2 * gpuMatchingBufferBytes(1024, 180 * 320 * 3, 150, true) < limits.hostBytes);
    CHECK(2 * gpuMatchingBufferBytes(256, 180 * 320 * 3, 150, true) > 64 * 1024 * 1024);
    CHECK(matchingBatchRows(MatchingLimits{1, 1, 1024}, 180, 320, 150, 100, false) == 1);
    CHECK(matchingBatchRows(limits, 180, 320, 10000, 1000000, false) < 1024);
    CHECK_THROWS_AS(checkedMatchProduct(std::numeric_limits<size_t>::max(), 2), std::overflow_error);
    FeatureMatrix<float, 192> matrix(10);
    auto *storage = matrix.data();
    matrix.resize(1);
    CHECK(matrix.data() == storage);
    CHECK_THROWS_AS(matrix.resize(std::numeric_limits<size_t>::max()), std::overflow_error);

    TempWorkspace workspace;
    writeLibraryImages(workspace.inputDir());
    writeTargetImage(workspace.targetPath());
    auto build = makeBuildRequest(workspace);
    build.normalizeWidth = 180;
    build.normalizeHeight = 320;
    REQUIRE(BuildService().run(build).ok);
    auto request = makeMosaicRequest(workspace);
    request.config.outW = 41;
    request.config.outH = 73;
    request.config.topNRandom = 3;
    request.config.lRange = 255;
    request.config.adaptiveWeights = true;
    request.config.analyze = true;
    request.config.overlayStrength = 0.35;
    for (bool gpu : {false, true})
    {
        if (gpu && !cuda::isCudaAvailable())
        {
            continue;
        }
        request.config.useGpu = gpu;
        std::vector<int> reference;
        cv::Mat expected;
        std::string expectedBytes;
        const std::pair<size_t, bool> profiles[] = {{1, false}, {3, false}, {1, true}, {3, true}};
        for (const auto &profile : profiles)
        {
            const auto batch = profile.first;
            const bool pipeline = profile.second;
            if (!gpu && pipeline)
            {
                continue;
            }
            MosaicEngine engine;
            MatchingAccess::configure(engine, MatchingLimits{limits.hostBytes, limits.featureBytes, batch, pipeline},
                                       true, 123);
            std::vector<int> selected;
            MatchingAccess::captureSelections(engine, selected);
            RuntimeContext runtime(batch == 1);
            REQUIRE(engine.generate(request.inputPath, request.dbPath, request.outputPath, request.config, runtime));
            CHECK(runtime.snapshot().warnings.count("feature_missing") == 0);
            const auto pixels = imreadUnicode(request.outputPath);
            std::ifstream encoded(u8path(request.outputPath), std::ios::binary);
            const std::string bytes(std::istreambuf_iterator<char>(encoded), {});
            REQUIRE(selected.size() == 25);
            if (batch == 1 && !pipeline)
            {
                reference = selected;
                expected = pixels;
                expectedBytes = bytes;
            }
            else
            {
                CHECK(selected == reference);
                CHECK(cv::norm(pixels, expected, cv::NORM_INF) == 0);
                CHECK(bytes == expectedBytes);
            }
        }
    }
    // 首批亮度不匹配，后续批次仍须继续；全程无候选则明确失败。
    cv::Mat partial(64, 36, CV_8UC3, cv::Scalar(0, 0, 0));
    partial(cv::Rect(18, 0, 18, 64)).setTo(cv::Scalar(119, 119, 119));
    requireWriteImage(workspace.targetPath(), partial);
    cv::Mat lab;
    cv::cvtColor(partial(cv::Rect(18, 0, 18, 64)), lab, cv::COLOR_BGR2Lab);
    sqlite3 *connection = nullptr;
    REQUIRE(sqlite3_open(build.dbPath.c_str(), &connection) == SQLITE_OK);
    const auto sql = "UPDATE images SET avg_l=" + std::to_string(cv::mean(lab)[0]);
    const auto changed = sqlite3_exec(connection, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(connection);
    REQUIRE(changed == SQLITE_OK);
    request.config.outW = 36;
    request.config.outH = 64;
    request.config.lRange = 1; // CUDA 与 OpenCV 的灰阶 LAB 量化有原有差异。
    request.config.analyze = false;
    request.config.topNRandom = 1;
    for (bool gpu : {false, true})
    {
        if (gpu && !cuda::isCudaAvailable())
        {
            continue;
        }
        request.config.useGpu = gpu;
        MosaicEngine engine;
        MatchingAccess::configure(engine, MatchingLimits{limits.hostBytes, limits.featureBytes, 1});
        std::vector<int> selected;
        MatchingAccess::captureSelections(engine, selected);
        REQUIRE(engine.generate(request.inputPath, request.dbPath, request.outputPath, request.config));
        CHECK(selected.front() == -1);
        CHECK(selected.back() >= 0);
        partial.setTo(cv::Scalar(0, 0, 0));
        requireWriteImage(workspace.targetPath(), partial);
        CHECK_FALSE(engine.generate(request.inputPath, request.dbPath, request.outputPath, request.config));
        partial(cv::Rect(18, 0, 18, 64)).setTo(cv::Scalar(119, 119, 119));
        requireWriteImage(workspace.targetPath(), partial);
    }
}

TEST_CASE("PNG multirow writing preserves refresh boundaries and encoded bytes")
{
    TempWorkspace workspace;
    cv::Mat rgb(2017, 37, CV_8UC3);
    cv::randu(rgb, 0, 255);
    const auto oldPath = utf8(workspace.outputDir() / "rows.png");
    const auto newPath = utf8(workspace.outputDir() / "blocks.png");
    PngStreamWriter rows(oldPath, rgb.cols, rgb.rows);
    PngStreamWriter blocks(newPath, rgb.cols, rgb.rows);
    for (int y = 0; y < rgb.rows; ++y)
    {
        REQUIRE(rows.writeRow(rgb.ptr<uint8_t>(y)));
    }
    for (int y = 0; y < rgb.rows; y += 64)
    {
        REQUIRE(blocks.writeRows(rgb.ptr<uint8_t>(y), rgb.step, std::min(64, rgb.rows - y)));
    }
    REQUIRE(rows.close());
    REQUIRE(blocks.close());
    PngStreamWriter partial(utf8(workspace.outputDir() / "partial.png"), rgb.cols, rgb.rows);
    REQUIRE(partial.writeRows(rgb.ptr<uint8_t>(), rgb.step, rgb.rows - 1));
    CHECK_FALSE(partial.close());
    CHECK_FALSE(partial.close());
    std::ifstream a(u8path(oldPath), std::ios::binary), b(u8path(newPath), std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(a), {}) == std::string(std::istreambuf_iterator<char>(b), {}));
}
