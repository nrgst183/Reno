// VEGAS's Paragraphic EQ DirectX effect (sfppack2_x64.dll, CLSID d616f3e0-d622-11ce-aac5-0020af0b99a3): its saved
// settings, presets and the filters, worked out from the DLL's own code. Reno's built-in Paragraphic EQ uses the same
// settings and saved form, so a project's VEGAS Paragraphic EQ loads into it and it saves back as VEGAS Paragraphic EQ.
//
// Unlike the other sfppack2 equalizers this one is made of recursive filters: up to four band filters (a resonator and
// a gain, in series), a low shelf and a high shelf (each two identical sections), and the sound is mixed with its dry
// signal. It works frame by frame, with no delay. The filter design is in vegassig.h.
#pragma once
#include "../core/base.h"
#include "vegassig.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <complex>
#include <cstring>
#include <string>
#include <vector>

namespace reno {
namespace vegpg {

const char* const kClsid = "d616f3e0-d622-11ce-aac5-0020af0b99a3";
const char* const kDxUid = "dshow:{D616F3E0-D622-11CE-AAC5-0020AF0B99A3}";

// The plug-in's own layout (what it saves, after its length and a GUID): 21 32-bit words.
struct Settings {
    int32_t dry = -250;                         // tenths of a dB, -250 (= -Inf.) .. 250
    int32_t wet = 0;                            // tenths of a dB, -250..250
    int32_t gain[4] = {0, 0, 0, 0};             // band gains, tenths of a dB, -250..250
    int32_t width[4] = {10, 10, 10, 10};        // band widths, tenths of an octave, 3..25
    int32_t freq[4] = {100, 300, 1000, 5000};   // band centers, Hz, 20..15000
    int32_t lowOn = 0;                          // low shelf on
    int32_t highOn = 0;                         // high shelf on
    int32_t lowFreq = 300;                      // Hz, 20..15000
    int32_t highFreq = 500;                     // Hz, 20..20000
    int32_t lowGain = 0;                        // tenths of a dB, -250 (= -Inf.) .. 250
    int32_t highGain = 0;
    int32_t extra = 4;                          // 0..4 (kept as the plug-in has it; the sound does not use it)

    bool operator==(const Settings& o) const { return std::memcmp(this, &o, sizeof(Settings)) == 0; }
    bool operator!=(const Settings& o) const { return !(*this == o); }
};
static_assert(sizeof(Settings) == 0x54, "VEGAS Paragraphic EQ's settings are 0x54 bytes");

// The GUID VEGAS Paragraphic EQ writes before its settings: d616f3ef-d622-11ce-aac5-0020af0b99a3
inline const uint8_t* persistGuid()
{
    static const uint8_t g[16] = {0xef, 0xf3, 0x16, 0xd6, 0x22, 0xd6, 0xce, 0x11, 0xaa, 0xc5, 0x00, 0x20, 0xaf, 0x0b, 0x99, 0xa3};
    return g;
}

inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return std::max(lo, std::min(hi, v)); }

inline void sanitize(Settings& s)
{
    s.dry = clampi(s.dry, -250, 250);
    s.wet = clampi(s.wet, -250, 250);
    for (int k = 0; k < 4; ++k) {
        s.gain[k] = clampi(s.gain[k], -250, 250);
        s.width[k] = clampi(s.width[k], 3, 25);
        s.freq[k] = clampi(s.freq[k], 20, 15000);
    }
    s.lowOn = clampi(s.lowOn, 0, 1);
    s.highOn = clampi(s.highOn, 0, 1);
    s.lowFreq = clampi(s.lowFreq, 20, 15000);
    s.highFreq = clampi(s.highFreq, 20, 20000);
    s.lowGain = clampi(s.lowGain, -250, 250);
    s.highGain = clampi(s.highGain, -250, 250);
    s.extra = (int32_t)std::min<uint32_t>((uint32_t)s.extra, 4u);
}

// "S:" and base64 of what IPersistStream::Save writes: u32 length (0x68), the GUID, the settings.
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
    if (len != 0x68 || std::memcmp(b.data() + 4, persistGuid(), 16) != 0) return false;
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

inline Settings mk(int dry, int wet, const int (&g)[4], const int (&w)[4], const int (&f)[4], int lowOn, int highOn, int lowFreq, int highFreq, int lowGain, int highGain)
{
    Settings s;
    s.dry = dry;
    s.wet = wet;
    for (int k = 0; k < 4; ++k) {
        s.gain[k] = g[k];
        s.width[k] = w[k];
        s.freq[k] = f[k];
    }
    s.lowOn = lowOn;
    s.highOn = highOn;
    s.lowFreq = lowFreq;
    s.highFreq = highFreq;
    s.lowGain = lowGain;
    s.highGain = highGain;
    return s;
}

// VEGAS's own presets, in its order
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = {
        {"(Default)", mk(-250, 0, {0, 0, 0, 0}, {10, 10, 10, 10}, {100, 300, 1000, 5000}, 0, 0, 300, 500, 0, 0)},
        {"Default all parameters", mk(-250, 0, {0, 0, 0, 0}, {10, 10, 10, 10}, {100, 300, 1000, 5000}, 0, 0, 300, 500, 0, 0)},
        {"15 dB notch centered at 1000 Hz", mk(-250, 0, {0, 0, -150, 0}, {10, 10, 10, 10}, {100, 300, 1000, 5000}, 0, 0, 300, 300, 0, 0)},
        {"60 Hz hum notch using four stacked filters", mk(-250, 0, {-250, -250, -250, -250}, {3, 3, 3, 3}, {60, 60, 60, 60}, 0, 0, 300, 300, 0, 0)},
        {"Add air (3 dB peak at 15 kHz)", mk(-250, 0, {0, 0, 0, 30}, {10, 10, 10, 7}, {100, 300, 1000, 15000}, 0, 0, 300, 300, 0, 0)},
        {"Add air (boost above 16k by 6 dB)", mk(-250, 0, {0, 0, 0, 0}, {10, 10, 10, 10}, {100, 300, 1000, 5000}, 0, 1, 300, 16000, 0, 60)},
        {"Anti-DTMF talk-off", mk(-250, 0, {-250, -250, -250, -250}, {3, 3, 3, 3}, {1209, 1336, 1447, 1633}, 0, 0, 300, 300, 0, 0)},
        {"Boost bass frequencies below 250 Hz by 4 dB", mk(-250, 0, {0, 0, 0, 0}, {10, 10, 10, 10}, {100, 300, 1000, 5000}, 1, 0, 250, 300, 40, 0)},
        {"Boost frequencies above 7 kHz", mk(-250, 0, {0, 0, 0, 0}, {10, 10, 10, 10}, {100, 300, 1000, 5000}, 0, 1, 300, 7000, 0, 30)},
        {"Fletcher-Munson curve (low volume compensation)", mk(-250, 0, {0, 0, -120, 0}, {10, 10, 10, 10}, {100, 300, 4000, 5000}, 1, 1, 200, 8000, 60, 60)},
        {"Hiss cut (cut above 8 kHz by 6 dB)", mk(-250, 0, {0, 0, 0, 0}, {10, 10, 10, 10}, {100, 300, 1000, 5000}, 0, 1, 300, 8000, 0, -60)},
        {"Remove low rumble below 80 Hz", mk(-250, 0, {0, 0, 0, 0}, {10, 10, 10, 10}, {100, 300, 1000, 5000}, 1, 0, 80, 300, -250, 0)},
        {"Remove very low and inaudible frequencies below 20 Hz", mk(-250, 0, {0, 0, 0, 0}, {10, 10, 10, 10}, {100, 300, 1000, 5000}, 1, 0, 20, 300, -250, 0)},
    };
    return list;
}

// The tiny values the DLL's filters round to zero (anything under 2^-32)
inline double flush(double v)
{
    uint64_t u;
    std::memcpy(&u, &v, 8);
    const uint32_t e = (uint32_t)(u >> 32) & 0x7ff00000u;
    return e < 0x3df00000u ? 0.0 : v;
}

// The filters and the mix, as the DLL has them (design: 0x18001d1c0 / 0x18001d4b0, per frame: 0x18001e570 / 0x18001d8f0)
class Dsp {
public:
    void setRate(int rate) { rate_ = std::max(1, rate); }
    void setSettings(const Settings& s)
    {
        if (!have_ || s != set_) {
            set_ = s;
            dirty_ = true;
        }
        have_ = true;
    }
    // A new stream (or a jump in time): the filters start again
    void restart() { started_ = false; }

    // The magnitude of the whole effect (filters and the dry/wet mix) at a frequency, in dB, for the page's graph
    // (designs the filters if they were not yet). Not the DLL's own code: the response of the filters it has.
    double responseDb(double freq)
    {
        if (!started_ || dirty_) {
            design(started_ ? 1 : 0);
            started_ = true;
            dirty_ = false;
        }
        const double w = 2.0 * 3.141592653589793 * freq / (double)rate_;
        const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
        const Chan& c = ch_[0];
        std::complex<double> h = 1.0;
        for (int k = 0; k < 4 && c.st[k].flag != 0; ++k) {
            const double* b = c.st[k].d;
            const std::complex<double> res = b[4] / (1.0 - b[2] * z1 + b[3] * z2);
            h *= 1.0 + b[5] * (1.0 - z1) * res;
        }
        for (int sec = 0; sec < 2; ++sec) {
            const double* r = c.a + 8 * sec;
            if (c.lowType == 0) break;
            const std::complex<double> res = r[4] / (1.0 - r[2] * z1 + r[3] * z2);
            if (c.lowType == 1) h *= r[7] * res + r[6];
            else h *= (1.0 - res) * r[7] + r[6];
        }
        for (int sec = 0; sec < 2; ++sec) {
            const double* r = c.a + 16 + 8 * sec;
            if (c.highType == 0) break;
            const std::complex<double> v = r[5] / (1.0 - r[2] * z1 + r[3] * z2);
            const std::complex<double> om = 1.0 - z1;
            h *= r[7] * v * om * om + r[6];
        }
        const std::complex<double> all = wet_ * h + dry_;
        const double m = std::abs(all);
        return m > 1e-6 ? 20.0 * std::log10(m) : -120.0;
    }

    void process(float* l, float* r, int n)
    {
        if (!started_ || dirty_) {
            design(started_ ? 1 : 0);
            started_ = true;
            dirty_ = false;
        }
        run(ch_[0], l, n);
        run(ch_[1], r, n);
    }

private:
    struct Stage {
        int32_t flag = 0;
        double d[6] = {0, 0, 0, 0, 0, 0};       // two states, then the coefficients
    };
    struct Chan {
        Stage st[4];
        double a[32] = {0};                     // low shelf (two sections of 8), high shelf
        int32_t lowType = 0, highType = 0;
    };
    int rate_ = 44100;
    Settings set_;
    bool have_ = false, dirty_ = false, started_ = false;
    Chan ch_[2];
    double wet_ = 1.0, dry_ = 0.0;

    // SfSig design for the settings (0x18001d4b0 and 0x18001d1c0). flag 0: a new stream, the filters' states are cleared.
    void design(int flag)
    {
        const Settings& s = set_;
        Chan& c = ch_[0];
        if (flag == 0)
            for (int k = 0; k < 4; ++k) c.st[k].d[0] = c.st[k].d[1] = 0.0;
        const double rate = (double)(uint32_t)rate_;
        // low shelf
        c.lowType = 0;
        if (s.lowOn != 0 && s.lowGain != 0) {
            double p[3];
            p[0] = (double)s.lowFreq / rate;
            p[1] = (double)s.lowGain / 10.0;
            if (s.lowGain > 0) {
                p[2] = 0.7;
                vegsig::lowShelfBoost(c.a, p, 1);
                c.lowType = 1;
            } else {
                p[2] = 0.1;
                vegsig::lowShelfCut(c.a, p, -25.0, 1);
                c.lowType = 2;
            }
            for (int k = 0; k < 8; ++k) c.a[8 + k] = c.a[k];
        }
        // high shelf
        c.highType = 0;
        if (s.highOn != 0 && s.highGain != 0) {
            double p[3];
            p[0] = (double)s.highFreq / rate;
            p[1] = (double)s.highGain / 10.0;
            if (s.highGain > 0) {
                p[2] = 0.7;
                vegsig::hiShelfBoost(c.a + 16, p, 1);
                c.highType = 1;
            } else {
                p[2] = 0.3;
                vegsig::hiShelfCut(c.a + 16, p, -25.0, 1);
                c.highType = 2;
            }
            for (int k = 0; k < 8; ++k) c.a[24 + k] = c.a[16 + k];
        }
        // the bands: the ones that are on are put first
        int cur = 0;
        for (int k = 0; k < 4; ++k) {
            c.st[k].flag = 0;
            const double gdb = (double)s.gain[k] / 10.0;
            c.st[cur].flag = gdb != 0.0 ? 1 : 0;
            if (c.st[cur].flag != 0) {
                double p[3];
                p[0] = (double)s.freq[k] / rate;
                p[1] = gdb;
                p[2] = 10.0 / (double)s.width[k];
                vegsig::bandPass(c.st[cur].d, p, 0);
                ++cur;
            }
        }
        // the other channel starts as a copy (states too)
        ch_[1] = ch_[0];
        wet_ = std::pow(10.0, (double)s.wet / 200.0);
        dry_ = 0.0;
        if (s.dry != -250) dry_ = std::pow(10.0, (double)s.dry / 200.0);
    }

    double filter(Chan& c, double x)
    {
        for (int k = 0; k < 4; ++k) {
            Stage& g = c.st[k];
            if (g.flag == 0) break;
            double* b = g.d;
            const double z = b[0];
            const double v = (x * b[4] - b[3] * b[1]) + z * b[2];
            b[1] = z;
            const double dd = (v - z) * b[5];
            b[0] = flush(v);
            x = x + dd;
        }
        double* r = c.a;
        if (c.lowType == 1) {
            const double s0 = r[0];
            const double t1 = r[3] * r[1];
            const double x1 = x * r[4];
            const double xc = x * r[6];
            r[1] = s0;
            const double t0 = s0 * r[2];
            const double fv = flush((x1 + t0) - t1);
            r[0] = fv;
            const double u = fv * r[7] + xc;
            const double s0b = r[8];
            const double t1b = r[11] * r[9];
            r[9] = s0b;
            const double t0b = s0b * r[10];
            const double u2 = u * r[12];
            const double uu = u * r[14];
            const double fv2 = flush((u2 + t0b) - t1b);
            r[8] = fv2;
            x = fv2 * r[15] + uu;
        } else if (c.lowType == 2) {
            const double s0 = r[0];
            const double t1 = r[3] * r[1];
            const double x1 = x * r[4];
            r[1] = s0;
            const double t0 = s0 * r[2];
            const double xc = x * r[6];
            const double fv = flush((x1 + t0) - t1);
            r[0] = fv;
            const double s0b = r[8];
            const double t1b = r[11] * r[9];
            const double t0b = s0b * r[10];
            const double u = (x - fv) * r[7] + xc;
            r[9] = s0b;
            const double uu = u * r[14];
            const double u2 = u * r[12];
            const double fv2 = flush((u2 + t0b) - t1b);
            r[8] = fv2;
            x = (u - fv2) * r[15] + uu;
        }
        if (c.highType != 0) {
            double* h = c.a + 16;
            const double s0 = h[0], s1 = h[1];
            const double a = s0 * h[2];
            const double yb = x * h[6];
            const double ya = x * h[5];
            const double t = s1 * h[3];
            const double two = s0 * 2.0;
            const double fv = flush((ya + a) - t);
            const double w = ((s1 + fv) - two) * h[7];
            h[1] = s0;
            h[0] = fv;
            const double s0b = h[8], s1b = h[9];
            const double u = w + yb;
            const double a2 = s0b * h[10];
            const double t2 = s1b * h[11];
            const double u2 = u * h[13];
            const double uu = u * h[14];
            const double two2 = s0b * 2.0;
            const double fv2 = flush((u2 + a2) - t2);
            const double w2 = ((s1b + fv2) - two2) * h[15];
            h[9] = s0b;
            h[8] = fv2;
            x = w2 + uu;
        }
        return x;
    }

    void run(Chan& c, float* p, int n)
    {
        for (int i = 0; i < n; ++i) {
            const double x = (double)p[i];
            const double y = filter(c, x);
            p[i] = (float)(y * wet_ + x * dry_);
        }
    }
};

}  // namespace vegpg
}  // namespace reno
