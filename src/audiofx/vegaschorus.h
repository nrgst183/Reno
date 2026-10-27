// VEGAS's Chorus (sfppack1_x64.dll, CSfChorus, DirectX class 28d9f1e0-6ecc-11d0-aebc-00a0c9053912): its settings,
// presets and sound. The engine is the plug-in's own code (its two "apply settings" routines and its per-voice sample
// routine) translated line by line, constants included, down to the quirks (a fresh start clears only the first half of
// each delay line).
#pragma once
#include "vegaspack1.h"
#include "vegassimpledelay.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {
namespace vegpack1 {

// The settings structure (16 ints): 0 input gain, 1 dry out and 2 chorus out in tenths of a dB (-600 and below: none),
// 3 chorus size (voices, 1 to 3), 4 feedback in %, 5 invert chorus phase, 6 delay in tenths of a ms, 7 modulation rate in
// mHz, 8 modulation depth in %, 9 attenuate high frequencies and 10 their frequency in Hz, 11 invert feedback phase,
// 12 bypass, 13 tempo note (18: measures), 14 its multiplier (x1000), 15 tempo sync.
class ChorusEngine : public Engine {
public:
    void setup(const int32_t* p, uint32_t rate, bool keep) override
    {
        if (rate < 1000) rate = 1000;
        if (!allocated_ || rate != rate_) allocate(rate);
        const Tempo t = tempo();
        if (!keep || !haveLast_) {
            full(p, t, true);
        } else if (p[3] != last_[3] || p[6] != last_[6] || t.bpm != lastBpm_ || t.num != lastNum_) {
            full(p, t, false);
        } else {
            light(p, t);
        }
        std::memcpy(last_, p, sizeof last_);
        lastBpm_ = t.bpm;
        lastNum_ = t.num;
        haveLast_ = true;
    }

    void process(float* left, float* right, int frames) override
    {
        if (!allocated_) return;
        for (int k = 0; k < frames; ++k) {
            double l = (double)left[k], r = (double)right[k];
            pass(0, &l, &r);
            if (size_ == 2 || size_ == 3) pass(1, &l, &r);
            if (size_ == 3) pass(2, &l, &r);
            left[k] = (float)l;
            right[k] = (float)r;
        }
    }

private:
    static constexpr double kMult[3] = {1.0, 0.811731, 1.132329};   // each voice's modulation period, relative

    void allocate(uint32_t rate)
    {
        rate_ = rate;
        double x = (double)(float)rate;
        x *= 2.0;
        x /= 10.0;
        x += 100.0;
        bufLen_ = (int)std::ceil(x);
        if (bufLen_ < 8) bufLen_ = 8;
        for (int i = 0; i < 3; ++i) {
            buf_[i].assign((size_t)bufLen_ * 2, 0.0);
            ctr_[i] = 0;
            w_[i] = 0;
            off_[i] = 0.0;
            st_[i][0] = st_[i][1] = 0.0;
        }
        allocated_ = true;
        haveLast_ = false;
    }

    static double dbGain(int32_t v) { return v > -600 ? std::pow(10.0, (double)v / 200.0) : 0.0; }

    // what both routines compute
    void common(const int32_t* p, const Tempo& t, bool clampPeriod)
    {
        const uint32_t rate = rate_;
        inG_ = dbGain(p[0]);
        dry_ = dbGain(p[1]);
        double out = dbGain(p[2]);
        if (p[5] != 0) out = out * -1.0;
        outG_ = out;
        gl_ = out * 0.9;
        gr_ = out * 0.95;
        outG_ = outG_ * 0.9;
        double fb = (double)p[4] / 100.0;
        if (p[11] != 0) fb = fb * -1.0;
        fb_ = fb;
        // the length of one modulation period, in seconds
        if (p[15] != 0) {
            double mult = (double)p[14] / 1000.0;
            double spb = 2.0;
            if (t.flags & 1) spb = 60.0 / t.bpm;
            const int note = p[13];
            if (note == 18) {
                double x = 4.0;
                if (t.flags & 4) x = (4.0 / (double)t.den) * (double)t.num;
                x *= spb;
                x *= mult;
                period_ = x;
            } else {
                double x = spb * noteQuarters()[std::max(0, std::min(17, note))];
                x *= mult;
                period_ = x;
            }
        } else if (p[7] != 0) {
            period_ = 1000.0 / (double)p[7];
        } else {
            period_ = 1.0;
        }
        for (int i = 0; i < 3; ++i) {
            double n = (double)(int)((double)rate * period_ * kMult[i]);
            if (n < 1.0) n = 1.0;
            periodSamples_[i] = n;
            inc_[i] = 6.28318530718 / n;
        }
        (void)clampPeriod;
        if (p[9] != 0 && p[10] < (int32_t)(rate >> 1)) {
            const double a = std::exp((double)p[10] * -6.28318530718 / (double)rate);
            lpA_ = a;
            lpB_ = 1.0 - a;
        } else {
            lpA_ = 0.0;
            lpB_ = 1.0;
        }
    }

    void light(const int32_t* p, const Tempo& t)
    {
        common(p, t, true);
        depth_ = (double)(int)((double)p[8] / 100.0 * maxDepth_);
    }

    void full(const int32_t* p, const Tempo& t, bool clear)
    {
        size_ = p[3];
        if (clear)
            for (int i = 0; i < 3; ++i) std::fill(buf_[i].begin(), buf_[i].begin() + bufLen_, 0.0);   // (the first half only)
        common(p, t, false);
        maxDepth_ = (double)(int)((double)p[6] * (double)rate_ / 10000.0);
        depth_ = (double)(int)((double)p[8] / 100.0 * maxDepth_);
        for (int i = 0; i < 3; ++i) {
            st_[i][0] = st_[i][1] = 0.0;
            const double base = (double)(int)(kMult[i] * maxDepth_);
            if (clear) {
                w_[i] = 0;
                off_[i] = base;
            } else {
                double o = (double)w_[i] + base;
                if (o > (double)bufLen_) o -= (double)bufLen_;
                off_[i] = o;
            }
        }
    }

    static bool tiny(double v) { return 2e-11 > std::fabs(v); }

    void pass(int i, double* pl, double* pr)
    {
        double xL, xR;
        if (i == 0) {
            xR = inG_ * *pr;
            xL = inG_ * *pl;
        } else {
            xL = *pl;
            xR = *pr;
        }
        const double s = std::sin((double)ctr_[i] * inc_[i]);
        double x = s * depth_ + off_[i];
        const double bd = (double)bufLen_;
        if (!(x < bd)) x += (0.0 - bd);
        else if (0.0 > x) x += bd;
        double tt = x - 0.499999985;
        tt += 6755399441055744.0;
        int64_t bits;
        std::memcpy(&bits, &tt, 8);
        int n = (int)(int32_t)(uint32_t)(bits & 0xffffffff);
        // (right at the end of the line the plug-in reads one place past it, whatever the heap holds there: a tiny number)
        const uint32_t b2 = (uint32_t)bufLen_ * 2u;
        const uint32_t curL = (uint32_t)(n + n);
        const uint32_t nextL = (curL + 2u) % b2;
        const double f = x - (double)n;
        const std::vector<double>& b = buf_[i];
        auto rd = [&](uint32_t k) { return k < b2 ? b[k] : 0.0; };
        double l1 = (rd(nextL) - rd(curL)) * f + rd(curL);
        const uint32_t curR = (curL + 1u) % b2;
        l1 *= lpB_;
        const double r0 = rd(curR);
        const uint32_t nextR = (nextL + 1u) % b2;
        double r1 = (rd(nextR) - r0) * f + r0;
        double sL = st_[i][0] * lpA_ + l1;
        if (tiny(sL)) sL = 0.0;
        st_[i][0] = sL;
        r1 *= lpB_;
        double sR = st_[i][1] * lpA_ + r1;
        if (tiny(sR)) sR = 0.0;
        st_[i][1] = sR;
        off_[i] -= 1.0;
        if (0.0 > off_[i]) off_[i] = (double)(bufLen_ - 1);
        ++ctr_[i];
        if (!((double)ctr_[i] < periodSamples_[i])) ctr_[i] = 0;
        if (i == 0) {
            const double dryL = xL * dry_;
            *pl = sL * outG_ * gl_ + dryL;
            const double dryR = xR * dry_;
            *pr = sR * outG_ * gr_ + dryR;
        } else {
            *pl = sL * gl_ + xL;
            *pr = sR * gr_ + xR;
        }
        const double fbL = fb_ * sL + xL, fbR = fb_ * sR + xR;
        std::vector<double>& wb = buf_[i];
        const uint32_t at = ((uint32_t)w_[i] * 2u) % b2;
        wb[at] = fbL;
        wb[(at + 1u) % b2] = fbR;
        if (--w_[i] < 0) w_[i] = bufLen_ - 1;
    }

    int32_t last_[16] = {0};
    bool haveLast_ = false, allocated_ = false;
    double lastBpm_ = 0;                // the project tempo the delays were last worked out for
    int lastNum_ = 0;
    uint32_t rate_ = 0;
    int bufLen_ = 0, size_ = 1;
    std::vector<double> buf_[3];
    int ctr_[3] = {0}, w_[3] = {0};
    double off_[3] = {0}, st_[3][2] = {{0}}, periodSamples_[3] = {1, 1, 1}, inc_[3] = {0};
    double inG_ = 0, dry_ = 0, outG_ = 0, gl_ = 0, gr_ = 0, fb_ = 0, lpA_ = 0, lpB_ = 1, period_ = 1, depth_ = 0, maxDepth_ = 0;
};
constexpr double ChorusEngine::kMult[3];

inline const Info& chorusInfo()
{
    static const Info info = [] {
        Info i;
        i.uid = "reno.chorus";
        i.name = "Chorus";
        i.clsid = "28d9f1e0-6ecc-11d0-aebc-00a0c9053912";
        const uint8_t g[16] = {0x19, 0xe8, 0x9b, 0x03, 0xbb, 0xe0, 0xf6, 0x40, 0x9f, 0x59, 0x0e, 0xe9, 0x35, 0xff, 0x0d, 0x74};
        std::copy(g, g + 16, i.persistGuid);
        i.nInts = 16;
        i.bypassSlot = 12;
        i.params = {
            {"Modulation Rate", 0.001, 20, 0.8, "Hz", 7, 1000, Num, "", 3, false},
            {"Modulation Depth", 1, 100, 3, "%", 8, 1, Num, "", 0, false},
            {"Delay", 0.1, 100, 40, "ms", 6, 10, Num, "", 1, false},
            {"Feedback", 0, 100, 0, "%", 4, 1, Num, "", 0, false},
            {"Chorus Size", 1, 3, 3, "", 3, 1, Num, "", 0, false},
            {"Invert Chorus Phase", 0, 1, 0, "", 5, 1, Bool, "", 0, false},
            {"Invert Feedback Phase", 0, 1, 0, "", 11, 1, Bool, "", 0, false},
            {"Attenuate High Frequencies", 0, 1, 0, "", 9, 1, Bool, "", 0, false},
            {"Attenuation Frequencies", 100, 10000, 5000, "Hz", 10, 1, Num, "", 0, false},
            {"Input Gain", -60, 0, 0, "dB", 0, 10, Num, "", 1, true},
            {"Dry Out", -60, 0, -6, "dB", 1, 10, Num, "", 1, true},
            {"Chorus Out", -60, 0, -4.4, "dB", 2, 10, Num, "", 1, true},
            {"Bypass", 0, 1, 0, "", 12, 1, Bool, "", 0, false},
            {"Modulation Period", 0, 18, 18, "", 13, 1, Choice, VEGPACK1_NOTES ",Measures", 0, false},
            {"Modulation Period Multiplier", 0.001, 32, 1, "", 14, 1000, Num, "", 3, false},
            {"Tempo Sync", 0, 1, 0, "", 15, 1, Bool, "", 0, false},
        };
        i.presets = {
            {"(Default)", {0, -60, -44, 3, 0, 0, 400, 800, 3, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Fast flange 1", {0, -34, -34, 1, 32, 0, 12, 700, 100, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Fast flange 2", {0, -57, -57, 1, 70, 0, 50, 2300, 16, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Slow flange 1", {0, -49, -57, 1, 50, 0, 8, 500, 75, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Slow flange 2", {-20, -80, -44, 1, 60, 0, 12, 500, 100, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Slow flange 3", {0, -80, -31, 1, 20, 0, 5, 700, 80, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Slow flange 4", {0, -44, -31, 1, 0, 0, 6, 200, 75, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Chorus/vibrato 1", {0, -37, -37, 1, 57, 0, 100, 500, 100, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Chorus/vibrato 2", {0, -37, -69, 1, 37, 0, 292, 7000, 8, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Chorus 1", {0, -60, -44, 3, 0, 0, 400, 800, 3, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Chorus 2", {0, -60, -60, 2, 15, 0, 300, 500, 8, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Chorus 3", {0, -60, -60, 3, 16, 0, 405, 500, 8, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Chorus 4", {0, -49, -52, 2, 26, 0, 90, 500, 26, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Chorus 5", {0, -49, -77, 2, 37, 0, 292, 2100, 12, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Cheap TV", {-20, 0, 0, 2, 0, 1, 1, 100, 100, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Echo-vibrato 1", {0, -92, -140, 2, 15, 0, 1000, 6000, 8, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Echo-vibrato 2", {0, -92, -60, 1, 0, 0, 1000, 6000, 5, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Echo-vibrato 3", {0, -92, -80, 3, 50, 0, 1000, 20000, 1, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Echo-vibrato 4", {0, -92, -60, 1, 50, 0, 1000, 20000, 1, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Vibrato-verb 1", {0, -92, -140, 1, 60, 0, 1000, 5000, 1, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Vibrato-verb 2", {0, -92, -91, 3, 50, 0, 1000, 7000, 1, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Vibrato-verb 3", {0, -92, -60, 3, 60, 0, 1000, 5600, 3, 0, 5000, 0, 0, 18, 1000, 0}},
            {"Hard flange", {-20, -31, -31, 1, 90, 0, 8, 500, 75, 0, 5000, 0, 0, 18, 1000, 0}},
        };
        i.make = [] { return std::unique_ptr<Engine>(new ChorusEngine()); };
        return i;
    }();
    return info;
}

}  // namespace vegpack1
}  // namespace reno
