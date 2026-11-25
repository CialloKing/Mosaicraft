#pragma once

#include <memory>
#include <opencv2/core.hpp>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace mosaicraft
{
enum class CropMode
{
    Center,
    SmartReal,
    SmartAnime
};

inline std::optional<CropMode> parseCropMode(std::string_view name)
{
    if (name == "center")
    {
        return CropMode::Center;
    }
    if (name == "smart-real")
    {
        return CropMode::SmartReal;
    }
    if (name == "smart-anime")
    {
        return CropMode::SmartAnime;
    }
    return std::nullopt;
}

inline const char *cropModeName(CropMode mode)
{
    switch (mode)
    {
    case CropMode::SmartReal:
        return "smart-real";
    case CropMode::SmartAnime:
        return "smart-anime";
    default:
        return "center";
    }
}

enum class CropDevice
{
    Auto,
    Cpu,
    Cuda,
    Vulkan,
    CudaVulkan
};

inline constexpr bool experimentalCropDevice(CropDevice device)
{
    return device == CropDevice::Vulkan || device == CropDevice::CudaVulkan;
}

inline std::optional<CropDevice> parseCropDevice(std::string_view name)
{
    if (name == "auto")
    {
        return CropDevice::Auto;
    }
    if (name == "cpu")
    {
        return CropDevice::Cpu;
    }
    if (name == "cuda")
    {
        return CropDevice::Cuda;
    }
    if (name == "vulkan")
    {
        return CropDevice::Vulkan;
    }
    if (name == "cuda+vulkan")
    {
        return CropDevice::CudaVulkan;
    }
    return std::nullopt;
}

inline const char *cropDeviceName(CropDevice device)
{
    switch (device)
    {
    case CropDevice::Auto: return "auto";
    case CropDevice::Cuda: return "cuda";
    case CropDevice::Vulkan: return "vulkan";
    case CropDevice::CudaVulkan: return "cuda+vulkan";
    default: return "cpu";
    }
}

enum class SmartCropCudaFault
{
    None,
    Retryable,
    RestartRequired
};

class SmartCropError : public std::runtime_error
{
public:
    SmartCropError(const char *code, const std::string &message, SmartCropCudaFault fault = SmartCropCudaFault::None)
        : std::runtime_error(message), m_code(code), m_fault(fault)
    {
    }
    const char *code() const noexcept
    {
        return m_code;
    }

    SmartCropCudaFault cudaFault() const noexcept
    {
        return m_fault;
    }

private:
    const char *m_code;
    SmartCropCudaFault m_fault;
};

struct SmartCropAnalysis
{
    cv::Mat foreground;
    // Coordinates are fractions of the uncropped image, independent of either model's input size.
    std::vector<cv::Rect2d> faces;
    double saliencyMs = 0, faceMs = 0;
    CropMode mode = CropMode::Center;
};

struct SmartCropModelPair
{
    CropMode mode;
    std::vector<unsigned char> saliency;
    std::vector<unsigned char> face;
};

struct SmartCropDecision
{
    cv::Rect roi;
    bool faceFound;
    bool mainFaceClipped;
};

struct SmartCropStats
{
    size_t facesFound = 0, moved = 0, centered = 0, mainFaceClipped = 0;
    double inferenceMs = 0, saliencyMs = 0, faceMs = 0;
};

SmartCropDecision chooseSmartCrop(cv::Size covered, cv::Size target, const SmartCropAnalysis &analysis);
bool smartCropAvailable() noexcept;
bool smartCropCudaAvailable() noexcept;
void requireSmartCropDevice(CropDevice device);
SmartCropModelPair loadSmartCropModelPair(CropMode mode);

// Each worker owns an instance: DNN inputs and detector state are mutable.
class SmartCropInference
{
public:
    SmartCropInference(const SmartCropModelPair &models, CropDevice device, std::optional<int> vulkan = {});
    ~SmartCropInference();
    void probe();
    SmartCropAnalysis analyze(const cv::Mat &image);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
