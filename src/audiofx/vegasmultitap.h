// VEGAS's Multi-Tap Delay (sfppack1_x64.dll, CSfEchomt, DirectX class 7298a3e0-78ee-11d0-aebc-00a0c9053912): its
// settings, presets and sound. The engine is the plug-in's own code (its "apply settings" routines and its stereo
// per-sample routine) translated line by line, constants included.
#pragma once
#include "vegaspack1.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {
namespace vegpack1 {

// The settings structure (35 ints): 0-7 delay of each tap in ms, 8-15 its gain in %, 16-23 its pan in % (-100 left, 100
// right), 24 number of taps, 25 wet out and 26 dry out in tenths of a dB (-800 and below: none), 27 feedback in %, 28 input
// gain (tenths of a dB), 29 low-pass on, 30 its start frequency in Hz, 31 modulation depth in %, 32 modulation rate in
// tenths of a Hz, 33 and 34 the dialog's own state (current tap, graph resolution; not part of the sound).
class MultiTapEngine : public Engine {
public:
    void setup(const int32_t* p, uint32_t rate, bool keep) override
    {
        if (rate < 1000) rate = 1000;
        bool structural = !haveLast_ || rate != rate_;
        for (int i = 0; i < 35 && !structural; ++i)
            if (isStructural(i) && p[i] != last_[i]) structural = true;
        if (!keep || structural) full(p, rate, keep);
        else light(p, rate);
        std::memcpy(last_, p, sizeof last_);
        haveLast_ = true;
        rate_ = rate;
    }

    void process(float* left, float* right, int frames) override
    {
        if (bufLen_ < 4) return;
        for (int k = 0; k < frames; ++k) {
            double l = (double)left[k], r = (double)right[k];
            frame(&l, &r);
            left[k] = (float)l;
            right[k] = (float)r;
        }
    }

private:
    static bool isStructural(int i) { return i <= 24 || i == 27 || i == 31; }
    static double flush(double v)
    {
        uint64_t u;
        std::memcpy(&u, &v, 8);
        if ((uint32_t)(((u >> 32) & 0x7ff00000u)) < 0x3df00000u) u = 0;
        std::memcpy(&v, &u, 8);
        return v;
    }
    static double dbGain(int32_t v) { return v > -800 ? std::pow(10.0, (double)v / 200.0) : 0.0; }

    // the part of the settings that does not change the size of anything
    void gainsAndFilters(const int32_t* p, uint32_t rate)
    {
        wet_ = dbGain(p[25]);
        dry_ = dbGain(p[26]);
        inG_ = dbGain(p[28]);
        if (p[29] != 0 && p[30] <= (int32_t)(rate / 2)) {
            const double a = std::exp((double)p[30] * -6.28318530718 / (double)rate);
            lpA_ = a;
            lpB_ = 1.0 - a;
        } else {
            lpA_ = 0.0;
            lpB_ = 1.0;
        }
    }
    void light(const int32_t* p, uint32_t rate)
    {
        gainsAndFilters(p, rate);
        double x = 10.0 / (double)std::max<int32_t>(1, p[32]);
        x *= (double)rate;
        lfoInc_ = 6.28318530718 / x;
    }

    void full(const int32_t* p, uint32_t rate, bool keep)
    {
        static const double kSpread[8] = {0.123, 0.377, -0.411, 0.095, -0.301, -0.253, 0.457, 0.179};
        static const double kDepth[8] = {0.7, 0.9, 0.4, 0.6, 0.8, 0.9, 0.1, 0.9};
        ntaps_ = std::max<int32_t>(0, std::min<int32_t>(8, p[24]));
        {
            double x = 10.0 / (double)std::max<int32_t>(1, p[32]);
            x *= (double)rate;
            lfoInc_ = 6.28318530718 / x;
        }
        double depth = (double)p[31] * 3e-05;
        depth *= (double)rate;
        for (int k = 0; k < 8; ++k) {
            spread_[k] = (double)rate * kSpread[k] / 1000.0;
            depth_[k] = depth * kDepth[k];
        }
        // gains of the taps to the two sides, and what they add up to
        double sum = 0.0;
        for (int k = 0; k < ntaps_; ++k) {
            const int32_t g = p[8 + k], pan = p[16 + k];
            double l, r;
            if (pan <= 0) {
                l = (double)g / 100.0;
                r = (((double)pan + 100.0) / 100.0 * (double)g) / 100.0;
            } else {
                r = (double)g / 100.0;
                l = ((100.0 - (double)pan) / 100.0 * (double)g) / 100.0;
            }
            gl_[k] = l;
            gr_[k] = r;
            sum += l;
            sum += r;
        }
        if (!(1.0 <= sum)) sum = 1.0;
        fbScale_ = (double)p[27] / (sum * 100.0);
        gainsAndFilters(p, rate);
        // where each tap reads: its delay in samples, spread a little so the taps do not line up
        const int msSamples = (int)((double)rate / 1000.0);
        int maxTap = 0;
        for (int k = 0; k < ntaps_; ++k) {
            const int d = (int)((int64_t)p[k] * msSamples) + (int)spread_[k];
            tapPos_[k] = d;
            if (d > maxTap) maxTap = d;
        }
        depth *= 4.0;
        int len = (int)((double)maxTap * 1.05 + depth);
        if (len < 4) len = 4;
        if (len > 64 * 1024 * 1024) len = 64 * 1024 * 1024;
        if (bufLen_ == len && keep && !buf_.empty()) return;     // same size: the echoes in it are kept
        bufLen_ = len;
        buf_.assign((size_t)len, 0.0);
        w_ = 0;
        phase_ = 0.0;
        wrap_ = len - 1;
        zL_ = zR_ = outL_ = outR_ = 0.0;
    }

    double at(int i) const { return (i >= 0 && i < bufLen_) ? buf_[(size_t)i] : 0.0; }

    void frame(double* pl, double* pr)
    {
        const double a = lpA_, b = lpB_;
        const double inL = inG_ * *pl, inR = inG_ * *pr;
        const double fL = flush(b * inL + a * zL_);
        const double fR = flush(a * zR_ + b * inR);
        zL_ = fL;
        zR_ = fR;
        const double s = std::sin(phase_);
        phase_ += lfoInc_;
        const double lfo = s + 1.0;
        double sumL = 0.0, sumR = 0.0;
        for (int k = 0; k < ntaps_; ++k) {
            const int pos = tapPos_[k];
            double x = (double)pos - lfo * depth_[k];
            if (0.0 > x) x += (double)wrap_;
            double t = x - 0.499999985;
            t += 6755399441055744.0;
            int64_t bits;
            std::memcpy(&bits, &t, 8);
            const int r10 = (int)(int32_t)(uint32_t)(bits & 0xffffffff);
            int r8 = r10 + 1;
            if (!(r8 < wrap_)) r8 -= wrap_;
            const double v0 = at(r10);
            double v = (at(r8) - v0) * (x - (double)r10) + v0;
            sumR += v * gr_[k];
            sumL += v * gl_[k];
            int np = pos - 1;
            if (np < 0) np = wrap_;
            tapPos_[k] = np;
        }
        const double dryL = inL * dry_;
        const double rawR = a * outR_ + b * sumR;
        const double rawL = b * sumL + a * outL_;
        const double oL = flush(rawL);
        outL_ = oL;
        const double oR = flush(rawR);
        outR_ = oR;
        *pl = flush(oL * wet_ + dryL);
        const double dryR = inR * dry_;
        *pr = flush(oR * wet_ + dryR);
        double fb = (oR + oL) * fbScale_;
        fb += (fR + fL);
        fb *= 0.5;
        if (fb > 16777216.0) fb = 16777216.0;
        else if (-16777216.0 > fb) fb = -16777216.0;
        buf_[(size_t)w_] = fb;
        if (--w_ < 0) w_ = wrap_;
    }

    int32_t last_[35] = {0};
    bool haveLast_ = false;
    uint32_t rate_ = 0;
    int ntaps_ = 0, bufLen_ = 0, wrap_ = 0, w_ = 0;
    std::vector<double> buf_;
    int tapPos_[8] = {0};
    double gl_[8] = {0}, gr_[8] = {0}, depth_[8] = {0}, spread_[8] = {0};
    double wet_ = 0, dry_ = 0, inG_ = 0, fbScale_ = 0, lpA_ = 0, lpB_ = 1, lfoInc_ = 0, phase_ = 0;
    double zL_ = 0, zR_ = 0, outL_ = 0, outR_ = 0;
};

inline const Info& multiTapInfo()
{
    static const Info info = [] {
        Info i;
        i.uid = "reno.multitapdelay";
        i.name = "Multi-Tap Delay";
        i.clsid = "7298a3e0-78ee-11d0-aebc-00a0c9053912";
        const uint8_t g[16] = {0xe3, 0xa3, 0x98, 0x72, 0xee, 0x78, 0xd0, 0x11, 0xae, 0xbc, 0x00, 0xa0, 0xc9, 0x05, 0x39, 0x12};
        std::copy(g, g + 16, i.persistGuid);
        i.nInts = 35;
        i.bypassSlot = -1;
        i.params = {
            {"Input Gain", -80, 0, 0, "dB", 28, 10, Num, "", 1, true},
            {"Dry Out", -80, 0, -6, "dB", 26, 10, Num, "", 1, true},
            {"Wet Out", -80, 0, -6, "dB", 25, 10, Num, "", 1, true},
            {"Modulation Rate", 0.1, 10, 0.1, "Hz", 32, 10, Num, "", 1, false},
            {"Modulation Depth", 0, 100, 0, "%", 31, 1, Num, "", 0, false},
            {"Feedback", 0, 150, 0, "%", 27, 1, Num, "", 0, false},
            {"Low-pass Filter", 0, 1, 0, "", 29, 1, Bool, "", 0, false},
            {"Low-pass Start Frequency", 20, 20000, 5000, "Hz", 30, 1, Num, "", 0, false},
            {"Number of Taps", 1, 8, 1, "", 24, 1, Num, "", 0, false},
        };
        static const char* const kN[8] = {"1", "2", "3", "4", "5", "6", "7", "8"};
        static std::string names[24];
        for (int k = 0; k < 8; ++k) {
            names[k] = std::string("Tap ") + kN[k] + " Delay";
            names[8 + k] = std::string("Tap ") + kN[k] + " Gain";
            names[16 + k] = std::string("Tap ") + kN[k] + " Pan";
            i.params.push_back({names[k].c_str(), 1, 2500, k == 0 ? 100.0 : 1.0, "ms", k, 1, Num, "", 0, false});
        }
        for (int k = 0; k < 8; ++k) i.params.push_back({names[8 + k].c_str(), -100, 100, 100, "%", 8 + k, 1, Num, "", 0, false});
        for (int k = 0; k < 8; ++k) i.params.push_back({names[16 + k].c_str(), -100, 100, 0, "%", 16 + k, 1, Num, "", 0, false});
        i.presets = {
            {"(Default)", {100, 1, 1, 1, 1, 1, 1, 1, 100, 100, 100, 100, 100, 100, 100, 100, 0, 0, 0, 0, 0, 0, 0, 0, 1, -60, -60, 0, 0, 0, 5000, 0, 1, 0, 0}},
            {"Small room 1", {17, 40, 85, 197, 275, 309, 1, 1, -48, 16, -11, 5, -2, 1, -100, -100, -24, 18, -14, -38, -6, -38, 0, 0, 6, -25, -92, 30, 0, 0, 5000, 5, 15, 0, 0}},
            {"Small hall 1", {34, 57, 113, 135, 174, 214, 281, 348, 39, 21, -8, -17, -11, 12, 5, 3, 0, 15, 26, 69, 31, 64, 39, 50, 8, -37, -14, 40, 0, 0, 5000, 10, 15, 0, 0}},
            {"Chorus-echo", {6, 13, 19, 33, 101, 163, 191, 207, -26, 25, -60, 57, -24, 10, -24, 28, -24, 18, -14, -38, -6, -38, 0, -18, 8, 0, -140, 0, 0, 0, 5000, 0, 1, 0, 0}},
            {"Chorus-reverse verb 2", {1, 12, 20, 32, 40, 57, 85, 113, 12, -17, 37, -42, 52, -49, 66, -85, -24, 18, -14, 20, -22, 0, 56, -44, 5, 0, -800, 0, 0, 0, 5000, 0, 1, 0, 0}},
            {"Chorus-reverse verb 3", {14, 32, 59, 86, 113, 126, 144, 162, 9, -11, 14, -24, 28, -35, 64, -85, 38, 59, 43, 60, 39, 50, 56, 33, 5, -31, -260, 0, 0, 0, 5000, 0, 1, 0, 0}},
            {"Chorus-reverse verb 1", {1, 12, 23, 29, 37, 44, 56, 65, 5, -11, 14, -24, 28, -35, 64, -85, -24, 18, -14, 20, -22, 0, 56, -44, 5, -31, -800, 0, 0, 0, 5000, 0, 1, 0, 0}},
            {"Low-frequency verb", {17, 187, 323, 361, 399, 427, 473, 500, 35, -24, 10, -8, -5, -2, 1, -1, -24, 18, -14, 20, -22, 0, 56, 0, 8, 0, -44, 70, 0, 1, 500, 0, 1, 0, 0}},
            {"Chorus-verb", {3, 7, 13, 19, 27, 33, 41, 53, 35, -31, 37, -40, 30, -35, 34, -51, -24, 18, -14, 20, -22, 0, 56, -44, 8, -60, -105, 3, 0, 0, 5000, 0, 1, 0, 0}},
            {"Stadium", {366, 500, 500, 1, 1, 1, 1, 1, 54, -10, 5, -100, -100, -100, -100, -100, -24, 18, -14, 0, 0, 0, 0, 0, 2, 0, -60, 40, 0, 1, 2000, 5, 15, 0, 0}},
            {"Medium hall 1", {71, 177, 315, 374, 424, 432, 453, 500, -58, -28, -12, 5, -5, 4, 11, -9, -30, 31, -18, -12, 20, 28, -22, 0, 5, -60, -92, 40, 0, 1, 4500, 10, 10, 0, 0}},
            {"Large hall 2", {100, 130, 185, 269, 424, 437, 453, 500, -51, -41, -22, -32, -9, 4, 11, -9, -30, 31, -18, -12, 20, 28, -22, 0, 6, -91, -14, 40, 0, 1, 4500, 15, 20, 0, 0}},
            {"Large hall 1", {90, 108, 212, 306, 361, 468, 482, 500, 54, -14, 13, 5, -6, 5, -2, 5, 38, 59, 43, 65, 55, 0, 0, 0, 8, -60, -92, 60, 0, 1, 4500, 10, 10, 0, 0}},
            {"Large hall 3", {100, 177, 315, 403, 474, 432, 453, 500, -58, -28, -12, 11, -5, 4, 11, -9, -30, 31, -18, -12, 20, 28, -22, 0, 5, -60, -92, 50, 0, 1, 4500, 10, 10, 0, 0}},
            {"Medium hall 2", {63, 177, 315, 403, 474, 432, 453, 500, -58, -22, -5, 4, -5, 4, 11, -9, -30, 31, -18, -12, 20, 28, -22, 0, 5, -60, -92, 40, 0, 1, 3500, 10, 10, 0, 0}},
            {"Cathedral 1", {191, 227, 249, 267, 291, 1, 1, 1, 30, -24, -15, -6, -2, -100, -100, -100, -20, 18, -28, 0, 6, 0, 0, 0, 5, -44, -44, 100, 0, 1, 4000, 5, 10, 0, 0}},
            {"Cathedral 2", {191, 269, 428, 500, 291, 1, 1, 1, -32, -24, -5, -6, -2, -100, -100, -100, -24, 18, -28, 0, 6, 0, 0, 0, 4, -20, -44, 100, 0, 1, 850, 10, 10, 0, 0}},
            {"Cathedral 3", {311, 369, 483, 500, 119, 1, 1, 1, -32, -24, -5, -6, 24, -100, -100, -100, -24, 18, -28, 0, 6, 0, 0, 0, 5, -37, -20, 70, 0, 1, 4000, 10, 10, 0, 0}},
            {"Large hall 4", {119, 311, 369, 461, 500, 1, 1, 1, -32, -19, -9, -9, -6, -100, -100, -100, -24, 18, -28, 0, 6, 0, 0, 0, 5, -37, -20, 80, 0, 1, 4000, 10, 10, 0, 0}},
            {"Small hall 2", {31, 110, 151, 206, 231, 336, 500, 500, -50, -12, -9, -9, -9, -5, -1, -9, -30, 31, -18, -12, 20, 28, -22, 0, 7, -44, 0, 50, 0, 1, 1000, 15, 10, 0, 0}},
            {"Small room 2", {27, 101, 193, 240, 302, 1, 1, 1, 100, -21, 8, -5, 3, -100, -100, -100, 38, 59, 43, 65, 55, 0, 0, 0, 5, -105, -92, 15, 0, 1, 2500, 0, 1, 0, 0}},
            {"Tapped delay 1", {17, 47, 89, 164, 240, 332, 416, 500, -91, 16, 77, -58, 73, -68, 70, -64, -24, 18, -14, -38, -6, -38, 0, 0, 8, -80, 0, 0, 0, 1, 4500, 0, 20, 0, 0}},
            {"Tapped delay 2", {17, 47, 89, 164, 240, 332, 416, 500, -91, 16, 77, -58, 73, -68, 70, -64, -24, 18, -14, -38, -6, -38, 0, 0, 8, -80, 0, 15, 0, 1, 4000, 0, 20, 0, 0}},
            {"Tapped delay, stereo pan", {17, 72, 118, 181, 240, 332, 416, 500, -91, 60, 40, -35, 31, -19, 14, -9, -100, 100, -100, 100, -100, 100, -100, 100, 8, -80, 0, 15, 0, 1, 4000, 0, 20, 0, 0}},
            {"Channel delay for stereo simulation", {1, 15, 212, 306, 361, 468, 482, 500, 100, 100, 13, 5, -6, 5, -2, 5, -100, 100, 43, 65, 55, 0, 0, 0, 2, 0, -800, 0, 0, 0, 4500, 0, 10, 0, 0}},
        };
        i.make = [] { return std::unique_ptr<Engine>(new MultiTapEngine()); };
        return i;
    }();
    return info;
}

}  // namespace vegpack1
}  // namespace reno
