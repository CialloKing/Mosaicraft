#pragma once
#include "CudaBackend.h"
#include <cfloat>

namespace mosaicraft::cuda
{
namespace
{
// 同一个快速内核可读常驻设备视图或临时上传视图；历史在主机逐行居中合成。
struct FastTileView
{
    const double *l, *a, *b, *edge;
    const float *grid, *lbp;
    const uint8_t *tiny;
    const double *labW, *gridW, *tinyW, *edgeW, *lbpW;
};
__device__ __constant__ float fastGridWeights[64] =
{
#include "core/GridWeights.inc"
};
__global__ void fastScoreKernel(FastTileView tile, GpuLibrary lib, const int *indices, int candidates,
                                size_t entries, float *result)
{
    const size_t pos = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
    if (pos >= entries)
    {
        return;
    }
    const int index = indices[pos];
    if (index < 0 || index >= lib.count)
    {
        result[pos] = FLT_MAX;
        return;
    }
    const size_t r = pos / candidates, i = size_t(index);
    const float l = float(tile.l[r]) - float(lib.d_lab[i * 3]);
    const float a = float(tile.a[r]) - float(lib.d_lab[i * 3 + 1]);
    const float b = float(tile.b[r]) - float(lib.d_lab[i * 3 + 2]);
    const float lab = sqrtf(l*l + a*a + b*b) / 100.0f;
    float grid = 0, lbp = 0;
    unsigned tiny = 0;
    for (int cell = 0; cell < 64; ++cell)
    {
        const float *x = tile.grid + r*192 + cell*3, *y = lib.d_grid + i*192 + cell*3;
        const float dl = x[0]-y[0], da = x[1]-y[1], db = x[2]-y[2];
        grid += sqrtf(dl*dl + da*da + db*db) * fastGridWeights[cell];
    }
    for (int bin = 0; bin < 256; ++bin)
    {
        const int diff = int(tile.tiny[r*256 + bin]) - int(lib.d_tiny[i*256 + bin]);
        tiny += unsigned(diff*diff);
        lbp += fabsf(tile.lbp[r*256 + bin] - lib.d_lbp[i*256 + bin]);
    }
    const float tinyD = !lib.d_tinyValid || lib.d_tinyValid[i] ? float(tiny)/(256.0f*255.0f*255.0f) : 1.0f;
    const float lbpD = !lib.d_lbpValid || lib.d_lbpValid[i] ? lbp/2.0f : 1.0f;
    result[pos] = float(tile.labW[r])*lab + float(tile.gridW[r])*(grid/6400.0f) + float(tile.tinyW[r])*tinyD +
        float(tile.edgeW[r])*fabsf(float(tile.edge[r]) - float(lib.d_edge[i])) + float(tile.lbpW[r])*lbpD;
}
}
}
