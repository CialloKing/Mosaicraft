#pragma once

#include <opencv2/imgproc.hpp>
#include <random>
#include <vector>

namespace mosaicraft
{
namespace detail
{
inline double brightnessFactor(int sample, double strength)
{
    return 1.0 + ((sample - 500) / 500.0) * strength;
}

inline double sampleBrightnessFactor(std::mt19937 &rng, double strength)
{
    // 均匀整数分布消除取模偏差；保留 1001 档，使乘数关于 1 对称。
    return brightnessFactor(std::uniform_int_distribution<int>(0, 1000)(rng), strength);
}

inline void adjustBrightness(cv::Mat &image, double strength, std::mt19937 &rng)
{
    // 零强度不经过 LAB 往返转换，避免量化误差改变原始像素。
    if (strength == 0.0)
    {
        return;
    }
    cv::Mat lab;
    cv::cvtColor(image, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> channels(3);
    cv::split(lab, channels);
    channels[0] = channels[0] * sampleBrightnessFactor(rng, strength);
    cv::merge(channels, lab);
    cv::cvtColor(lab, image, cv::COLOR_Lab2BGR);
}
} // namespace detail
} // namespace mosaicraft
