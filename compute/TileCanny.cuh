#pragma once

// 模板生成专用：L1 Sobel、方向抑制、60/120 双阈值和格内八连通追踪。
// 各阶段在同一 stream 上；连通队列的每个弱像素只会被成功加入一次。
namespace mosaicraft::cuda
{
namespace
{
__global__ void tileGray(const uint8_t *images, int *gray, int pixels)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < pixels)
    {
        const int pos = blockIdx.y * pixels + i;
        const uint8_t *p = images + pos * 3;
        gray[pos] = (3735 * p[0] + 19235 * p[1] + 9798 * p[2] + 16384) >> 15;
    }
}
__global__ void tileGradient(const int *gray, int *gradient, int w, int h)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= w * h)
    {
        return;
    }
    const int x = i % w, y = i / w;
    const int l = max(x - 1, 0), r = min(x + 1, w - 1);
    const int base = blockIdx.y * w * h;
    const int *a = gray + base + max(0, y - 1) * w, *b = gray + base + y * w;
    const int *c = gray + base + min(h - 1, y + 1) * w;
    const int dx = a[r] - a[l] + 2 * (b[r] - b[l]) + c[r] - c[l];
    const int dy = c[l] - a[l] + 2 * (c[x] - a[x]) + c[r] - a[r];
    const int ax = abs(dx), ay = abs(dy), lo = ax * 13573;
    const int dir = (ay << 15) < lo ? 0 : (ay << 15) > lo + (ax << 16) ? 1 : (dx ^ dy) < 0 ? 2 : 3;
    gradient[base + i] = ((ax + ay) << 2) | dir;
}
__device__ int neighborMagnitude(const int *g, int x, int y, int w, int h)
{
    return x < 0 || x >= w || y < 0 || y >= h ? 0 : g[y * w + x] >> 2;
}
__global__ void tileMaxima(const int *grad, int *map, int w, int h)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= w * h)
    {
        return;
    }
    const int base = blockIdx.y * w * h;
    grad += base;
    const int val = grad[i], mag = val >> 2, dir = val & 3;
    int mark = 0;
    if (mag > 60)
    {
        const int x = i % w, y = i / w, ox = dir == 1 ? 0 : dir == 2 ? -1 : 1, oy = dir != 0;
        const int prev = neighborMagnitude(grad, x - ox, y - oy, w, h);
        const int next = neighborMagnitude(grad, x + ox, y + oy, w, h);
        if (mag > prev && (dir < 2 ? mag >= next : mag > next))
        {
            mark = mag > 120 ? 2 : 1;
        }
    }
    map[base + i] = mark;
}
__global__ void tileHysteresis(int *map, int *queue, double *edge, int w, int h)
{
    __shared__ int tail, end;
    const int n = w * h;
    map += blockIdx.x * n;
    queue += blockIdx.x * n;
    if (threadIdx.x == 0)
    {
        tail = 0;
    }
    __syncthreads();
    for (int i = threadIdx.x; i < n; i += blockDim.x)
    {
        if (map[i] == 2)
        {
            queue[atomicAdd(&tail, 1)] = i;
        }
    }
    __syncthreads();
    for (int begin = 0;;)
    {
        if (threadIdx.x == 0)
        {
            end = tail;
        }
        __syncthreads();
        if (begin == end)
        {
            break;
        }
        const int limit = end;
        for (int j = begin + threadIdx.x; j < limit; j += blockDim.x)
        {
            const int pos = queue[j], x = pos % w, y = pos / w;
            for (int ny = max(0, y - 1); ny <= min(h - 1, y + 1); ++ny)
            {
                for (int nx = max(0, x - 1); nx <= min(w - 1, x + 1); ++nx)
                {
                    const int next = ny * w + nx;
                    if (atomicCAS(map + next, 1, 2) == 1)
                    {
                        queue[atomicAdd(&tail, 1)] = next;
                    }
                }
            }
        }
        begin = limit;
        __syncthreads();
    }
    if (threadIdx.x == 0)
    {
        edge[blockIdx.x] = double(tail) / n;
    }
}
}
}
