// VEGAS's Amplitude Modulation DirectX effect (sfppack3_x64.dll, CLSID fdb0d300-6f82-11d0-aebc-00a0c9053912): its saved
// settings, presets and the sound itself, translated from the plug-in's own code (CSfAmpmod's engine). Reno's built-in
// Amplitude Modulation uses the same settings and saved form, so a project's VEGAS Amplitude Modulation loads into it and
// it saves back as VEGAS Amplitude Modulation.
//
// The effect multiplies the sound by a gain that follows a graph (up to 32 points: time along one cycle of the modulation,
// the height from 0 (no change) down to the "amplitude" depth in dB at the bottom), mixed in as the "wet" signal with an
// optional low-pass filter on it, plus the unprocessed ("dry") signal. The cycle length is a frequency or, with tempo
// sync, a note length. "Stereo pan" shifts the right channel's place in the cycle.
#pragma once
#include "vegaspack3.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace reno {
namespace vegamp {

const char* const kPersistGuid = "8f2a0360-d1fb-4d46-ae1f-890a154419c4";
const int kMaxPoints = 32;
const int kGraphEnd = 1000;             // time runs 0 .. 1000 (one cycle)
const int kGraphLow = -100;             // height -100 (the full depth) .. 0 (no change)

// The plug-in's own layout (what it saves, after its length and a GUID): 0x134 bytes.
struct Settings {
    int32_t lowPassFreq = 400;          // Hz
    int32_t lowPassOn = 0;
    int32_t pan = 0;                    // stereo pan 0 .. 100
    int32_t blendEdges = 0;
    int32_t amplitude = -480;           // depth in tenths of a dB, -800 .. 0
    int32_t frequency = 1000;           // modulation frequency, 0.001 .. 5000 Hz, times 1000
    int32_t wetOut = 0;                 // tenths of a dB, -800 (-Inf.) .. 240
    int32_t dryOut = -800;              // tenths of a dB, -800 (-Inf.) .. 0
    int32_t count = 2;                  // points in use
    int32_t pts[kMaxPoints * 2] = {0, -50, kGraphEnd, -50};
    int32_t bypass = 0;
    int32_t period = 18;                // tempo sync: note length, 0 (64th note triplet) .. 17 (dotted half note), 18 (measures)
    int32_t multiplier = 1000;          // times 1000: 0.001 .. 32
    int32_t tempoSync = 0;
};
static_assert(sizeof(Settings) == 0x134, "VEGAS Amplitude Modulation's settings are 0x134 bytes");

inline void sanitize(Settings& s)
{
    s.lowPassFreq = std::max(100, std::min(10000, (int)s.lowPassFreq));
    s.lowPassOn = s.lowPassOn ? 1 : 0;
    s.pan = std::max(0, std::min(100, (int)s.pan));
    s.blendEdges = s.blendEdges ? 1 : 0;
    s.amplitude = std::max(-800, std::min(0, (int)s.amplitude));
    s.frequency = std::max(1, std::min(5000000, (int)s.frequency));
    s.wetOut = std::max(-800, std::min(240, (int)s.wetOut));
    s.dryOut = std::max(-800, std::min(0, (int)s.dryOut));
    s.count = std::max(2, std::min(kMaxPoints, (int)s.count));
    for (int k = 0; k < s.count; ++k) {
        s.pts[2 * k] = std::max(0, std::min(kGraphEnd, (int)s.pts[2 * k]));
        s.pts[2 * k + 1] = std::max(kGraphLow, std::min(0, (int)s.pts[2 * k + 1]));
    }
    s.bypass = s.bypass ? 1 : 0;
    s.period = std::max(0, std::min(18, (int)s.period));
    s.multiplier = std::max(1, std::min(32000, (int)s.multiplier));
    s.tempoSync = s.tempoSync ? 1 : 0;
}
inline std::string encode(const Settings& s) { return vegpack3::encode(kPersistGuid, &s, sizeof s); }
inline bool decode(const std::string& st, Settings* out)
{
    Settings s;
    if (!vegpack3::decode(kPersistGuid, st, &s, sizeof s)) return false;
    sanitize(s);
    *out = s;
    return true;
}

struct Preset {
    const char* name;
    Settings s;
};
inline Settings make(int pan, int blend, int frequency, std::initializer_list<int> pts, int period = 18, int multiplier = 1000)
{
    Settings s;
    s.pan = pan;
    s.blendEdges = blend;
    s.frequency = frequency;
    std::memset(s.pts, 0, sizeof s.pts);
    int n = 0;
    for (int v : pts) s.pts[n++] = v;
    s.count = n / 2;
    s.period = period;
    s.multiplier = multiplier;
    return s;
}
// VEGAS's own presets, in its order
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = {
        {"Default all parameters", make(0, 0, 1000, {0, -50, 1000, -50})},
        {"Triangle 1", make(0, 0, 1000, {0, -50, 250, 0, 750, -100, 1000, -50})},
        {"Sine", make(0, 0, 1000, {0, -50, 125, -12, 188, -3, 250, 0, 313, -3, 375, -12, 625, -88, 688, -97, 750, -100, 813, -97, 875, -88, 1000, -50})},
        {"Sine out of phase", make(100, 1, 5000, {0, -50, 125, -12, 188, -3, 250, 0, 313, -3, 375, -12, 625, -88, 688, -97, 750, -100, 813, -97, 875, -88, 1000, -50})},
        {"Soft square", make(0, 0, 5000, {0, -50, 50, 0, 450, 0, 550, -100, 950, -100, 1000, -50})},
        {"Hard square", make(0, 0, 5000, {0, 0, 500, 0, 501, -100, 1000, -100})},
        {"Wacky", make(25, 0, 6500, {0, -50, 90, -25, 125, -20, 160, -25, 340, -70, 375, -75, 410, -70, 590, -10, 625, -5, 660, -10, 840, -90, 875, -95, 910, -90, 1000, -50})},
        {"Sawtooth 1", make(0, 0, 1000, {0, -50, 1000, 0}, 0, 0)},
        {"Sawtooth 2", make(0, 0, 1000, {0, -100, 1000, 0}, 0, 0)},
    };
    return list;
}

using Tempo = vegpack3::Tempo;

// ---------------------------------------------------------------------------------------------------------------
// The sound. The plug-in's engine keeps a table of 1001 gains (the last slot also holds the step of the phase), the phase of
// the left (or only) channel and of the right one for a pan, the low-pass filter's two factors and its state per channel.
class Engine {
public:
    // The settings the plug-in is going to use from its next setup on.
    void setSettings(const Settings& s)
    {
        if (s.pan != cur_.pan) panDirty_ = 1;
        cur_ = s;
        changed_ = 1;
    }
    // The format: sample rate (the sound is stereo float). Sets the engine up from the settings with no tempo and starts afresh.
    void setFormat(int rate)
    {
        rate_ = (unsigned)rate;
        setup(cur_, nullptr, false);
        changed_ = 0;
        needTempo_ = true;
    }
    // Where the next sample is in the project (in samples); with tempo sync the phase follows it.
    void setPosition(double samples)
    {
        position_ = samples;
        havePosition_ = 1;
        needTempo_ = true;
    }

    // One call of the plug-in's own process function on 'frames' interleaved stereo frames, in place. 'tempo' is what the
    // project says (used with tempo sync). end: the end of the stream (nothing is processed). Returns 1 at the end, else 0.
    int process(float* data, int* frames, const Tempo* tempo, bool end = false)
    {
        if (end) {
            *frames = 0;
            return 1;
        }
        if (changed_ || needTempo_) {
            setup(cur_, tempo, true);
            changed_ = 0;
            needTempo_ = false;
        }
        run(data, *frames);
        return 0;
    }

private:
    double tbl_[kGraphEnd + 1] = {};    // gains; [1000] is also the step of the phase (the plug-in keeps both in the same place)
    double phase_ = 0, phase2_ = 0;
    double lowA_ = 0, lowB_ = 1;
    double lowState_[2] = {0, 0};
    double dry_ = 0, wet_ = 0;
    double position_ = 0;
    int havePosition_ = 0, panActive_ = 0, panDirty_ = 0, bypass_ = 0;
    int changed_ = 0, needTempo_ = 0;
    unsigned rate_ = 48000;
    Settings cur_;

    static double& step(double* t) { return t[kGraphEnd]; }
    // the plug-in's imul by 1000 (32 bits, wrapping)
    static int times1000(int x) { return (int)((uint32_t)x * 1000u); }

    // The plug-in's setup. keep: carry on (the filters and the phase stay), else start afresh.
    void setup(const Settings& s, const Tempo* tempo, bool keep)
    {
        wet_ = s.wetOut > -800 ? std::pow(10.0, (double)s.wetOut / 200.0) : 0.0;
        dry_ = s.dryOut > -800 ? std::pow(10.0, (double)s.dryOut / 200.0) : 0.0;
        bypass_ = s.bypass;
        const double depth = ((double)s.amplitude * -1.0) / 10.0;
        vegpack3::fillGraph(tbl_, s.pts, s.count, s.blendEdges != 0, kGraphLow, 0);
        for (int k = 0; k <= kGraphEnd; ++k) tbl_[k] = std::pow(10.0, ((tbl_[k] / 100.0) * depth) / 20.0);
        // the step of the phase per sample: 1000 / the samples in a cycle
        if (s.tempoSync != 0 && tempo != nullptr) {
            const double len = vegpack3::syncCycle(*tempo, s.period, s.multiplier, rate_);
            step(tbl_) = 1000.0 / len;
        } else {
            const double hz = (double)s.frequency / 1000.0;
            const int period = vegpack3::cvtt((double)rate_ / hz);
            step(tbl_) = 1000.0 / (double)period;
        }
        if (!keep) {
            phase_ = 0;
            for (int c = 0; c < 2; ++c) lowState_[c] = 0;
        } else if (s.tempoSync != 0 && tempo != nullptr && havePosition_ != 0) {
            havePosition_ = 0;
            double ph = position_ * step(tbl_);
            phase_ = ph;
            if (ph >= 1000.0) phase_ = ph - (double)times1000(vegpack3::cvtt(ph / 1000.0));
        }
        if (!keep || panDirty_ != 0) {
            if (s.pan != 0) {
                panActive_ = 1;
                phase2_ = (double)s.pan / 100.0 * 0.5 * 1000.0 + phase_;
            } else {
                panActive_ = 0;
            }
            panDirty_ = 0;
        }
        // the low-pass filter on the wet signal
        if (s.lowPassOn != 0 && s.lowPassFreq <= (int)rate_ / 2) {
            static const uint64_t kTwoPi = 0xc01921fb54442eeaull;      // -6.28318530718
            double c;
            std::memcpy(&c, &kTwoPi, 8);
            lowA_ = std::exp((double)s.lowPassFreq * c / (double)rate_);
            lowB_ = 1.0 - lowA_;
        } else {
            lowA_ = 0.0;
            lowB_ = 1.0;
        }
    }

    float one(float x, double phase, int ch)
    {
        const double lp = lowA_ * lowState_[ch];
        const double xd = (double)x;
        const double dryPart = xd * dry_;
        double y = ((xd * tbl_[(uint32_t)(int64_t)phase]) * wet_) * lowB_;
        y = y + lp;
        if (2e-11 > std::fabs(y)) y = 0.0;
        lowState_[ch] = y;
        return (float)(y + dryPart);
    }
    static void wrap(double& ph)
    {
        if (ph >= 1000.0) ph = ph - (double)times1000(vegpack3::cvtt(ph / 1000.0));
    }

    void run(float* data, int frames)
    {
        if (bypass_ != 0) return;
        if (panActive_ == 0) {
            for (int k = 0; k < frames; ++k) {
                wrap(phase_);
                data[2 * k] = one(data[2 * k], phase_, 0);
                data[2 * k + 1] = one(data[2 * k + 1], phase_, 1);
                phase_ = phase_ + step(tbl_);
            }
        } else {
            for (int k = 0; k < frames; ++k) {
                wrap(phase_);
                wrap(phase2_);
                data[2 * k] = one(data[2 * k], phase_, 0);
                data[2 * k + 1] = one(data[2 * k + 1], phase2_, 1);
                const double st = step(tbl_);
                const double n2 = st + phase2_;
                const double n1 = st + phase_;
                phase2_ = n2;
                phase_ = n1;
            }
        }
    }
};

}  // namespace vegamp
}  // namespace reno
