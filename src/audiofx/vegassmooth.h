// VEGAS's Smooth/Enhance DirectX effect (sfppack3_x64.dll, CLSID 9e3e4540-8339-11d0-aebc-00a0c9053912): its saved
// settings, presets and the sound itself, translated from the plug-in's own code (CSfSmooth). Reno's built-in
// Smooth/Enhance uses the same settings and saved form, so a project's VEGAS Smooth/Enhance loads into it and it saves
// back as VEGAS Smooth/Enhance.
#pragma once
#include "vegaspack3.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace reno {
namespace vegsmooth {

const char* const kPersistGuid = "a720c8e9-d534-4cf8-a2db-83cfbdca8dc5";

// The plug-in's own layout (what it saves, after its length and a GUID).
struct Settings {
    int32_t operation = 0;      // -5 (smooth most) .. 5 (enhance most); 0 does nothing
    int32_t bypass = 0;
};
static_assert(sizeof(Settings) == 8, "VEGAS Smooth/Enhance's settings are 8 bytes");

inline void sanitize(Settings& s)
{
    s.operation = std::max(-5, std::min(5, (int)s.operation));
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
// VEGAS's own presets, in its order
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = {{"(Default)", {0, 0}}, {"Boost high frequencies", {3, 0}}};
    return list;
}

// The sound: a one-pole filter per channel, the same for every operation but with its own pole (the plug-in's table).
// Negative operations smooth (low-pass); positive ones enhance (the pole flipped, so highs are lifted) with a gain to
// bring the level back up. Operation 0 passes the sound through.
class Engine {
public:
    void setup(int operation)
    {
        op_ = operation;
        passThrough_ = false;
        gain_ = 0;
        double f = 0;
        switch (operation) {
        case -5: f = 0.008; break;
        case -4: f = 0.02; break;
        case -3: f = 0.06; break;
        case -2: f = 0.15; break;
        case -1: f = 0.25; break;
        case 1: f = 0.2; gain_ = 1.78; break;
        case 2: f = 0.15; gain_ = 2.5; break;
        case 3: f = 0.1; gain_ = 3.1; break;
        case 4: f = 0.08; gain_ = 4.0; break;
        case 5: f = 0.05; gain_ = 5.6; break;
        default: passThrough_ = true; break;
        }
        if (operation < 0) {
            const double a = std::exp(f * -6.2831853);
            x_ = a;
            y_ = 1.0 - a;
        } else if (operation > 0) {
            const double a = std::exp(f * -6.2831853);
            y_ = 1.0 - a;
            x_ = a * -1.0;
        }
        reset();        // the plug-in clears its filters whenever its settings are set
    }
    void reset() { s_[0] = s_[1] = 0; }
    int operation() const { return op_; }
    void process(float* l, float* r, int n)
    {
        if (passThrough_) return;
        float* io[2] = {l, r};
        for (int k = 0; k < n; ++k)
            for (int c = 0; c < 2; ++c) {
                const double in = (double)io[c][k];
                double y = x_ * s_[c] + in * y_;
                if (std::fabs(y) < 2e-11) y = 0.0;
                const double out = op_ > 0 ? y * gain_ : y;
                s_[c] = y;
                io[c][k] = (float)out;
            }
    }

private:
    int op_ = 0;
    bool passThrough_ = true;
    double x_ = 0, y_ = 0, gain_ = 0;
    double s_[2] = {0, 0};
};

}  // namespace vegsmooth
}  // namespace reno
