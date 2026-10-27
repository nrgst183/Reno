// Shared by the vegasfx_*_test programs: runs a Reno version of a sfppack1 effect from a small script (the same script
// drives the DLL harness that made the stored results) on a fixed, exactly representable test signal, and hashes what
// comes out. Script lines: rate N / ch N / params v0 v1 ... / tempo flags bpm num den / reset / update / proc N.
#pragma once
#include "../../src/audiofx/vegaspack1.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace vegfx {

// Frame i, channel c: 16-bit noise from an integer hash, in bursts (every value is a multiple of 2^-17: exact in float).
inline std::vector<float> testInput(long frames)
{
    std::vector<float> v((size_t)frames * 2);
    for (long i = 0; i < frames; ++i)
        for (int c = 0; c < 2; ++c) {
            const uint32_t h = (uint32_t)i * 2654435761u + (uint32_t)c * 40503u;
            const double n = (double)((h >> 12) & 0xFFFF) / 32768.0 - 1.0;
            const double env = ((i / 6000) % 2 == 0) ? 1.0 : 0.25;
            v[(size_t)(i * 2 + c)] = (float)(n * 0.5 * env);
        }
    return v;
}

inline uint64_t fnv(const std::vector<float>& v)
{
    uint64_t h = 1469598103934665603ull;
    for (float f : v) {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        for (int k = 0; k < 4; ++k) {
            h ^= (b >> (8 * k)) & 0xFF;
            h *= 1099511628211ull;
        }
    }
    return h;
}

inline std::vector<float> run(const std::string& uid, const std::string& script, long inFrames)
{
    using namespace reno;
    std::vector<float> out;
    const vegpack1::Info* info = vegpack1::findInfo(uid);
    if (!info) return out;
    std::unique_ptr<AudioPlugin> p = vegpack1::create(uid);
    const std::vector<float> in = testInput(inFrames);
    std::vector<int32_t> params((size_t)info->nInts, 0);
    long inPos = 0;
    int64_t pos = 0;
    bool jump = false;
    size_t at = 0;
    while (at < script.size()) {
        size_t e = script.find('\n', at);
        if (e == std::string::npos) e = script.size();
        const std::string line = script.substr(at, e - at);
        at = e + 1;
        char cmd[32];
        if (std::sscanf(line.c_str(), "%31s", cmd) != 1) continue;
        char* r = const_cast<char*>(line.c_str()) + std::strlen(cmd);
        if (!std::strcmp(cmd, "rate")) p->activate(std::atof(r), 512);
        else if (!std::strcmp(cmd, "params")) {
            for (int i = 0; i < info->nInts; ++i) params[(size_t)i] = (int32_t)std::strtol(r, &r, 10);
        } else if (!std::strcmp(cmd, "tempo")) {
            std::strtol(r, &r, 10);
            const double bpm = std::strtod(r, &r);
            vegpack1::setTempo(bpm, (int)std::strtol(r, &r, 10));
        } else if (!std::strcmp(cmd, "reset")) {
            p->loadState(vegpack1::encode(*info, params.data()));
            jump = true;
        } else if (!std::strcmp(cmd, "update")) {
            p->loadState(vegpack1::encode(*info, params.data()));
        } else if (!std::strcmp(cmd, "proc")) {
            long cnt = std::atol(r);
            cnt = std::min(cnt, inFrames - inPos);
            if (jump) { pos += 1000003; jump = false; }
            long done = 0;
            while (done < cnt) {
                const int m = (int)std::min<long>(512, cnt - done);
                std::vector<float> l((size_t)m), rr((size_t)m);
                for (int k = 0; k < m; ++k) {
                    l[(size_t)k] = in[(size_t)((inPos + done + k) * 2)];
                    rr[(size_t)k] = in[(size_t)((inPos + done + k) * 2 + 1)];
                }
                p->process(l.data(), rr.data(), m, pos, true);
                for (int k = 0; k < m; ++k) { out.push_back(l[(size_t)k]); out.push_back(rr[(size_t)k]); }
                done += m;
                pos += m;
            }
            inPos += cnt;
        }
    }
    return out;
}

struct Case {
    const char* name;
    const char* script;
    uint64_t hash;        // FNV-1a of the output's bytes
    uint32_t samples;     // how many floats
    uint32_t probe[8];    // bits of the floats at probeIndex()
};
inline long probeIndex(int k, long n)
{
    const long at[8] = {1000, 1001, 20000, 20001, 50001, 89999, n - 2, n - 1};
    return at[k];
}

// Runs every case and counts the ones that differ from the DLL's output; prints one line per case.
inline int checkCases(const char* uid, const Case* cases, int count, long frames = 120000)
{
    int bad = 0;
    for (int c = 0; c < count; ++c) {
        const std::vector<float> o = run(uid, cases[c].script, frames);
        bool ok = o.size() == cases[c].samples && fnv(o) == cases[c].hash;
        for (int k = 0; ok && k < 8; ++k) {
            uint32_t b;
            std::memcpy(&b, &o[(size_t)probeIndex(k, (long)o.size())], 4);
            ok = b == cases[c].probe[k];
        }
        std::printf("%s %s: %s\n", ok ? "PASS" : "FAIL", uid, cases[c].name);
        if (!ok) ++bad;
    }
    return bad;
}

}  // namespace vegfx
