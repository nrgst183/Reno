// VEGAS's ExpressFX Flange/Wah-Wah (sfxpfx1_x64.dll, CXpFlange, DirectX class 00000004-0f56-11d2-9887-00a0c969725b): its
// settings, presets and sound. The engine is the filter's own code (its "apply settings" routine, the flanger's
// per-frame routine, the wah-wah's table builder and per-sample routine) translated line by line.
//
// Flange: a ring of 6500 frames; a sine moves the read point around 1 ms (Depth) at Rate Hz; the interpolated tap
// (with half of itself fed back into the ring) is mixed with the input. Wah-Wah: a resonant two-pole filter whose
// coefficients come from tables of 2000 entries at most, swept up and down by a triangle at Rate Hz.
#pragma once
#include "vegaspack1.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {
namespace vegpack1 {

// The settings structure (8 ints): 0 effect (0 Flange, 2 Wah-Wah, 1 the plug-in's other wah variant), 1 and 2 two
// levels in tenths of a dB that the plug-in keeps but does not use (-800), 3 Rate in mHz (1..20000), 4 Depth in percent
// (1..100), 5 a frequency in Hz (100..5000, kept, 500), 6 the wah-wah's resonance (1..100, kept, 75), 7 the mix of the
// effect against the original in tenths of a percent.
class XpFlangeEngine : public Engine {
public:
    void setup(const int32_t* p, uint32_t rate, bool keep) override
    {
        std::memcpy(s_, p, sizeof s_);
        rate_ = (uint32_t)rate;
        // the plug-in's "apply settings" (inline in its block routine)
        eff_ = s_[0];
        wet_ = (double)s_[7] / 1000.0;
        dry_ = 1.0 - wet_;
        if (!keep) reset();
        if (eff_ == 1 || eff_ == 2) {
            wahTables();
        } else {
            double x = wet_;
            g1_ = x * 0.95;
            g0_ = x * 0.9;
            double d = (double)s_[4] / 100.0;
            d *= samplesPerMs_;
            depth_ = (double)(int32_t)d;
            wet_ = x * 0.9;
            double per = 1000.0 / (double)s_[3];
            per *= (double)rate_;
            period_ = (double)(int32_t)per;
            incr_ = 6.28318530718 / period_;
        }
    }
    void process(float* left, float* right, int frames) override
    {
        if (eff_ == 0) {
            for (int k = 0; k < frames; ++k) {
                double d[2] = {(double)left[k], (double)right[k]};
                flangeFrame(d);
                left[k] = (float)d[0];
                right[k] = (float)d[1];
            }
        } else if (eff_ == 1 || eff_ == 2) {
            const double p0 = pos_, st0 = step_;
            float* ch[2] = {left, right};
            for (int c = 0; c < 2; ++c) {
                pos_ = p0;
                step_ = st0;
                for (int k = 0; k < frames; ++k) ch[c][k] = (float)wahSample((double)ch[c][k], c);
            }
        }
    }

private:
    static constexpr int kRing = 6500;
    int32_t s_[8] = {0, -800, 0, 1000, 50, 500, 75, 500};
    uint32_t rate_ = 48000;
    int32_t eff_ = 0;
    double wet_ = 0, dry_ = 1, g0_ = 0, g1_ = 0;
    // flanger
    int32_t phase_ = 0, writeIdx_ = 0;
    double c8_ = 0, samplesPerMs_ = 0, period_ = 1, incr_ = 0, depth_ = 0;
    double state_[2] = {0, 0};
    std::vector<double> ring_ = std::vector<double>(2 * kRing, 0.0);
    // wah-wah
    int32_t len_ = 0;
    double pos_ = 0, step_ = 0;
    double t1_[2000], t2_[2000];
    double z0_[2] = {0, 0}, z1_[2] = {0, 0};

    // the plug-in's reset routine (0x180005f40): buffers cleared, positions to the start
    void reset()
    {
        std::fill(ring_.begin(), ring_.end(), 0.0);
        writeIdx_ = 0;
        state_[0] = state_[1] = 0.0;
        phase_ = 0;
        double v = (double)rate_;
        v *= 10.0;
        v /= 10000.0;
        samplesPerMs_ = (double)(int32_t)v;
        c8_ = samplesPerMs_;
        std::memset(t1_, 0, sizeof t1_);
        std::memset(t2_, 0, sizeof t2_);
        z0_[0] = z0_[1] = z1_[0] = z1_[1] = 0.0;
        pos_ = 0.0;
    }

    // the plug-in's flanger routine for one frame (0x1800068f0), two channels
    void flangeFrame(double* d)
    {
        const int nch = 2;
        double c = c8_;
        double dd = std::sin((double)phase_ * incr_) * depth_;
        dd += c;
        if (dd >= 6500.0) dd += 0.0 - 6500.0;
        else if (0.0 > dd) dd += 6500.0;
        const int ti = (int)dd;
        int edx = ti * nch;
        const double frac = dd - (double)ti;
        const int total = nch * kRing;
        if (edx < 0) edx = 0;
        else edx = std::min(edx, total - 1);
        int r9 = edx + nch;
        if (r9 >= total) r9 -= total;
        c8_ = c - 1.0;
        if (0.0 > c8_) c8_ = 6499.0;
        int ph = phase_ + 1;
        phase_ = ph;
        if ((double)ph >= period_) phase_ = 0;
        long rsi = edx, r10 = r9;
        const double tiny = 2e-11;
        const double g[2] = {g0_, g1_};
        for (int ch = 0; ch < nch; ++ch) {
            const double a = ring_[(size_t)rsi];
            double v = (ring_[(size_t)r10] - a) * frac + a;
            v *= 1.0;
            const double fb = 0.0 * state_[ch];
            v += fb;
            state_[ch] = v;
            if (tiny > std::fabs(v)) state_[ch] = 0.0;
            const double x = d[ch];
            double out = v * wet_;
            const double dryPart = x * dry_;
            out *= g[ch];
            out += dryPart;
            d[ch] = out;
            double w = v * 0.5;
            w += x;
            if (tiny > std::fabs(w)) w = 0.0;
            int widx = writeIdx_ * nch + ch;
            if (widx >= total) widx -= total;
            ring_[(size_t)widx] = w;
            ++edx;
            ++rsi;
            if (edx >= total) {
                edx -= total;
                rsi -= total;
            }
            ++r9;
            ++r10;
            if (r9 >= total) {
                r9 -= total;
                r10 -= total;
            }
        }
        if (--writeIdx_ < 0) writeIdx_ = kRing - 1;
    }

    // the wah-wah's table builder (0x180006110)
    void wahTables()
    {
        const uint32_t n = ((uint32_t)rate_ * 1000u) / ((uint32_t)(s_[3] * 2));
        int32_t edi = (int32_t)n;
        if ((int32_t)n > 2000) edi = 2000;
        len_ = edi;
        step_ = (double)edi / (double)(int32_t)n;
        const int32_t s6 = s_[6];
        double x9 = (double)(s6 - 50);
        x9 = x9 * x9;
        x9 = x9 * 0.008;
        x9 = x9 + 1.0;
        if (s6 < 50) x9 = 1.0 / x9;
        const int32_t s4 = s_[4];
        double x1 = (double)(s4 - 50);
        x1 = x1 * x1;
        x1 = x1 * 0.002;
        x1 = x1 + 1.0;
        if ((double)s4 > 50.0) x1 = 1.0 / x1;
        const double msPer = 1000.0 / (double)rate_;
        double x8, x7;
        if (x1 == 0.0) {
            x8 = msPer;
            x7 = 0.0;
        } else {
            const double pw = std::pow(2.0, 1.0 / x1);
            x8 = msPer / pw;
            x7 = pw * msPer;
            if (x8 > x7) x7 = x8;
            else if (x7 > 0.5) x7 = 0.5;
            x7 -= x8;
            x7 /= (double)edi;
        }
        for (int i = 0; i < edi; ++i) {
            double a = x8 * -3.14159265356;
            a /= x9;
            const double e = std::exp(a);
            double cs = std::cos(x8 * 6.28318530718);
            cs *= 2.0;
            t1_[i] = e * cs;
            double b = x8 * -6.28318530718;
            b /= x9;
            t2_[i] = std::exp(b);
            x8 += x7;
        }
    }

    // the wah-wah's per-sample routine (0x180006c60)
    double wahSample(double x, int c)
    {
        const double pos = pos_;
        int i0 = (int)pos;
        if (i0 < 0) i0 = 0;
        else i0 = std::min(i0, len_ - 1);
        int i1 = i0 + 1;
        if (i1 < 0) i1 = 0;
        else i1 = std::min(i1, len_ - 1);
        double a, b;
        if (pos == (double)i0) {
            a = t1_[i0];
            b = t2_[i0];
        } else {
            const double fr = pos - (double)i0;
            a = t1_[i1] - t1_[i0];
            a *= fr;
            a += t1_[i0];
            b = t2_[i1] - t2_[i0];
            b *= fr;
            b += t2_[i0];
        }
        // the sweep: a triangle between the ends of the table
        const double stp = step_;
        const double np = stp + pos;
        pos_ = np;
        const double top = (double)(len_ - 1);
        bool flip = false;
        if (np >= top) {
            pos_ = top;
            flip = true;
        } else if (!(0.0 < np)) {
            pos_ = 0.0;
            flip = true;
        }
        if (flip) step_ = stp * -1.0;
        const double zb = z1_[c], za = z0_[c];
        z1_[c] = za;
        const double m = za * a;
        double y = m + x;
        y -= zb * b;
        double bp = y * b;
        bp -= m;
        bp += zb;
        if (2e-11 > std::fabs(y)) y = 0.0;
        z0_[c] = y;
        if (eff_ == 1) {
            bp += x;
            bp *= 0.5;
        } else {
            bp = x - bp;
        }
        const double dryPart = x * dry_;
        bp *= wet_;
        return dryPart + bp;
    }
};

inline const Info& xpFlangeInfo()
{
    static const Info info = [] {
        Info i;
        i.uid = "reno.xpflange";
        i.name = "ExpressFX Flange/Wah-Wah";
        i.clsid = "00000004-0f56-11d2-9887-00a0c969725b";
        const uint8_t g[16] = {0x28, 0xfc, 0x97, 0xba, 0x62, 0x0f, 0xd2, 0x11, 0x98, 0x87, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x5b};
        std::copy(g, g + 16, i.persistGuid);
        i.nInts = 8;
        i.bypassSlot = -1;
        i.params = {
            {"Effect", 0, 2, 0, "", 0, 1, Choice, "Flange,Wah-Wah (other),Wah-Wah", 0, false},
            {"Rate", 0.001, 20, 1, "Hz", 3, 1000, Num, "", 3, false},
            {"Depth", 1, 100, 50, "%", 4, 1, Num, "", 0, false},
            {"Mix", 0, 100, 50, "%", 7, 10, Num, "", 1, false},
        };
        i.presets = {
            {"(Default)", {0, -800, 0, 1000, 50, 500, 75, 500}},
            {"Default all parameters", {0, -800, 0, 1000, 50, 500, 75, 500}},
            {"Bouncing flange", {0, -800, 0, 1000, 50, 500, 75, 500}},
            {"Slow flange", {0, -800, 0, 200, 80, 500, 75, 500}},
            {"Mad flange", {0, -800, 0, 20000, 100, 500, 75, 1000}},
            {"Tremolo flange", {0, -800, 0, 15000, 25, 500, 75, 500}},
            {"Bouncing wah", {2, -800, 0, 1000, 50, 500, 75, 500}},
            {"Slow wah", {2, -800, 0, 200, 80, 500, 75, 500}},
            {"Mad wah", {2, -800, 0, 20000, 100, 500, 75, 1000}},
            {"Tremolo wah", {2, -800, 0, 15000, 25, 500, 75, 500}},
        };
        i.make = [] { return std::unique_ptr<Engine>(new XpFlangeEngine()); };
        return i;
    }();
    return info;
}

}  // namespace vegpack1
}  // namespace reno
