#pragma once

#include <array>
#include <cstdint>

namespace mosaicraft
{
// 纹理与素材元数据分开，列表和颜色重排不需要为它们分配空间。
struct TextureFeatures
{
    std::array<uint8_t, 256> tiny{};
    std::array<float, 256> lbp{};
    bool tinyValid = false, lbpValid = false;
};
}
