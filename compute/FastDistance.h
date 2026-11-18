#pragma once
#include <cstdint>

namespace mosaicraft::compute
{
// 三个固定长度的归约共用同一调度；不把布局、精度或 ISA 假设传给兼容路径。
struct DistanceSums
{
    float grid, lbp;
    unsigned tiny;
};
using ReduceDistances = DistanceSums (*)(const float *, const float *, const float *, const float *,
                                         const uint8_t *, const uint8_t *, const float *);
DistanceSums vectorDistanceSums(const float *grid, const float *otherGrid, const float *lbp,
                                const float *otherLbp, const uint8_t *tiny, const uint8_t *otherTiny,
                                const float *weights);
DistanceSums avx512DistanceSums(const float *grid, const float *otherGrid, const float *lbp,
                                const float *otherLbp, const uint8_t *tiny, const uint8_t *otherTiny,
                                const float *weights);
} // namespace mosaicraft::compute
