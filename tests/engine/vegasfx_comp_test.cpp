// VEGAS's Track Compressor (sftrkfx1_x64.dll, CSfTComp) as Reno's built-in "reno.trkcomp" reproduces it: the compressor's code
// against samples computed with the DLL's own code (run on Linux in a loader that maps the DLL) at 44100 and 48000 Hz,
// the saved form, the presets, a change of settings while running, settings of
// older Reno versions, the DirectX fallback and the way it is saved as VEGAS's.
//
//   vegasfx_comp_test
//
// The DLL is not needed to run this: the expected values are stored here.
#include "../../src/audiofx/audioplugin.h"
#include "../../src/audiofx/vegas_trkfx.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

static const int kFrames = 30000;
static float input(int k)       // exact pseudo-random numbers in -1..1
{
    uint32_t h = (uint32_t)k * 2654435761u;
    h ^= h >> 15;
    return (float)((int)(h & 0xffff) - 32768) / 32768.f;
}
// the test signal: bursts of different levels, 2000 frames each (so the gate opens, holds and closes), stereo
static void makeInput(std::vector<float>& l, std::vector<float>& r)
{
    static const float amp[7] = {0.0005f, 0.02f, 0.5f, 0.0002f, 0.8f, 0.01f, 0.3f};
    l.resize(kFrames);
    r.resize(kFrames);
    for (int k = 0; k < kFrames; ++k) {
        const float a = amp[(k / 2000) % 7];
        l[(size_t)k] = a * input(2 * k);
        r[(size_t)k] = a * input(2 * k + 1);
    }
}
static uint32_t bitsOf(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
static uint32_t fnvInter(const std::vector<float>& l, const std::vector<float>& r)
{
    uint32_t h = 2166136261u;
    for (size_t k = 0; k < l.size(); ++k)
        for (float f : {l[k], r[k]}) {
            const uint32_t b = bitsOf(f);
            for (int i = 0; i < 4; ++i) {
                h ^= (b >> (8 * i)) & 0xff;
                h *= 16777619u;
            }
        }
    return h;
}

struct Sample {
    int index;               // into the interleaved output (2 * frame + channel)
    uint32_t bits;
};
struct Case {
    const char* name;
    float inGain, outGain, threshold, amount, attack, release;
    int autoGain, smooth, bypass;
    uint32_t hash;           // FNV-1a over the 60000 output floats
    int changed;             // how many of them differ from the input
    std::vector<Sample> samples;
};

// computed with the DLL's prepare and processing for the input above
static const std::vector<Case> k44100 = {
    {"Default", 0.f, 0.f, 0.f, 1.f, 15.f, 250.f, 1, 0, 0, 0x2bd1eda0u, 0, {}},
    {"Soft Limiter", 0.f, 0.f, -6.f, 25.f, 0.f, 250.f, 0, 1, 0, 0x09c8bf09u, 43998, {{16002, 0xbf0e7b69}, {23335, 0x3b8bcb31}, {30668, 0x39b93a2a}, {38001, 0x3e8de677}, {45334, 0xbea07017}, {52667, 0xbd66cbf7}}},
    {"1.5:1 from -24 dB", 0.f, 0.f, -24.f, 1.5f, 15.f, 250.f, 0, 1, 0, 0x9785030bu, 52000, {{8000, 0xbe3e220d}, {16666, 0xbec1b68f}, {25333, 0xbc12f631}, {34000, 0xbacac7ac}, {42666, 0x36a7be80}, {51333, 0x3a276aee}}},
    {"2:1 from -18 dB", 0.f, 0.f, -18.f, 2.f, 15.f, 250.f, 1, 0, 0, 0x0f645e90u, 60000, {{0, 0xba875b54}, {10000, 0xbe83b84c}, {20000, 0xb9fcdc01}, {30000, 0x39908a9f}, {40000, 0x392bc69e}, {50000, 0x3b7d87ff}}},
    {"3:1 from -15 dB", 0.f, 0.f, -15.f, 3.f, 15.f, 250.f, 1, 1, 0, 0x692c7f56u, 60000, {{0, 0xba055a5a}, {10000, 0xbe9b7189}, {20000, 0xba09e0e7}, {30000, 0x398259c2}, {40000, 0x394762c1}, {50000, 0x3b860c18}}},
    {"6:1 from -12 dB", 0.f, 0.f, -12.f, 6.f, 15.f, 250.f, 1, 1, 0, 0xec3cbf2du, 60000, {{0, 0xba055a5a}, {10000, 0xbeab622f}, {20000, 0xba14ccc4}, {30000, 0x39918464}, {40000, 0x39682738}, {50000, 0x3b920c5d}}},
    {"10:1 from -9 dB", 0.f, 0.f, -9.f, 10.f, 15.f, 250.f, 1, 1, 0, 0x66ee5e69u, 60000, {{0, 0xba04c486}, {10000, 0xbea61f79}, {20000, 0xba1f6ac0}, {30000, 0x39947a8d}, {40000, 0x3974b61d}, {50000, 0x3b9ee373}}},
    {"Hard limiter -6 dB", 0.f, 0.f, -6.f, 50.f, 0.f, 250.f, 0, 0, 0, 0x2dd7ee60u, 43998, {{16002, 0xbf0d46f9}, {23335, 0x3b987423}, {30668, 0x39dd23ff}, {38001, 0x3eaa882e}, {45334, 0xbe9ed74b}, {52667, 0xbd81711d}}},
    {"Gains and times", 5.f, -3.f, -30.f, 4.f, 2.f, 40.f, 1, 0, 0, 0xb514ad40u, 60000, {{0, 0xbb0e5c63}, {10000, 0xbe261576}, {20000, 0xb9a49399}, {30000, 0x3986c08c}, {40000, 0x38e1006f}, {50000, 0x3b3fd911}}},
    {"Slow", -10.f, 6.f, -3.f, 8.f, 100.f, 2000.f, 0, 1, 0, 0x70b5eb4du, 60000, {{0, 0xba04e43d}, {10000, 0xbe5fc86a}, {20000, 0xb9e9079d}, {30000, 0x3926dd68}, {40000, 0x38f05079}, {50000, 0x3b4659e9}}},
    {"Extremes", 12.f, 0.f, -60.f, 20.f, 500.f, 5000.f, 1, 1, 0, 0x084651cfu, 60000, {{0, 0xba9ba370}, {10000, 0xbfa108fa}, {20000, 0xbb27b515}, {30000, 0x3a637095}, {40000, 0x3a1ec21b}, {50000, 0x3c6c23e0}}},
    {"Bypass", 0.f, 0.f, -20.f, 3.f, 5.f, 50.f, 1, 0, 1, 0x2bd1eda0u, 0, {}},
};
static const std::vector<Case> k48000 = {
    {"Default", 0.f, 0.f, 0.f, 1.f, 15.f, 250.f, 1, 0, 0, 0x2bd1eda0u, 0, {}},
    {"Soft Limiter", 0.f, 0.f, -6.f, 25.f, 0.f, 250.f, 0, 1, 0, 0x79688eb1u, 43998, {{16002, 0xbf0e7b69}, {23335, 0x3b8b515f}, {30668, 0x39b79243}, {38001, 0x3e8c3d6f}, {45334, 0xbea06ee3}, {52667, 0xbd65b71a}}},
    {"1.5:1 from -24 dB", 0.f, 0.f, -24.f, 1.5f, 15.f, 250.f, 0, 1, 0, 0x92cc5bc4u, 52000, {{8000, 0xbe3e22de}, {16666, 0xbec4da9a}, {25333, 0xbc1399dd}, {34000, 0xbac944e6}, {42666, 0x36a7ff08}, {51333, 0x3a27b45d}}},
    {"2:1 from -18 dB", 0.f, 0.f, -18.f, 2.f, 15.f, 250.f, 1, 0, 0, 0x030e283au, 60000, {{0, 0xba875b54}, {10000, 0xbe8546cc}, {20000, 0xb9fd0608}, {30000, 0x398dfc4c}, {40000, 0x392beb96}, {50000, 0x3b7cdbf9}}},
    {"3:1 from -15 dB", 0.f, 0.f, -15.f, 3.f, 15.f, 250.f, 1, 1, 0, 0xc6876274u, 60000, {{0, 0xba055a5a}, {10000, 0xbe9b5fdf}, {20000, 0xba0b465f}, {30000, 0x39814a4a}, {40000, 0x394792a6}, {50000, 0x3b870080}}},
    {"6:1 from -12 dB", 0.f, 0.f, -12.f, 6.f, 15.f, 250.f, 1, 1, 0, 0xa6868ec8u, 60000, {{0, 0xba055a5a}, {10000, 0xbeaa5c0f}, {20000, 0xba167eb3}, {30000, 0x399024b8}, {40000, 0x3967e57c}, {50000, 0x3b934b4a}}},
    {"10:1 from -9 dB", 0.f, 0.f, -9.f, 10.f, 15.f, 250.f, 1, 1, 0, 0xc42d5349u, 60000, {{0, 0xba04c486}, {10000, 0xbea43ec8}, {20000, 0xba20676a}, {30000, 0x399321fd}, {40000, 0x3971945a}, {50000, 0x3b9fabb6}}},
    {"Hard limiter -6 dB", 0.f, 0.f, -6.f, 50.f, 0.f, 250.f, 0, 0, 0, 0x43764d89u, 43998, {{16002, 0xbf0d696b}, {23335, 0x3b97110f}, {30668, 0x39da44db}, {38001, 0x3ea8d5bc}, {45334, 0xbe9edff0}, {52667, 0xbd800119}}},
    {"Gains and times", 5.f, -3.f, -30.f, 4.f, 2.f, 40.f, 1, 0, 0, 0x31336f89u, 60000, {{0, 0xbb0e5962}, {10000, 0xbe2617ca}, {20000, 0xb9a49219}, {30000, 0x3984a169}, {40000, 0x38e0fd3b}, {50000, 0x3b3dedd6}}},
    {"Slow", -10.f, 6.f, -3.f, 8.f, 100.f, 2000.f, 0, 1, 0, 0x92c3cff1u, 60000, {{0, 0xba04e482}, {10000, 0xbe62f031}, {20000, 0xb9ecc4a9}, {30000, 0x392904c2}, {40000, 0x38f275c4}, {50000, 0x3b477ae5}}},
    {"Extremes", 12.f, 0.f, -60.f, 20.f, 500.f, 5000.f, 1, 1, 0, 0x13e53a3du, 60000, {{0, 0xba9ba33d}, {10000, 0xbf9e4bd3}, {20000, 0xbb269bbf}, {30000, 0x3a6439ec}, {40000, 0x3a20249b}, {50000, 0x3c7080cb}}},
    {"Bypass", 0.f, 0.f, -20.f, 3.f, 5.f, 50.f, 1, 0, 1, 0x2bd1eda0u, 0, {}},
};
// a change of settings in the middle (at frame 15360): 2:1 from -24 dB, then 6:1 from -30 dB with other gains and times
struct SwitchCase {
    int rate;
    uint32_t hash;
    int changed;
    std::vector<Sample> samples;
};
static const SwitchCase kSwitch[2] = {
    {44100, 0x75b928fau, 60000, {{0, 0xbaac6094}, {10000, 0xbe68b037}, {20000, 0xb9e06459}, {30000, 0x396f10cf}, {40000, 0x3771ef1f}, {50000, 0x39a3b7fa}}},
    {48000, 0x4000f62cu, 60000, {{0, 0xbaac6094}, {10000, 0xbe6db910}, {20000, 0xb9e08468}, {30000, 0x396b75b3}, {40000, 0x3772692b}, {50000, 0x39a331cf}}},
};

static bool matches(const Case& c, const std::vector<float>& l, const std::vector<float>& r, const std::vector<float>& l0, const std::vector<float>& r0, std::string* why)
{
    char b[160];
    if (fnvInter(l, r) != c.hash) {
        std::snprintf(b, sizeof b, "hash %08x, expected %08x", fnvInter(l, r), c.hash);
        *why = b;
        return false;
    }
    int changed = 0;
    for (size_t k = 0; k < l.size(); ++k) changed += (bitsOf(l[k]) != bitsOf(l0[k])) + (bitsOf(r[k]) != bitsOf(r0[k]));
    if (changed != c.changed) {
        *why = "changed samples " + std::to_string(changed) + ", expected " + std::to_string(c.changed);
        return false;
    }
    for (const Sample& s : c.samples) {
        const float f = (s.index & 1) ? r[(size_t)(s.index / 2)] : l[(size_t)(s.index / 2)];
        if (bitsOf(f) != s.bits) {
            std::snprintf(b, sizeof b, "sample %d is %08x, expected %08x", s.index, bitsOf(f), s.bits);
            *why = b;
            return false;
        }
    }
    return true;
}

static vegcomp::Settings settingsOf(float ig, float og, float t, float a, float at, float rl, int ag, int sm, int by)
{
    vegcomp::Settings s;
    s.inputGain = ig;
    s.outputGain = og;
    s.threshold = t;
    s.amount = a;
    s.attack = at;
    s.release = rl;
    s.autoGain = ag;
    s.smooth = sm;
    s.bypass = by;
    return s;
}
static vegcomp::Settings settingsOf(const Case& c) { return settingsOf(c.inGain, c.outGain, c.threshold, c.amount, c.attack, c.release, c.autoGain, c.smooth, c.bypass); }

static void testKernel(const std::vector<Case>& cases, int rate)
{
    std::vector<float> l0, r0;
    makeInput(l0, r0);
    for (const Case& c : cases) {
        vegcomp::Comp comp;
        comp.prepare(settingsOf(c), rate, false);
        std::vector<float> l = l0, r = r0;
        for (int at = 0; at < kFrames; at += 480) comp.process(&l[(size_t)at], &r[(size_t)at], std::min(480, kFrames - at));
        std::string why;
        check(matches(c, l, r, l0, r0, &why), strfmt("compressor %s at %d Hz: all 60000 samples as the DLL's", c.name, rate), why);
    }
}

static std::unique_ptr<AudioPlugin> make(const std::string& format, const std::string& uid, double rate)
{
    AudioPluginDesc d;
    d.format = format;
    d.uid = uid;
    std::string err;
    std::unique_ptr<AudioPlugin> p = createAudioPlugin(d, &err);
    if (p) p->activate(rate, 512);
    return p;
}

static void testPlugin(const std::vector<Case>& cases, int rate)
{
    const std::string tag = strfmt(" at %d Hz", rate);
    std::vector<float> l0, r0;
    makeInput(l0, r0);
    for (const Case& c : cases) {
        auto p = make("BUILTIN", "reno.trkcomp", rate);
        if (!p) { check(false, "the built-in compressor is made" + tag); return; }
        p->loadState(vegcomp::encode(settingsOf(c)));
        std::vector<float> l = l0, r = r0;
        for (int at = 0; at < kFrames; at += 512) p->process(&l[(size_t)at], &r[(size_t)at], std::min(512, kFrames - at), at, true);
        std::string why;
        check(matches(c, l, r, l0, r0, &why), strfmt("plug-in %s%s: the DLL's samples", c.name, tag.c_str()), why);
    }
    // settings changed while it runs (the running gain stays): by loading another saved form between two blocks
    for (const SwitchCase& sc : kSwitch) {
        if (sc.rate != rate) continue;
        auto p = make("BUILTIN", "reno.trkcomp", rate);
        p->loadState(vegcomp::encode(settingsOf(0, 0, -24, 2, 15, 250, 1, 0, 0)));
        std::vector<float> l = l0, r = r0;
        for (int at = 0; at < kFrames; at += 512) {
            if (at == 15360) p->loadState(vegcomp::encode(settingsOf(4, -2, -30, 6, 3, 100, 0, 1, 0)));
            p->process(&l[(size_t)at], &r[(size_t)at], std::min(512, kFrames - at), at, true);
        }
        Case c = {"switch", 0, 0, 0, 0, 0, 0, 0, 0, 0, sc.hash, sc.changed, sc.samples};
        std::string why;
        check(matches(c, l, r, l0, r0, &why), "a change of settings in the middle keeps the compressor's running gain" + tag, why);
    }
    // a jump in time (seek) restarts it: the same input again from the start gives the same output
    {
        auto p = make("BUILTIN", "reno.trkcomp", rate);
        p->loadState(vegcomp::encode(settingsOf(0, 0, -24, 2, 15, 250, 1, 0, 0)));
        std::vector<float> l = l0, r = r0;
        p->process(l.data(), r.data(), 2048, 0, true);
        std::vector<float> l2 = l0, r2 = r0;
        p->process(l2.data(), r2.data(), 2048, 0, true);      // position 0 again: a jump back
        bool same = true;
        for (int k = 0; k < 2048; ++k) same = same && bitsOf(l[(size_t)k]) == bitsOf(l2[(size_t)k]) && bitsOf(r[(size_t)k]) == bitsOf(r2[(size_t)k]);
        check(same, "going back to the start restarts the compressor" + tag);
    }
}

static void testSettings()
{
    vegcomp::Settings d;
    const std::string st = vegcomp::encode(d);
    const std::string raw = base64Decode(st.substr(2));
    check(raw.size() == 60 && (uint8_t)raw[0] == 0x3c && std::memcmp(raw.data() + 4, vegcomp::persistGuid(), 16) == 0, "the saved form is 60 bytes: length 0x3c, the plug-in's GUID, 40 bytes of settings");
    uint32_t amountBits, attackBits, releaseBits;
    std::memcpy(&amountBits, raw.data() + 20 + 12, 4);
    std::memcpy(&attackBits, raw.data() + 20 + 16, 4);
    std::memcpy(&releaseBits, raw.data() + 20 + 20, 4);
    int32_t autoGain;
    std::memcpy(&autoGain, raw.data() + 20 + 24, 4);
    check(amountBits == 0x3f800000u && attackBits == 0x41700000u && releaseBits == 0x437a0000u && autoGain == 1, "  the defaults: amount 1, attack 15 ms, release 250 ms, auto gain compensation on");
    vegcomp::Settings s;
    check(vegcomp::decode(st, &s) && vegcomp::encode(s) == st, "  and reads back to the same bytes");
    check(vegcomp::presets().size() == 9, "nine presets (the two defaults and seven more)");
    check(std::string(vegcomp::presets()[2].name) == "Soft Limiter" && vegcomp::presets()[2].s.threshold == -6.f && vegcomp::presets()[2].s.amount == 25.f && vegcomp::presets()[2].s.attack == 0.f, "  Soft Limiter: -6 dB, 25:1, attack 0 ms");
    check(std::string(vegcomp::presets()[8].name) == "Limit levels to -6 dB (hard limiter)" && vegcomp::presets()[8].s.amount == 50.f, "  Limit levels to -6 dB (hard limiter): 50:1");
    vegcomp::Settings w = settingsOf(99, -99, 5, 0, -3, 9999, 7, 7, 7);
    vegcomp::sanitize(w);
    check(w.inputGain == 25.f && w.outputGain == -60.f && w.threshold == 0.f && w.amount == 1.f && w.attack == 0.f && w.release == 5000.f && w.autoGain == 1 && w.smooth == 1 && w.bypass == 1,
          "out of range values are limited to the dialog's ranges");
    check(!vegcomp::decode("S:AAAA", &s) && !vegcomp::decode("garbage", &s), "a short or foreign form is refused");

    auto p = make("BUILTIN", "reno.trkcomp", 48000);
    check(p != nullptr, "the built-in Track Compressor is made");
    if (!p) return;
    check(p->paramCount() == 8 && p->paramName(0) == "Input Gain" && p->paramName(1) == "Output Gain" && p->paramName(2) == "Threshold" && p->paramName(3) == "Amount" && p->paramName(4) == "Attack Time" &&
              p->paramName(5) == "Release Time" && p->paramName(6) == "Automatic Gain Compensation" && p->paramName(7) == "Smooth Saturation",
          "eight settings");
    double lo = 0, hi = 0;
    check(p->paramRange(0, &lo, &hi) && lo == -60 && hi == 25, "input gain -60..25 dB");
    check(p->paramRange(2, &lo, &hi) && lo == -60 && hi == 0, "threshold -60..0 dB");
    check(p->paramRange(3, &lo, &hi) && lo == 1 && hi == 50, "amount 1..50");
    check(p->paramRange(5, &lo, &hi) && lo == 0 && hi == 5000, "release 0..5000 ms");
    check(p->saveState() == st, "the default plug-in saves as the default form");
    check(p->programCount() == 9 && p->programName(0) == "(Default)" && p->programName(2) == "Soft Limiter", "VEGAS's nine presets");
    check(p->setProgram(6) && p->saveState() == vegcomp::encode(vegcomp::presets()[6].s) && p->currentProgram() == 6, "preset 6:1 compression starting at -12 dB");
    check(p->paramText(2) == "-12.0 dB", "  threshold -12.0 dB: " + p->paramText(2));
    p->loadState(vegcomp::encode(settingsOf(-60, 3, -20, 3, 5, 50, 1, 0, 1)));
    check(p->paramText(0) == "-Inf." && p->saveState() == vegcomp::encode(settingsOf(-60, 3, -20, 3, 5, 50, 1, 0, 1)), "-Inf. input gain and a bypass flag in a loaded form are kept");
    check(p->loadState("reno-builtin-audio 1\nThreshold=-20\n") && p->saveState() == st, "settings of an older Reno load as the defaults");
    check(!p->loadState("garbage"), "unknown text is refused");
}

static void testDxFallback()
{
    const std::string dx = "dshow:{23C9F225-40EC-11D2-9D36-00C04F8EDC1E}";
    check(builtinUidForDx(dx) == "reno.trkcomp", "the DirectX Track Compressor stands for the built-in one");
    check(dxUidForBuiltin("reno.trkcomp") == dx, "the built-in one is saved as the DirectX Track Compressor");
    AudioPluginDesc d;
    d.format = "DX";
    d.uid = dx;
    d.name = "VEGAS Track Compressor";
    std::string err;
    auto p = createAudioPlugin(d, &err);      // no DirectX here (or no VEGAS): Reno's version
    check(p != nullptr, "a DX Track Compressor slot makes a plug-in", err);
    if (p) {
        check(p->desc.format == "DX" && p->desc.uid == dx, "  that keeps the slot's description");
        check(builtinPageUid(p->desc) == "reno.trkcomp" || p->desc.vendor != kBuiltinAudioVendor, "  and shows the Track Compressor page");
    }
}

int main()
{
    testKernel(k44100, 44100);
    testKernel(k48000, 48000);
    testPlugin(k44100, 44100);
    testPlugin(k48000, 48000);
    testSettings();
    testDxFallback();
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
