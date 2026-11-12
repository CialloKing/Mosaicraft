#include <metal_stdlib>
using namespace metal;

kernel void features(device const uint *pixels [[buffer(0)]], device uint *partials [[buffer(1)]],
                     constant uint4 &p [[buffer(4)]], uint tid [[thread_index_in_threadgroup]],
                     uint2 group [[threadgroup_position_in_grid]])
{
    threadgroup atomic_uint sums[451];
    for (uint i = tid; i < 451; i += 128)
    {
        atomic_store_explicit(sums + i, 0u, memory_order_relaxed);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint width = p.x, height = p.y, size = width * height;
    uint base = group.y * size, end = min((group.x + 1) * 4096, size);
    uint cellW = (width + 7) / 8, cellH = (height + 7) / 8;
    for (uint i = group.x * 4096 + tid; i < end; i += 128)
    {
        uint v = pixels[base + i];
        atomic_fetch_add_explicit(sums, v & 255, memory_order_relaxed);
        atomic_fetch_add_explicit(sums + 1, (v >> 8) & 255, memory_order_relaxed);
        atomic_fetch_add_explicit(sums + 2, (v >> 16) & 255, memory_order_relaxed);
        uint x = i % width, y = i / width;
        if (width >= 8 && height >= 8)
        {
            uint cell = (y / cellH * 8 + x / cellW) * 3 + 3;
            atomic_fetch_add_explicit(sums + cell, v & 255, memory_order_relaxed);
            atomic_fetch_add_explicit(sums + cell + 1, (v >> 8) & 255, memory_order_relaxed);
            atomic_fetch_add_explicit(sums + cell + 2, (v >> 16) & 255, memory_order_relaxed);
        }
        if (x > 0 && y > 0 && x + 1 < width && y + 1 < height)
        {
            uint c = v >> 24;
            uint code = uint((pixels[base + i - width - 1] >> 24) >= c) |
                (uint((pixels[base + i - width] >> 24) >= c) << 1) |
                (uint((pixels[base + i - width + 1] >> 24) >= c) << 2) |
                (uint((pixels[base + i + 1] >> 24) >= c) << 3) |
                (uint((pixels[base + i + width + 1] >> 24) >= c) << 4) |
                (uint((pixels[base + i + width] >> 24) >= c) << 5) |
                (uint((pixels[base + i + width - 1] >> 24) >= c) << 6) |
                (uint((pixels[base + i - 1] >> 24) >= c) << 7);
            atomic_fetch_add_explicit(sums + 195 + code, 1u, memory_order_relaxed);
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint outBase = (group.y * p.z + group.x) * 451;
    for (uint i = tid; i < 451; i += 128)
    {
        partials[outBase + i] = atomic_load_explicit(sums + i, memory_order_relaxed);
    }
}

constant float weights[64] =
{
#include "GridWeights.inc"
};

kernel void scoring(device const uint *library [[buffer(0)]], device const uint *tiles [[buffer(1)]],
                    device const int *indices [[buffer(2)]], device float *distances [[buffer(3)]],
                    constant uint4 &p [[buffer(4)]], uint position [[thread_position_in_grid]])
{
    if (position >= p.x * p.y)
    {
        return;
    }
    int index = indices[position];
    if (index < 0 || uint(index) >= p.z)
    {
        for (uint i = 0; i < 5; ++i)
        {
            distances[position * 5 + i] = 0;
        }
        return;
    }
    uint a = (position / p.y) * 518, b = uint(index) * 518;
    float dl = as_type<float>(tiles[a]) - as_type<float>(library[b]);
    float da = as_type<float>(tiles[a + 1]) - as_type<float>(library[b + 1]);
    float db = as_type<float>(tiles[a + 2]) - as_type<float>(library[b + 2]);
    float lab = sqrt(dl * dl + da * da + db * db) / 100.0f;
    float grid = 0, tiny = 0, lbp = 0;
    for (uint cell = 0; cell < 64; ++cell)
    {
        uint off = 4 + cell * 3;
        dl = as_type<float>(tiles[a + off]) - as_type<float>(library[b + off]);
        da = as_type<float>(tiles[a + off + 1]) - as_type<float>(library[b + off + 1]);
        db = as_type<float>(tiles[a + off + 2]) - as_type<float>(library[b + off + 2]);
        grid += sqrt(dl * dl + da * da + db * db) * weights[cell];
    }
    for (uint bin = 0; bin < 256; ++bin)
    {
        uint shift = (bin % 4) * 8;
        float diff = float((tiles[a + 196 + bin / 4] >> shift) & 255) -
                     float((library[b + 196 + bin / 4] >> shift) & 255);
        tiny += diff * diff;
        lbp += abs(as_type<float>(tiles[a + 260 + bin]) - as_type<float>(library[b + 260 + bin]));
    }
    distances[position * 5] = lab;
    distances[position * 5 + 1] = grid / 64.0f / 100.0f;
    distances[position * 5 + 2] = library[b + 516] != 0 ? tiny / (256.0f * 255.0f * 255.0f) : 1.0f;
    distances[position * 5 + 3] = abs(as_type<float>(tiles[a + 3]) - as_type<float>(library[b + 3]));
    distances[position * 5 + 4] = library[b + 517] != 0 ? lbp / 2.0f : 1.0f;
}
