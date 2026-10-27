// VEGAS's Resonant Filter DirectX effect (sfresfilter_x64.dll, CLSID a6a78627-d619-48bf-ad26-0c6b44b5c7d8): its saved
// settings, presets and its processing, worked out from the DLL's own code. Reno's built-in Resonant Filter uses the same
// settings and saved form, so a project's VEGAS Resonant Filter loads into it and it saves back as VEGAS Resonant Filter.
//
// The sound is filtered in place, a frame at a time, without delay: a state variable filter (low pass, band pass or high
// pass output) run three times per sample (it is oversampled by three), once for the second order and twice one after
// the other for the fourth order. The output is the filtered sound times the wet part plus the input times the dry part.
#pragma once
#include "../core/base.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace reno {
namespace vegrf {

const char* const kClsid = "a6a78627-d619-48bf-ad26-0c6b44b5c7d8";
const char* const kDxUid = "dshow:{A6A78627-D619-48BF-AD26-0C6B44B5C7D8}";

// The plug-in's own layout (what it saves, after its length and a GUID): six floats in the order of its parameters.
struct Settings {
    float freq = 20000.f;       // Hz: 20..20000
    float res = 0.f;            // %: 0..100
    float type = 0.f;           // 0 low pass, 1 band pass, 2 high pass
    float order = 0.f;          // 0 second order, 1 fourth order
    float bypass = 0.f;         // 0..1 (the host's: the plug-in's own sound does not look at it; it is not saved)
    float wet = 100.f;          // %: 0..100

    bool operator==(const Settings& o) const { return std::memcmp(this, &o, sizeof(Settings)) == 0; }
    bool operator!=(const Settings& o) const { return !(*this == o); }
};
static_assert(sizeof(Settings) == 24, "VEGAS Resonant Filter's settings are 6 floats");

// The GUID VEGAS Resonant Filter writes before its settings (length 0x2c); it also reads an older form (length 0x28:
// five floats, the bypass is zero and the wet part full).
inline const uint8_t* persistGuid()
{
    static const uint8_t g[16] = {0xa3, 0x0a, 0xd1, 0xc5, 0x65, 0x5a, 0xea, 0x42, 0xb1, 0x8c, 0x0c, 0x18, 0xed, 0xe3, 0x58, 0x2c};
    return g;
}
inline const uint8_t* persistGuidOld()
{
    static const uint8_t g[16] = {0x7d, 0xb2, 0x58, 0xab, 0x3b, 0x44, 0xe1, 0x4b, 0xa5, 0x13, 0x23, 0x7f, 0xf1, 0xe8, 0x13, 0xf0};
    return g;
}

// The plug-in's own way of keeping a value in its range (a value that is not a number stays)
inline float clampf(float v, float lo, float hi)
{
    if (v > hi) v = hi;
    if (lo > v) v = lo;
    return v;
}
inline void sanitize(Settings& s)
{
    s.freq = clampf(s.freq, 20.f, 20000.f);
    s.res = clampf(s.res, 0.f, 100.f);
    s.type = clampf(s.type, 0.f, 2.f);
    s.order = clampf(s.order, 0.f, 1.f);
    s.bypass = clampf(s.bypass, 0.f, 1.f);
    s.wet = clampf(s.wet, 0.f, 100.f);
}

// "S:" and base64 of what IPersistStream::Save writes: u32 length (0x2c), the GUID, the settings (the bypass as zero)
inline std::string encode(const Settings& in)
{
    Settings s = in;
    sanitize(s);
    s.bypass = 0.f;
    std::string b(4 + 16 + sizeof(Settings), '\0');
    const uint32_t len = (uint32_t)b.size();
    std::memcpy(&b[0], &len, 4);
    std::memcpy(&b[4], persistGuid(), 16);
    std::memcpy(&b[20], &s, sizeof(Settings));
    return "S:" + base64Encode(b.data(), b.size());
}

inline bool decode(const std::string& st, Settings* out)
{
    if (st.compare(0, 2, "S:") != 0) return false;
    const std::string b = base64Decode(st.substr(2));
    if (b.size() < 20) return false;
    uint32_t len;
    std::memcpy(&len, b.data(), 4);
    Settings s;
    if (std::memcmp(b.data() + 4, persistGuid(), 16) == 0 && len == 20 + sizeof(Settings) && b.size() >= len) {
        std::memcpy(static_cast<void*>(&s), b.data() + 20, sizeof(Settings));
    } else if (std::memcmp(b.data() + 4, persistGuidOld(), 16) == 0 && len == 20 + sizeof(Settings) - 4 && b.size() >= len) {
        std::memcpy(static_cast<void*>(&s), b.data() + 20, sizeof(Settings) - 4);
        s.bypass = 0.f;
        s.wet = 100.f;
    } else {
        return false;
    }
    sanitize(s);
    *out = s;
    return true;
}

struct Preset {
    std::string name;
    Settings s;
};

// VEGAS's own presets, in its order
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = [] {
        struct Raw {
            const char* name;
            float v[6];
        };
        static const Raw raw[] = {
            {"(Default)", {20000.f, 0.f, 0.f, 0.f, 0.f, 100.f}},
            {"Default all parameters", {20000.f, 0.f, 0.f, 0.f, 0.f, 100.f}},
            {"Bass Hum", {135.f, 90.f, 1.f, 1.f, 0.f, 100.f}},
            {"Big Boom Maker", {60.f, 100.f, 0.f, 0.f, 0.f, 100.f}},
            {"Chest Punch", {150.f, 75.f, 0.f, 0.f, 0.f, 100.f}},
            {"Ear Sugar", {5750.f, 33.3f, 1.f, 0.f, 0.f, 100.f}},
            {"Hank on Escape Route", {880.f, 25.f, 1.f, 1.f, 0.f, 100.f}},
            {"High Howl", {1500.f, 100.f, 0.f, 0.f, 0.f, 100.f}},
            {"High Pierce", {3600.f, 100.f, 0.f, 0.f, 0.f, 100.f}},
            {"Light Chime", {900.f, 75.f, 0.f, 0.f, 0.f, 100.f}},
            {"Low Howl", {150.f, 100.f, 0.f, 0.f, 0.f, 100.f}},
            {"Mid Howl", {500.f, 100.f, 0.f, 0.f, 0.f, 100.f}},
            {"Muffler", {350.f, 0.f, 0.f, 0.f, 0.f, 100.f}},
            {"PipSqueaking", {6000.f, 100.f, 2.f, 1.f, 0.f, 100.f}},
            {"Ringing Chime", {900.f, 100.f, 1.f, 0.f, 0.f, 100.f}},
            {"Rumbly Growl", {75.f, 50.f, 0.f, 1.f, 0.f, 100.f}},
            {"Slicing Vocals", {3000.f, 5.f, 1.f, 1.f, 0.f, 100.f}},
            {"Teenage Late-Night Radio by the Bed", {1500.f, 20.f, 2.f, 0.f, 0.f, 100.f}},
        };
        std::vector<Preset> v;
        for (const Raw& r : raw) {
            Preset p;
            p.name = r.name;
            std::memcpy(static_cast<void*>(&p.s), r.v, sizeof p.s);
            v.push_back(p);
        }
        return v;
    }();
    return list;
}

// -------------------------------------------------------------------------------------------------------- the sound
// the plug-in's way of keeping a recursive filter's state from going denormal: below 2^-32 it is zero
inline double flush(double v)
{
    uint64_t b;
    std::memcpy(&b, &v, 8);
    if ((uint32_t)((b >> 32) & 0x7ff00000u) < 0x3df00000u) b = 0;
    std::memcpy(&v, &b, 8);
    return v;
}

inline int toInt(float x)
{
    if (!(x > -2147483649.f && x < 2147483648.f)) return (int)0x80000000u;
    return (int)x;
}

class Dsp {
public:
    void setFormat(int rate, int channels)
    {
        rate_ = rate;
        ch_ = channels;
        changed_ = true;
    }
    void setSettings(const Settings& in)
    {
        Settings s = in;
        sanitize(s);
        s_ = s;
        changed_ = true;
    }
    // what the plug-in does when the sound starts again (a discontinuity): the states of the first stage are cleared
    // (the second stage's are not) and everything is set up again
    void restart() { restart_ = true; }

    // filters l (and r when there are two channels) in place
    void process(float* l, float* r, int n)
    {
        if (ch_ != 1 && ch_ != 2) return;
        if (restart_) {
            init(true);
            restart_ = false;
            changed_ = false;
        } else if (changed_) {
            init(false);
            changed_ = false;
        }
        for (int k = 0; k < n; ++k) {
            l[k] = frame(l[k], 0);
            if (ch_ == 2) r[k] = frame(r[k], 1);
        }
    }

private:
    struct St {
        double s[3] = {0, 0, 0};
    };

    int rate_ = 44100;
    int ch_ = 2;
    Settings s_;
    bool changed_ = true;
    bool restart_ = false;
    double fc_ = 0, q_ = 0, wet_ = 0, dry_ = 1;
    int type_ = 0, order_ = 0;
    St st1_[2], st2_[2];

    // SfResfilter 0x180006270
    void init(bool zero)
    {
        if (zero) {
            st1_[0] = St();
            st1_[1] = St();
        }
        double x = (double)s_.freq / (double)(3 * rate_);
        if (!(0.15117157974300832 > x)) x = 0.15117157974300832;
        fc_ = std::sin(x * 3.141592653589793) * 2.0;
        q_ = 1.0 / (((double)s_.res / 100.0) * 24.292893218813454 + 0.7071067811865476);
        wet_ = (double)s_.wet / 100.0;
        dry_ = 1.0 - wet_;
        type_ = toInt(s_.type);
        order_ = toInt(s_.order);
    }

    // SfResfilter 0x180009080: three steps of the state variable filter with the same input
    void kernel(St& st, float in) const
    {
        double s0 = st.s[0], s1 = st.s[1], s2 = st.s[2];
        for (int i = 0; i < 3; ++i) {
            s0 = flush((s2 * fc_) + s0);
            s1 = flush(((double)in - s0) - (s2 * q_));
            s2 = flush((fc_ * s1) + s2);
        }
        st.s[0] = s0;
        st.s[1] = s1;
        st.s[2] = s2;
    }
    float pick(const St& st) const { return (float)st.s[type_ == 0 ? 0 : (type_ == 1 ? 2 : 1)]; }

    float frame(float in, int c)
    {
        if (type_ < 0 || type_ > 2) return in;
        const double dryIn = (double)in * dry_;
        kernel(st1_[c], in);
        float y = pick(st1_[c]);
        if (order_ != 0) {
            kernel(st2_[c], y);
            y = pick(st2_[c]);
        }
        const float wetF = (float)wet_;
        const float a = wetF * y;
        const float b = (float)dryIn;
        return a + b;
    }
};

}  // namespace vegrf
}  // namespace reno
