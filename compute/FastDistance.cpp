#include "FastDistance.h"
#if defined(MOSAICRAFT_AVX2_DISTANCE)
#include <immintrin.h>
#else
#include <arm_neon.h>
#endif

namespace mosaicraft::compute
{
template<bool Planar>
DistanceSums vectorDistanceSums(const float *grid, const float *otherGrid, const float *lbp,
                                const float *otherLbp, const uint8_t *tiny, const uint8_t *otherTiny,
                                const float *weights)
{
#if defined(MOSAICRAFT_AVX2_DISTANCE)
    const auto lanes = _mm256_setr_epi32(0, 3, 6, 9, 12, 15, 18, 21);
    auto g = _mm256_setzero_ps(), h = g;
    auto t = _mm256_setzero_si256();
    for (int i = 0; i < 64; i += 8)
    {
        const auto channel = [&](int c)
        {
            if constexpr (Planar)
            {
                return _mm256_sub_ps(_mm256_loadu_ps(grid + 64 * c + i), _mm256_loadu_ps(otherGrid + 64 * c + i));
            }
            return _mm256_sub_ps(_mm256_i32gather_ps(grid + 3 * i + c, lanes, 4),
                                 _mm256_i32gather_ps(otherGrid + 3 * i + c, lanes, 4));
        };
        const auto l = channel(0), a = channel(1), b = channel(2);
        const auto d = _mm256_sqrt_ps(
            _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(l, l), _mm256_mul_ps(a, a)), _mm256_mul_ps(b, b)));
        g = _mm256_add_ps(g, _mm256_mul_ps(d, _mm256_loadu_ps(weights + i)));
    }
    for (int i = 0; i < 256; i += 16)
    {
        const auto diff = _mm256_sub_epi16(
            _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i *>(tiny + i))),
            _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i *>(otherTiny + i))));
        t = _mm256_add_epi32(t, _mm256_madd_epi16(diff, diff));
        for (int j = i; j < i + 16; j += 8)
        {
            const auto delta = _mm256_sub_ps(_mm256_loadu_ps(lbp + j), _mm256_loadu_ps(otherLbp + j));
            h = _mm256_add_ps(h, _mm256_andnot_ps(_mm256_set1_ps(-0.0f), delta));
        }
    }
    alignas(32) float gs[8], hs[8];
    alignas(32) unsigned ts[8];
    _mm256_store_ps(gs, g);
    _mm256_store_ps(hs, h);
    _mm256_store_si256(reinterpret_cast<__m256i *>(ts), t);
    DistanceSums sums{};
    for (int i = 0; i < 8; ++i)
    {
        sums.grid += gs[i];
        sums.lbp += hs[i];
        sums.tiny += ts[i];
    }
    return sums;
#else
    auto g = vdupq_n_f32(0), h = g;
    auto t = vdupq_n_u32(0);
    for (int i = 0; i < 64; i += 4)
    {
        const auto load = [&](const float *values)
        {
            if constexpr (Planar)
            {
                float32x4x3_t channels;
                for (int c = 0; c < 3; ++c)
                {
                    channels.val[c] = vld1q_f32(values + 64 * c + i);
                }
                return channels;
            }
            return vld3q_f32(values + i * 3);
        };
        const auto x = load(grid), y = load(otherGrid);
        const auto l = vsubq_f32(x.val[0], y.val[0]), a = vsubq_f32(x.val[1], y.val[1]),
                   b = vsubq_f32(x.val[2], y.val[2]);
        const auto d = vsqrtq_f32(vaddq_f32(vaddq_f32(vmulq_f32(l, l), vmulq_f32(a, a)), vmulq_f32(b, b)));
        g = vaddq_f32(g, vmulq_f32(d, vld1q_f32(weights + i)));
    }
    for (int i = 0; i < 256; i += 8)
    {
        const auto diff = vabdl_u8(vld1_u8(tiny + i), vld1_u8(otherTiny + i));
        t = vaddq_u32(t, vmull_u16(vget_low_u16(diff), vget_low_u16(diff)));
        t = vaddq_u32(t, vmull_u16(vget_high_u16(diff), vget_high_u16(diff)));
        for (int j = i; j < i + 8; j += 4)
        {
            h = vaddq_f32(h, vabdq_f32(vld1q_f32(lbp + j), vld1q_f32(otherLbp + j)));
        }
    }
    return {vaddvq_f32(g), vaddvq_f32(h), vaddvq_u32(t)};
#endif
}
template DistanceSums vectorDistanceSums<true>(const float *, const float *, const float *, const float *,
    const uint8_t *, const uint8_t *, const float *);
template DistanceSums vectorDistanceSums<false>(const float *, const float *, const float *, const float *,
    const uint8_t *, const uint8_t *, const float *);
} // namespace mosaicraft::compute
