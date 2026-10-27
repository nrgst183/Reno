// Smooth/Enhance, Reno's version of VEGAS's DirectX effect (sfppack3_x64.dll, vegassmooth.h): the sound bit for bit against
// what the plug-in's own code gave (expected hashes and samples were made with the DLL), the saved form, the presets.
//
//   vegasfx_smooth_test
#include "../../src/audiofx/vegassmooth.h"
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
    int op;
    int rate;
    uint64_t hash;
    uint32_t samples[4];        // output bits at frames 2000 and 9999, left and right
};
static const Case kCases[] = {
    {-5, 48000, 0x1f9d843c10b6ab90ull, {0xbc907025u, 0xbc8d328cu, 0xb57c9accu, 0x3e1a96e9u}},
    {-4, 48000, 0xc2bfd95e06dc9fffull, {0xbd107f77u, 0x3cac2b4fu, 0xb5a66652u, 0x3e12d4b6u}},
    {-3, 48000, 0x41db4834ec3389b9ull, {0xbd13acd7u, 0x3d44b1aau, 0xb62ce6ddu, 0x3d3f774au}},
    {-2, 48000, 0xd11b0d7c3cd664f8ull, {0xbcc25866u, 0x3d655316u, 0xb65d4cdeu, 0x3d1bda13u}},
    {-1, 48000, 0x19110713cd3bf479ull, {0x39a20acdu, 0x3d6d4361u, 0xb64eda7au, 0x3d235e2au}},
    {0, 48000, 0x9369fe8de6a5d722ull, {0x3d793a80u, 0x3d72cdd8u, 0xb6351d18u, 0x3d28e840u}},
    {1, 48000, 0xdfa1c58bfec86104ull, {0x3e775cbeu, 0x3d755242u, 0xb60f242fu, 0x3d2c0b72u}},
    {2, 48000, 0x6528debd8939d10aull, {0x3ecd70fbu, 0x3d888cb0u, 0xb603aec6u, 0x3d3f7ea5u}},
    {3, 48000, 0x3fc77e270601d02aull, {0x3f162aeau, 0x3d6be887u, 0xb5457c50u, 0x3d1f753au}},
    {4, 48000, 0x3dab6e660139ac5aull, {0x3f4c8b15u, 0x3d76ea9bu, 0x3402cd6du, 0x3d184958u}},
    {5, 48000, 0x1a5b203291470a3dull, {0x3f94a5adu, 0x3d5b9e20u, 0x362a6e35u, 0x3c487b26u}},
    {-3, 44100, 0x41db4834ec3389b9ull, {0xbd13acd7u, 0x3d44b1aau, 0xb62ce6ddu, 0x3d3f774au}},     // the plug-in does not look at the sample rate
    {5, 44100, 0x1a5b203291470a3dull, {0x3f94a5adu, 0x3d5b9e20u, 0x362a6e35u, 0x3c487b26u}},
};

int main()
{
    const std::vector<float> in = p3test::signal(24000);
    for (const Case& c : kCases) {
        vegsmooth::Settings s;
        s.operation = c.op;
        const std::string state = vegsmooth::encode(s);
        const std::vector<float> out = p3test::run("reno.smooth", state, c.rate, in);
        const std::string tag = "operation " + std::to_string(c.op) + " at " + std::to_string(c.rate) + " Hz";
        char buf[64];
        std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)p3test::hashBits(out));
        check(!out.empty() && p3test::hashBits(out) == c.hash, "sound is bit-identical to the plug-in's (" + tag + ")", buf);
        const size_t at[4] = {2000 * 2, 2000 * 2 + 1, 9999 * 2, 9999 * 2 + 1};
        bool same = !out.empty();
        for (int k = 0; k < 4 && same; ++k) same = p3test::bitsOf(out[at[k]]) == c.samples[k];
        check(same, "stored samples match (" + tag + ")");
    }
    // the block size makes no difference
    {
        vegsmooth::Settings s;
        s.operation = -2;
        const std::string state = vegsmooth::encode(s);
        check(p3test::run("reno.smooth", state, 48000, in, 512) == p3test::run("reno.smooth", state, 48000, in, 77), "block size 77 gives the same sound as 512");
    }
    // bypassed (the plug-in's own Bypass setting): untouched
    {
        vegsmooth::Settings s;
        s.operation = 4;
        s.bypass = 1;
        check(p3test::run("reno.smooth", vegsmooth::encode(s), 48000, in) == in, "bypass leaves the sound alone");
    }
    // saved form: [u32 length 0x1c][the effect's GUID][operation][bypass]
    {
        vegsmooth::Settings s;
        s.operation = -4;
        const std::string st = vegsmooth::encode(s);
        const std::string raw = base64Decode(st.substr(2));
        static const unsigned char expect[28] = {0x1c, 0, 0, 0, 0xe9, 0xc8, 0x20, 0xa7, 0x34, 0xd5, 0xf8, 0x4c, 0xa2, 0xdb, 0x83, 0xcf, 0xbd, 0xca, 0x8d, 0xc5,
                                                 0xfc, 0xff, 0xff, 0xff, 0, 0, 0, 0};
        check(st.compare(0, 2, "S:") == 0 && raw.size() == 28 && std::memcmp(raw.data(), expect, 28) == 0, "saved form is VEGAS's: length, GUID, settings");
        vegsmooth::Settings back;
        check(vegsmooth::decode(st, &back) && back.operation == -4, "saved form reads back");
        AudioPluginDesc d;
        d.format = "BUILTIN";
        d.uid = "reno.smooth";
        std::string err;
        auto p = builtinAudioCreate(d, &err);
        check(p && p->loadState(st) && p->saveState() == st, "the plug-in loads and saves the same form");
        check(p && !p->loadState("garbage") && !p->loadState("S:AAAA"), "other states are refused");
        check(p && p->loadState("reno-builtin-audio 1\nOperation=3\n") && p->saveState() == vegsmooth::encode(vegsmooth::Settings()), "a stand-in's old state loads as the defaults");
        // a bigger or out-of-range value is brought into range
        vegsmooth::Settings wild;
        wild.operation = 99;
        vegsmooth::decode(vegsmooth::encode(wild), &back);
        check(back.operation == 5, "an operation beyond 5 is limited to 5");
    }
    // presets and parameters
    {
        AudioPluginDesc d;
        d.format = "BUILTIN";
        d.uid = "reno.smooth";
        std::string err;
        auto p = builtinAudioCreate(d, &err);
        check(p && p->paramCount() == 1 && p->paramName(0) == "Operation", "one parameter: Operation");
        double lo = 0, hi = 0;
        check(p && p->paramRange(0, &lo, &hi) && lo == -5 && hi == 5, "Operation runs from -5 to 5");
        check(p && p->programCount() == 2 && p->programName(0) == "(Default)" && p->programName(1) == "Boost high frequencies", "VEGAS's presets: (Default), Boost high frequencies");
        vegsmooth::Settings s;
        check(p && p->setProgram(1) && vegsmooth::decode(p->saveState(), &s) && s.operation == 3, "Boost high frequencies is operation 3");
        check(p && p->setProgram(0) && vegsmooth::decode(p->saveState(), &s) && s.operation == 0, "(Default) is operation 0");
    }
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
