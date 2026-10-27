// VEGAS's Dither (sftrkfx1_x64.dll, CSfTDither) as Reno's built-in "reno.trkdither" reproduces it: the effect's code against
// samples computed with the DLL's own code (run on Linux in a loader that maps the DLL, with the generator seeded as Reno
// seeds it, 12345, and the sound handed over in blocks of 512 frames) at 44100 and 48000 Hz (the effect does not depend on
// the rate), the saved form, the presets, settings of older Reno versions, the DirectX fallback and the way it is saved as
// VEGAS's.
//
//   vegasfx_dither_test
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
    int depth, type, shape;
    uint32_t hash;           // FNV-1a over the 60000 output floats
    int changed;             // how many of them differ from the input
    std::vector<Sample> samples;
};

// computed with the DLL's generator set-up and its sample function (0x6bd40) for the input above
static const std::vector<Case> kCases = {
    {"Default", 2, 1, 0, 0xe0852395u, 53726, {{0, 0xba080000}, {10106, 0xbe656000}, {20806, 0x3c030000}, {29994, 0xb9800000}, {41016, 0x38c00000}, {50788, 0xbc1e0000}}},
    {"Dither for 8-bit (low sample rate)", 0, 3, 0, 0xd25a5172u, 59966, {{0, 0x00000000}, {9999, 0xbdb00000}, {20008, 0x3c000000}, {30004, 0x00000000}, {40003, 0x00000000}, {50005, 0x00000000}}},
    {"Dither for 8-bit", 0, 4, 1, 0x89a7d566u, 59976, {{0, 0x00000000}, {9999, 0xbdb00000}, {20007, 0x00000000}, {30003, 0x3c000000}, {40003, 0x00000000}, {50004, 0xbc000000}}},
    {"Dither for 16-bit (low sample rate)", 2, 3, 2, 0xc726737eu, 56899, {{0, 0xba080000}, {10065, 0xbbe80000}, {20401, 0x3bb60000}, {29998, 0xb9400000}, {40515, 0x00000000}, {50393, 0x3a180000}}},
    {"Dither for 16-bit", 2, 4, 3, 0xefe70238u, 55269, {{0, 0xba080000}, {10064, 0xbe902400}, {20624, 0xbae00000}, {30002, 0xb9b00000}, {40766, 0x39400000}, {50588, 0xbaf00000}}},
    {"12 bit, none", 1, 0, 0, 0xcec2a30fu, 59325, {{0, 0xba000000}, {9947, 0xbe528000}, {20086, 0x3b600000}, {29985, 0x3a000000}, {40003, 0x00000000}, {50103, 0xbb000000}}},
    {"18 bit, rectangular", 3, 2, 0, 0x22517db5u, 54441, {{0, 0xba040000}, {10302, 0x3eb07100}, {20611, 0xbc12e000}, {29994, 0xb9780000}, {41146, 0xb8c00000}, {50654, 0xba4c0000}}},
    {"20 bit, gaussian, high-pass contour", 4, 5, 1, 0xb308ece1u, 54474, {{0, 0xba030000}, {10308, 0x3dc3d700}, {20685, 0x3bd2d000}, {30044, 0x39a00000}, {41166, 0xb7900000}, {50654, 0xba4d0000}}},
    {"24 bit, gaussian, equal loudness", 5, 5, 2, 0xded7f179u, 54920, {{0, 0xba031800}, {10115, 0x3ede9204}, {20553, 0x3c010380}, {30005, 0xb9a6c000}, {40934, 0xb7c60000}, {50566, 0x3c014200}}},
    {"32 bit, triangular", 6, 3, 0, 0x60a32bd0u, 24984, {{0, 0xba031270}, {5234, 0x3b2b70a2}, {15558, 0xb91657a0}, {30015, 0xb979c2a0}, {41271, 0x38ad4600}, {51421, 0x3a786668}}},
    {"16 bit, half rectangular, high-pass contour", 2, 1, 1, 0xc00b12a5u, 55842, {{0, 0xba080000}, {10025, 0x3ec10400}, {20538, 0x3ba10000}, {30001, 0xb8c00000}, {40681, 0xb8000000}, {50525, 0xbba50000}}},
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

static vegdither::Settings settingsOf(int d, int t, int s)
{
    vegdither::Settings v;
    v.depth = d;
    v.type = t;
    v.shape = s;
    return v;
}

static void testKernel()
{
    std::vector<float> l0, r0;
    makeInput(l0, r0);
    for (const Case& c : kCases) {
        vegdither::Dither d;
        d.prepare(settingsOf(c.depth, c.type, c.shape), 12345u);
        std::vector<float> l = l0, r = r0;
        for (int at = 0; at < kFrames; at += 512) {
            float* ch[2] = {&l[(size_t)at], &r[(size_t)at]};
            d.process(ch, 2, std::min(512, kFrames - at));
        }
        std::string why;
        check(matches(c, l, r, l0, r0, &why), strfmt("dither %s: all 60000 samples as the DLL's", c.name), why);
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

static void testPlugin(int rate)
{
    const std::string tag = strfmt(" at %d Hz", rate);
    std::vector<float> l0, r0;
    makeInput(l0, r0);
    for (const Case& c : kCases) {
        auto p = make("BUILTIN", "reno.trkdither", rate);
        if (!p) { check(false, "the built-in dither is made" + tag); return; }
        p->loadState(vegdither::encode(settingsOf(c.depth, c.type, c.shape)));
        std::vector<float> l = l0, r = r0;
        for (int at = 0; at < kFrames; at += 512) p->process(&l[(size_t)at], &r[(size_t)at], std::min(512, kFrames - at), at, true);
        std::string why;
        check(matches(c, l, r, l0, r0, &why), strfmt("plug-in %s%s: the DLL's samples", c.name, tag.c_str()), why);
    }
    // a render repeats: the same input twice from the start gives the same output
    {
        auto p = make("BUILTIN", "reno.trkdither", rate);
        p->loadState(vegdither::encode(settingsOf(2, 4, 2)));
        std::vector<float> l = l0, r = r0, l2 = l0, r2 = r0;
        p->process(l.data(), r.data(), 2048, 0, true);
        p->process(l2.data(), r2.data(), 2048, 0, true);      // position 0 again: a jump back
        bool same = true;
        for (int k = 0; k < 2048; ++k) same = same && bitsOf(l[(size_t)k]) == bitsOf(l2[(size_t)k]) && bitsOf(r[(size_t)k]) == bitsOf(r2[(size_t)k]);
        check(same, "going back to the start gives the same noise again" + tag);
    }
}

static void testSettings()
{
    vegdither::Settings d;
    const std::string st = vegdither::encode(d);
    const std::string raw = base64Decode(st.substr(2));
    check(raw.size() == 36 && (uint8_t)raw[0] == 0x24 && std::memcmp(raw.data() + 4, vegdither::persistGuid(), 16) == 0, "the saved form is 36 bytes: length 0x24, the plug-in's GUID, 16 bytes of settings");
    vegdither::Settings s;
    check(vegdither::decode(st, &s) && s.depth == 2 && s.type == 1 && s.shape == 0 && vegdither::encode(s) == st, "  the defaults (16 bit, half rectangular, no shaping) read back to the same bytes");
    check(vegdither::presets().size() == 6, "six presets (the two defaults and four more)");
    check(std::string(vegdither::presets()[3].name) == "Dither for 8-bit" && vegdither::presets()[3].s.depth == 0 && vegdither::presets()[3].s.type == 4 && vegdither::presets()[3].s.shape == 1, "  Dither for 8-bit: 8 bit, highpass triangular, high-pass contour");
    vegdither::Settings w = settingsOf(99, -4, 77);
    vegdither::sanitize(w);
    check(w.depth == 6 && w.type == 0 && w.shape == 3, "out of range values are limited");
    check(!vegdither::decode("S:AAAA", &s) && !vegdither::decode("garbage", &s), "a short or foreign form is refused");
    check(std::string(vegdither::depthName(5)) == "24 bit" && std::string(vegdither::typeName(5)) == "Gaussian" && std::string(vegdither::shapeName(2)) == "Equal loudness contour", "the list entries");

    auto p = make("BUILTIN", "reno.trkdither", 48000);
    check(p != nullptr, "the built-in Dither is made");
    if (!p) return;
    check(p->paramCount() == 0, "no automatable settings (all three are lists in its page)");
    check(p->saveState() == st, "the default plug-in saves as the default form");
    check(p->programCount() == 6 && p->programName(0) == "(Default)" && p->programName(5) == "Dither for 16-bit", "VEGAS's six presets");
    check(p->setProgram(2) && p->saveState() == vegdither::encode(settingsOf(0, 3, 0)) && p->currentProgram() == 2, "preset Dither for 8-bit (low sample rate)");
    check(p->loadState(vegdither::encode(settingsOf(4, 2, 1))) && p->saveState() == vegdither::encode(settingsOf(4, 2, 1)), "a saved form loads and saves back");
    check(p->loadState("reno-builtin-audio 1\nx=1\n") && p->saveState() == st, "settings of an older Reno load as the defaults");
    check(!p->loadState("garbage"), "unknown text is refused");
}

static void testDxFallback()
{
    const std::string dx = "dshow:{260DF3E1-AC77-11D2-9E93-00C04F68BE44}";
    check(builtinUidForDx(dx) == "reno.trkdither", "the DirectX Dither stands for the built-in one");
    check(dxUidForBuiltin("reno.trkdither") == dx, "the built-in one is saved as the DirectX Dither");
    AudioPluginDesc d;
    d.format = "DX";
    d.uid = dx;
    d.name = "VEGAS Dither";
    std::string err;
    auto p = createAudioPlugin(d, &err);      // no DirectX here (or no VEGAS): Reno's version
    check(p != nullptr, "a DX Dither slot makes a plug-in", err);
    if (p) {
        check(p->desc.format == "DX" && p->desc.uid == dx, "  that keeps the slot's description");
        check(builtinPageUid(p->desc) == "reno.trkdither" || p->desc.vendor != kBuiltinAudioVendor, "  and shows the Dither page");
    }
}

int main()
{
    testKernel();
    testPlugin(44100);
    testPlugin(48000);
    testSettings();
    testDxFallback();
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
