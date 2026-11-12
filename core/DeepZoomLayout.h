#pragma once

#include <cstdint>
#include <vector>

namespace mosaicraft
{
inline constexpr int deepZoomTileSize = 256;

struct DeepZoomLevel
{
    int width;
    int height;

    static int tiles(int edge)
    {
        return edge / deepZoomTileSize + (edge % deepZoomTileSize != 0);
    }

    uint64_t files() const
    {
        return uint64_t(tiles(width)) * tiles(height);
    }
};

// 从全分辨率往下逐级向上取整，保留奇数边缘像素。
inline std::vector<DeepZoomLevel> deepZoomLevels(int width, int height)
{
    std::vector<DeepZoomLevel> levels{{width, height}};
    while (width > 1 || height > 1)
    {
        width = width / 2 + width % 2;
        height = height / 2 + height % 2;
        levels.push_back({width, height});
    }
    return levels;
}
} // namespace mosaicraft
