// The compositor's blend modes (everything but Over) on 8-bit premultiplied BGRA rows, four pixels at a time with SSE2.
// The arithmetic and its order are those of the scalar loop in engine_video.cpp (blendRows), so the bytes are the same.
#pragma once
#include "../core/pixrows.h"
#include <algorithm>

namespace reno {

enum class Blend { Over, Add, Subtract, Multiply, Screen, Overlay, HardLight, Dodge, Burn, Darken, Lighten, Difference, DiffSquared, Cut };

namespace pix {

#ifdef RENO_PIXROWS_SSE2
template <Blend M>
inline __m128 blendChannelV(__m128 s, __m128 d)
{
    const __m128 one = _mm_set1_ps(1.f), zero = _mm_setzero_ps(), half = _mm_set1_ps(0.5f), two = _mm_set1_ps(2.f);
    auto pick = [](__m128 mask, __m128 a, __m128 b) { return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b)); };
    switch (M) {
    case Blend::Add: return _mm_min_ps(_mm_add_ps(s, d), one);
    case Blend::Subtract: return _mm_max_ps(_mm_sub_ps(d, s), zero);
    case Blend::Multiply: return _mm_mul_ps(s, d);
    case Blend::Screen: return _mm_sub_ps(_mm_add_ps(s, d), _mm_mul_ps(s, d));
    case Blend::Overlay: {
        const __m128 lo = _mm_mul_ps(_mm_mul_ps(two, s), d);
        const __m128 hi = _mm_sub_ps(one, _mm_mul_ps(_mm_mul_ps(two, _mm_sub_ps(one, s)), _mm_sub_ps(one, d)));
        return pick(_mm_cmple_ps(d, half), lo, hi);
    }
    case Blend::HardLight: {
        const __m128 lo = _mm_mul_ps(_mm_mul_ps(two, s), d);
        const __m128 hi = _mm_sub_ps(one, _mm_mul_ps(_mm_mul_ps(two, _mm_sub_ps(one, s)), _mm_sub_ps(one, d)));
        return pick(_mm_cmple_ps(s, half), lo, hi);
    }
    case Blend::Dodge: {
        const __m128 q = _mm_min_ps(_mm_div_ps(d, _mm_sub_ps(one, s)), one);
        return pick(_mm_cmple_ps(d, zero), zero, pick(_mm_cmpge_ps(s, one), one, q));
    }
    case Blend::Burn: {
        const __m128 q = _mm_sub_ps(one, _mm_min_ps(_mm_div_ps(_mm_sub_ps(one, d), s), one));
        return pick(_mm_cmpge_ps(d, one), one, pick(_mm_cmple_ps(s, zero), zero, q));
    }
    case Blend::Darken: return _mm_min_ps(d, s);
    case Blend::Lighten: return _mm_max_ps(d, s);
    case Blend::Difference: return _mm_andnot_ps(_mm_set1_ps(-0.f), _mm_sub_ps(s, d));
    case Blend::DiffSquared: {
        const __m128 x = _mm_sub_ps(s, d);
        return _mm_mul_ps(x, x);
    }
    default: return s;
    }
}

// Blends the first n - n % 4 pixels of a row; returns how many it did (the caller does the rest).
template <Blend M>
inline int blendModeRow(uint8_t* d, const uint8_t* s, int n, float opacity)
{
    const __m128 k = _mm_set1_ps(1.f / 255.f), op = _mm_set1_ps(opacity), zero = _mm_setzero_ps(), one = _mm_set1_ps(1.f);
    const __m128 c255 = _mm_set1_ps(255.f), half = _mm_set1_ps(0.5f);
    const __m128i ff = _mm_set1_epi32(0xFF);
    int x = 0;
    for (; x + 4 <= n; x += 4) {
        const __m128i sv = _mm_loadu_si128((const __m128i*)(s + (size_t)x * 4));
        const __m128 sa = _mm_mul_ps(_mm_mul_ps(_mm_cvtepi32_ps(_mm_srli_epi32(sv, 24)), k), op);
        const __m128 live = _mm_cmpgt_ps(sa, zero);
        const int liveBits = _mm_movemask_ps(live);
        if (!liveBits) continue;
        const __m128i dv = _mm_loadu_si128((const __m128i*)(d + (size_t)x * 4));
        const __m128 da = _mm_mul_ps(_mm_cvtepi32_ps(_mm_srli_epi32(dv, 24)), k);
        const __m128 isa = _mm_sub_ps(one, sa);
        __m128 out[4];
        // solid over solid: the weights of the colours under and over are exactly 0, 0 and 1, so only the mode's colour is left
        if (M != Blend::Cut && _mm_movemask_ps(_mm_and_ps(_mm_cmpeq_ps(sa, one), _mm_cmpeq_ps(da, one))) == 15) {
            for (int c = 0; c < 3; ++c) {
                const __m128 sp = _mm_mul_ps(_mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(sv, 8 * c), ff)), k), op);
                const __m128 dp = _mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(dv, 8 * c), ff)), k);
                out[c] = blendChannelV<M>(_mm_min_ps(sp, one), _mm_min_ps(dp, one));
            }
            out[3] = one;
        } else if (M == Blend::Cut) {
            for (int c = 0; c < 4; ++c) {
                const __m128 dc = _mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(dv, 8 * c), ff));
                out[c] = _mm_mul_ps(_mm_mul_ps(dc, k), isa);
            }
        } else {
            const __m128 ida = _mm_sub_ps(one, da), sada = _mm_mul_ps(sa, da), dlive = _mm_cmpgt_ps(da, zero);
            for (int c = 0; c < 3; ++c) {
                const __m128 sp = _mm_mul_ps(_mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(sv, 8 * c), ff)), k), op);
                const __m128 dp = _mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(dv, 8 * c), ff)), k);
                const __m128 sc = _mm_min_ps(_mm_div_ps(sp, sa), one);
                const __m128 dc = _mm_and_ps(dlive, _mm_min_ps(_mm_div_ps(dp, da), one));
                const __m128 mixed = _mm_add_ps(_mm_mul_ps(sp, ida), _mm_mul_ps(dp, isa));
                out[c] = _mm_add_ps(mixed, _mm_mul_ps(sada, blendChannelV<M>(sc, dc)));
            }
            out[3] = _mm_sub_ps(_mm_add_ps(sa, da), sada);
        }
        __m128 v[4];
        for (int c = 0; c < 4; ++c) v[c] = _mm_max_ps(_mm_min_ps(_mm_add_ps(_mm_mul_ps(out[c], c255), half), c255), zero);
        for (int c = 0; c < 3; ++c) v[c] = _mm_min_ps(v[c], v[3]);
        __m128i res = _mm_cvttps_epi32(v[0]);
        res = _mm_or_si128(res, _mm_slli_epi32(_mm_cvttps_epi32(v[1]), 8));
        res = _mm_or_si128(res, _mm_slli_epi32(_mm_cvttps_epi32(v[2]), 16));
        res = _mm_or_si128(res, _mm_slli_epi32(_mm_cvttps_epi32(v[3]), 24));
        if (liveBits != 15) {
            const __m128i m = _mm_castps_si128(live);
            res = _mm_or_si128(_mm_and_si128(m, res), _mm_andnot_si128(m, dv));
        }
        _mm_storeu_si128((__m128i*)(d + (size_t)x * 4), res);
    }
    return x;
}
#else
template <Blend M>
inline int blendModeRow(uint8_t*, const uint8_t*, int, float) { return 0; }
#endif

}  // namespace pix
}  // namespace reno
