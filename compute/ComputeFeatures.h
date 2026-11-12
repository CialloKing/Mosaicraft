#pragma once
#include <array>
#include <cstdint>

namespace mosaicraft::compute
{
// 固定维度是图库契约，不随设备后端或评分精度改变。
struct Feature
{
    std::array<double, 3> lab{};
    std::array<float, 192> grid{};
    std::array<uint8_t, 256> tiny{};
    std::array<float, 256> lbp{};
    double edge = 0;
};
} // namespace mosaicraft::compute
