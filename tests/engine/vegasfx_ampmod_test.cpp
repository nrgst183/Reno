// Amplitude Modulation, Reno's version of VEGAS's DirectX effect (sfppack3_x64.dll, vegasampmod.h): the sound the engine makes bit
// for bit against what the plug-in's own code gave (expected hashes and samples were made with the DLL), the saved form, the
// presets, the parameters, and Reno's in-place use of the engine.
//
//   vegasfx_ampmod_test
#include "../../src/audiofx/vegasampmod.h"
#include "vegasfx_pack3_common.h"

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// "lowPassFreq,lowPassOn,pan,blend,amplitude,frequency,wet,dry,bypass,period,multiplier,sync|x,y;x,y;..." (the plug-in's own units)
static vegamp::Settings parse(const char* s)
{
    vegamp::Settings st;
    std::memset(st.pts, 0, sizeof st.pts);
    int lpf = 0, lpon = 0, pan = 0, blend = 0, amp = 0, freq = 0, wet = 0, dry = 0, byp = 0, period = 0, mult = 0, sync = 0;
    const char* bar = std::strchr(s, '|');
    std::sscanf(s, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &lpf, &lpon, &pan, &blend, &amp, &freq, &wet, &dry, &byp, &period, &mult, &sync);
    st.lowPassFreq = lpf; st.lowPassOn = lpon; st.pan = pan; st.blendEdges = blend; st.amplitude = amp; st.frequency = freq;
    st.wetOut = wet; st.dryOut = dry; st.bypass = byp; st.period = period; st.multiplier = mult; st.tempoSync = sync;
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
    int switchAt;               // frame
    int posAt;                  // frame where the position is told (-1: never)
    double pos;                 // and what it is
    int rate, block;
    const char* tempo;          // "-": the plug-in gets none, else "flags,bpm,numerator,denominator"
    uint64_t hash;
    uint32_t samples[4];        // output bits at frame 2000 and at the last frame, left and right
};
static const Case kCases[] = {
    {"def", "400,0,0,0,-480,1000,0,-800,0,18,1000,0|0,-50;1000,-50", "-", 0, -1, 0.0, 48000, 512, "-", 0xd1f49af4c881f75cull, {0x3b7b9aa6u, 0x3b751e51u, 0xb23ca7afu, 0x3c552581u}},
    {"tri", "400,0,0,0,-480,1000,0,-800,0,18,1000,0|0,-50;250,0;750,-100;1000,-50", "-", 0, -1, 0.0, 48000, 512, "-", 0x5fd69805aab0dd84ull, {0x3bc5eb4au, 0x3bc0d13au, 0xb23ec06eu, 0x3c5783eeu}},
    {"sine", "400,0,0,0,-480,1000,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 512, "-", 0x195a0c0854071703ull, {0x3bfa825cu, 0x3bf40d41u, 0xb23fd9e3u, 0x3c58c1eeu}},
    {"sineop", "400,0,100,1,-480,5000,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 512, "-", 0x2635d984ee3185d1ull, {0x3d5ec0c4u, 0x398a6ee4u, 0xb23fd9e3u, 0x3c51a457u}},
    {"soft", "400,0,0,0,-480,5000,0,-800,0,18,1000,0|0,-50;50,0;450,0;550,-100;950,-100;1000,-50", "-", 0, -1, 0.0, 48000, 512, "-", 0xc628cab488d9daf2ull, {0x3d793a80u, 0x3d72cdd8u, 0xb2475fb9u, 0x3c6141c7u}},
    {"hard", "400,0,0,0,-480,5000,0,-800,0,18,1000,0|0,0;500,0;501,-100;1000,-100", "-", 0, -1, 0.0, 48000, 512, "-", 0x1f177b5fa7b3cff8ull, {0x3d793a80u, 0x3d72cdd8u, 0xb43adfb0u, 0x3e53224fu}},
    {"wacky", "400,0,25,0,-480,6500,0,-800,0,18,1000,0|0,-50;90,-25;125,-20;160,-25;340,-70;375,-75;410,-70;590,-10;625,-5;660,-10;840,-90;875,-95;910,-90;1000,-50", "-", 0, -1, 0.0, 48000, 512, "-", 0xc9c81151ec8a635eull, {0x3b5b2355u, 0x3a90343bu, 0xb2589addu, 0x3b5628f0u}},
    {"saw1", "400,0,0,0,-480,1000,0,-800,0,0,1,0|0,-50;1000,0", "-", 0, -1, 0.0, 48000, 512, "-", 0x8804dd9d48d46b2eull, {0x3b8ce478u, 0x3b8942b8u, 0xb33b3e83u, 0x3d538d72u}},
    {"saw2", "400,0,0,0,-480,1000,0,-800,0,0,1,0|0,-100;1000,0", "-", 0, -1, 0.0, 48000, 512, "-", 0x99dfef1115638016ull, {0x399f4c0bu, 0x399b30d8u, 0xb23b9d87u, 0x3c53f8cbu}},
    {"lp", "400,1,0,0,-800,7000,0,-100,0,18,1000,0|0,0;500,0;501,-100;1000,-100", "-", 0, -1, 0.0, 48000, 512, "-", 0x5a81ddaf7f919481ull, {0x3a8bce3bu, 0x3b353d0cu, 0xb400cca5u, 0x3e030975u}},
    {"lp2", "10000,1,0,0,-300,3000,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 512, "-", 0x209936dc48cc7c2full, {0xbbd4fb51u, 0x3d1adbd0u, 0xb2ec6648u, 0x3d165d03u}},
    {"lphi", "30000,1,0,0,-480,1000,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 512, "-", 0x195a0c0854071703ull, {0x3bfa825cu, 0x3bf40d41u, 0xb23fd9e3u, 0x3c58c1eeu}},
    {"blend", "400,0,0,1,-480,2000,0,-800,0,18,1000,0|0,40;60,10;940,-50;1000,80", "-", 0, -1, 0.0, 48000, 512, "-", 0xfc7e37a8502d9204ull, {0x3dc693ffu, 0x3dc17595u, 0xb4352de1u, 0x3e4cb33fu}},
    {"blend2", "400,0,0,1,-480,1000,0,-800,0,18,1000,0|0,-20;30,-10;970,-60;1000,-80", "-", 0, -1, 0.0, 48000, 512, "-", 0x060fa8acc56565e0ull, {0x3d0ada5cu, 0x3d074612u, 0xb2d8ba5cu, 0x3cf4dd2au}},
    {"blend3", "400,0,0,1,-480,1000,0,-800,0,18,1000,0|0,0;1000,-100", "-", 0, -1, 0.0, 48000, 512, "-", 0x4d08b401ac44e51full, {0x3cf1ac3bu, 0x3ceb716fu, 0xb23db350u, 0x3c5653e1u}},
    {"mute", "400,0,0,0,-480,1000,-800,0,0,18,1000,0|0,-50;1000,-50", "-", 0, -1, 0.0, 48000, 512, "-", 0x9369fe8de6a5d722ull, {0x3d793a80u, 0x3d72cdd8u, 0xb43adfb0u, 0x3e53224fu}},
    {"drywet", "400,0,50,0,-480,1000,100,-60,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 512, "-", 0x85787ef550d939f7ull, {0x3d5feec0u, 0x3e4abe6bu, 0xb40393a0u, 0x3dd8f733u}},
    {"fast", "400,0,0,0,-480,5000000,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 512, "-", 0x97745f1c9bf86825ull, {0x3d673ffbu, 0x3d6149f7u, 0xb195c48cu, 0x3ba935fau}},
    {"slow", "400,0,0,0,-480,1,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 512, "-", 0xd1f49af4c881f75cull, {0x3b7b9aa6u, 0x3b751e51u, 0xb23ca7afu, 0x3c552581u}},
    {"pan99", "400,0,99,0,-480,333,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 512, "-", 0xc340dc65916e6451ull, {0x3b9c81f9u, 0x3b564b01u, 0xb4051508u, 0x3a9d28bdu}},
    {"sync18", "400,0,0,0,-480,1000,0,-800,0,18,1000,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 512, "1,120,4,4", 0x5d119d104c20d4feull, {0x3bb009ddu, 0x3bab8030u, 0xb43a5ff0u, 0x3e5291f9u}},
    {"sync0", "400,0,0,0,-480,1000,0,-800,0,0,1000,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 512, "1,120,4,4", 0xc9d52e7652959326ull, {0x3b7b9aa6u, 0x3b751e51u, 0xb239831cu, 0x3c51987au}},
    {"sync13", "400,0,30,0,-480,1000,0,-800,0,13,500,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 512, "1,120,4,4", 0x39dd76f898197fc2ull, {0x3d317cf2u, 0x3d48d19du, 0xb239831cu, 0x3e0379a8u}},
    {"sync17", "400,0,0,0,-480,1000,0,-800,0,17,32000,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 512, "1,120,4,4", 0x96bf955a64536f1eull, {0x3b7b9aa6u, 0x3b751e51u, 0xb25f2a8au, 0x3c7c2350u}},
    {"sync10", "400,1,0,0,-480,1000,0,-800,0,10,1,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 512, "1,120,4,4", 0xb7d3cc2e8d98ae9cull, {0xbcc45adfu, 0xbb81a391u, 0xb065aaa7u, 0x3c60538eu}},
    {"sync7", "400,0,0,0,-480,1000,0,-800,0,7,3333,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 512, "1,120,4,4", 0x0f742ce2939bb8f2ull, {0x3ca8be67u, 0x3ca464ddu, 0xb4237cbcu, 0x3e38b61eu}},
    {"sync18 90.5 bpm 3/4", "400,0,0,0,-480,1000,0,-800,0,18,1000,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 512, "5,90.5,3,4", 0x7df8a7d5a11b1532ull, {0x3bb009ddu, 0x3bab8030u, 0xb43a61f6u, 0x3e529443u}},
    {"sync13 no bpm", "400,0,30,0,-480,1000,0,-800,0,13,500,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 97, "7,77,7,8", 0x8339f6d34f696a2dull, {0x3cbaa3bbu, 0x3d6f0022u, 0xb42b548cu, 0x3d243aa9u}},
    {"sync7 no tempo", "400,0,0,0,-480,1000,0,-800,0,7,3333,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 512, "-", 0x195a0c0854071703ull, {0x3bfa825cu, 0x3bf40d41u, 0xb23fd9e3u, 0x3c58c1eeu}},
    {"sync18 at position", "400,0,0,0,-480,1000,0,-800,0,18,1000,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, 5000, 123456.0, 48000, 512, "1,120,4,4", 0x64d8d448bca228deull, {0x3bb009ddu, 0x3bab8030u, 0xb27f449bu, 0x3c903421u}},
    {"sync10 at position", "400,1,0,0,-480,1000,0,-800,0,10,1,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, 0, 1000000000.5, 48000, 512, "5,90.5,3,4", 0x1194a6b086c47322ull, {0x3bf89ceau, 0xbc2ab4cfu, 0xb19f7494u, 0x3ba78332u}},
    {"change", "400,0,0,0,-480,1000,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "400,0,100,1,-480,5000,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", 8000, -1, 0.0, 48000, 512, "-", 0xc47000a407fa0c75ull, {0x3bfa825cu, 0x3bf40d41u, 0xb0682043u, 0x3e2d3ad7u}},
    {"change pan", "400,0,0,0,-480,1000,0,-800,0,18,1000,0|0,-50;250,0;750,-100;1000,-50", "400,0,25,0,-480,6500,0,-800,0,18,1000,0|0,-50;90,-25;125,-20;160,-25;340,-70;375,-75;410,-70;590,-10;625,-5;660,-10;840,-90;875,-95;910,-90;1000,-50", 5000, -1, 0.0, 48000, 333, "-", 0x1abb69c8ab043b77ull, {0x3bc5eb4au, 0x3bc0d13au, 0xb3fdd9c7u, 0x3c1720c3u}},
    {"change sync", "400,0,0,0,-480,1000,0,-800,0,18,1000,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "400,0,30,0,-480,1000,0,-800,0,13,500,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", 4000, -1, 0.0, 48000, 500, "1,120,4,4", 0xe9fd67d14bbfa1aeull, {0x3bb009ddu, 0x3bab8030u, 0xb05516f9u, 0x3ab6328eu}},
    {"change lp", "400,1,0,0,-800,7000,0,-100,0,18,1000,0|0,0;500,0;501,-100;1000,-100", "10000,1,0,0,-300,3000,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", 4000, -1, 0.0, 48000, 500, "-", 0xa861a5d22a065ce9ull, {0x3a8bce3bu, 0x3b353d0cu, 0xb1cc0756u, 0x3c01c622u}},
    {"small blocks", "400,0,25,0,-480,6500,0,-800,0,18,1000,0|0,-50;90,-25;125,-20;160,-25;340,-70;375,-75;410,-70;590,-10;625,-5;660,-10;840,-90;875,-95;910,-90;1000,-50", "-", 0, -1, 0.0, 48000, 97, "-", 0xc9c81151ec8a635eull, {0x3b5b2355u, 0x3a90343bu, 0xb2589addu, 0x3b5628f0u}},
    {"def", "400,0,0,0,-480,1000,0,-800,0,18,1000,0|0,-50;1000,-50", "-", 0, -1, 0.0, 44100, 512, "-", 0xd1f49af4c881f75cull, {0x3b7b9aa6u, 0x3b751e51u, 0xb23ca7afu, 0x3c552581u}},
    {"tri", "400,0,0,0,-480,1000,0,-800,0,18,1000,0|0,-50;250,0;750,-100;1000,-50", "-", 0, -1, 0.0, 44100, 512, "-", 0x5c231a5ebb3593aaull, {0x3bcedd87u, 0x3bc9886cu, 0xb1e8016bu, 0x3c030ffbu}},
    {"sine", "400,0,0,0,-480,1000,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 44100, 512, "-", 0xc3c9a3920bb99b53ull, {0x3c05f5f7u, 0x3c0281f5u, 0xb1b42aa8u, 0x3bcb8e5fu}},
    {"sineop", "400,0,100,1,-480,5000,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 44100, 512, "-", 0x1984663d41a319d9ull, {0x3d69bc93u, 0x3983ed90u, 0xb04e5c58u, 0x3e42dbcbu}},
    {"soft", "400,0,0,0,-480,5000,0,-800,0,18,1000,0|0,-50;50,0;450,0;550,-100;950,-100;1000,-50", "-", 0, -1, 0.0, 44100, 512, "-", 0x5deca03b3dce5719ull, {0x3d793a80u, 0x3d72cdd8u, 0xb03e7407u, 0x3a572d9cu}},
    {"hard", "400,0,0,0,-480,5000,0,-800,0,18,1000,0|0,0;500,0;501,-100;1000,-100", "-", 0, -1, 0.0, 44100, 512, "-", 0xaf32359b1b1e7733ull, {0x3d793a80u, 0x3d72cdd8u, 0xb03e7407u, 0x3a572d9cu}},
    {"wacky", "400,0,25,0,-480,6500,0,-800,0,18,1000,0|0,-50;90,-25;125,-20;160,-25;340,-70;375,-75;410,-70;590,-10;625,-5;660,-10;840,-90;875,-95;910,-90;1000,-50", "-", 0, -1, 0.0, 44100, 512, "-", 0xda70ccc21b3755a2ull, {0x3b1d4bc7u, 0x3abf9ac8u, 0xb32208eau, 0x3de7579bu}},
    {"saw1", "400,0,0,0,-480,1000,0,-800,0,0,1,0|0,-50;1000,0", "-", 0, -1, 0.0, 44100, 512, "-", 0x9852a7ea40583227ull, {0x3b8e7552u, 0x3b8ac93eu, 0xb3540910u, 0x3d6f8fe9u}},
    {"saw2", "400,0,0,0,-480,1000,0,-800,0,0,1,0|0,-100;1000,0", "-", 0, -1, 0.0, 44100, 512, "-", 0xfce6888050c3f70cull, {0x39a2db84u, 0x399ea8d2u, 0xb27095bbu, 0x3c87e8b4u}},
    {"lp", "400,1,0,0,-800,7000,0,-100,0,18,1000,0|0,0;500,0;501,-100;1000,-100", "-", 0, -1, 0.0, 44100, 512, "-", 0x2afccbaa2f7ea6a3ull, {0xb92f0b12u, 0x3bb92216u, 0xb36c60f3u, 0x3d858c14u}},
    {"lp2", "10000,1,0,0,-300,3000,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 44100, 512, "-", 0xb1b516d98214b697ull, {0xbb783249u, 0x3d24860cu, 0xb1f364a2u, 0x3c1af646u}},
    {"lphi", "30000,1,0,0,-480,1000,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 44100, 512, "-", 0xc3c9a3920bb99b53ull, {0x3c05f5f7u, 0x3c0281f5u, 0xb1b42aa8u, 0x3bcb8e5fu}},
    {"blend", "400,0,0,1,-480,2000,0,-800,0,18,1000,0|0,40;60,10;940,-50;1000,80", "-", 0, -1, 0.0, 44100, 512, "-", 0xda6cc2421bbc6908ull, {0x3dc168b9u, 0x3dbc6c6bu, 0xb4921de7u, 0x3ea515ffu}},
    {"blend2", "400,0,0,1,-480,1000,0,-800,0,18,1000,0|0,-20;30,-10;970,-60;1000,-80", "-", 0, -1, 0.0, 44100, 512, "-", 0xb9b978b45a5765f0ull, {0x3d093adcu, 0x3d05b148u, 0xb2bde00au, 0x3cd68669u}},
    {"blend3", "400,0,0,1,-480,1000,0,-800,0,18,1000,0|0,0;1000,-100", "-", 0, -1, 0.0, 44100, 512, "-", 0xf2bf42b238d46dbdull, {0x3d136f1cu, 0x3d0fa232u, 0xb213ef22u, 0x3c27239fu}},
    {"mute", "400,0,0,0,-480,1000,-800,0,0,18,1000,0|0,-50;1000,-50", "-", 0, -1, 0.0, 44100, 512, "-", 0x9369fe8de6a5d722ull, {0x3d793a80u, 0x3d72cdd8u, 0xb43adfb0u, 0x3e53224fu}},
    {"drywet", "400,0,50,0,-480,1000,100,-60,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 44100, 512, "-", 0xae23ba430ee554b6ull, {0x3d66d0c2u, 0x3e48f081u, 0xb3deed12u, 0x3dd99a9eu}},
    {"fast", "400,0,0,0,-480,5000000,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 44100, 512, "-", 0x5c1e1da92315413eull, {0x3b7b9aa6u, 0x3b751e51u, 0xb0b8d2f1u, 0x3ad0d170u}},
    {"slow", "400,0,0,0,-480,1,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 44100, 512, "-", 0xd1f49af4c881f75cull, {0x3b7b9aa6u, 0x3b751e51u, 0xb23ca7afu, 0x3c552581u}},
    {"pan99", "400,0,99,0,-480,333,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 44100, 512, "-", 0x2b5e4b58b9413d0cull, {0x3ba1db08u, 0x3b4f3667u, 0xb415d007u, 0x3a8b9bb5u}},
    {"sync18", "400,0,0,0,-480,1000,0,-800,0,18,1000,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 44100, 512, "1,120,4,4", 0xb62223bfd73afd99ull, {0x3bb60dc5u, 0x3bb15c66u, 0xb4305cb9u, 0x3e474200u}},
    {"sync0", "400,0,0,0,-480,1000,0,-800,0,0,1000,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 44100, 512, "1,120,4,4", 0xf0fccba3ed4b42aaull, {0x3d401164u, 0x3d3b1df0u, 0xb3b40ceeu, 0x3dcb6cc9u}},
    {"sync13", "400,0,30,0,-480,1000,0,-800,0,13,500,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 44100, 512, "1,120,4,4", 0x382fd99523f9c558ull, {0x3d47cd11u, 0x3d320d41u, 0xb41003a7u, 0x3e2129f8u}},
    {"sync17", "400,0,0,0,-480,1000,0,-800,0,17,32000,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 44100, 512, "1,120,4,4", 0xf5102da0ebcfde84ull, {0x3b7b9aa6u, 0x3b751e51u, 0xb262f26bu, 0x3c80346cu}},
    {"sync10", "400,1,0,0,-480,1000,0,-800,0,10,1,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 44100, 512, "1,120,4,4", 0xdfd2222e138bc9b2ull, {0x3c429606u, 0xba8e78cdu, 0xb28776d5u, 0x3c26fc7cu}},
    {"sync7", "400,0,0,0,-480,1000,0,-800,0,7,3333,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 44100, 512, "1,120,4,4", 0x6ecb90d9997af0eeull, {0x3cc1045fu, 0x3cbc0aa7u, 0xb42144a0u, 0x3e363442u}},
    {"sync18 90.5 bpm 3/4", "400,0,0,0,-480,1000,0,-800,0,18,1000,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 44100, 512, "5,90.5,3,4", 0xfc58ff2afd37f61aull, {0x3bb60dc5u, 0x3bb15c66u, 0xb42fe612u, 0x3e46bbf1u}},
    {"sync13 no bpm", "400,0,30,0,-480,1000,0,-800,0,13,500,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 44100, 97, "7,77,7,8", 0x18ef8ba9a1bc7fd2ull, {0x3cdcc833u, 0x3d68cb0eu, 0xb38751f3u, 0x3bc4d496u}},
    {"sync7 no tempo", "400,0,0,0,-480,1000,0,-800,0,7,3333,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 44100, 512, "-", 0xc3c9a3920bb99b53ull, {0x3c05f5f7u, 0x3c0281f5u, 0xb1b42aa8u, 0x3bcb8e5fu}},
    {"sync18 at position", "400,0,0,0,-480,1000,0,-800,0,18,1000,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, 5000, 123456.0, 44100, 512, "1,120,4,4", 0x76a222b51637d2e4ull, {0x3bb60dc5u, 0x3bb15c66u, 0xb0e21ae7u, 0x3aff7559u}},
    {"sync10 at position", "400,1,0,0,-480,1000,0,-800,0,10,1,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, 0, 1000000000.5, 44100, 512, "5,90.5,3,4", 0x347e133353c7b1f6ull, {0x3c9c4539u, 0xbb8adaa7u, 0x31847cd9u, 0x3c685459u}},
    {"change", "400,0,0,0,-480,1000,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "400,0,100,1,-480,5000,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", 8000, -1, 0.0, 44100, 512, "-", 0x12a2435487b24843ull, {0x3c05f5f7u, 0x3c0281f5u, 0xb200d861u, 0x3c9cd6fbu}},
    {"change pan", "400,0,0,0,-480,1000,0,-800,0,18,1000,0|0,-50;250,0;750,-100;1000,-50", "400,0,25,0,-480,6500,0,-800,0,18,1000,0|0,-50;90,-25;125,-20;160,-25;340,-70;375,-75;410,-70;590,-10;625,-5;660,-10;840,-90;875,-95;910,-90;1000,-50", 5000, -1, 0.0, 44100, 333, "-", 0xb65b26addfc27148ull, {0x3bcedd87u, 0x3bc9886cu, 0xb07f0fd4u, 0x3c4aedeau}},
    {"change sync", "400,0,0,0,-480,1000,0,-800,0,18,1000,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "400,0,30,0,-480,1000,0,-800,0,13,500,1|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", 4000, -1, 0.0, 44100, 500, "1,120,4,4", 0xc90f0a583c171ac4ull, {0x3bb60dc5u, 0x3bb15c66u, 0xb0a28f9fu, 0x3c77efffu}},
    {"change lp", "400,1,0,0,-800,7000,0,-100,0,18,1000,0|0,0;500,0;501,-100;1000,-100", "10000,1,0,0,-300,3000,0,-800,0,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", 4000, -1, 0.0, 44100, 500, "-", 0xb00a8591eeacf045ull, {0xb92f0b12u, 0x3bb92216u, 0xb2de0c3au, 0x3d0d5f52u}},
    {"small blocks", "400,0,25,0,-480,6500,0,-800,0,18,1000,0|0,-50;90,-25;125,-20;160,-25;340,-70;375,-75;410,-70;590,-10;625,-5;660,-10;840,-90;875,-95;910,-90;1000,-50", "-", 0, -1, 0.0, 44100, 97, "-", 0xda70ccc21b3755a2ull, {0x3b1d4bc7u, 0x3abf9ac8u, 0xb32208eau, 0x3de7579bu}},
    {"bypass", "400,0,0,0,-480,1000,0,-800,1,18,1000,0|0,-50;125,-12;188,-3;250,0;313,-3;375,-12;625,-88;688,-97;750,-100;813,-97;875,-88;1000,-50", "-", 0, -1, 0.0, 48000, 512, "-", 0x9369fe8de6a5d722ull, {0x3d793a80u, 0x3d72cdd8u, 0xb43adfb0u, 0x3e53224fu}},
};

// the engine over the signal as the DLL harness did: blocks, in place, setup again when the settings or the position change
static std::vector<float> stream(const Case& c, const std::vector<float>& in)
{
    vegamp::Engine e;
    e.setSettings(parse(c.a));
    e.setFormat(c.rate);
    vegamp::Tempo tp;
    const vegamp::Tempo* tempo = nullptr;
    if (std::strcmp(c.tempo, "-") != 0) {
        unsigned fl = 0;
        double bpm = 0;
        int num = 0, den = 0;
        std::sscanf(c.tempo, "%u,%lf,%d,%d", &fl, &bpm, &num, &den);
        tp.haveBpm = (fl & 1) != 0; tp.bpm = bpm; tp.haveMeter = (fl & 4) != 0; tp.numerator = num; tp.denominator = den;
        tempo = &tp;
    }
    std::vector<float> d = in;
    const int n = (int)(in.size() / 2);
    bool switched = false, posDone = false;
    for (int k = 0; k < n; k += c.block) {
        int cnt = std::min(c.block, n - k);
        if (std::strcmp(c.b, "-") != 0 && !switched && k >= c.switchAt) { e.setSettings(parse(c.b)); switched = true; }
        if (c.posAt >= 0 && !posDone && k >= c.posAt) { e.setPosition(c.pos); posDone = true; }
        e.process(&d[(size_t)k * 2], &cnt, tempo);
    }
    return d;
}

int main()
{
    const std::vector<float> in = p3test::signal(24000);
    for (const Case& c : kCases) {
        const std::string tag = std::string(c.name) + " at " + std::to_string(c.rate) + " Hz, blocks of " + std::to_string(c.block);
        const std::vector<float> out = stream(c, in);
        char buf[96];
        std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)p3test::hashBits(out));
        check(p3test::hashBits(out) == c.hash, "sound is bit-identical to the plug-in's (" + tag + ")", buf);
        const size_t at[4] = {2000 * 2, 2000 * 2 + 1, 23999 * 2, 23999 * 2 + 1};
        bool same = true;
        for (int k = 0; k < 4 && same; ++k) same = p3test::bitsOf(out[at[k]]) == c.samples[k];
        check(same, "stored samples match (" + tag + ")");
    }
    // the blocks the sound is given in do not matter
    {
        const Case& c = kCases[0];
        Case d = c;
        d.block = 77;
        check(stream(c, in) == stream(d, in), "the block size does not change the sound");
    }
    // saved form: [u32 length 0x148][the effect's GUID][0x134 bytes]
    {
        vegamp::Settings s = vegamp::presets()[2].s;
        const std::string st = vegamp::encode(s);
        std::string raw;
        check(st.compare(0, 2, "S:") == 0, "the saved form starts with S:");
        raw = base64Decode(st.substr(2));
        uint32_t len = 0;
        if (raw.size() >= 4) std::memcpy(&len, raw.data(), 4);
        static const unsigned char guid[16] = {0x60, 0x03, 0x2a, 0x8f, 0xfb, 0xd1, 0x46, 0x4d, 0xae, 0x1f, 0x89, 0x0a, 0x15, 0x44, 0x19, 0xc4};
        check(raw.size() == 4 + 16 + 0x134 && len == 0x148 && std::memcmp(raw.data() + 4, guid, 16) == 0, "length 0x148, the plug-in's GUID, 0x134 bytes of settings");
        vegamp::Settings t;
        check(vegamp::decode(st, &t) && std::memcmp(&t, &s, sizeof s) == 0 && vegamp::encode(t) == st, "the settings are read back and written byte for byte");
        // the layout: low-pass frequency first, graph points from 0x24, period at 0x128
        int32_t w[0x134 / 4];
        std::memcpy(w, raw.data() + 20, sizeof w);
        check(w[0] == 400 && w[2] == 0 && w[4] == -480 && w[5] == 1000 && w[6] == 0 && w[7] == -800 && w[8] == 12 && w[9] == 0 && w[10] == -50 && w[0x128 / 4] == 18 && w[0x12c / 4] == 1000,
              "fields are at the plug-in's offsets");
        AudioPluginDesc d;
        d.format = "BUILTIN";
        d.uid = "reno.ampmod";
        std::string err;
        auto p = builtinAudioCreate(d, &err);
        check(p && p->loadState(st) && p->saveState() == st, "the plug-in loads and saves the same form");
        check(p && !p->loadState("garbage") && !p->loadState("S:AAAA"), "other states are refused");
        check(p && p->loadState("reno-builtin-audio 1\nRate=3\n") && p->saveState() == vegamp::encode(vegamp::presets()[0].s), "a stand-in's old state loads as the defaults");
        vegamp::Settings wild;
        wild.count = 99;
        wild.amplitude = -99999;
        wild.pan = 500;
        wild.period = 40;
        wild.multiplier = 0;
        wild.pts[0] = -5;
        wild.pts[1] = 500;
        vegamp::decode(vegamp::encode(wild), &t);
        check(t.count == 32 && t.amplitude == -800 && t.pan == 100 && t.period == 18 && t.multiplier == 1 && t.pts[0] == 0 && t.pts[1] == 0, "out-of-range values are brought into range");
    }
    // parameters and presets
    {
        AudioPluginDesc d;
        d.format = "BUILTIN";
        d.uid = "reno.ampmod";
        std::string err;
        auto p = builtinAudioCreate(d, &err);
        static const char* const names[] = {"Amplitude", "Modulation Frequency", "Stereo Pan", "Low-pass Filter Enable", "Low-pass Filter Frequency", "Blend Graph Edges",
                                            "Dry Out", "Wet Out", "Modulation Period", "Modulation Period Multiplier", "Tempo Sync"};
        bool ok = p && p->paramCount() == 11;
        for (int k = 0; k < 11 && ok; ++k) ok = p->paramName(k) == names[k];
        check(ok, "the plug-in's own eleven parameters, in its order");
        check(p && p->programCount() == 9 && p->programName(0) == "Default all parameters" && p->programName(1) == "Triangle 1" && p->programName(3) == "Sine out of phase" &&
                  p->programName(8) == "Sawtooth 2",
              "VEGAS's 9 presets");
        vegamp::Settings q;
        check(p && p->setProgram(3) && vegamp::decode(p->saveState(), &q) && q.pan == 100 && q.blendEdges == 1 && q.frequency == 5000 && q.count == 12, "Sine out of phase: pan 100, blend, 5 Hz, 12 points");
        check(p && p->setProgram(5) && vegamp::decode(p->saveState(), &q) && q.count == 4 && q.pts[4] == 501 && q.pts[5] == -100, "Hard square has its step at 501");
        check(p && p->setProgram(7) && vegamp::decode(p->saveState(), &q) && q.period == 0 && q.multiplier == 1 && q.count == 2, "Sawtooth 1: period 0, multiplier brought to 0.001");
        check(p && p->setProgram(0) && p->paramText(7) == "0.0 dB" && p->paramText(6) == "-Inf." && p->paramText(8) == "Measures" && p->paramText(0) == "-48.0 dB",
              "the default: wet 0 dB, dry -Inf., measures, -48 dB");
    }
    // Reno's in-place use is the engine's
    {
        const Case& c = kCases[2];      // Sine at 48000
        const std::vector<float> out = p3test::run("reno.ampmod", vegamp::encode(parse(c.a)), c.rate, in);
        const std::vector<float> ref = stream(c, in);
        check(out == ref, "in place, in blocks of 512, the effect gives the engine's sound");
        const std::vector<float> out2 = p3test::run("reno.ampmod", vegamp::encode(parse(c.a)), c.rate, in, 77);
        check(out2 == ref, "and in blocks of 77");
        vegamp::Settings u = parse(c.a);
        u.bypass = 1;
        check(p3test::run("reno.ampmod", vegamp::encode(u), 48000, in) == in, "bypass leaves the sound alone");
        // tempo sync is 120 beats per minute in 4/4 with the cycle following the stream's position
        const Case& s = kCases[0];
        vegamp::Settings sy = parse(kCases[0].a);
        sy.tempoSync = 1;
        sy.period = 13;
        sy.multiplier = 1000;
        Case e = s;
        std::string text = "400,0,0,0,-480,1000,0,-800,0,13,1000,1|0,-50;1000,-50";
        e.a = text.c_str();
        e.tempo = "5,120,4,4";
        const std::vector<float> sref = stream(e, in);
        const std::vector<float> sout = p3test::run("reno.ampmod", vegamp::encode(sy), 48000, in);
        check(sout == sref, "tempo sync uses 120 beats per minute");
    }
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
