// afxbench: runs each built-in audio effect on a fixed synthetic signal (defaults, then a few fixed pseudo-random
// settings) and prints a hash of every output ("HASH <uid> <variant> <hex>"), to compare two builds sample for sample.
//
//   afxbench [--secs S] [--variants N] [--only uid1,uid2] [--list] [--time]
#include "../../src/audiofx/audioplugin.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace reno;

int main(int argc, char** argv)
{
    double secs = 2;
    int variants = 3;
    bool list = false, timeIt = false;
    std::set<std::string> only;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--secs" && i + 1 < argc) secs = atof(argv[++i]);
        else if (a == "--variants" && i + 1 < argc) variants = atoi(argv[++i]);
        else if (a == "--list") list = true;
        else if (a == "--time") timeIt = true;
        else if (a == "--only" && i + 1 < argc) {
            std::string s = argv[++i];
            size_t p = 0;
            while (p <= s.size()) {
                size_t q = s.find(',', p);
                if (q == std::string::npos) q = s.size();
                only.insert(s.substr(p, q - p));
                p = q + 1;
            }
        }
    }
    const double sr = 48000;
    const int n = (int)(secs * sr);
    std::vector<float> inL(n), inR(n);
    uint32_t rs = 12345;
    for (int k = 0; k < n; ++k) {
        rs = rs * 1664525u + 1013904223u;
        const float noise = ((rs >> 8) & 0xffff) / 65536.f - 0.5f;
        const double t = k / sr;
        const float env = (k / 6000) % 4 == 3 ? 0.002f : 1.f;     // quiet gaps for gates / expanders
        inL[k] = env * (0.35f * (float)std::sin(2 * 3.14159265358979 * 440 * t) + 0.2f * noise + 0.15f * (float)std::sin(2 * 3.14159265358979 * 3100 * t));
        inR[k] = env * (0.3f * (float)std::sin(2 * 3.14159265358979 * 523 * t) + 0.25f * noise);
    }
    std::vector<AudioPluginDesc> all = builtinAudioPlugins();
    std::vector<float> l(n), r(n);
    for (const AudioPluginDesc& d : all) {
        if (!only.empty() && !only.count(d.uid)) continue;
        if (list) { std::printf("%s\n", d.uid.c_str()); continue; }
        for (int v = 0; v < variants; ++v) {
            std::string err;
            std::unique_ptr<AudioPlugin> p = createAudioPlugin(d, &err);
            if (!p) { std::printf("NOPLUGIN %s\n", d.uid.c_str()); break; }
            p->activate(sr, 512);
            if (v > 0) {
                uint32_t s = 777u + 31u * (uint32_t)v;
                for (int k = 0; k < p->paramCount(); ++k) {
                    s = s * 1664525u + 1013904223u;
                    p->paramSet(k, ((s >> 8) & 0xffff) / 65535.f);
                }
            }
            l = inL; r = inR;
            const auto t0 = std::chrono::steady_clock::now();
            for (int at = 0; at < n; at += 512) {
                const int m = std::min(512, n - at);
                p->process(l.data() + at, r.data() + at, m, at, true);
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            uint64_t h = 1469598103934665603ull;
            for (int k = 0; k < n; ++k) {
                uint32_t a, b;
                std::memcpy(&a, &l[k], 4);
                std::memcpy(&b, &r[k], 4);
                h = (h ^ a) * 1099511628211ull;
                h = (h ^ b) * 1099511628211ull;
            }
            std::printf("HASH %s %d %016llx\n", d.uid.c_str(), v, (unsigned long long)h);
            if (timeIt) std::printf("TIME %s %d %.1f\n", d.uid.c_str(), v, ms);
        }
        std::fflush(stdout);
    }
    return 0;
}
