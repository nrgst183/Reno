// Gapper/Snipper, Reno's version of VEGAS's DirectX effect (sfppack3_x64.dll, vegasgapper.h): the stream the engine makes
// bit for bit against what the plug-in's own code gave (expected hashes, lengths and samples were made with the DLL), the
// saved form, the presets, and Reno's in-place use of the stream.
//
//   vegasfx_gapper_test
#include "../../src/audiofx/vegasgapper.h"
#include "vegasfx_pack3_common.h"

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

struct Case {
    int freq, len, fade, mode, rate;
    int frames;                 // length of the stream the plug-in made from 24000 input frames
    uint64_t hash;
    uint32_t samples[4];        // output bits at frame 2000 and at the last frame, left and right
};
static const Case kCases[] = {
    {200000, 750, 15, 0, 48000, 56400, 0xb4ec430314f5536aull, {0x3d793a4du, 0x3d72cda7u, 0xb1a61c24u, 0x3bbbacb1u}},
    {100000, 500, 15, 0, 48000, 33600, 0x1b2a3249b8e46505ull, {0x3d793ad4u, 0x3d72ce2au, 0xb0a61c47u, 0x3abbacd8u}},
    {10000000, 10, 15, 0, 48000, 47472, 0xcf8b266ab1fece0cull, {0x00000000u, 0x00000000u, 0xb43adfb0u, 0x3e53224fu}},
    {900000, 100, 15, 0, 48000, 45120, 0xa9956cfbb6d97f72ull, {0x00000000u, 0x00000000u, 0xb38dedf3u, 0x3da05ae0u}},
    {2000000, 50, 15, 1, 48000, 12050, 0xbd58f6c3e5083947ull, {0xbc61a95du, 0xbe966edcu, 0xa73ede32u, 0xb151d07eu}},
    {10000000, 10, 30, 1, 48000, 12144, 0xa89cb19c2c022cf9ull, {0xbc00f30du, 0xbe2bec64u, 0xb3d591edu, 0x3df14bc7u}},
    {3500000, 100, 5, 1, 48000, 5382, 0xf2130921c233a32dull, {0xb6f1405eu, 0x3e4b376cu, 0xa6a8af20u, 0x32ba0fd6u}},
    {50000, 300, 50, 0, 48000, 26880, 0x3169d97c199d7265ull, {0x3ccfcccfu, 0x3cca7189u, 0xb43acadbu, 0x3e530ac5u}},
    {300000, 1000, 0, 1, 48000, 6404, 0x20b8ebb1e7a793e5ull, {0x3895ec9cu, 0xbe4b376cu, 0x00000000u, 0x80000000u}},
    {200000, 750, 15, 0, 44100, 57070, 0x3a39329b90310fa9ull, {0x3d1ad2dbu, 0x3d16d52du, 0xb416a1a8u, 0x3e2a2fcau}},
    {100000, 500, 15, 0, 44100, 35025, 0xe2c80f608b24d566ull, {0x3d793afdu, 0x3d72ce51u, 0xb43ae00du, 0x3e5322b9u}},
    {10000000, 10, 15, 0, 44100, 47452, 0x1054eb16a4e32aa2ull, {0xbe0665cfu, 0x3cbe0542u, 0xb43adfb0u, 0x3e53224fu}},
    {900000, 100, 15, 0, 44100, 45168, 0x1720f46450d1ae01ull, {0x3e487561u, 0x3c532245u, 0xb41708f7u, 0x3e2aa482u}},
    {2000000, 50, 15, 1, 44100, 12120, 0xa4fc06ad17e8a6e4ull, {0x3b4f8b06u, 0x3bb8432fu, 0xb43adfaau, 0x3e532248u}},
    {10000000, 10, 30, 1, 44100, 12150, 0x841edb70c39824cdull, {0xbc1d1d54u, 0x3e93cb37u, 0xa89b0834u, 0x31cddb0cu}},
    {3500000, 100, 5, 1, 44100, 5461, 0x128303aec532bfd6ull, {0xb6f6578cu, 0x3e20fd5cu, 0xa8d5a8c0u, 0xb2361a65u}},
    {50000, 300, 50, 0, 44100, 26646, 0xf6f479e6d090dea1ull, {0x3ce22c42u, 0x3cdc57bfu, 0xb3d0bd53u, 0x3debd6a3u}},
    {300000, 1000, 0, 1, 44100, 6360, 0x597bcaa14efd8e40ull, {0xb8d1a062u, 0xbe55c5f0u, 0xb43adfb0u, 0x3e53224fu}},
};

// the engine over the signal as the DLL harness did: blocks of 512 frames, room for 4096 frames of output
static std::vector<float> stream(const Case& c, const std::vector<float>& in)
{
    veggap::Settings s;
    s.frequency = c.freq; s.length = c.len; s.fade = c.fade; s.mode = c.mode;
    veggap::Engine e;
    e.setup(s, c.rate, true);
    const int n = (int)(in.size() / 2);
    std::vector<float> L((size_t)n), R((size_t)n), out;
    for (int k = 0; k < n; ++k) { L[(size_t)k] = in[(size_t)k * 2]; R[(size_t)k] = in[(size_t)k * 2 + 1]; }
    std::vector<float> tl(4096), tr(4096);
    for (int k = 0; k < n; k += 512) {
        int left = std::min(512, n - k), at = k;
        while (left > 0) {
            const float* ip[2] = {&L[(size_t)at], &R[(size_t)at]};
            float* op[2] = {tl.data(), tr.data()};
            int cons = 0, prod = 0;
            e.process(ip, left, op, 4096, &cons, &prod);
            for (int j = 0; j < prod; ++j) { out.push_back(tl[(size_t)j]); out.push_back(tr[(size_t)j]); }
            at += cons;
            left -= cons;
            if (cons == 0 && prod == 0) return {};
        }
    }
    return out;
}

int main()
{
    const std::vector<float> in = p3test::signal(24000);
    for (const Case& c : kCases) {
        const std::string tag = std::string(c.mode ? "Snipper " : "Gapper ") + std::to_string(c.freq) + "/" + std::to_string(c.len) + "/" + std::to_string(c.fade) + " at " + std::to_string(c.rate) + " Hz";
        const std::vector<float> out = stream(c, in);
        char buf[64];
        std::snprintf(buf, sizeof buf, "%016llx, %d frames", (unsigned long long)p3test::hashBits(out), (int)(out.size() / 2));
        check((int)(out.size() / 2) == c.frames && p3test::hashBits(out) == c.hash, "stream is bit-identical to the plug-in's (" + tag + ")", buf);
        bool same = (int)(out.size() / 2) == c.frames;
        const size_t at[4] = {2000 * 2, 2000 * 2 + 1, (size_t)c.frames * 2 - 2, (size_t)c.frames * 2 - 1};
        for (int k = 0; k < 4 && same; ++k) same = p3test::bitsOf(out[at[k]]) == c.samples[k];
        check(same, "stored samples match (" + tag + ")");
    }
    // saved form: [u32 length 0x24][the effect's GUID][frequency][length][fade][mode]
    {
        veggap::Settings s;
        s.frequency = 100000; s.length = 500; s.fade = 15; s.mode = veggap::Snipper;
        const std::string st = veggap::encode(s);
        const std::string raw = base64Decode(st.substr(2));
        static const unsigned char expect[36] = {0x24, 0, 0, 0, 0x24, 0x1a, 0x90, 0x3f, 0xbe, 0x79, 0xd0, 0x11, 0xae, 0xbc, 0x00, 0xa0, 0xc9, 0x05, 0x39, 0x12,
                                                 0xa0, 0x86, 0x01, 0, 0xf4, 0x01, 0, 0, 0x0f, 0, 0, 0, 0x01, 0, 0, 0};
        check(st.compare(0, 2, "S:") == 0 && raw.size() == 36 && std::memcmp(raw.data(), expect, 36) == 0, "saved form is VEGAS's: length, GUID, settings");
        veggap::Settings back;
        check(veggap::decode(st, &back) && back.frequency == 100000 && back.length == 500 && back.fade == 15 && back.mode == 1, "saved form reads back");
        veggap::Settings wild = s;
        wild.frequency = 0; wild.length = 99999; wild.fade = 90;
        veggap::decode(veggap::encode(wild), &back);
        check(back.frequency == 1 && back.length == 10000 && back.fade == 50, "out-of-range values are brought into range");
    }
    // the percent of the original under the settings
    {
        veggap::Settings s;
        check(veggap::percentOfOriginal(s) == 250, "the default gapper makes 250 %");
        s.mode = veggap::Snipper;
        check(veggap::percentOfOriginal(s) == 40, "the default settings as a snipper make 40 %");
    }
    // the built-in effect
    {
        AudioPluginDesc d;
        d.format = "BUILTIN";
        d.uid = "reno.gapsnip";
        std::string err;
        auto p = builtinAudioCreate(d, &err);
        veggap::Settings s;
        check(p && p->loadState(veggap::encode(s)) && p->saveState() == veggap::encode(s), "the plug-in loads and saves the same form");
        check(p && !p->loadState("garbage") && !p->loadState("S:AAAA"), "other states are refused");
        check(p && p->loadState("reno-builtin-audio 1\nMode=1\n") && p->saveState() == veggap::encode(veggap::Settings()), "a stand-in's old state loads as the defaults");
        check(p && p->paramCount() == 4, "four parameters");
        check(p && p->programCount() == 8 && p->programName(0) == "(Default)" && p->programName(7) == "Very fast clipper", "VEGAS's 8 presets");
        veggap::Settings q;
        check(p && p->setProgram(5) && veggap::decode(p->saveState(), &q) && q.frequency == 2000000 && q.length == 50 && q.mode == 1, "Fast & distorted 1 is 200 Hz, 5 ms, snipper");
    }
    // Reno cuts and inserts in place: the first frames are the stream's own, and the Gapper is silent where it inserts
    {
        const Case& c = kCases[0];
        veggap::Settings s;
        s.frequency = c.freq; s.length = c.len; s.fade = c.fade; s.mode = c.mode;
        const std::vector<float> ref = stream(c, in);
        const std::vector<float> out = p3test::run("reno.gapsnip", veggap::encode(s), c.rate, in);
        bool same = out.size() == in.size();
        for (size_t k = 0; k < 800 && same; ++k) same = out[k] == ref[k];
        check(same, "the first frames out of the effect are the stream's");
        char buf[64];
        std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)p3test::hashBits(out));
        check(out.size() == in.size() && p3test::hashBits(out) == 0x535bf8760de54c63ull, "in-place output of the default gapper is unchanged", buf);
        s.mode = veggap::Snipper;
        const std::vector<float> out2 = p3test::run("reno.gapsnip", veggap::encode(s), 44100, in, 77);
        std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)p3test::hashBits(out2));
        check(out2.size() == in.size() && p3test::hashBits(out2) == 0x8268141aa508f7a0ull, "in-place output of the default snipper (block 77) is unchanged", buf);
    }
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
