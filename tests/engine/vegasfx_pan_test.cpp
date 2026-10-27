// VEGAS's Pan effect (sffrgpnv_x64.dll) as Reno's built-in "reno.trkpan" reproduces it: the processing against
// samples computed with the DLL's own code (run on Linux in a loader that maps the DLL), its saved form, presets, settings of older Reno versions, the DirectX fallback and the way it is saved as VEGAS's.
//
//   vegasfx_pan_test
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
    float pan;
    uint32_t hash;           // FNV-1a over the 8192 output floats (4096 frames, interleaved)
    uint32_t first[4], last[4];
};
// computed with the DLL's pan routine (0x180008750) for the input above
static const Expect kExpect[] = {
    {-1.f, 0x065eaaabu, {0x80000000, 0xbfba2100, 0x00000000, 0x3f483c00}, {0x00000000, 0x3f62fc00, 0x80000000, 0xbfaf3e00}},
    {-0.6f, 0x37401a2eu, {0xbecccccc, 0xbf86edcd, 0x3d165999, 0x3f3ed666}, {0x3e88dfff, 0x3f1e8c00, 0xbe2fcccc, 0xbf994466}},
    {-0.25f, 0x736a50eau, {0xbf400000, 0xbf344200, 0x3d8cf400, 0x3f369d80}, {0x3f005200, 0x3ec55400, 0xbea4d000, 0xbf860a00}},
    {-0.001f, 0x4daeadc5u, {0xbf7fbe77, 0xbee90712, 0x3dbbbfe3, 0x3f30c404}, {0x3f2aec33, 0x3e603f33, 0xbedb87be, 0xbf70b821}},
    {0.f, 0x38aba519u, {0xbf800000, 0xbee88400, 0x3dbbf000, 0x3f30be00}, {0x3f2b1800, 0x3e5f9000, 0xbedbc000, 0xbf709c00}},
    {0.25f, 0xeef08ed7u, {0xbf8e8840, 0xbeae6300, 0x3e875b00, 0x3f048e80}, {0x3f391100, 0x3e27ac00, 0xbf2a0700, 0xbf347500}},
    {0.7f, 0x9870a271u, {0xbfa8b0b3, 0xbe0b8267, 0x3f133633, 0x3e541734}, {0x3f52379a, 0x3d862334, 0xbf8b269a, 0xbe905d9a}},
    {1.f, 0x02e9016bu, {0xbfba2100, 0x80000000, 0x3f483c00, 0x00000000}, {0x3f62fc00, 0x00000000, 0xbfaf3e00, 0x80000000}},
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
        vegforge::applyPan(l.data(), r.data(), n, e.pan);
        std::vector<float> inter((size_t)n * 2);
        for (int k = 0; k < n; ++k) {
            inter[(size_t)(2 * k)] = l[(size_t)k];
            inter[(size_t)(2 * k + 1)] = r[(size_t)k];
        }
        std::string why;
        check(sameAsExpected(e, inter, &why), strfmt("Pan %g: all 8192 samples as the DLL's", (double)e.pan), why);
    }
}

static void testPlugin(double rate)
{
    const std::string tag = strfmt(" at %d Hz", (int)rate);
    auto p = make("BUILTIN", "reno.trkpan", rate);
    check(p != nullptr, "the built-in Pan is made" + tag);
    if (!p) return;
    check(p->paramCount() == 1 && p->paramName(0) == "Pan", "one setting, Pan" + tag);
    double lo = -9, hi = -9;
    check(p->paramRange(0, &lo, &hi) && lo == -1.0 && hi == 1.0, "its range is -1 to 1 (the DLL's descriptor)" + tag);
    check(std::fabs(p->paramGet(0) - 0.5f) < 1e-6f, "the default is 0 (half way)" + tag);
    check(p->programCount() == 2 && p->programName(0) == "(Default)" && p->programName(1) == "Default all parameters", "VEGAS's two presets" + tag);
    for (const Expect& e : kExpect) {
        auto q = make("BUILTIN", "reno.trkpan", rate);
        check(q->loadState(vegforge::encode(vegforge::pan(), e.pan)), "state with pan loads" + tag);
        const std::vector<float> out = runPlugin(*q, e.pan > 0 ? 100 : 512);
        std::string why;
        check(sameAsExpected(e, out, &why), strfmt("plug-in with pan %g gives the DLL's samples%s", (double)e.pan, tag.c_str()), why);
        float back = 9;
        check(vegforge::decode(vegforge::pan(), q->saveState(), &back) && back == e.pan, strfmt("  pan %g is kept exactly in the saved form%s", (double)e.pan, tag.c_str()));
    }
    p->paramSet(0, 0.75f);
    float v = -9;
    check(vegforge::decode(vegforge::pan(), p->saveState(), &v) && v == 0.5f, "paramSet(0.75) is a pan of 0.5" + tag);
    check(p->paramText(0) == "Left: 50.0 %", "its text: " + p->paramText(0) + tag);
    p->paramSet(0, 0.f);
    check(p->paramText(0) == "Right: 100.0 %", "-1 reads Right: 100 %: " + p->paramText(0) + tag);
    p->paramSet(0, 0.1f);
    check(p->loadState("reno-builtin-audio 1\nPan=0.3\n") && std::fabs(p->paramGet(0) - 0.5f) < 1e-6f, "settings of an older Reno load as the defaults" + tag);
    check(!p->loadState("garbage"), "unknown text is refused" + tag);
    check(p->setProgram(1) && p->currentProgram() == 1, "a preset can be chosen" + tag);
}

static void testSavedForm()
{
    std::string st = vegforge::encode(vegforge::pan(), 0.5f);
    const std::string raw = base64Decode(st.substr(2));
    check(raw.size() == 24 && raw[0] == 0x18 && raw[1] == 0 && raw[2] == 0 && raw[3] == 0, "the saved form is 24 bytes: length, GUID, float");
    float g = 0;
    check(vegforge::decode(vegforge::pan(), st, &g) && g == 0.5f, "it reads back");
    check(!vegforge::decode(vegforge::volume(), st, &g), "a Pan form is not taken for a Volume one (other GUID)");
    check(vegforge::limit(vegforge::pan(), 3.f) == 1.f && vegforge::limit(vegforge::pan(), -3.f) == -1.f, "typed values are limited to -1..1");
    auto p = make("BUILTIN", "reno.trkpan", 48000);
    check(p->loadState(st) && p->saveState() == st, "the plug-in keeps it byte for byte");
}

static void testDxFallback()
{
    const std::string dx = "dshow:{EB6213DB-08FF-4510-9F8D-3058B0ECE4C6}";
    check(builtinUidForDx(dx) == "reno.trkpan", "the DirectX Pan stands for the built-in one");
    check(dxUidForBuiltin("reno.trkpan") == dx, "the built-in one is saved as the DirectX Pan");
    check(builtinUidForDx("dshow:{EE38CA88-D78E-4BFB-B05E-577892730C83}") == "reno.trkvolume", "Volume's mapping is separate");
    AudioPluginDesc d;
    d.format = "DX";
    d.uid = dx;
    d.name = "VEGAS Pan";
    std::string err;
    auto p = createAudioPlugin(d, &err);      // no DirectX here (or no VEGAS): Reno's version
    check(p != nullptr, "a DX Pan slot makes a plug-in", err);
    if (p) {
        check(p->desc.format == "DX" && p->desc.uid == dx, "  that keeps the slot's description");
        check(builtinPageUid(p->desc) == "reno.trkpan" || p->desc.vendor != kBuiltinAudioVendor, "  and shows the Pan page");
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
