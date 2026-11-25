#pragma once

#include <memory>
#include <opencv2/core.hpp>

namespace mosaicraft
{
enum class CropMode;

class SmartCropVulkan
{
public:
    SmartCropVulkan(int device, CropMode mode);
    ~SmartCropVulkan();
    cv::Mat foreground(const cv::Mat &blob) const;
    cv::Mat animeDistances(const cv::Mat &blob, int anchors) const;
    std::vector<cv::Rect2d> realBoxes(const cv::Mat &image) const;
    static int selectDevice(int requested, bool separateFromCuda);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
