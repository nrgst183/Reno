// Measures the processor time of each time-stretcher (stretch.h) per second of 48 kHz stereo sound at a few stretch
// factors, fed block by block. Needs only src/engine/stretch.cpp:
//
//   g++ -std=c++17 -O2 tests/engine/stretch_bench.cpp src/engine/stretch.cpp -o stretch_bench && ./stretch_bench
#include "../../src/engine/stretch.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace reno;

int main()
{
    const int rate = 48000, seconds = 20, block = 1024;
    struct M { const char* method; const char* mode; const char* label; };
    const M methods[] = {
        {"elastique", "pro", "elastique Pro"},
        {"elastique", "efficient", "elastique Efficient"},
        {"elastique", "soloist-mono", "elastique Soloist (Monophonic)"},
        {"elastique", "soloist-speech", "elastique Soloist (Speech)"},
        {"classic", "A01", "Classic A01"},
        {"classic", "A03", "Classic A03"},
        {"classic", "A07", "Classic A07"},
        {"classic", "A17", "Classic A17"},
    };
    const double rates[] = {0.5, 1.25, 2.0};
    // a buzz with a bit of noise (one second of it, made once and repeated): some work for every method
    std::vector<float> table((size_t)rate);
    uint32_t seed = 1;
    for (int k = 0; k < rate; ++k) {
        const double t = (double)k / rate;
        double v = 0;
        for (int h = 1; h <= 12; ++h) v += 0.06 * std::sin(2.0 * 3.14159265358979 * 150.0 * h * t) / h;
        seed = seed * 1664525u + 1013904223u;
        v += 0.01 * ((double)(seed >> 8) / (1 << 24) - 0.5);
        table[(size_t)k] = (float)v;
    }
    const StretchPull pull = [&](int64_t idx, int n, float* l, float* r) {
        for (int k = 0; k < n; ++k) {
            const float v = table[(size_t)(((idx + k) % rate + rate) % rate)];
            l[k] = v;
            r[k] = 0.8f * v;
        }
    };
    std::printf("%-32s", "ms of processor per second of sound");
    for (double r : rates) std::printf("   rate %.2f", r);
    std::printf("\n");
    std::vector<float> l(block), r(block);
    for (const M& m : methods) {
        std::printf("%-32s", m.label);
        for (double rr : rates) {
            StretchParams p;
            p.method = m.method;
            p.mode = m.mode;
            p.rate = rate;
            p.alpha = 1.0 / rr;
            auto s = makeStretcher(p);
            const auto t0 = std::chrono::steady_clock::now();
            for (int done = 0; done < rate * seconds; done += block) s->process(l.data(), r.data(), block, pull);
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("   %9.1f", secs * 1000.0 / seconds);
        }
        std::printf("\n");
    }
    // the cost of making the input alone
    {
        std::vector<float> a(block), b(block);
        const auto t0 = std::chrono::steady_clock::now();
        for (int done = 0; done < rate * seconds; done += block) pull(done, block, a.data(), b.data());
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("(reading the test sound itself: %.2f ms per second)\n", secs * 1000.0 / seconds);
    }
    return 0;
}
