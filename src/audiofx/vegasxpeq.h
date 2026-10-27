// VEGAS's ExpressFX Equalization (sfxpfx2_x64.dll, CXpEq, DirectX class 00000003-0f56-11d2-9887-00a0c969725b): its settings,
// presets and sound. The engine is the plug-in's own code translated line by line: the filter design routines (a low shelf
// for the bass, a band for the middle and a high shelf for the treble, with the plug-in's own correction tables), the
// routine that applies the settings, and the one that runs one sample through the three filters; the mix of the equalized
// and the original sound is done as the plug-in does it.
#pragma once
#include "vegaspack1.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace reno {
namespace vegpack1 {

// The settings structure (12 ints, as the plug-in saves it): 0 the mix, equalized sound against the original, in tenths of
// a percent (0 to 1000); 1 the middle gain, 2 the bass gain, 3 the treble gain, each in tenths of a dB (-250 to 250);
// 4 and 5 are saved but nothing reads them; 6 the width of the middle band, 7 its centre in Hz, 8 and 9 switch the bass and
// the treble filter on, 10 the bass corner in Hz, 11 the treble corner in Hz (the dialog does not change 6 to 11).
class XpEqEngine : public Engine {
public:
    void setup(const int32_t* p, uint32_t rate, bool keep) override
    {
        if (rate < 1000) rate = 1000;
        if (!ready_ || !keep || rate != rate_) {
            // a new connection or a jump in the stream: the plug-in starts with silent filters (it applies its settings
            // with the "discontinuity" flag, which clears the state of the middle band)
            rate_ = rate;
            std::memset(s1_, 0, sizeof s1_);
            std::memset(s2_, 0, sizeof s2_);
            apply(p, false);
            ready_ = true;
            return;
        }
        apply(p, true);
    }

    void process(float* left, float* right, int frames) override
    {
        if (!ready_) return;
        run(left, frames, 0);
        run(right, frames, 1);
    }

private:
    struct Mid {            // the plug-in's 0x38 bytes of the middle band
        int32_t on;
        double z1, z2, c1, c2, c3, gain;
    };
    struct Shelf {          // the plug-in's 0x180 bytes of the two shelves
        int32_t lowType;    // 0 off, 1 boost, 2 cut
        double a[8], b[8];  // the bass filter as two identical sections
        int32_t highType;
        double c[8], d[8];  // the treble filter as two identical sections
    };

    // ---- the plug-in's helper tables
    static const double* tabA() { static const double t[7] = {0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 101.0}; return t; }
    static const double* tabB()
    {
        static const double t[7] = {0.0, 0.0011337868480725624, 0.0022675736961451248, 0.022675736961451247, 0.11337868480725624, 0.22675736961451248, 0.5};
        return t;
    }
    static const double* tabT()
    {
        static const double t[8 * 7] = {
            4.5351473922902495e-05, 0.0016553287981859411, 0.003197278911564626, 0.034013605442176874, 0.22675736961451248, 0.45351473922902497, 1.0,
            3.401360544217687e-05, 0.0016553287981859411, 0.003197278911564626, 0.034013605442176874, 0.22675736961451248, 0.45351473922902497, 1.0,
            2.2675736961451248e-05, 0.001179138321995465, 0.002494331065759637, 0.026077097505668934, 0.1473922902494331, 0.36281179138321995, 0.8,
            2.2675736961451248e-05, 0.001179138321995465, 0.002494331065759637, 0.024263038548752835, 0.13038548752834467, 0.2947845804988662, 0.6518820861678004,
            2.2675736961451248e-05, 0.001179138321995465, 0.00235827664399093, 0.023582766439909298, 0.12471655328798185, 0.2494331065759637, 0.5500453514739229,
            2.2675736961451248e-05, 0.0011564625850340137, 0.0023129251700680273, 0.023356009070294784, 0.12018140589569161, 0.2494331065759637, 0.5500453514739229,
            2.2675736961451248e-05, 0.0011564625850340137, 0.0023129251700680273, 0.023356009070294784, 0.12018140589569161, 0.2494331065759637, 0.5500453514739229,
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        return t;
    }
    static const double* tabDb()
    {
        static const double t[16] = {0.0, -1.0, -2.0, -3.0, -4.0, -5.0, -5.0, -6.0, -7.0, -8.0, -9.0, -10.0, -11.0, -12.0, -12.0, -13.0};
        return t;
    }

    // the plug-in's flush of tiny numbers: anything whose exponent is below 2^-32 becomes zero
    static double flush(double v)
    {
        uint64_t b;
        std::memcpy(&b, &v, 8);
        if (((uint32_t)(b >> 32) & 0x7ff00000u) < 0x3df00000u) b = 0;
        std::memcpy(&v, &b, 8);
        return v;
    }

    // 0x1800436b0: the two poles of a filter from the corner (as a share of the sample rate) and a width
    static void poles(double* coef, double x, double y, int reset)
    {
        if (0.0003 > x) x = 0.0003;
        else if (x > 0.49) x = 0.49;
        double a = x * -3.141592653589793;
        double w = x * 6.283185307179586;
        a /= y;
        const double e1 = std::exp(a);
        double c = std::cos(w);
        w /= y;
        c *= 2.0;
        const double r = e1 * c;
        coef[2] = r;
        const double e2 = std::exp(-w);
        coef[3] = e2;
        coef[4] = (1.0 - r) + e2;
        if (reset) coef[0] = coef[1] = 0.0;
    }

    // 0x1800437a0: a gain from the corner, read off the table of decibels
    static double shelfGain(double x)
    {
        double x2 = x;
        if (0.0003 > x) x2 = 0.0003;
        else if (x > 0.49) x2 = 0.49;
        const double step = 0.022675736961451247;
        double a = step;
        int k = 1;
        bool found = false;
        for (;;) {
            if (a >= x2) { found = true; break; }
            a += step;
            if (a >= x2) { k += 1; found = true; break; }
            a += step;
            if (a >= x2) { k += 2; found = true; break; }
            a += step;
            if (a >= x2) { k += 3; found = true; break; }
            a += step;
            if (a >= x2) { k += 4; found = true; break; }
            k += 5;
            a += step;
            if (!(k < 16)) break;
        }
        if (!found) {
            --k;
            a += (0.0 - step);
        }
        const double lo = a - step;
        const double tk = tabDb()[k], tk1 = tabDb()[k - 1];
        double r;
        if (lo == a) r = tk1;
        else if (tk1 == tk) r = tk1;
        else if (x2 == lo) r = tk1;
        else if (x2 == a) r = tk;
        else {
            r = tk - tk1;
            const double d1 = x2 - lo;
            const double d2 = a - lo;
            r = r * d1;
            r = r / d2;
            r = r + tk1;
        }
        return std::pow(10.0, r / 20.0);
    }

    // 0x180043c80: the bass boost (a low shelf)
    static void lowShelfBoost(double* coef, const double* p, int reset)
    {
        poles(coef, p[0] / 1.5, p[2], reset);
        coef[6] = 1.0;
        coef[7] = std::pow(10.0, p[1] / 40.0) - 1.0;
    }

    // 0x180043be0: the bass cut
    static void lowShelfCut(double* coef, const double* p, double minGain, int reset)
    {
        double x = p[0];
        if (0.011337868480725623 > x) x = x / 2.2;
        else x = x / 3.3;
        poles(coef, x, p[2], reset);
        if (minGain >= p[1]) coef[6] = 0.0;
        else coef[6] = std::pow(10.0, p[1] / 40.0);
        coef[7] = 1.0 - coef[6];
    }

    // 0x180043e10: the treble boost (a high shelf)
    static void highShelfBoost(double* coef, const double* p, int reset)
    {
        poles(coef, p[0], p[2], reset);
        coef[5] = shelfGain(p[0]);
        coef[4] = 1.0;
        coef[6] = 1.0;
        coef[7] = std::pow(10.0, p[1] / 40.0) - 1.0;
    }

    // 0x180043d00: the treble cut
    static void highShelfCut(double* coef, const double* p, double minGain, int reset)
    {
        double x = p[0];
        if (0.0004 > x) {
            x = 0.0004 * 0.4;
        } else {
            bool mid = true;
            if (x > 0.49) x = 0.49;
            else if (0.125 > x) { x = x * 0.4; mid = false; }
            if (mid) {
                double t = x - 0.125;
                t = t * 1.6;
                t = t + 0.4;
                x = x * t;
            }
        }
        poles(coef, x, p[2], reset);
        coef[5] = shelfGain(x);
        coef[6] = 1.0;
        if (minGain < p[1]) coef[7] = std::pow(10.0, p[1] / 40.0) - 1.0;
        else coef[7] = -1.0;
    }

    // 0x1800439f0: how far the corner of the middle band is moved, from its width and the corner (two tables, interpolated)
    static double bandCorner(double x, double q)
    {
        const double* A = tabA();
        const double* B = tabB();
        const double* T = tabT();
        double f = 1.0;
        int i = 0;
        for (; i < 7; ++i)
            if (A[i] >= q) break;
        int j = 0;
        for (; j < 7; ++j)
            if (T[i * 7 + j] >= x) break;
        if ((unsigned)(i - 1) > 5u || (unsigned)(j - 1) > 5u) return f * x;
        double b0 = B[j - 1], b1 = B[j];
        double t0 = T[(i - 1) * 7 + (j - 1)], t1 = T[(i - 1) * 7 + j];
        double r0 = b0 / t0, r1 = b1 / t1;
        if (t0 == t1) {
        } else if (r0 == r1) {
        } else if (x == t0) {
        } else if (x == t1) {
            r0 = r1;
        } else {
            r1 -= r0;
            const double d = x - t0;
            t1 -= t0;
            r1 *= d;
            r1 /= t1;
            r1 += r0;
            r0 = r1;
        }
        b0 = B[j - 1];
        b1 = B[j];
        t0 = T[i * 7 + (j - 1)];
        t1 = T[i * 7 + j];
        double s0 = b0 / t0, s1 = b1 / t1;
        if (t0 == t1) {
        } else if (s0 == s1) {
        } else if (x == t0) {
        } else if (x == t1) {
            s0 = s1;
        } else {
            s1 -= s0;
            const double d = x - t0;
            t1 -= t0;
            s1 *= d;
            s1 /= t1;
            s1 += s0;
            s0 = s1;
        }
        // r0 is the factor from the row above, s0 the one from this row: they are mixed by the width
        const double a0 = A[i - 1], a1 = A[i];
        double res = r0;
        double hi = s0;
        if (a0 == a1) {
        } else if (res == hi) {
        } else if (q == a0) {
        } else if (q == a1) {
            res = hi;
        } else {
            hi -= res;
            const double d = q - a0;
            const double w = a1 - a0;
            hi *= d;
            hi /= w;
            hi += res;
            res = hi;
        }
        return res * x;
    }

    // 0x180043910: the gain factor of the middle band
    static void bandGain(double* coef, const double* p)
    {
        if (p[1] == 0.0) return;
        const double g = std::pow(10.0, p[1] / 20.0);
        const double g1 = g - 1.0;
        if (p[1] > 0.0) coef[5] = g1 / p[2];
        double base = (p[0] > 0.001814) ? 0.85 : 0.65;
        double t;
        if (1.0 > p[2]) {
            t = 1.0 - p[2];
            t = t * 0.9;
            t = t + base;
        } else {
            t = base / p[2];
        }
        t *= g1;
        coef[5] = t;
    }

    // 0x180043ea0: the middle band (the poles from the corner and width, and the gain)
    static void band(double* coef, const double* in, int reset)
    {
        double q[3] = {in[0], in[1], in[2]};
        double x = q[0];
        if (0.0003 > x) x = 0.0003;
        else if (x > 0.49) x = 0.49;
        const double corner = bandCorner(x, q[2]);
        q[0] = corner;
        bandGain(coef, q);
        double f;
        if (0.0003 > corner) f = 0.0003;
        else if (corner > 0.49) f = 0.49;
        else f = corner;
        double fp = f * 3.141592653589793;
        double f2 = f * 6.283185307179586;
        double e = std::exp(-(fp / q[2]));
        double c = std::cos(f2);
        f2 = f2 / q[2];
        c = c * 2.0;
        e = e * c;
        coef[2] = e;
        const double e2 = std::exp(-f2);
        coef[3] = e2;
        double s = (1.0 - e) + e2;
        s = std::sqrt(s);
        double cp = std::cos(fp);
        s = s * cp;
        coef[4] = s;
        if (reset) coef[0] = coef[1] = 0.0;
    }

    // 0x180016640: designs the three filters of one channel from the settings
    void design(const int32_t* st, Mid* m, Shelf* s)
    {
        const double rate = (double)rate_;
        s->lowType = 0;
        if (st[8] != 0 && st[2] != 0) {
            double arr[3];
            const int g = st[2];
            arr[1] = (double)g / 10.0;
            arr[0] = (double)st[10] / rate;
            if (g > 0) {
                arr[2] = 0.7;
                lowShelfBoost(s->a, arr, 1);
                s->lowType = 1;
            } else {
                arr[2] = 0.1;
                lowShelfCut(s->a, arr, -25.0, 1);
                s->lowType = 2;
            }
            std::memcpy(s->b, s->a, sizeof s->a);
        }
        s->highType = 0;
        if (st[9] != 0 && st[3] != 0) {
            double arr[3];
            const int g = st[3];
            arr[1] = (double)g / 10.0;
            arr[0] = (double)st[11] / rate;
            if (g > 0) {
                arr[2] = 0.7;
                highShelfBoost(s->c, arr, 1);
                s->highType = 1;
            } else {
                arr[2] = 0.3;
                highShelfCut(s->c, arr, -25.0, 1);
                s->highType = 2;
            }
            std::memcpy(s->d, s->c, sizeof s->c);
        }
        const double gain = (double)st[1] / 10.0;
        m->on = (gain == 0.0) ? 0 : 1;
        if (m->on) {
            double q[3];
            q[0] = (double)st[7] / rate;
            q[1] = gain;
            q[2] = 10.0 / (double)st[6];
            band(&m->z1, q, 0);
        }
    }

    // 0x180016940: applies the settings; on a jump in the stream the middle band's state is cleared first, and the second
    // channel starts as a copy of the first (state included, as in the plug-in)
    void apply(const int32_t* st, bool continuing)
    {
        if (!continuing) s1_[0].z1 = s1_[0].z2 = 0.0;
        design(st, &s1_[0], &s2_[0]);
        s1_[1] = s1_[0];
        s2_[1] = s2_[0];
        wet_ = (double)st[0] / 1000.0;
        dry_ = 1.0 - wet_;
    }

    // 0x180016ce0: one sample through the middle band, the bass shelf and the treble shelf
    static double sample(double v, Mid* m, Shelf* s)
    {
        if (m->on) {
            const double z1 = m->z1;
            const double t2 = v * m->c3;
            const double t0 = m->c2 * m->z2;
            m->z2 = z1;
            const double t1 = z1 * m->c1;
            double y = t2 - t0;
            y += t1;
            double d = y - z1;
            const double yf = flush(y);
            d *= m->gain;
            m->z1 = yf;
            v += d;
        }
        if (s->lowType == 1) {
            double* a = s->a;
            double* b = s->b;
            const double z1 = a[0];
            double xv = v * a[6];
            double x2 = v * a[4];
            const double t1 = a[3] * a[1];
            a[1] = z1;
            const double t0 = z1 * a[2];
            x2 += t0;
            x2 -= t1;
            const double y = flush(x2);
            a[0] = y;
            const double w1 = b[0];
            double t1b = b[3];
            double q4 = y * a[7];
            t1b = t1b * b[1];
            q4 += xv;
            const double t0b = w1 * b[2];
            b[1] = w1;
            double x2b = q4 * b[4];
            q4 = q4 * b[6];
            x2b += t0b;
            x2b -= t1b;
            double y2 = flush(x2b);
            b[0] = y2;
            y2 *= b[7];
            y2 += q4;
            v = y2;
        } else if (s->lowType != 0) {
            double* a = s->a;
            double* b = s->b;
            const double z1 = a[0];
            double t1 = a[3];
            double x2 = v * a[4];
            t1 *= a[1];
            a[1] = z1;
            double x4 = v;
            const double t0 = z1 * a[2];
            const double xv = v * a[6];
            x2 += t0;
            x2 -= t1;
            const double y = flush(x2);
            a[0] = y;
            const double w1 = b[0];
            double t1b = b[3];
            t1b *= b[1];
            x4 -= y;
            const double t0b = w1 * b[2];
            x4 *= a[7];
            b[1] = w1;
            x4 += xv;
            double x2b = x4;
            double xv2 = x4;
            x4 *= b[6];
            x2b *= b[4];
            x2b += t0b;
            x2b -= t1b;
            const double y2 = flush(x2b);
            xv2 -= y2;
            b[0] = y2;
            xv2 *= b[7];
            xv2 += x4;
            v = xv2;
        }
        if (s->highType != 0) {
            double* c = s->c;
            double* d = s->d;
            const double z1 = c[0];
            double z2 = c[1];
            double x0 = z1;
            double x2 = v;
            const double xv = v * c[6];
            x0 = x0 * c[2];
            x2 = x2 * c[5];
            const double t1 = z2 * c[3];
            x2 += x0;
            x0 = z1 * 2.0;
            x2 -= t1;
            const double y = flush(x2);
            z2 += y;
            z2 -= x0;
            z2 *= c[7];
            c[1] = z1;
            c[0] = y;
            const double w1 = d[0];
            double w2 = d[1];
            const double t1b = w2 * d[3];
            z2 += xv;
            double x0b = w1 * d[2];
            double x2b = z2 * d[5];
            z2 *= d[6];
            x2b += x0b;
            x0b = w1 * 2.0;
            x2b -= t1b;
            const double y2 = flush(x2b);
            w2 += y2;
            w2 -= x0b;
            w2 *= d[7];
            d[1] = w1;
            d[0] = y2;
            w2 += z2;
            v = w2;
        }
        return v;
    }

    void run(float* buf, int frames, int ch)
    {
        for (int k = 0; k < frames; ++k) {
            const double x = (double)buf[k];
            const double y = sample(x, &s1_[ch], &s2_[ch]);
            const double out = y * wet_ + x * dry_;
            buf[k] = (float)out;
        }
    }

    bool ready_ = false;
    uint32_t rate_ = 48000;
    double wet_ = 1.0, dry_ = 0.0;
    Mid s1_[2];
    Shelf s2_[2];
};

inline const Info& xpEqInfo()
{
    static const Info info = [] {
        Info i;
        i.uid = "reno.xpeq";
        i.name = "ExpressFX Equalization";
        i.clsid = "00000003-0f56-11d2-9887-00a0c969725b";
        // the effect's own GUID, written before its settings: ba97fc24-0f62-11d2-9887-00a0c969725b
        const uint8_t g[16] = {0x24, 0xfc, 0x97, 0xba, 0x62, 0x0f, 0xd2, 0x11, 0x98, 0x87, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x5b};
        std::copy(g, g + 16, i.persistGuid);
        i.nInts = 12;
        i.bypassSlot = -1;
        i.params = {
            {"Bass", -25, 25, 0, "dB", 2, 10, Num, "", 1, false},
            {"Mid", -25, 25, 0, "dB", 1, 10, Num, "", 1, false},
            {"Treble", -25, 25, 0, "dB", 3, 10, Num, "", 1, false},
            {"Mix", 0, 100, 100, "%", 0, 10, Num, "", 1, false},
        };
        i.presets = {
            {"(Default)", {1000, 0, 0, 0, -250, 0, 25, 400, 1, 1, 300, 500}},
            {"Default all parameters", {1000, 0, 0, 0, -250, 0, 25, 400, 1, 1, 300, 500}},
            {"Bass only", {1000, -250, 0, -250, -250, 0, 25, 400, 1, 1, 300, 500}},
            {"Bass boost", {1000, 0, 120, 0, -250, 0, 25, 400, 1, 1, 300, 500}},
            {"Contour (drop middle)", {1000, -250, 60, 60, -250, 0, 25, 400, 1, 1, 300, 500}},
            {"Treble only", {1000, -250, -250, 60, -250, 0, 25, 400, 1, 1, 300, 500}},
            {"Treble boost", {1000, 0, 0, 120, -250, 0, 25, 400, 1, 1, 300, 500}},
        };
        i.make = [] { return std::unique_ptr<Engine>(new XpEqEngine()); };
        return i;
    }();
    return info;
}

}  // namespace vegpack1
}  // namespace reno
