// Tests of Reno's VEGAS Resonant Filter (src/audiofx/vegasresfilter.h and the built-in "reno.resonantfilter"): the sound is
// compared with the real sfresfilter_x64.dll's (its own filter code, run on Linux with a harness; the DLL is not needed
// here), for the low, band and high pass, both orders, the wet / dry mix, settings that change while the sound runs and a
// restart of the stream, plus the saved form, the presets, the parameters and the DirectX map.
//
//   vegasfx_resonantfilter_test
#include "../../src/audiofx/audioplugin.h"
#include "../../src/audiofx/vegasfx_pack2.h"
#include "../../src/audiofx/vegasresfilter.h"
#include <cstdio>
#include <cstdlib>
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

// A case: the rate, the size of the buffers, the stretches of the sound ("frames:" and the six numbers of the plug-in's
// settings (frequency, resonance, type, order, bypass, wet), a trailing ":d" = the stretch starts a new stream; the last
// one runs to the end) and what the DLL gave: a CRC of the first 20000 frames of the output (interleaved floats) and the
// left channel at 8 frames. The sound comes out with no delay. Each call of the effect is one buffer.
struct Ref {
    int rate, buffer;
    std::vector<const char*> segs;
    uint32_t crc;
    uint32_t at[8];
};
static const int kFrames = 20000;
static const int kAt[8] = {300, 1301, 4999, 5000, 5001, 9000, 11005, 15555};
static const Ref kRefs[] = {
    {44100, 512, {"0:135,90,1,1,0,100"}, 0xcb9c16d4u, {0xbce7f6b0u, 0xbe591150u, 0x3caee4beu, 0x3d5b1536u, 0x3daf39b2u, 0x3fc19a73u, 0x3eb7dff5u, 0x3d09eeceu}},
    {48000, 512, {"0:6000,100,2,1,0,100"}, 0x26babd19u, {0x40df2cdbu, 0xc0af5a74u, 0xc1b4dc02u, 0xc0f797cfu, 0x411fe185u, 0xc11857e7u, 0xbd84e1eeu, 0xbd9d62c3u}},
    {44100, 1000, {"0:1000,60,0,0,0,50"}, 0xd1d94d8du, {0x3eaf2cccu, 0xbe9dd71du, 0xbe70701cu, 0x3e82f128u, 0xbdb702c8u, 0xbe9f3cbeu, 0xb82d8f6eu, 0xb9d6571du}},
    {48000, 97, {"5000:500,100,0,0,0,100", "6000:2500,40,1,1,0,75:d", "0:900,70,2,0,0,100"}, 0xf5c14e9au, {0xbd31f113u, 0x3dbceab2u, 0x3ebf2af0u, 0x3e0d1bf0u, 0xbd8b95d0u, 0xbfc545e5u, 0x3a14540fu, 0xbb0ea794u}},
    {44100, 4096, {"0:20000,100,0,0,0,100"}, 0x05c1246cu, {0x40158470u, 0xbf266c94u, 0xbf861b90u, 0x3ff5f078u, 0xc01b4337u, 0xbff51ef0u, 0xbb1fa701u, 0xbb8c736bu}},
    {44100, 441, {"0:5750,33.3,1,0,0,25"}, 0xb1a7aa5fu, {0x3e8b84ccu, 0xbeae9a78u, 0xbe9a0a27u, 0x3ec72cbdu, 0xbebb2f4au, 0xbe54112cu, 0x3a13c2a8u, 0xbaa28d8au}},
    {48000, 1000, {"7777:1500,100,0,1,0,100", "5000:300,100,2,1,0,100:d", "0:75,50,0,1,0,60:d"}, 0xbfbb18c8u, {0xbf2f2d6bu, 0xbf21b2b2u, 0x3f40857bu, 0x3ffa029au, 0x40454c5au, 0x3f4c21a9u, 0xc07581f4u, 0xbe8fdde9u}},
};
// one channel, straight from the plug-in's own calls: the first 20000 frames
static const Ref kMono[] = {
    {44100, 512, {"0:880,25,1,1,0,100"}, 0x9e024631u, {0xbf39549eu, 0xbf0a6bbcu, 0x3da54826u, 0x3ea39435u, 0x3f0be77cu, 0xbf60342cu, 0x3aaf4f98u, 0x3a76fbc1u}},
    {48000, 1000, {"0:3600,100,0,0,0,100"}, 0x6d2deefau, {0xbedf520eu, 0xbe727370u, 0xbebbae84u, 0xbf20809au, 0xbf36ba10u, 0xbe5021ccu, 0x3b48a263u, 0x3b4ea4fau}},
};

static vegrf::Settings parseSettings(const char* seg, long* frames, bool* restart)
{
    char* q = const_cast<char*>(seg);
    *frames = strtol(q, &q, 10);
    ++q;
    float v[6] = {0};
    for (int k = 0; k < 6; ++k) {
        v[k] = strtof(q, &q);
        if (*q == ',') ++q;
    }
    *restart = *q == ':';
    vegrf::Settings s;
    std::memcpy(static_cast<void*>(&s), v, sizeof s);
    return s;
}

static void testDsp()
{
    const int frames = 24000;
    const std::vector<float> in = makeInput(frames);
    for (const Ref& r : kRefs) {
        AudioPluginDesc d;
        d.format = "BUILTIN";
        d.uid = "reno.resonantfilter";
        std::unique_ptr<AudioPlugin> p = builtinAudioCreate(d, nullptr);
        p->activate(r.rate, 512);
        std::vector<float> got;
        long pos = 0, jump = 0;
        std::vector<float> l((size_t)r.buffer), rr((size_t)r.buffer);
        for (const char* seg : r.segs) {
            long n = 0;
            bool restart = false;
            const vegrf::Settings s = parseSettings(seg, &n, &restart);
            p->loadState(vegrf::encode(s));
            if (restart) jump += 5000000;      // a jump in time starts the stream again
            if (n <= 0 || pos + n > frames) n = frames - pos;
            const long end = pos + n;
            while (pos < end) {
                const int m = (int)std::min<long>(r.buffer, end - pos);
                for (int k = 0; k < m; ++k) {
                    l[(size_t)k] = in[(size_t)(pos + k) * 2];
                    rr[(size_t)k] = in[(size_t)(pos + k) * 2 + 1];
                }
                p->process(l.data(), rr.data(), m, pos + jump, true);
                for (int k = 0; k < m; ++k) {
                    got.push_back(l[(size_t)k]);
                    got.push_back(rr[(size_t)k]);
                }
                pos += m;
            }
        }
        const std::string tag = strfmt("%d Hz, %zu stretch(es), buffers of %d, first: %.30s", r.rate, r.segs.size(), r.buffer, r.segs[0]);
        bool enough = got.size() / 2 >= (size_t)kFrames;
        check(enough, "enough sound: " + tag);
        if (!enough) continue;
        check(crcOf((const uint8_t*)got.data(), (size_t)kFrames * 8) == r.crc, "sound equals the DLL's: " + tag);
        bool same = true;
        for (int k = 0; k < 8; ++k) same = same && bitsOf(got[(size_t)(2 * kAt[k])]) == r.at[k];
        check(same, "  and its stored samples");
    }
}

// the effect's own engine with one channel
static void testMono()
{
    const int frames = 24000;
    const std::vector<float> in2 = makeInput(frames);
    std::vector<float> in((size_t)frames);
    for (int i = 0; i < frames; ++i) in[(size_t)i] = in2[(size_t)i * 2];
    for (const Ref& r : kMono) {
        long n = 0;
        bool restart = false;
        const vegrf::Settings s = parseSettings(r.segs[0], &n, &restart);
        vegrf::Dsp d;
        d.setSettings(s);
        d.setFormat(r.rate, 1);
        std::vector<float> got = in;
        for (int pos = 0; pos < frames; pos += r.buffer) d.process(got.data() + pos, nullptr, std::min(r.buffer, frames - pos));
        const std::string tag = strfmt("one channel, %d Hz, buffers of %d", r.rate, r.buffer);
        check(crcOf((const uint8_t*)got.data(), (size_t)kFrames * 4) == r.crc, "sound equals the DLL's: " + tag);
        bool same = true;
        for (int k = 0; k < 8; ++k) same = same && bitsOf(got[(size_t)kAt[k]]) == r.at[k];
        check(same, "  and its stored samples");
    }
}

static void testBypass()
{
    AudioPluginDesc d;
    d.format = "BUILTIN";
    d.uid = "reno.resonantfilter";
    std::unique_ptr<AudioPlugin> p = builtinAudioCreate(d, nullptr);
    p->activate(44100, 512);
    vegrf::Settings s = vegrf::presets()[2].s;
    p->loadState(vegrf::encode(s));
    p->paramSet(4, 1.f);                  // the Bypass parameter: the host leaves the effect out
    const std::vector<float> in = makeInput(4096);
    std::vector<float> l(2048), r(2048);
    for (int k = 0; k < 2048; ++k) {
        l[(size_t)k] = in[(size_t)k * 2];
        r[(size_t)k] = in[(size_t)k * 2 + 1];
    }
    const std::vector<float> l0 = l, r0 = r;
    p->process(l.data(), r.data(), 2048, 0, true);
    check(l == l0 && r == r0, "bypassed, the sound goes through as it is");
    p->paramSet(4, 0.f);
    p->process(l.data(), r.data(), 2048, 2048, true);
    check(l != l0, "and filtered again when the bypass is off");
}

static void testPlugin()
{
    AudioPluginDesc d;
    d.format = "BUILTIN";
    d.uid = "reno.resonantfilter";
    std::string err;
    std::unique_ptr<AudioPlugin> p = builtinAudioCreate(d, &err);
    check(p != nullptr, "the built-in Resonant Filter is made");
    if (!p) return;
    p->activate(44100, 512);
    check(p->paramCount() == 6, "it has the plug-in's 6 parameters");
    check(p->paramName(0) == "Frequency" && p->paramName(1) == "Resonance" && p->paramName(2) == "Filter Type" && p->paramName(3) == "Filter Order" &&
              p->paramName(4) == "Bypass" && p->paramName(5) == "Wet/Dry",
          "  with its names, in its order");
    check(p->programCount() == 18, "it has VEGAS's 18 presets");
    check(p->programName(0) == "(Default)" && p->programName(1) == "Default all parameters" && p->programName(2) == "Bass Hum" &&
              p->programName(7) == "High Howl" && p->programName(17) == "Teenage Late-Night Radio by the Bed",
          "  with their names");
    check(p->latencySamples() == 0, "it has no delay");
    vegrf::Settings s;
    s.freq = 1234.f;
    s.res = 56.7f;
    s.type = 2.f;
    s.order = 1.f;
    s.wet = 33.3f;
    check(p->loadState(vegrf::encode(s)), "the VEGAS saved form loads");
    check(p->saveState() == vegrf::encode(s), "  and saves back the same");
    vegrf::Settings back;
    check(vegrf::decode(p->saveState(), &back) && back == s, "  with the same settings");
    const std::string st = p->saveState();
    check(st.compare(0, 2, "S:") == 0 && base64Decode(st.substr(2)).size() == 0x2c, "  as 0x2c bytes after S:");
    check(p->paramText(0) == "1234 Hz" && p->paramText(1) == "56.7 %" && p->paramText(2) == "High Pass" && p->paramText(3) == "Fourth Order" && p->paramText(5) == "33.3 %",
          "frequency in Hz, resonance and wet in %, the type and the order named");
    check(p->loadState("reno-builtin-audio 1\nWet=3\n") && p->saveState() == vegrf::encode(vegrf::presets()[0].s), "settings of an older stand-in give the defaults");
    check(!p->loadState("garbage"), "garbage is refused");
    check(p->setProgram(5) && p->saveState() == vegrf::encode(vegrf::presets()[5].s), "a preset sets the settings");
    // the bypass is not saved
    p->paramSet(4, 1.f);
    vegrf::Settings nb;
    check(vegrf::decode(p->saveState(), &nb) && nb.bypass == 0.f, "the bypass is not in the saved form");
    // an old form (five floats) loads
    std::string old(4 + 16 + 20, '\0');
    const uint32_t oldLen = (uint32_t)old.size();
    std::memcpy(&old[0], &oldLen, 4);
    std::memcpy(&old[4], vegrf::persistGuidOld(), 16);
    const float five[5] = {2500.f, 40.f, 1.f, 1.f, 1.f};
    std::memcpy(&old[20], five, 20);
    vegrf::Settings d2;
    check(vegrf::decode("S:" + base64Encode(old.data(), old.size()), &d2) && d2.freq == 2500.f && d2.type == 1.f && d2.bypass == 0.f && d2.wet == 100.f,
          "the older saved form (five floats) loads");
    // the ranges hold
    vegrf::Settings wild;
    wild.freq = 5.f;
    wild.res = 500.f;
    wild.type = 9.f;
    wild.wet = -4.f;
    std::string wb(4 + 16 + 24, '\0');
    const uint32_t wl = (uint32_t)wb.size();
    std::memcpy(&wb[0], &wl, 4);
    std::memcpy(&wb[4], vegrf::persistGuid(), 16);
    std::memcpy(&wb[20], &wild, 24);
    vegrf::Settings kept;
    check(vegrf::decode("S:" + base64Encode(wb.data(), wb.size()), &kept) && kept.freq == 20.f && kept.res == 100.f && kept.type == 2.f && kept.wet == 0.f, "values out of range are kept in range");
    // the presets are the plug-in's
    const vegrf::Settings& a = vegrf::presets()[2].s;
    check(a.freq == 135.f && a.res == 90.f && a.type == 1.f && a.order == 1.f && a.wet == 100.f, "Bass Hum: band pass, fourth order, 135 Hz, 90 %");
    const vegrf::Settings& b = vegrf::presets()[13].s;
    check(b.freq == 6000.f && b.type == 2.f && b.order == 1.f, "PipSqueaking: high pass, fourth order, 6000 Hz");
    check(vegrf::presets()[5].s.res == 33.3f, "Ear Sugar: resonance 33.3 %");
}

static void testMap()
{
    check(builtinUidForDx("dshow:{A6A78627-D619-48BF-AD26-0C6B44B5C7D8}") == "reno.resonantfilter", "VEGAS Resonant Filter's DirectX uid maps to the built-in");
    check(dxUidForBuiltin("reno.resonantfilter") == "dshow:{A6A78627-D619-48BF-AD26-0C6B44B5C7D8}", "and back");
    check(builtinUidForDx("dshow:{026D0AA0-9BB9-11D0-AEBC-00A0C9053912}") == "reno.multibanddynamics", "Multi-Band Dynamics's map still works");
    AudioPluginDesc dx;
    dx.format = "DX";
    dx.uid = "a6a78627-d619-48bf-ad26-0c6b44b5c7d8";
    check(audioPluginFileHint(dx).compare(0, 19, "sfresfilter_x64.dll") == 0, "the file the DirectX effect lives in is known");
}

int main()
{
    testDsp();
    testMono();
    testBypass();
    testPlugin();
    testMap();
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
