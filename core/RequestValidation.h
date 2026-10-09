#pragma once

#include "BuildService.h"
#include "UnicodeIO.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>

namespace mosaicraft
{
// 在分配 OpenCV 图像或计算 int 尺寸之前统一验证，CLI、API 和直接服务调用共用。
inline bool validImageSize(int64_t width, int64_t height)
{
    return width > 0 && height > 0 && width <= 1000000 && height <= 1000000 &&
           width * height <= 100000000;
}

inline bool validateBuildRequest(const BuildRequest& request, std::string& error)
{
    if (request.threads < 0 || request.threads > 256 || request.normalizeWidth > 8192 ||
        request.normalizeHeight > 8192 || !validImageSize(request.normalizeWidth, request.normalizeHeight))
    {
        error = "invalid build dimensions or thread count (dimensions <=8192, threads 0..256)";
        return false;
    }
    return true;
}

inline bool validateMosaicRequest(MosaicRequest& request, std::string& error)
{
    auto& cfg = request.config;
    if (cfg.tileW < 4 || cfg.tileH < 4 || !validImageSize(cfg.tileW, cfg.tileH) ||
        !validImageSize(cfg.nativeTileW, cfg.nativeTileH) ||
        cfg.outW < 0 || cfg.outH < 0 || cfg.outW > 1000000 || cfg.outH > 1000000 ||
        cfg.upscale < 0 || cfg.upscale > 64 || cfg.candidates < 1 || cfg.candidates > 10000 ||
        cfg.topNrandom < 1 || cfg.topNrandom > 10000 || cfg.neighborWindow < 0 ||
        cfg.neighborWindow > 1000000 || cfg.jpegQuality < 1 || cfg.jpegQuality > 100 ||
        cfg.pngCompressionLevel < 1 || cfg.pngCompressionLevel > 9)
    {
        error = "invalid mosaic dimensions or integer options (upscale 0..64, candidates 1..10000)";
        return false;
    }
    double sum = 0;
    for (double weight : {cfg.labWeight, cfg.gridWeight, cfg.tinyWeight, cfg.edgeWeight, cfg.lbpWeight})
    {
        if (!std::isfinite(weight) || weight < 0)
        {
            error = "feature weights must be finite and nonnegative";
            return false;
        }
        sum += weight;
    }
    if (!std::isfinite(sum) || sum <= 0 || !std::isfinite(cfg.usePenalty) || cfg.usePenalty < 0 ||
        !std::isfinite(cfg.neighborPenalty) || cfg.neighborPenalty < 0 ||
        !std::isfinite(cfg.lRange) || cfg.lRange < 0 || cfg.lRange > 255 ||
        !std::isfinite(cfg.colorStrength) || cfg.colorStrength < 0 || cfg.colorStrength > 0.5)
    {
        error = "weights, penalties and ranges must be finite and within their valid ranges";
        return false;
    }
    if (cfg.writeMode != "auto" && cfg.writeMode != "batch" && cfg.writeMode != "stream")
    {
        error = "writeMode must be auto, batch or stream";
        return false;
    }
    auto lower = [](std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };
    cfg.outputFormat = lower(cfg.outputFormat);
    auto extension = lower(pathToUtf8(u8path(request.outputPath).extension()));
    if (!cfg.formatExplicit && !cfg.tiledOutput && !cfg.deepZoom && !extension.empty())
    {
        cfg.outputFormat = extension.substr(1);
    }
    if (cfg.outputFormat == "jpeg")
    {
        cfg.outputFormat = "jpg";
    }
    if (cfg.outputFormat == "tif")
    {
        cfg.outputFormat = "tiff";
    }
    if (cfg.outputFormat != "jpg" && cfg.outputFormat != "png" && cfg.outputFormat != "webp" &&
        cfg.outputFormat != "tiff")
    {
        error = "output format must be jpg, png, webp or tiff";
        return false;
    }
    return true;
}
}
