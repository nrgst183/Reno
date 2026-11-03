// Fill Light as VEGAS draws it with its OpenCL kernels (Vfx1.ofx, GPU acceleration on; host at 0x1803c2800, colour table builder 0x1803be8c0, kernels prog23). The picture's brightness
// (Rec.601 luma) picks a colour from a 256 entry table; the table colour is blurred when Softness is set, scaled by Strength and added to the picture. The table holds a hue rotated grey
// (a colour matrix chain: Hue, Tint as a saturation) weighted by a hump over the brightness whose peak the Bias moves, written as bytes. Pictures are RGBA floats, the BOTTOM row first.
// Nothing here depends on Reno's Ctx, so a test can drive it with plain arrays.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "vfxgauss.h"

#if defined(__clang__)
#define RENO_FILLLIGHT_NO_FMA _Pragma("clang fp contract(off)")
#else
#define RENO_FILLLIGHT_NO_FMA
#endif

namespace reno {
namespace ofx {
namespace builtin {
namespace filllightk {

// The 5 x 5 colour matrices of the table builder (row vector times matrix, floats, the sums in the order the code adds them).
struct Mat {
    float m[25];
    void identity() { for (int i = 0; i < 25; ++i) m[i] = (i % 6 == 0) ? 1.0f : 0.0f; }
    // this = this * b (0x1803c0880)
    void mul(const Mat& b)
    {
        RENO_FILLLIGHT_NO_FMA
        Mat a = *this;
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5; ++j) {
                float s = a.m[i * 5 + 0] * b.m[0 * 5 + j];
                s = s + 0.0f;
                s = s + a.m[i * 5 + 1] * b.m[1 * 5 + j];
                s = s + a.m[i * 5 + 2] * b.m[2 * 5 + j];
                s = s + a.m[i * 5 + 3] * b.m[3 * 5 + j];
                s = s + a.m[i * 5 + 4] * b.m[4 * 5 + j];
                m[i * 5 + j] = s;
            }
    }
    void rotY(float a, float b) { Mat t; t.identity(); t.m[0] = b; t.m[2] = a; t.m[10] = -a; t.m[12] = b; mul(t); }   // 0x1803c0cf0
    void rotX(float a, float b) { Mat t; t.identity(); t.m[6] = b; t.m[7] = -a; t.m[11] = a; t.m[12] = b; mul(t); }   // 0x1803c0e00
    void shearZ(float p, float q) { Mat t; t.identity(); t.m[2] = p; t.m[7] = q; mul(t); }                           // 0x1803c0f10
};

// 0x1803c0550: a rotation of the hue by 'angle' (radians, a float) about the grey axis, the luminance plane kept level (the shear).
inline void hueRotate(Mat& dest, float angle)
{
    RENO_FILLLIGHT_NO_FMA
    const double twoPi = 6.283185307179586;
    const float theta = (float)(twoPi - std::fmod((double)angle, twoPi));
    Mat M;
    M.identity();
    M.rotX(-0.7071067690849304f, 0.7071067690849304f);
    M.rotY(0.5773502588272095f, 0.8164966106414795f);
    const float wr = 0.29899999499320984f, wg = 0.5870000123977661f, wb = 0.11400000005960464f;
    float lx = M.m[0] * wr;
    lx = lx + M.m[5] * wg;
    lx = lx + M.m[10] * wb;
    float ly = M.m[1] * wr;
    ly = ly + M.m[6] * wg;
    ly = ly + M.m[11] * wb;
    float lz = M.m[2] * wr;
    lz = lz + M.m[7] * wg;
    lz = lz + M.m[12] * wb;
    const float zsx = lx / lz, zsy = ly / lz;
    M.shearZ(zsx, zsy);
    const float c = std::cos(theta), s = std::sin(theta);
    Mat Z;
    Z.identity();
    Z.m[0] = c; Z.m[1] = -s; Z.m[5] = s; Z.m[6] = c;
    M.mul(Z);
    M.shearZ(-zsx, -zsy);
    M.rotY(-0.5773502588272095f, 0.8164966106414795f);
    M.rotX(0.7071067690849304f, 0.7071067690849304f);
    dest.mul(M);
}

// 0x1803c0340: the colour matrix of the table: red scaled to luma 1, hue rotated, then the saturation matrix with sat = Tint x 0.1 (as a float) mixing the luma back in.
inline void colourMatrix(Mat& dest, float hue, float sat)
{
    RENO_FILLLIGHT_NO_FMA
    dest.identity();
    Mat t0;
    t0.identity();
    t0.m[0] = 3.3444817066192627f;
    t0.m[6] = 0.0f;
    t0.m[12] = 0.0f;
    dest.mul(t0);
    hueRotate(dest, hue * 6.2831854820251465f);
    Mat t;
    t.identity();
    const float k = 1.0f - sat;
    const float a = k * 0.29899999499320984f, b = k * 0.5870000123977661f, c = k * 0.11400000005960464f;
    t.m[1] = a; t.m[2] = a; t.m[0] = a + sat;
    t.m[5] = b; t.m[7] = b; t.m[6] = b + sat;
    t.m[10] = c; t.m[11] = c; t.m[12] = c + sat;
    dest.mul(t);
}

inline uint8_t toByte(float v)
{
    // cvttss2si then the low byte (a value below 0 wraps, as the code stores it)
    if (!(v == v) || v >= 2147483648.0f || v <= -2147483648.0f) return 0;
    return (uint8_t)((int)v & 255);
}

// The 256 entry table (R, G, B, 0 bytes) from the settings the host derives: bias = Bias x 0.01, hue = Hue / 360, tint = Tint x 0.01 (doubles).
inline void buildTable(double bias, double hue, double tint, uint8_t lut[256][4])
{
    RENO_FILLLIGHT_NO_FMA
    const double mid = bias * 0.08 + 0.25;
    Mat M;
    const float satf = (float)((double)(float)tint * 0.1);
    colourMatrix(M, (float)hue, satf);
    double xs[128];
    float cols[128][4];
    for (int i = 0; i < 128; ++i) {
        const double x = (double)i / 127.0;
        xs[i] = x;
        double w = 0.0;
        if (mid > x) {
            const double t = x / mid, u = 1.0 - t;
            double a = t * 3.0;
            const double t2 = t * t, u2 = u * u;
            a = a * u2;
            double w0 = u2 * u;
            a = a * 0.0;
            w0 = w0 * 0.0;
            w0 = w0 + a;
            double b = t2;
            b = b * 3.0;
            double t3 = t2 * t;
            b = b * u;
            t3 = t3 * 0.3;
            b = b * 0.3;
            w0 = w0 + b;
            w = w0 + t3;
        } else if (!(0.85 < x)) {
            const double t = (x - mid) / (0.85 - mid), u = 1.0 - t;
            const double t2 = t * t;
            double a = t * 3.0;
            const double u2 = u * u;
            double w0 = u2;
            a = a * u2;
            double b = t2;
            w0 = w0 * u;
            b = b * 3.0;
            a = a * 0.3;
            w0 = w0 * 0.3;
            b = b * u;
            double t3 = t2 * t;
            w0 = w0 + a;
            b = b * 0.0;
            t3 = t3 * 0.0;
            w0 = w0 + b;
            w = w0 + t3;
        }
        const float wf = (float)w;
        for (int k = 0; k < 4; ++k) {
            float s = M.m[0 * 5 + k] * wf;
            s = s + M.m[1 * 5 + k] * wf;
            s = s + M.m[2 * 5 + k] * wf;
            cols[i][k] = s;
        }
    }
    for (int i = 0; i < 256; ++i) {
        const double x = (double)i / 255.0;
        float c[3] = {0, 0, 0};
        if (xs[0] >= x) { c[0] = cols[0][0]; c[1] = cols[0][1]; c[2] = cols[0][2]; }
        else if (x >= xs[127]) { c[0] = cols[127][0]; c[1] = cols[127][1]; c[2] = cols[127][2]; }
        else {
            for (int k = 0; k < 127; ++k) {
                if (x > xs[k] && xs[k + 1] > x) {
                    const double f = (x - xs[k]) / (xs[k + 1] - xs[k]);
                    for (int q = 0; q < 3; ++q) {
                        const float d = cols[k + 1][q] - cols[k][q];
                        c[q] = (float)((double)d * f + (double)cols[k][q]);
                    }
                    break;
                }
            }
        }
        for (int q = 0; q < 3; ++q) lut[i][q] = toByte(c[q] * 255.0f + 0.5f);
        lut[i][3] = 0;
    }
}

struct Settings {
    double hue = 0, strength = 0, bias = 0, tint = 0, softness = 0;   // the parameters as set (Hue 0..360, Strength / Bias / Tint / Softness in percent)
    float (*store8)(float) = nullptr;                                   // the 8-bit store of a kernel's float result (null: a float frame)
};

inline void cut(std::vector<float>& v, float (*store8)(float))
{
    if (store8)
        for (float& x : v) x = store8(x);
}

// src, out: W x H x 4 floats, bottom row first.
inline void render(const std::vector<float>& src, std::vector<float>& out, int W, int H, const Settings& s)
{
    RENO_FILLLIGHT_NO_FMA
    out = src;
    if (W <= 0 || H <= 0) return;
    const double bias = s.bias * 0.01, hue = s.hue / 360.0, tint = s.tint * 0.01, soft = s.softness * 0.002, strength = s.strength * 0.009999997913837433;
    if (!(strength > 0.0001)) return;                       // (the host copies the picture)
    uint8_t lut[256][4];
    buildTable(bias, hue, tint, lut);
    // kernel 588 and 589: the luma picture, then its colour from the table (a float picture)
    std::vector<float> X((size_t)W * H * 4);
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        const float* p = &src[i * 4];
        float l = p[0] * 0.299f;
        l = l + p[1] * 0.587f;
        l = l + p[2] * 0.114f;
        l = l + 0.0f * 0.0f;
        int idx = (int)(l * 255.0f + 0.5f);
        idx = std::min(std::max(idx, 0), 255);
        for (int k = 0; k < 4; ++k) X[i * 4 + k] = (float)lut[idx][k] / 255.0f;
    }
    if (soft > 0.0001) {
        // the host's sigmas (pixel aspect 1): the square root of the softness times the longer side
        float sz = std::max((float)W * 1.0f, (float)H * 1.0f);
        const float sc = (float)soft * sz;
        const float sigA = std::sqrt(1.0f * sc), sigB = std::sqrt(1.0f * sc);
        auto plain = [](float a, float b, float c) { return a * b + c; };
        const vfxgauss::Coefsf cy = vfxgauss::toFloats(vfxgauss::build((double)sigA));
        const vfxgauss::Coefsf cx = vfxgauss::toFloats(vfxgauss::build((double)sigB));
        std::vector<float> t1, t2;
        vfxgauss::passColumns(cy, X, W, H, t1, t2, plain);
        cut(t1, s.store8);
        cut(t2, s.store8);
        std::vector<float> sumT((size_t)W * H * 4);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int k = 0; k < 4; ++k) sumT[((size_t)x * H + y) * 4 + k] = t1[((size_t)y * W + x) * 4 + k] + t2[((size_t)y * W + x) * 4 + k];
        std::vector<float> ra, rb;
        vfxgauss::passColumns(cx, sumT, H, W, ra, rb, plain);
        std::vector<float> r1((size_t)W * H * 4), r2((size_t)W * H * 4);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int k = 0; k < 4; ++k) {
                    r1[((size_t)y * W + x) * 4 + k] = ra[((size_t)x * H + y) * 4 + k];
                    r2[((size_t)y * W + x) * 4 + k] = rb[((size_t)x * H + y) * 4 + k];
                }
        cut(r1, s.store8);
        cut(r2, s.store8);
        for (size_t i = 0; i < X.size(); ++i) X[i] = r1[i] + r2[i];
    }
    // kernel 5810: the picture plus the fill times Strength / 2, kept in 0..1, the alpha untouched
    const float k = (float)strength * 0.5f;
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        for (int c = 0; c < 3; ++c) {
            float v = X[i * 4 + c] * k + src[i * 4 + c];
            v = std::fmin(std::fmax(v, 0.0f), 1.0f);
            out[i * 4 + c] = s.store8 ? s.store8(v) : v;
        }
        out[i * 4 + 3] = s.store8 ? s.store8(src[i * 4 + 3]) : src[i * 4 + 3];
    }
}

}  // namespace filllightk
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
