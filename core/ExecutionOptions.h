#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace mosaicraft
{
enum class OutputMode
{
    Image, Html, Tiles, DeepZoom
};
enum class BackendPolicy
{
    Auto, Cpu, Cuda, Vulkan, Metal
};

enum class ScoreMode
{
    Compatible, Fast
};

inline ScoreMode parseScoreMode(const std::string &value)
{
    if (value == "compatible")
    {
        return ScoreMode::Compatible;
    }
    if (value == "fast")
    {
        return ScoreMode::Fast;
    }
    throw std::invalid_argument("scoreMode must be compatible or fast");
}

inline const char *scoreModeName(ScoreMode mode)
{
    return mode == ScoreMode::Fast ? "fast" : "compatible";
}

inline std::string outputModeName(OutputMode mode)
{
    switch (mode)
    {
    case OutputMode::Html: return "html";
    case OutputMode::Tiles: return "tiles";
    case OutputMode::DeepZoom: return "deepzoom";
    default: return "image";
    }
}

inline OutputMode parseOutputMode(const std::string &value)
{
    if (value == "image")
    {
        return OutputMode::Image;
    }
    if (value == "html")
    {
        return OutputMode::Html;
    }
    if (value == "tiles")
    {
        return OutputMode::Tiles;
    }
    if (value == "deepzoom")
    {
        return OutputMode::DeepZoom;
    }
    throw std::invalid_argument("outputMode must be image, html, tiles or deepzoom");
}

inline BackendPolicy parseBackend(const std::string &value)
{
    if (value == "auto")
    {
        return BackendPolicy::Auto;
    }
    if (value == "cpu")
    {
        return BackendPolicy::Cpu;
    }
    if (value == "cuda")
    {
        return BackendPolicy::Cuda;
    }
    if (value == "vulkan")
    {
        return BackendPolicy::Vulkan;
    }
    if (value == "metal")
    {
        return BackendPolicy::Metal;
    }
    throw std::invalid_argument("backend must be auto, cpu, cuda, vulkan or metal");
}

inline std::string backendName(BackendPolicy policy)
{
    switch (policy)
    {
    case BackendPolicy::Cpu: return "cpu";
    case BackendPolicy::Cuda: return "cuda";
    case BackendPolicy::Vulkan: return "vulkan";
    case BackendPolicy::Metal: return "metal";
    default: return "auto";
    }
}
}
