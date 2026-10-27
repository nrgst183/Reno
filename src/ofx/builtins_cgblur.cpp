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

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
