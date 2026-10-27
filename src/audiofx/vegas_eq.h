// VEGAS's Track EQ DirectX effect (sftrkfx1_x64.dll, CLSID 8cb69a0a-10e8-11d2-9b89-00104b8d13c2): its saved settings, presets
// and the processing, worked out from the DLL's code (same double math, same order, same state).
//
// Four bands, each a low shelf (type 1), a bell (type 2) or a high shelf (type 3) with a frequency, a gain, a bandwidth and, for
// shelves, a roll-off of 1 to 4 stages. Each band is built as a filter that is added to the signal: y = x + (A - 1) * f(x), so
// a band at 0 dB does nothing and is left out; a gain of -15 dB or lower is a complete cut of what the band selects.
#pragma once
#include "../core/base.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace reno {
namespace vegeq {

const char* const kClsid = "8cb69a0a-10e8-11d2-9b89-00104b8d13c2";
const char* const kDxUid = "dshow:{8CB69A0A-10E8-11D2-9B89-00104B8D13C2}";

struct Cx {
    double re, im;
};
// complex products and quotients with the operations in the order the DLL's code does them
inline Cx cmul(Cx a, Cx b) { return {a.re * b.re - a.im * b.im, a.im * b.re + a.re * b.im}; }
inline Cx cdiv(Cx a, Cx b)
{
    const double d = b.re * b.re + b.im * b.im;
    return {(a.re * b.re + a.im * b.im) / d, (a.im * b.re - a.re * b.im) / d};
}

// One band's filter section as the DLL keeps it: c0, c1 (the feedback), g (the gain of what is added), the number of
// cascaded passes and the state of the passes (pairs of values).
struct Stage {
    double c0 = 0, c1 = 0;
    double g = 0;
    int count = 0;
    double z[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    // the hand-over from the settings before to the ones now: a counter and the old coefficients (see the processing)
    int ctr = 0;
    double oldC0 = 0, oldC1 = 0, oldG = 0;
};

// What the DLL's per-band design functions read: one band's frequency (as a fraction of the sample rate), gain (dB), bandwidth
// (octaves) and, for the shelves, the roll-off.
struct BandIn {
    double gain = 0;
    double freq = 0;
    double bw = 0;
    int rolloff = 1;
};

inline double tableCorrection(double freq, int rolloff)
{
    // 1.67 2.035 2.385 / 1.4944 ... : the stretch of a shelf's corner for 2, 3 and 4 passes, at 0, 1/8, ... 1/2 of the sample rate
    static const double t[18] = {1.67, 2.035, 2.385, 1.4944, 1.7751, 1.9735, 1.2882, 1.4132, 1.4891, 1.1176, 1.1609, 1.1854, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0};
    const int idx = (int)(freq * 8.0);
    const double frac = freq - (double)idx * 0.125;
    const int row = idx * 3 + (rolloff - 2);
    double d = t[row + 3] - t[row];
    d = frac * d;
    return d + t[row];
}

inline double gainFactor(double gainDb, double sectionGain)
{
    // -15 dB and below cuts everything: the added part is minus the signal
    double a = 0.0;
    if (!(-15.0 >= gainDb)) a = std::pow(10.0, gainDb / 20.0);
    a = a - 1.0;
    return a * sectionGain;
}

// type 1: low shelf (0x18001f380)
inline void snapshot(Stage* st, int ctr, bool withC1)
{
    if (st->ctr != 0) return;
    st->ctr = ctr;
    st->oldG = st->g;
    st->oldC0 = st->c0;
    if (withC1) st->oldC1 = st->c1;
}

inline void designLowShelf(const BandIn& in, Stage* st)
{
    st->count = in.rolloff;
    snapshot(st, in.rolloff, false);
    double f = in.freq;
    if (in.rolloff > 1) {
        f = f / tableCorrection(f, in.rolloff);
        if (f >= 0.5) f = 0.4999;
    }
    const double t = std::tan(f * 3.141592653589793);
    const double c0 = (1.0 - t) / (t + 1.0);
    double g = 1.0 - c0;
    st->c0 = c0;
    g = g * 0.5;
    if (-15.0 >= in.gain) g = g * -1.0;
    else g = gainFactor(in.gain, g);
    st->g = g;
}
// type 3: high shelf (0x18001f510)
inline void designHighShelf(const BandIn& in, Stage* st)
{
    st->count = in.rolloff;
    snapshot(st, in.rolloff, false);
    double f = in.freq;
    if (in.rolloff > 1) {
        f = f * tableCorrection(f, in.rolloff);
        if (f >= 0.5) f = 0.4999;
    }
    const double t = std::tan(f * 3.141592653589793);
    double c0 = (1.0 - t) / (t + 1.0);
    st->c0 = c0;
    double g = c0 + 1.0;
    g = g * 0.5;
    if (-15.0 >= in.gain) g = g * -1.0;
    else g = gainFactor(in.gain, g);
    st->g = g;
}
// type 2: bell (0x18001f970). The two edges of the band, w / r and w * r (r is half the bandwidth in octaves as a ratio), go
// through the bilinear transform's prewarping to an analog band-pass with the poles p1 and p2, back to two z-plane poles z1 and
// z2; the gain of the resulting resonator is set to one at the middle of the band.
inline void designBell(const BandIn& in, Stage* st)
{
    const double pi = 3.141592653589793;
    const double r = std::sqrt(std::pow(2.0, in.bw));
    double wHi = in.freq * r;
    const double wLo = in.freq / r;
    if (wHi > 0.49) wHi = 0.49;
    double t1 = std::tan(wLo * pi);
    t1 = t1 * 2.0;
    double t2 = std::tan(wHi * pi);
    t2 = t2 * 2.0;
    const double w0 = std::sqrt(t2 * t1);
    const double nb = (t2 - t1) * -0.5;                      // minus half the analog bandwidth
    const Cx zc = cdiv({w0, 0.0}, {nb, 0.0});
    const Cx zz = cmul(zc, zc);
    const Cx w = {1.0 - zz.re, 0.0 + (0.0 - zz.im)};
    // the complex square root of w
    const double h = std::hypot(w.re, w.im);
    double si = 0.0, sr = 0.0;
    const double ia = (h - w.re) * 0.5;
    if (ia >= 0.0) si = std::sqrt(ia);
    const double ra = (w.re + h) * 0.5;
    if (ra >= 0.0) sr = std::sqrt(ra);
    if (0.0 > w.im) si = -si;
    const Cx p1 = cmul({sr + 1.0, 0.0 + si}, {nb, 0.0});
    const Cx p2 = cmul({1.0 - sr, 0.0 + (0.0 - si)}, {nb, 0.0});
    const Cx z1 = cdiv({p1.re + 2.0, 0.0 + p1.im}, {2.0 - p1.re, 0.0 + (0.0 - p1.im)});
    const Cx z2 = cdiv({p2.re + 2.0, 0.0 + p2.im}, {2.0 - p2.re, 0.0 + (0.0 - p2.im)});
    const Cx a = {(0.0 - z1.re) - z2.re, (0.0 + (0.0 - z1.im)) + (0.0 - z2.im)};       // minus the sum of the poles
    const Cx pz = cmul(z1, z2);                                                          // their product
    snapshot(st, 1, true);
    st->c0 = 0.0 - a.re;
    st->c1 = 0.0 - pz.re;
    // the resonator's gain in the middle of the band, theta = (wHi + wLo) * pi
    const double theta = (wHi + wLo) * pi;
    const double c = std::cos(theta), s = std::sin(theta);
    const double reAe = c * a.re - s * a.im;
    const double imAe = s * a.re + a.im * c;
    const double c2 = c * c - s * s;
    const double s2 = (s * c) + (s * c);
    const double reD = (c2 + reAe) + pz.re;
    const double imD = (s2 + imAe) + pz.im;
    const double reN = c2 - 1.0;
    const double imN = s2 + 0.0;
    const double dd = imD * imD + reD * reD;
    const double qIm = (imN * reD - imD * reN) / dd;
    const double qRe = (imN * imD + reN * reD) / dd;
    double g;
    if (qRe == 0.0 && qIm == 0.0) g = 1.0;
    else g = 1.0 / std::hypot(qIm, qRe);
    st->g = gainFactor(in.gain, g);
}

// ------------------------------------------------------------------------------------------ settings
// The plug-in's own layout (what it saves, after its length and a GUID): 32 integers.
struct Settings {
    int32_t gain[4] = {0, 0, 0, 0};                 // tenths of a dB, -150..150 (the presets go to -250: the same full cut)
    int32_t bw[4] = {10, 10, 10, 10};               // tenths of an octave, 1..40
    int32_t freq[4] = {100, 300, 1000, 5000};       // Hz, 20..20000
    int32_t type[4] = {1, 2, 2, 3};                 // 1 low shelf, 2 bell, 3 high shelf
    int32_t rolloff[4] = {2, 1, 1, 2};              // passes of a shelf, 1..4
    int32_t enable[4] = {1, 1, 1, 1};
    int32_t selected = 0;                           // the band the page shows
    int32_t unused = 0;
    int32_t channel[6] = {1, 1, 1, 1, 1, 1};        // left, right, center, left surround, right surround, LFE
};
static_assert(sizeof(Settings) == 128, "VEGAS Track EQ's settings are 128 bytes");

inline const uint8_t* persistGuid()
{
    static const uint8_t g[16] = {0x17, 0x06, 0x63, 0xc7, 0x8f, 0x89, 0x13, 0x4b, 0x86, 0x98, 0x02, 0x83, 0x42, 0x75, 0xe8, 0x20};
    return g;
}

inline void sanitize(Settings& s)
{
    for (int b = 0; b < 4; ++b) {
        s.gain[b] = std::max(-250, std::min(150, (int)s.gain[b]));
        s.bw[b] = std::max(1, std::min(40, (int)s.bw[b]));
        s.freq[b] = std::max(20, std::min(20000, (int)s.freq[b]));
        s.type[b] = std::max(1, std::min(3, (int)s.type[b]));
        s.rolloff[b] = std::max(1, std::min(4, (int)s.rolloff[b]));
        s.enable[b] = s.enable[b] ? 1 : 0;
    }
    s.selected = std::max(0, std::min(3, (int)s.selected));
    for (int c = 0; c < 6; ++c) s.channel[c] = s.channel[c] ? 1 : 0;
}

// "S:" and base64 of what IPersistStream::Save writes: u32 length (0x94), the GUID, the settings.
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
    if (b.size() < 20 + sizeof(Settings) || std::memcmp(b.data() + 4, persistGuid(), 16) != 0) return false;
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

inline Settings make(const int (&gain)[4], const int (&bw)[4], const int (&freq)[4], const int (&type)[4], const int (&rolloff)[4], const int (&enable)[4], int selected, int unused,
                     const int (&ch)[4])
{
    Settings s;
    for (int b = 0; b < 4; ++b) {
        s.gain[b] = gain[b];
        s.bw[b] = bw[b];
        s.freq[b] = freq[b];
        s.type[b] = type[b];
        s.rolloff[b] = rolloff[b];
        s.enable[b] = enable[b];
        s.channel[b] = ch[b];
    }
    s.selected = selected;
    s.unused = unused;
    return s;
}

// VEGAS's own presets, in its order (names from the DLL's string table)
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = {
        {"(Default)", make({0,0,0,0}, {10,10,10,10}, {100,300,1000,5000}, {1,2,2,3}, {2,1,1,2}, {1,1,1,1}, 0, 0, {1,1,1,1})},
        {"Default all parameters", make({0,0,0,0}, {10,10,10,10}, {100,300,1000,5000}, {1,2,2,3}, {2,1,1,2}, {1,1,1,1}, 0, 0, {1,1,1,1})},
        {"15 dB notch centered at 1000 Hz", make({0,0,-150,0}, {10,10,10,10}, {100,300,1000,5000}, {1,2,2,3}, {1,1,1,1}, {0,0,1,0}, 2, 0, {1,1,1,1})},
        {"60 Hz hum notch using four stacked filters", make({-250,-250,-250,-250}, {3,3,3,3}, {60,60,60,60}, {2,2,2,2}, {1,1,1,1}, {1,1,1,1}, 0, 0, {1,1,1,1})},
        {"Add air (3 dB peak at 15 kHz)", make({0,0,0,30}, {10,10,10,7}, {100,300,1000,15000}, {2,2,2,2}, {1,1,1,1}, {0,0,0,1}, 3, 0, {1,1,1,1})},
        {"Add air (boost above 16k by 6 dB)", make({0,0,0,60}, {10,10,10,10}, {100,300,1000,16000}, {1,2,2,3}, {1,1,1,1}, {0,0,0,1}, 3, 0, {1,1,1,1})},
        {"Anti-DTMF Talkoff", make({-250,-250,-250,-250}, {3,3,3,3}, {1209,1336,1447,1633}, {2,2,2,2}, {1,1,1,1}, {1,1,1,1}, 0, 0, {1,1,1,1})},
        {"Boost bass frequencies below 250 Hz by 4 dB", make({40,0,0,0}, {10,10,10,10}, {250,300,1000,5000}, {1,2,2,3}, {1,1,1,1}, {1,0,0,0}, 0, 0, {1,1,1,1})},
        {"Boost frequencies above 7 kHz", make({0,0,0,30}, {10,10,10,10}, {100,300,1000,7000}, {1,2,2,3}, {1,1,1,1}, {0,0,0,1}, 3, 0, {1,1,1,1})},
        {"Fletcher-Munson curve (low volume compensation)", make({60,0,-120,60}, {10,10,10,10}, {200,300,4000,8000}, {1,2,2,3}, {1,1,1,1}, {1,0,1,1}, 0, 0, {1,1,1,1})},
        {"Hiss cut (cut above 8 kHz by 6 dB)", make({0,0,0,-60}, {10,10,10,10}, {100,300,1000,8000}, {1,2,2,3}, {1,1,1,1}, {0,0,0,1}, 3, 0, {1,1,1,1})},
        {"Remove low rumble below 80 Hz", make({-250,0,0,0}, {10,10,10,10}, {80,300,1000,5000}, {1,2,2,3}, {1,1,1,1}, {1,0,0,0}, 0, 0, {1,1,1,1})},
        {"Remove very low and inaudible frequencies below 20 Hz", make({-250,0,0,0}, {10,10,10,10}, {20,300,1000,5000}, {1,2,2,3}, {1,1,1,1}, {1,0,0,0}, 0, 0, {1,1,1,1})},
    };
    return list;
}

// ------------------------------------------------------------------------------------------ the filter
inline double flushSmall(double d)
{
    uint64_t b;
    std::memcpy(&b, &d, 8);
    if ((uint32_t)((b >> 32) & 0x7ff00000u) < 0x3df00000u) return 0.0;
    return d;
}

// One channel's bands: the active bands in order, each with its section.
struct Chain {
    int count = 0;           // active bands
    int type[4] = {0, 0, 0, 0};
    Stage st[4];
};

// The Track EQ for a pair of channels (VEGAS runs both channels of a stereo stream through the same band after band, with
// the sections of both side by side), as the DLL's float path does it.
struct Eq {
    Chain ch[2];
    bool idle = true;        // no band is active: the sound passes untouched
    double rate = 48000;

    // The DLL's reading of the settings (0x1800204a0) and building of the bands (0x180020050). 'keep' keeps what the sections
    // hold of the sound so far (a change of settings in the middle of a stream); otherwise it starts silent.
    void prepare(const Settings& in, double sampleRate, bool keep)
    {
        Settings s = in;
        sanitize(s);
        rate = sampleRate;
        double gain[4], fr[4], bw[4];
        int type[4], rolloff[4];
        const uint32_t r = (uint32_t)(int64_t)sampleRate;
        for (int b = 0; b < 4; ++b) {
            gain[b] = (s.enable[b] && s.gain[b] != 0) ? (double)s.gain[b] / 10.0 : 0.0;
            double f = (double)s.freq[b] / (double)r;
            if (f > 0.5) f = 0.5;
            fr[b] = f;
            bw[b] = (double)s.bw[b] / 10.0;
            type[b] = s.type[b];
            rolloff[b] = s.rolloff[b];
            if (type[b] != 2 && rolloff[b] > 1) gain[b] = gain[b] / (double)((float)rolloff[b] / 1.25f);
        }
        idle = true;
        for (int c = 0; c < 2; ++c) {
            Chain& chain = ch[c];
            chain.count = 0;
            for (int b = 0; b < 4; ++b) {
                chain.type[b] = 0;
                if (gain[b] == 0.0) continue;
                BandIn bi;
                bi.gain = gain[b];
                bi.freq = fr[b];
                bi.bw = bw[b];
                bi.rolloff = rolloff[b];
                Stage* st = &chain.st[chain.count];
                switch (type[b]) {
                case 1: designLowShelf(bi, st); break;
                case 2: designBell(bi, st); break;
                case 3: designHighShelf(bi, st); break;
                default: break;
                }
                chain.type[chain.count] = type[b];
                ++chain.count;
                idle = false;
            }
        }
        if (idle || !keep)
            for (Chain& chain : ch)
                for (Stage& st : chain.st)
                    for (double& z : st.z) z = 0.0;
    }

    void process(float* l, float* r, int n)
    {
        if (idle || n <= 0) return;
        for (int i = 0; i < ch[0].count; ++i) {
            Stage& a = ch[0].st[i];
            Stage& b = ch[1].st[i];
            a.z[0] = flushSmall(a.z[0]);
            a.z[1] = flushSmall(a.z[1]);
            b.z[0] = flushSmall(b.z[0]);
            b.z[1] = flushSmall(b.z[1]);
            switch (ch[0].type[i]) {
            case 1:
                for (int p = 0; p < a.count; ++p) {
                    lowShelfPass(a, b, l, r, n);
                    if (a.count > 1) {
                        rotate(a);
                        rotate(b);
                    }
                }
                break;
            case 2: bellPass(a, b, l, r, n); break;
            case 3:
                for (int p = 0; p < a.count; ++p) {
                    highShelfPass(a, b, l, r, n);
                    if (a.count > 1) {
                        rotate(a);
                        rotate(b);
                    }
                }
                break;
            default: break;
            }
        }
    }

    // The size of the response at 'hz' in dB (for the page's graph), from the sections' coefficients: each pass of a shelf is
    // 1 + (1 -+ z^-1) g / (1 - c0 z^-1) (the low shelf adds, the high shelf takes), the bell 1 + (1 - z^-2) g / (1 - c0 z^-1 - c1 z^-2).
    double responseDb(double hz) const
    {
        const double w = 2.0 * 3.141592653589793 * hz / rate;
        const Cx z1 = {std::cos(w), -std::sin(w)};
        const Cx z2 = {std::cos(2 * w), -std::sin(2 * w)};
        Cx h = {1.0, 0.0};
        const Chain& c = ch[0];
        for (int i = 0; i < c.count; ++i) {
            const Stage& s = c.st[i];
            Cx one;
            if (c.type[i] == 2) {
                const Cx den = {1.0 - s.c0 * z1.re - s.c1 * z2.re, -s.c0 * z1.im - s.c1 * z2.im};
                const Cx num = {s.g * (1.0 - z2.re), -s.g * z2.im};
                const Cx q = cdiv(num, den);
                one = {1.0 + q.re, q.im};
            } else {
                const double sg = c.type[i] == 1 ? 1.0 : -1.0;
                const Cx den = {1.0 - s.c0 * z1.re, -s.c0 * z1.im};
                const Cx num = {s.g * (1.0 + sg * z1.re), s.g * sg * z1.im};
                const Cx q = cdiv(num, den);
                one = {1.0 + q.re, q.im};
                Cx all = one;
                for (int p = 1; p < s.count; ++p) all = cmul(all, one);
                one = all;
            }
            h = cmul(h, one);
        }
        const double m = h.re * h.re + h.im * h.im;
        return m > 1e-30 ? 10.0 * std::log10(m) : -300.0;
    }

private:
    // after a pass of a shelf with several passes, the next pair of the section's state values comes first
    static void rotate(Stage& s)
    {
        const int n = s.count;
        const double a = s.z[0], b = s.z[1];
        for (int i = 0; i < 2 * n - 2; ++i) s.z[i] = s.z[i + 2];
        s.z[2 * n - 2] = a;
        s.z[2 * n - 1] = b;
    }

    static void lowShelfPass(Stage& L, Stage& R, float* l, float* r, int n)
    {
        double zl = L.z[0], zr = R.z[0];
        for (int k = 0; k < n; ++k) {
            const double xl = (double)l[k], xr = (double)r[k];
            const double sl = xl + zl, sr = xr + zr;
            const double tl = flushSmall(zl * L.c0), tr = flushSmall(zr * R.c0);
            double nl = tl + xl * L.g, nr = tr + xr * R.g;
            if (L.ctr) {
                --L.ctr;
                nl = ((L.oldG - L.g) * xl + (L.oldC0 + 1.0) * nl) / (L.c0 + 1.0);
            }
            if (R.ctr) {
                --R.ctr;
                nr = ((R.oldG - R.g) * xr + (R.oldC0 + 1.0) * nr) / (R.c0 + 1.0);
            }
            l[k] = (float)(sl + nl);
            r[k] = (float)(sr + nr);
            zl = nl;
            zr = nr;
        }
        L.z[0] = zl;
        R.z[0] = zr;
    }

    static void highShelfPass(Stage& L, Stage& R, float* l, float* r, int n)
    {
        double zl = L.z[0], zr = R.z[0];
        if (L.ctr) {
            const double x = (double)l[0];
            --L.ctr;
            zl = ((L.oldG - L.g) * x + (L.oldC0 - 1.0) * zl) / (L.c0 - 1.0);
        }
        if (R.ctr) {
            const double x = (double)r[0];
            --R.ctr;
            zr = ((R.oldG - R.g) * x + (R.oldC0 - 1.0) * zr) / (R.c0 - 1.0);
        }
        for (int k = 0; k < n; ++k) {
            const double xl = (double)l[k], xr = (double)r[k];
            const double dl = xl - zl, dr = xr - zr;
            const double nl = flushSmall(zl * L.c0) + xl * L.g;
            const double nr = flushSmall(zr * R.c0) + xr * R.g;
            l[k] = (float)(dl + nl);
            r[k] = (float)(dr + nr);
            zl = nl;
            zr = nr;
        }
        L.z[0] = zl;
        R.z[0] = zr;
    }

    // the bell: before the sound, a one-time correction of the section's state for the change of coefficients; then frame
    // after frame y = (x - z1) + v, v = (z0 c0 + z1 c1) + x g (an odd frame count starts with one frame done the other way round)
    static void bellStart(Stage& s, double x)
    {
        if (!s.ctr) return;
        const double d = (s.c0 + s.c1) - 1.0;
        if (d != 0.0) {
            double t = (s.oldG - s.g) * x;
            t = t + (s.oldC0 - s.c0) * s.z[0];
            t = t + (s.oldC1 - s.c1) * s.z[1];
            t = t / d;
            const double z1 = s.z[1], z0 = s.z[0];
            s.z[1] = t + z1;
            s.z[0] = t + z0;
        }
        --s.ctr;
    }

    static void bellPass(Stage& L, Stage& R, float* l, float* r, int n)
    {
        bellStart(L, (double)l[0]);
        bellStart(R, (double)l[0]);      // (the DLL takes the left channel's first sample for both)
        int k = 0;
        if (n & 1) {
            oddFrame(L, l[0], &l[0]);
            oddFrame(R, r[0], &r[0]);
            k = 1;
        }
        double s0l = L.z[0], s1l = L.z[1], s0r = R.z[0], s1r = R.z[1];
        for (; k < n; ++k) {
            const double xl = (double)l[k], xr = (double)r[k];
            const double dl = xl - s1l, dr = xr - s1r;
            const double t1l = flushSmall(s1l * L.c1), t1r = flushSmall(s1r * R.c1);
            double vl = s0l * L.c0;
            vl = vl + t1l;
            vl = vl + xl * L.g;
            double vr = s0r * R.c0;
            vr = vr + t1r;
            vr = vr + xr * R.g;
            l[k] = (float)(dl + vl);
            r[k] = (float)(dr + vr);
            s1l = s0l;
            s0l = vl;
            s1r = s0r;
            s0r = vr;
        }
        L.z[0] = s0l;
        L.z[1] = s1l;
        R.z[0] = s0r;
        R.z[1] = s1r;
    }

    static void oddFrame(Stage& s, float in, float* out)
    {
        const double x = (double)in;
        double v = x * s.g;
        v = v + s.z[0] * s.c0;
        v = v + s.z[1] * s.c1;
        *out = (float)((x + v) - s.z[1]);
        s.z[1] = s.z[0];
        s.z[0] = v;
        s.z[1] = flushSmall(s.z[1]);
        s.z[0] = flushSmall(s.z[0]);
    }
};

// ------------------------------------------------------------------------------------------ the settings as a list of values
// The 30 values Reno's effect (and its page) keep: for the bands 0..3 in turn the frequency (Hz), gain (dB), bandwidth
// (octaves), roll-off, mode (0 low shelf, 1 band, 2 high shelf) and enable; then the six channel enables.
const int kParams = 30;
inline Settings fromParams(const double (&v)[kParams], int selected, int unused)
{
    Settings s;
    for (int b = 0; b < 4; ++b) {
        s.freq[b] = (int32_t)std::lround(v[b]);
        s.gain[b] = (int32_t)std::lround(v[4 + b] * 10.0);
        s.bw[b] = (int32_t)std::lround(v[8 + b] * 10.0);
        s.rolloff[b] = (int32_t)std::lround(v[12 + b]);
        s.type[b] = (int32_t)std::lround(v[16 + b]) + 1;
        s.enable[b] = v[20 + b] >= 0.5 ? 1 : 0;
    }
    for (int c = 0; c < 6; ++c) s.channel[c] = v[24 + c] >= 0.5 ? 1 : 0;
    s.selected = selected;
    s.unused = unused;
    sanitize(s);
    return s;
}
inline void toParams(const Settings& in, double (&v)[kParams])
{
    Settings s = in;
    sanitize(s);
    for (int b = 0; b < 4; ++b) {
        v[b] = s.freq[b];
        v[4 + b] = s.gain[b] / 10.0;
        v[8 + b] = s.bw[b] / 10.0;
        v[12 + b] = s.rolloff[b];
        v[16 + b] = s.type[b] - 1;
        v[20 + b] = s.enable[b];
    }
    for (int c = 0; c < 6; ++c) v[24 + c] = s.channel[c];
}

}  // namespace vegeq
}  // namespace reno
