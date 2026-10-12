#include "ColorOrder.h"

#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace mosaicraft
{

std::int64_t imageColorOrderKey(const cv::Mat& image)
{
    if (image.empty() || image.type() != CV_8UC3)
    {
        throw std::invalid_argument("color ordering requires a nonempty BGR image");
    }
    cv::Mat thumbnail, hsv, lab;
    cv::resize(image, thumbnail, cv::Size(32, 32), 0, 0, cv::INTER_AREA);
    cv::cvtColor(thumbnail, hsv, cv::COLOR_BGR2HSV);
    cv::cvtColor(thumbnail, lab, cv::COLOR_BGR2Lab);

    constexpr int bins = 36;
    constexpr double pi = 3.14159265358979323846;
    std::array<int, bins> counts{};
    std::array<double, bins> sine{}, cosine{};
    int neutral = 0;
    for (int y = 0; y < hsv.rows; ++y)
    {
        for (int x = 0; x < hsv.cols; ++x)
        {
            const auto pixel = hsv.at<cv::Vec3b>(y, x);
            if (pixel[1] < 32 || pixel[2] < 16)
            {
                ++neutral;
                continue;
            }
            const int bin = ((pixel[0] + 2) / 5) % bins;
            ++counts[bin];
            const double angle = pixel[0] * pi / 90.0;
            sine[bin] += pixel[1] * std::sin(angle);
            cosine[bin] += pixel[1] * std::cos(angle);
        }
    }
    int dominant = 0, largest = 0;
    for (int i = 0; i < bins; ++i)
    {
        // 合并相邻色箱，避免同一背景色跨箱后被少量其他颜色抢占。
        const int population = counts[(i + bins - 1) % bins] + counts[i] + counts[(i + 1) % bins];
        if (population > largest)
        {
            largest = population;
            dominant = i;
        }
    }
    const auto lightness = static_cast<std::int64_t>(std::lround(cv::mean(lab)[0]));
    if (neutral >= largest)
    {
        return lightness;
    }
    double sumSin = 0.0, sumCos = 0.0;
    for (int offset = -1; offset <= 1; ++offset)
    {
        const int bin = (dominant + bins + offset) % bins;
        sumSin += sine[bin];
        sumCos += cosine[bin];
    }
    double angle = std::atan2(sumSin, sumCos) * 180.0 / pi;
    if (angle < 0)
    {
        angle += 360.0;
    }
    const auto hue = static_cast<std::int64_t>(std::lround(angle)) % 360;
    const auto band = hue / 15;
    const auto saturation = static_cast<std::int64_t>(std::lround(cv::mean(hsv)[1]));
    // 灰度区以白色结束，首个彩色区从亮到暗；相邻色相区交替方向。
    const auto brightness = band % 2 == 0 ? 255 - lightness : lightness;
    return (std::int64_t{1} << 40) | (band << 32) | (brightness << 24) | (hue << 12) |
           (saturation << 4);
}

} // namespace mosaicraft
