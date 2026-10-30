// CoreGraphics.Native's Gaussian blur, worked out from its machine code (GaussianBlur, with the filter's number function and its row and column routines).
// Per line, four channels at a time, everything in single precision: first the gain and three feedback numbers from the deviation, then
//   forward   y[i] = ((x[i] * gain + c1 * y[i-1]) + c2 * y[i-2]) + c3 * y[i-3]   with y[-1] = y[-2] = y[-3] = x[0]
//   backward  z[i] = ((y[i] * gain + c1 * z[i+1]) + c2 * z[i+2]) + c3 * z[i+3]   with z[n] = z[n+1] = z[n+2] = y[n-1]
// and every result below 1e-5 is set to 0. Rows are filtered into a float picture, then its columns. Bytes come out as the nearest whole number (ties to even) held to 0..255.
#include "builtins_cgblur.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include "../core/base.h"

namespace reno {
namespace ofx {
namespace builtin {

CgBlurCoefs cgBlurCoefs(double sigmaIn)
{
    // the caller: below 0.5 is 0.5, else at most 54; as a float
    float s = (float)(0.5 > sigmaIn ? 0.5 : std::min(54.0, sigmaIn));
    float q;
    if (s >= 2.5f) {
        q = s * 0.97588f - 0.9633f;
    } else {
        const double t = 1.0 - (double)s * 0.26891;
        q = 3.97156f - (float)std::sqrt(t) * 4.14554f;
    }
    const float q2 = q * q;
    const float q3 = q * q2;
    const double dq = (double)q, dq2 = (double)q2, dq3 = (double)q3;
    const double a1 = dq * 2.44413;
    double b0 = a1 + 1.57825;
    const double t2 = dq2 * 1.4281;
    b0 += t2;
    const double t3 = dq3 * 0.422205;
    b0 += t3;
    const float f0 = (float)b0;
    const float b3 = (float)t3;
    double b1 = dq2 * 2.85619 + a1;
    b1 += dq3 * 1.26661;
    double b2 = dq3 * 1.2666 + t2;
    b2 = -b2;
    CgBlurCoefs c;
    c.c1 = (float)b1 / f0;
    c.c2 = (float)b2 / f0;
    c.c3 = b3 / f0;
    const float sum = c.c1 + c.c2 + c.c3;
    if (sum > 0.999999f) {
        const float k = 0.999999f / sum;
        c.c1 *= k;
        c.c2 *= k;
        c.c3 *= k;
    }
    c.gain = (float)(1.0 - (double)(c.c1 + c.c2 + c.c3));
    return c;
}

int cgBlurByte(float v)
{
    const double r = std::nearbyint((double)v + 1.5e-8);
    return r <= 0.0 ? 0 : (r >= 255.0 ? 255 : (int)r);
}

namespace {

// One line of n pixels: x[i * xs + k] in, the result in y (n pixels of NC floats, contiguous)
template <int NC>
void filterLine(const CgBlurCoefs& c, const float* x, ptrdiff_t xs, int n, float* y)
{
    if (n <= 0) return;
    const float tiny = 1e-5f;
    float h1[NC], h2[NC], h3[NC];
    for (int k = 0; k < NC; ++k) h1[k] = h2[k] = h3[k] = x[k];
    for (int i = 0; i < n; ++i) {
        const float* xi = x + (ptrdiff_t)i * xs;
        float* yi = y + (size_t)i * NC;
        for (int k = 0; k < NC; ++k) {
            float acc = xi[k] * c.gain;
            acc = acc + c.c1 * h1[k];
            acc = acc + c.c2 * h2[k];
            acc = acc + c.c3 * h3[k];
            if (tiny > acc) acc = 0.f;
            yi[k] = acc;
        }
        for (int k = 0; k < NC; ++k) { h3[k] = h2[k]; h2[k] = h1[k]; h1[k] = yi[k]; }
    }
    const float* last = y + (size_t)(n - 1) * NC;
    for (int k = 0; k < NC; ++k) h1[k] = h2[k] = h3[k] = last[k];
    for (int i = n - 1; i >= 0; --i) {
        float* yi = y + (size_t)i * NC;
        for (int k = 0; k < NC; ++k) {
            float acc = yi[k] * c.gain;
            acc = acc + c.c1 * h1[k];
            acc = acc + c.c2 * h2[k];
            acc = acc + c.c3 * h3[k];
            if (tiny > acc) acc = 0.f;
            yi[k] = acc;
        }
        for (int k = 0; k < NC; ++k) { h3[k] = h2[k]; h2[k] = h1[k]; h1[k] = yi[k]; }
    }
}

}  // namespace

namespace {
template <int NC>
void blurPicture(float* px, int w, int h, double sigmaX, double sigmaY)
{
    if (!px || w <= 0 || h <= 0) return;
    const CgBlurCoefs cx = cgBlurCoefs(sigmaX), cy = cgBlurCoefs(sigmaY);
    parallelRows(h, (size_t)w * NC * 8, [&](int ya, int yb) {
        std::vector<float> line((size_t)w * NC);
        for (int y = ya; y < yb; ++y) {
            float* row = px + (size_t)y * w * NC;
            filterLine<NC>(cx, row, NC, w, line.data());
            std::copy(line.begin(), line.end(), row);
        }
    });
    // columns, a band of them at a time
    parallelRows(w, (size_t)h * NC * 8, [&](int xa, int xb) {
        std::vector<float> line((size_t)h * NC);
        for (int x = xa; x < xb; ++x) {
            float* col = px + (size_t)x * NC;
            filterLine<NC>(cy, col, (ptrdiff_t)w * NC, h, line.data());
            for (int y = 0; y < h; ++y) std::copy(line.begin() + (size_t)y * NC, line.begin() + (size_t)y * NC + NC, col + (size_t)y * w * NC);
        }
    });
}
}  // namespace

void cgGaussianBlur(float* px, int w, int h, double sigmaX, double sigmaY) { blurPicture<4>(px, w, h, sigmaX, sigmaY); }

// The same for a picture of one channel (a plane of w x h floats): what the native does to every channel of a grey picture.
void cgGaussianBlurPlane(float* px, int w, int h, double sigmaX, double sigmaY) { blurPicture<1>(px, w, h, sigmaX, sigmaY); }


// ---- GaussianBlurOCL: the OpenCL path VEGAS takes when an OpenCL device is there (CoreGraphics.Native's GaussianBlurOCL and its coefficient builder). A recursive Gaussian of
// three poles (two for a deviation above 100): a causal pass y[i] = n0 x[i] + n1 x[i-1] + n2 x[i-2] - d1 y[i-1] - d2 y[i-2] - d3 y[i-3] and an anticausal pass with the tail numerators
// t[k] = n[k] - d[k] n0, both started from the edge value (state G x[0], G the pass's DC gain), summed. The number it gets is (6 sigma + 1.5) / sqrt(2 ln 255), not the deviation itself.
OclBlurCoefs oclBlurCoefs(double sigmaIn)
{
    OclBlurCoefs c;
    for (int k = 0; k < 5; ++k) c.n[k] = c.d[k] = c.t[k] = 0.0;
    const double s = std::max(sigmaIn, 0.001);
    double n[5] = {0, 0, 0, 0, 0}, d[5] = {0, 0, 0, 0, 0};
    if (s <= 100.0) {
        const double a = 1.475 / s;
        const double sa = std::sin(a), ca = std::cos(a);
        const double e1 = std::exp(-1.512 / s), e2 = std::exp(-1.556 / s), e3 = std::exp(-3.024 / s), e4 = std::exp(-3.068 / s);
        const double x8 = sa * 1.021;
        double x11 = e1 * x8 - (ca * 3.796) * e1;
        x11 = x11 + e2 * 0.8929 + e1 * (ca * 0.8929);
        const double x1 = e4 * (ca * 0.8929) + e4 * x8;
        const double x5 = e3 * 1.898 - x1;
        const double d1 = -e2 - (2.0 * ca) * e1;
        const double d2 = (2.0 * e2 * ca) * e1 + e3;
        const double d3 = -(e2 * e3);
        const double sumN = 1.0051 + x11 + x5;
        const double dsum = d1 + 1.0 + d2 + d3;
        const double k = 2.0 * sumN / dsum - 1.0051;
        n[0] = 1.0051 / k; n[1] = x11 / k; n[2] = x5 / k;
        d[1] = d1; d[2] = d2; d[3] = d3;
    } else {
        const double a = 0.8448 / s;
        const double cs = std::cos(a), sn = std::sin(a);
        const double e = std::exp(-1.26 / s);
        double x9 = cs * 0.9629 * e - sn * 1.942 * e;
        double x10 = (cs + cs) * e;
        x9 = -x9; x10 = -x10;
        const double e2 = std::exp(-2.52 / s);
        const double k = 2.0 * (x9 + 0.9629) / (x10 + 1.0 + e2) - 0.9629;
        n[0] = 0.9629 / k; n[1] = x9 / k;
        d[1] = x10; d[2] = e2;
    }
    double sn = 0, sd = 1.0, st = 0;
    for (int i = 0; i < 5; ++i) { c.n[i] = n[i]; c.d[i] = d[i]; c.t[i] = i == 0 ? 0.0 : n[i] - d[i] * n[0]; sn += n[i]; sd += d[i]; st += c.t[i]; }
    c.gc = (sd > 1e-17 || -sd > 1e-17) ? sn / sd : 1.0;
    c.ga = (sd > 1e-17 || -sd > 1e-17) ? st / sd : 1.0;
    // (the machine code divides by the sum of the d's plus one only when it is not nearly zero)
    return c;
}

namespace {
void oclLine(const OclBlurCoefs& c, float* v, ptrdiff_t stride, int n, int nc, std::vector<float>& tmp)
{
    if (n <= 0) return;
    const float n0 = (float)c.n[0], n1 = (float)c.n[1], n2 = (float)c.n[2], d1 = (float)c.d[1], d2 = (float)c.d[2], d3 = (float)c.d[3];
    const float t1 = (float)c.t[1], t2 = (float)c.t[2], t3 = (float)c.t[3];
    const float gc = (float)c.gc, ga = (float)c.ga;
    tmp.assign((size_t)n * nc, 0.f);
    for (int k = 0; k < nc; ++k) {
        float* y = tmp.data();
        float xm1 = v[k], xm2 = v[k];
        float y1 = gc * v[k], y2 = y1, y3 = y1;
        for (int i = 0; i < n; ++i) {
            const float x = v[(ptrdiff_t)i * stride + k];
            float acc = n0 * x;
            acc = acc + n1 * xm1;
            acc = acc + n2 * xm2;
            acc = acc - d1 * y1;
            acc = acc - d2 * y2;
            acc = acc - d3 * y3;
            y[(size_t)i * nc + k] = acc;
            xm2 = xm1; xm1 = x;
            y3 = y2; y2 = y1; y1 = acc;
        }
        const float xl = v[(ptrdiff_t)(n - 1) * stride + k];
        float xp1 = xl, xp2 = xl, xp3 = xl;
        float z1 = ga * xl, z2 = z1, z3 = z1;
        for (int i = n - 1; i >= 0; --i) {
            float acc = t1 * xp1;
            acc = acc + t2 * xp2;
            acc = acc + t3 * xp3;
            acc = acc - d1 * z1;
            acc = acc - d2 * z2;
            acc = acc - d3 * z3;
            y[(size_t)i * nc + k] += acc;
            xp3 = xp2; xp2 = xp1; xp1 = v[(ptrdiff_t)i * stride + k];
            z3 = z2; z2 = z1; z1 = acc;
        }
    }
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < nc; ++k) v[(ptrdiff_t)i * stride + k] = tmp[(size_t)i * nc + k];
}
}  // namespace

double oclBlurSigma(double p)
{
    const float s = (float)p;
    float x = s * 6.0f + 1.5f;
    x = x * x;
    const float den = (float)(2.0 * std::log((double)(1.0f / 255.0f)));
    const float q = -(x / den);
    return (double)(float)std::sqrt((double)q);
}

void cgOclBlur(float* px, int w, int h, double sigmaX, double sigmaY)
{
    if (!px || w <= 0 || h <= 0) return;
    const OclBlurCoefs cx = oclBlurCoefs(oclBlurSigma(sigmaX)), cy = oclBlurCoefs(oclBlurSigma(sigmaY));
    parallelRows(h, (size_t)w * 4 * 8, [&](int ya, int yb) {
        std::vector<float> tmp;
        for (int y = ya; y < yb; ++y) oclLine(cx, px + (size_t)y * w * 4, 4, w, 4, tmp);
    });
    parallelRows(w, (size_t)h * 4 * 8, [&](int xa, int xb) {
        std::vector<float> tmp;
        for (int x = xa; x < xb; ++x) oclLine(cy, px + (size_t)x * 4, (ptrdiff_t)w * 4, h, 4, tmp);
    });
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
