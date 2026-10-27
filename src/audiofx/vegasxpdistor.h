// VEGAS's ExpressFX Distortion (sfxpfx1_x64.dll, CXpDistor, DirectX class 00000002-0f56-11d2-9887-00a0c969725b): its
// settings, presets and sound. The engine is the filter's own code (its "apply settings" routine and its per-sample
// routine, called once per channel over the whole block) translated line by line.
//
// The sound: the input is pushed towards a threshold (soft: a knee set by Distortion, hard: a clip at the threshold with
// the knee flag), averaged with itself delayed by half the Slew setting (up to 250 samples), limited in how fast it may
// move (the slew limit) and mixed with the original.
#pragma once
#include "vegaspack1.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {
namespace vegpack1 {

// The settings structure (4 ints): 0 Slew and 1 Distortion in tenths of a percent (0..1000), 2 Enable hard clipping,
// 3 the mix of the effect against the original in tenths of a percent (1000: only the effect).
class XpDistorEngine : public Engine {
public:
    void setup(const int32_t* p, uint32_t rate, bool keep) override
    {
        (void)rate;
        // "apply settings" (the plug-in's 0x1800126a0)
        wet_ = (double)p[3] / 1000.0;
        dry_ = 1.0 - wet_;
        if (p[0] == 1000) {
            const uint64_t bits = 0x3feff7ced916872bull;     // 0.999
            std::memcpy(&d80_, &bits, 8);
        } else {
            d80_ = (double)p[0] / 1000.0;
        }
        const int32_t sgn = p[0] >> 31;
        delay_ = (p[0] - sgn) >> 1;
        hard_ = p[2];
        double x6 = 1.0;
        if (hard_ != 0) {
            const double x = (double)p[1] / 1000.0 * 0.95;
            x6 -= x;
            d88_ = x6;
            if (x6 == 0.0) d90_ = 0.5;
            else d90_ = x6 * 0.5 + 0.5;
            gain_ = 1.0;
        } else {
            if (p[1] == 0) d88_ = 0.0;
            else d88_ = std::log10((double)p[1]) / 3.0;
            const double a = x6 - d88_ * 0.98;
            double b = x6 - a;
            d88_ = a;
            d90_ = a;
            b *= a;
            b += a;
            gain_ = x6 / b;
        }
        if (!keep) {
            std::memset(ring_, 0, sizeof ring_);
            std::memset(idx_, 0, sizeof idx_);
            std::memset(prev_, 0, sizeof prev_);
        }
    }
    void process(float* left, float* right, int frames) override
    {
        float* ch[2] = {left, right};
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < frames; ++k) ch[c][k] = (float)sample((double)ch[c][k], c);
    }

private:
    double wet_ = 0, dry_ = 1, d80_ = 0, d88_ = 0, d90_ = 0, gain_ = 1;
    int32_t delay_ = 0, hard_ = 0;
    double ring_[2][500];
    int32_t idx_[2];
    double prev_[2];

    // the plug-in's per-sample routine (0x180012a70)
    double sample(double x, int c)
    {
        const double thr = d90_;
        const double kk = (1.0 - d80_) * 0.01;
        double t = thr;
        bool flag = false;
        bool inside = false;
        if (x > thr || (hard_ != 0 && x > 0.0)) {
            flag = kk > x;
        } else {
            t = -thr;
            if (t > x || (hard_ != 0 && !(0.0 <= x))) flag = x > -kk;
            else inside = true;                                 // nothing to push
        }
        double y = x;
        if (!inside) {
            y = (x - t) * d88_ + t;
            if (flag && d88_ != 1.0) y *= x / 0.01;
        }
        y *= gain_;
        // average with the sample 'delay_' back
        int32_t i = idx_[c];
        ring_[c][i] = y;
        int32_t j = i - delay_;
        if (j < 0) j += 500;
        y = (y + ring_[c][j]) * 0.5;
        idx_[c] = i + 1 == 500 ? 0 : i + 1;
        // slew limit
        const double pv = prev_[c];
        const double lim = 1.0 - d80_ * 0.9;
        const double diff = y - pv;
        if (d80_ != 1.0) {
            if (diff > lim) y = pv + lim;
            else if (0.0 - lim > diff) y = pv - lim;
        }
        prev_[c] = y;
        return y * wet_ + x * dry_;
    }
};

inline const Info& xpDistorInfo()
{
    static const Info info = [] {
        Info i;
        i.uid = "reno.xpdistortion";
        i.name = "ExpressFX Distortion";
        i.clsid = "00000002-0f56-11d2-9887-00a0c969725b";
        const uint8_t g[16] = {0x84, 0xfb, 0xb5, 0xc7, 0x31, 0x10, 0xd2, 0x11, 0x98, 0x87, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x5b};
        std::copy(g, g + 16, i.persistGuid);
        i.nInts = 4;
        i.bypassSlot = -1;
        i.params = {
            {"Slew", 0, 100, 0, "%", 0, 10, Num, "", 1, false},
            {"Distortion", 0, 100, 50, "%", 1, 10, Num, "", 1, false},
            {"Enable Hard Clipping", 0, 1, 0, "", 2, 1, Bool, "", 0, false},
            {"Mix", 0, 100, 50, "%", 3, 10, Num, "", 1, false},
        };
        i.presets = {
            {"(Default)", {0, 500, 0, 500}},
            {"Default all parameters", {0, 500, 0, 500}},
            {"Mangle", {0, 1000, 1, 1000}},
            {"Fuzz", {1000, 750, 0, 500}},
            {"Gristle", {750, 250, 1, 500}},
            {"Spaced", {50, 0, 0, 1000}},
        };
        i.make = [] { return std::unique_ptr<Engine>(new XpDistorEngine()); };
        return i;
    }();
    return info;
}

}  // namespace vegpack1
}  // namespace reno
