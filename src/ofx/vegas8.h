// Small helpers for the built-ins that reproduce VEGAS's 8-bit processing exactly (the integer/float steps are copied from VEGAS's own code,
// so the order of the operations and the roundings matter: do not "simplify" them).
#pragma once
#include <cstdint>
#include <cstring>
#include <cstddef>
#include "../core/pixrows.h"

namespace reno {
namespace ofx {
namespace builtin {
namespace v8 {

// A float picture value (0..1, from an 8-bit picture) as the 8-bit level it came from.
inline int toLevel(float v)
{
    v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    const float s = v * 255.f;
    const int i = (int)s;
    return i + ((s - (float)i) >= 0.5f ? 1 : 0);
}
// toLevel on 'n' floats (not a number: 0), four at a time where SSE2 is there; the same bytes as the plain loop
inline void levelsRow(const float* q, uint8_t* d, size_t n)
{
    size_t i = 0;
#ifdef RENO_PIXROWS_SSE2
    const __m128 zerof = _mm_setzero_ps(), one = _mm_set1_ps(1.f), k255 = _mm_set1_ps(255.f), half = _mm_set1_ps(0.5f);
    auto lv = [&](const float* p) {
        __m128 v = _mm_loadu_ps(p);
        v = _mm_andnot_ps(_mm_cmplt_ps(v, zerof), v);
        const __m128 gt = _mm_cmpgt_ps(v, one);
        v = _mm_or_ps(_mm_and_ps(gt, one), _mm_andnot_ps(gt, v));
        const __m128 s = _mm_mul_ps(v, k255);
        const __m128i t = _mm_cvttps_epi32(s);
        const __m128 up = _mm_cmpge_ps(_mm_sub_ps(s, _mm_cvtepi32_ps(t)), half);
        return _mm_sub_epi32(t, _mm_castps_si128(up));
    };
    for (; i + 16 <= n; i += 16) {
        const __m128i a = _mm_packs_epi32(lv(q + i), lv(q + i + 4)), b = _mm_packs_epi32(lv(q + i + 8), lv(q + i + 12));
        _mm_storeu_si128((__m128i*)(d + i), _mm_packus_epi16(a, b));
    }
#endif
    for (; i < n; ++i) d[i] = (uint8_t)toLevel(q[i]);
}
inline float fromLevel(int b) { return (float)b * (1.f / 255.f); }

// VEGAS's way of rounding a float to a whole number: x + 1.5e-8 in double, then the 2^52 + 2^51 trick (round to nearest, ties to even, on the nudged value).
// The nudge lifts exact halves up. Valid for |x| < 2^31.
inline int roundMagic(double x)
{
    double d = x + 1.5e-8;
    d = d + 6755399441055744.0;
    int64_t bits;
    std::memcpy(&bits, &d, 8);
    return (int)(int32_t)(bits & 0xffffffff);
}

}  // namespace v8
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
