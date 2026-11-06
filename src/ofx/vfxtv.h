// VEGAS TV Simulator, as the older (DirectX) effect draws it: a re-implementation of what its 8-bit render function does (read from the user's own VEGAS
// install, vfx1.dll, and checked bit for bit against that function and its parts), not a fit. No dependencies but vfxrand.h, so a test program can include it alone.
//
// The settings, as the render function takes them (P): 0 zoom (1 = none; the page's Detail zoom runs 1..4), 1 aperture grill, 2 interlacing, 3 line sync, 4 vertical
// sync, 5 scan phasing, 6 phosphorescence, 7 static (all 0..1 sliders; line and vertical sync 1 = steady).
//
// What it does, row by row of the output (the rows are counted from the top of the picture buffer):
//  - the row read is (row + roll) mod height, roll = (1 / max(0.001, vertical sync) - 1) x zoom x height x seconds (whole rows, towards zero, the time being the event's
//    time stamp in 100 ns ticks);
//  - with line sync below 1 the row is first moved sideways by a fractal noise (6 octaves of a 32 x 64 table of random numbers, read between 4 x 4 values with a cubic
//    spline, the time and the row being the two axes) times (1 - line sync) / aspect times the width: the row is resampled with 14-bit fixed point positions, taken
//    from the row above or below where it runs off either end, and when the move between two rows is more than 1.4 pixels two taps are averaged and the row faded;
//  - the scanline pattern (brightness through a 10-row period, interlacing deep, with a moving band of up to 2.5 times when scan phasing is on), the aperture grill
//    (a 5 x 5 pixel mask of blue, green and red stripes 192 bright, with a dark row in 5, and static noise added to the mask) are mixed in by max(grill, static) x 0.75;
//  - with phosphorescence the result goes through a 256-entry gamma table per colour (exponents 1 - 0.6 p, 1 - 0.5 p, 1 - 0.15 p plus a little noise, p the setting).
// The noise tables come from the shift-register generator (vfxrand.h) seeded with the time stamp; the fractal noise's table is made once from the C library's rand(),
// where VEGAS has left it after the first picture it drew: Reno uses the table that follows from time stamp 3333334 (its first Bad Sync picture in the capture).
#pragma once
#include "../core/rmath.h"
#include <cmath>
#include <cstdint>
#include <climits>
#include <cstring>
#include <vector>
#include <algorithm>
#include "vfxrand.h"

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

namespace reno {
namespace ofx {
namespace vftv {

// cvttsd2si: whole number towards zero, the "indefinite" integer for what does not fit
inline int cvt32(double v) { return (v > -2147483649.0 && v < 2147483648.0) ? (int)v : INT_MIN; }
inline int64_t cvt64(double v) { return (v > -9223372036854775808.0 && v < 9223372036854775808.0) ? (int64_t)v : INT64_MIN; }
// the machine's wrapping integer arithmetic
inline int add32(int a, int b) { return (int)((unsigned)a + (unsigned)b); }
inline int sub32(int a, int b) { return (int)((unsigned)a - (unsigned)b); }
inline int mul32(int a, int b) { return (int)((unsigned)a * (unsigned)b); }
// (x + (x < 0 ? 16383 : 0)) >> 14: x / 16384 towards zero
inline int div14(int x) { return (int)(((int64_t)x + ((x >> 31) & 0x3fff)) >> 14); }
// the remainder as the C library has it (the sign of the dividend); a zero divisor gives 0
inline int rem32(int a, int b) { return (b == 0 || (a == INT_MIN && b == -1)) ? 0 : a % b; }

// ---- the cubic spline between four values (0x1801ada30): t between the second and the third
inline double cubic(double t, double a, double b, double c, double d)
{
    const double x5 = a * 0.5, x4 = d * 0.5;
    double x3 = b * 1.5;
    double x12 = a * 0.0;
    const double c15 = c * 1.5;
    x3 = x3 - x5;
    const double x9 = d * 0.0;
    x12 = x12 + b;
    x3 = x3 - c15;
    const double b25 = b * 2.5;
    double x2 = a;
    double x1 = c + c;
    x3 = x3 + x4;
    x2 = x2 - b25;
    const double c05 = c * 0.5;
    const double x11 = c * 0.0;
    x3 = x3 * t;
    x2 = x2 + x1;
    x1 = b * 0.0;
    x12 = x12 + x11;
    x2 = x2 - x4;
    x12 = x12 + x9;
    x1 = x1 - x5;
    x3 = x3 + x2;
    x1 = x1 + c05;
    x3 = x3 * t;
    x1 = x1 + x9;
    x3 = x3 + x1;
    x3 = x3 * t;
    x3 = x3 + x12;
    return x3;
}

// ---- the fractal noise (0x1801adb60): T is the table of 64 rows of 32 numbers; the result is the sideways move of a row in pixels
inline double fractalNoise(const double* T, int W, int H, double amp, double fx, double fy, int octaves, int y, int64_t stamp)
{
    double acc = 0.0;
    if (octaves != 0) acc = fractalNoise(T, W, H, amp / 2.71, fx * 2.231, fy * 2.142, octaves - 1, y, stamp);
    double u = ((double)y / (double)H) * fx;
    u = u + u;
    u = std::fmod(u, 32.0);
    double v = ((double)stamp / 10000000.0) * fy;
    v = v + v;
    v = std::fmod(v, 64.0);
    int ei = cvt32(v) - 1;
    int eb = cvt32(u);
    const double fu = std::fmod(u, 1.0);
    const double fv = std::fmod(v, 1.0);
    eb = eb - 1;
    if (eb < 0) eb += 32; else eb = rem32(eb, 32);
    if (ei < 0) ei += 64; else ei = rem32(ei, 64);
    const int c3 = rem32(eb + 3, 32), c2 = rem32(eb + 2, 32), c1 = rem32(eb + 1, 32), c0 = rem32(eb, 32);
    double r[4];
    for (int k = 0; k < 4; ++k) {
        const int row = rem32(ei, 64) * 32;
        r[k] = cubic(fu, T[row + c0], T[row + c1], T[row + c2], T[row + c3]);
        ++ei;
    }
    const double f = cubic(fv, r[0], r[1], r[2], r[3]);
    const double a = amp * 0.15;
    double res = f * a;
    res = res * (double)W;
    return res + acc;
}

// The table of random numbers (2048: 64 rows of 32) from the C library's rand() as the Microsoft one runs, from the state it is in after the shift-register generator
// was seeded: ((rand() mod 4096) - 2048) / 2048.
inline void makeNoiseTable(uint32_t randState, double* T)
{
    uint32_t st = randState;
    for (int i = 0; i < 2048; ++i) {
        st = st * 214013u + 2531011u;
        const int r = (int)((st >> 16) & 0x7fffu);
        T[i] = (double)((r & 0xfff) - 0x800) * 0.00048828125;
    }
}

// The table VEGAS left in the capture's session (the first time stamp that needed it): from the generator seeded with 3333334.
inline const double* defaultNoiseTable()
{
    static const std::vector<double> t = []() {
        std::vector<double> v(2048);
        builtin::VegasShiftRandom rng(3333334u);
        makeNoiseTable(rng.randState(), v.data());
        return v;
    }();
    return t.data();
}

// ---- one pixel read at a position in 14-bit fixed point along the row (0x1801b21b0): a position before the row's start is read from the row above (counted from its end),
// one past the end from the row below (wrapped); a tap between this pixel and the next, with a 14-bit fraction.
inline void samplePixel(uint8_t* out, int x, const uint8_t* above, const uint8_t* cur, const uint8_t* below, int W14)
{
    const uint8_t* p1;
    int xn = x, frac;
    if (x < 0) {
        xn = add32(x, W14);
        if (xn < 0) xn = 0;
        p1 = above + (int64_t)div14(xn) * 4;
    } else if (x >= W14) {
        xn = rem32(x, W14);
        p1 = below + (int64_t)div14(xn) * 4;
    } else {
        p1 = cur + (int64_t)div14(xn) * 4;
    }
    frac = rem32(xn, 16384);
    int x2 = add32(xn, 0x4000);
    const uint8_t* p2;
    if (x2 < 0) {
        x2 = add32(x2, W14);
        if (x2 < 0) x2 = 0;
        p2 = above + (int64_t)div14(x2) * 4;
    } else if (x2 >= W14) {
        x2 = rem32(x2, W14);
        p2 = below + (int64_t)div14(x2) * 4;
    } else {
        p2 = cur + (int64_t)div14(x2) * 4;
    }
    const int w1 = 0x4000 - frac;
    for (int c = 0; c < 4; ++c) out[c] = (uint8_t)div14(add32(mul32(p1[c], w1), mul32(p2[c], frac)));
}

// ---- a row moved sideways (0x1801ae460): out gets W pixels of the row 'sr' of the picture at 'base', moved by the noise 'curJ' (the row before had 'prevJ')
inline void shiftRow8(uint8_t* out, int W, int H, const uint8_t* base, int pitch, int sr, double prevJ, double curJ)
{
    const double delta = curJ - prevJ;
    const int rowAbove = (sr - 1 >= 0) ? sr - 1 : 0;
    const int rowBelow = std::min(H - 1, sr + 1);
    const uint8_t* above = base + (int64_t)mul32(rowAbove, pitch);
    const uint8_t* cur = base + (int64_t)mul32(sr, pitch);
    const uint8_t* below = base + (int64_t)mul32(rowBelow, pitch);
    const int W14 = (int)((unsigned)W << 14);
    double x0 = (double)W + delta;
    x0 = x0 - 1.0;
    double x1 = (double)(W - 1);
    x1 = x1 / x0;
    x1 = x1 * 16384.0;
    const int step = cvt32(x1);
    const double stepd = (double)step;
    const double ad = std::fabs(delta);
    double x1b = delta * 0.5;
    x1b = x1b + prevJ;
    x1b = x1b * stepd;
    int pb = cvt32(x1b);
    double x0b = stepd * curJ;
    int pa = cvt32(x0b);
    if (ad > 1.4) {
        double e = ad - 1.4;
        e = e * 16384.0;
        const int ecx = sub32(0x18000, cvt32(e));
        const int edx = (int)(((int64_t)0x2aaaaaab * (int64_t)ecx) >> 32);
        int r8 = add32(add32((int)((unsigned)edx >> 31), 0x1555), edx);
        int mixw = r8 < 0 ? 0 : (r8 > 0x4000 ? 0x4000 : r8);
        for (int i = 0; i < W; ++i) {
            uint8_t s1[4], s2[4];
            // the two taps, one at each of the two positions
            samplePixel(s1, pa, above, cur, below, W14);
            samplePixel(s2, pb, above, cur, below, W14);
            pa = add32(pa, step);
            pb = add32(pb, step);
            for (int c = 0; c < 3; ++c) {
                int v = (s2[c] + s1[c]) >> 1;
                v = mul32(v, mixw);
                out[4 * i + c] = (uint8_t)div14(v);
            }
            out[4 * i + 3] = (uint8_t)((s2[3] + s1[3]) >> 1);
        }
    } else {
        int x = pa;
        for (int i = 0; i < W; ++i) {
            samplePixel(out + 4 * i, x, above, cur, below, W14);
            x = add32(x, step);
        }
    }
}

// One static noise value: the generator's low 16 bits as a signed number times 512 / 65536 times the amount, taken to the nearest whole number the way the machine does it
// (added to 2^52 + 2^31 and the low word read back).
inline int staticValue(int rv, double amount)
{
    double v = (double)rv * 512.0;
    v = v * 1.52587890625e-05;
    v = v + 0.0;
    v = v * amount;
    v = v + 4503601774854144.0;
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    return (int)((uint32_t)bits + 0x80000000u);
}

// The five-pixel mask rows (VEGAS keeps them as bytes in its data; the values are the small constants): row 0..3 = blue, green, red stripes of 192 at pixels 1..3, row 4 = dark.
inline void maskRow(int row, uint8_t px[5][4])
{
    std::memset(px, 0, 5 * 4);
    if (row < 0 || row > 3) return;
    px[1][0] = 192; px[1][3] = 255;
    px[2][1] = 192; px[2][3] = 255;
    px[3][2] = 192; px[3][3] = 255;
}

// The brightness through a 10-row period and the band's profile
inline const double* scanTable() { static const double t[10] = {0.8, 1.2, 1.2, 1.2, 1.2, 0.8, 0.5, 0.2, 0.2, 0.5}; return t; }
inline const double* bandTable() { static const double t[5] = {1.0, 1.7, 2.5, 1.7, 1.0}; return t; }

// ---- the 8-bit render (0x1801b05c0): BGRA bytes, premultiplied, rows top first. P: the eight settings as above. stamp: the event's time stamp (100 ns ticks since the start of the event).
// aspect: the pixel aspect ratio (1). T: the fractal noise table (defaultNoiseTable()). Returns 0.
inline int render8(uint8_t* dst, int dstPitch, int W, int H, const uint8_t* src, int srcPitch, const double* P, int64_t stamp, double aspect, const double* T)
{
    const double seconds = (double)stamp / 10000000.0;
    const double xH = (double)H;
    // the roll
    double m = (0.001 > P[4]) ? 0.001 : P[4];
    double t0 = 1.0 / m;
    t0 = t0 - 1.0;
    t0 = t0 * P[0];
    t0 = t0 * (xH * seconds);
    const int64_t rollOff = H != 0 ? cvt64(t0) % (int64_t)H : 0;
    const bool phos = P[6] > 0.0;
    builtin::VegasShiftRandom rng((uint32_t)stamp);
    uint8_t lut[256][4];
    if (phos) {
        const int r8 = rem32((int)rng.next(), 1024);
        const double p6 = P[6];
        const double x0 = p6 * 0.6, x1 = p6 * 0.5;
        double n = (double)r8;
        n = n * 0.0009765625;
        n = n * 0.05;
        n = n * p6;
        const double x3 = p6 * 0.15000000000000002;
        double g0 = 1.0 - x0, g1 = 1.0 - x1, g2 = 1.0 - x3;
        g0 = g0 + n; g1 = g1 + n; g2 = g2 + n;
        g0 = (g0 < 1.0) ? g0 : 1.0;
        g1 = (g1 < 1.0) ? g1 : 1.0;
        g2 = (g2 < 1.0) ? g2 : 1.0;
        for (int i = 0; i < 256; ++i) {
            const double base = (double)i / 255.0;
            lut[i][0] = (uint8_t)cvt32(rm::pow(base, g0) * 255.0);
            lut[i][1] = (uint8_t)cvt32(rm::pow(base, g1) * 255.0);
            lut[i][2] = (uint8_t)cvt32(rm::pow(base, g2) * 255.0);
            lut[i][3] = 0;
        }
    }
    double mm = (P[1] > P[7]) ? P[1] : P[7];
    const int mix14 = cvt32(mm * 12288.0);
    const double xmm7 = xH * 0.011127083333333334;
    double lineFreq = 10.0 / (xmm7 + xmm7);
    const double zdiv = xH / P[0];
    lineFreq = lineFreq / P[0];
    double phase = xH - zdiv;
    phase = phase * 0.5;
    double bandH = 0.0, yTop = 0.0, yBot = 0.0;
    if (P[5] > 0.0001) {
        const double s3 = P[5] * 3.0;
        bandH = (xH * 0.35) * P[0];
        const double sec2 = seconds / (1.0 / s3);
        const double fr = std::fmod(sec2, 1.0);
        double hh = xH * 0.5;
        double y1 = fr * xH;
        double xm8 = xH * P[0];
        y1 = y1 * P[0];
        xm8 = xm8 * 0.5;
        hh = hh - xm8;
        y1 = y1 + hh;
        y1 = y1 - bandH * 0.5;
        yTop = y1;
        yBot = y1 + bandH;
    }
    int yPos = cvt32(phase * 16384.0);
    const double xW = (double)W;
    double q2 = xW * 0.00667625;
    const double Wz = xW / P[0];
    q2 = q2 / aspect;
    double xs = xW - Wz;
    const double rcp = 16384.0 / P[0];
    xs = xs * aspect;
    xs = xs * 0.5;
    xs = xs * 16384.0;
    const int xStart = cvt32(xs);
    double r5 = rcp * 5.0;
    double r4 = rcp * 4.0;
    r4 = r4 / q2;
    r5 = r5 / xmm7;
    const int xStep = cvt32(r4);
    const int yStep = cvt32(r5);
    if (H <= 0) return 0;
    const bool jitter = 1.0 > P[3];
    std::vector<uint8_t> jbuf(jitter ? (size_t)std::max(W, 0) * 4 : 0);
    double prevJ = 0.0;
    int twoY = mul32(yPos, 2);
    int yPosRow = yPos;
    const bool staticOn = P[7] > 0.0;
    int T1[129];
    uint8_t* dRow = dst;
    for (int row = 0; row < H; ++row) {
        const int sr = rem32(add32((int)rollOff, row), H);
        const uint8_t* srcRow = src + (int64_t)mul32(srcPitch, sr);
        if (jitter) {
            const double amp = (1.0 - P[3]) / aspect;
            const double cur = fractalNoise(T, W, H, amp, 4.567 / P[0], 6.213, 5, sr + 1, stamp);
            shiftRow8(jbuf.data(), W, H, src, srcPitch, sr, prevJ, cur);
            prevJ = cur;
            srcRow = jbuf.data();
        }
        // the scanline brightness
        double fm = std::fmod(phase, 10.0);
        const int fi = cvt32(fm);
        const double ff = fm - (double)fi;
        const unsigned ci = (unsigned)(fi + 1) % 10u;
        double sv = (1.0 - ff) * scanTable()[(unsigned)fi % 10u];
        sv = sv + ff * scanTable()[ci];
        const double qi = 1.0 - P[2];
        sv = sv * P[2];
        sv = sv + qi;
        sv = sv * 16384.0;
        int scanMul = cvt32(sv);
        if (bandH != 0.0) {
            const double rd = (double)row;
            if (!(rd < yTop) && !(yBot < rd)) {
                double t = rd - yTop;
                t = t / bandH;
                t = t * 4.0;
                const int bi = cvt32(t);
                unsigned bj = (unsigned)(bi + 1);
                if (bj > 4u) bj = 4u;
                const double bf = t - (double)bi;
                double w = (1.0 - bf) * bandTable()[(unsigned)bi % 5u];
                w = w + bf * bandTable()[bj];
                w = w * (double)scanMul;
                scanMul = cvt32(w);
            }
        }
        const int yFrac = rem32(yPosRow, 16384);
        const int yInt = div14(yPosRow);
        const int rem5 = rem32(yInt, 5);
        int xPos = xStart;
        if (staticOn) {
            const int hi = (int)(((int64_t)0x66666667 * (int64_t)twoY) >> 32);
            int es = hi >> 15;
            es = add32(es, (int)((unsigned)es >> 31));
            int ed = hi >> 1;
            ed = add32(ed, (int)((unsigned)ed >> 31));
            ed = rem32(ed, 16384);
            const int seedBase = add32((int)(uint32_t)stamp, 0x4fde5b);
            {
                builtin::VegasShiftRandom r1((uint32_t)mul32(seedBase, es));
                for (int k = 0; k < 128; ++k) T1[k] = staticValue(rem32((int)r1.next(), 65536), P[7]);
            }
            {
                builtin::VegasShiftRandom r2((uint32_t)mul32(add32(es, 1), seedBase));
                const int w0 = 0x4000 - ed;
                for (int k = 0; k < 128; ++k) {
                    const int v2 = staticValue(rem32((int)r2.next(), 65536), P[7]);
                    const int a = mul32(w0, T1[k]);
                    const int b = mul32(v2, ed);
                    T1[k] = div14(add32(b, a));
                }
            }
            T1[128] = T1[0];
        }
        // the 5 x 4 mask pixels of this row: between mask row (rem5 - 1) and rem5 with the row fraction
        uint8_t pat[5][4];
        {
            uint8_t cur5[5][4], prev5[5][4];
            maskRow(rem5, cur5);
            maskRow(rem5 - 1, prev5);
            for (int j = 0; j < 5; ++j) {
                for (int c = 0; c < 3; ++c) pat[j][c] = (uint8_t)div14(add32(mul32(prev5[j][c], 0x4000 - yFrac), mul32(cur5[j][c], yFrac)));
                pat[j][3] = 0xff;
            }
        }
        const int keep = 0x4000 - mix14;
        const uint8_t* s = srcRow;
        uint8_t* d = dRow;
        for (int x = 0; x < W; ++x, s += 4, d += 4) {
            const int xf = rem32(xPos, 16384);
            const int xi = div14(xPos);
            const int i0 = rem32(xi, 4), i1 = i0 + 1;
            const int wl = 0x4000 - xf;
            int pc[3];
            for (int c = 0; c < 3; ++c) pc[c] = (int)(uint8_t)div14(add32(mul32(pat[i0 < 0 ? 0 : i0][c], wl), mul32(pat[i1 < 0 ? 0 : i1][c], xf))) ;
            int n = 0;
            if (staticOn) {
                int xj = rem32(xi, 128);
                if (xj < 0) xj = 0;
                n = div14(add32(mul32(T1[xj + 1], xf), mul32(T1[xj], wl)));
            }
            const int alpha = s[3];
            for (int c = 0; c < 3; ++c) {
                const int pv = pc[c] + n;
                const int sc = s[c];
                int a2 = mul32(pv, sc);
                a2 = add32(a2, a2);
                const int t1 = (a2 + (a2 < 0 ? 255 : 0)) >> 8;
                int q4 = pv + (pv < 0 ? 3 : 0);
                q4 = q4 >> 2;
                int e = add32(q4, t1);
                e = mul32(e, mix14);
                e = add32(e, mul32(keep, sc));
                e = div14(e);
                e = mul32(e, scanMul);
                e = div14(e);
                e = e < 0 ? 0 : (e > alpha ? alpha : e);
                d[c] = (uint8_t)e;
            }
            d[3] = s[3];
            if (phos) {
                for (int c = 0; c < 3; ++c) {
                    const int l = lut[d[c]][c];
                    d[c] = (uint8_t)(l >= alpha ? alpha : l);
                }
            }
            xPos = add32(xPos, xStep);
        }
        phase = phase + lineFreq;
        yPosRow = add32(yPosRow, yStep);
        twoY = add32(twoY, mul32(yStep, 2));
        dRow += dstPitch;
    }
    return 0;
}


// ---- the same render split for a card: everything that is done once per row (the roll, the sideways move's numbers, the scanline brightness, the mask pixels and the static
// noise's 129 values) is worked out here exactly as render8() does it, and the per-pixel part (all whole-number maths on the levels) is left to the card's kernels
// (tools/gpukernels_fx2.py, vkfx2.cpp). Row k of 'rows' holds kRowInts numbers:
//   0 the source row, 1 the scanline brightness (14-bit), 2 the move's step, 3 and 4 the two start positions, 5 the weight of the two-tap mix (-1: one tap),
//   6 and 7 the rows above and below the source row, 8..22 the mask pixels (5 x blue, green, red), 23..151 the static noise (129 values).
const int kRowInts = 152;
struct Rows8 {
    int mix14 = 0, xStart = 0, xStep = 0;
    bool phos = false, staticOn = false, jitter = false;
    std::vector<int32_t> rows;
    uint8_t lut[256][4];
};
inline void prepare8(int W, int H, const double* P, int64_t stamp, double aspect, const double* T, Rows8& o)
{
    std::memset(o.lut, 0, sizeof(o.lut));
    o.rows.assign((size_t)std::max(H, 0) * kRowInts, 0);
    const double seconds = (double)stamp / 10000000.0;
    const double xH = (double)H;
    double m = (0.001 > P[4]) ? 0.001 : P[4];
    double t0 = 1.0 / m;
    t0 = t0 - 1.0;
    t0 = t0 * P[0];
    t0 = t0 * (xH * seconds);
    const int64_t rollOff = H != 0 ? cvt64(t0) % (int64_t)H : 0;
    const bool phos = P[6] > 0.0;
    builtin::VegasShiftRandom rng((uint32_t)stamp);
    if (phos) {
        const int r8 = rem32((int)rng.next(), 1024);
        const double p6 = P[6];
        const double x0 = p6 * 0.6, x1 = p6 * 0.5;
        double n = (double)r8;
        n = n * 0.0009765625;
        n = n * 0.05;
        n = n * p6;
        const double x3 = p6 * 0.15000000000000002;
        double g0 = 1.0 - x0, g1 = 1.0 - x1, g2 = 1.0 - x3;
        g0 = g0 + n; g1 = g1 + n; g2 = g2 + n;
        g0 = (g0 < 1.0) ? g0 : 1.0;
        g1 = (g1 < 1.0) ? g1 : 1.0;
        g2 = (g2 < 1.0) ? g2 : 1.0;
        for (int i = 0; i < 256; ++i) {
            const double base = (double)i / 255.0;
            o.lut[i][0] = (uint8_t)cvt32(rm::pow(base, g0) * 255.0);
            o.lut[i][1] = (uint8_t)cvt32(rm::pow(base, g1) * 255.0);
            o.lut[i][2] = (uint8_t)cvt32(rm::pow(base, g2) * 255.0);
            o.lut[i][3] = 0;
        }
    }
    double mm = (P[1] > P[7]) ? P[1] : P[7];
    const int mix14 = cvt32(mm * 12288.0);
    const double xmm7 = xH * 0.011127083333333334;
    double lineFreq = 10.0 / (xmm7 + xmm7);
    const double zdiv = xH / P[0];
    lineFreq = lineFreq / P[0];
    double phase = xH - zdiv;
    phase = phase * 0.5;
    double bandH = 0.0, yTop = 0.0, yBot = 0.0;
    if (P[5] > 0.0001) {
        const double s3 = P[5] * 3.0;
        bandH = (xH * 0.35) * P[0];
        const double sec2 = seconds / (1.0 / s3);
        const double fr = std::fmod(sec2, 1.0);
        double hh = xH * 0.5;
        double y1 = fr * xH;
        double xm8 = xH * P[0];
        y1 = y1 * P[0];
        xm8 = xm8 * 0.5;
        hh = hh - xm8;
        y1 = y1 + hh;
        y1 = y1 - bandH * 0.5;
        yTop = y1;
        yBot = y1 + bandH;
    }
    int yPos = cvt32(phase * 16384.0);
    const double xW = (double)W;
    double q2 = xW * 0.00667625;
    const double Wz = xW / P[0];
    q2 = q2 / aspect;
    double xs = xW - Wz;
    const double rcp = 16384.0 / P[0];
    xs = xs * aspect;
    xs = xs * 0.5;
    xs = xs * 16384.0;
    const int xStart = cvt32(xs);
    double r5 = rcp * 5.0;
    double r4 = rcp * 4.0;
    r4 = r4 / q2;
    r5 = r5 / xmm7;
    const int xStep = cvt32(r4);
    const int yStep = cvt32(r5);
    o.mix14 = mix14;
    o.xStart = xStart;
    o.xStep = xStep;
    o.phos = phos;
    o.jitter = 1.0 > P[3];
    o.staticOn = P[7] > 0.0;
    if (H <= 0) return;
    double prevJ = 0.0;
    int twoY = mul32(yPos, 2);
    int yPosRow = yPos;
    for (int row = 0; row < H; ++row) {
        int32_t* R = &o.rows[(size_t)row * kRowInts];
        const int sr = rem32(add32((int)rollOff, row), H);
        R[0] = sr;
        R[5] = -1;
        if (o.jitter) {
            const double amp = (1.0 - P[3]) / aspect;
            const double curJ = fractalNoise(T, W, H, amp, 4.567 / P[0], 6.213, 5, sr + 1, stamp);
            // shiftRow8()'s numbers
            const double delta = curJ - prevJ;
            R[6] = (sr - 1 >= 0) ? sr - 1 : 0;
            R[7] = std::min(H - 1, sr + 1);
            double x0 = (double)W + delta;
            x0 = x0 - 1.0;
            double x1 = (double)(W - 1);
            x1 = x1 / x0;
            x1 = x1 * 16384.0;
            const int step = cvt32(x1);
            const double stepd = (double)step;
            const double ad = std::fabs(delta);
            double x1b = delta * 0.5;
            x1b = x1b + prevJ;
            x1b = x1b * stepd;
            R[2] = step;
            R[4] = cvt32(x1b);
            R[3] = cvt32(stepd * curJ);
            if (ad > 1.4) {
                double e = ad - 1.4;
                e = e * 16384.0;
                const int ecx = sub32(0x18000, cvt32(e));
                const int edx = (int)(((int64_t)0x2aaaaaab * (int64_t)ecx) >> 32);
                int r8 = add32(add32((int)((unsigned)edx >> 31), 0x1555), edx);
                R[5] = r8 < 0 ? 0 : (r8 > 0x4000 ? 0x4000 : r8);
            }
            prevJ = curJ;
        }
        double fm = std::fmod(phase, 10.0);
        const int fi = cvt32(fm);
        const double ff = fm - (double)fi;
        const unsigned ci = (unsigned)(fi + 1) % 10u;
        double sv = (1.0 - ff) * scanTable()[(unsigned)fi % 10u];
        sv = sv + ff * scanTable()[ci];
        const double qi = 1.0 - P[2];
        sv = sv * P[2];
        sv = sv + qi;
        sv = sv * 16384.0;
        int scanMul = cvt32(sv);
        if (bandH != 0.0) {
            const double rd = (double)row;
            if (!(rd < yTop) && !(yBot < rd)) {
                double t = rd - yTop;
                t = t / bandH;
                t = t * 4.0;
                const int bi = cvt32(t);
                unsigned bj = (unsigned)(bi + 1);
                if (bj > 4u) bj = 4u;
                const double bf = t - (double)bi;
                double w = (1.0 - bf) * bandTable()[(unsigned)bi % 5u];
                w = w + bf * bandTable()[bj];
                w = w * (double)scanMul;
                scanMul = cvt32(w);
            }
        }
        R[1] = scanMul;
        const int yFrac = rem32(yPosRow, 16384);
        const int yInt = div14(yPosRow);
        const int rem5 = rem32(yInt, 5);
        if (o.staticOn) {
            int T1[129];
            const int hi = (int)(((int64_t)0x66666667 * (int64_t)twoY) >> 32);
            int es = hi >> 15;
            es = add32(es, (int)((unsigned)es >> 31));
            int ed = hi >> 1;
            ed = add32(ed, (int)((unsigned)ed >> 31));
            ed = rem32(ed, 16384);
            const int seedBase = add32((int)(uint32_t)stamp, 0x4fde5b);
            {
                builtin::VegasShiftRandom r1((uint32_t)mul32(seedBase, es));
                for (int k = 0; k < 128; ++k) T1[k] = staticValue(rem32((int)r1.next(), 65536), P[7]);
            }
            {
                builtin::VegasShiftRandom r2((uint32_t)mul32(add32(es, 1), seedBase));
                const int w0 = 0x4000 - ed;
                for (int k = 0; k < 128; ++k) {
                    const int v2 = staticValue(rem32((int)r2.next(), 65536), P[7]);
                    const int a = mul32(w0, T1[k]);
                    const int b = mul32(v2, ed);
                    T1[k] = div14(add32(b, a));
                }
            }
            T1[128] = T1[0];
            for (int k = 0; k < 129; ++k) R[23 + k] = T1[k];
        }
        uint8_t cur5[5][4], prev5[5][4];
        maskRow(rem5, cur5);
        maskRow(rem5 - 1, prev5);
        for (int j = 0; j < 5; ++j)
            for (int c = 0; c < 3; ++c) R[8 + j * 3 + c] = (uint8_t)div14(add32(mul32(prev5[j][c], 0x4000 - yFrac), mul32(cur5[j][c], yFrac)));
        phase = phase + lineFreq;
        yPosRow = add32(yPosRow, yStep);
        twoY = add32(twoY, mul32(yStep, 2));
    }
}

}
}
}
