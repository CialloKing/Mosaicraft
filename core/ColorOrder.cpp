#include "ColorOrder.h"

#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mosaicraft
{

std::int64_t meanLabColorOrderKey(double averageL, double averageA, double averageB)
{
    if (!std::isfinite(averageL) || !std::isfinite(averageA) || !std::isfinite(averageB) ||
        averageL < -0.001 || averageL > 255.001 ||
        averageA < 0 || averageA > 255 || averageB < 0 || averageB > 255)
    {
        throw std::invalid_argument("color ordering requires valid encoded average LAB");
    }
    // CUDA 的亮度端点可能有浮点尾差；分类前保留平均色小数，不能先量化色度。
    const auto lightness = static_cast<std::int64_t>(std::lround(std::clamp(averageL, 0.0, 255.0)));
    const double a = averageA - 128.0, b = averageB - 128.0;
    const double chroma = std::hypot(a, b);
    if (chroma < 0.5)
    {
        return lightness;
    }

    cv::Vec3f pixel(static_cast<float>(std::clamp(averageL, 0.0, 255.0) * 100.0 / 255.0),
                    static_cast<float>(a), static_cast<float>(b));
    cv::Mat lab(1, 1, CV_32FC3, &pixel), bgr, hsv;
    cv::cvtColor(lab, bgr, cv::COLOR_Lab2BGR);
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
    const auto hue = static_cast<std::int64_t>(std::lround(hsv.at<cv::Vec3f>(0, 0)[0])) % 360;
    // 色相环从 345 度开始，红色的两侧不会被拆到编号首尾。
    const auto rotatedHue = (hue + 15) % 360;
    const auto band = rotatedHue / 15;
    const auto brightness = band % 2 == 0 ? 255 - lightness : lightness;
    const auto colorfulness = static_cast<std::int64_t>(std::lround(chroma));
    return (std::int64_t(1) << 40) | (band << 32) | (brightness << 24) |
           (rotatedHue << 12) | (colorfulness << 4);
}

} // namespace mosaicraft
