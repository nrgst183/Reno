// VEGAS's Reverb (sfppack1_x64.dll, CSfReverb, DirectX class 607682e0-6e21-11d0-aebc-00a0c9053912): its settings, presets
// and sound. The engine is the plug-in's own code (its "apply settings" routine and its per-sample stereo routine)
// translated line by line, with the room and early reflection tables read out of the DLL.
//
// The sound: the mono sum of the input goes through a one-pole low pass (Attenuate high frequencies) and a two-pole
// high pass (Attenuate bass frequencies), then into a bank of damped feedback delay lines (the room's times, each one's
// gain set so that it falls by 60 dB in the decay time). The left output takes the sum of the bank's outputs plus two
// special lines of its own, the right output plus two others (0.93 and 1.06 times as long); the result passes a
// pre-delay line. In parallel three early reflections (taps of the input, with the style's levels and pans) are added.
#pragma once
#include "vegaspack1.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#include <cstdint>
#if defined(_M_X64) || defined(__SSE2__)
#include <xmmintrin.h>
#endif

namespace reno {
namespace vegpack1 {

// the room table: per Reverberation Mode a count and ten delay times in ms (from VEGAS's own table)
static const double kRooms[19][11] = {
    {10.0, 23.583, 47.049, 25.481, 27.493, 29.67, 34.518, 32.056, 37.317, 40.358, 19.941},
    {8.0, 23.161, 18.481, 20.611, 16.035, 26.45, 29.281, 12.957, 33.46, 8.812, 9.9256},
    {10.0, 53.703, 67.624, 41.051, 72.937, 84.262, 49.613, 45.85, 57.837, 62.5, 78.724},
    {10.0, 13.453, 24.966, 28.415, 12.028, 15.817, 17.836, 10.72, 19.719, 9.439, 22.011},
    {10.0, 54.208, 28.707, 21.029, 49.33, 44.418, 35.918, 12.332, 66.457, 40.313, 60.051},
    {10.0, 70.539, 46.457, 52.802, 39.727, 76.939, 58.518, 64.33, 31.62, 90.769, 83.537},
    {10.0, 46.463, 89.887, 53.024, 58.824, 83.067, 70.527, 3901.707, 76.433, 64.719, 31.627},
    {9.0, 25.037, 34.831, 29.502, 37.527, 32.139, 22.731, 40.532, 43.812, 47.07, 9.958},
    {10.0, 32.963, 35.329, 44.227, 38.224, 62.813, 30.368, 41.467, 47.525, 51.611, 56.539},
    {10.0, 13.407, 24.354, 11.88, 10.291, 4.527, 15.213, 20.313, 8.519, 17.221, 6.513},
    {10.0, 31.71, 21.921, 5.019, 27.327, 11.403, 16.029, 6.604, 9.733, 8.169, 13.309},
    {10.0, 13.319, 20.32, 12.034, 10.609, 4.697, 14.75, 17.036, 9.106, 6.564, 7.949},
    {10.0, 21.096, 66.413, 28.715, 49.337, 44.433, 35.993, 12.339, 40.301, 54.257, 60.013},
    {10.0, 41.005, 45.815, 49.637, 53.795, 57.812, 62.504, 67.637, 72.921, 78.721, 84.212},
    {10.0, 37.051, 45.853, 62.817, 53.717, 57.815, 17.296, 67.611, 72.953, 78.716, 84.214},
    {9.0, 50.139, 62.811, 53.916, 25.834, 17.235, 67.81, 73.626, 93.927, 84.219, 84.295},
    {9.0, 31.737, 15.013, 11.528, 36.037, 19.131, 5.439, 9.41, 26.433, 7.437, 9.904},
    {10.0, 3.101, 18.37, 12.86, 11.063, 5.027, 15.513, 6.544, 9.539, 21.734, 7.924},
    {10.0, 29.809, 12.764, 49.653, 20.817, 25.333, 43.015, 15.957, 4.613, 9.111, 36.519},
};
// the early reflection styles: three delays in ms, three levels in % and three pans
struct EarlyStyle { double ms[3]; int32_t level[3]; int32_t pan[3]; };
static const EarlyStyle kStyles[10] = {
    {{6.113, 17.041, 29.231}, {46, -60, 21}, {-72, 71, -6}},
    {{8.213, 20.043, 53.213}, {56, -60, 18}, {70, -70, 16}},
    {{11.057, 17.049, 29.455}, {-47, 60, 28}, {-76, 100, 35}},
    {{16.011, 30.058, 55.013}, {79, -80, 40}, {64, -100, -26}},
    {{20.013, 29.021, 49.014}, {100, -50, 40}, {25, -29, -25}},
    {{36.501, 47.132, 62.022}, {100, -50, 40}, {25, -29, -25}},
    {{8.213, 20.043, 53.213}, {56, -60, 18}, {50, 70, 26}},
    {{8.213, 20.043, 53.213}, {56, -60, 18}, {-50, -70, -26}},
    {{16.011, 30.058, 55.013}, {79, -80, 40}, {80, 90, 25}},
    {{16.011, 30.058, 55.013}, {79, -80, 40}, {-80, -90, -25}},
};

// The settings structure (12 ints): 0 dry out, 1 reverb out and 2 early out in tenths of a dB (-800: none), 3 reverberation
// mode (0 to 18), 4 early reflection style (0 to 9), 5 decay time in tenths of a second, 6 pre-delay in ms, 7 attenuate
// bass on, 8 its frequency in Hz, 9 attenuate high frequencies on, 10 its frequency in Hz, 11 bypass.
class ReverbEngine : public Engine {
public:
    void setup(const int32_t* p, uint32_t rate, bool keep) override
    {
        if (rate < 1000) rate = 1000;
        std::memcpy(s_, p, sizeof s_);
        const bool full = !keep || !built_ || rate != rate_;
        rate_ = rate;
        apply(full);
        built_ = true;
    }

    void process(float* left, float* right, int frames) override
    {
        if (!built_) return;
#if defined(_M_X64) || defined(__SSE2__)
        // the plug-in runs with denormals flushed to zero
        const unsigned oldCsr = _mm_getcsr();
        _mm_setcsr(oldCsr | 0x8040u);
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
        // (ARM: FPCR's FZ bit flushes denormal inputs and results to zero, as MXCSR's FTZ and DAZ do)
        uint64_t oldFpcr;
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(oldFpcr));
        __asm__ __volatile__("msr fpcr, %0" : : "r"(oldFpcr | (1ull << 24)));
#endif
        for (int k = 0; k < frames; ++k) {
            double l = (double)left[k], r = (double)right[k];
            sample(&l, &r);
            left[k] = (float)l;
            right[k] = (float)r;
        }
#if defined(_M_X64) || defined(__SSE2__)
        _mm_setcsr(oldCsr);
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
        __asm__ __volatile__("msr fpcr, %0" : : "r"(oldFpcr));
#endif
    }

private:
    // one feedback delay line of the bank
    struct Line {
        std::vector<double> buf;
        size_t cur = 0;
        double gain = 0, state = 0;
        double read() const { return buf.empty() ? 0.0 : buf[cur]; }
        void write(double v)
        {
            if (buf.empty()) return;
            buf[cur] = v;
            if (++cur >= buf.size()) cur = 0;
        }
    };
    // a ring of samples read at several places
    struct Ring {
        std::vector<double> buf;
        size_t w = 0;
        size_t tap[2] = {0, 0};
        double at(size_t i) const { return i < buf.size() ? buf[i] : 0.0; }
        void step()
        {
            const size_t n = buf.size();
            if (++w >= n) w = 0;
            for (size_t& t : tap)
                if (++t >= n) t = 0;
        }
    };

    int32_t s_[12] = {0};
    uint32_t rate_ = 48000;
    bool built_ = false;
    int count_ = 0;                          // lines in the bank
    std::vector<Line> bank_;                 // the left channel's bank (all lines)
    Line right_[2];                          // the right channel's two special lines
    std::vector<double> late_[2];            // the pre-delay lines
    size_t lateW_[2] = {0, 0};
    Ring early_[2];                          // early reflection rings (left and right input)
    double earlyGain_[3][2] = {{0, 0}, {0, 0}, {0, 0}};
    double dry_ = 0, rev_ = 0, earlyOut_ = 0;
    double lpA_ = 1.0, lpB_ = 0.0, lpState_ = 0.0;
    bool bassOn_ = false, hiOn_ = false;
    double hp_[5] = {0, 0, 0, 0, 0};         // two states and three coefficients of the bass filter

    static double gainOf(int32_t tenthsDb) { return std::pow(10.0, (double)tenthsDb / 200.0); }

    // the two-pole filter behind "Attenuate bass frequencies"
    void bassFilter(double f, bool reset)
    {
        double ff = f;
        if (0.0004 > ff) ff = 0.0004;
        else if (ff > 0.49) ff = 0.49;
        double r = std::exp(ff * -3.141592653589793 / 0.1);
        double c = std::cos(ff * 6.283185307179586);
        double e = ff * -6.283185307179586;
        c *= 2.0;
        e /= 0.1;
        const double c0 = r * c;
        hp_[2] = c0;
        const double r2 = std::exp(e);
        hp_[3] = r2;
        hp_[4] = (1.0 - c0) + r2;
        if (reset) hp_[0] = hp_[1] = 0.0;
    }

    void lowPass()
    {
        if (hiOn_ && s_[10] >= 0 && (uint32_t)s_[10] < rate_) {
            const double b = std::exp((double)s_[10] * -12.56637061436 / (double)rate_);
            lpB_ = b;
            lpA_ = 1.0 - b;
        } else {
            lpB_ = 0.0;
            lpA_ = 1.0;
        }
    }

    void apply(bool full)
    {
        const int mode = std::max(0, std::min(18, (int)s_[3]));
        const int style = std::max(0, std::min(9, (int)s_[4]));
        const double* room = kRooms[mode];
        const double decay = (double)s_[5] / 10.0;
        const bool resetFilter = full;       // the plug-in resets its filters on a fresh start
        bool bassReset = resetFilter;
        if (s_[7] != 0 && !bassOn_) bassReset = true;
        bassOn_ = s_[7] != 0;
        hiOn_ = s_[9] != 0;
        dry_ = s_[0] > -800 ? gainOf(s_[0]) : 0.0;
        rev_ = s_[1] > -800 ? gainOf(s_[1]) : 0.0;
        earlyOut_ = s_[2] == -800 ? 0.0 : gainOf(s_[2]) / 100.0;
        const double rate = (double)rate_;
        const double perMs = rate / 1000.0;
        const double mult[2] = {0.93, 1.06};

        if (full) {
            count_ = (int)room[0];
            bank_.assign((size_t)count_, Line());
            for (int k = 0; k < count_; ++k) {
                const double t = room[1 + k] / 1000.0;
                Line& ln = bank_[(size_t)k];
                ln.gain = std::pow(10.0, t * -3.0 / decay);
                const double len = (double)(int)(rate * t + 0.5);
                ln.buf.assign((size_t)std::max(0.0, len), 0.0);
                ln.state = 0.0;
            }
            for (int k = 0; k < 2; ++k) {
                const double t = room[1 + k] * mult[k] / 1000.0;
                Line& ln = right_[k];
                ln = Line();
                ln.gain = std::pow(10.0, t * -3.0 / decay);
                const double len = (double)(int)(rate * t + 0.5);
                ln.buf.assign((size_t)std::max(0.0, len), 0.0);
            }
            // the pre-delay
            int pre = (int)((double)s_[6] * perMs);
            if (pre < 1) pre = 1;
            for (int c = 0; c < 2; ++c) {
                late_[c].assign((size_t)pre, 0.0);
                lateW_[c] = 0;
            }
            // the early reflections: three taps of the input
            const EarlyStyle& es = kStyles[style];
            int pos[3] = {(int)((es.ms[0] + 0.051) * perMs), (int)((es.ms[1] + 0.151) * perMs), (int)((es.ms[2] + 0.078) * perMs)};
            double g[3][2];
            for (int t = 0; t < 3; ++t) {
                const double gd = (double)es.level[t];
                double f = (double)es.pan[t] + 100.0;
                f /= 200.0;
                double l = 1.0 - f;
                l *= gd;
                const double r = gd * f;
                g[t][0] = l;
                g[t][1] = r;
            }
            if (pos[0] > pos[1]) {
                std::swap(pos[0], pos[1]);
                std::swap(g[0][0], g[1][0]);
                std::swap(g[0][1], g[1][1]);
            }
            if (pos[1] > pos[2]) {
                std::swap(pos[1], pos[2]);
                std::swap(g[1][0], g[2][0]);
                std::swap(g[1][1], g[2][1]);
            }
            for (int t = 0; t < 3; ++t) {
                earlyGain_[t][0] = g[t][0];
                earlyGain_[t][1] = g[t][1];
            }
            const int total = std::max(1, pos[2]);
            for (int c = 0; c < 2; ++c) {
                early_[c].buf.assign((size_t)total, 0.0);
                early_[c].w = 0;
                early_[c].tap[0] = (size_t)std::max(0, std::min(total, total - pos[0]));
                early_[c].tap[1] = (size_t)std::max(0, std::min(total, total - pos[1]));
            }
        } else {
            for (int k = 0; k < count_; ++k) {
                const double t = room[1 + k] / 1000.0;
                bank_[(size_t)k].gain = std::pow(10.0, t * -3.0 / decay);
            }
            for (int k = 0; k < 2; ++k) {
                const double t = room[1 + k] * mult[k] / 1000.0;
                right_[k].gain = std::pow(10.0, t * -3.0 / decay);
            }
        }
        lowPass();
        if (bassOn_) {
            double f = (double)s_[8] / rate;
            if (0.011337868480725623 > f) f /= 2.2;
            else f /= 3.3;
            bassFilter(f, bassReset);
        }
    }

    // one sample of both channels (the plug-in's stereo routine)
    void sample(double* pl, double* pr)
    {
        const double g = 1.0;
        const double l0 = *pl * g, r0 = *pr * g;
        double x = (r0 + l0) * 0.5;
        x *= lpA_;
        x += lpB_ * lpState_;
        lpState_ = x;
        if (bassOn_) {
            const double t = hp_[4] * x;
            const double u = hp_[2] * hp_[0] - hp_[3] * hp_[1];
            const double lp = t + u;
            x = x - lp;
            hp_[1] = hp_[0];
            hp_[0] = lp;
        }
        const double a = lpA_, b = lpB_;
        double sum = 0.0;
        for (int k = 2; k < count_; ++k) {
            Line& ln = bank_[(size_t)k];
            const double d = ln.read();
            sum += d;
            ln.state = ln.state * b + d * a;
            ln.write((ln.state + x) * ln.gain);
        }
        // the two special lines of each channel
        double outL = sum, outR = sum;
        {
            Line& l1 = bank_[0];
            Line& l2 = bank_[1];
            const double d1 = l1.read(), d2 = l2.read();
            outL += d1;
            outL += d2;
            l1.state = l1.state * b + d1 * a;
            l2.state = l2.state * b + d2 * a;
            l1.write((l1.state + x) * l1.gain);
            l2.write((l2.state + x) * l2.gain);
        }
        {
            Line& l1 = right_[0];
            Line& l2 = right_[1];
            const double d1 = l1.read(), d2 = l2.read();
            outR += d1;
            outR += d2;
            l1.state = l1.state * b + d1 * a;
            l2.state = l2.state * b + d2 * a;
            l1.write((l1.state + x) * l1.gain);
            l2.write((l2.state + x) * l2.gain);
        }
        // the pre-delay: written, stepped, then read (so it reads the oldest sample)
        late_[0][lateW_[0]] = outL;
        late_[1][lateW_[1]] = outR;
        for (int c = 0; c < 2; ++c)
            if (++lateW_[c] >= late_[c].size()) lateW_[c] = 0;
        const double lateL = late_[0][lateW_[0]], lateR = late_[1][lateW_[1]];
        // the early reflections
        const double t0 = early_[1].at(early_[1].tap[0]) + early_[0].at(early_[0].tap[0]);
        const double t1 = early_[1].at(early_[1].tap[1]) + early_[0].at(early_[0].tap[1]);
        const double t2 = early_[0].at(early_[0].w) + early_[1].at(early_[1].w);
        double eL = t1 * earlyGain_[1][0];
        double eR = t1 * earlyGain_[1][1];
        eL += t0 * earlyGain_[0][0];
        eR += t0 * earlyGain_[0][1];
        early_[0].buf[early_[0].w] = l0;
        early_[1].buf[early_[1].w] = r0;
        eL += t2 * earlyGain_[2][0];
        eR += t2 * earlyGain_[2][1];
        eL *= earlyOut_;
        eR *= earlyOut_;
        const double dL = dry_ * l0, dR = dry_ * r0;
        const double sL = dL + eL, sR = dR + eR;
        *pl = lateL * rev_ + sL;
        *pr = lateR * rev_ + sR;
        early_[0].step();
        early_[1].step();
    }
};

inline const Info& reverbInfo()
{
    static const Info info = [] {
        Info i;
        i.uid = "reno.reverb";
        i.name = "Reverb";
        i.clsid = "607682e0-6e21-11d0-aebc-00a0c9053912";
        const uint8_t g[16] = {0x27, 0xf7, 0x50, 0xb4, 0xfb, 0xe8, 0x34, 0x4f, 0xb5, 0x3b, 0xcc, 0x30, 0xa1, 0x8c, 0x17, 0xa0};
        std::copy(g, g + 16, i.persistGuid);
        i.nInts = 12;
        i.bypassSlot = 11;
        i.params = {
            {"Pre-Delay", 0, 200, 30, "ms", 6, 1, Num, "", 0, false},
            {"Decay Time", 0.5, 5, 2.5, "s", 5, 10, Num, "", 1, false},
            {"Reverberation Mode", 0, 18, 0, "", 3, 1, Choice,
             "Rich Hall,Wide Open Hall,Concert Hall,Deep Hall,Long Hall,Medium Hall 1,Medium Hall 2,Warm Space,Cavernous Space,"
             "Bright Corridor,Bright Small Room,Bright Medium Room,Medium Room,Subtle Ambience,Live Ambience,Warm Ambience,Plate 1,"
             "Plate 2 Ringy,Metal Tank", 0, false},
            {"Early Reflection Style", 0, 9, 3, "", 4, 1, Choice,
             "Mode 1 - 6 ms,Mode 2 - 8 ms,Mode 3 - 10 ms,Mode 4 - 16 ms,Mode 5 - 20 ms,Mode 6 - 36 ms,Mode 7 - 8 ms pan right,"
             "Mode 8 - 8 ms pan left,Mode 9 - 16 ms pan right,Mode 10 - 16 ms pan left", 0, false},
            {"Attenuate Bass Frequencies", 0, 1, 1, "", 7, 1, Bool, "", 0, false},
            {"Bass Attenuation Frequency", 10, 5000, 20, "Hz", 8, 1, Num, "", 0, false},
            {"Attenuate High Frequencies", 0, 1, 1, "", 9, 1, Bool, "", 0, false},
            {"High Attenuation Frequency", 100, 15000, 12000, "Hz", 10, 1, Num, "", 0, false},
            {"Dry Out", -80, 0, 0, "dB", 0, 10, Num, "", 1, true},
            {"Reverb Out", -80, 0, -35, "dB", 1, 10, Num, "", 1, true},
            {"Early Out", -80, 0, -20, "dB", 2, 10, Num, "", 1, true},
            {"Bypass", 0, 1, 0, "", 11, 1, Bool, "", 0, false},
        };
        i.presets = {
            {"(Default)", {0, -350, -200, 0, 3, 25, 30, 1, 20, 1, 12000, 0}},
            {"Bright hall", {0, -350, -200, 0, 3, 25, 30, 1, 20, 1, 12000, 0}},
            {"Cathedral", {0, -240, -200, 8, 5, 30, 70, 1, 20, 1, 7000, 0}},
            {"Dense room", {0, -250, -200, 10, 1, 6, 1, 1, 20, 1, 5000, 0}},
            {"Long hall", {0, -350, -300, 4, 2, 25, 60, 1, 20, 1, 5000, 0}},
            {"Medium hall", {0, -300, -300, 2, 1, 12, 5, 1, 20, 1, 5000, 0}},
            {"Medium room", {0, -200, -800, 13, 0, 5, 1, 1, 20, 1, 5000, 0}},
            {"Metal tank", {0, -160, -200, 18, 5, 25, 10, 1, 400, 1, 3000, 0}},
            {"Plate", {0, -350, -200, 16, 3, 20, 15, 1, 20, 0, 5000, 0}},
            {"Sewer", {0, -160, -200, 18, 3, 20, 15, 1, 20, 1, 10000, 0}},
            {"Small hall", {0, -240, -200, 2, 1, 10, 10, 1, 20, 1, 8000, 0}},
            {"Very long hall", {0, -418, -300, 4, 5, 50, 45, 1, 100, 1, 10000, 0}},
        };
        i.make = [] { return std::unique_ptr<Engine>(new ReverbEngine()); };
        return i;
    }();
    return info;
}

}  // namespace vegpack1
}  // namespace reno
