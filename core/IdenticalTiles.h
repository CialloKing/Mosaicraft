#pragma once
#include <opencv2/core.hpp>
#include <array>
#include <cstring>
#include <vector>

namespace mosaicraft::detail
{
// 只在当前批次内借用目标像素；采样签名用于寻找候选，命中须逐行比较全部 BGR 字节。
inline unsigned identicalTileOwners(const cv::Mat &target, cv::Size tile, int columns, int first, int count,
                                    std::vector<int> &owners)
{
    std::array<int, 256> slots;
    slots.fill(-1);
    owners.resize(count);
    unsigned hits = 0;
    const size_t bytes = size_t(tile.width) * 3;
    const auto origin = [&](int local)
    {
        const int pos = first + local;
        return target.ptr<uint8_t>(pos / columns * tile.height) + (pos % columns) * bytes;
    };
    for (int i = 0; i < count; ++i)
    {
        const auto *pixels = origin(i);
        unsigned hash = 2166136261u;
        for (int point = 0; point < 3; ++point)
        {
            const auto *rgb = pixels + size_t((tile.height - 1) * point / 2) * target.step + (tile.width - 1) * point / 2 * 3;
            for (int c = 0; c < 3; ++c)
            {
                hash = (hash ^ rgb[c]) * 16777619u;
            }
        }
        auto &slot = slots[(hash ^ (hash >> 16)) & 255];
        if (slot >= 0)
        {
            const auto *other = origin(slot);
            int y = 0;
            for (; y < tile.height && std::memcmp(pixels + y * target.step, other + y * target.step, bytes) == 0; ++y)
            {
            }
            if (y == tile.height)
            {
                owners[i] = slot;
                ++hits;
                continue;
            }
        }
        owners[i] = slot = i;
    }
    if (hits == 0)
    {
        owners.clear();
    }
    return hits;
}
} // namespace mosaicraft::detail
