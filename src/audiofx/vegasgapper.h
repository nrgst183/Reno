// VEGAS's Gapper/Snipper DirectX effect (sfppack3_x64.dll, CLSID 3f901a20-79be-11d0-aebc-00a0c9053912): its saved
// settings, presets and the sound itself, translated from the plug-in's own code (CSfGapsnp). Reno's built-in
// Gapper/Snipper uses the same settings and saved form, so a project's VEGAS Gapper/Snipper loads into it and it saves
// back as VEGAS Gapper/Snipper.
//
// The effect works on a cycle: the sound passes for one period (1 / frequency) with its edges faded, then an event
// of the set length follows: Gapper puts that much silence in (the sound is delayed by it), Snipper cuts that much
// of the sound out. So the output has more (Gapper) or fewer (Snipper) samples than the input; the plug-in
// takes input and gives output of its own length, and the host (VEGAS, Reno's built-in effect) deals with that.
#pragma once
#include "vegaspack3.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace reno {
namespace veggap {

const char* const kPersistGuid = "3f901a24-79be-11d0-aebc-00a0c9053912";

enum Mode { Gapper = 0, Snipper = 1 };

// The plug-in's own layout (what it saves, after its length and a GUID).
struct Settings {
    int32_t frequency = 200000;     // events per second times 10000 (0.0001 to 1000 Hz)
    int32_t length = 750;           // seconds times 10000 (0.0001 to 1 s)
    int32_t fade = 15;              // percent of the period (0 to 50)
    int32_t mode = Gapper;
};
static_assert(sizeof(Settings) == 16, "VEGAS Gapper/Snipper's settings are 16 bytes");

inline void sanitize(Settings& s)
{
    s.frequency = std::max(1, std::min(10000000, (int)s.frequency));
    s.length = std::max(1, std::min(10000, (int)s.length));
    s.fade = std::max(0, std::min(50, (int)s.fade));
    s.mode = s.mode ? Snipper : Gapper;
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
// VEGAS's own presets, in its order
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = {
        {"(Default)", {200000, 750, 15, Gapper}},
        {"Chopper 1", {200000, 750, 15, Gapper}},
        {"Chopper 2", {100000, 500, 15, Gapper}},
        {"Distort 1", {10000000, 10, 15, Gapper}},
        {"Distort 2", {900000, 100, 15, Gapper}},
        {"Fast & distorted 1", {2000000, 50, 15, Snipper}},
        {"Fast & distorted 2", {10000000, 10, 30, Snipper}},
        {"Very fast clipper", {3500000, 100, 5, Snipper}},
    };
    return list;
}

// What the page says under the settings: the length of the result as a percent of the original ("Percent of original").
inline int percentOfOriginal(const Settings& s)
{
    float x = ((float)s.frequency / 10000.0f) * ((float)s.length / 10000.0f);
    float pct = s.mode == Gapper ? (x + 1.0f) * 100.0f : 100.0f / (x + 1.0f);
    if (pct < 0) pct = 0;
    return (int)pct;
}

class Engine {
public:
    // The plug-in sets itself up from its settings and the sample rate; reset starts a new cycle (it does at the start
    // of the sound and after a jump in time, not when only the settings change).
    void setup(const Settings& s, int rate, bool reset)
    {
        mode_ = s.mode;
        const float freq = (float)s.frequency / 10000.0f;
        const float len = (float)s.length / 10000.0f;
        period_ = (int)((float)rate / freq);
        event_ = (int)((float)rate * len);
        if (event_ < 1) event_ = 1;
        fadeLen_ = (int)(((float)s.fade * (float)period_) / 100.0f);
        if (fadeLen_ == 0) fadeLen_ = 1;
        step_ = 1.0f / (float)fadeLen_;
        if (reset) {
            pos_ = 0;
            rest_ = 0;
            gain_ = 0.0f;
        }
    }

    // One call of the plug-in's process: takes up to nIn frames from in, writes up to cap frames to out (stereo, one array
    // per channel; the sound part is written without looking at cap, so cap must be at least nIn). Returns how many
    // input frames it took and how many it wrote.
    void process(const float* const in[2], int nIn, float* const out[2], int cap, int* consumed, int* produced)
    {
        int n = 0;                          // frames of the sound that passed
        int at = 0;                         // where the next input / output frame is
        auto pass = [&] {
            if (pos_ < fadeLen_) gain_ += step_;
            if (pos_ > period_ - fadeLen_) gain_ -= step_;
            for (int c = 0; c < 2; ++c) {
                const double x = (double)in[c][at];
                out[c][at] = (float)((double)gain_ * x);
            }
        };
        if (mode_ == Gapper) {
            if (pos_ <= period_) {
                while (n < nIn) {
                    pass();
                    ++at;
                    ++pos_;
                    ++n;
                    if (!(pos_ <= period_)) break;
                }
            }
            *consumed = n;
            int w = n;                      // frames written, the silence after the sound too
            if (pos_ >= period_ && rest_ != event_) {
                while (w < cap) {
                    out[0][w] = 0.0f;
                    out[1][w] = 0.0f;
                    ++w;
                    ++rest_;
                    if (rest_ == event_) break;
                }
            }
            *produced = w;
        } else {
            if (pos_ <= period_) {
                while (n < nIn) {
                    pass();
                    ++at;
                    ++pos_;
                    ++n;
                    if (!(pos_ <= period_)) break;
                }
            }
            *produced = n;
            int taken = n;                  // the cut part is taken from the input and not written
            if (pos_ >= period_ && rest_ < event_ && taken < nIn) {
                while (taken < nIn) {
                    ++rest_;
                    ++taken;
                    if (!(rest_ < event_)) break;
                }
            }
            *consumed = taken;
        }
        // the cycle is over: the next one starts
        if (pos_ >= period_ && rest_ >= event_) {
            pos_ = 0;
            rest_ = 0;
            gain_ = 0.0f;
        }
    }

private:
    int mode_ = Gapper;
    int period_ = 1, event_ = 1, fadeLen_ = 1;
    int pos_ = 0, rest_ = 0;
    float step_ = 1.0f, gain_ = 0.0f;
};

}  // namespace veggap
}  // namespace reno
