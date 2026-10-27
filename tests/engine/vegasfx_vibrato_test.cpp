// Vibrato, Reno's version of VEGAS's DirectX effect (sfppack3_x64.dll, vegasvibrato.h): the stream the engine makes bit for bit
// against what the plug-in's own code gave (expected hashes, lengths and samples were made with the DLL), the saved form
// (one from a real project among them), the presets, and Reno's in-place use of the stream.
//
//   vegasfx_vibrato_test
#include "../../src/audiofx/vegasvibrato.h"
#include "vegasfx_pack3_common.h"

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// "blend,semitones,frequency,gain,bypass|x,y;x,y;..." (the plug-in's own units: semitones x100, Hz x1000, tenths of a dB)
static vegvib::Settings parse(const char* s)
{
    vegvib::Settings st;
    std::memset(st.pts, 0, sizeof st.pts);
    int blend = 0, semis = 0, freq = 0, gain = 0, byp = 0;
    const char* bar = std::strchr(s, '|');
    std::sscanf(s, "%d,%d,%d,%d,%d", &blend, &semis, &freq, &gain, &byp);
    st.blendEdges = blend; st.semitones = semis; st.frequency = freq; st.gain = gain; st.bypass = byp;
    const char* p = bar + 1;
    int n = 0;
    while (*p) {
        int x = 0, y = 0, c = 0;
        if (std::sscanf(p, "%d,%d%n", &x, &y, &c) < 2) break;
        st.pts[2 * n] = x; st.pts[2 * n + 1] = y;
        ++n;
        p += c;
        if (*p == ';') ++p;
    }
    st.count = n;
    return st;
}

struct Case {
    const char* name;
    const char* a;              // the settings at the start
    const char* b;              // the settings from 'switchAt' on ("-": no change)
    int switchAt;               // input frame
    int disc;                   // input frame where the sound jumps (-1: never)
    int rate, block, cap;
    int frames;                 // length of the stream the plug-in made from 24000 input frames
    uint64_t hash;
    uint64_t callsHash;         // what each call took and gave
    uint32_t samples[4];        // output bits at frame 2000 and at the last frame, left and right
};
static const Case kCases[] = {
    {"flat", "0,1200,1000,0,0|0,0;1000,0", "-", 0, -1, 48000, 512, 4096, 23999, 0x645f4342b0d1ec0dull, 0x4edb93a618c390b1ull, {0x3d793a80u, 0x3d72cdd8u, 0x3208f340u, 0x3e4ddb0du}},
    {"up", "0,1200,1000,0,0|0,100;1000,100", "-", 0, -1, 48000, 512, 4096, 12000, 0x65bd6aa3ae3ae955ull, 0xa069cea7397a5fc8ull, {0xbcf75bb4u, 0xbec34c89u, 0x3208f340u, 0x3e4ddb0du}},
    {"down", "0,1200,1000,0,0|0,-100;1000,-100", "-", 0, -1, 48000, 512, 4096, 47998, 0x4a56b8d5a3322d25ull, 0x3fe9a895d80417b6ull, {0x3e07cfc4u, 0xbdc34c8au, 0xb3b2507cu, 0x3e507eaeu}},
    {"vib1", "0,1200,5000,0,0|0,0;250,100;750,-100;1000,0", "-", 0, -1, 48000, 512, 4096, 21481, 0xbf0a79455daa63ebull, 0x85ca6787cc2f75d6ull, {0xba88f73fu, 0x3e3e7d11u, 0xb4c81dc7u, 0x3e498943u}},
    {"vib2", "0,1200,9000,0,0|0,0;200,90;250,100;300,90;700,-90;750,-100;800,-90;1000,0", "-", 0, -1, 48000, 512, 4096, 21831, 0x0ea44720f5c56aa0ull, 0xb9aede7c0e8d9162ull, {0x3b8b37f5u, 0x3ebdf9afu, 0xb2a52eb7u, 0x3e4ea4ffu}},
    {"vib3", "0,1200,9000,0,0|0,0;50,100;450,100;550,-100;950,-100;1000,0", "-", 0, -1, 48000, 512, 4096, 18344, 0x84cb04975c0d4cc9ull, 0xb4c69c21ea640ecbull, {0xbc28c9a7u, 0x3e191913u, 0xb4312505u, 0x3e52df0au}},
    {"weird", "0,1200,9000,0,0|0,0;50,90;100,100;150,90;350,-70;400,-80;450,-70;600,20;650,30;700,40;800,70;850,60;1000,0", "-", 0, -1, 48000, 512, 4096, 20003, 0x9bdb749c1b3f5ba2ull, 0xe672432691bdf969ull, {0xbc40083cu, 0x3db84b39u, 0xb436d132u, 0x3e4bd560u}},
    {"tri", "0,600,9000,0,0|0,-100;500,100", "-", 0, -1, 48000, 512, 4096, 23805, 0x80bc15b963795b6cull, 0xbdf23a33a94b4654ull, {0xbe2735c9u, 0xbe472bfdu, 0xb3a411eeu, 0x3e4ce5feu}},
    {"tri3", "0,600,9000,0,0|0,-100;500,100;1000,-100", "-", 0, -1, 48000, 512, 4096, 23642, 0xa7220ab86f5120cfull, 0xd288efcca879d647ull, {0xbe2735c9u, 0xbe472bfdu, 0xb30ac3eeu, 0x3e4f0618u}},
    {"blend2", "1,2400,20000,-60,0|0,100;1000,-60", "-", 0, -1, 48000, 512, 4096, 15013, 0x0b34061a570dd04aull, 0x26f8f999c7429784ull, {0xbb8302e0u, 0xbd976796u, 0xb21443cdu, 0x3dcf13a5u}},
    {"blend3", "1,1200,3000,30,0|0,40;30,10;970,-50;1000,80", "-", 0, -1, 48000, 512, 4096, 26120, 0xbeaa04d955d3ed58ull, 0xcefab3f538c2314full, {0x3c5ef0d3u, 0x3e67f8ccu, 0xb3f39c59u, 0x3e933295u}},
    {"blend4", "1,1200,3000,0,0|0,40;60,10;940,-50;1000,80", "-", 0, -1, 48000, 512, 4096, 25618, 0x40aaf57591fb2e8aull, 0x7ae2efd20224410eull, {0xbb1434c4u, 0x3e470a00u, 0xb3db531bu, 0x3e510c75u}},
    {"blend6", "1,1200,2500,0,0|0,0;500,100;500,-100;1000,0", "-", 0, -1, 48000, 512, 4096, 22112, 0x61d6a973c526807bull, 0xb86f93573a7513a0ull, {0xbbffbb1eu, 0xbe2a78bcu, 0xb3d814fcu, 0x3e51013fu}},
    {"mute", "0,1200,1000,-800,0|0,0;1000,0", "-", 0, -1, 48000, 512, 4096, 23999, 0x45c17cf09fc1eba3ull, 0x4edb93a618c390b1ull, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {"gain-", "0,600,7777,-123,0|0,30;400,-80;1000,50", "-", 0, -1, 48000, 512, 4096, 25360, 0xa7311ea6cee1bfacull, 0x959eef101c2eb081ull, {0x3bbdded5u, 0x3cbceb53u, 0xb2cdf288u, 0x3d4ad171u}},
    {"dup", "0,1200,3000,0,0|0,50;300,20;300,-30;1000,0", "-", 0, -1, 48000, 512, 4096, 22769, 0x62f869bd677529b2ull, 0xf1f39d6758cd1cc2ull, {0x3c1041b6u, 0x3e6147d3u, 0xb3ff8be7u, 0x3e5189adu}},
    {"fast", "0,2400,1000000,0,0|0,-100;500,100;1000,-100", "-", 0, -1, 48000, 512, 4096, 17733, 0x9bf851b2bafc7930ull, 0xfa89e392a2b02634ull, {0xbbc90916u, 0xbe7433d7u, 0xb43abb7eu, 0x3e4bcac9u}},
    {"slow", "0,2400,1,0,0|0,-100;500,100;1000,-100", "-", 0, -1, 48000, 512, 4096, 95731, 0x4c7b98ee461a0da4ull, 0x8e342682d12df203ull, {0xbed4df04u, 0xbe30d322u, 0xb41e25efu, 0x3e525bb2u}},
    {"flat", "0,1200,1000,0,0|0,0;1000,0", "-", 0, -1, 44100, 512, 4096, 23999, 0x645f4342b0d1ec0dull, 0x4edb93a618c390b1ull, {0x3d793a80u, 0x3d72cdd8u, 0x3208f340u, 0x3e4ddb0du}},
    {"up", "0,1200,1000,0,0|0,100;1000,100", "-", 0, -1, 44100, 512, 4096, 12000, 0x65bd6aa3ae3ae955ull, 0xa069cea7397a5fc8ull, {0xbcf75bb4u, 0xbec34c89u, 0x3208f340u, 0x3e4ddb0du}},
    {"down", "0,1200,1000,0,0|0,-100;1000,-100", "-", 0, -1, 44100, 512, 4096, 47998, 0x4a56b8d5a3322d25ull, 0x3fe9a895d80417b6ull, {0x3e07cfc4u, 0xbdc34c8au, 0xb3b2507cu, 0x3e507eaeu}},
    {"vib1", "0,1200,5000,0,0|0,0;250,100;750,-100;1000,0", "-", 0, -1, 44100, 512, 4096, 20857, 0xd44f7734fdf87ed7ull, 0xb933c1d922e8373aull, {0x3b8a79adu, 0x3daaa04eu, 0xb396a404u, 0x3e501f02u}},
    {"vib2", "0,1200,9000,0,0|0,0;200,90;250,100;300,90;700,-90;750,-100;800,-90;1000,0", "-", 0, -1, 44100, 512, 4096, 21167, 0xc1200457a79c48a2ull, 0xb4c7b435f1dc276dull, {0xbbfca626u, 0x3e5e688bu, 0xb3baa805u, 0x3e509b85u}},
    {"vib3", "0,1200,9000,0,0|0,0;50,100;450,100;550,-100;950,-100;1000,0", "-", 0, -1, 44100, 512, 4096, 19731, 0x8927e6411cdab238ull, 0x687409188eb61744ull, {0x3c49f4d7u, 0xbdc33c47u, 0xb42f13cdu, 0x3e4bea50u}},
    {"weird", "0,1200,9000,0,0|0,0;50,90;100,100;150,90;350,-70;400,-80;450,-70;600,20;650,30;700,40;800,70;850,60;1000,0", "-", 0, -1, 44100, 512, 4096, 19812, 0xa77a8b5af3368981ull, 0x84a0c3b74e8f19c9ull, {0xbaff890du, 0xbd3dd629u, 0xb494cb8du, 0x3e4a9ee9u}},
    {"tri", "0,600,9000,0,0|0,-100;500,100", "-", 0, -1, 44100, 512, 4096, 23752, 0x344f88ba5d1c5de9ull, 0x5a05084e34ff7771ull, {0xbdb57090u, 0x3d438122u, 0xb4374d81u, 0x3e53099eu}},
    {"tri3", "0,600,9000,0,0|0,-100;500,100;1000,-100", "-", 0, -1, 44100, 512, 4096, 23317, 0xc5fc8a1d8ee369cfull, 0x3601d3613bb559f3ull, {0xbdb57090u, 0x3d438122u, 0xb389a2a9u, 0x3e4ff20du}},
    {"blend2", "1,2400,20000,-60,0|0,100;1000,-60", "-", 0, -1, 44100, 512, 4096, 15487, 0x85c507e2802bb9f5ull, 0x692e31c342f41d87ull, {0x3ba1028bu, 0x3e474f2eu, 0xb46fed64u, 0x3dc8d154u}},
    {"blend3", "1,1200,3000,30,0|0,40;30,10;970,-50;1000,80", "-", 0, -1, 44100, 512, 4096, 26459, 0x658828aa30f4624bull, 0x981bb8d6aa51b4d3ull, {0x3c948336u, 0x3e06fe2eu, 0xb37688b6u, 0x3e926290u}},
    {"blend4", "1,1200,3000,0,0|0,40;60,10;940,-50;1000,80", "-", 0, -1, 44100, 512, 4096, 25977, 0xb72e698f99884ba7ull, 0x3ee101fce4b8c01bull, {0xbbd8161fu, 0x3dbf98aau, 0xb40d8bf5u, 0x3e51e8e9u}},
    {"blend6", "1,1200,2500,0,0|0,0;500,100;500,-100;1000,0", "-", 0, -1, 44100, 512, 4096, 21806, 0xcf8b7788b1d50459ull, 0xba72cf903dfd2af3ull, {0xbbb900fbu, 0xbdbfc360u, 0xb3ecacb2u, 0x3e514870u}},
    {"mute", "0,1200,1000,-800,0|0,0;1000,0", "-", 0, -1, 44100, 512, 4096, 23999, 0x45c17cf09fc1eba3ull, 0x4edb93a618c390b1ull, {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {"gain-", "0,600,7777,-123,0|0,30;400,-80;1000,50", "-", 0, -1, 44100, 512, 4096, 25592, 0x6c17326a13aee2beull, 0x58f5c11ad3ed0dafull, {0x3d42969eu, 0xbbfb0a57u, 0xb27605c9u, 0x3d49b2b9u}},
    {"dup", "0,1200,3000,0,0|0,50;300,20;300,-30;1000,0", "-", 0, -1, 44100, 512, 4096, 23180, 0xd8c8f42fcb88687dull, 0x08099e7daf48e469ull, {0xb9c78867u, 0x3e2bb73cu, 0xb40b0c03u, 0x3e51d7a0u}},
    {"fast", "0,2400,1000000,0,0|0,-100;500,100;1000,-100", "-", 0, -1, 44100, 512, 4096, 17718, 0xe91d4434b222be7bull, 0x4443891241cb2ed0ull, {0xbb80b258u, 0x3e4a93f1u, 0xb44dac04u, 0x3e4b978du}},
    {"slow", "0,2400,1,0,0|0,-100;500,100;1000,-100", "-", 0, -1, 44100, 512, 4096, 95668, 0xc919abcb0c2a360aull, 0x8c127be386211acaull, {0xbed4df04u, 0xbe30d322u, 0xb422b48fu, 0x3e527b34u}},
    {"bypass", "0,1200,5000,0,1|0,0;250,100;750,-100;1000,0", "-", 0, -1, 48000, 512, 4096, 24000, 0x9369fe8de6a5d722ull, 0x801ddb55eb6b9833ull, {0x3d793a80u, 0x3d72cdd8u, 0xb43adfb0u, 0x3e53224fu}},
    {"change", "0,1200,5000,0,0|0,0;250,100;750,-100;1000,0", "1,1200,3000,30,0|0,40;30,10;970,-50;1000,80", 8000, -1, 48000, 512, 4096, 24271, 0x4c6329cdb5af6497ull, 0x058cd05d0a46878cull, {0xba88f73fu, 0x3e3e7d11u, 0xb4261749u, 0x3e93cbafu}},
    {"disc", "0,1200,9000,0,0|0,0;200,90;250,100;300,90;700,-90;750,-100;800,-90;1000,0", "-", 0, 10000, 48000, 512, 4096, 20471, 0x23c9bf1656c3dbf4ull, 0x01c60663ed4038c4ull, {0x3b8b37f5u, 0x3ebdf9afu, 0xb42388d0u, 0x3e4c0989u}},
    {"small blocks", "0,1200,9000,0,0|0,0;50,90;100,100;150,90;350,-70;400,-80;450,-70;600,20;650,30;700,40;800,70;850,60;1000,0", "-", 0, -1, 48000, 97, 300, 20003, 0x7090c7f6250dad6bull, 0x3df9d876798bf550ull, {0xbc40083cu, 0x3db84b39u, 0xb436d132u, 0x3e4bd560u}},
    {"change", "0,1200,5000,0,0|0,0;250,100;750,-100;1000,0", "1,1200,3000,30,0|0,40;30,10;970,-50;1000,80", 8000, -1, 44100, 512, 4096, 24959, 0xee7670a479e409f7ull, 0xeadfa3ce0e8f10a8ull, {0x3b8a79adu, 0x3daaa04eu, 0xb3bdde25u, 0x3e92d5b0u}},
    {"disc", "0,1200,9000,0,0|0,0;200,90;250,100;300,90;700,-90;750,-100;800,-90;1000,0", "-", 0, 10000, 44100, 512, 4096, 20861, 0x3e75c8a949dab54aull, 0x3432fe7448317a59ull, {0xbbfca626u, 0x3e5e688bu, 0xb305d831u, 0x3e4d6962u}},
    {"small blocks", "0,1200,9000,0,0|0,0;50,90;100,100;150,90;350,-70;400,-80;450,-70;600,20;650,30;700,40;800,70;850,60;1000,0", "-", 0, -1, 44100, 97, 300, 19812, 0xad8084472eb3f531ull, 0xe587ecf1bf371de7ull, {0xbaff890du, 0xbd3dd629u, 0xb494cb8du, 0x3e4a9ee9u}},
};

// the engine over the signal as the DLL harness did: blocks, room for 'cap' frames of output, the first call is a discontinuity
static std::vector<float> stream(const Case& c, const std::vector<float>& in, uint64_t* callsHash)
{
    vegvib::Engine e;
    e.setSettings(parse(c.a));
    e.setFormat(c.rate);
    const int n = (int)(in.size() / 2);
    std::vector<float> out, tmp((size_t)c.cap * 2);
    uint64_t h = 1469598103934665603ull;
    int mode = 0, disc = c.disc;
    bool switched = false;
    for (int k = 0; k < n; k += c.block) {
        int left = std::min(c.block, n - k), at = k;
        if (std::strcmp(c.b, "-") != 0 && !switched && k >= c.switchAt) { e.setSettings(parse(c.b)); switched = true; }
        if (disc >= 0 && k >= disc) { mode = 0; disc = -1; }
        while (left > 0) {
            int nin = left, nout = c.cap;
            e.process(mode, &in[(size_t)at * 2], &nin, tmp.data(), &nout);
            mode = 1;
            out.insert(out.end(), tmp.begin(), tmp.begin() + 2 * nout);
            const int v[2] = {nin, nout};
            for (int b = 0; b < 8; ++b) { h ^= ((const uint8_t*)v)[b]; h *= 1099511628211ull; }
            at += nin;
            left -= nin;
            if (nin == 0 && nout == 0) return {};
        }
    }
    if (callsHash) *callsHash = h;
    return out;
}

int main()
{
    const std::vector<float> in = p3test::signal(24000);
    for (const Case& c : kCases) {
        const std::string tag = std::string(c.name) + " at " + std::to_string(c.rate) + " Hz, blocks of " + std::to_string(c.block);
        uint64_t calls = 0;
        const std::vector<float> out = stream(c, in, &calls);
        char buf[96];
        std::snprintf(buf, sizeof buf, "%016llx, %d frames", (unsigned long long)p3test::hashBits(out), (int)(out.size() / 2));
        check((int)(out.size() / 2) == c.frames && p3test::hashBits(out) == c.hash, "stream is bit-identical to the plug-in's (" + tag + ")", buf);
        check(calls == c.callsHash, "the plug-in takes and gives the same frames in each call (" + tag + ")");
        bool same = (int)(out.size() / 2) == c.frames;
        const size_t at[4] = {2000 * 2, 2000 * 2 + 1, (size_t)c.frames * 2 - 2, (size_t)c.frames * 2 - 1};
        for (int k = 0; k < 4 && same; ++k) same = p3test::bitsOf(out[at[k]]) == c.samples[k];
        check(same, "stored samples match (" + tag + ")");
    }
    // saved form: [u32 length 0x12c][the effect's GUID][0x118 bytes]; this one is from a real project (colbert.veg)
    {
        static const char* kReal =
            "00000000b1040000c800000000000000020000000000000000000000e80300009cffffff0000000000000000";
        unsigned char settings[0x118];
        std::memset(settings, 0, sizeof settings);
        for (int k = 0; k < 44; ++k) {
            unsigned v = 0;
            std::sscanf(kReal + 2 * k, "%2x", &v);
            settings[k] = (unsigned char)v;
        }
        std::string raw(4 + 16 + sizeof settings, '\0');
        const uint32_t len = 0x12c;
        std::memcpy(&raw[0], &len, 4);
        static const unsigned char guid[16] = {0xe9, 0xc8, 0x20, 0xa7, 0x34, 0xd5, 0xf8, 0x4c, 0xa2, 0xdb, 0x83, 0xcf, 0xbd, 0xca, 0x8d, 0xc5};
        std::memcpy(&raw[4], guid, 16);
        std::memcpy(&raw[20], settings, sizeof settings);
        const std::string st = "S:" + base64Encode(raw.data(), raw.size());
        vegvib::Settings s;
        check(vegvib::decode(st, &s) && s.blendEdges == 0 && s.semitones == 1201 && s.frequency == 200 && s.gain == 0 && s.count == 2 &&
                  s.pts[0] == 0 && s.pts[1] == 0 && s.pts[2] == 1000 && s.pts[3] == -100 && s.bypass == 0,
              "a real project's Vibrato settings are read");
        check(vegvib::encode(s) == st, "and written back byte for byte");
        AudioPluginDesc d;
        d.format = "BUILTIN";
        d.uid = "reno.vibrato";
        std::string err;
        auto p = builtinAudioCreate(d, &err);
        check(p && p->loadState(st) && p->saveState() == st, "the plug-in loads and saves the same form");
        double lo = 0, hi = 0;
        check(p && p->paramRange(0, &lo, &hi) && std::fabs(p->paramGet(0) * (hi - lo) + lo - 12.01) < 1e-5, "Semitones shows 12.01");
        check(p && !p->loadState("garbage") && !p->loadState("S:AAAA"), "other states are refused");
        check(p && p->loadState("reno-builtin-audio 1\nRate=3\n") && p->saveState() == vegvib::encode(vegvib::presets()[0].s), "a stand-in's old state loads as the defaults");
        vegvib::Settings wild;
        wild.count = 99;
        wild.semitones = 99999;
        wild.pts[0] = -5;
        wild.pts[1] = 500;
        vegvib::decode(vegvib::encode(wild), &s);
        check(s.count == 32 && s.semitones == 2400 && s.pts[0] == 0 && s.pts[1] == 100, "out-of-range values are brought into range");
    }
    // parameters and presets
    {
        AudioPluginDesc d;
        d.format = "BUILTIN";
        d.uid = "reno.vibrato";
        std::string err;
        auto p = builtinAudioCreate(d, &err);
        check(p && p->paramCount() == 4 && p->paramName(0) == "Semitones" && p->paramName(1) == "Modulation Frequency" && p->paramName(2) == "Blend Graph Edges" &&
                  p->paramName(3) == "Output Gain",
              "the plug-in's own four parameters");
        check(p && p->programCount() == 8 && p->programName(0) == "Default all parameters" && p->programName(1) == "Pitch shift octave up" && p->programName(7) == "Triangle",
              "VEGAS's 8 presets");
        vegvib::Settings q;
        check(p && p->setProgram(4) && vegvib::decode(p->saveState(), &q) && q.frequency == 9000 && q.count == 8 && q.pts[2 * 3] == 300 && q.pts[2 * 3 + 1] == 90, "Vibrato 2 is 9 Hz with 8 points");
        check(p && p->setProgram(7) && vegvib::decode(p->saveState(), &q) && q.semitones == 600 && q.count == 2 && q.pts[4] == 1000 && q.pts[5] == -100,
              "Triangle is stored with a count of 2 and a third point, as VEGAS has it");
    }
    // Reno's in-place use: the plug-in's first frames come out after one silent frame; a balanced graph keeps the length
    {
        const Case& c = kCases[0];      // flat graph at 48000: the sound unchanged
        vegvib::Settings s = parse(c.a);
        const std::vector<float> out = p3test::run("reno.vibrato", vegvib::encode(s), c.rate, in);
        bool same = out.size() == in.size() && out[0] == 0.f && out[1] == 0.f;
        for (size_t k = 2; k < in.size() && same; ++k) same = out[k] == in[k - 2];
        check(same, "a flat graph gives the sound back, late by one frame");
        const std::vector<float> ref = stream(c, in, nullptr);
        s = parse(kCases[3].a);
        const std::vector<float> v1 = p3test::run("reno.vibrato", vegvib::encode(s), kCases[3].rate, in);
        const std::vector<float> ref1 = stream(kCases[3], in, nullptr);
        bool ok = v1.size() == in.size();
        for (size_t k = 2; k < 800 && ok; ++k) ok = v1[k] == ref1[k - 2];
        check(ok, "the first frames out of the effect are the stream's (Vibrato 1)");
        char buf[64];
        std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)p3test::hashBits(v1));
        check(v1.size() == in.size() && p3test::hashBits(v1) == 0xe6486d9574dc37dcull, "in-place output of Vibrato 1 is unchanged", buf);
        vegvib::Settings u = parse(kCases[1].a);
        u.bypass = 1;
        check(p3test::run("reno.vibrato", vegvib::encode(u), 48000, in) == in, "bypass leaves the sound alone");
        const std::vector<float> v2 = p3test::run("reno.vibrato", vegvib::encode(parse(kCases[1].a)), 44100, in, 77);
        std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)p3test::hashBits(v2));
        check(v2.size() == in.size() && p3test::hashBits(v2) == 0xc255fe29f9de10b5ull, "in-place output of the octave up preset (blocks of 77) is unchanged", buf);
        (void)ref;
    }
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
