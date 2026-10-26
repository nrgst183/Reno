// Four floats at a time for the fast built-in effects (builtins_fast*.cpp): SSE2 where the compiler has it (see
// core/pixrows.h for when), else four plain floats.
#pragma once
#include "../core/pixrows.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
namespace v4 {

#ifdef RENO_PIXROWS_SSE2
typedef __m128 V4;
inline V4 ld(const float* p) { return _mm_loadu_ps(p); }
inline void st(float* p, V4 v) { _mm_storeu_ps(p, v); }
inline V4 add(V4 a, V4 b) { return _mm_add_ps(a, b); }
inline V4 sub(V4 a, V4 b) { return _mm_sub_ps(a, b); }
inline V4 mul(V4 a, V4 b) { return _mm_mul_ps(a, b); }
inline V4 vmin(V4 a, V4 b) { return _mm_min_ps(a, b); }
inline V4 vmax(V4 a, V4 b) { return _mm_max_ps(a, b); }
inline V4 set(float x) { return _mm_set1_ps(x); }
inline V4 zero() { return _mm_setzero_ps(); }
inline V4 div(V4 a, V4 b) { return _mm_div_ps(a, b); }
inline V4 vsqrt(V4 a) { return _mm_sqrt_ps(a); }
// Comparisons give a mask: all bits of a lane set where it holds.
inline V4 lt(V4 a, V4 b) { return _mm_cmplt_ps(a, b); }
inline V4 le(V4 a, V4 b) { return _mm_cmple_ps(a, b); }
inline V4 gt(V4 a, V4 b) { return _mm_cmpgt_ps(a, b); }
inline V4 ge(V4 a, V4 b) { return _mm_cmpge_ps(a, b); }
inline V4 eq(V4 a, V4 b) { return _mm_cmpeq_ps(a, b); }
inline V4 vand(V4 a, V4 b) { return _mm_and_ps(a, b); }
inline V4 vandnot(V4 a, V4 b) { return _mm_andnot_ps(a, b); }      // (not a) and b
inline V4 vor(V4 a, V4 b) { return _mm_or_ps(a, b); }
inline V4 vabs(V4 a) { return _mm_andnot_ps(_mm_set1_ps(-0.0f), a); }
// Lanes of 'a' where the mask is set, else of 'b'.
inline V4 blend(V4 m, V4 a, V4 b) { return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, b)); }
inline int movemask(V4 m) { return _mm_movemask_ps(m); }          // bit k: lane k set
inline void lanes(V4 a, float out[4]) { _mm_storeu_ps(out, a); }
// Four RGBA pixels (16 floats) into a vector per channel and back.
inline void load4(const float* p, V4& r, V4& g, V4& b, V4& a)
{
    r = _mm_loadu_ps(p); g = _mm_loadu_ps(p + 4); b = _mm_loadu_ps(p + 8); a = _mm_loadu_ps(p + 12);
    _MM_TRANSPOSE4_PS(r, g, b, a);
}
inline void store4(float* p, V4 r, V4 g, V4 b, V4 a)
{
    _MM_TRANSPOSE4_PS(r, g, b, a);
    _mm_storeu_ps(p, r); _mm_storeu_ps(p + 4, g); _mm_storeu_ps(p + 8, b); _mm_storeu_ps(p + 12, a);
}
#else
struct V4 { float v[4]; };
inline V4 ld(const float* p) { V4 r; for (int i = 0; i < 4; ++i) r.v[i] = p[i]; return r; }
inline void st(float* p, V4 a) { for (int i = 0; i < 4; ++i) p[i] = a.v[i]; }
inline V4 add(V4 a, V4 b) { for (int i = 0; i < 4; ++i) a.v[i] += b.v[i]; return a; }
inline V4 sub(V4 a, V4 b) { for (int i = 0; i < 4; ++i) a.v[i] -= b.v[i]; return a; }
inline V4 mul(V4 a, V4 b) { for (int i = 0; i < 4; ++i) a.v[i] *= b.v[i]; return a; }
inline V4 vmin(V4 a, V4 b) { for (int i = 0; i < 4; ++i) a.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; return a; }
inline V4 vmax(V4 a, V4 b) { for (int i = 0; i < 4; ++i) a.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; return a; }
inline V4 set(float x) { V4 r; for (int i = 0; i < 4; ++i) r.v[i] = x; return r; }
inline V4 zero() { return set(0.f); }
inline V4 div(V4 a, V4 b) { for (int i = 0; i < 4; ++i) a.v[i] /= b.v[i]; return a; }
inline V4 vsqrt(V4 a) { for (int i = 0; i < 4; ++i) a.v[i] = std::sqrt(a.v[i]); return a; }
// (a mask is a lane of all bits set or none, kept in a float's bits)
inline float maskBits(bool on) { uint32_t u = on ? 0xFFFFFFFFu : 0u; float f; std::memcpy(&f, &u, 4); return f; }
inline uint32_t bitsOf(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float fromBits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
inline V4 lt(V4 a, V4 b) { V4 r; for (int i = 0; i < 4; ++i) r.v[i] = maskBits(a.v[i] < b.v[i]); return r; }
inline V4 le(V4 a, V4 b) { V4 r; for (int i = 0; i < 4; ++i) r.v[i] = maskBits(a.v[i] <= b.v[i]); return r; }
inline V4 gt(V4 a, V4 b) { V4 r; for (int i = 0; i < 4; ++i) r.v[i] = maskBits(a.v[i] > b.v[i]); return r; }
inline V4 ge(V4 a, V4 b) { V4 r; for (int i = 0; i < 4; ++i) r.v[i] = maskBits(a.v[i] >= b.v[i]); return r; }
inline V4 eq(V4 a, V4 b) { V4 r; for (int i = 0; i < 4; ++i) r.v[i] = maskBits(a.v[i] == b.v[i]); return r; }
inline V4 vand(V4 a, V4 b) { V4 r; for (int i = 0; i < 4; ++i) r.v[i] = fromBits(bitsOf(a.v[i]) & bitsOf(b.v[i])); return r; }
inline V4 vandnot(V4 a, V4 b) { V4 r; for (int i = 0; i < 4; ++i) r.v[i] = fromBits(~bitsOf(a.v[i]) & bitsOf(b.v[i])); return r; }
inline V4 vor(V4 a, V4 b) { V4 r; for (int i = 0; i < 4; ++i) r.v[i] = fromBits(bitsOf(a.v[i]) | bitsOf(b.v[i])); return r; }
inline V4 vabs(V4 a) { for (int i = 0; i < 4; ++i) a.v[i] = fromBits(bitsOf(a.v[i]) & 0x7FFFFFFFu); return a; }
inline V4 blend(V4 m, V4 a, V4 b) { return vor(vand(m, a), vandnot(m, b)); }
inline int movemask(V4 m) { int r = 0; for (int i = 0; i < 4; ++i) r |= (int)(bitsOf(m.v[i]) >> 31) << i; return r; }
inline void lanes(V4 a, float out[4]) { for (int i = 0; i < 4; ++i) out[i] = a.v[i]; }
inline void load4(const float* p, V4& r, V4& g, V4& b, V4& a)
{
    for (int i = 0; i < 4; ++i) { r.v[i] = p[i * 4]; g.v[i] = p[i * 4 + 1]; b.v[i] = p[i * 4 + 2]; a.v[i] = p[i * 4 + 3]; }
}
inline void store4(float* p, V4 r, V4 g, V4 b, V4 a)
{
    for (int i = 0; i < 4; ++i) { p[i * 4] = r.v[i]; p[i * 4 + 1] = g.v[i]; p[i * 4 + 2] = b.v[i]; p[i * 4 + 3] = a.v[i]; }
}
#endif

// a + (b - a) t
inline V4 lerp(V4 a, V4 b, float t) { return add(a, mul(sub(b, a), set(t))); }

// The frame's pixel (x, y) as a vector.
inline V4 pix(const float* px, int w, int x, int y) { return ld(px + ((size_t)y * w + (size_t)x) * 4); }

// Bilinear sample of RGBA floats at (x, y) given in pixel-index coordinates (pixel centres at whole numbers), which must
// already lie in [0, w - 1] x [0, h - 1] (edge clamping is the caller's clamp).
inline V4 bilinear(const float* px, int w, int h, float x, float y)
{
    const int x0 = (int)x, y0 = (int)y;
    const float fx = x - (float)x0, fy = y - (float)y0;
    const int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
    const V4 top = lerp(pix(px, w, x0, y0), pix(px, w, x1, y0), fx);
    const V4 bot = lerp(pix(px, w, x0, y1), pix(px, w, x1, y1), fx);
    return lerp(top, bot, fy);
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// clamp01 of builtins_impl.h for four lanes (a value of -0 stays -0, as there)
inline V4 clamp01(V4 v) { return vmin(vmax(zero(), v), set(1.f)); }

// ---- per-pixel colour effects, four pixels at a time
// Runs fn(r, g, b) (vectors: a colour channel of four pixels, straight colour) over 'n' premultiplied RGBA pixels from s to o, with exactly what perPixel() in builtins_filters.cpp does per pixel:
// nothing for a pixel with no alpha (all zeros), colour divided by alpha (zero where alpha is tiny), fn, colour times alpha. The last pixels of a row go through a copy.
template <class F>
inline void pixelRow(const float* s, float* o, int n, F fn)
{
    const V4 z = zero(), one = set(1.f), tiny = set(1e-6f);
    for (int x = 0; x < n; x += 4) {
        float tmpIn[16], tmpOut[16];
        const float* ps = s + (size_t)x * 4;
        float* po = o + (size_t)x * 4;
        const bool part = x + 4 > n;
        if (part) {
            std::memset(tmpIn, 0, sizeof tmpIn);
            std::memcpy(tmpIn, ps, (size_t)(n - x) * 4 * sizeof(float));
            ps = tmpIn;
            po = tmpOut;
        }
        V4 r, g, b, a;
        load4(ps, r, g, b, a);
        const V4 valid = gt(a, z);
        if (!movemask(valid)) {
            st(po, z); st(po + 4, z); st(po + 8, z); st(po + 12, z);
        } else {
            const bool opaque = movemask(eq(a, one)) == 15;
            if (!opaque) {
                const V4 has = gt(a, tiny);
                r = vand(has, div(r, a)); g = vand(has, div(g, a)); b = vand(has, div(b, a));
            }
            fn(r, g, b);
            if (!opaque) { r = mul(r, a); g = mul(g, a); b = mul(b, a); }
            store4(po, vand(valid, r), vand(valid, g), vand(valid, b), vand(valid, a));
        }
        if (part) std::memcpy(o + (size_t)x * 4, tmpOut, (size_t)(n - x) * 4 * sizeof(float));
    }
}

}  // namespace v4
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
