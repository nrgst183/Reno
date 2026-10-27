// VEGAS's ExpressFX Reverb (sfxpfx1_x64.dll, CXpReverb, DirectX class 00000006-0f56-11d2-9887-00a0c969725b): its settings,
// presets and sound. The engine is the filter's own code (its "apply settings" routine and the per-frame routine for two
// channels) translated line by line.
//
// How it works: the input of both channels is averaged and passes a one-pole low pass (the "damping" at S10 Hz), then a
// resonant two-pole filter whose output is subtracted (the rumble cut at S8 Hz). The result feeds a bank of 8..10 comb
// filters (damped feedback, each with its own low pass); the left channel uses all of them, the right channel shares all but
// the first two and has two of its own (delays of 0.93 and 1.06 times the first two). The comb sums go through a delay of
// "room size" milliseconds. Three taps of the (dry) input of both channels, cross-mixed with per-channel gains, give the early
// reflections. Output = early reflections * level + delayed comb sum * wet + input * dry.
#pragma once
#include "vegaspack1.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {
namespace vegpack1 {

// The settings structure (12 ints): 0 and 1 two levels in tenths of a dB that the plug-in keeps but does not use
// (-800..0), 2 level of the early reflections (tenths of a dB, -800 = off), 3 room type (0..9: which set of comb delays),
// 4 pattern of the early reflections (0..9, a set of three tap times and gains), 5 Liveliness (0..100: the reverb time is
// this / 20 seconds), 6 Room size (0..200, shown as half a percent: the delay in ms before the reverb), 7 and 8 the rumble
// filter on/off and its frequency (10..5000 Hz), 9 and 10 the damping low pass on/off and its frequency (100..15000 Hz),
// 11 the mix of the reverb against the original in tenths of a percent.
class XpReverbEngine : public Engine {
public:
    XpReverbEngine() {}

    void setup(const int32_t* p, uint32_t rate, bool keep) override
    {
        int32_t n[12];
        std::memcpy(n, p, sizeof n);
        clampSettings(n);
        // the plug-in's setters raise its "rebuild" flag when the room type, the early pattern or the room size change
        // (and a loaded state always does); a rebuild makes new, silent buffers
        bool rebuild = !keep || !built_ || rate != rate_ || n[3] != s_[3] || n[4] != s_[4] || n[6] != s_[6];
        std::memcpy(s_, n, sizeof s_);
        rate_ = rate;
        prep(rebuild);
        built_ = true;
    }

    void process(float* left, float* right, int frames) override
    {
        for (int k = 0; k < frames; ++k) {
            double l = (double)left[k];
            double r = (double)right[k];
            frame(l, r);
            left[k] = (float)l;
            right[k] = (float)r;
        }
    }

private:
    int32_t s_[12] = {-800, -240, -160, 0, 2, 25, 30, 1, 20, 1, 12000, 500};
    uint32_t rate_ = 48000;
    bool built_ = false;

    struct Comb {
        std::vector<double> buf;
        size_t pos = 0;
        size_t len = 1;
        double gain = 0, lp = 0;
    };
    int count_ = 0;
    Comb comb_[10];
    Comb right_[2];
    // the delay of the comb sums (room size) and the three early taps, one ring per channel
    std::vector<double> pre_[2];
    size_t prePos_ = 0, preLen_ = 1;
    std::vector<double> tap_[2];
    size_t tapW_ = 0, tap0_ = 0, tap1_ = 0, tapLen_ = 1;
    double gL_[3] = {0, 0, 0}, gR_[3] = {0, 0, 0};
    // gains and filters
    double outGain_ = 0, wet_ = 0, dry_ = 1, lpA_ = 0, lpB_ = 1, inGain_ = 1, lpState_ = 0;
    double hp_[5] = {0, 0, 0, 0, 0};     // the two-pole filter: two past outputs, two feedback coefficients, the input gain

    struct RowA { int count; double d[10]; };
    struct RowB { double d[3]; int g[6]; };
    static const RowA* rowsA()
    {
        static const RowA t[10] = {
            {10, {23.583, 47.049, 25.481, 27.493, 29.67, 34.518, 32.056, 37.317, 40.358, 19.941}},
            {8, {23.161, 18.481, 20.611, 16.035, 26.45, 29.281, 12.957, 33.46, 8.812, 9.9256}},
            {10, {13.453, 24.966, 28.415, 12.028, 15.817, 17.836, 10.72, 19.719, 9.439, 22.011}},
            {9, {25.037, 34.831, 29.502, 37.527, 32.139, 22.731, 40.532, 43.812, 47.07, 9.958}},
            {10, {32.963, 35.329, 44.227, 38.224, 62.813, 30.368, 41.467, 47.525, 51.611, 56.539}},
            {10, {31.71, 21.921, 5.019, 27.327, 11.403, 16.029, 6.604, 9.733, 8.169, 13.309}},
            {10, {21.096, 66.413, 28.715, 49.337, 44.433, 35.993, 12.339, 40.301, 54.257, 60.013}},
            {10, {37.051, 45.853, 62.817, 53.717, 57.815, 17.296, 67.611, 72.953, 78.716, 84.214}},
            {9, {31.737, 15.013, 11.528, 36.037, 19.131, 5.439, 9.41, 26.433, 7.437, 9.904}},
            {10, {29.809, 12.764, 49.653, 20.817, 25.333, 43.015, 15.957, 4.613, 9.111, 36.519}},
        };
        return t;
    }
    static const RowB* rowsB()
    {
        static const RowB t[10] = {
            {{6.113, 17.041, 29.231}, {46, -60, 21, -72, 71, -6}},
            {{8.213, 20.043, 53.213}, {56, -60, 18, 70, -70, 16}},
            {{11.057, 17.049, 29.455}, {-47, 60, 28, -76, 100, 35}},
            {{16.011, 30.058, 55.013}, {79, -80, 40, 64, -100, -26}},
            {{20.013, 29.021, 49.014}, {100, -50, 40, 25, -29, -25}},
            {{36.501, 47.132, 62.022}, {100, -50, 40, 25, -29, -25}},
            {{8.213, 20.043, 53.213}, {56, -60, 18, 50, 70, 26}},
            {{8.213, 20.043, 53.213}, {56, -60, 18, -50, -70, -26}},
            {{16.011, 30.058, 55.013}, {79, -80, 40, 80, 90, 25}},
            {{16.011, 30.058, 55.013}, {79, -80, 40, -80, -90, -25}},
        };
        return t;
    }

    // the plug-in's own limits for the settings (its validation routine)
    static void clampSettings(int32_t* n)
    {
        for (int i = 0; i < 3; ++i) n[i] = std::min(0, std::max(-800, n[i]));
        n[3] = std::min(9, std::max(0, n[3]));
        n[4] = std::min(9, std::max(0, n[4]));
        n[5] = std::min(100, std::max(0, n[5]));
        n[6] = std::min(200, std::max(0, n[6]));
        n[7] = std::min(1, std::max(0, n[7]));
        n[8] = std::min(5000, std::max(10, n[8]));
        n[9] = std::min(1, std::max(0, n[9]));
        n[10] = std::min(15000, std::max(100, n[10]));
        n[11] = std::min(1000, std::max(0, n[11]));
    }

    // the plug-in's flush of tiny values: anything below 2^-32 becomes zero
    static double flush(double v)
    {
        uint64_t b;
        std::memcpy(&b, &v, 8);
        if ((uint32_t)((b >> 32) & 0x7ff00000u) < 0x3df00000u) b = 0;
        std::memcpy(&v, &b, 8);
        return v;
    }

    // the design of the rumble filter (0x18000d1c0): a resonant two-pole at frequency f (fraction of the sample rate)
    void rumbleDesign(double f, double q)
    {
        double fc = f;
        if (0.0004 > f) fc = 0.0004;
        else if (f > 0.49) fc = 0.49;
        double r = std::exp(fc * -3.141592653589793 / q);
        double c = std::cos(fc * 6.283185307179586);
        double e = fc * -6.283185307179586;
        double a = r * (c * 2.0);
        e /= q;
        double b = std::exp(e);
        hp_[0] = 0.0;
        hp_[1] = 0.0;
        hp_[2] = a;
        hp_[3] = b;
        hp_[4] = (1.0 - a) + b;
    }

    // the plug-in's "apply settings" routine (0x18000d2a0); rebuild = the long path that also makes new buffers
    void prep(bool rebuild)
    {
        const RowA& ra = rowsA()[s_[3]];
        const RowB& rb = rowsB()[s_[4]];
        const double rateD = (double)rate_;
        double wet = (double)s_[11] / 1000.0;
        wet *= 0.1;
        wet_ = wet;
        dry_ = 1.0 - wet;
        if (s_[2] == -800) outGain_ = 0.0;
        else outGain_ = std::pow(10.0, (double)s_[2] / 200.0) / 100.0;
        if (s_[9] != 0 && (uint32_t)s_[10] < rate_) {
            double a = std::exp((double)s_[10] * -12.56637061436 / rateD);
            lpA_ = a;
            lpB_ = 1.0 - a;
        } else {
            lpA_ = 0.0;
            lpB_ = 1.0;
        }
        const double msToSamples = rateD / 1000.0;
        const double reverbTime = (double)s_[5] / 20.0;
        if (rebuild) {
            count_ = ra.count;
            for (int i = 0; i < 10; ++i) comb_[i] = Comb();
            for (int i = 0; i < 2; ++i) right_[i] = Comb();
        }
        for (int i = 0; i < count_; ++i) combDesign(comb_[i], ra.d[i], reverbTime, rebuild);
        for (int i = 0; i < 2; ++i) combDesign(right_[i], ra.d[i] * (i == 0 ? 0.93 : 1.06), reverbTime, rebuild);
        if (rebuild) {
            int pre = (int)((double)s_[6] * msToSamples);
            if (pre < 1) pre = 1;
            preLen_ = (size_t)pre;
            for (int c = 0; c < 2; ++c) pre_[c].assign(preLen_ + 1, 0.0);
            prePos_ = 0;
            int t0 = (int)((rb.d[0] + 0.051) * msToSamples);
            int t1 = (int)((rb.d[1] + 0.151) * msToSamples);
            int t2 = (int)((rb.d[2] + 0.078) * msToSamples);
            double g[3] = {(double)rb.g[0], (double)rb.g[1], (double)rb.g[2]};
            for (int k = 0; k < 3; ++k) {
                double x = ((double)rb.g[3 + k] + 100.0) / 200.0;
                gL_[k] = (1.0 - x) * g[k];
                gR_[k] = g[k] * x;
            }
            // the plug-in puts the three times in order with two compare-and-swap steps (not a full sort)
            if (t0 > t1) {
                std::swap(t0, t1);
                std::swap(gL_[0], gL_[1]);
                std::swap(gR_[0], gR_[1]);
            }
            if (t1 > t2) {
                std::swap(t1, t2);
                std::swap(gL_[1], gL_[2]);
                std::swap(gR_[1], gR_[2]);
            }
            tapLen_ = (size_t)std::max(t2, 1);
            for (int c = 0; c < 2; ++c) tap_[c].assign(tapLen_ + 1, 0.0);
            tapW_ = 0;
            tap0_ = tapLen_ - (size_t)std::max(0, std::min(t0, (int)tapLen_));
            tap1_ = tapLen_ - (size_t)std::max(0, std::min(t1, (int)tapLen_));
            inGain_ = 1.0;
            lpState_ = 0.0;
        }
        if (s_[7] != 0) {
            double f = (double)s_[8] / rateD;
            if (0.011337868480725623 > f) f /= 2.2;
            else f /= 3.3;
            rumbleDesign(f, 0.1);
        }
    }

    // gain and length of one comb filter from its delay in ms and the reverb time
    void combDesign(Comb& c, double ms, double reverbTime, bool rebuild)
    {
        double d = ms / 1000.0;
        c.gain = std::pow(10.0, d * -3.0 / reverbTime);
        double len = (double)rate_ * d;
        len += 0.5;
        int n = (int)len;
        if (rebuild) {
            c.len = (size_t)std::max(n, 1);
            c.buf.assign(c.len + 1, 0.0);
            c.pos = 0;
            c.lp = 0.0;
        }
    }

    // one pass of a comb filter: returns its stored value, writes the new one
    static double combStep(Comb& c, double in, double lpA, double lpB)
    {
        double lp = c.lp * lpA;
        double v = c.buf[c.pos];
        double sum = v;
        v *= lpB;
        v += lp;
        c.lp = v;
        if (1e-20 > std::fabs(v)) c.lp = 0.0;
        double w = in + c.lp;
        w *= c.gain;
        c.buf[c.pos] = w;
        if (++c.pos >= c.len) c.pos = 0;
        return sum;
    }

    // the plug-in's per-frame routine for two channels (0x18000f410)
    void frame(double& L, double& R)
    {
        double a = lpState_ * lpA_;
        double xR = inGain_ * R;
        double xL = inGain_ * L;
        double m = xR + xL;
        m *= 0.5;
        m *= lpB_;
        m += a;
        double s = flush(m);
        lpState_ = s;
        if (s_[7] != 0) {
            double y1 = hp_[0];
            double t0 = s * hp_[4];
            double t1 = hp_[3] * hp_[1];
            hp_[1] = y1;
            double y = y1 * hp_[2];
            y += t0;
            y -= t1;
            y = flush(y);
            hp_[0] = y;
            double yf = (double)(float)y;
            s += 0.0 - yf;
        }
        double sumL = 0.0;
        for (int i = 2; i < count_; ++i) sumL += combStep(comb_[i], s, lpA_, lpB_);
        double sumR = sumL;
        for (int i = 0; i < 2; ++i) {
            sumL += combStep(comb_[i], s, lpA_, lpB_);
            sumR += combStep(right_[i], s, lpA_, lpB_);
        }
        pre_[0][prePos_] = sumL;
        pre_[1][prePos_] = sumR;
        if (++prePos_ >= preLen_) prePos_ = 0;
        double e1 = tap_[0][tap1_] + tap_[1][tap1_];
        double e0 = tap_[0][tap0_] + tap_[1][tap0_];
        double e2 = tap_[0][tapW_] + tap_[1][tapW_];
        double outL = e1 * gL_[1];
        outL += e0 * gL_[0];
        outL += e2 * gL_[2];
        double wetL = pre_[0][prePos_] * wet_;
        outL *= outGain_;
        outL += wetL;
        outL += xL * dry_;
        double outR = e1 * gR_[1];
        outR += e0 * gR_[0];
        outR += e2 * gR_[2];
        double wetR = pre_[1][prePos_] * wet_;
        outR *= outGain_;
        outR += wetR;
        outR += xR * dry_;
        L = outL;
        R = outR;
        tap_[0][tapW_] = xL;
        tap_[1][tapW_] = xR;
        if (++tap0_ >= tapLen_) tap0_ = 0;
        if (++tap1_ >= tapLen_) tap1_ = 0;
        if (++tapW_ >= tapLen_) tapW_ = 0;
    }
};

inline const Info& xpReverbInfo()
{
    static const Info info = [] {
        Info i;
        i.uid = "reno.xpreverb";
        i.name = "ExpressFX Reverb";
        i.clsid = "00000006-0f56-11d2-9887-00a0c969725b";
        const uint8_t g[16] = {0x30, 0xfc, 0x97, 0xba, 0x62, 0x0f, 0xd2, 0x11, 0x98, 0x87, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x5b};
        std::copy(g, g + 16, i.persistGuid);
        i.nInts = 12;
        i.bypassSlot = -1;
        i.params = {
            {"Room type", 0, 9, 0, "", 3, 1, Choice,
             "Rich hall,Wide open hall,Deep hall,Warm space,Cavernous space,Bright small room,Medium room,Live ambience,Plate,Metal tank", 0, false},
            {"Room size", 0, 100, 15, "%", 6, 2, Num, "", 1, false},
            {"Liveliness", 0, 100, 25, "%", 5, 1, Num, "", 0, false},
            {"Mix", 0, 100, 50, "%", 11, 10, Num, "", 1, false},
        };
        i.presets = {
            {"(Default)", {-800, -240, -160, 0, 2, 25, 30, 1, 20, 1, 12000, 500}},
            {"Default all parameters", {-800, -240, -160, 0, 2, 25, 30, 1, 20, 1, 12000, 500}},
            {"Dream-verb", {-800, -240, -160, 8, 2, 100, 0, 1, 20, 1, 12000, 1000}},
            {"Indoor olympic-size pool", {-800, -240, -160, 4, 2, 100, 160, 1, 20, 1, 12000, 750}},
            {"Hospital hallway", {-800, -240, -160, 5, 2, 65, 70, 1, 20, 1, 12000, 400}},
            {"Small metal room", {-800, -240, -160, 9, 2, 25, 30, 1, 20, 1, 12000, 1000}},
            {"Small night club", {-800, -240, -160, 7, 2, 15, 60, 1, 20, 1, 12000, 500}},
        };
        i.make = [] { return std::unique_ptr<Engine>(new XpReverbEngine()); };
        return i;
    }();
    return info;
}

}  // namespace vegpack1
}  // namespace reno
