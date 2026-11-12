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
inline bool validImageSize(int64_t width, int64_t height)
{
    return width > 0 && height > 0 && width <= 1000000 && height <= 1000000 && width * height <= 100000000;
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
    if (!validateDevice(request.backend, request.device, true, error, field))
    {
        return false;
    }
    if (request.threads < 0 || request.threads > 256 || request.normalizeWidth > 8192 ||
        request.normalizeHeight > 8192 || !validImageSize(request.normalizeWidth, request.normalizeHeight))
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
        !validImageSize(cfg.nativeTileW, cfg.nativeTileH) || cfg.outW < 0 || cfg.outH < 0 || cfg.outW > 1000000 ||
        cfg.outH > 1000000 || cfg.upscale < 0 || cfg.upscale > 64 || cfg.candidates < 1 || cfg.candidates > 10000 ||
        cfg.topNRandom < 1 || cfg.topNRandom > 10000 || cfg.neighborWindow < 0 || cfg.neighborWindow > 1000000 ||
        cfg.jpegQuality < 1 || cfg.jpegQuality > 100 || cfg.pngCompressionLevel < 1 || cfg.pngCompressionLevel > 9)
    {
        if (field)
        {
            *field = "config";
        }
        error = "invalid mosaic dimensions or integer options (upscale 0..64, candidates 1..10000)";
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
