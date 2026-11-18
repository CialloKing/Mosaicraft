#pragma once
#include <cuda_runtime.h>
#include <cstdint>

namespace mosaicraft::cuda
{
// Both CUDA feature paths must clamp interpolation to the tile, not the parent image.
__device__ inline void resizeTilePixel(const uint8_t *src, size_t stride, int tileW, int tileH,
                                       int xOut, int yOut, double scaleX, double scaleY, uint8_t *bgr)
{
    const float fx = static_cast<float>((xOut + 0.5) * scaleX - 0.5);
    const float fy = static_cast<float>((yOut + 0.5) * scaleY - 0.5);
    const int x = __float2int_rd(fx), y = __float2int_rd(fy);
    const int x0 = max(0, min(x, tileW - 1)), x1 = max(0, min(x + 1, tileW - 1));
    const int y0 = max(0, min(y, tileH - 1)), y1 = max(0, min(y + 1, tileH - 1));
    const int ax = __float2int_rn((fx - x) * 2048), ay = __float2int_rn((fy - y) * 2048);
    for (int k = 0; k < 3; ++k)
    {
        const auto *a = src + size_t(y0) * stride + k, *b = src + size_t(y1) * stride + k;
        const int top = ((2048 - ax) * a[x0 * 3] + ax * a[x1 * 3]) >> 4;
        const int bot = ((2048 - ax) * b[x0 * 3] + ax * b[x1 * 3]) >> 4;
        bgr[k] = static_cast<uint8_t>((((2048 - ay) * top >> 16) + (ay * bot >> 16) + 2) >> 2);
    }
}
}
