// VEGAS's Vibrato DirectX effect (sfppack3_x64.dll, CLSID d6802ba0-a056-11d0-aebc-00a0c9053912): its saved settings,
// presets and the sound itself, translated from the plug-in's own code (CSfVibrat's engine). Reno's built-in Vibrato
// uses the same settings and saved form, so a project's VEGAS Vibrato loads into it and it saves back as VEGAS Vibrato.
//
// The effect plays the sound back at a changing speed: a graph (up to 32 points, time along one cycle of the
// modulation, height as a percent of the "semitones" range) says by how many semitones the pitch is moved at each moment
// of the cycle, and the sound is read at the matching speed with a straight line between two neighbouring samples.
// So a graph that is not balanced makes the output longer or shorter than the input (the plug-in takes input and
// gives output of its own length, and the host deals with that).
#pragma once
#include "vegaspack3.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <cstring>
#include <vector>

namespace reno {
namespace vegvib {

const char* const kPersistGuid = "a720c8e9-d534-4cf8-a2db-83cfbdca8dc5";      // the plug-ins of this DLL that have no GUID of their own use this one
const int kMaxPoints = 32;
const int kGraphEnd = 1000;             // the graph's time runs 0 .. 1000 (one cycle of the modulation)
const int kGraphTop = 100;              // and its height -100 .. 100 (percent of the semitones)

struct Point {
    int32_t x = 0, y = 0;
};

// The plug-in's own layout (what it saves, after its length and a GUID): 0x118 bytes.
struct Settings {
    int32_t blendEdges = 0;             // "Blend graph edges"
    int32_t semitones = 1200;           // 0.01 .. 24, times 100
    int32_t frequency = 1000;           // modulation frequency, 0.001 .. 1000 Hz, times 1000
    int32_t gain = 0;                   // output gain in tenths of a dB, -800 (-Inf.) .. 240
    int32_t count = 2;                  // points in use
    int32_t pts[kMaxPoints * 2] = {0, 0, kGraphEnd, 0};    // (x, y) pairs
    int32_t bypass = 0;
};
static_assert(sizeof(Settings) == 0x118, "VEGAS Vibrato's settings are 0x118 bytes");

inline void sanitize(Settings& s)
{
    s.blendEdges = s.blendEdges ? 1 : 0;
    s.semitones = std::max(1, std::min(2400, (int)s.semitones));
    s.frequency = std::max(1, std::min(1000000, (int)s.frequency));
    s.gain = std::max(-800, std::min(240, (int)s.gain));
    s.count = std::max(2, std::min(kMaxPoints, (int)s.count));
    for (int k = 0; k < s.count; ++k) {
        s.pts[2 * k] = std::max(0, std::min(kGraphEnd, (int)s.pts[2 * k]));
        s.pts[2 * k + 1] = std::max(-kGraphTop, std::min(kGraphTop, (int)s.pts[2 * k + 1]));
    }
    s.bypass = s.bypass ? 1 : 0;
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
inline Settings make(int semitones, int frequency, std::initializer_list<int> pts, int declaredCount = -1)
{
    Settings s;
    s.semitones = semitones;
    s.frequency = frequency;
    std::memset(s.pts, 0, sizeof s.pts);
    int n = 0;
    for (int v : pts) s.pts[n++] = v;
    s.count = declaredCount >= 0 ? declaredCount : n / 2;
    return s;
}
// VEGAS's own presets, in its order (the Triangle one is stored with a count of 2 for its 3 points, as VEGAS has it)
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = {
        {"Default all parameters", make(1200, 1000, {0, 0, 1000, 0})},
        {"Pitch shift octave up", make(1200, 1000, {0, 100, 1000, 100})},
        {"Pitch shift octave down", make(1200, 1000, {0, -100, 1000, -100})},
        {"Vibrato 1", make(1200, 5000, {0, 0, 250, 100, 750, -100, 1000, 0})},
        {"Vibrato 2", make(1200, 9000, {0, 0, 200, 90, 250, 100, 300, 90, 700, -90, 750, -100, 800, -90, 1000, 0})},
        {"Vibrato 3", make(1200, 9000, {0, 0, 50, 100, 450, 100, 550, -100, 950, -100, 1000, 0})},
        {"Weird 1", make(1200, 9000, {0, 0, 50, 90, 100, 100, 150, 90, 350, -70, 400, -80, 450, -70, 600, 20, 650, 30, 700, 40, 800, 70, 850, 60, 1000, 0})},
        {"Triangle", make(600, 9000, {0, -100, 500, 100, 1000, -100}, 2)},
    };
    return list;
}

// ---------------------------------------------------------------------------------------------------------------
// The sound. The plug-in's engine keeps a table of 1000 playback speeds (plus one slot that also holds the step of the
// modulation's phase), the position it reads from, the two samples around that position for each channel, and a few flags.
class Engine {
public:
    // The plug-in has been given new settings (they are used at the next process call).
    void setSettings(const Settings& s)
    {
        cur_ = s;
        changed_ = 1;
    }
    // The format: sample rate (the sound is stereo float). Sets the engine up from the settings and starts afresh.
    void setFormat(int rate)
    {
        rate_ = (unsigned)rate;
        setup(cur_, 0);
        changed_ = 0;
    }

    // One call of the plug-in's own process function. mode: 0 the sound jumps (discontinuity), 1 it goes on, 2 end of
    // stream (nothing comes out). in: nIn interleaved stereo frames; out has room for *nOut frames. On return *nIn is how
    // many input frames were used and *nOut how many were made. Returns 1 at the end of the stream, else 0.
    int process(int mode, const float* in, int* nIn, float* out, int* nOut)
    {
        int consumed = nIn ? *nIn : 0;
        int produced = 0;
        int result = 0;
        const bool changed = changed_ != 0;
        changed_ = 0;
        if (changed || !ready_ || (mode == 0 && started_ != 0)) setup(cur_, mode);
        started_ = 1;
        if (mode == 2) {
            result = 1;
        } else if (bypass_ == 0) {
            int ni = consumed, no = nOut ? *nOut : 0;
            run(in, &ni, out, &no);
            consumed = ni;
            produced = no;
        } else {
            std::memmove(out, in, (size_t)consumed * 2 * sizeof(float));
            produced = consumed;
        }
        if (nIn) *nIn = consumed;
        if (nOut) *nOut = produced;
        return result;
    }

private:
    // --- (the plug-in's engine fields)
    double tbl_[kGraphEnd + 1] = {};    // speeds; [1000] is also the step of the phase (the plug-in keeps both in the same place)
    double phase_ = 0;                  // where in the modulation's cycle, 0 .. 1000
    double pos_ = 0;                    // where in the input the next output sample is read from
    double s0_[2] = {0, 0}, s1_[2] = {0, 0};    // the samples at the integer position and the next one
    int f0_ = 0, f1_ = 0;               // those are loaded
    double gain_ = 1, semis_ = 0;
    int bypass_ = 0, ready_ = 0, started_ = 0, changed_ = 0;
    unsigned rate_ = 48000;
    Settings cur_;

    static double& step(double* t) { return t[kGraphEnd]; }
    // the plug-in's rounding trick for an integer position: add and subtract a magic number so that the low word of the double
    // holds the value, truncated towards zero (with 0.499999985 where one would use 0.5)
    static int toInt(double x)
    {
        static const uint64_t kC2 = 0x3fdfffffefe4d4d6ull;
        double c2;
        std::memcpy(&c2, &kC2, 8);
        const double magic = 6755399441055744.0;
        volatile double y = 0.0 <= x ? (x - c2) + magic : (c2 + x) + magic;
        const double yy = y;
        uint64_t b;
        std::memcpy(&b, &yy, 8);
        return (int)(uint32_t)b;
    }

    // the graph into the table, with the plug-in's blending of its edges
    void graph(const Settings& s) { vegpack3::fillGraph(tbl_, s.pts, s.count, s.blendEdges != 0, -kGraphTop, kGraphTop); }

    // The plug-in's setup from its settings; flag 0 starts afresh (the state is cleared), else it carries on.
    void setup(const Settings& s, int flag)
    {
        gain_ = s.gain > -800 ? std::pow(10.0, (double)s.gain / 200.0) : 0.0;
        semis_ = (double)s.semitones / 100.0;
        bypass_ = s.bypass;
        graph(s);
        for (int k = 0; k < kGraphEnd; ++k) tbl_[k] = std::pow(2.0, ((tbl_[k] / 100.0) * semis_) / 12.0);
        const double hz = (double)s.frequency / 1000.0;
        const int period = vegpack3::cvtt((double)rate_ / hz);
        step(tbl_) = 1000.0 / (double)period;
        if (flag == 0) {
            phase_ = 0;
            pos_ = 0;
            for (int c = 0; c < 2; ++c) s0_[c] = s1_[c] = 0;
            f0_ = f1_ = 0;
        }
        ready_ = 1;
    }

    // The loop over the input: the output sample at 'pos' is made from the two input samples around it, then 'pos' goes on by
    // the table's speed at the modulation's phase. *nIn: frames given, on return the frames used (the position is brought back
    // by that many); *nOut: room, on return the frames made.
    void run(const float* in, int* nInP, float* out, int* nOutP)
    {
        const int nIn = *nInP, cap = *nOutP;
        int outCount = 0;
        int idx = toInt(pos_);
        for (;;) {
            const int start = idx;
            if (idx < nIn && f0_ == 0) {
                for (int c = 0; c < 2; ++c) s0_[c] = (double)in[(size_t)idx * 2 + (size_t)c];
                f0_ = 1;
            }
            ++idx;
            if (idx < nIn && f1_ == 0) {
                for (int c = 0; c < 2; ++c) s1_[c] = (double)in[(size_t)idx * 2 + (size_t)c];
                f1_ = 1;
            }
            if (f0_ != 0 && f1_ != 0) {
                for (;;) {
                    if ((unsigned)outCount >= (unsigned)cap) goto done;
                    const double frac = pos_ - (double)toInt(pos_);
                    for (int c = 0; c < 2; ++c) {
                        const double v = ((s1_[c] - s0_[c]) * frac + s0_[c]) * gain_;
                        out[(size_t)outCount * 2 + (size_t)c] = (float)v;
                    }
                    ++outCount;
                    phase_ = step(tbl_) + phase_;
                    if (phase_ > 1000.0) phase_ = phase_ - 1000.0;
                    pos_ = tbl_[(uint32_t)(int64_t)phase_] + pos_;
                    idx = toInt(pos_);
                    f0_ = f1_ = (start == idx) ? 1 : 0;
                    if (!f0_) break;
                }
            }
            if ((unsigned)outCount >= (unsigned)cap) break;
            if (!(idx < nIn)) break;
        }
    done:
        *nInP = idx;
        pos_ = pos_ - (double)(uint32_t)idx;
        *nOutP = outCount;
    }
};

}  // namespace vegvib
}  // namespace reno
