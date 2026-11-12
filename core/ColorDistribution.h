#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mosaicraft
{
inline int lightnessBand(double value)
{
    return value < 50 ? 0 : value < 150 ? 1 : 2;
}

struct LibraryCoverage
{
    bool dark, mid, green, red, blue, yellow;
};

inline LibraryCoverage libraryCoverage(double darkPct, double midPct, double minA, double maxA, double minB,
                                       double maxB)
{
    return {darkPct < 5, midPct < 15, minA > 115, maxA < 140, minB > 115, maxB < 140};
}

struct ColorDistribution
{
    static constexpr int axes = 32;
    std::array<uint32_t, axes * axes> ab{};
    std::array<uint32_t, 24> hue{};
    std::array<uint32_t, 8> lightness{};
    uint32_t gray = 0, total = 0;

    void add(double l, double a, double b)
    {
        if (!std::isfinite(l) || !std::isfinite(a) || !std::isfinite(b))
        {
            return;
        }
        const auto bin = [](double value, int width)
        {
            return static_cast<int>(std::clamp(value, 0.0, 255.0)) / width;
        };
        ++total;
        ++lightness[bin(l, 32)];
        ++ab[bin(b, 8) * axes + bin(a, 8)];
        if (std::hypot(a - 128, b - 128) < 0.5)
        {
            ++gray;
        }
        else
        {
            constexpr double toDegrees = 180.0 / 3.14159265358979323846;
            const double angle = std::atan2(b - 128, a - 128) * toDegrees;
            ++hue[static_cast<int>((angle < 0 ? angle + 360 : angle) / 15) % 24];
        }
    }
};

inline ColorDistribution summarizeColors(const std::vector<std::array<double, 3>> &colors)
{
    ColorDistribution result;
    for (const auto &color : colors)
    {
        result.add(color[0], color[1], color[2]);
    }
    return result;
}

struct BuildVisuals
{
    ColorDistribution before, after;
    uint64_t scanned = 0, normalized = 0, duplicates = 0, inserted = 0;
    bool hasColors = false;
};
} // namespace mosaicraft
