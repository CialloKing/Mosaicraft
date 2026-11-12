#include "MosaicPlanning.h"

#include "Database.h"
#include "DeepZoomLayout.h"
#include "MatchingBatch.h"
#include "MosaicOutput.h"
#include "RequestValidation.h"
#include "TileCompositor.h"
#include "UnicodeIO.h"
#include <opencv2/imgproc.hpp>

#include <charconv>
#include <array>
#include <cmath>
#include <limits>
#include <thread>

namespace mosaicraft
{
namespace
{
constexpr uint64_t MiB = 1024 * 1024;

uint64_t boundedBytes(long double value)
{
    constexpr auto maximum = std::numeric_limits<uint64_t>::max();
    return value >= static_cast<long double>(maximum) ? maximum : static_cast<uint64_t>(value);
}

uint64_t divUp(uint64_t value, uint64_t divisor)
{
    return value / divisor + (value % divisor != 0);
}

// 仅缓存最近一张目标图的尺寸，调整表单参数时复用正式解码的尺寸口径。
cv::Size sourceSize(const std::string &input)
{
    const auto path = std::filesystem::absolute(u8path(input)).lexically_normal();
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    if (error)
    {
        return {};
    }
    const auto modified = std::filesystem::last_write_time(path, error);
    if (error)
    {
        return {};
    }
    static std::mutex mutex;
    static std::filesystem::path cachedPath;
    static std::filesystem::file_time_type cachedModified;
    static uintmax_t cachedBytes = 0;
    static cv::Size cachedSize;
    const std::lock_guard<std::mutex> lock(mutex);
    if (path == cachedPath && modified == cachedModified && bytes == cachedBytes)
    {
        return cachedSize;
    }
    const cv::Mat image = imreadUnicode(pathToUtf8(path), cv::IMREAD_COLOR);
    if (!image.empty())
    {
        cachedPath = path;
        cachedModified = modified;
        cachedBytes = bytes;
        cachedSize = image.size();
    }
    return image.size();
}

void estimateFileRange(MosaicPlan &plan, const MosaicEngine::Config &config)
{
    uint64_t width = plan.output.width, height = plan.output.height;
    long double pixels = static_cast<long double>(width) * height;
    plan.files = plan.mode == "single" ? 1 : uint64_t(plan.output.tilesX) * plan.output.tilesY;
    if (plan.mode == "deepzoom")
    {
        plan.files = 2; // DZI 清单和查看页。
        pixels = 0;
        for (const auto &level : deepZoomLevels(plan.output.width, plan.output.height))
        {
            plan.files += level.files();
            pixels += static_cast<long double>(level.width) * level.height;
        }
    }
    if (config.generatePreview)
    {
        ++plan.files;
    }

    const auto &format = plan.output.format;
    const double compression = config.pngCompressionLevel / 9.0;
    // 平色、渐变和随机纹理实测的每像素字节数加余量，供尚无选图时预估。
    double low = 0.003, high = 3.2;
    if (format == "jpg" || format == "webp")
    {
        constexpr std::array<int, 5> quality{1, 25, 50, 75, 100};
        constexpr std::array<double, 5> jpg{0.065, 0.29, 0.48, 0.71, 2.30};
        constexpr std::array<double, 5> webp{0.21, 0.49, 0.62, 0.74, 1.32};
        const auto &bounds = format == "jpg" ? jpg : webp;
        const auto end = std::lower_bound(quality.begin() + 1, quality.end(), config.jpegQuality) - quality.begin();
        const double t = double(config.jpegQuality - quality[end - 1]) / (quality[end] - quality[end - 1]);
        high = bounds[end - 1] + t * (bounds[end] - bounds[end - 1]);
        low = format == "jpg" ? 0.012 : 0.001;
    }
    else if (format == "png")
    {
        low = 0.005 - 0.002 * compression;
    }
    else if (plan.rawBytes >= 400 * MiB)
    {
        // BigTIFF 在此阈值后使用未压缩条带。
        plan.fileBytes = {plan.rawBytes, boundedBytes(pixels * 3 + height * 24.0L + 8.0L * MiB)};
        return;
    }
    else
    {
        low = 0.015;
        high = 4.7;
    }
    const uint64_t jpegFiles = format == "jpg" ? plan.files - (plan.mode == "deepzoom" ? 2 : config.generatePreview) : 0;
    const long double headers = 700.0L * jpegFiles + 512.0L * (plan.files - jpegFiles) +
                                (plan.mode == "deepzoom" ? 4096.0L : 0);
    const long double preview = config.generatePreview ? 8.0L * MiB : 0;
    plan.fileBytes = {boundedBytes(pixels * low + 600.0L * jpegFiles),
                      boundedBytes(pixels * high + headers + preview)};
}
} // namespace

bool resolveTargetGeometry(int width, int height, const MosaicEngine::Config &config,
                           TargetGeometry &geometry, std::string &error)
{
    if (!validImageSize(width, height))
    {
        error = "Target exceeds 100 million pixels.";
        return false;
    }
    const int64_t resizedWidth = config.outW > 0 ? config.outW : config.outH > 0
        ? std::max<int64_t>(1, std::llround(static_cast<double>(width) * config.outH / height)) : width;
    const int64_t resizedHeight = config.outH > 0 ? config.outH : config.outW > 0
        ? std::max<int64_t>(1, std::llround(static_cast<double>(height) * config.outW / width)) : height;
    if (!validImageSize(resizedWidth, resizedHeight))
    {
        error = "Target resize exceeds 100 million pixels.";
        return false;
    }
    const auto scale = std::max(1, config.upscale);
    const auto workingWidth = resizedWidth * scale;
    const auto workingHeight = resizedHeight * scale;
    if (!validImageSize(workingWidth, workingHeight))
    {
        error = "Upscaled target exceeds 100 million pixels.";
        return false;
    }
    geometry = {static_cast<int>(resizedWidth), static_cast<int>(resizedHeight),
                static_cast<int>(workingWidth), static_cast<int>(workingHeight),
                static_cast<int>(divUp(workingWidth, config.tileW)), static_cast<int>(divUp(workingHeight, config.tileH))};
    return true;
}

void resizeTarget(cv::Mat &target, const TargetGeometry &geometry, const MosaicEngine::Config &config)
{
    if (config.outW > 0 || config.outH > 0)
    {
        cv::Mat resized;
        cv::resize(target, resized, cv::Size(geometry.resizedWidth, geometry.resizedHeight), 0, 0, cv::INTER_AREA);
        target = std::move(resized);
    }
    if (config.upscale > 1)
    {
        cv::Mat upscaled;
        cv::resize(target, upscaled, cv::Size(geometry.width, geometry.height), 0, 0, cv::INTER_LINEAR);
        target = std::move(upscaled);
    }
}

void padTarget(cv::Mat &target, const MosaicEngine::Config &config, int columns, int rows)
{
    const int right = columns * config.tileW - target.cols;
    const int bottom = rows * config.tileH - target.rows;
    if (right > 0 || bottom > 0)
    {
        cv::copyMakeBorder(target, target, 0, bottom, 0, right, cv::BORDER_REFLECT);
    }
}

bool readLibraryDimensions(Database &database, int &width, int &height)
{
    const auto w = database.getMeta("feature_w");
    const auto h = database.getMeta("feature_h");
    const auto parseWidth = std::from_chars(w.data(), w.data() + w.size(), width);
    const auto parseHeight = std::from_chars(h.data(), h.data() + h.size(), height);
    return parseWidth.ec == std::errc{} && parseWidth.ptr == w.data() + w.size() &&
           parseHeight.ec == std::errc{} && parseHeight.ptr == h.data() + h.size() &&
           width <= 8192 && height <= 8192 && validImageSize(width, height);
}

bool orientOutputTile(MosaicEngine::Config &config, int libraryWidth, int libraryHeight)
{
    if (config.nativeTileW == 180 && config.nativeTileH == 320 && libraryWidth > libraryHeight)
    {
        config.nativeTileW = 320;
        config.nativeTileH = 180;
        return true;
    }
    return false;
}

ServiceResult planMosaic(MosaicRequest request, MosaicPlan &plan)
{
    plan = {};
    std::string error, field;
    if (!validateMosaicRequest(request, error, &field))
    {
        return ServiceResult::failure(1, error, "invalid_argument", field);
    }
    std::error_code pathError;
    if (std::filesystem::is_directory(u8path(request.outputPath), pathError))
    {
        return ServiceResult::failure(1, "outputPath is a directory", "invalid_argument", "outputPath");
    }
    const auto size = sourceSize(request.inputPath);
    if (size.empty())
    {
        return ServiceResult::failure(1, "cannot read target image", "input_unreadable", "inputPath");
    }
    auto &config = request.config;
    plan.sourceWidth = size.width;
    plan.sourceHeight = size.height;
    if (!resolveTargetGeometry(size.width, size.height, config, plan.target, error))
    {
        return ServiceResult::failure(1, error, "invalid_argument", "config");
    }
    Database database(resolveDbPathForService(request.dbPath), Database::OpenMode::ReadOnly);
    if (!database.isOpen())
    {
        return ServiceResult::failure(2, database.error(), database.errorCode(), "dbPath");
    }
    plan.libraryCount = database.totalCount();
    if (!readLibraryDimensions(database, plan.libraryWidth, plan.libraryHeight))
    {
        return ServiceResult::failure(2, "invalid library feature dimensions", "library_incompatible", "dbPath");
    }
    if (plan.libraryCount <= 0)
    {
        return ServiceResult::failure(2, "database is empty", "library_unavailable", "dbPath");
    }
    orientOutputTile(config, plan.libraryWidth, plan.libraryHeight);
    const bool single = !config.tiledOutput && !config.deepZoom;
    detail::OutputFormat format;
    if (!detail::resolveOutputFormat(request.inputPath, request.outputPath, config.outputFormat, config.formatExplicit,
                                     single, format, error, &field) ||
        !detail::makeOutputPlan(format, request.outputPath, plan.target.columns, plan.target.rows,
                                config.nativeTileW, config.nativeTileH, single, plan.output, error))
    {
        return ServiceResult::failure(1, error, "invalid_argument", field.empty() ? "format" : field);
    }
    plan.mode = config.deepZoom ? "deepzoom" : config.tiledOutput ? "tiled" : "single";
    if (!single)
    {
        plan.output.format = "jpg"; // 分块与 DeepZoom 的实际编码器固定为 JPEG。
        plan.output.path += config.deepZoom ? ".dzi" : "_files/0";
    }
    if (plan.output.scaled)
    {
        plan.warnings.push_back("output_scaled");
    }
    if (plan.output.formatChanged)
    {
        plan.warnings.push_back("output_format_changed");
    }
    if (std::abs(double(plan.libraryWidth) / plan.libraryHeight - double(plan.output.tileWidth) / plan.output.tileHeight) > 0.02)
    {
        plan.warnings.push_back("tile_aspect_mismatch");
    }
    const double targetAspect = double(plan.target.width) / plan.target.height;
    const double outputAspect = double(plan.output.width) / plan.output.height;
    if (std::abs(outputAspect / targetAspect - 1.0) > 0.02)
    {
        plan.warnings.push_back("output_aspect_mismatch");
    }

    const uint64_t count = uint64_t(plan.target.columns) * plan.target.rows;
    const uint64_t targetBytes = uint64_t(plan.target.columns) * plan.target.rows * config.tileW * config.tileH * 3;
    const int candidates = std::min(config.candidates, plan.libraryCount);
    const auto rows = std::min<uint64_t>(count, matchingBatchRows({}, plan.libraryWidth, plan.libraryHeight,
                                                                  candidates, plan.libraryCount, false));
    const uint64_t basicMatching = targetBytes + count * sizeof(int) + cpuMatchingBufferBytes(rows, candidates);
    plan.matchingRam = {basicMatching, basicMatching + 256 * MiB + uint64_t(plan.libraryCount) * 1536};
#ifdef MOSAICRAFT_CUDA
    if (config.useGpu)
    {
        const uint64_t imageBytes = uint64_t(plan.libraryWidth) * plan.libraryHeight * 3;
        const auto scratch = gpuMatchingBufferBytes(rows, imageBytes, candidates, config.analyze);
        plan.matchingVram = ByteRange{scratch, scratch + std::min<uint64_t>(512 * MiB, imageBytes * plan.libraryCount)};
    }
#endif
    const uint64_t overlay = config.overlayStrength > 0 ? TileCompositor::workingBytes({plan.output.tileWidth, plan.output.tileHeight}) : 0;
    const auto workers = std::min<uint64_t>(count, std::min(8u, std::max(1u, std::thread::hardware_concurrency())));
    const auto budget = outputBudget(config, plan.output, config.overlayStrength > 0 ? targetBytes : 0, overlay * workers);
    plan.rawBytes = budget.rawBytes;
    plan.writeMode = single ? (budget.stream ? "stream" : "batch") : "tiles";
    plan.writingRam = single ? ByteRange{budget.stream ? budget.rowBytes : budget.rawBytes,
                                         budget.stream ? budget.streamBytes : budget.batchBytes}
                             : ByteRange{uint64_t(plan.output.tileWidth) * plan.output.tileHeight * 3, budget.streamBytes};
    estimateFileRange(plan, config);
    return ServiceResult::success();
}
} // namespace mosaicraft
