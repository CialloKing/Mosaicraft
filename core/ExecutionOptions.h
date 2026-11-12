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
    Auto, Cpu, Cuda
};

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
    throw std::invalid_argument("backend must be auto, cpu or cuda");
}

inline std::string backendName(BackendPolicy policy)
{
    return policy == BackendPolicy::Cpu ? "cpu" : policy == BackendPolicy::Cuda ? "cuda" : "auto";
}
}
