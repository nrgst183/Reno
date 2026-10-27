// Tests of Reno's VEGAS Noise Gate (src/audiofx/vegasnoisegate.h and the built-in "reno.noisegate"): the sound is
// compared with samples that the real sfppack2_x64.dll produced (its own set-format and process code, run on Linux
// with a harness: the DLL is not needed here), plus the saved form, the presets and the DirectX map.
//
//   vegasfx_noisegate_test
#include "../../src/audiofx/audioplugin.h"
#include "../../src/audiofx/vegasfx_pack2.h"
#include "../../src/audiofx/vegasnoisegate.h"
#include <cstdio>
#include <cstring>

using namespace reno;

static int gFail = 0, gPass = 0;
static void check(bool ok, const std::string& what)
{
    if (ok) { ++gPass; std::printf("PASS %s\n", what.c_str()); }
    else { ++gFail; std::printf("FAIL %s\n", what.c_str()); }
}

static uint32_t crcOf(const uint8_t* p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t k = 0; k < n; ++k) {
        crc ^= p[k];
        for (int i = 0; i < 8; ++i) crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    return ~crc;
}
static uint32_t bitsOf(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// The input the reference output was made from: loud noise, quiet noise, and a middle level in turns
static std::vector<float> makeInput(int frames)
{
    std::vector<float> v((size_t)frames * 2);
    uint32_t s = 12345u;
    for (int i = 0; i < frames; ++i) {
        float amp = ((i / 9600) % 2) ? 0.002f : 0.5f;
        if ((i / 9600) % 5 == 4) amp = 0.05f;
        for (int c = 0; c < 2; ++c) {
            s = s * 1664525u + 1013904223u;
            const int x = (int)((s >> 16) & 0xffff) - 32768;
            v[(size_t)i * 2 + (size_t)c] = (float)x / 32768.0f * amp;
        }
    }
    return v;
}

struct Ref {
    int rate, attack, release, threshold;
    uint32_t crc;           // of all complete blocks the plug-in output (interleaved float)
    uint32_t at[8];         // samples 1200, 1201, 9650, 9651, 19300, 19301, 40001, 40002 (as float bits)
};
static const Ref kRefs[] = {
    {44100, 3, 100, -200, 0x16efd251u, {0x3e3bffd3u, 0x3e97054du, 0x3d33e000u, 0xbe428400u, 0xba77cac1u, 0xbaa07ae2u, 0x3e8e1f94u, 0x3d4bcbc7u}},
    {48000, 10, 500, -600, 0x27dddea0u, {0x3e3a4e3du, 0x3e97054du, 0x3d33b1f4u, 0xbe428400u, 0xba77cac1u, 0xbaa07ae2u, 0x3e8e4400u, 0x3d4c0000u}},
    {48000, 1, 300, -150, 0x1c74f2d1u, {0x3e3c3000u, 0x3e972c00u, 0x3d33e000u, 0xbe428400u, 0xba77cac1u, 0xbaa07ae2u, 0x3e8e4400u, 0x3d4c0000u}},
    {44100, 300, 1000, -400, 0x6dc038b9u, {0x3c91c39bu, 0x3d883c0fu, 0x3d0fb757u, 0xbe20fa61u, 0xba6dd4a7u, 0xba9af25cu, 0x3e38a760u, 0x3cf10cdeu}},
    {48000, 5, 50, -800, 0xea6a15e6u, {0x3e3bffd3u, 0x3e97054du, 0x3d33e000u, 0xbe428400u, 0xba77cac1u, 0xbaa07ae2u, 0x3e8e4400u, 0x3d4c0000u}},
};

static std::vector<float> runDsp(const Ref& r, const std::vector<float>& in, int frames)
{
    vegng::Dsp d;
    vegng::Settings s;
    s.attack = r.attack;
    s.release = r.release;
    s.threshold = r.threshold;
    d.setSettings(s);
    d.setFormat(r.rate, 2);
    std::vector<float> out;
    for (int i = 0; i < frames; ++i) {
        d.applyChanges();
        if (d.push(&in[(size_t)i * 2]))
            for (int k = 0; k < d.blockFrames(); ++k) {
                out.push_back(d.outAt(0, k));
                out.push_back(d.outAt(1, k));
            }
    }
    return out;
}

static void testDsp()
{
    const int frames = 100000;
    const std::vector<float> in = makeInput(frames);
    static const int idx[8] = {1200, 1201, 9650, 9651, 19300, 19301, 40001, 40002};
    for (const Ref& r : kRefs) {
        const std::vector<float> out = runDsp(r, in, frames);
        const std::string tag = strfmt("%d Hz, attack %d ms, release %d ms, threshold %.1f dB", r.rate, r.attack, r.release, r.threshold / 10.0);
        check(crcOf((const uint8_t*)out.data(), out.size() * 4) == r.crc, "sound equals the DLL's: " + tag);
        bool same = true;
        for (int k = 0; k < 8; ++k) same = same && bitsOf(out[(size_t)idx[k]]) == r.at[k];
        check(same, "  and its stored samples");
    }
}

static void testPlugin()
{
    const int frames = 30000;
    const std::vector<float> in = makeInput(frames);
    AudioPluginDesc d;
    d.format = "BUILTIN";
    d.uid = "reno.noisegate";
    std::string err;
    std::unique_ptr<AudioPlugin> p = builtinAudioCreate(d, &err);
    check(p != nullptr, "the built-in Noise Gate is made");
    if (!p) return;
    check(p->paramCount() == 3, "it has attack, release and threshold");
    check(p->programCount() == 7, "it has VEGAS's 7 presets");
    check(p->programName(1) == "Default all parameters" && p->programName(6) == "Slow attack", "  with their names");
    // settings in the VEGAS saved form
    vegng::Settings s;
    s.attack = 10;
    s.release = 500;
    s.threshold = -600;
    check(p->loadState(vegng::encode(s)), "the VEGAS saved form loads");
    check(p->saveState() == vegng::encode(s), "  and saves back the same");
    vegng::Settings back;
    check(vegng::decode(p->saveState(), &back) && back.attack == 10 && back.release == 500 && back.threshold == -600, "  with the same settings");
    const std::string st = p->saveState();
    check(st.compare(0, 2, "S:") == 0 && base64Decode(st.substr(2)).size() == 0x20, "  as 0x20 bytes after S:");
    check(p->paramText(2) == "-60.0 dB", "the threshold is shown in dB");
    check(p->loadState("reno-builtin-audio 1\nAttack=3\n") && p->saveState() == vegng::encode(vegng::presets()[0].s), "settings of the older stand-in give the defaults");
    check(!p->loadState("garbage"), "garbage is refused");
    p->loadState(vegng::encode(s));
    // the sound: one block late
    const int rate = 48000;
    p->activate(rate, 512);
    check(p->latencySamples() == 960, "it is 20 ms late");
    Ref r{rate, 10, 500, -600, 0, {}};
    const std::vector<float> want = runDsp(r, in, frames);
    std::vector<float> got((size_t)frames * 2);
    std::vector<float> l((size_t)frames), rr((size_t)frames);
    for (int i = 0; i < frames; ++i) {
        l[(size_t)i] = in[(size_t)i * 2];
        rr[(size_t)i] = in[(size_t)i * 2 + 1];
    }
    int64_t pos = 0;
    for (int at = 0; at < frames;) {
        const int n = std::min(frames - at, at % 3 == 0 ? 512 : 137);
        p->process(l.data() + at, rr.data() + at, n, pos, true);
        at += n;
        pos += n;
    }
    bool silent = true, same = true;
    for (int i = 0; i < 960; ++i) silent = silent && l[(size_t)i] == 0.f && rr[(size_t)i] == 0.f;
    for (size_t k = 0; k + 960 < (size_t)frames && (k + 1) * 2 <= want.size(); ++k)
        same = same && bitsOf(l[k + 960]) == bitsOf(want[k * 2]) && bitsOf(rr[k + 960]) == bitsOf(want[k * 2 + 1]);
    check(silent, "the first block is silent");
    check(same, "the rest is the DLL's sound, a block later, whatever the buffer sizes");
    // a jump in time starts it again
    std::vector<float> l2(l.begin(), l.begin() + 2000), r2(rr.begin(), rr.begin() + 2000);
    p->process(l2.data(), r2.data(), 2000, 5000000, true);
    bool quiet = true;
    for (int i = 0; i < 960; ++i) quiet = quiet && l2[(size_t)i] == 0.f;
    check(quiet, "after a jump the first block is silent again");
    // presets
    check(p->setProgram(2) && p->saveState() == vegng::encode(vegng::presets()[2].s), "a preset sets the settings");
    check(vegng::presets()[2].s.attack == 1 && vegng::presets()[2].s.release == 300 && vegng::presets()[2].s.threshold == -60, "  \"Strong beats only\" is 1 ms, 300 ms, -6 dB");
}

static void testMap()
{
    check(builtinUidForDx("dshow:{B97C0F23-196D-11D1-B99B-00A0C9053912}") == "reno.noisegate", "VEGAS Noise Gate's DirectX uid maps to the built-in");
    check(dxUidForBuiltin("reno.noisegate") == "dshow:{B97C0F23-196D-11D1-B99B-00A0C9053912}", "and back");
    check(builtinUidForDx("dshow:{39224540-6F92-11D0-AEBC-00A0C9053912}") == "reno.distortion", "Distortion's map still works");
}

int main()
{
    testDsp();
    testPlugin();
    testMap();
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
