// Picture In Picture and Crop as VEGAS draws them with GPU acceleration on: the maths of the two OpenCL kernels of Vfx1.ofx (one picture sampled
// through a 3 x 3 matrix) and of the float matrix builders that feed them, re-implemented here step for step in floats. The pictures are RGBA floats,
// rows from the bottom (VEGAS's memory order), premultiplied; W x H pixels. Plain C++17 with no Reno types, so the test programs can include it alone.
#pragma once
#include "../core/rmath.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace vfxpc {

// A kernel's float3 dot product, left to right.
inline float dot3(float a, float b, float c, float x, float y) { return a * x + b * y + c; }

// What the builders take that Reno has no field for: the pixel aspect (width / height of one pixel, 1 for square pixels).
// The builder's sa / sb scale pairs (par < 1: sa = (par, 1), sb = (1 / par, 1); otherwise sa = (1, 1 / par), sb = (1, par)), all in floats.
struct AspectPairs { float sax, say, sbx, sby; };
inline AspectPairs aspectPairs(float par)
{
    AspectPairs a;
    const float inv = 1.0f / par;
    if (1.0f > par) { a.sax = par; a.say = 1.0f; a.sbx = inv; a.sby = 1.0f; }
    else { a.sax = 1.0f; a.say = inv; a.sbx = 1.0f; a.sby = par; }
    return a;
}

// The shared tail of both builders (0x18066af80 and 0x1806741f0): the turn a (float), the scale pairs, the shear K, the translation t and the origin o
// make two rows: m0 = (sax sbx c, -(say sbx s), ty m01 + tx m00 + ox), m1 = ((c K + s)(sby sax), (c - s K)(sby say), ty m11 + tx m10 + oy).
inline void rowsFrom(float a, const AspectPairs& p, float sbx, float sby, float K, float tx, float ty, float ox, float oy, float* m)
{
    const float c = reno::rm::cos(a), s = reno::rm::sin(a);
    m[0] = p.sax * sbx * c;
    m[1] = -(p.say * sbx * s);
    m[2] = ty * m[1] + tx * m[0] + ox;
    m[3] = (c * K + s) * (sby * p.sax);
    m[4] = (c - s * K) * (sby * p.say);
    m[5] = ty * m[4] + tx * m[3] + oy;
}

// ---------------------------------------------------------------------------------------------------------------------------------- Crop
// Kernel arguments of Crop: two matrix rows and the bounds (-hx, hx, hy, -hy) the transformed pixel must lie inside (inclusive).
struct CropSetup { float m[6]; float b[4]; float opacity; };

// W x H: the frame; lx, ly: Location (0..1, y up as the parameter); xs, ys: X Crop and Y Crop; angle: degrees; par: pixel aspect.
inline CropSetup cropSetup(int W, int H, double lx, double ly, double xs, double ys, double angle, double opacity, float par = 1.0f)
{
    CropSetup o;
    const float a = (float)std::fmod(angle * 0.01745329252, 6.28318530718);
    const AspectPairs p = aspectPairs(par);
    rowsFrom(a, p, p.sbx, p.sby, 0.0f, (float)-(W * lx), (float)-(H * ly), 0.0f, 0.0f, o.m);
    const float hx = (float)(xs * 0.5 * W), hy = (float)(ys * 0.5 * H);
    o.b[0] = -hx; o.b[1] = hx; o.b[2] = hy; o.b[3] = -hy;
    o.opacity = (float)opacity;
    return o;
}
// One pixel (x, y from the bottom): true when it is kept. (The picture is read at the pixel itself.)
inline bool cropInside(const CropSetup& s, int x, int y)
{
    const float fx = (float)x, fy = (float)y;
    const float vx = dot3(s.m[0], s.m[1], s.m[2], fx, fy), vy = dot3(s.m[3], s.m[4], s.m[5], fx, fy);
    return vx >= s.b[0] && vx <= s.b[1] && vy <= s.b[2] && vy >= s.b[3];
}

// ---------------------------------------------------------------------------------------------------------------------------------- Picture In Picture
// Kernel arguments of Picture In Picture: the opacity and the 3 x 3 matrix (rows) that takes a frame pixel to a position in the source.
struct PipSetup8 { float M[9]; float opacity; };

// Fixed Shape (mode 0), Fixed Parallelogram (1): the builder 0x18066b120. scale: Scale in X; scaleY: Scale in Y (mode 1; mode 0 uses scale for both); shear: Shear (mode 1).
inline PipSetup8 pipSetupAffine(int W, int H, int mode, double lx, double ly, double scale, double scaleY, double shear, double angle, double opacity, float par = 1.0f,
                                float shearScale = 1.0f)
{
    PipSetup8 o;
    AspectPairs p = aspectPairs(par);
    float sbx, sby;
    if (1.0 > (double)par) { sbx = (float)(1.0 / ((double)par * scale)); sby = (float)(1.0 / scale); }
    else { sbx = (float)(1.0 / scale); sby = (float)((double)par / scale); }
    float K = 0.0f;
    if (mode != 0) {
        if (1.0 > (double)par) sby = (float)(1.0 / scaleY); else sby = (float)((double)par / scaleY);
        K = (float)shear * shearScale;
    }
    const float a = (float)std::fmod(angle * 0.017453292519943295, 6.283185307179586);
    rowsFrom(a, p, sbx, sby, K, (float)-(W * lx), (float)-(H * ly), (float)W * 0.5f, (float)H * 0.5f, o.M);
    o.M[6] = 0.0f; o.M[7] = 0.0f; o.M[8] = 1.0f;
    o.opacity = (float)opacity;
    return o;
}

// Free Form (mode 2): the four corner points (0..1, y up) are scaled to frame pixels and mapped to the source rectangle by the 4-point solver (0x180495300):
// an 8 x 8 system in doubles for the homography h0..h7 (h8 = 1) taking each corner (x, y) to its source corner (X, Y):
//   x h0 + y h1 + h2 - X x h6 - X y h7 = X,   x h3 + y h4 + h5 - Y x h6 - Y y h7 = Y, with the products X x, X y, Y x, Y y formed in floats.
// Corner order TL, TR, BL, BR -> source (0, H), (W, H), (0, 0), (W, 0). Returns false when the system cannot be solved.
inline bool solve8(double A[8][8], double b[8], double x[8])
{
    for (int c = 0; c < 8; ++c) {
        int best = c;
        for (int r = c + 1; r < 8; ++r)
            if (std::fabs(A[r][c]) > std::fabs(A[best][c])) best = r;
        if (std::fabs(A[best][c]) < 1e-9) return false;
        if (best != c) {
            for (int k = 0; k < 8; ++k) std::swap(A[c][k], A[best][k]);
            std::swap(b[c], b[best]);
        }
        for (int r = c + 1; r < 8; ++r) {
            const double f = A[r][c] / A[c][c];
            for (int k = c; k < 8; ++k) A[r][k] -= f * A[c][k];
            b[r] -= f * b[c];
        }
    }
    for (int r = 7; r >= 0; --r) {
        double s = b[r];
        for (int k = r + 1; k < 8; ++k) s -= A[r][k] * x[k];
        x[r] = s / A[r][r];
    }
    return true;
}
inline bool pipSetupFreeForm(int W, int H, const double corners[4][2], double opacity, PipSetup8& o)
{
    // the corner points as the builder 0x18066ce80 makes them: (float)x * W, (float)y * H in floats
    float d[4][2];
    for (int i = 0; i < 4; ++i) { d[i][0] = (float)corners[i][0] * (float)W; d[i][1] = (float)corners[i][1] * (float)H; }
    const float s[4][2] = {{0.0f, (float)H}, {(float)W, (float)H}, {0.0f, 0.0f}, {(float)W, 0.0f}};
    double A[8][8], b[8], x[8];
    for (int i = 0; i < 4; ++i) {
        const float px = d[i][0], py = d[i][1], X = s[i][0], Y = s[i][1];
        const double r0[8] = {(double)px, (double)py, 1.0, 0.0, 0.0, 0.0, (double)-(X * px), (double)-(X * py)};
        const double r1[8] = {0.0, 0.0, 0.0, (double)px, (double)py, 1.0, (double)-(Y * px), (double)-(Y * py)};
        for (int k = 0; k < 8; ++k) { A[2 * i][k] = r0[k]; A[2 * i + 1][k] = r1[k]; }
        b[2 * i] = X; b[2 * i + 1] = Y;
    }
    if (!solve8(A, b, x)) return false;
    for (int k = 0; k < 8; ++k) o.M[k] = (float)x[k];
    o.M[8] = 1.0f;
    o.opacity = (float)opacity;
    return true;
}

// The card's bilinear read (CL_FILTER_LINEAR, pixel coordinates; texels outside the picture are transparent black: CL_ADDRESS_CLAMP) of img (W x H RGBA, rows from the bottom) at (px, py)
// with the fractions cut to 1/256 as the texture unit does, blended along x and then along y.
inline void linearRead(const std::vector<float>& img, int W, int H, float px, float py, bool edgeRepeat, float* o)
{
    const float flx = std::floor(px), fly = std::floor(py);
    float fx = px - flx, fy = py - fly;
    fx = std::floor(fx * 256.f + 0.5f) / 256.f;
    fy = std::floor(fy * 256.f + 0.5f) / 256.f;
    const int x0 = (int)std::max(-2.0, std::min((double)W + 1, (double)flx)), y0 = (int)std::max(-2.0, std::min((double)H + 1, (double)fly));
    auto tex = [&](int x, int y, int k) -> float {
        if (edgeRepeat) { x = std::max(0, std::min(W - 1, x)); y = std::max(0, std::min(H - 1, y)); }
        else if (x < 0 || y < 0 || x >= W || y >= H) return 0.f;
        return img[((size_t)y * W + x) * 4 + k];
    };
    for (int k = 0; k < 4; ++k) {
        const float a = tex(x0, y0, k), b = tex(x0 + 1, y0, k), c = tex(x0, y0 + 1, k), d = tex(x0 + 1, y0 + 1, k);
        const float t0 = a + (b - a) * fx, t1 = c + (d - c) * fx;
        o[k] = t0 + (t1 - t0) * fy;
    }
}

}  // namespace vfxpc
