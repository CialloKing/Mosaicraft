#pragma once

#include <memory>
#include <opencv2/core.hpp>

namespace mosaicraft
{
class SmartCropVulkan
{
public:
    explicit SmartCropVulkan(int device);
    ~SmartCropVulkan();
    cv::Mat foreground(const cv::Mat &blob) const;
    std::vector<cv::Rect2d> realBoxes(const cv::Mat &image) const;
    static unsigned deviceVendor(int device);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
