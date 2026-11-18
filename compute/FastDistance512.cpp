#include "FastDistance.h"
#include <immintrin.h>

namespace mosaicraft::compute
{
namespace
{
// 保留 AVX2 的八个累加通道和加法顺序，避免用改变 Top-N 平分结果换取吞吐。
__m256 accumulate(__m256 sum, __m512 values)
{
    return _mm256_add_ps(_mm256_add_ps(sum, _mm512_castps512_ps256(values)), _mm512_extractf32x8_ps(values, 1));
}
}

template<bool Planar>
DistanceSums avx512DistanceSums(const float *grid, const float *otherGrid, const float *lbp,
                                const float *otherLbp, const uint8_t *tiny, const uint8_t *otherTiny,
                                const float *weights)
{
    const auto lanes = _mm512_setr_epi32(0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 42, 45);
    auto g = _mm256_setzero_ps(), h = g;
    auto t = _mm512_setzero_si512();
    for (int i = 0; i < 64; i += 16)
    {
        const auto channel = [&](int c)
        {
            if constexpr (Planar)
            {
                return _mm512_sub_ps(_mm512_loadu_ps(grid + 64 * c + i), _mm512_loadu_ps(otherGrid + 64 * c + i));
            }
            return _mm512_sub_ps(_mm512_i32gather_ps(lanes, grid + 3 * i + c, 4),
                                 _mm512_i32gather_ps(lanes, otherGrid + 3 * i + c, 4));
        };
        const auto l = channel(0), a = channel(1), b = channel(2);
        const auto d = _mm512_sqrt_ps(
            _mm512_add_ps(_mm512_add_ps(_mm512_mul_ps(l, l), _mm512_mul_ps(a, a)), _mm512_mul_ps(b, b)));
        g = accumulate(g, _mm512_mul_ps(d, _mm512_loadu_ps(weights + i)));
    }
    for (int i = 0; i < 256; i += 32)
    {
        const auto diff = _mm512_sub_epi16(
            _mm512_cvtepu8_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(tiny + i))),
            _mm512_cvtepu8_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(otherTiny + i))));
        t = _mm512_add_epi32(t, _mm512_madd_epi16(diff, diff));
        for (int j = i; j < i + 32; j += 16)
        {
            const auto delta = _mm512_sub_ps(_mm512_loadu_ps(lbp + j), _mm512_loadu_ps(otherLbp + j));
            h = accumulate(h, _mm512_abs_ps(delta));
        }
    }
    alignas(32) float gs[8], hs[8];
    _mm256_store_ps(gs, g);
    _mm256_store_ps(hs, h);
    DistanceSums sums{};
    sums.tiny = static_cast<unsigned>(_mm512_reduce_add_epi32(t));
    for (int i = 0; i < 8; ++i)
    {
        sums.grid += gs[i];
        sums.lbp += hs[i];
    }
    return sums;
}
template DistanceSums avx512DistanceSums<true>(const float *, const float *, const float *, const float *,
    const uint8_t *, const uint8_t *, const float *);
template DistanceSums avx512DistanceSums<false>(const float *, const float *, const float *, const float *,
    const uint8_t *, const uint8_t *, const float *);
} // namespace mosaicraft::compute
