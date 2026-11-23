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

class SmartCropError : public std::runtime_error
{
public:
    SmartCropError(const char *code, const std::string &message) : std::runtime_error(message), m_code(code)
    {
    }
    const char *code() const noexcept
    {
        return m_code;
    }

private:
    const char *m_code;
};

struct SmartCropAnalysis
{
    cv::Mat foreground;
    // Coordinates are fractions of the uncropped image, independent of either model's input size.
    std::vector<cv::Rect2d> faces;
};

struct SmartCropModelPair
{
    CropMode mode;
    std::vector<unsigned char> saliency;
    std::vector<unsigned char> face;
};

bool smartCropAvailable() noexcept;
SmartCropModelPair loadSmartCropModelPair(CropMode mode);

// Each worker owns an instance: DNN inputs and detector state are mutable.
class SmartCropInference
{
public:
    explicit SmartCropInference(const SmartCropModelPair &models);
    ~SmartCropInference();
    void probe();
    SmartCropAnalysis analyze(const cv::Mat &image);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
