// Dissolve as VEGAS draws it with its OpenCL kernel (Vfx1.ofx, GPU acceleration on): the whole effect is one per-pixel function of the two pictures' floats
// (RGBA, 0..1, premultiplied) and the progress. The host (0x1805ca120) hands the kernel the type, the progress as a float and a four-number colour: the
// colour-bleed speeds, the colour-morph speeds or the fade-through colour, depending on the type (the same kernel for all). Nothing here depends on Reno's Ctx.
#pragma once
#include <algorithm>
#include <cmath>

#if defined(__clang__)
#define RENO_DISSOLVE_NO_FMA _Pragma("clang fp contract(off)")
#else
#define RENO_DISSOLVE_NO_FMA
#endif

namespace reno {
namespace ofx {
namespace builtin {
namespace dissolvek {

inline float clamp01(float v) { return std::fmin(std::fmax(v, 0.0f), 1.0f); }
// OpenCL mix(): x + (y - x) * a
inline float mixf(float x, float y, float a) { RENO_DISSOLVE_NO_FMA return x + (y - x) * a; }

// type: 0 additive, 1 subtractive dissolve, 2 subtractive crossfade, 3 colour bleed, 4 colour morph, 5 threshold dissolve, 6 threshold appear, 7 fade through grayscale,
// 8 fade through colour, 9 R then G then B. p is the progress (float), col the kernel's colour argument (speeds r g b a, or the fade colour r g b a).
inline void pixel(int type, float p, const float* col, const float* a, const float* b, float* o)
{
    RENO_DISSOLVE_NO_FMA
    switch (type) {
    case 0: {
        const float* s1;
        const float* s2;
        float t, add;
        if (p < 0.5f) { t = p; add = 0.0f; s1 = b; s2 = a; }
        else { t = 0.5f - p; add = 1.0f; s1 = a; s2 = b; }
        const float k = 2.0f * t + add;
        for (int i = 0; i < 4; ++i) o[i] = s1[i] * k + s2[i];
        break;
    }
    case 1: {
        float s1[4], s2[4];
        float t, add;
        if (p < 0.5f) { t = p; add = 0.0f; for (int i = 0; i < 4; ++i) { s1[i] = -b[i]; s2[i] = a[i]; } }
        else { t = 0.5f - p; add = 1.0f; for (int i = 0; i < 4; ++i) { s1[i] = -a[i]; s2[i] = b[i]; } }
        const float k = 2.0f * t + add;
        for (int i = 0; i < 4; ++i) o[i] = std::fabs(k * s1[i] + s2[i]);
        o[3] = mixf(a[3], b[3], p);
        break;
    }
    case 2:
        for (int i = 0; i < 4; ++i) o[i] = std::fabs(p * (a[i] + b[i]) + (-a[i]));
        o[3] = mixf(a[3], b[3], p);
        break;
    case 3: case 4: {
        float s[4], r[4];
        for (int i = 0; i < 4; ++i) { s[i] = col[i] + 1.0f; r[i] = 1.0f / s[i]; }
        const float m = std::fmax(std::fmax(r[0], r[1]), std::fmax(r[2], r[3]));
        const float v23 = p * m;
        float up[4];
        for (int i = 0; i < 4; ++i) up[i] = clamp01(s[i] * v23);
        if (type == 3) {
            const float v24 = m - v23;
            for (int i = 0; i < 4; ++i) {
                const float dn = clamp01(s[i] * v24);
                o[i] = std::fmax(a[i] - up[i], 0.0f) + std::fmax(b[i] - dn, 0.0f);
            }
            o[3] = clamp01(o[3]);
        } else {
            for (int i = 0; i < 4; ++i) {
                const float hi = a[i] + up[i], lo = a[i] - up[i];
                o[i] = (a[i] < b[i]) ? std::fmin(b[i], hi) : std::fmax(b[i], lo);
            }
        }
        break;
    }
    case 5: {
        const float m = (a[0] + a[1] + a[2]) * (1.0f / 3.0f);
        for (int i = 0; i < 4; ++i) o[i] = (m > p) ? a[i] : b[i];
        break;
    }
    case 6: {
        const float m = (b[0] + b[1] + b[2]) * (1.0f / 3.0f);
        for (int i = 0; i < 4; ++i) o[i] = (m >= (1.0f - p)) ? b[i] : a[i];
        break;
    }
    case 7: case 8: {
        float mid[4];
        if (type == 7) {
            const float g = ((a[0] + b[0]) + (a[1] + b[1]) + (a[2] + b[2])) * (1.0f / 6.0f);
            mid[0] = mid[1] = mid[2] = mid[3] = g;
        } else {
            for (int i = 0; i < 3; ++i) mid[i] = col[i] * col[3];
            mid[3] = col[3];
        }
        const float* s1;
        const float* s2;
        float t;
        if (p < 0.5f) { t = (type == 7) ? clamp01(p * 2.0f) : p * 2.0f; s1 = a; s2 = mid; }
        else { t = clamp01((p - 0.5f) * 2.0f); s1 = mid; s2 = b; }
        for (int i = 0; i < 4; ++i) o[i] = mixf(s1[i], s2[i], t);
        if (type == 7) o[3] = mixf(a[3], b[3], p);
        break;
    }
    default: {
        const float third = 1.0f / 3.0f;
        // (the host passes 0 for the kernel's swap flag, so the first channel is R)
        if (p < third) {
            const float t = p * 3.0f;
            o[3] = mixf(a[3], b[3], t);
            o[1] = a[1];
            o[0] = mixf(a[0], b[0], t);
            o[2] = a[2];
        } else if (p < 2.0f * third) {
            const float t = clamp01((p - third) * 3.0f);
            o[3] = mixf(a[3], b[3], p);
            o[1] = mixf(a[1], b[1], t);
            o[0] = b[0];
            o[2] = a[2];
        } else {
            const float t = clamp01((p - 2.0f * third) * 3.0f);
            o[3] = mixf(a[3], b[3], p);
            o[1] = b[1];
            o[0] = b[0];
            o[2] = mixf(a[2], b[2], t);
        }
        break;
    }
    }
}

} // namespace dissolvek
} // namespace builtin
} // namespace ofx
} // namespace reno
