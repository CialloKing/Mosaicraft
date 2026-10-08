#include "../core/GridDuplicateCache.h"
#include <deque>
#include <limits>
#include "../core/ImageCache.h"
#include "../core/MosaicOutput.h"
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../core/BuildService.h"
#include "../core/DatabaseService.h"
#include "../core/InspectService.h"
#include "../core/MosaicService.h"
#include "../core/UnicodeIO.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

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
    }
    CHECK_FALSE(writeMosaicOutput(config, utf8(workspace.root / "missing" / "out.png"),
        2, 3, 18, 32, records, selected, {}, stats));
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
