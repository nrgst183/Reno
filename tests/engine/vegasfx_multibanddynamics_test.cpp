// Tests of Reno's VEGAS Multi-Band Dynamics (src/audiofx/vegasmbdyn.h and the built-in "reno.multibanddynamics"): the sound
// is compared with the real sfppack2_x64.dll's (its block engine and its four bands, run on Linux with a harness; the DLL
// is not needed here), including settings that change while the sound runs, solo, synced and separate channels and a
// restart of the stream, plus the saved form, the presets, the parameters and the DirectX map.
//
//   vegasfx_multibanddynamics_test
#include "../../src/audiofx/audioplugin.h"
#include "../../src/audiofx/vegasfx_pack2.h"
#include "../../src/audiofx/vegasmbdyn.h"
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

// A case: the rate, the size of the buffers, the stretches of the sound ("frames:" and the 53 numbers of the plug-in's
// settings, a trailing ":d" = the stretch starts a new stream; the last one runs to the end) and what the DLL gave
// (the sound comes out one 20 ms block late, a block of silence first): a CRC of the first 20000 frames of the output
// (interleaved floats) and the left channel at 8 frames. Each call of the effect is one buffer, as the DLL's Process.
struct Ref {
    int rate, buffer;
    std::vector<const char*> segs;
    uint32_t crc;
    uint32_t at[8];
};
static const int kFrames = 20000;
static const int kAt[8] = {300, 1301, 4999, 5000, 5001, 9000, 11005, 15555};
static const Ref kRefs[] = {
    {44100, 512, {"0:0,-250,-250,-250,-250,1,1,1,1,300,300,300,300,10,10,10,10,1000,1000,1000,1000,0,0,0,0,7,7,7,7,1209,1336,1477,1633,1,0,0,0,0,128,128,128,128,16,16,16,16,1,0,0,0,0,0,0"}, 0x6c9581fdu, {0x00000000u, 0x3cd249dcu, 0x3e4c3f99u, 0x3db764d2u, 0x3e6d7476u, 0xbe4b68aeu, 0x389205b3u, 0x3a4fc0d1u}},
    {48000, 512, {"0:0,-300,0,0,0,1,0,0,0,250,0,0,0,10,0,0,0,1000,0,0,0,20,0,0,0,10,0,0,0,5000,0,0,0,1,0,1,1,1,128,128,128,128,16,16,16,16,1,0,0,0,0,0,0"}, 0xd194b419u, {0x00000000u, 0x3ea7db3cu, 0xbeb22a99u, 0x3e5563ffu, 0xbe7a1446u, 0x3e4d4b2fu, 0xb95592cfu, 0x3ab5df10u}},
    {44100, 1000, {"0:0,-300,-250,-200,-150,0,1,2,1,40,25,60,500,0,50,200,10,3000,500,10000,100,60,-80,70,-250,10,5,10,30,300,1500,6000,12000,1,0,0,0,0,128,128,128,128,16,16,16,16,1,0,0,0,0,0,0"}, 0x0960b832u, {0x00000000u, 0x3d2f455du, 0x3d89b4b9u, 0x3ca372f3u, 0xbd9b5231u, 0x3d16133au, 0xb985f7c8u, 0x3a3b9694u}},
    {48000, 97, {"4990:0,-300,-300,-300,-300,1,1,1,1,30,30,30,30,10,10,10,10,1000,1000,1000,1000,0,0,0,0,10,10,10,10,200,1000,4000,10000,1,0,0,0,1,128,128,128,128,16,16,16,16,1,0,0,0,0,0,0", "6000:30,-200,-400,-100,-100,2,0,1,1,500,100,20,20,10,10,10,10,1000,1000,1000,1000,-50,40,0,0,10,10,10,10,200,1000,4000,10000,0,0,0,1,1,128,128,128,128,16,16,16,16,1,0,0,0,0,0,0:d", "0:-30,-250,-250,-250,-250,1,1,1,1,60,60,60,60,10,10,10,10,1000,1000,1000,1000,30,-30,30,-30,10,10,10,10,200,1000,4000,10000,1,0,0,0,0,128,128,128,128,16,16,16,16,1,0,0,0,0,0,0"}, 0x01930669u, {0x00000000u, 0x3ea241b0u, 0x00000000u, 0x00000000u, 0x00000000u, 0xbc866f59u, 0x37f3a2aeu, 0x3a1c4098u}},
    {44100, 4096, {"0:30,-250,-250,-250,-250,0,2,0,2,500,500,500,500,0,0,0,0,0,0,0,0,-80,0,80,0,10,10,10,10,150,3000,500,9000,0,0,0,0,0,128,128,128,128,16,16,16,16,1,0,0,0,0,0,0"}, 0x1bab1be0u, {0x00000000u, 0xbd151b80u, 0xbec23775u, 0xbf042760u, 0xbed682c1u, 0xbd852f29u, 0xb99bf06fu, 0x3a779199u}},
    {44100, 441, {"0:0,-200,-200,-200,-200,1,1,1,1,100,100,100,100,50,50,50,50,1000,1000,1000,1000,0,0,0,0,10,10,10,10,300,1200,3500,8000,1,0,0,0,0,128,128,128,128,16,16,16,16,1,0,0,1,0,0,0"}, 0x0ee97684u, {0x00000000u, 0x3d382ab2u, 0x3df8dcb1u, 0x3c518964u, 0x3e442f96u, 0xbe6fd032u, 0xb893ae72u, 0x3a531ff8u}},
    {48000, 1000, {"7777:0,-250,-250,-250,-250,1,1,1,1,200,200,200,200,10,10,10,10,1000,1000,1000,1000,0,0,0,0,10,10,10,10,200,1000,4000,10000,1,0,0,1,1,128,128,128,128,16,16,16,16,1,0,0,0,0,0,0", "0:0,-120,-150,-90,-90,1,2,0,0,300,300,300,300,10,200,300,300,200,200,200,200,0,60,-60,0,7,7,7,7,200,1000,4000,10000,1,0,0,0,1,128,128,128,128,16,16,16,16,1,0,0,0,0,0,0"}, 0x807b50c3u, {0x00000000u, 0x3e915a55u, 0xbeb949f0u, 0x3e355777u, 0xbed32affu, 0x3deeee86u, 0x38e179e8u, 0x3ae04b18u}},
};
// one channel, straight from the plug-in's own calls (no delay added): the first 20000 frames
static const Ref kMono[] = {
    {44100, 512, {"0:0,-250,-250,-250,-250,1,1,1,1,300,300,300,300,10,10,10,10,1000,1000,1000,1000,0,0,0,0,7,7,7,7,1209,1336,1477,1633,1,0,0,0,0,128,128,128,128,16,16,16,16,1,0,0,0,0,0,0"}, 0x29c1a82au, {0x3e9af7cau, 0xbe34aaffu, 0xbec1ca9eu, 0x3ec51c5eu, 0xbea8743au, 0xbe881b7du, 0xb9aa9e33u, 0xbaca4ff0u}},
    {48000, 1000, {"0:0,-300,-250,-200,-150,0,1,2,1,40,25,60,500,0,50,200,10,3000,500,10000,100,60,-80,70,-250,10,5,10,30,300,1500,6000,12000,1,0,0,0,0,128,128,128,128,16,16,16,16,1,0,0,0,0,0,0"}, 0xe917785bu, {0x3df50a13u, 0xbe2897c0u, 0xbd57c28cu, 0x3dd67575u, 0x3da52eb0u, 0xbda0d11fu, 0x3a963e4eu, 0x3a356fa4u}},
};

static vegmb::Settings parseSettings(const char* seg, long* frames, bool* restart)
{
    char* q = const_cast<char*>(seg);
    *frames = strtol(q, &q, 10);
    ++q;
    int32_t v[vegmb::kInts] = {0};
    for (int k = 0; k < vegmb::kInts; ++k) {
        v[k] = (int32_t)strtol(q, &q, 10);
        if (*q == ',') ++q;
    }
    *restart = *q == ':';
    vegmb::Settings s;
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
        d.uid = "reno.multibanddynamics";
        std::unique_ptr<AudioPlugin> p = builtinAudioCreate(d, nullptr);
        p->activate(r.rate, 512);
        std::vector<float> got;
        long pos = 0, jump = 0;
        std::vector<float> l((size_t)r.buffer), rr((size_t)r.buffer);
        for (const char* seg : r.segs) {
            long n = 0;
            bool restart = false;
            const vegmb::Settings s = parseSettings(seg, &n, &restart);
            p->loadState(vegmb::encode(s));
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

// the effect's own engine with one channel, called as the DLL is
static void testMono()
{
    const int frames = 24000;
    const std::vector<float> in2 = makeInput(frames);
    std::vector<float> in((size_t)frames);
    for (int i = 0; i < frames; ++i) in[(size_t)i] = in2[(size_t)i * 2];
    for (const Ref& r : kMono) {
        long n = 0;
        bool restart = false;
        const vegmb::Settings s = parseSettings(r.segs[0], &n, &restart);
        vegmb::Dsp d;
        d.setSettings(s);
        d.setFormat(r.rate, 1);
        std::vector<float> got;
        std::vector<float> o((size_t)r.buffer + 8);
        for (int pos = 0; pos < frames;) {
            const int m = std::min(r.buffer, frames - pos);
            int done = 0;
            while (done < m) {
                int used = 0, given = 0;
                d.process(in.data() + pos + done, m - done, o.data(), m, &used, &given);
                got.insert(got.end(), o.begin(), o.begin() + given);
                done += used;
                if (used == 0 && given == 0) break;
            }
            pos += m;
        }
        const std::string tag = strfmt("one channel, %d Hz, buffers of %d", r.rate, r.buffer);
        bool enough = got.size() >= (size_t)kFrames;
        check(enough, "enough sound: " + tag);
        if (!enough) continue;
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
    d.uid = "reno.multibanddynamics";
    std::unique_ptr<AudioPlugin> p = builtinAudioCreate(d, nullptr);
    p->activate(44100, 512);
    vegmb::Settings s = vegmb::presets()[2].s;
    s.bypassAll = 1;
    p->loadState(vegmb::encode(s));
    const std::vector<float> in = makeInput(4096);
    std::vector<float> l(2048), r(2048);
    for (int k = 0; k < 2048; ++k) {
        l[(size_t)k] = in[(size_t)k * 2];
        r[(size_t)k] = in[(size_t)k * 2 + 1];
    }
    const std::vector<float> l0 = l, r0 = r;
    p->process(l.data(), r.data(), 2048, 0, true);
    check(l == l0 && r == r0, "bypassed, the sound goes through as it is, with no delay");
}

static void testPlugin()
{
    AudioPluginDesc d;
    d.format = "BUILTIN";
    d.uid = "reno.multibanddynamics";
    std::string err;
    std::unique_ptr<AudioPlugin> p = builtinAudioCreate(d, &err);
    check(p != nullptr, "the built-in Multi-Band Dynamics is made");
    if (!p) return;
    p->activate(44100, 512);
    check(p->paramCount() == 43, "it has the plug-in's 43 parameters");
    check(p->paramName(0) == "Band 1 Threshold" && p->paramName(6) == "Band 1 Center" && p->paramName(39) == "Band 4 Solo" &&
              p->paramName(40) == "Stereo Sync Channels" && p->paramName(41) == "Output Gain" && p->paramName(42) == "Bypass",
          "  with its names, in its order");
    check(p->programCount() == 6, "it has VEGAS's 6 presets");
    check(p->programName(0) == "(Default)" && p->programName(2) == "Anti-DTMF talk-off" && p->programName(5) == "Reduce plosives and sibilants", "  with their names");
    check(p->latencySamples() == 882, "it is one 20 ms block late");
    vegmb::Settings s = vegmb::presets()[5].s;
    s.outGain = -77;
    s.threshold[1] = -123;
    s.gain[2] = 45;
    s.attack[0] = 123;
    s.release[3] = 4567;
    s.width[1] = 17;
    s.freq[3] = 12345;
    s.type[2] = 2;
    s.solo[1] = 1;
    s.sync = 0;
    s.extra[2] = 99;
    check(p->loadState(vegmb::encode(s)), "the VEGAS saved form loads");
    check(p->saveState() == vegmb::encode(s), "  and saves back the same (also the integers of its window)");
    vegmb::Settings back, kept = s;
    vegmb::sanitize(kept);          // (the ranges of the parameters hold: the presets' unused bands have a 0 Hz center)
    check(vegmb::decode(p->saveState(), &back) && back == kept, "  with the same settings");
    const std::string st = p->saveState();
    check(st.compare(0, 2, "S:") == 0 && base64Decode(st.substr(2)).size() == 0xe8, "  as 0xe8 bytes after S:");
    check(p->paramText(1 * 0 + 10 + 0) == "-12.3 dB" && p->paramText(3) == "12.3 ms" && p->paramText(7 + 10) == "1.7 octaves" && p->paramText(36) == "12345 Hz",
          "levels in dB, times in ms, the width in octaves, the center in Hz");
    check(p->paramText(25) == "High-shelf", "the type is named");
    check(p->loadState("reno-builtin-audio 1\nWet=3\n") && p->saveState() == vegmb::encode(vegmb::presets()[0].s), "settings of an older stand-in give the defaults");
    check(!p->loadState("garbage"), "garbage is refused");
    check(p->setProgram(4) && p->saveState() == vegmb::encode(vegmb::presets()[4].s), "a preset sets the settings");
    // an old form (without the bypass) loads
    std::string old = base64Decode(vegmb::encode(s).substr(2));
    old.resize(old.size() - 4);
    const uint32_t oldLen = (uint32_t)old.size();
    std::memcpy(&old[0], &oldLen, 4);
    std::memcpy(&old[4], vegmb::persistGuidOld(), 16);
    vegmb::Settings d2;
    check(vegmb::decode("S:" + base64Encode(old.data(), old.size()), &d2) && d2.bypassAll == 0 && d2.freq[3] == 12345, "the older saved form (no bypass) loads");
    // the presets are the plug-in's: a look at the ones with the most in them
    const vegmb::Settings& a = vegmb::presets()[2].s;
    check(a.threshold[0] == -250 && a.amount[3] == 300 && a.freq[3] == 1633 && a.width[0] == 7 && a.bypass[3] == 0 && a.sync == 1, "Anti-DTMF talk-off: four notches at 1209..1633 Hz");
    const vegmb::Settings& b = vegmb::presets()[4].s;
    check(b.type[0] == 1 && b.amount[0] == 250 && b.gain[0] == 20 && b.freq[0] == 5000 && b.release[0] == 1000 && b.bypass[1] == 1, "de-esser: band 1 at 5000 Hz");
}

static void testMap()
{
    check(builtinUidForDx("dshow:{026D0AA0-9BB9-11D0-AEBC-00A0C9053912}") == "reno.multibanddynamics", "VEGAS Multi-Band Dynamics's DirectX uid maps to the built-in");
    check(dxUidForBuiltin("reno.multibanddynamics") == "dshow:{026D0AA0-9BB9-11D0-AEBC-00A0C9053912}", "and back");
    check(builtinUidForDx("dshow:{F3B8E880-B4E0-11D0-AEBC-00A0C9053912}") == "reno.graphicdynamics", "Graphic Dynamics's map still works");
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
