// Shared by vegasfx_<effect>_test.cpp (the sfppack3_x64.dll effects): a fixed test signal made only of exact operations
// (no libm), a hash of an output buffer's float bits, and running a built-in effect over it in blocks. The expected
// hashes and sample values in each test were made by running the DLL's own code (a harness that maps the DLL and calls its
// setup and per-sample functions) over the same signal, so the tests need not have the DLL.
#pragma once
#include "../../src/audiofx/audioplugin.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace p3test {

// Stereo, interleaved. Left: noise whose level drops in steps (1, 1/16, 1/256, ...) every 2048 frames, with a few impulses and a
// very quiet stretch. Right: a sawtooth with a different period and a square wave added every now and then.
inline std::vector<float> signal(int frames)
{
    std::vector<float> s((size_t)frames * 2);
    uint32_t seed = 12345u;
    for (int i = 0; i < frames; ++i) {
        seed = seed * 1103515245u + 12345u;
        const float noise = (float)(int32_t)((seed >> 8) & 0xffffffu) / 8388608.0f - 1.0f;      // -1 .. 1, exact
        const int step = (i / 2048) % 6;
        float gain = 0.5f;
        for (int k = 0; k < step; ++k) gain *= 0.0625f;
        float l = noise * gain;
        if (i % 7000 == 100) l = 0.9f;
        if (i >= 15000 && i < 16000) l *= 1e-9f;
        float r = (float)(i % 97) / 97.0f * 0.5f - 0.25f;
        if ((i / 3000) % 2) r += ((i / 25) % 2) ? 0.25f : -0.25f;
        s[(size_t)i * 2] = l;
        s[(size_t)i * 2 + 1] = r;
    }
    return s;
}

inline uint64_t hashBits(const std::vector<float>& v)
{
    uint64_t h = 1469598103934665603ull;
    for (float f : v) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        for (int b = 0; b < 4; ++b) {
            h ^= (u >> (8 * b)) & 0xffu;
            h *= 1099511628211ull;
        }
    }
    return h;
}

inline uint32_t bitsOf(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// runs the built-in effect (its saved form 'state') over the signal in blocks; returns interleaved output
inline std::vector<float> run(const std::string& uid, const std::string& state, double rate, const std::vector<float>& in, int block = 512,
                              reno::AudioPlugin** keep = nullptr)
{
    reno::AudioPluginDesc d;
    d.format = "BUILTIN";
    d.uid = uid;
    std::string err;
    static std::unique_ptr<reno::AudioPlugin> p;
    p = reno::builtinAudioCreate(d, &err);
    if (!p) return {};
    if (!state.empty() && !p->loadState(state)) return {};
    p->activate(rate, 512);
    const int n = (int)(in.size() / 2);
    std::vector<float> l((size_t)n), r((size_t)n);
    for (int k = 0; k < n; ++k) { l[(size_t)k] = in[(size_t)k * 2]; r[(size_t)k] = in[(size_t)k * 2 + 1]; }
    for (int k = 0; k < n; k += block) p->process(&l[(size_t)k], &r[(size_t)k], std::min(block, n - k), k, true);
    std::vector<float> out((size_t)n * 2);
    for (int k = 0; k < n; ++k) { out[(size_t)k * 2] = l[(size_t)k]; out[(size_t)k * 2 + 1] = r[(size_t)k]; }
    if (keep) *keep = p.get();
    return out;
}

}  // namespace p3test
