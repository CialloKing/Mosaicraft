#include "MosaicPlanning.h"
#include "BrightnessAdjustment.h"
#include "ImageCache.h"
#include "MosaicSelection.h"
#include "TileCompositor.h"
#include "UnicodeIO.h"
#include <opencv2/imgcodecs.hpp>

namespace mosaicraft
{
ServiceResult sampleMosaicSize(const MosaicRequest &request, const MosaicSelection &selection,
                               const std::atomic_bool &canceled, FileSizeSample &sample)
{
    const std::unique_lock<std::mutex> pipeline(pipelineMutex(), std::try_to_lock);
    if (!pipeline.owns_lock())
    {
        return ServiceResult::failure(1, "another task is running", "library_busy");
    }
    MosaicPlan plan;
    if (auto result = planMosaic(request, plan); !result.ok)
    {
        return result;
    }
    const auto &cfg = request.config;
    if (!MosaicEngine::selectionCurrent(request.inputPath, resolveDbPathForService(request.dbPath), cfg, selection))
    {
        return ServiceResult::failure(1, "preview has changed", "preview_stale");
    }
    const auto &out = plan.output;
    const int64_t tilePixels = int64_t(out.tileWidth) * out.tileHeight;
    const int64_t maxPixels = plan.mode == "deepzoom" ? 3200000 : 4000000;
    if (tilePixels > maxPixels)
    {
        return ServiceResult::failure(1, "one output tile exceeds the sample budget", "sample_unavailable");
    }
    const bool full = plan.mode == "single" && int64_t(out.width) * out.height <= maxPixels;
    const int strata = std::min<int64_t>(4, std::sqrt(maxPixels / tilePixels));
    const int side = std::max(1, static_cast<int>(std::sqrt(maxPixels / (strata * strata * tilePixels))));
    const int regionW = full ? selection.columns : plan.mode == "tiled" ? 1 : std::min(side, selection.columns);
    const int regionH = full ? selection.rows : plan.mode == "tiled" ? 1 : std::min(side, selection.rows);
    const int strataX = full ? 1 : std::min(strata, (selection.columns + regionW - 1) / regionW);
    const int strataY = full ? 1 : std::min(strata, (selection.rows + regionH - 1) / regionH);

    cv::Mat target;
    std::unique_ptr<TileCompositor> compositor;
    if (cfg.overlayStrength > 0)
    {
        target = imreadUnicode(request.inputPath, cv::IMREAD_COLOR);
        if (target.empty())
        {
            return ServiceResult::failure(1, "cannot read target image", "input_unreadable");
        }
        resizeTarget(target, plan.target, cfg);
        padTarget(target, cfg, selection.columns, selection.rows);
        compositor = std::make_unique<TileCompositor>(target, selection.columns, selection.rows,
                                                     cv::Size(out.tileWidth, out.tileHeight));
    }
    std::vector<int> codec;
    if (out.format == "png")
    {
        codec = {cv::IMWRITE_PNG_COMPRESSION, cfg.pngCompressionLevel};
    }
    else if (out.format == "tiff")
    {
        codec = {cv::IMWRITE_TIFF_COMPRESSION, plan.rawBytes < 400ULL * 1024 * 1024 ? 5 : 1};
    }
    else
    {
        codec = {out.format == "webp" ? cv::IMWRITE_WEBP_QUALITY : cv::IMWRITE_JPEG_QUALITY, cfg.jpegQuality};
    }
    ImageCache cache(32 * 1024 * 1024);
    std::mt19937 rng(0);
    uint64_t encodedBytes = 0;
    uint64_t primaryPixels = 0;
    long double scaledDensity = 0;
    for (int ry = 0; ry < strataY; ++ry)
    {
        for (int rx = 0; rx < strataX; ++rx)
        {
            const int left = rx * (selection.columns - regionW) / std::max(1, strataX - 1);
            const int top = ry * (selection.rows - regionH) / std::max(1, strataY - 1);
            cv::Mat pixels(regionH * out.tileHeight, regionW * out.tileWidth, CV_8UC3, cv::Scalar());
            for (int y = 0; y < regionH; ++y)
            {
                for (int x = 0; x < regionW; ++x)
                {
                    if (canceled.load())
                    {
                        return ServiceResult::failure(1, "sampling canceled", "preview_canceled");
                    }
                    const size_t position = size_t(top + y) * selection.columns + left + x;
                    const int id = selection.imageIds[position];
                    const auto found = selection.assets.find(id);
                    if (found == selection.assets.end())
                    {
                        continue;
                    }
                    auto image = cache.getShared(id, found->second.path, out.tileWidth, out.tileHeight);
                    if (!image)
                    {
                        continue;
                    }
                    cv::Mat tile = pixels(cv::Rect(x * out.tileWidth, y * out.tileHeight, out.tileWidth, out.tileHeight));
                    image->copyTo(tile);
                    if (cfg.colorAdjust)
                    {
                        detail::adjustBrightness(tile, cfg.colorStrength, rng);
                    }
                    if (compositor)
                    {
                        cv::Mat reference;
                        compositor->sample(position, reference);
                        cv::addWeighted(tile, 1.0 - cfg.overlayStrength, reference, cfg.overlayStrength, 0.0, tile);
                    }
                }
            }
            std::vector<unsigned char> encoded;
            if (!cv::imencode("." + out.format, pixels, encoded, codec))
            {
                return ServiceResult::failure(1, "sample encoding failed", "sample_unavailable");
            }
            sample.pixels += pixels.total();
            primaryPixels += pixels.total();
            ++sample.regions;
            encodedBytes += encoded.size();
            if (plan.mode == "deepzoom")
            {
                // 低层像素通过相同采样块缩小后编码，按各层面积加权。
                cv::Mat half;
                cv::resize(pixels, half, cv::Size(std::max(1, pixels.cols / 2), std::max(1, pixels.rows / 2)),
                           0, 0, cv::INTER_AREA);
                if (cv::imencode(".jpg", half, encoded, codec))
                {
                    scaledDensity += static_cast<long double>(encoded.size()) / half.total();
                    sample.pixels += half.total();
                }
            }
        }
    }
    if (!full && sample.regions < 2)
    {
        return ServiceResult::failure(1, "not enough sample regions", "sample_unavailable");
    }
    const long double outputPixels = static_cast<long double>(out.width) * out.height;
    const long double base = static_cast<long double>(encodedBytes) / primaryPixels * outputPixels;
    const long double extra = plan.mode == "deepzoom" ? outputPixels * scaledDensity / sample.regions / 3 : 0;
    const long double headers = plan.mode == "deepzoom" ? 512.0L * plan.files : 0;
    const auto estimate = std::min<long double>(base + extra + headers, std::numeric_limits<uint64_t>::max() / 4);
    const uint64_t previewBudget = cfg.generatePreview ? 8ULL * 1024 * 1024 : 0;
    const bool exactSample = full && !cfg.colorAdjust && out.format != "tiff" && plan.writeMode == "batch";
    sample.bytes = {static_cast<uint64_t>(estimate * (exactSample ? 0.97 : 0.65)),
                    static_cast<uint64_t>(estimate * (exactSample ? 1.03 : 1.50)) + previewBudget};
    return ServiceResult::success();
}
} // namespace mosaicraft
