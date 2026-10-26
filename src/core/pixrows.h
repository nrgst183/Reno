// Row kernels for 8-bit BGRA frames: the same arithmetic as the plain loops they replace, four pixels at a time with
// SSE2 where the processor has it (every x64 one does).
#pragma once
#include <cstdint>
#include <cstddef>
#include <cmath>
// (SSE2 for GCC/clang on Linux, and for Visual C++ itself. Not for clang working against Visual C++'s headers - the cross
// build here and clang-cl - where those headers declare the intrinsics for the compiler Microsoft makes, not for clang:
// it takes the plain loops, which give the same bytes.)
#if !defined(RENO_NO_SSE2) && (defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64))
#  include <emmintrin.h>
#  define RENO_PIXROWS_SSE2 1
#endif

namespace reno {
namespace pix {

inline unsigned div255(unsigned v) { return (v + 128 + ((v + 128) >> 8)) >> 8; }

#ifdef RENO_PIXROWS_SSE2
// v / 255 rounded, for 16-bit lanes holding 0 .. 65025
inline __m128i div255v(__m128i v)
{
    v = _mm_add_epi16(v, _mm_set1_epi16(128));
    return _mm_srli_epi16(_mm_add_epi16(v, _mm_srli_epi16(v, 8)), 8);
}
// the alpha of each pixel (two in a register, as 16-bit lanes B G R A B G R A) in all four of its lanes
inline __m128i alphaLanes(__m128i p) { return _mm_shufflehi_epi16(_mm_shufflelo_epi16(p, 0xFF), 0xFF); }
#endif

// d = s over d for 'n' pixels (premultiplied), the source's strength 'op' of 255 (compositeOver's 8-bit loop)
inline void overRow(uint8_t* d, const uint8_t* s, int n, unsigned op)
{
    int x = 0;
#ifdef RENO_PIXROWS_SSE2
    const __m128i zero = _mm_setzero_si128(), c255 = _mm_set1_epi16(255), vop = _mm_set1_epi16((short)op);
    for (; x + 4 <= n; x += 4) {
        const __m128i sv = _mm_loadu_si128((const __m128i*)(s + (size_t)x * 4));
        // four solid pixels at full strength: they replace what is under them (the sum below gives the source itself)
        if (op == 255 && (_mm_movemask_epi8(_mm_cmpeq_epi8(sv, _mm_set1_epi8(-1))) & 0x8888) == 0x8888) {
            _mm_storeu_si128((__m128i*)(d + (size_t)x * 4), sv);
            continue;
        }
        const __m128i dv = _mm_loadu_si128((const __m128i*)(d + (size_t)x * 4));
        __m128i out[2];
        for (int h = 0; h < 2; ++h) {
            const __m128i s16 = h ? _mm_unpackhi_epi8(sv, zero) : _mm_unpacklo_epi8(sv, zero);
            const __m128i d16 = h ? _mm_unpackhi_epi8(dv, zero) : _mm_unpacklo_epi8(dv, zero);
            __m128i a = alphaLanes(s16), sc = s16;
            if (op != 255) {
                a = div255v(_mm_mullo_epi16(a, vop));
                sc = div255v(_mm_mullo_epi16(s16, vop));
            }
            const __m128i inv = _mm_sub_epi16(c255, a);
            const __m128i r = div255v(_mm_mullo_epi16(d16, inv));
            out[h] = _mm_min_epi16(_mm_add_epi16(sc, r), c255);
        }
        _mm_storeu_si128((__m128i*)(d + (size_t)x * 4), _mm_packus_epi16(out[0], out[1]));
    }
#endif
    d += (size_t)x * 4;
    s += (size_t)x * 4;
    for (; x < n; ++x, d += 4, s += 4) {
        // (the scalar code of compositeOver)
        unsigned sa = s[3];
        if (op == 255) {
            if (sa == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; continue; }
            if (sa == 0 && !(s[0] | s[1] | s[2])) continue;
            unsigned inv = 255 - sa;
            for (int k = 0; k < 4; ++k) {
                unsigned v = s[k] + div255(d[k] * inv);
                d[k] = (uint8_t)(v > 255 ? 255 : v);
            }
        } else {
            unsigned a = div255(sa * op);
            unsigned inv = 255 - a;
            for (int k = 0; k < 4; ++k) {
                unsigned v = div255(s[k] * op) + div255(d[k] * inv);
                d[k] = (uint8_t)(v > 255 ? 255 : v);
            }
        }
    }
}

// d = (a x ta + b x tb + 128) >> 8 for 'n' bytes (ta + tb = 256): blendImages' 8-bit loop
inline void blendRow(uint8_t* d, const uint8_t* a, const uint8_t* b, int n, unsigned ta, unsigned tb)
{
    int i = 0;
#ifdef RENO_PIXROWS_SSE2
    const __m128i zero = _mm_setzero_si128(), vta = _mm_set1_epi16((short)ta), vtb = _mm_set1_epi16((short)tb), c128 = _mm_set1_epi16(128);
    for (; i + 16 <= n; i += 16) {
        const __m128i av = _mm_loadu_si128((const __m128i*)(a + i)), bv = _mm_loadu_si128((const __m128i*)(b + i));
        __m128i out[2];
        for (int h = 0; h < 2; ++h) {
            const __m128i a16 = h ? _mm_unpackhi_epi8(av, zero) : _mm_unpacklo_epi8(av, zero);
            const __m128i b16 = h ? _mm_unpackhi_epi8(bv, zero) : _mm_unpacklo_epi8(bv, zero);
            __m128i v = _mm_add_epi16(_mm_mullo_epi16(a16, vta), _mm_mullo_epi16(b16, vtb));
            out[h] = _mm_srli_epi16(_mm_add_epi16(v, c128), 8);
        }
        _mm_storeu_si128((__m128i*)(d + i), _mm_packus_epi16(out[0], out[1]));
    }
#endif
    for (; i < n; ++i) d[i] = (uint8_t)((a[i] * ta + b[i] * tb + 128) >> 8);
}

// d += (s x m + 128) >> 8, to at most 255, for 'n' bytes (addScaled's 8-bit loop)
inline void addScaledRow(uint8_t* d, const uint8_t* s, int n, unsigned m)
{
    int i = 0;
#ifdef RENO_PIXROWS_SSE2
    const __m128i zero = _mm_setzero_si128(), vm = _mm_set1_epi16((short)m), c128 = _mm_set1_epi16(128), c255 = _mm_set1_epi16(255);
    for (; i + 16 <= n; i += 16) {
        const __m128i sv = _mm_loadu_si128((const __m128i*)(s + i)), dv = _mm_loadu_si128((const __m128i*)(d + i));
        __m128i out[2];
        for (int h = 0; h < 2; ++h) {
            const __m128i s16 = h ? _mm_unpackhi_epi8(sv, zero) : _mm_unpacklo_epi8(sv, zero);
            const __m128i d16 = h ? _mm_unpackhi_epi8(dv, zero) : _mm_unpacklo_epi8(dv, zero);
            const __m128i add = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(s16, vm), c128), 8);
            out[h] = _mm_min_epi16(_mm_add_epi16(d16, add), c255);
        }
        _mm_storeu_si128((__m128i*)(d + i), _mm_packus_epi16(out[0], out[1]));
    }
#endif
    for (; i < n; ++i) {
        const unsigned v = d[i] + ((s[i] * m + 128) >> 8);
        d[i] = (uint8_t)(v > 255 ? 255 : v);
    }
}

// the alpha byte of each of 'n' pixels set to 255 (makeOpaque)
inline void opaqueRow(uint8_t* d, int n)
{
    int x = 0;
#ifdef RENO_PIXROWS_SSE2
    const __m128i a = _mm_set1_epi32((int)0xFF000000u);
    for (; x + 4 <= n; x += 4) {
        const __m128i v = _mm_loadu_si128((const __m128i*)(d + (size_t)x * 4));
        _mm_storeu_si128((__m128i*)(d + (size_t)x * 4), _mm_or_si128(v, a));
    }
#endif
    for (; x < n; ++x) d[(size_t)x * 4 + 3] = 255;
}

// ---- OpenFX built-ins' float <-> 8-bit conversions (builtins.cpp: load / store)
// 'n' float RGBA pixels (premultiplied) to 8-bit: alpha made 0..1 (not a number: 0), a colour channel 0..alpha (not
// finite: 0), each times 255 rounded half up; 'bgra' puts red and blue in the order of a BGRA frame.
inline void floatRowToBytes(const float* p, uint8_t* o, int n, bool bgra)
{
    int x = 0;
#ifdef RENO_PIXROWS_SSE2
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.f), k255 = _mm_set1_ps(255.f), half = _mm_set1_ps(0.5f);
    const __m128 inf = _mm_castsi128_ps(_mm_set1_epi32(0x7F800000)), absMask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    const __m128 alphaLane = _mm_castsi128_ps(_mm_set_epi32(-1, 0, 0, 0));
    auto one4 = [&](const float* q) -> __m128i {
        __m128 v = _mm_loadu_ps(q);
        __m128 a = _mm_shuffle_ps(v, v, 0xFF);
        a = _mm_and_ps(a, _mm_cmpge_ps(a, zero));                          // (not a number or negative: 0)
        a = _mm_min_ps(a, one);
        __m128 c = _mm_min_ps(_mm_max_ps(v, zero), a);
        c = _mm_and_ps(c, _mm_cmplt_ps(_mm_and_ps(v, absMask), inf));      // (not finite: 0)
        c = _mm_or_ps(_mm_andnot_ps(alphaLane, c), _mm_and_ps(alphaLane, a));
        if (bgra) c = _mm_shuffle_ps(c, c, _MM_SHUFFLE(3, 0, 1, 2));
        const __m128 m = _mm_mul_ps(c, k255);
        const __m128i i = _mm_cvttps_epi32(m);
        const __m128 up = _mm_cmpge_ps(_mm_sub_ps(m, _mm_cvtepi32_ps(i)), half);
        return _mm_sub_epi32(i, _mm_castps_si128(up));                      // (the mask is -1 where the part left over is half or more)
    };
    for (; x + 4 <= n; x += 4) {
        const __m128i a = _mm_packs_epi32(one4(p + (size_t)x * 4), one4(p + (size_t)x * 4 + 4));
        const __m128i b = _mm_packs_epi32(one4(p + (size_t)x * 4 + 8), one4(p + (size_t)x * 4 + 12));
        _mm_storeu_si128((__m128i*)(o + (size_t)x * 4), _mm_packus_epi16(a, b));
    }
#endif
    for (; x < n; ++x) {
        const float* q = p + (size_t)x * 4;
        uint8_t* d = o + (size_t)x * 4;
        float a = q[3];
        if (!(a >= 0.f)) a = 0.f;
        if (a > 1.f) a = 1.f;
        auto rn = [](float v) { const int i = (int)v; return (uint8_t)(i + ((v - (float)i) >= 0.5f ? 1 : 0)); };
        auto ch = [&](float c) { c = (c - c == 0.f) ? (c < 0.f ? 0.f : c) : 0.f; if (a < c) c = a; return rn(c * 255.f); };
        d[bgra ? 2 : 0] = ch(q[0]);
        d[1] = ch(q[1]);
        d[bgra ? 0 : 2] = ch(q[2]);
        d[3] = rn(a * 255.f);
    }
}

// 'n' 8-bit RGBA / BGRA pixels to floats (value x (1 / 255)), red first.
inline void bytesRowToFloat(const uint8_t* s, float* d, int n, bool bgra)
{
    int x = 0;
    const float k = 1.f / 255.f;
#ifdef RENO_PIXROWS_SSE2
    const __m128i zero = _mm_setzero_si128();
    const __m128 vk = _mm_set1_ps(k);
    for (; x + 4 <= n; x += 4) {
        const __m128i v = _mm_loadu_si128((const __m128i*)(s + (size_t)x * 4));
        const __m128i lo = _mm_unpacklo_epi8(v, zero), hi = _mm_unpackhi_epi8(v, zero);
        __m128 f[4] = {_mm_mul_ps(_mm_cvtepi32_ps(_mm_unpacklo_epi16(lo, zero)), vk), _mm_mul_ps(_mm_cvtepi32_ps(_mm_unpackhi_epi16(lo, zero)), vk),
                       _mm_mul_ps(_mm_cvtepi32_ps(_mm_unpacklo_epi16(hi, zero)), vk), _mm_mul_ps(_mm_cvtepi32_ps(_mm_unpackhi_epi16(hi, zero)), vk)};
        for (int j = 0; j < 4; ++j) {
            if (bgra) f[j] = _mm_shuffle_ps(f[j], f[j], _MM_SHUFFLE(3, 0, 1, 2));
            _mm_storeu_ps(d + (size_t)(x + j) * 4, f[j]);
        }
    }
#endif
    for (; x < n; ++x) {
        const uint8_t* q = s + (size_t)x * 4;
        float* o = d + (size_t)x * 4;
        o[0] = q[bgra ? 2 : 0] * k;
        o[1] = q[1] * k;
        o[2] = q[bgra ? 0 : 2] * k;
        o[3] = q[3] * k;
    }
}

// True when none of the n floats is an infinity or a NaN (the effects that shortcut sums over terms with weight 0 first check
// that their picture has only numbers in it: 0 times a number is 0, but 0 times an infinity is not)
inline bool allFinite(const float* p, size_t n)
{
    size_t i = 0;
    bool ok = true;
#ifdef RENO_PIXROWS_SSE2
    const __m128i expo = _mm_set1_epi32(0x7F800000);
    __m128i bad = _mm_setzero_si128();
    for (; i + 4 <= n; i += 4) {
        const __m128i v = _mm_and_si128(_mm_castps_si128(_mm_loadu_ps(p + i)), expo);
        bad = _mm_or_si128(bad, _mm_cmpeq_epi32(v, expo));
    }
    ok = _mm_movemask_epi8(bad) == 0;
#endif
    for (; i < n; ++i) ok = ok && std::isfinite(p[i]);
    return ok;
}

}  // namespace pix
}  // namespace reno
