// Tests of Reno's VEGAS Parametric EQ (src/audiofx/vegaspmeq.h, vegasfir.h and the built-in "reno.parametriceq"): the
// sound is compared with the real sfppack2_x64.dll's (its own filter design and FIR engine, run on Linux with a
// harness; the DLL is not needed here), including settings that change while the sound runs, plus the saved form, the
// presets and the DirectX map.
//
//   vegasfx_parametriceq_test
#include "../../src/audiofx/audioplugin.h"
#include "../../src/audiofx/vegasfx_pack2.h"
#include "../../src/audiofx/vegaspmeq.h"
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

// A case: the rate, the size of the buffers the sound is given in, the stretches of the sound ("frames:style,freq,
// width,amount,outGain,accuracy" - the settings in the plug-in's own units, the last one to the end) and what the
// DLL gave: a CRC of the first 20000 frames of its output (interleaved floats) and the left channel at 8 frames.
struct Ref {
    int rate, buffer;
    std::vector<const char*> segs;
    uint32_t crc;
    uint32_t at[8];
};
static const int kFrames = 20000;
static const int kAt[8] = {300, 1301, 4999, 5000, 5001, 9000, 11005, 15555};
static const Ref kRefs[] = {
    {44100, 512, {"0:2,650,10,-40,0,1"}, 0x09d0007bu, {0x3e51355au, 0xbe412cf0u, 0xbe93d8f2u, 0x3e85ba73u, 0xbe78dabdu, 0xbe8b7810u, 0x381114eeu, 0xba6c9410u}},
    {48000, 512, {"0:0,100,30,-300,50,2"}, 0xd90d9d63u, {0x3efab184u, 0xbed54550u, 0xbf5194eau, 0x3f3a5ed6u, 0xbf32ba71u, 0xbf3f1cf6u, 0x39271ce2u, 0xbb3aa08au}},
    {44100, 512, {"0:1,8000,20,150,-100,0"}, 0xfefee44cu, {0x3ee74598u, 0xbedc400cu, 0xbe938b8au, 0x3f23242cu, 0xbf252e07u, 0xbe615964u, 0xba531ce6u, 0xbadca020u}},
    {48000, 1000, {"0:3,1000,5,-200,100,1"}, 0x205727a0u, {0x3fa27054u, 0xbf42e90bu, 0xbfc51e66u, 0x3f9b804du, 0xbfa8c3ebu, 0xbf961ae2u, 0xb940cb0du, 0xbba99f0cu}},
    {44100, 97, {"5000:2,1500,10,-100,0,0", "6000:3,300,20,120,0,0", "0:0,5000,40,-250,30,0"}, 0x410e6d96u, {0x3ddba8e0u, 0xbe0a2974u, 0xbe1ec0ceu, 0x3dfa6c9au, 0xbe0189f6u, 0xbe41d9eeu, 0x36f0e730u, 0xbb2d7ecau}},
    {48000, 512, {"7000:1,3000,10,-60,0,1", "0:2,500,10,-200,0,1"}, 0x8543e1a9u, {0x3e5b731cu, 0xbe3d473eu, 0xbe5ad56du, 0x3e5ecc9fu, 0xbe3cce64u, 0xbe7c5abdu, 0xb8d633bau, 0xb8330ed5u}},
};

static vegpm::Settings parseSettings(const char* seg, long* frames)
{
    char* q = const_cast<char*>(seg);
    *frames = strtol(q, &q, 10);
    ++q;
    int32_t v[6] = {0};
    for (int k = 0; k < 6; ++k) {
        v[k] = (int32_t)strtol(q, &q, 10);
        if (*q == ',') ++q;
    }
    vegpm::Settings s;
    std::memcpy(&s, v, sizeof s);
    return s;
}

static void testDsp()
{
    const int frames = 24000;
    const std::vector<float> in = makeInput(frames);
    for (const Ref& r : kRefs) {
        AudioPluginDesc d;
        d.format = "BUILTIN";
        d.uid = "reno.parametriceq";
        std::unique_ptr<AudioPlugin> p = builtinAudioCreate(d, nullptr);
        p->activate(r.rate, 512);
        std::vector<float> got;
        long pos = 0;
        std::vector<float> l((size_t)r.buffer), rr((size_t)r.buffer);
        for (const char* seg : r.segs) {
            long n = 0;
            const vegpm::Settings s = parseSettings(seg, &n);
            p->loadState(vegpm::encode(s));
            if (n <= 0 || pos + n > frames) n = frames - pos;
            const long end = pos + n;
            while (pos < end) {
                const int m = (int)std::min<long>(r.buffer, end - pos);
                for (int k = 0; k < m; ++k) {
                    l[(size_t)k] = in[(size_t)(pos + k) * 2];
                    rr[(size_t)k] = in[(size_t)(pos + k) * 2 + 1];
                }
                p->process(l.data(), rr.data(), m, pos, true);
                for (int k = 0; k < m; ++k) {
                    got.push_back(l[(size_t)k]);
                    got.push_back(rr[(size_t)k]);
                }
                pos += m;
            }
        }
        const size_t lat = (size_t)p->latencySamples();      // the plug-in's sound is this many frames late
        const std::string tag = strfmt("%d Hz, %zu stretch(es), buffers of %d, first: %s", r.rate, r.segs.size(), r.buffer, r.segs[0]);
        bool enough = got.size() / 2 >= lat + (size_t)kFrames;
        check(enough, "enough sound: " + tag);
        if (!enough) continue;
        const float* base = got.data() + 2 * lat;
        check(crcOf((const uint8_t*)base, (size_t)kFrames * 8) == r.crc, "sound equals the DLL's: " + tag);
        bool same = true;
        for (int k = 0; k < 8; ++k) same = same && bitsOf(base[2 * kAt[k]]) == r.at[k];
        check(same, "  and its stored samples");
    }
}

static void testPlugin()
{
    AudioPluginDesc d;
    d.format = "BUILTIN";
    d.uid = "reno.parametriceq";
    std::string err;
    std::unique_ptr<AudioPlugin> p = builtinAudioCreate(d, &err);
    check(p != nullptr, "the built-in Parametric EQ is made");
    if (!p) return;
    check(p->paramCount() == 6, "it has filter style, frequency, width, amount, output gain and accuracy");
    check(p->programCount() == 6, "it has VEGAS's 6 presets");
    check(p->programName(1) == "Default all parameters" && p->programName(2) == "200 Hz high-pass" && p->programName(4) == "Phone line effect", "  with their names");
    check(p->latencySamples() == 512, "the default (medium accuracy) is 512 frames late");
    vegpm::Settings s;
    s.style = 3;
    s.freq = 1234;
    s.width = 17;
    s.amount = -123;
    s.outGain = 45;
    s.accuracy = 2;
    check(p->loadState(vegpm::encode(s)), "the VEGAS saved form loads");
    check(p->saveState() == vegpm::encode(s), "  and saves back the same");
    vegpm::Settings back;
    check(vegpm::decode(p->saveState(), &back) && std::memcmp(&back, &s, sizeof s) == 0, "  with the same settings");
    const std::string st = p->saveState();
    check(st.compare(0, 2, "S:") == 0 && base64Decode(st.substr(2)).size() == 0x2c, "  as 0x2c bytes after S:");
    check(p->latencySamples() == 2048, "high accuracy is 2048 frames late");
    check(p->paramText(3) == "-12.3 dB" && p->paramText(1) == "1234 Hz", "the amount is shown in dB, the frequency in Hz");
    check(p->loadState("reno-builtin-audio 1\nFrequency=3\n") && p->saveState() == vegpm::encode(vegpm::presets()[0].s), "settings of an older stand-in give the defaults");
    check(!p->loadState("garbage"), "garbage is refused");
    check(p->setProgram(5) && p->saveState() == vegpm::encode(vegpm::presets()[5].s), "a preset sets the settings");
    check(vegpm::presets()[5].s.style == 3 && vegpm::presets()[5].s.freq == 1500 && vegpm::presets()[5].s.amount == -250, "  \"Remove frequencies around 1500 Hz by 25 dB\" is a notch at 1500 Hz of -25 dB");
    // a jump in time starts it again
    p->loadState(vegpm::encode(vegpm::presets()[0].s));
    p->activate(48000, 512);
    std::vector<float> l(2000, 0.3f), r(2000, 0.3f);
    p->process(l.data(), r.data(), 2000, 0, true);
    std::vector<float> l2(2000, 0.3f), r2(2000, 0.3f);
    p->process(l2.data(), r2.data(), 2000, 5000000, true);
    bool quiet = true;
    for (int i = 0; i < 512; ++i) quiet = quiet && l2[(size_t)i] == 0.f;
    check(quiet, "after a jump the first block is silent again");
}

static void testMap()
{
    check(builtinUidForDx("dshow:{A8448720-96FD-11D0-AEBC-00A0C9053912}") == "reno.parametriceq", "VEGAS Parametric EQ's DirectX uid maps to the built-in");
    check(dxUidForBuiltin("reno.parametriceq") == "dshow:{A8448720-96FD-11D0-AEBC-00A0C9053912}", "and back");
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
