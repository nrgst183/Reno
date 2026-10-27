// Tests of Reno's VEGAS Paragraphic EQ (src/audiofx/vegaspgeq.h, vegasfir.h and the built-in "reno.parametriceq"): the
// sound is compared with the real sfppack2_x64.dll's (its recursive filters, run on Linux with a harness; the DLL is
// not needed here), including settings that change while the sound runs and a restart of the stream, plus the saved
// form, the presets and the DirectX map.
//
//   vegasfx_paragraphiceq_test
#include "../../src/audiofx/audioplugin.h"
#include "../../src/audiofx/vegasfx_pack2.h"
#include "../../src/audiofx/vegaspgeq.h"
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

// A case: the rate, the size of the buffers, the stretches of the sound ("frames:" and the 21 settings in the plug-in's
// own units, a trailing ":d" = the stretch starts a new stream; the last one runs to the end) and what the DLL gave: a
// CRC of the first 20000 frames of its output (interleaved floats) and the left channel at 8 frames.
struct Ref {
    int rate, buffer;
    std::vector<const char*> segs;
    uint32_t crc;
    uint32_t at[8];
};
static const int kFrames = 20000;
static const int kAt[8] = {300, 1301, 4999, 5000, 5001, 9000, 11005, 15555};
static const Ref kRefs[] = {
    {44100, 512, {"0:-250,0,0,0,-150,0,10,10,10,10,100,300,1000,5000,0,0,300,500,0,0,4"}, 0xfbabf19au, {0x3eb04a65u, 0xbe4c9e77u, 0xbed9e651u, 0x3ece07cfu, 0xbeb8865eu, 0xbeb71532u, 0xb96da3fbu, 0xbac812c3u}},
    {48000, 512, {"0:0,0,120,-80,60,200,5,15,25,8,80,700,3500,12000,1,1,250,9000,60,-90,4"}, 0xb4674166u, {0x3da92374u, 0x3f9f9282u, 0xc0857ef4u, 0xbfc76b68u, 0xbf2793b6u, 0xc05fafa3u, 0x3ae6d1e7u, 0xbc1c5fc4u}},
    {44100, 1000, {"0:0,-60,0,0,0,0,10,10,10,10,100,300,1000,5000,1,1,400,6000,-100,60,4"}, 0x3ddc948au, {0x3f2707dbu, 0xbf1208bau, 0xbf54a125u, 0x3f6f4fe8u, 0xbf4a830au, 0xbf4eec9au, 0xba283676u, 0xbb3ea1b0u}},
    {48000, 97, {"5000:-250,0,-100,0,0,80,10,10,10,10,100,300,1000,5000,0,1,300,7000,0,-60,4", "6000:-100,30,90,0,-250,50,20,10,10,25,200,300,1000,9000,1,0,500,500,-250,0,4:d", "0:-250,0,0,0,0,0,10,10,10,10,100,300,1000,5000,0,0,300,500,0,0,4"}, 0x36b65851u, {0x3e2d0a26u, 0x3dfb3953u, 0xbe02b58au, 0x3f88b627u, 0xbf951aceu, 0xbfa01ceau, 0x38d9fbe8u, 0xbacb3334u}},
    {44100, 512, {"0:-250,0,0,0,0,0,10,10,10,10,100,300,1000,5000,1,1,150,12000,-250,-250,4"}, 0xb3d32b81u, {0xbe1b1f13u, 0x3e99eb88u, 0x3e1068a6u, 0xbf0f514bu, 0x3cf91aacu, 0x3dcdffbcu, 0x3b2ac935u, 0x38b7abb7u}},
    {44100, 333, {"8000:-250,0,60,-60,60,-60,3,25,12,18,60,61,15000,20,0,0,300,500,0,0,4", "0:-250,0,-250,250,0,0,10,10,10,10,100,300,1000,5000,1,0,20,500,250,0,4"}, 0xaae6a377u, {0x3f22967au, 0xbf0736a2u, 0xbf789d0au, 0x3f7deb29u, 0xbf3a462eu, 0x3da717d4u, 0xbd2cbf49u, 0x398c19cdu}},
};

static vegpg::Settings parseSettings(const char* seg, long* frames, bool* restart)
{
    char* q = const_cast<char*>(seg);
    *frames = strtol(q, &q, 10);
    ++q;
    int32_t v[21] = {0};
    for (int k = 0; k < 21; ++k) {
        v[k] = (int32_t)strtol(q, &q, 10);
        if (*q == ',') ++q;
    }
    *restart = *q == ':';
    vegpg::Settings s;
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
        d.uid = "reno.paragraphiceq";
        std::unique_ptr<AudioPlugin> p = builtinAudioCreate(d, nullptr);
        p->activate(r.rate, 512);
        std::vector<float> got;
        long pos = 0, jump = 0;
        std::vector<float> l((size_t)r.buffer), rr((size_t)r.buffer);
        for (const char* seg : r.segs) {
            long n = 0;
            bool restart = false;
            const vegpg::Settings s = parseSettings(seg, &n, &restart);
            p->loadState(vegpg::encode(s));
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
        const std::string tag = strfmt("%d Hz, %zu stretch(es), buffers of %d, first: %.40s", r.rate, r.segs.size(), r.buffer, r.segs[0]);
        bool enough = got.size() / 2 >= (size_t)kFrames;
        check(enough, "enough sound: " + tag);
        if (!enough) continue;
        check(crcOf((const uint8_t*)got.data(), (size_t)kFrames * 8) == r.crc, "sound equals the DLL's: " + tag);
        bool same = true;
        for (int k = 0; k < 8; ++k) same = same && bitsOf(got[(size_t)(2 * kAt[k])]) == r.at[k];
        check(same, "  and its stored samples");
    }
}

static void testPlugin()
{
    AudioPluginDesc d;
    d.format = "BUILTIN";
    d.uid = "reno.paragraphiceq";
    std::string err;
    std::unique_ptr<AudioPlugin> p = builtinAudioCreate(d, &err);
    check(p != nullptr, "the built-in Paragraphic EQ is made");
    if (!p) return;
    check(p->paramCount() == 20, "it has dry, wet, 4 bands of gain/width/frequency and the two shelves");
    check(p->programCount() == 13, "it has VEGAS's 13 presets");
    check(p->programName(1) == "Default all parameters" && p->programName(2) == "15 dB notch centered at 1000 Hz", "  with their names");
    check(p->latencySamples() == 0, "it has no latency");
    vegpg::Settings s;
    s.dry = -100;
    s.wet = 45;
    s.gain[2] = -123;
    s.width[1] = 17;
    s.freq[3] = 7777;
    s.lowOn = 1;
    s.lowGain = -250;
    s.highFreq = 12000;
    s.extra = 2;
    check(p->loadState(vegpg::encode(s)), "the VEGAS saved form loads");
    check(p->saveState() == vegpg::encode(s), "  and saves back the same");
    vegpg::Settings back;
    check(vegpg::decode(p->saveState(), &back) && back == s, "  with the same settings (including the one the sound does not use)");
    const std::string st = p->saveState();
    check(st.compare(0, 2, "S:") == 0 && base64Decode(st.substr(2)).size() == 0x68, "  as 0x68 bytes after S:");
    check(p->paramText(4) == "-12.3 dB" && p->paramText(13) == "7777 Hz" && p->paramText(18) == "-Inf.", "gains in dB (-Inf. at the bottom), frequencies in Hz");
    check(p->loadState("reno-builtin-audio 1\nWet=3\n") && p->saveState() == vegpg::encode(vegpg::presets()[0].s), "settings of an older stand-in give the defaults");
    check(!p->loadState("garbage"), "garbage is refused");
    check(p->setProgram(5) && p->saveState() == vegpg::encode(vegpg::presets()[5].s), "a preset sets the settings");
}

static void testMap()
{
    check(builtinUidForDx("dshow:{D616F3E0-D622-11CE-AAC5-0020AF0B99A3}") == "reno.paragraphiceq", "VEGAS Paragraphic EQ's DirectX uid maps to the built-in");
    check(dxUidForBuiltin("reno.paragraphiceq") == "dshow:{D616F3E0-D622-11CE-AAC5-0020AF0B99A3}", "and back");
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
