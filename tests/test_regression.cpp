#include "../core/AnalysisTiles.h"
#include "../core/CandidateFeatures.h"
#include "../core/GridDuplicateCache.h"
#include "../core/ImageCache.h"
#include "../core/MosaicOutput.h"
#include "../core/PngStreamWriter.h"
#include <deque>
#include <limits>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../core/BuildService.h"
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
    request.config.topNrandom = 1;
    return request;
}

} // namespace

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

TEST_CASE("color cache migrates legacy tables and retains fractional LAB sources")
{
    TempWorkspace workspace;
    fs::create_directories(workspace.libraryDir());
    sqlite3* legacy = nullptr;
    REQUIRE(sqlite3_open(utf8(workspace.dbPath()).c_str(), &legacy) == SQLITE_OK);
    const int created = sqlite3_exec(legacy,
        "CREATE TABLE image_color_order(image_id INTEGER PRIMARY KEY REFERENCES images(id) ON DELETE CASCADE,"
        "file_hash TEXT NOT NULL,version INTEGER NOT NULL,sort_key INTEGER NOT NULL);"
        "INSERT INTO image_color_order VALUES(1,'legacy',1,0)", nullptr, nullptr, nullptr);
    sqlite3_close(legacy);
    REQUIRE(created == SQLITE_OK);
    Database database(utf8(workspace.dbPath()), Database::OpenMode::Build);
    REQUIRE(database.createTables());
    const auto old = database.imageColorOrders().at(1);
    CHECK(old.version == 1);
    CHECK(std::isnan(old.sourceL));
    ImageRecord record;
    record.filePath = "fixture.png";
    record.fileHash = "legacy";
    REQUIRE(database.insertImage(record));
    ImageColorOrder color
    {
        record.fileHash, kColorOrderVersion, meanLabColorOrderKey(100.25, 128.6, 128.2),
        100.25, 128.6, 128.2
    };
    REQUIRE(database.saveImageColorOrder(1, color));
    const auto cached = database.imageColorOrders().at(1);
    CHECK(cached.version == 2);
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
    SUBCASE("relative paths use the original working directory")
    {
        Database database(request.dbPath, Database::OpenMode::Build);
        journal["working_directory"] = utf8(workspace.root);
        for (size_t i = 0; i < 2; ++i)
        {
            const auto relative = utf8(fs::relative(u8path(records[i].filePath), workspace.root));
            REQUIRE(database.updateImagePath(records[i].id, records[i].filePath, relative));
            journal["entries"][i]["old_db_path"] = relative;
            records[i].filePath = relative;
        }
        const auto& entry = journal["entries"][0];
        fs::rename(u8path(entry["original"].get<std::string>()), u8path(entry["temporary"].get<std::string>()));
    }
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
        output << journal;
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
    SUBCASE("v1 cache upgrades without modifying matching features")
    {
        auto cached = database.imageColorOrders().at(records[0].id);
        cached.version = 1;
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
            CHECK(found->histPath == before.histPath);
            CHECK(found->grid4x4 == before.grid4x4);
            CHECK(found->useCount == before.useCount);
            CHECK(fs::exists(u8path(found->tinyPath)));
            CHECK(fs::exists(u8path(found->histPath)));
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
        records[i].grid4x4.assign(192, static_cast<float>(i / 2) * 0.1f);
    }
    records[77].grid4x4.clear();
    records[78].grid4x4[0] = std::numeric_limits<float>::infinity();
    records[78].grid4x4[191] = std::numeric_limits<float>::quiet_NaN();
    records[79].grid4x4[191] = std::numeric_limits<float>::quiet_NaN();
    GridDuplicateCache cache(records);
    const double boundary = gridDistance8x8(records[0].grid4x4, records[2].grid4x4);
    for (double threshold : {0.010, boundary, std::nextafter(boundary, 0.0),
                             std::nextafter(boundary, 1.0)})
    {
        for (int a = 0; a < 80; ++a)
        {
            for (int b = 0; b < 80; ++b)
            {
                CHECK(cache.similar(a, b, threshold) ==
                      (gridDistance8x8(records[a].grid4x4, records[b].grid4x4) < threshold));
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
                    oldDuplicate |= gridDistance8x8(records[i].grid4x4, records[r].grid4x4) < 0.010;
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
        records[i].featureVersion = 6;
        records[i].fileHash = "image-" + std::to_string(i);
        records[i].tinyPath = dir + "/" + std::to_string(i) + ".tiny";
        records[i].histPath = dir + "/" + std::to_string(i) + ".hist";
        std::vector<uint8_t> tiny(256, static_cast<uint8_t>(i + 1));
        std::vector<float> lbp(256, 0.25f);
        std::ofstream t(u8path(records[i].tinyPath), std::ios::binary);
        t.write(reinterpret_cast<const char *>(tiny.data()), 256);
        std::ofstream l(u8path(records[i].histPath), std::ios::binary);
        l.write(reinterpret_cast<const char *>(lbp.data()), 1024);
    }
    const auto manifestPath = folder / "features.pack";
    auto publishedPath = [&](const char *suffix)
    {
        std::ifstream manifest(manifestPath);
        std::string magic, generation;
        int version;
        manifest >> magic >> version >> generation;
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
        std::ofstream file(u8path(records[2].tinyPath), std::ios::binary);
        file.write(reinterpret_cast<const char *>(changed.data()), 256);
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
        std::ofstream file(u8path(records[1].tinyPath), std::ios::binary);
        file.write(reinterpret_cast<const char *>(changed.data()), 256);
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
    fs::remove(u8path(records[0].histPath));
    fs::resize_file(u8path(records[0].tinyPath), 5);
    CandidateFeatures missing(records, needed, dir);
    CHECK(missing.tiny(2)[0] == 42);
    CHECK_FALSE(missing.hasTiny(0));
    CHECK_FALSE(missing.hasLbp(0));
    FeaturePack::Reader missingReader(dir, records);
    CHECK_FALSE(missingReader.get(0).tinyValid);
    CHECK_FALSE(missingReader.get(0).lbpValid);
    fs::resize_file(publishedPath(".lbp.bin"), 5);
    CandidateFeatures truncated(records, needed, dir);
    CHECK(truncated.tiny(2)[0] == 42);
    CHECK_FALSE(truncated.hasLbp(0));
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
    request.normalizeHeight = 128; // 非 CUDA 模板尺寸，两种构建都验收 CPU 批次。
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
