// Tests of Reno's VEGAS Graphic EQ (src/audiofx/vegasgraphiceq.h and the built-in "reno.graphiceq"): the sound is
// compared with samples that the real sfppack2_x64.dll produced (its own set-format and process code, run on Linux
// with a harness: the DLL is not needed here), the band faders' envelope with what the plug-in's page code made of it,
// plus the saved form, the presets and the DirectX map.
//
//   vegasfx_graphiceq_test
// (compiled with -DMKREF it writes the settings of the sound cases as case<k>.bin, for the harness to make the references from)
#include "../../src/audiofx/audioplugin.h"
#include "../../src/audiofx/vegasfx_pack2.h"
#include "../../src/audiofx/vegasgraphiceq.h"
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

// The settings of the sound cases
static veggeq::Settings makeSettings(int which)
{
    veggeq::Settings s = veggeq::presets()[0].s;
    switch (which) {
    case 0: s = veggeq::presets()[3].s; break;
    case 1: s = veggeq::presets()[8].s; break;
    case 2: s = veggeq::presets()[2].s; s.accuracy = 0; break;
    case 3: s = veggeq::presets()[6].s; s.accuracy = 2; s.outGain = 45; break;
    case 4: {   // 64 points, from a fixed sequence
        s.accuracy = 2;
        s.outGain = -30;
        s.count = 64;
        uint32_t r = 777u;
        int x = 0;
        for (int k = 0; k < 64; ++k) {
            r = r * 1664525u + 1013904223u;
            x = k == 0 ? 0 : (k == 63 ? 10000 : std::min(9990, x + 20 + (int)((r >> 16) % 250)));
            r = r * 1664525u + 1013904223u;
            s.pt[k][0] = x;
            s.pt[k][1] = ((r >> 16) % 9 == 0) ? -500 : (int)((r >> 20) % 700) - 300;
        }
        break;
    }
    case 5: {   // made with the band faders
        int32_t p[veggeq::kMaxPoints + 8][2] = {{0, 0}, {10000, 0}};
        int n = 2;
        veggeq::bandToPoints(p, &n, 4, 60, 250);
        veggeq::bandToPoints(p, &n, 10, -60, 250);
        veggeq::bandToPoints(p, &n, 19, 100, 250);
        veggeq::bandToPoints(p, &n, 3, 90, 500);
        s.count = n;
        for (int k = 0; k < n; ++k) s.pt[k][0] = p[k][0], s.pt[k][1] = p[k][1];
        break;
    }
    }
    return s;
}

struct Ref {
    int rate;
    uint32_t crc;           // of all complete blocks the plug-in output (interleaved float)
    uint32_t at[8];         // samples 1200, 1201, 9650, 9651, 19300, 19301, 40001, 40002 (as float bits)
};
static const Ref kRefs[] = {
    {48000, 0x4d4a8b0fu, {0x3e4a0f3du, 0x3e7d7528u, 0x3dd3bc5au, 0xbe4271abu, 0xbaedcc18u, 0xbbfe14dcu, 0x3e96cd74u, 0x3d947062u}},
    {44100, 0x7575ccd3u, {0x3e38993du, 0x3e7be88eu, 0x3da7527bu, 0xbe13176eu, 0xbc72fdcau, 0x3b24d200u, 0x3eabffa6u, 0x38b14b00u}},
    {48000, 0x694ca19bu, {0x3e354247u, 0x3e9f619au, 0x3ca3f056u, 0xbe3c1a92u, 0xba3f1225u, 0x3a4f7904u, 0x3e8ab12eu, 0x3d164a8fu}},
    {44100, 0xc02c4a7cu, {0x3f9195b8u, 0x3ef218e5u, 0xbeb699e9u, 0xbe1b271au, 0x39d54a4au, 0xbb9d5593u, 0x3eaeed5fu, 0xbe4d753au}},
    {48000, 0x984b0195u, {0xbda0b93fu, 0x3e6f3feeu, 0x3b633605u, 0xbe75a923u, 0xbe6d312fu, 0xbce0530fu, 0x3e97c70eu, 0x3dc4b102u}},
    {44100, 0x54ef2d52u, {0x3f9fa251u, 0x3d55d9ecu, 0xbe06e3e4u, 0xb93efe00u, 0x3b2657b2u, 0xb9d744d8u, 0xbda3b943u, 0x3d2306e8u}},
};

static std::vector<float> runDsp(const veggeq::Settings& s, int rate, const std::vector<float>& in, int frames)
{
    veggeq::Dsp d;
    d.setSettings(s);
    d.setFormat(rate, 2);
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
    const int frames = 60000;
    const std::vector<float> in = makeInput(frames);
    static const int idx[8] = {1200, 1201, 9650, 9651, 19300, 19301, 40001, 40002};
    int k = 0;
    for (const Ref& r : kRefs) {
        const veggeq::Settings s = makeSettings(k);
        const std::vector<float> out = runDsp(s, r.rate, in, frames);
        const std::string tag = strfmt("case %d: %d Hz, accuracy %d, %d points, output %.1f dB", k, r.rate, s.accuracy, s.count, s.outGain / 10.0);
        check(crcOf((const uint8_t*)out.data(), out.size() * 4) == r.crc, "sound equals the DLL's: " + tag);
        bool same = true;
        for (int j = 0; j < 8; ++j) same = same && bitsOf(out[(size_t)idx[j]]) == r.at[j];
        check(same, "  and its stored samples");
        ++k;
    }
}

static void testPlugin()
{
    const int frames = 30000;
    const std::vector<float> in = makeInput(frames);
    AudioPluginDesc d;
    d.format = "BUILTIN";
    d.uid = "reno.graphiceq";
    std::string err;
    std::unique_ptr<AudioPlugin> p = builtinAudioCreate(d, &err);
    check(p != nullptr, "the built-in Graphic EQ is made");
    if (!p) return;
    check(p->paramCount() == 2, "it has the accuracy and the output gain as settings");
    check(p->programCount() == 9, "it has VEGAS's 9 presets");
    check(p->programName(0) == "(Default)" && p->programName(1) == "Default all parameters" && p->programName(2) == "Roll-off frequencies below 120 Hz" &&
              p->programName(8) == "Notch frequencies around 450 Hz",
          "  with their names");
    // settings in the VEGAS saved form
    veggeq::Settings s = makeSettings(4);
    s.band[3] = -100;
    s.band[19] = 240;
    check(p->loadState(veggeq::encode(s)), "the VEGAS saved form loads");
    check(p->saveState() == veggeq::encode(s), "  and saves back the same (all 151 words)");
    const std::string st = p->saveState();
    check(st.compare(0, 2, "S:") == 0 && base64Decode(st.substr(2)).size() == 0x270, "  as 0x270 bytes after S:");
    check(p->paramText(1) == "-3.0 dB", "the output gain is shown in dB");
    check(p->loadState("reno-builtin-audio 1\nAccuracy=2\n") && p->saveState() == veggeq::encode(veggeq::presets()[0].s), "settings of an older stand-in give the defaults");
    check(!p->loadState("garbage"), "garbage is refused");
    // the delay is half the filter size
    for (int a = 0; a < 3; ++a) {
        veggeq::Settings t = veggeq::presets()[0].s;
        t.accuracy = a;
        p->loadState(veggeq::encode(t));
        check(p->latencySamples() == (a == 0 ? 128 : a == 1 ? 512 : 2048), strfmt("accuracy %d is %d frames late", a, a == 0 ? 128 : a == 1 ? 512 : 2048));
    }
    // the sound: a block late, whatever the buffer sizes
    const int rate = 48000;
    veggeq::Settings sound = makeSettings(0);
    p->loadState(veggeq::encode(sound));
    p->activate(rate, 512);
    const int hop = p->latencySamples();
    const std::vector<float> want = runDsp(sound, rate, in, frames);
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
    for (int i = 0; i < hop; ++i) silent = silent && l[(size_t)i] == 0.f && rr[(size_t)i] == 0.f;
    for (size_t k = 0; k + (size_t)hop < (size_t)frames && (k + 1) * 2 <= want.size(); ++k)
        same = same && bitsOf(l[k + (size_t)hop]) == bitsOf(want[k * 2]) && bitsOf(rr[k + (size_t)hop]) == bitsOf(want[k * 2 + 1]);
    check(silent, "the first block is silent");
    check(same, "the rest is the DLL's sound, a block later, whatever the buffer sizes");
    // a jump in time starts it again
    std::vector<float> l2(l.begin(), l.begin() + 2000), r2(rr.begin(), rr.begin() + 2000);
    p->process(l2.data(), r2.data(), 2000, 5000000, true);
    bool quiet = true;
    for (int i = 0; i < hop; ++i) quiet = quiet && l2[(size_t)i] == 0.f;
    check(quiet, "after a jump the first block is silent again");
    // a change of the accuracy while it runs: the new size is used from the next call, with a silent first block
    {
        veggeq::Settings t = sound;
        t.accuracy = 2;
        p->loadState(veggeq::encode(t));
        std::vector<float> l3(l.begin(), l.begin() + 6000), r3(rr.begin(), rr.begin() + 6000);
        p->process(l3.data(), r3.data(), 6000, 5002000, true);
        bool sil = true;
        for (int i = 0; i < 2048; ++i) sil = sil && l3[(size_t)i] == 0.f;
        bool live = false;
        for (int i = 2048; i < 6000; ++i) live = live || l3[(size_t)i] != 0.f;
        check(sil && live && p->latencySamples() == 2048, "a new accuracy changes the delay to 2048 frames and goes on");
    }
    // presets
    check(p->setProgram(3) && p->saveState() == veggeq::encode(veggeq::presets()[3].s), "a preset sets the settings");
    const veggeq::Settings b = veggeq::presets()[3].s;
    check(b.count == 6 && b.pt[2][0] == 1250 && b.pt[2][1] == 202 && b.pt[3][0] == 2750, "  \"Boost bass around 80 Hz by 6 dB\" is the VEGAS curve");
}

static void testFormat()
{
    veggeq::Settings s = makeSettings(4);
    for (int k = 0; k < 20; ++k) s.band[k] = k * 20 - 300;
    const std::string e = veggeq::encode(s);
    veggeq::Settings back;
    check(veggeq::decode(e, &back) && std::memcmp(&back, &s, sizeof s) == 0, "the saved form round trips");
    const std::string raw = base64Decode(e.substr(2));
    uint32_t len = 0;
    std::memcpy(&len, raw.data(), 4);
    check(len == 0x270 && raw.size() == 0x270, "  with the length 0x270 in front");
    static const uint8_t guid[16] = {0xee, 0x2a, 0x72, 0x8b, 0x84, 0x45, 0xd1, 0x11, 0xb4, 0xcb, 0x00, 0xa0, 0xc9, 0x27, 0x0a, 0x10};
    check(std::memcmp(raw.data() + 4, guid, 16) == 0, "  and the persist GUID 8b722aee-4584-11d1-b4cb-00a0c9270a10");
    std::string bad = raw;
    bad[4] ^= 1;
    check(!veggeq::decode("S:" + base64Encode(bad.data(), bad.size()), &back), "another GUID is refused");
    bad = raw;
    bad[0] = 0x71;
    check(!veggeq::decode("S:" + base64Encode(bad.data(), bad.size()), &back), "so is another length");
    // out of range values are brought into range, as the plug-in does
    veggeq::Settings w = s;
    w.accuracy = 9;
    w.outGain = 999;
    w.band[0] = -999;
    w.count = 1;
    w.pt[0][0] = -5;
    w.pt[0][1] = 900;
    veggeq::sanitize(w);
    check(w.accuracy == 2 && w.outGain == 200 && w.band[0] == -480 && w.count == 2 && w.pt[0][0] == 0 && w.pt[0][1] == 400, "out of range settings are clamped");
    // the saved state of a Graphic EQ in a real project (VEGAS Pro 18), "boost the bass and cut the rest": 5 points, high accuracy
    {
        const std::string real = "S:cAIAAO4qcouERdERtMsAoMknChACAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAkAEAAMMHAACQAQAAxAcAABIAAACXDwAAAgAAABAnAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAUAAAAAAAAA";
        veggeq::Settings rs;
        check(veggeq::decode(real, &rs) && rs.accuracy == 2 && rs.count == 5 && rs.pt[0][0] == 0 && rs.pt[0][1] == 400 && rs.pt[1][0] == 1987 && rs.pt[4][0] == 10000 && rs.outGain == 0,
              "a Graphic EQ saved by VEGAS in a real project loads");
        check(veggeq::encode(rs) == real, "  and saves back the same bytes");
    }
    check(veggeq::presets().size() == 9 && veggeq::presets()[7].s.count == 3 && veggeq::presets()[7].s.pt[2][1] == 150, "the presets are VEGAS's");
}

// What the plug-in's page code made of the band faders (the harness called it with a copy of the envelope)
struct BandRef {
    int band, tenths, w;
    std::vector<int> in, out;
};
static const BandRef kBands[] = {
    {4, 60, 250, {0, 0, 10000, 0}, {0, 0, 1500, 0, 1750, 200, 2250, 200, 2500, 0, 10000, 0}},
    {0, 120, 250, {0, 0, 10000, 0}, {0, 300, 250, 300, 500, 0, 10000, 0}},
    {19, -100, 250, {0, 0, 10000, 0}, {0, 0, 9000, 0, 9250, -274, 10000, -274}},
    {5, 240, 500, {0, 0, 10000, 0}, {0, 0, 1500, 0, 2000, 400, 3000, 400, 3500, 0, 10000, 0}},
    {1, -480, 500, {0, 0, 10000, 0}, {0, 0, 0, -500, 1000, -500, 1500, 0, 10000, 0}},
    {9, 90, 500, {0, 0, 1000, 0, 1250, 202, 2750, 202, 3000, 0, 10000, 0}, {0, 0, 1000, 0, 1250, 202, 2750, 202, 3000, 0, 4000, 258, 5000, 258, 5500, 0, 10000, 0}},
    {10, -60, 250, {0, 0, 4000, 0, 4250, -500, 4750, -500, 5000, 0, 10000, 0}, {0, 0, 4000, 0, 4250, -500, 4750, -200, 5250, -200, 5500, 0, 10000, 0}},
    {19, 100, 500, {0, 0, 5000, 0, 10000, 150}, {0, 0, 5000, 0, 8500, 105, 9000, 274, 10000, 274}},
    {3, 30, 250, {0, 0, 1000, 100, 1100, 100, 1500, 20, 2000, 20, 10000, -50}, {0, 0, 1000, 100, 1750, 100, 2000, 20, 10000, -50}},
    {1, 45, 250, {0, 0, 500, 0, 10000, 0}, {0, 158, 750, 158, 1000, 0, 10000, 0}},
};
struct PosRef {
    int tenths, pos;
};
static const PosRef kPos[] = {{-480, -500}, {-479, -500}, {-300, -432}, {-100, -274}, {-31, -105}, {-30, -100}, {-29, -98}, {0, 0}, {1, 5}, {29, 98}, {30, 100}, {31, 105}, {60, 200}, {100, 274}, {150, 332}, {239, 399}, {240, 400}};
struct CleanRef {
    std::vector<int> in, out;
};
static const CleanRef kClean[] = {
    {{300, 50, 0, 0, 100, 0, 5000, 0, 10000, 0, 150, 5}, {0, 0, 100, 0, 300, 50, 5000, 0, 10000, 0}},
    {{0, 0, 10, 5, 40, 3, 4000, 400, 4050, 405, 10000, 0}, {0, 0, 4000, 400, 10000, 0}},
    {{0, 600, 5000, -700, 10000, 0}, {0, 400, 5000, -500, 10000, 0}},
    {{0, 0, 2000, 10, 4000, 10, 6000, 10, 8000, 10, 10000, 0}, {0, 0, 2000, 10, 8000, 10, 10000, 0}},
};

static void testBands()
{
    bool ok = true;
    for (const PosRef& r : kPos) ok = ok && veggeq::posOfTenths(r.tenths) == r.pos;
    check(ok, "a fader's level becomes the plug-in's slider position (-Inf. -500, 3 dB 100, 6 dB 200, 24 dB 400)");
    int k = 0;
    for (const BandRef& r : kBands) {
        int32_t p[veggeq::kMaxPoints + 8][2] = {};
        int n = (int)r.in.size() / 2;
        for (int i = 0; i < n; ++i) p[i][0] = r.in[(size_t)2 * i], p[i][1] = r.in[(size_t)2 * i + 1];
        veggeq::bandToPoints(p, &n, r.band, r.tenths, r.w);
        bool same = n == (int)r.out.size() / 2;
        for (int i = 0; i < n && same; ++i) same = p[i][0] == r.out[(size_t)2 * i] && p[i][1] == r.out[(size_t)2 * i + 1];
        check(same, strfmt("band %d at %.1f dB (width %d) makes the plug-in's trapezoid (case %d)", r.band, r.tenths / 10.0, r.w, k));
        ++k;
    }
    k = 0;
    for (const CleanRef& r : kClean) {
        int32_t p[veggeq::kMaxPoints + 8][2] = {};
        int n = (int)r.in.size() / 2;
        for (int i = 0; i < n; ++i) p[i][0] = r.in[(size_t)2 * i], p[i][1] = r.in[(size_t)2 * i + 1];
        veggeq::cleanPoints(p, &n);
        bool same = n == (int)r.out.size() / 2;
        for (int i = 0; i < n && same; ++i) same = p[i][0] == r.out[(size_t)2 * i] && p[i][1] == r.out[(size_t)2 * i + 1];
        check(same, strfmt("the clean up of an envelope is the plug-in's (case %d)", k));
        ++k;
    }
}

static void testMap()
{
    check(builtinUidForDx("dshow:{8B7226EE-4584-11D1-B4CB-00A0C9270A10}") == "reno.graphiceq", "VEGAS Graphic EQ's DirectX uid maps to the built-in");
    check(dxUidForBuiltin("reno.graphiceq") == "dshow:{8B7226EE-4584-11D1-B4CB-00A0C9270A10}", "and back");
    check(pack2VegasName("reno.graphiceq") == "VEGAS Graphic EQ", "its VEGAS name");
    check(builtinUidForDx("dshow:{B97C0F23-196D-11D1-B99B-00A0C9053912}") == "reno.noisegate" && builtinUidForDx("dshow:{39224540-6F92-11D0-AEBC-00A0C9053912}") == "reno.distortion",
          "the other maps still work");
}

int main()
{
#ifdef MKREF
    for (int k = 0; k < 6; ++k) {
        const veggeq::Settings s = makeSettings(k);
        char name[64];
        std::snprintf(name, sizeof name, "case%d.bin", k);
        FILE* f = std::fopen(name, "wb");
        std::fwrite(&s, sizeof s, 1, f);
        std::fclose(f);
    }
    return 0;
#else
    testDsp();
    testPlugin();
    testFormat();
    testBands();
    testMap();
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
#endif
}
