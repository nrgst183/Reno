// VEGAS's ExpressFX Amplitude Modulation (sfxpfx2_x64.dll, CXpAmpmod, DirectX class 00000005-0f56-11d2-9887-00a0c969725b):
// its settings, presets and sound. The engine is the plug-in's own code translated line by line: the routine that applies
// the settings (it builds the table of a thousand gains from the shape of the modulation, the rate and the depth), and the
// loop that runs the sound through that table with a free-running phase.
#pragma once
#include "vegaspack1.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace reno {
namespace vegpack1 {

// The settings structure (74 ints, as the plug-in saves it): 0 the corner of the smoothing filter in Hz, 1 the smoothing
// switch, 2 the phase of the right channel against the left in percent (of half a period), 3 makes the two ends of the shape
// meet, 4 the depth in tenths of a percent (0 to 1000), 5 the rate in tenths of a Hz (1 to 50000), 6 and 7 are saved but
// nothing reads them, 8 the mix, modulated sound against the original, in tenths of a percent (0 to 1000), 9 the number of
// points of the shape and from 10 the points as pairs (position 0 to 1000, level -100 to 0).
class XpAmpModEngine : public Engine {
public:
    XpAmpModEngine() { std::memset(tab_, 0, sizeof tab_); }

    void setup(const int32_t* p, uint32_t rate, bool keep) override
    {
        if (rate < 1000) rate = 1000;
        const bool reset = !keep || !ready_ || rate != rate_;
        rate_ = rate;
        if (reset) {
            apply(p, true);
            ready_ = true;
        } else {
            // (the plug-in works out the right channel's phase again when that setting changes)
            if (p[2] != lastPhase_) phaseChanged_ = true;
            apply(p, false);
        }
        lastPhase_ = p[2];
    }

    void process(float* left, float* right, int frames) override
    {
        if (!ready_) return;
        float* ch[2] = {left, right};
        if (!stereoPhase_) {
            for (int k = 0; k < frames; ++k) {
                if (phase1_ >= 1000.0) phase1_ -= 1000.0;
                const int idx = index(phase1_);
                for (int c = 0; c < 2; ++c) ch[c][k] = voice(ch[c][k], idx, c);
                phase1_ = phase1_ + inc_;
            }
        } else {
            for (int k = 0; k < frames; ++k) {
                if (phase1_ >= 1000.0) phase1_ -= 1000.0;
                if (phase2_ >= 1000.0) phase2_ -= 1000.0;
                ch[0][k] = voice(ch[0][k], index(phase1_), 0);
                ch[1][k] = voice(ch[1][k], index(phase2_), 1);
                phase2_ = inc_ + phase2_;
                phase1_ = inc_ + phase1_;
            }
        }
    }

private:
    // cvttsd2si into 32 bits: whatever does not fit gives the "integer indefinite" value
    static int trunc32(double d)
    {
        if (!(d > -2147483649.0 && d < 2147483648.0)) return INT_MIN;
        return (int)d;
    }
    // the table place of a phase. The plug-in's table has 1001 doubles and the fields that follow it in the object lie right
    // behind them (place 1000 is the phase step, 1001 the first phase, 1003 the second, 1004 and 1005 the smoothing
    // factors, 1006 and 1007 the smoothing memories), and a phase that is still past the end after its one wrap reads
    // those, so tableAt() gives them the same way; a phase beyond that is held at the last of them
    static int index(double phase)
    {
        const double t = phase;
        if (!(t >= 0.0)) return 0;
        if (t >= 1008.0) return 1007;
        return (int)t;
    }
    double tableAt(int idx) const
    {
        switch (idx) {
        case 1000: return inc_;
        case 1001: return phase1_;
        case 1002: return 0.0; // the 32-bit flag of the stereo phase, a denormal number
        case 1003: return phase2_;
        case 1004: return a_;
        case 1005: return b_;
        case 1006: return z_[0];
        case 1007: return z_[1];
        default: return tab_[idx];
        }
    }

    // 0x180006900 and the body of the loop: one sample of one channel, through the table and the smoothing
    float voice(float in, int idx, int c)
    {
        const double x = (double)in;
        const double dryPart = x * dry_;
        double w = x * tableAt(idx);
        w = w * wet_;
        w = w * b_;
        const double z = a_ * z_[c];
        w = w + z;
        if (2e-11 > std::fabs(w)) w = 0.0;
        z_[c] = w;
        return (float)(w + dryPart);
    }

    // 0x180006000: a straight line in the table between two places
    void fill(int a, int b)
    {
        int lo = a, hi = b;
        if (lo > hi) std::swap(lo, hi);
        const double dx = (double)(hi - lo);
        if (dx == 0.0) return;
        if (lo < 0 || hi > 1000) return;
        const double slope = (tab_[hi] - tab_[lo]) / dx;
        for (int k = lo + 1; k < hi; ++k) tab_[k] = (double)(k - lo) * slope + tab_[lo];
    }

    // 0x1800061a0: the table from the points of the shape
    void shape(const int32_t* st)
    {
        int n = st[9];
        if (n < 0) n = 0;
        if (n > 32) n = 32;
        int P[2 * 40];
        std::memset(P, 0, sizeof P);
        for (int i = 0; i < 2 * n; ++i) P[i] = st[10 + i];
        if (n < 2) return;
        if (st[3] != 0) {
            const int y0 = P[1];
            const int ylast = P[2 * (n - 1) + 1];
            int mid = (y0 + ylast) / 2;
            const int x1 = P[2];
            if (x1 > 50) {
                const int y1 = P[3];
                double slope = (double)(y1 - y0) / (double)x1;
                for (int k = n - 1; k >= 1; --k) {
                    P[2 * (k + 1)] = P[2 * k];
                    P[2 * (k + 1) + 1] = P[2 * k + 1];
                }
                slope *= -50.0;
                ++n;
                P[2] = 50;
                const int e = trunc32(slope);
                int r9 = (int)((uint32_t)y0 - (uint32_t)e);
                if (r9 < -100) P[3] = -100;
                else {
                    if (r9 > 0) r9 = 0;
                    P[3] = r9;
                }
            }
            const int k2 = n - 2;
            const int xk = P[2 * k2];
            if (xk < 950) {
                const int last = n - 1;
                double slope = (double)(P[2 * last + 1] - P[2 * k2 + 1]);
                slope /= (double)(P[2 * last] - xk);
                P[2 * n] = P[2 * last];
                P[2 * n + 1] = P[2 * last + 1];
                slope *= 50.0;
                ++n;
                const int idx = n - 2;
                P[2 * idx] = 950;
                const int yl = P[2 * (n - 1) + 1];
                const int e = trunc32(slope);
                int v = (int)((uint32_t)yl - (uint32_t)e);
                if (v < -100) v = -100;
                else if (v > 0) v = 0;
                P[2 * idx + 1] = v;
            }
            if (mid < -100) {
                P[2 * (n - 1) + 1] = -100;
                P[1] = -100;
            } else {
                if (mid > 0) mid = 0;
                P[2 * (n - 1) + 1] = mid;
                P[1] = mid;
            }
        }
        for (int i = 0; i < n - 1; ++i) {
            const int xa = P[2 * i], xb = P[2 * (i + 1)];
            if (xa >= 0 && xa <= 1000) tab_[xa] = (double)P[2 * i + 1];
            if (xb >= 0 && xb <= 1000) tab_[xb] = (double)P[2 * (i + 1) + 1];
            fill(xb, xa);
        }
    }

    // 0x180006420: applies the settings
    void apply(const int32_t* st, bool reset)
    {
        wet_ = (double)st[8] / 1000.0;
        dry_ = 1.0 - wet_;
        double d = 1.0 - (double)st[4] / 1000.0;
        if (d == 0.0) d = 1e-05;
        const double depthDb = -(std::log10(d) * 20.0);
        shape(st);
        for (int i = 0; i < 1001; ++i) {
            double v = tab_[i] / 100.0;
            v = v * depthDb;
            v = v / 20.0;
            tab_[i] = std::pow(10.0, v);
        }
        const double hz = (double)st[5] / 10.0;
        const int cycle = trunc32((double)rate_ / hz);
        inc_ = 1000.0 / (double)cycle;
        if (reset) {
            phase1_ = 0.0;
            z_[0] = z_[1] = 0.0;
        }
        if (phaseChanged_ || reset) {
            if (st[2] != 0) {
                stereoPhase_ = true;
                double o = (double)st[2] / 100.0;
                o = o * 0.5;
                o = o * 1000.0;
                phase2_ = o + phase1_;
            } else {
                stereoPhase_ = false;
            }
            phaseChanged_ = false;
        }
        if (st[1] != 0 && !(st[0] > (int)rate_ / 2)) {
            a_ = std::exp((double)st[0] * -6.28318530718 / (double)rate_);
            b_ = 1.0 - a_;
        } else {
            a_ = 0.0;
            b_ = 1.0;
        }
    }

    bool ready_ = false, phaseChanged_ = true, stereoPhase_ = false;
    uint32_t rate_ = 48000;
    int32_t lastPhase_ = 0;
    double tab_[1001];
    double wet_ = 0, dry_ = 1, inc_ = 0, phase1_ = 0, phase2_ = 0, a_ = 0, b_ = 1;
    double z_[2] = {0, 0};
};

inline const Info& xpAmpModInfo()
{
    static const Info info = [] {
        Info i;
        i.uid = "reno.xpampmod";
        i.name = "ExpressFX Amplitude Modulation";
        i.clsid = "00000005-0f56-11d2-9887-00a0c969725b";
        // the effect's own GUID, written before its settings: ba97fc2c-0f62-11d2-9887-00a0c969725b
        const uint8_t g[16] = {0x2c, 0xfc, 0x97, 0xba, 0x62, 0x0f, 0xd2, 0x11, 0x98, 0x87, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x5b};
        std::copy(g, g + 16, i.persistGuid);
        i.nInts = 74;
        i.bypassSlot = -1;
        i.params = {
            {"Rate", 0.1, 5000, 1, "Hz", 5, 10, Num, "", 1, false},
            {"Depth", 0, 100, 100, "%", 4, 10, Num, "", 1, false},
            {"Mix", 0, 100, 50, "%", 8, 10, Num, "", 1, false},
        };
        // the shape the presets share: a dip of a hundred percent in the middle of the period, in twelve points
        static const int kShape[24] = {0, -50, 125, -12, 188, -3, 250, 0, 313, -3, 375, -12, 625, -88, 688, -97, 750, -100, 813, -97, 875, -88, 1000, -50};
        auto make = [](int depth, int rate, int mix) {
            std::vector<int32_t> v(74, 0);
            v[0] = 400;
            v[4] = depth;
            v[5] = rate;
            v[7] = -800;
            v[8] = mix;
            v[9] = 12;
            for (int k = 0; k < 24; ++k) v[(size_t)(10 + k)] = kShape[k];
            return v;
        };
        i.presets = {
            {"(Default)", make(1000, 10, 500)},
            {"Default all parameters", make(1000, 10, 500)},
            {"Slow modulation", make(500, 20, 750)},
            {"Fast modulation", make(750, 150, 1000)},
            {"440 Hz noise", make(1000, 4400, 1000)},
        };
        i.make = [] { return std::unique_ptr<Engine>(new XpAmpModEngine()); };
        return i;
    }();
    return info;
}

}  // namespace vegpack1
}  // namespace reno
