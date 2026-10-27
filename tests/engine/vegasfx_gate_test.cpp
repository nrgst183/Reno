// VEGAS's Track Noise Gate (sftrkfx1_x64.dll, CSfTNoisg) as Reno's built-in "reno.trkgate" reproduces it: the gate's code
// against samples computed with the DLL's own code (run on Linux in a loader that maps the DLL) at 44100 and 48000 Hz,
// the saved form against one from a real VEGAS project, the presets, a change of settings while running, settings of
// older Reno versions, the DirectX fallback and the way it is saved as VEGAS's.
//
//   vegasfx_gate_test
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
    int attack, release, threshold, bypass;
    uint32_t hash;           // FNV-1a over the 60000 output floats
    int changed;             // how many of them differ from the input
    std::vector<Sample> samples;
};

// computed with the DLL's prepare (0x18002f480) and per-frame function (0x1800308e0) for the input above
static const std::vector<Case> k44100 = {
    {"Default", 3, 100, -800, 0, 0x2bd1eda0u, 0, {}},
    {"Strong beats only", 1, 300, -60, 0, 0xa637c9b6u, 51202, {{0, 0xba013b0c}, {8533, 0x80000000}, {17067, 0xbf2cfffa}, {29857, 0x39a3e84f}, {38390, 0xbd8289c4}, {51466, 0x3b9ae3e2}}},
    {"Long decay time", 10, 1000, -380, 0, 0x302426b5u, 18446, {{0, 0xba013b0c}, {3074, 0x00000000}, {14694, 0xb9064ade}, {24129, 0x3e82390d}, {32365, 0x3c12cdba}, {51787, 0xbc0e97a9}}},
    {"Noise gate 1", 10, 500, -600, 0, 0xab28f759u, 5924, {{0, 0xba013b0c}, {987, 0x2f8a7887}, {1974, 0x00000000}, {2962, 0x80000000}, {3949, 0x80000000}, {4936, 0xbb70e723}}},
    {"Noise gate 2", 1, 500, -320, 0, 0x9713faf8u, 25220, {{0, 0xba013b0c}, {4203, 0x00000000}, {8406, 0x3e69f773}, {24122, 0x3e97881a}, {33749, 0x3c5d8adf}, {43572, 0x38ad3edb}}},
    {"Slow attack", 300, 1000, -400, 0, 0x7302b6bdu, 59508, {{0, 0xba013b0c}, {9918, 0x3d747216}, {19836, 0x3d1d295d}, {29754, 0xb99c2805}, {39672, 0xbe0e99a0}, {49590, 0xbaa9c5b9}}},
    {"Fast", 0, 1, -250, 0, 0xc51470ddu, 28188, {{0, 0xba013b0c}, {4698, 0x80000000}, {14648, 0x37d91625}, {30546, 0x37ad5e1a}, {35244, 0x00000000}, {49950, 0x38fed57d}}},
    {"Bypass", 2, 20, -250, 1, 0x2bd1eda0u, 0, {}},
};
static const std::vector<Case> k48000 = {
    {"Default", 3, 100, -800, 0, 0x2bd1eda0u, 0, {}},
    {"Strong beats only", 1, 300, -60, 0, 0x3f476beau, 50654, {{0, 0xba013b0c}, {8442, 0x80000000}, {16884, 0xbf0c67b9}, {29563, 0xb99488f8}, {38005, 0xbe051d20}, {51557, 0xbc0aafb6}}},
    {"Long decay time", 10, 1000, -380, 0, 0x9ebeb51bu, 16174, {{0, 0xba013b0c}, {2695, 0x80000000}, {5391, 0xbca10db6}, {23529, 0x3bec6cc8}, {31914, 0xb93269f8}, {44232, 0xbed7f414}}},
    {"Noise gate 1", 10, 500, -600, 0, 0x0cbf0f7au, 8078, {{0, 0xba013b0c}, {1346, 0xaacb3d3c}, {2692, 0x00000000}, {4039, 0x378c01eb}, {5385, 0x3c96df7e}, {16485, 0x3ebfbeff}}},
    {"Noise gate 2", 1, 500, -320, 0, 0x662d43dau, 26868, {{0, 0xba013b0c}, {4478, 0x00000000}, {8956, 0xbd249fe3}, {24424, 0x3dbdd1fa}, {35114, 0x3c8fe92e}, {50512, 0xb7e5bd40}}},
    {"Slow attack", 300, 1000, -400, 0, 0x413c9764u, 50966, {{0, 0xba013b0c}, {8494, 0x3b93787e}, {16988, 0xbea22770}, {25483, 0xbdea8845}, {43011, 0xb936c38c}, {51505, 0x3b2bbe78}}},
    {"Fast", 0, 1, -250, 0, 0x469fa227u, 29928, {{0, 0xba013b0c}, {4988, 0x00000000}, {14588, 0xb190f921}, {29760, 0x342ba092}, {34748, 0x80000000}, {45148, 0x3e045999}}},
    {"Bypass", 2, 20, -250, 1, 0x2bd1eda0u, 0, {}},
};
// a change of settings in the middle (at frame 15360): Noise gate 1, then Slow attack
struct SwitchCase {
    int rate;
    uint32_t hash;
    int changed;
    std::vector<Sample> samples;
};
static const SwitchCase kSwitch[2] = {
    {44100, 0xf4a124a3u, 34612, {{0, 0xba013b0c}, {5768, 0xb7e66662}, {36689, 0xbbba27e1}, {42458, 0xb80f3acc}, {48226, 0xbbbe1d43}, {53995, 0x3d57ac9f}}},
    {48000, 0x1c383912u, 36878, {{0, 0xba013b0c}, {15900, 0xb88872b4}, {35414, 0xba76aa70}, {41561, 0xb761a663}, {47707, 0x3e7893c4}, {53853, 0x3cfcc714}}},
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

static void testKernel(const std::vector<Case>& cases, int rate)
{
    std::vector<float> l0, r0;
    makeInput(l0, r0);
    for (const Case& c : cases) {
        veggate::Gate g;
        veggate::Settings s;
        s.attack = c.attack;
        s.release = c.release;
        s.threshold = c.threshold;
        s.bypass = c.bypass;
        g.prepare(s, rate, false);
        g.start(rate);
        std::vector<float> l = l0, r = r0;
        if (!g.bypass)
            for (int k = 0; k < kFrames; ++k) {
                float f[2] = {l[(size_t)k], r[(size_t)k]};
                g.frame(f, 2);
                l[(size_t)k] = f[0];
                r[(size_t)k] = f[1];
            }
        std::string why;
        check(matches(c, l, r, l0, r0, &why), strfmt("gate %s at %d Hz: all 60000 samples as the DLL's", c.name, rate), why);
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

static veggate::Settings settingsOf(int a, int r, int t, int b)
{
    veggate::Settings s;
    s.attack = a;
    s.release = r;
    s.threshold = t;
    s.bypass = b;
    return s;
}

static void testPlugin(const std::vector<Case>& cases, int rate)
{
    const std::string tag = strfmt(" at %d Hz", rate);
    std::vector<float> l0, r0;
    makeInput(l0, r0);
    for (const Case& c : cases) {
        auto p = make("BUILTIN", "reno.trkgate", rate);
        if (!p) { check(false, "the built-in gate is made" + tag); return; }
        p->loadState(veggate::encode(settingsOf(c.attack, c.release, c.threshold, c.bypass)));
        std::vector<float> l = l0, r = r0;
        for (int at = 0; at < kFrames; at += 512) p->process(&l[(size_t)at], &r[(size_t)at], std::min(512, kFrames - at), at, true);
        std::string why;
        check(matches(c, l, r, l0, r0, &why), strfmt("plug-in %s%s: the DLL's samples", c.name, tag.c_str()), why);
    }
    // settings changed while it runs (the gate keeps its state): by loading another saved form between two blocks
    for (const SwitchCase& sc : kSwitch) {
        if (sc.rate != rate) continue;
        auto p = make("BUILTIN", "reno.trkgate", rate);
        p->loadState(veggate::encode(settingsOf(10, 500, -600, 0)));
        std::vector<float> l = l0, r = r0;
        for (int at = 0; at < kFrames; at += 512) {
            if (at == 15360) p->loadState(veggate::encode(settingsOf(300, 1000, -400, 0)));
            p->process(&l[(size_t)at], &r[(size_t)at], std::min(512, kFrames - at), at, true);
        }
        Case c = {"switch", 0, 0, 0, 0, sc.hash, sc.changed, sc.samples};
        std::string why;
        check(matches(c, l, r, l0, r0, &why), "a change of settings in the middle keeps the gate's state" + tag, why);
    }
    // a jump in time (seek) restarts the gate: the same input again from the start gives the same output
    {
        auto p = make("BUILTIN", "reno.trkgate", rate);
        p->loadState(veggate::encode(settingsOf(1, 300, -60, 0)));
        std::vector<float> l = l0, r = r0;
        p->process(l.data(), r.data(), 2048, 0, true);
        std::vector<float> l2 = l0, r2 = r0;
        p->process(l2.data(), r2.data(), 2048, 0, true);      // position 0 again: a jump back
        bool same = true;
        for (int k = 0; k < 2048; ++k) same = same && bitsOf(l[(size_t)k]) == bitsOf(l2[(size_t)k]) && bitsOf(r[(size_t)k]) == bitsOf(r2[(size_t)k]);
        check(same, "going back to the start restarts the gate" + tag);
    }
}

static void testSettings()
{
    // the saved form of a Sony / VEGAS Track Noise Gate in VEGAS Pro projects (the default settings)
    const unsigned char real[36] = {0x24, 0x00, 0x00, 0x00, 0x9b, 0xf9, 0x00, 0xc4, 0x6e, 0x0c, 0x4c, 0x44, 0x8f, 0x20, 0x98, 0xdb, 0x21, 0x6b, 0xe5, 0x6e,
                                    0x03, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0xe0, 0xfc, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00};
    const std::string st = "S:" + base64Encode(real, sizeof real);
    veggate::Settings s;
    check(veggate::decode(st, &s), "the saved form from a VEGAS project is read");
    check(s.attack == 3 && s.release == 100 && s.threshold == -800 && s.bypass == 0, "  attack 3 ms, release 100 ms, threshold -Inf., not bypassed");
    check(veggate::encode(s) == st, "  and writes back to the same bytes");
    check(veggate::presets().size() == 7, "seven presets (the two defaults and five more)");
    check(std::string(veggate::presets()[2].name) == "Strong beats only" && veggate::presets()[2].s.threshold == -60 && veggate::presets()[2].s.release == 300, "  Strong beats only: 1 ms, 300 ms, -6.0 dB");
    check(std::string(veggate::presets()[6].name) == "Slow attack" && veggate::presets()[6].s.attack == 300 && veggate::presets()[6].s.threshold == -400, "  Slow attack: 300 ms, 1000 ms, -40.0 dB");
    veggate::Settings w = settingsOf(9999, -5, 55, 7);
    veggate::sanitize(w);
    check(w.attack == 500 && w.release == 1 && w.threshold == 0 && w.bypass == 1, "out of range values are limited to the dialog's ranges");
    check(!veggate::decode("S:AAAA", &s) && !veggate::decode("garbage", &s), "a short or foreign form is refused");

    auto p = make("BUILTIN", "reno.trkgate", 48000);
    check(p != nullptr, "the built-in Noise Gate is made");
    if (!p) return;
    check(p->paramCount() == 4 && p->paramName(0) == "Threshold Level" && p->paramName(1) == "Attack Time" && p->paramName(2) == "Release Time" && p->paramName(3) == "Bypass",
          "four settings, named as the plug-in names them");
    double lo = 0, hi = 0;
    check(p->paramRange(0, &lo, &hi) && lo == -80 && hi == 0, "threshold -80..0 dB");
    check(p->paramRange(1, &lo, &hi) && lo == 0 && hi == 500, "attack 0..500 ms");
    check(p->paramRange(2, &lo, &hi) && lo == 1 && hi == 5000, "release 1..5000 ms");
    check(p->paramText(0) == "-Inf." && p->paramText(1) == "3 ms" && p->paramText(2) == "100 ms", "default texts: " + p->paramText(0) + ", " + p->paramText(1) + ", " + p->paramText(2));
    check(p->saveState() == st, "the default plug-in saves as the real default form");
    check(p->programCount() == 7 && p->programName(0) == "(Default)" && p->programName(5) == "Noise gate 2", "VEGAS's seven presets");
    check(p->setProgram(4) && p->saveState() == veggate::encode(settingsOf(10, 500, -600, 0)) && p->currentProgram() == 4, "preset Noise gate 1");
    check(p->paramText(0) == "-60.0 dB", "  threshold -60.0 dB: " + p->paramText(0));
    check(p->loadState("reno-builtin-audio 1\nThreshold=-20\n") && p->saveState() == st, "settings of an older Reno load as the defaults");
    check(!p->loadState("garbage"), "unknown text is refused");
}

static void testDxFallback()
{
    const std::string dx = "dshow:{869419DD-501F-11D3-8CDC-00C04F6B8E4C}";
    check(builtinUidForDx(dx) == "reno.trkgate", "the DirectX Track Noise Gate stands for the built-in one");
    check(dxUidForBuiltin("reno.trkgate") == dx, "the built-in one is saved as the DirectX Track Noise Gate");
    AudioPluginDesc d;
    d.format = "DX";
    d.uid = dx;
    d.name = "VEGAS Track Noise Gate";
    std::string err;
    auto p = createAudioPlugin(d, &err);      // no DirectX here (or no VEGAS): Reno's version
    check(p != nullptr, "a DX Track Noise Gate slot makes a plug-in", err);
    if (p) {
        check(p->desc.format == "DX" && p->desc.uid == dx, "  that keeps the slot's description");
        check(builtinPageUid(p->desc) == "reno.trkgate" || p->desc.vendor != kBuiltinAudioVendor, "  and shows the Noise Gate page");
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
