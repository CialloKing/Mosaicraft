#pragma once

#include "BuildService.h"
#include "OutputPlan.h"
#include "UnicodeIO.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>

namespace mosaicraft
{
// 在分配 OpenCV 图像或计算 int 尺寸之前统一验证，CLI、API 和直接服务调用共用。
inline constexpr int64_t kMaxImageDimension = 1000000;

inline bool validImageSize(int64_t width, int64_t height)
{
    return width > 0 && height > 0 && width <= kMaxImageDimension && height <= kMaxImageDimension && width * height <= 100000000;
}

inline bool validateDevice(BackendPolicy backend, const std::string &device, bool useGpu,
                           std::string &error, std::string *field)
{
    bool valid = device == "auto" || device == "cpu";
    const auto colon = device.find(':');
    if (colon != std::string::npos)
    {
        const auto prefix = device.substr(0, colon);
        valid = (prefix == "cuda" || prefix == "vulkan" || prefix == "metal") && colon + 1 < device.size() &&
            std::all_of(device.begin() + colon + 1, device.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
    }
    const bool specificGpu = device != "auto" && device != "cpu";
    if (!valid || (specificGpu && (!useGpu || backend == BackendPolicy::Cpu)) ||
        (device == "cpu" && backend != BackendPolicy::Auto && backend != BackendPolicy::Cpu) ||
        (specificGpu && backend != BackendPolicy::Auto && device.substr(0, colon) != backendName(backend)))
    {
        error = "device must be auto, cpu or <backend>:<index> and agree with backend/useGpu";
        if (field)
        {
            *field = "device";
        }
        return false;
    }
    return true;
}

inline bool validateBuildRequest(const BuildRequest &request, std::string &error, std::string *field = nullptr)
{
    if (request.appendMode && request.cropMode != CropMode::Center)
    {
        if (field)
        {
            *field = "cropMode";
        }
        error = "smart crop is only available for library build or normalize";
        return false;
    }
    if (request.cropDevice && request.cropMode == CropMode::Center)
    {
        if (field)
        {
            *field = "cropDevice";
        }
        error = "crop device requires smart-real or smart-anime";
        return false;
    }
    if (request.cropDevice && experimentalCropDevice(*request.cropDevice))
    {
        if (!request.normalizeOnly || request.appendMode)
        {
            if (field)
            {
                *field = "cropDevice";
            }
            error = "Vulkan smart crop devices are experimental and only available for standalone normalization";
            return false;
        }
        if (*request.cropDevice == CropDevice::Vulkan && request.cropMode != CropMode::SmartReal)
        {
            if (field)
            {
                *field = "cropDevice";
            }
            error = "Vulkan alone currently requires smart-real; use cuda+vulkan for dual GPU anime normalization";
            return false;
        }
    }
    if (request.benchmark && (!request.appendMode || request.normalizeOnly))
    {
        if (field)
        {
            *field = "benchmark";
        }
        error = "library benchmark is only available in append mode";
        return false;
    }
    if (!validateDevice(request.backend, request.device, true, error, field))
    {
        return false;
    }
    if (request.threads < 0 || request.threads > 256 ||
        (!request.appendMode && (request.normalizeWidth > 8192 || request.normalizeHeight > 8192 ||
                                 !validImageSize(request.normalizeWidth, request.normalizeHeight))))
    {
        if (field)
        {
            *field = request.threads < 0 || request.threads > 256
                         ? "threads"
                         : (request.normalizeWidth <= 0 || request.normalizeWidth > 8192 ? "normalizeWidth"
                                                                                         : "normalizeHeight");
        }
        error = "invalid build dimensions or thread count (dimensions <=8192, threads 0..256)";
        return false;
    }
    if (!request.normalizeOnly)
    {
        const auto validLevel = [&](const char *name, int value, int min, int max)
        {
            if (value >= min && value <= max)
            {
                return true;
            }
            if (field)
            {
                *field = name;
            }
            error = std::string(name) + " must be between " + std::to_string(min) + " and " + std::to_string(max);
            return false;
        };
        return validLevel("jpgQuality", request.encoding.jpgQuality, 1, 100) &&
               (!request.encoding.webpQuality || validLevel("webpQuality", *request.encoding.webpQuality, 1, 100)) &&
               validLevel("pngLevel", request.encoding.pngLevel, 0, 9);
    }
    return true;
}

inline bool validateMosaicRequest(MosaicRequest &request, std::string &error, std::string *field = nullptr)
{
    auto &cfg = request.config;
    if (!validateDevice(cfg.backend, cfg.device, cfg.useGpu, error, field))
    {
        return false;
    }
    if (cfg.generatePreview && (cfg.outputMode != OutputMode::Image))
    {
        if (field)
        {
            *field = "generatePreview";
        }
        error = "--thumbnail requires image mode; incompatible with html/tiles/deepzoom";
        return false;
    }
    if (!std::isfinite(cfg.overlayStrength) || cfg.overlayStrength < 0 || cfg.overlayStrength > 1)
    {
        if (field)
        {
            *field = "overlayStrength";
        }
        error = "overlayStrength must be finite and between 0 and 1";
        return false;
    }
    if (cfg.threads < 0 || cfg.threads > 256 || cfg.tileW < 4 || cfg.tileH < 4 || !validImageSize(cfg.tileW, cfg.tileH) ||
        !validImageSize(cfg.nativeTileW, cfg.nativeTileH) || cfg.outW < 0 || cfg.outH < 0 || cfg.outW > kMaxImageDimension ||
        cfg.outH > kMaxImageDimension || cfg.candidates < 1 || cfg.candidates > 10000 ||
        cfg.topNRandom < 1 || cfg.topNRandom > 10000 || cfg.neighborWindow < 0 || cfg.neighborWindow > 1000000 ||
        cfg.jpegQuality < 1 || cfg.jpegQuality > 100 || cfg.pngCompressionLevel < 1 || cfg.pngCompressionLevel > 9)
    {
        if (field)
        {
            *field = "config";
        }
        error = "invalid mosaic dimensions or integer options (candidates 1..10000)";
        return false;
    }
    if (cfg.targetPercentMilli < 0 || (cfg.targetPercentMilli > 0 && (cfg.outW != 0 || cfg.outH != 0)))
    {
        if (field)
        {
            *field = "targetSize";
        }
        error = "target size percentage must be positive and cannot be combined with pixel dimensions";
        return false;
    }
    double sum = 0;
    for (double weight : {cfg.labWeight, cfg.gridWeight, cfg.tinyWeight, cfg.edgeWeight, cfg.lbpWeight})
    {
        if (!std::isfinite(weight) || weight < 0)
        {
            if (field)
            {
                *field = "weights";
            }
            error = "feature weights must be finite and nonnegative";
            return false;
        }
        sum += weight;
    }
    if (!std::isfinite(sum) || sum <= 0 || !std::isfinite(cfg.usePenalty) || cfg.usePenalty < 0 ||
        !std::isfinite(cfg.neighborPenalty) || cfg.neighborPenalty < 0 || !std::isfinite(cfg.lRange) ||
        cfg.lRange < 0 || cfg.lRange > 255 || !std::isfinite(cfg.colorStrength) || cfg.colorStrength < 0 ||
        cfg.colorStrength > 0.5)
    {
        if (field)
        {
            *field = "weights";
        }
        error = "weights, penalties and ranges must be finite and within their valid ranges";
        return false;
    }
    if (cfg.writeMode != "auto" && cfg.writeMode != "batch" && cfg.writeMode != "stream")
    {
        if (field)
        {
            *field = "writeMode";
        }
        error = "writeMode must be auto, batch or stream";
        return false;
    }
    if (cfg.outputMode == OutputMode::Html)
    {
        if (cfg.formatExplicit || (cfg.colorAdjust && cfg.colorStrength != 0) || cfg.generatePreview ||
            cfg.writeMode != "auto")
        {
            error = "HTML is incompatible with image encoding options, thumbnail output and nonzero brightness jitter";
            if (field)
            {
                *field = "outputMode";
            }
            return false;
        }
        return true;
    }
    detail::OutputFormat format;
    if (!detail::resolveOutputFormat(request.inputPath, request.outputPath, cfg.outputFormat, cfg.formatExplicit,
                                     cfg.outputMode == OutputMode::Image, format, error, field))
    {
        return false;
    }
    // 只规范化工作配置，保留显式来源；任务恢复不能把推导格式变成 --format。
    cfg.outputFormat = format.format;
    return true;
}
} // namespace mosaicraft
