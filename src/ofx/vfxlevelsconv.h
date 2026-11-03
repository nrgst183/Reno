// VEGAS's Levels and Convolution Kernel as their OpenCL kernels draw them (the float path VEGAS takes with GPU acceleration on), re-written from the decoded
// kernels and their host code (Vfx1): 32-bit floats in the kernels' order, no fused multiply-add, the host's constants in doubles where the host uses doubles.
// The kernels' pictures are premultiplied RGBA with the rows counted from the bottom; Convolution Kernel here takes Reno's top-first rows and flips the tap rows.
#pragma once
#include <algorithm>
#include <cmath>

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

namespace reno {
namespace ofx {
namespace vfxlc {

// ------------------------------------------------------------------ Levels
// channel: 0 All, 1 Red, 2 Green, 3 Blue, 4 Alpha (colour and alpha)
struct LevelsSetup {
    int channel = 0;
    bool linear = true;                 // |Gamma - 1| < 1e-6: the straight kernels
    bool curve0 = true;                 // Gamma > 1 (the kernels' v7 == 0)
    float range = 1.f, inStart = 0.f, outStart = 0.f, outEnd = 1.f, gamma = 1.f;   // (inEnd - inStart) cut to float, the rest cast
    float toe = 0.f, off = 0.f, slope = 0.f, v7 = 0.f, e = 1.f;
};

inline LevelsSetup levelsSetup(int channel, double inStart, double inEnd, double outStart, double outEnd, double gamma)
{
    LevelsSetup s;
    s.channel = (channel >= 0 && channel <= 4) ? channel : 0;     // (a choice out of range draws as All)
    s.range = (float)(inEnd - inStart);
    s.inStart = (float)inStart;
    s.outStart = (float)outStart;
    s.outEnd = (float)outEnd;
    s.gamma = (float)gamma;
    s.linear = std::fabs(gamma - 1.0) < 1e-6;
    if (s.linear) return s;
    // the curve builder: a power with a straight piece (slope) below the threshold k, joined at k by the offset
    const double iv = 1.0 / gamma;
    const double gg = 1.0 / iv;
    double k = std::pow(gg, -0.9418051973077521) * 0.03818376618407356;
    const double slope = std::pow(gg, 1.8836103946155043);
    double off = 0.0, v7 = 0.0;
    if (iv < 1.0) {
        const float t = (float)std::pow(k, iv);
        off = (std::pow(gg, 1.8836103946155043) * k - (double)t) / (double)(t - 1.0f);
        s.curve0 = true;
    } else if (iv > 1.0) {
        const float t = (float)std::pow(std::pow(gg, 1.8836103946155043) * k, gg);
        off = (k - (double)t) / (double)(t - 1.0f);
        v7 = 1.0 / (off + 1.0);
        s.curve0 = false;
    } else {
        off = 0.0;
        k = 100.0;
    }
    s.toe = (float)k;
    s.off = (float)off;
    s.v7 = (float)v7;
    s.slope = (float)slope;
    s.e = 1.0f / s.gamma;
    return s;
}

// One value through the kernel's function (a straight value, 0..1 floats, nothing clamped)
inline float levelsF(const LevelsSetup& s, float x)
{
    if (s.linear) {
        const float d = x - s.inStart;
        const float m = d * (s.outEnd - s.outStart);
        const float q = m / s.range;
        return q + s.outStart;
    }
    float t = (x - s.inStart) / s.range;
    if (s.curve0) {
        if (t >= s.toe) t = (1.0f + s.off) * std::pow(t, s.e) - s.off;
        else if (t > -s.toe) t = t * s.slope;
        else t = (-1.0f - s.off) * std::pow(-t, s.e) + s.off;
    } else {
        if (t >= s.toe) t = std::pow((t + s.off) * s.v7, s.e);
        else if (t > -s.toe) t = t * s.slope;
        else t = -std::pow((t - s.off) * (-s.v7), s.e);
    }
    const float r = t * (s.outEnd - s.outStart);
    return r + s.outStart;
}

// A premultiplied pixel (r, g, b, a) in, the kernel's result out
inline void levelsPixel(const LevelsSetup& s, const float* p, float* o)
{
    const float a = p[3];
    if (!(a > 0.f)) { o[0] = o[1] = o[2] = o[3] = 0.f; return; }
    if (s.channel == 0) {
        for (int k = 0; k < 3; ++k) o[k] = levelsF(s, p[k] / a) * a;
        o[3] = a;
    } else if (s.channel <= 3) {
        o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = a;
        const int k = s.channel - 1;
        o[k] = levelsF(s, p[k] / a) * a;
    } else {
        for (int k = 0; k < 3; ++k) o[k] = levelsF(s, p[k] / a) * a;
        o[3] = levelsF(s, a);
    }
}

// ------------------------------------------------------------------ Convolution Kernel
struct ConvSetup {
    float A[9];                         // weight of the tap at (dx, dy) = (-1,-1) (0,-1) (1,-1) (-1,0) (0,0) (1,0) (-1,1) (0,1) (1,1), y counted up
    float offset = 0.f;
    bool alphaPlain = false;            // the weights sum below 1: the alpha is the centre pixel's
};

// w: Row1Column1 .. Row3Column3 (row by row); rotation in degrees
inline ConvSetup convSetup(const double* wd, double rotation, double scaleIn, double offset, bool autoNormalize)
{
    ConvSetup s;
    float w[9];
    for (int i = 0; i < 9; ++i) w[i] = (float)wd[i];
    float rot = (float)rotation;
    if (rot < 0.f) rot += (float)(std::abs((int)(rot / 360.f)) * 360 + 360);
    if (rot == 0.f) {
        const float t[9] = {w[6], w[7], w[8], w[3], w[4], w[5], w[0], w[1], w[2]};
        for (int i = 0; i < 9; ++i) s.A[i] = t[i];
    } else {
        const float ring[8] = {w[6], w[7], w[8], w[5], w[2], w[1], w[0], w[3]};
        const int idx = (int)(rot / 45.f);
        const float fr = (rot - (float)(45 * idx)) / 45.f, iv = 1.f - fr;
        auto tap = [&](int k) { return fr * ring[(idx + k + 1) & 7] + iv * ring[(idx + k) & 7]; };
        s.A[0] = tap(0); s.A[1] = tap(1); s.A[2] = tap(2); s.A[3] = tap(-1); s.A[4] = w[4];
        s.A[5] = tap(3); s.A[6] = tap(-2); s.A[7] = tap(-3); s.A[8] = tap(-4);
    }
    float scale = (float)scaleIn;
    if (autoNormalize) {
        float sum = s.A[0];
        for (int i = 1; i < 9; ++i) sum = sum + s.A[i];
        if (std::fabs(sum) <= 1e-4f) scale = 1.f;
        else scale = std::min(100.f, std::max(-100.f, (float)(1.0 / (double)sum)));
    }
    for (int i = 0; i < 9; ++i) s.A[i] = s.A[i] * scale;
    float sumW = s.A[0];
    for (int i = 1; i < 9; ++i) sumW = sumW + s.A[i];
    s.alphaPlain = 1.f > sumW;
    s.offset = (float)offset;
    return s;
}

// Rows y0 .. y1 of the picture (W x H premultiplied RGBA floats, Reno's rows: the first row is the top) through the kernel; the taps read with the edges repeated.
inline void convRows(const ConvSetup& s, const float* src, int W, int H, float* dst, int y0, int y1)
{
    static const int dxs[9] = {-1, 0, 1, -1, 0, 1, -1, 0, 1};
    static const int dys[9] = {-1, -1, -1, 0, 0, 0, 1, 1, 1};
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < W; ++x) {
            float v[4] = {0.f, 0.f, 0.f, 0.f};
            for (int i = 0; i < 9; ++i) {
                const int xx = std::min(W - 1, std::max(0, x + dxs[i]));
                const int yy = std::min(H - 1, std::max(0, y - dys[i]));      // (OpenCL's y runs up)
                const float* q = src + ((size_t)yy * W + xx) * 4;
                for (int k = 0; k < 4; ++k) {
                    const float term = s.A[i] * q[k];
                    v[k] = i == 0 ? term : v[k] + term;
                }
            }
            const float* c = src + ((size_t)y * W + x) * 4;
            if (s.alphaPlain) v[3] = c[3];
            for (int k = 0; k < 3; ++k) v[k] = v[k] + s.offset;
            v[3] = std::fmin(std::fmax(v[3], 0.f), 1.f);
            float* o = dst + ((size_t)y * W + x) * 4;
            o[0] = v[0] * v[3]; o[1] = v[1] * v[3]; o[2] = v[2] * v[3]; o[3] = v[3];
        }
    }
}

}  // namespace vfxlc
}  // namespace ofx
}  // namespace reno
