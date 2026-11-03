// Flicker Control's (de.magix:FlickerReducer, MagixCVFx.ofx) OpenCL kernels, re-implemented from the plug-in's kernel text and host code (what VEGAS draws with GPU acceleration on):
// the temporal blend over at most 6 frames, the Addition and the mask. Float maths in the kernels' own order; the 8-bit store is more::oclStore12 (done by the caller).
// Pictures are RGBA floats 0..1 (the 8-bit values / 255).
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {
namespace vfxfl {

// The tone weight of a pixel (the kernels' f3, called for another frame than the middle one): m is the largest of R, G, B. Type 0 highlights, 1 midtones, 2 shadows, else 1.
inline float toneOf(int type, float m)
{
    if (type == 2) return 1.0f - m * m;
    if (type == 1) return (float)(1.0 - 4.0 * ((double)m - 0.5) * ((double)m - 0.5));        // (the kernel's constants are doubles)
    if (type == 0) return 1.0f - (1.0f - m) * (1.0f - m);
    return 1.0f;
}

// The window function of the plug-in (0x18020ae50): 'tw' + 1 weights for the frames time - tw / 2 ... time + (tw - tw / 2); the frame itself is index 'before'. The first loop makes
// numbers like binomial coefficients (each factor a whole-number division, in float), the second blends them with the flat mean by the sensitivity (from one half up: toward the
// flat mean, below: toward the middle frame), then they are divided by their sum.
inline void windowWeights(int tw, float sens, int& before, int& after, std::vector<float>& w)
{
    before = tw / 2;
    after = tw - before;
    const int n = tw + 1;
    w.assign((size_t)n, 0.f);
    const float flat = 1.0f / ((float)tw + 1.0f);
    if (n == 2) {
        w[0] = 0.666f;
        w[1] = 0.333f;
    } else {
        for (int i = 0; i < n; ++i) {
            const int e = (2 * i <= tw) ? i : tw - i;
            float v = 1.0f;
            for (int k = 1; k <= e; ++k) v *= (float)((tw - e + k) / k);
            w[(size_t)i] = v;
        }
    }
    float sum = 0.f;
    for (int i = 0; i < n; ++i) {
        float a, b;
        const float x = w[(size_t)i];
        if (0.5f <= sens) {
            const float d = flat - (x + x);
            a = d;
            b = flat - d;
        } else {
            a = (i == before) ? x - 2.0f : x - 0.0f;
            b = x - a * 0.5f;
        }
        const float r = a * sens + b;
        w[(size_t)i] = r;
        sum += r;
    }
    if (sum != 0.f)
        for (int i = 0; i < n; ++i) w[(size_t)i] = w[(size_t)i] / sum;
}

struct Slot {
    const float* px = nullptr;      // the frame's pixel (RGBA)
    float w = 0.f;                  // its weight (0 for a slot that is not used, or whose motion position fell outside the frame)
};

// One pixel of the temporal blend kernel (6 slots; 'cen' is the slot of the frame itself, which takes no tone weight). Type 2 (Shadow) is the kernel's plain weighted sum (no tone,
// no division); the others divide the tone-weighted sum by the sum of the weights.
inline void blendPixel(const Slot* s, int type, int cen, float* o)
{
    if (type == 2) {
        for (int c = 0; c < 4; ++c) {
            float v = s[0].w * s[0].px[c];
            for (int k = 1; k < 6; ++k) v = v + s[k].w * s[k].px[c];
            o[c] = v;
        }
        return;
    }
    float wk[6];
    for (int k = 0; k < 6; ++k) {
        const float* p = s[k].px;
        const float m = std::max(p[0], std::max(p[1], p[2]));
        wk[k] = s[k].w * (k != cen ? toneOf(type, m) : 1.0f);
    }
    float sum = wk[0];
    for (int k = 1; k < 6; ++k) sum = sum + wk[k];
    for (int c = 0; c < 4; ++c) {
        float v = s[0].px[c] * wk[0];
        for (int k = 1; k < 6; ++k) v = v + s[k].px[c] * wk[k];
        o[c] = sum == 0.f ? v / 1.0f : v / sum;
    }
}

// Show mask (the kernels' f4 with the limits 0 and 0.2): 5 times the largest channel difference between the frame itself 'a' and the result 'r', as grey, alpha of 'a'.
inline void maskPixel(const float* a, const float* r, float* o)
{
    const float k = 1.0f / (0.2f - 0.0f);
    float d = 0.f;
    for (int c = 0; c < 3; ++c) d = std::max(d, a[c] > r[c] ? a[c] - r[c] : r[c] - a[c]);
    const float v = std::max(0.f, std::min(1.f, k * (d - 0.f)));
    o[0] = o[1] = o[2] = v;
    o[3] = a[3];
}

// One pixel of the Addition kernel: every colour channel gets tone * amount * alpha added (kept in 0..1).
inline void additionPixel(const float* p, int type, float amount, float* o)
{
    const float t = toneOf(type, std::max(p[0], std::max(p[1], p[2])));
    for (int c = 0; c < 3; ++c) o[c] = std::max(0.f, std::min(1.f, p[c] + t * amount * p[3]));
    o[3] = p[3];
}

// The kernel's position map: (x', y') of the pixel (x, y) for the 8 numbers h (the ninth is 1).
inline void mapPoint(const float* h, float x, float y, float& ox, float& oy)
{
    const float den = h[6] * x + h[7] * y + 1.0f;
    ox = (h[0] * x + h[1] * y + h[2]) / den;
    oy = (h[3] * x + h[4] * y + h[5]) / den;
}

}  // namespace vfxfl
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
