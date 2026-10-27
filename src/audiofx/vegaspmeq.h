// VEGAS's Parametric EQ DirectX effect (sfppack2_x64.dll, CLSID a8448720-96fd-11d0-aebc-00a0c9053912): its saved
// settings, presets and its filter design, worked out from the DLL's own code. Reno's built-in Parametric EQ uses the
// same settings and saved form, so a project's VEGAS Parametric EQ loads into it and it saves back as VEGAS Parametric EQ.
//
// The sound goes through the FIR engine of the sfppack2 equalizers (vegasfir.h). This effect designs a magnitude
// response of one filter: a low or high shelf, a band pass or a band notch/boost, put at a frequency with a width in
// octaves, with the response a straight line (per bin) between the flat level and the "amount" level.
#pragma once
#include "../core/base.h"
#include "vegasfir.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace reno {
namespace vegpm {

const char* const kClsid = "a8448720-96fd-11d0-aebc-00a0c9053912";
const char* const kDxUid = "dshow:{A8448720-96FD-11D0-AEBC-00A0C9053912}";

// The plug-in's own layout (what it saves, after its length and a GUID): 6 32-bit words.
struct Settings {
    int32_t style = 2;          // 0 low-frequency shelf, 1 high-frequency shelf, 2 band-pass, 3 band-notch/boost
    int32_t freq = 650;         // Hz, 20..22050
    int32_t width = 10;         // tenths of an octave, 1..50
    int32_t amount = -40;       // tenths of a dB, -600..200
    int32_t outGain = 0;        // tenths of a dB, -600..200
    int32_t accuracy = 1;       // 0: 256 point filter, 1: 1024, 2: 4096
};
static_assert(sizeof(Settings) == 24, "VEGAS Parametric EQ's settings are 24 bytes");

// The GUID VEGAS Parametric EQ writes before its settings (and only reads this one): a8448723-96fd-11d0-aebc-00a0c9053912
inline const uint8_t* persistGuid()
{
    static const uint8_t g[16] = {0x23, 0x87, 0x44, 0xa8, 0xfd, 0x96, 0xd0, 0x11, 0xae, 0xbc, 0x00, 0xa0, 0xc9, 0x05, 0x39, 0x12};
    return g;
}

inline void sanitize(Settings& s)
{
    s.style = std::max(0, std::min(3, (int)s.style));
    s.freq = std::max(20, std::min(22050, (int)s.freq));
    s.width = std::max(1, std::min(50, (int)s.width));
    s.amount = std::max(-600, std::min(200, (int)s.amount));
    s.outGain = std::max(-600, std::min(200, (int)s.outGain));
    s.accuracy = std::max(0, std::min(2, (int)s.accuracy));
}

// "S:" and base64 of what IPersistStream::Save writes: u32 length (0x2c), the GUID, the settings.
inline std::string encode(const Settings& s)
{
    std::string b(4 + 16 + sizeof(Settings), '\0');
    const uint32_t len = (uint32_t)b.size();
    std::memcpy(&b[0], &len, 4);
    std::memcpy(&b[4], persistGuid(), 16);
    std::memcpy(&b[20], &s, sizeof(Settings));
    return "S:" + base64Encode(b.data(), b.size());
}

inline bool decode(const std::string& st, Settings* out)
{
    if (st.size() < 3 || st.compare(0, 2, "S:") != 0) return false;
    const std::string b = base64Decode(st.substr(2));
    if (b.size() < 20 + sizeof(Settings)) return false;
    uint32_t len = 0;
    std::memcpy(&len, b.data(), 4);
    if (len != 0x2c || std::memcmp(b.data() + 4, persistGuid(), 16) != 0) return false;
    Settings s;
    std::memcpy(&s, b.data() + 20, sizeof(Settings));
    sanitize(s);
    *out = s;
    return true;
}

struct Preset {
    const char* name;
    Settings s;
};

// VEGAS's own presets, in its order (style, freq, width, amount, out gain, accuracy)
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = {
        {"(Default)", {2, 650, 10, -40, 0, 1}},
        {"Default all parameters", {2, 650, 10, -40, 0, 1}},
        {"200 Hz high-pass", {0, 200, 4, -150, 0, 2}},
        {"Hiss removal (cut frequencies above 6 kHz by 40 dB)", {1, 6000, 5, -400, 0, 2}},
        {"Phone line effect", {2, 800, 20, -250, 0, 1}},
        {"Remove frequencies around 1500 Hz by 25 dB", {3, 1500, 10, -250, 0, 2}},
    };
    return list;
}

inline const char* styleName(int s)
{
    static const char* const n[4] = {"Low-frequency shelf", "High-frequency shelf", "Band-pass", "Band-notch/boost"};
    return n[std::max(0, std::min(3, s))];
}

// The filter's magnitude response over n bins from 0 Hz to half the rate (the plug-in's design function, 0x1800140d0):
// m[0..n) is filled; returns the gain for the output (1: the output gain is in the response).
inline double design(double* m, int n, int rate, const Settings& s)
{
    const double amt = (double)(float)std::pow(10.0, (double)s.amount / 200.0);       // the amount as a gain (a float's value)
    double f = (double)s.freq;
    const double lim = (double)rate * 0.47;
    if (f > lim) f = lim;
    const double G = std::pow(10.0, (double)s.outGain / 200.0);
    double w = std::pow(2.0, (double)s.width / 10.0);
    if (s.style == 2 || s.style == 3) w = std::sqrt(w);
    for (int i = 0; i < n; ++i) m[i] = G;
    const double half = (double)rate * 0.5;
    int c = (int)(f / half * (double)(n - 1) + 0.5);
    if (c >= n) c = n - 1;
    int lo1 = (int)((double)c / w + 0.5);
    int hi1 = (int)((double)c * w + 0.5);
    if (hi1 >= n) hi1 = n - 1;
    const double w2 = (1.2 > w) ? (double)(float)w : 1.2000000476837158;
    const int lo2 = (int)((double)lo1 / w2 + 0.5);
    int hi2 = (int)((double)hi1 * w2 + 0.5);
    if (hi2 >= n) hi2 = n - 1;
    const double GA = G * amt;
    switch (s.style) {
    case 0: {
        for (int i = 0; i < lo1; ++i) m[i] = GA;
        const int cnt = c - lo1;
        if (cnt > 0 && lo1 < c) {
            const double step = (1.0 - amt) / (double)cnt;
            double g = amt;
            for (int i = lo1; i < c; ++i) {
                m[i] = g * G;
                g += step;
            }
        }
        break;
    }
    case 1: {
        for (int i = n - 1; i > hi1; --i) m[i] = GA;
        const int cnt = hi1 - c;
        if (cnt > 0 && hi1 > c) {
            const double step = (1.0 - amt) / (double)cnt;
            double g = amt;
            for (int i = hi1; i > c; --i) {
                m[i] = g * G;
                g += step;
            }
        }
        break;
    }
    case 2: {
        for (int i = 0; i < lo2; ++i) m[i] = GA;
        int cnt = lo1 - lo2;
        if (cnt > 0 && lo2 < lo1) {
            const double step = (1.0 - amt) / (double)cnt;
            double g = amt;
            for (int i = lo2; i < lo1; ++i) {
                m[i] = g * G;
                g += step;
            }
        }
        for (int i = hi2; i < n; ++i) m[i] = GA;
        cnt = hi2 - hi1;
        if (cnt > 0 && hi2 > hi1) {
            const double step = (1.0 - amt) / (double)cnt;
            double g = amt;
            for (int i = hi2; i > hi1; --i) {
                m[i] = g * G;
                g += step;
            }
        }
        break;
    }
    default: {
        for (int i = lo1; i < hi1; ++i) m[i] = GA;
        int cnt = hi2 - hi1;
        if (cnt > 0 && hi1 < hi2) {
            const double step = (1.0 - amt) / (double)cnt;
            double g = amt;
            for (int i = hi1; i < hi2; ++i) {
                m[i] = g * G;
                g += step;
            }
        }
        cnt = lo1 - lo2;
        if (cnt > 0 && lo1 > lo2) {
            const double step = (1.0 - amt) / (double)cnt;
            double g = amt;
            for (int i = lo1; i > lo2; --i) {
                m[i] = g * G;
                g += step;
            }
        }
        break;
    }
    }
    return 1.0;
}

}  // namespace vegpm
}  // namespace reno
