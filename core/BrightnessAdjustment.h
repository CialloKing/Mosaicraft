#pragma once

#include <opencv2/imgproc.hpp>
#include <random>
#include <cstdint>
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

inline void applyBrightnessFactor(cv::Mat &image, double factor)
{
    cv::Mat lab;
    cv::cvtColor(image, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> channels(3);
    cv::split(lab, channels);
    channels[0] = channels[0] * factor;
    cv::merge(channels, lab);
    cv::cvtColor(lab, image, cv::COLOR_Lab2BGR);
}
inline void adjustBrightness(cv::Mat &image, double strength, std::mt19937 &rng)
{
    if (strength != 0.0)
    {
        applyBrightnessFactor(image, sampleBrightnessFactor(rng, strength));
    }
}

inline void adjustBrightnessAt(cv::Mat &image, double strength, uint32_t seed, size_t position)
{
    if (strength == 0.0)
    {
        return;
    }
    // 每格独立状态使外观不依赖线程数及完成顺序。
    uint64_t state = (uint64_t(seed) << 32) ^ uint64_t(position) ^ UINT64_C(0x425249474854);
    uint64_t value = 0;
    do
    {
        state += UINT64_C(0x9e3779b97f4a7c15);
        value = state;
        value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
        value ^= value >> 31;
    } while (value >= UINT64_MAX - UINT64_MAX % 1001);
    applyBrightnessFactor(image, brightnessFactor(static_cast<int>(value % 1001), strength));
}
} // namespace detail
} // namespace mosaicraft
