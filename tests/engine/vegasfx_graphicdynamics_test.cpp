// Tests of Reno's VEGAS Graphic Dynamics (src/audiofx/vegasgdyn.h and the built-in "reno.graphicdynamics"): the sound is
// compared with the real sfppack2_x64.dll's (its block engine, run on Linux with a harness; the DLL is not needed here),
// including settings that change while the sound runs, a graph that does not reach the ends, and a restart of the
// stream, plus the saved form, the presets, the graph edits of the page and the DirectX map.
//
//   vegasfx_graphicdynamics_test
#include "../../src/audiofx/audioplugin.h"
#include "../../src/audiofx/vegasfx_pack2.h"
#include "../../src/audiofx/vegasgdyn.h"
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

// A case: the rate, the size of the buffers, the stretches of the sound ("frames:" and the 41 numbers of the plug-in's
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
    {44100, 512, {"0:0,10,5000,1,1,-840,-840,-614,-840,-150,-150,0,-119,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,-210,10,0"}, 0x8823889eu, {0x00000000u, 0x3d11b8a2u, 0x3e013ab0u, 0xbd6feec7u, 0x3e896e35u, 0xbf028fd6u, 0xb95db50du, 0x3a7b0072u}},
    {48000, 512, {"0:0,50,1000,0,1,-840,-840,-122,-124,-57,-78,0,-68,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,-210,10,0"}, 0xec23eda4u, {0x00000000u, 0x3e8aaffcu, 0xbe9bde7cu, 0x3e0bf240u, 0xbeb8574cu, 0x3e374979u, 0xb94b56fcu, 0x3aa95c32u}},
    {44100, 1000, {"0:0,100,3000,1,0,-840,-840,-456,-840,-419,-562,-334,-361,-240,-238,-110,-150,0,-119,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,7,-210,10,0"}, 0x893547b7u, {0x00000000u, 0x3d078eaeu, 0x3df55740u, 0xbd63c15bu, 0x3e827496u, 0xbef74be5u, 0xb949f85bu, 0x3a0fa75du}},
    {48000, 97, {"4990:0,10,5000,0,1,-840,-840,-614,-840,-150,-150,0,-119,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,-210,10,0", "6000:60,2000,500,1,0,-840,-840,-122,-124,-57,-78,0,-68,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,-210,10,0:d", "0:-30,0,0,0,1,-840,-840,-456,-840,-419,-562,-334,-361,-240,-238,-110,-150,0,-119,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,7,-210,10,0"}, 0x3fe59d90u, {0x00000000u, 0x3e19f985u, 0x00000000u, 0x00000000u, 0x00000000u, 0x3f1c2d9bu, 0xba6750b4u, 0x00000000u}},
    {44100, 4096, {"0:0,20,2500,0,1,-300,-300,-120,-200,-50,-40,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,-210,10,0"}, 0x3a349e5du, {0x00000000u, 0x3ca56a73u, 0x3d92c0edu, 0xbd083c10u, 0x3e1c1127u, 0xbe938acau, 0xb90228f4u, 0x3a51f7a0u}},
    {44100, 441, {"0:60,0,100,1,1,-840,-840,-300,-300,0,-200,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,-210,10,0"}, 0x9f0ecfbeu, {0x00000000u, 0x3d8524f3u, 0x3e6c2bf2u, 0xbddb3e4cu, 0x3efb28e5u, 0xbf6e096bu, 0xbb1dfc72u, 0x3c80f22fu}},
    {48000, 1000, {"7777:0,10,5000,0,1,-840,-840,-240,-240,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,-210,10,0", "0:-100,300,200,1,1,-840,-840,-614,-840,-150,-150,0,-119,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,-210,10,0"}, 0x17560929u, {0x00000000u, 0x3eaeb000u, 0xbec43c00u, 0x3e303000u, 0xbee81400u, 0x3dadf1b7u, 0xb8a9f239u, 0x393ef7ebu}},
};

static veggd::Settings parseSettings(const char* seg, long* frames, bool* restart)
{
    char* q = const_cast<char*>(seg);
    *frames = strtol(q, &q, 10);
    ++q;
    int32_t v[41] = {0};
    for (int k = 0; k < 41; ++k) {
        v[k] = (int32_t)strtol(q, &q, 10);
        if (*q == ',') ++q;
    }
    *restart = *q == ':';
    veggd::Settings s;
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
        d.uid = "reno.graphicdynamics";
        std::unique_ptr<AudioPlugin> p = builtinAudioCreate(d, nullptr);
        p->activate(r.rate, 512);
        std::vector<float> got;
        long pos = 0, jump = 0;
        std::vector<float> l((size_t)r.buffer), rr((size_t)r.buffer);
        for (const char* seg : r.segs) {
            long n = 0;
            bool restart = false;
            const veggd::Settings s = parseSettings(seg, &n, &restart);
            p->loadState(veggd::encode(s));
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

static void testBypass()
{
    AudioPluginDesc d;
    d.format = "BUILTIN";
    d.uid = "reno.graphicdynamics";
    std::unique_ptr<AudioPlugin> p = builtinAudioCreate(d, nullptr);
    p->activate(44100, 512);
    veggd::Settings s = veggd::presets()[2].s;
    s.bypass = 1;
    p->loadState(veggd::encode(s));
    const std::vector<float> in = makeInput(4096);
    std::vector<float> l(2048), r(2048);
    bool same = true;
    for (int k = 0; k < 2048; ++k) {
        l[(size_t)k] = in[(size_t)k * 2];
        r[(size_t)k] = in[(size_t)k * 2 + 1];
    }
    const std::vector<float> l0 = l, r0 = r;
    p->process(l.data(), r.data(), 2048, 0, true);
    same = l == l0 && r == r0;
    check(same, "bypassed, the sound goes through as it is, with no delay");
}

static void testPlugin()
{
    AudioPluginDesc d;
    d.format = "BUILTIN";
    d.uid = "reno.graphicdynamics";
    std::string err;
    std::unique_ptr<AudioPlugin> p = builtinAudioCreate(d, &err);
    check(p != nullptr, "the built-in Graphic Dynamics is made");
    if (!p) return;
    p->activate(44100, 512);
    check(p->paramCount() == 6, "it has attack, release, auto gain, sync, output gain and bypass");
    check(p->programCount() == 11, "it has VEGAS's 11 presets");
    check(p->programName(2) == "Compressor/Gate" && p->programName(9) == "Soft knee compressor/gate (-24 dB threshold)", "  with their names");
    check(p->latencySamples() == 882, "it is one 20 ms block late");
    veggd::Settings s = veggd::presets()[2].s;
    s.attack = 123;
    s.release = 4567;
    s.outGain = -77;
    s.autoGain = 0;
    s.sync = 0;
    check(p->loadState(veggd::encode(s)), "the VEGAS saved form loads");
    check(p->saveState() == veggd::encode(s), "  and saves back the same");
    veggd::Settings back;
    check(veggd::decode(p->saveState(), &back) && back.attack == 123 && back.release == 4567 && back.outGain == -77 && back.count == s.count, "  with the same settings");
    const std::string st = p->saveState();
    check(st.compare(0, 2, "S:") == 0 && base64Decode(st.substr(2)).size() == 0xb8, "  as 0xb8 bytes after S:");
    check(p->paramText(0) == "12.3 ms" && p->paramText(4) == "-7.7 dB", "times in ms, the gain in dB");
    check(p->loadState("reno-builtin-audio 1\nWet=3\n") && p->saveState() == veggd::encode(veggd::presets()[0].s), "settings of an older stand-in give the defaults");
    check(!p->loadState("garbage"), "garbage is refused");
    check(p->setProgram(6) && p->saveState() == veggd::encode(veggd::presets()[6].s), "a preset sets the settings");
    // the saved threshold and ratio come from the graph
    veggd::Settings c = veggd::presets()[8].s;       // 3:1 compression starting at -15 dB
    c.threshold = -3;
    c.ratio = 77;
    veggd::Settings d2;
    check(veggd::decode(veggd::encode(c), &d2) && d2.threshold == -150 && d2.ratio == 30, "the threshold and the ratio are found from the graph when it is saved");
    // an old form (without the bypass) loads
    std::string old = base64Decode(veggd::encode(c).substr(2));
    old.resize(old.size() - 4);
    const uint32_t oldLen = (uint32_t)old.size();
    std::memcpy(&old[0], &oldLen, 4);
    std::memcpy(&old[4], veggd::persistGuidOld(), 16);
    check(veggd::decode("S:" + base64Encode(old.data(), old.size()), &d2) && d2.bypass == 0 && d2.count == c.count, "the older saved form (no bypass) loads");
}

static void testEdits()
{
    // what the page's sliders and Reset Graph do to the graph (checked against the DLL's own routines)
    veggd::Settings s = veggd::presets()[8].s;       // -15 dB knee, 3:1
    veggd::setRatio(s, 50);
    check(s.pt[2][1] == -120 && s.count == 3, "ratio 5.0:1 puts the last point at -12 dB");
    veggd::setThreshold(s, -300);
    check(s.pt[1][0] == -300 && s.pt[1][1] == -300 && s.pt[2][1] == -240, "threshold -30 dB moves the knee along the diagonal and keeps the ratio");
    veggd::setRatio(s, 500);
    check(s.pt[2][1] == s.pt[1][1], "ratio Inf.:1 makes the last point level with the knee");
    veggd::resetGraph(s);
    check(s.count == 3 && s.pt[1][0] == -210 && s.pt[1][1] == -210 && s.threshold == -210 && s.ratio == 10, "Reset Graph: three points, threshold -21 dB, ratio 1.0:1");
}

static void testMap()
{
    check(builtinUidForDx("dshow:{F3B8E880-B4E0-11D0-AEBC-00A0C9053912}") == "reno.graphicdynamics", "VEGAS Graphic Dynamics's DirectX uid maps to the built-in");
    check(dxUidForBuiltin("reno.graphicdynamics") == "dshow:{F3B8E880-B4E0-11D0-AEBC-00A0C9053912}", "and back");
    check(builtinUidForDx("dshow:{D616F3E0-D622-11CE-AAC5-0020AF0B99A3}") == "reno.paragraphiceq", "Paragraphic EQ's map still works");
}

int main()
{
    testDsp();
    testBypass();
    testPlugin();
    testEdits();
    testMap();
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
