// VEGAS's Volume effect (sffrgpnv_x64.dll) as Reno's built-in "reno.trkvolume" reproduces it: the processing against
// samples computed with the DLL's own code (run on Linux in a loader that maps the DLL), its saved form against one from a
// real VEGAS project, presets, settings of older Reno versions, the DirectX fallback and the way it is saved as VEGAS's.
//
//   vegasfx_volume_test
//
// The DLL is not needed to run this: the expected values are stored here.
#include "../../src/audiofx/audioplugin.h"
#include "../../src/audiofx/vegas_trkfx.h"
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

// the input of the stored results: pseudo-random but exact (integers over 32768), stereo
static float input(int k)
{
    uint32_t h = (uint32_t)k * 2654435761u;
    h ^= h >> 15;
    return (float)((int)(h & 0xffff) - 32768) / 32768.f;
}
static uint32_t bitsOf(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
static uint32_t fnv(const std::vector<float>& v)
{
    uint32_t h = 2166136261u;
    for (float f : v) {
        const uint32_t b = bitsOf(f);
        for (int i = 0; i < 4; ++i) {
            h ^= (b >> (8 * i)) & 0xff;
            h *= 16777619u;
        }
    }
    return h;
}

struct Expect {
    float gain;
    uint32_t hash;           // FNV-1a over the 8192 output floats (4096 frames, interleaved)
    uint32_t first[4], last[4];
};
// computed with the DLL's volume routine (0x180005540) for the input above
static const Expect kExpect[] = {
    {0.f, 0x23fd3345u, {0x80000000, 0x80000000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x80000000, 0x80000000}},
    {0.25f, 0x94e5939bu, {0xbe800000, 0xbde88400, 0x3cbbf000, 0x3e30be00}, {0x3e2b1800, 0x3d5f9000, 0xbddbc000, 0xbe709c00}},
    {0.5f, 0x1e223305u, {0xbf000000, 0xbe688400, 0x3d3bf000, 0x3eb0be00}, {0x3eab1800, 0x3ddf9000, 0xbe5bc000, 0xbef09c00}},
    {1.f, 0x38aba519u, {0xbf800000, 0xbee88400, 0x3dbbf000, 0x3f30be00}, {0x3f2b1800, 0x3e5f9000, 0xbedbc000, 0xbf709c00}},
    {1.4142135f, 0xfafeff80u, {0xbfb504f3, 0xbf2469d3, 0x3e04e452, 0x3f79f381}, {0x3f71f68e, 0x3e9e1522, 0xbf1b6300, 0xbfaa22f3}},
    {2.f, 0x5e975a07u, {0xc0000000, 0xbf688400, 0x3e3bf000, 0x3fb0be00}, {0x3fab1800, 0x3edf9000, 0xbf5bc000, 0xbff09c00}},
    {37.5f, 0x44dae733u, {0xc2160000, 0xc1883d58, 0x405c3d40, 0x41cf1ea8}, {0x41c88020, 0x4102fe60, 0xc180c280, 0xc20cfb68}},
};

static bool sameAsExpected(const Expect& e, const std::vector<float>& inter, std::string* why)
{
    char b[160];
    if (fnv(inter) != e.hash) {
        std::snprintf(b, sizeof b, "hash %08x, expected %08x", fnv(inter), e.hash);
        *why = b;
        return false;
    }
    for (int i = 0; i < 4; ++i)
        if (bitsOf(inter[(size_t)i]) != e.first[i] || bitsOf(inter[inter.size() - 4 + (size_t)i]) != e.last[i]) {
            *why = "first/last samples differ";
            return false;
        }
    return true;
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

// runs the 4096 frames through a plug-in in blocks of 'block', returns them interleaved
static std::vector<float> runPlugin(AudioPlugin& p, int block)
{
    const int n = 4096;
    std::vector<float> l(n), r(n);
    for (int k = 0; k < n; ++k) {
        l[(size_t)k] = input(2 * k);
        r[(size_t)k] = input(2 * k + 1);
    }
    for (int at = 0; at < n; at += block) {
        const int m = std::min(block, n - at);
        p.process(&l[(size_t)at], &r[(size_t)at], m, at, true);
    }
    std::vector<float> out((size_t)n * 2);
    for (int k = 0; k < n; ++k) {
        out[(size_t)(2 * k)] = l[(size_t)k];
        out[(size_t)(2 * k + 1)] = r[(size_t)k];
    }
    return out;
}

static void testKernel()
{
    for (const Expect& e : kExpect) {
        const int n = 4096;
        std::vector<float> l(n), r(n);
        for (int k = 0; k < n; ++k) {
            l[(size_t)k] = input(2 * k);
            r[(size_t)k] = input(2 * k + 1);
        }
        vegforge::applyVolume(l.data(), r.data(), n, e.gain);
        std::vector<float> inter((size_t)n * 2);
        for (int k = 0; k < n; ++k) {
            inter[(size_t)(2 * k)] = l[(size_t)k];
            inter[(size_t)(2 * k + 1)] = r[(size_t)k];
        }
        std::string why;
        check(sameAsExpected(e, inter, &why), strfmt("Volume %g: all 8192 samples as the DLL's", (double)e.gain), why);
    }
}

static void testPlugin(double rate)
{
    const std::string tag = strfmt(" at %d Hz", (int)rate);
    auto p = make("BUILTIN", "reno.trkvolume", rate);
    check(p != nullptr, "the built-in Volume is made" + tag);
    if (!p) return;
    check(p->paramCount() == 1 && p->paramName(0) == "Volume", "one setting, Volume" + tag);
    double lo = -1, hi = -1;
    check(p->paramRange(0, &lo, &hi) && lo == 0.0 && hi == 2.0, "its range is 0 to 2 (the DLL's descriptor)" + tag);
    check(std::fabs(p->paramGet(0) - 0.5f) < 1e-6f, "the default is 1 (half way)" + tag);
    check(p->programCount() == 2 && p->programName(0) == "(Default)" && p->programName(1) == "Default all parameters", "VEGAS's two presets" + tag);
    for (const Expect& e : kExpect) {
        auto q = make("BUILTIN", "reno.trkvolume", rate);
        check(q->loadState(vegforge::encode(vegforge::volume(), e.gain)), "state with gain loads" + tag);
        const std::vector<float> out = runPlugin(*q, e.gain == 2.f ? 100 : 512);
        std::string why;
        check(sameAsExpected(e, out, &why), strfmt("plug-in with gain %g gives the DLL's samples%s", (double)e.gain, tag.c_str()), why);
    }
    // automation moves the gain: 0..1 is 0..2
    p->paramSet(0, 0.25f);
    const std::string st = p->saveState();
    float g = -1;
    check(vegforge::decode(vegforge::volume(), st, &g) && g == 0.5f, "paramSet(0.25) is a gain of 0.5, saved as VEGAS's" + tag);
    check(p->paramText(0).find("-6.0") == 0, "its text is the dB: " + p->paramText(0) + tag);
    // a gain above the automation range (typed dB in the dialog) stays in the saved form
    check(p->loadState(vegforge::encode(vegforge::volume(), 37.5f)) && p->paramGet(0) == 1.f, "gain 37.5 reads as the top of the range" + tag);
    check(p->saveState() == vegforge::encode(vegforge::volume(), 37.5f), "  and is kept as 37.5 in the saved form" + tag);
    // an older Reno's state loads as the defaults
    p->paramSet(0, 0.1f);
    check(p->loadState("reno-builtin-audio 1\nVolume=0.3\n") && std::fabs(p->paramGet(0) - 0.5f) < 1e-6f, "settings of an older Reno load as the defaults" + tag);
    check(!p->loadState("garbage"), "unknown text is refused" + tag);
    check(p->setProgram(1) && p->currentProgram() == 1, "a preset can be chosen" + tag);
}

static void testSavedForm()
{
    // "VEGAS Volume" as saved in a real VEGAS Pro 18 project (gain 1.58489, 0x3fcaddc8)
    const unsigned char real[24] = {0x18, 0x00, 0x00, 0x00, 0x9c, 0x6b, 0x95, 0xc6, 0x70, 0x44, 0xb2, 0x45,
                                    0x8a, 0x7c, 0xe4, 0xf9, 0x8a, 0x39, 0x43, 0xb9, 0xc8, 0xdd, 0xca, 0x3f};
    const std::string st = "S:" + base64Encode(real, sizeof real);
    float g = 0;
    check(vegforge::decode(vegforge::volume(), st, &g), "the saved form from a VEGAS project is read");
    check(bitsOf(g) == 0x3fcaddc8u, strfmt("  its gain is %.5f (a 1.585 = +4 dB)", (double)g));
    check(vegforge::encode(vegforge::volume(), g) == st, "  and writes back to the same bytes");
    check(!vegforge::decode(vegforge::pan(), st, &g), "a Volume form is not taken for a Pan one (other GUID)");
    check(vegforge::limit(vegforge::volume(), 5000.f) == 2000.f && vegforge::limit(vegforge::volume(), -1.f) == 0.f, "typed values are limited to 0..2000");
    auto p = make("BUILTIN", "reno.trkvolume", 48000);
    check(p->loadState(st) && p->saveState() == st, "the plug-in keeps it byte for byte");
}

static void testDxFallback()
{
    const std::string dx = "dshow:{EE38CA88-D78E-4BFB-B05E-577892730C83}";
    check(builtinUidForDx(dx) == "reno.trkvolume", "the DirectX Volume stands for the built-in one");
    check(builtinUidForDx("dshow:{ee38ca88-d78e-4bfb-b05e-577892730c83}") == "reno.trkvolume", "  in any case");
    check(dxUidForBuiltin("reno.trkvolume") == dx, "the built-in one is saved as the DirectX Volume");
    check(builtinUidForDx("dshow:{39224540-6F92-11D0-AEBC-00A0C9053912}") == "reno.distortion", "Distortion's mapping is unchanged");
    AudioPluginDesc d;
    d.format = "DX";
    d.uid = dx;
    d.name = "VEGAS Volume";
    std::string err;
    auto p = createAudioPlugin(d, &err);      // no DirectX here (or no VEGAS): Reno's version
    check(p != nullptr, "a DX Volume slot makes a plug-in", err);
    if (p) {
        check(p->desc.format == "DX" && p->desc.uid == dx, "  that keeps the slot's description");
        check(builtinPageUid(p->desc) == "reno.trkvolume" || p->desc.vendor != kBuiltinAudioVendor, "  and shows the Volume page");
    }
}

int main()
{
    testKernel();
    testPlugin(44100);
    testPlugin(48000);
    testSavedForm();
    testDxFallback();
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
