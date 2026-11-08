// envelopeSamples (engine_audio.cpp) walks along the envelope's points instead of searching for them for each sample;
// it must give exactly what Envelope::valueAt gives, bit for bit, for every curve type, for "mute" envelopes (always a
// hold), repeated times, points out of order and blocks that start before, inside and after the envelope. Also times it.
// The test includes the source file to reach the function, which lives in an unnamed namespace.
//   envsamples_test
#include "../../src/engine/engine_audio.cpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

using namespace reno;

static void reference(const Envelope& env, int64_t s0, int rate, double origin, int n, double* out)
{
    const double t0 = (double)s0 / rate - origin, t1 = (double)(s0 + n) / rate - origin;
    const std::vector<EnvPoint>& pts = env.points;
    if (t0 >= pts.back().time || t1 <= pts.front().time) {
        std::fill(out, out + n, t0 >= pts.back().time ? pts.back().value : pts.front().value);
        return;
    }
    for (int k = 0; k < n; ++k) out[k] = env.valueAt((double)(s0 + k) / rate - origin, 0.0);
}

int main()
{
    std::mt19937 rng(11);
    auto u = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
    int cases = 0, bad = 0;
    for (int round = 0; round < 3000; ++round) {
        Envelope env;
        env.kind = round % 7 == 0 ? "mute" : (round % 2 ? "volume" : "pan");
        const int np = 1 + (int)(rng() % 40);
        double t = u(-1, 2);
        for (int i = 0; i < np; ++i) {
            EnvPoint p;
            t += (rng() % 5 == 0) ? 0.0 : u(0.0, 0.8);           // some repeated times
            p.time = t;
            p.value = u(-2, 2);
            p.curve = (int)(rng() % 7);
            env.points.push_back(p);
        }
        if (round % 11 == 0 && np > 3) std::swap(env.points[1], env.points[2]);      // out of order
        const int rate = round % 3 ? 48000 : 44100;
        const int n = 1 + (int)(rng() % 1500);
        const int64_t s0 = (int64_t)std::llround(u(-1.0, t + 1.0) * rate);
        const double origin = round % 5 == 0 ? u(0, 3) : 0.0;
        std::vector<double> a((size_t)n), b((size_t)n);
        reference(env, s0, rate, origin, n, a.data());
        envelopeSamples(env, s0, rate, origin, n, b.data());
        ++cases;
        if (std::memcmp(a.data(), b.data(), (size_t)n * sizeof(double)) != 0) {
            ++bad;
            if (bad < 5) std::printf("DIFF round %d kind %s points %d\n", round, env.kind.c_str(), np);
        }
    }
    std::printf("%s %d envelopes compared, %d differ\n", bad ? "FAIL" : "PASS", cases, bad);

    // timing: a 200-point envelope over 10 s in 1024-sample blocks
    Envelope env;
    env.kind = "volume";
    for (int i = 0; i < 200; ++i) { EnvPoint p; p.time = i * 0.05; p.value = (i % 7) / 7.0; p.curve = i % 5; env.points.push_back(p); }
    std::vector<double> o(1024), o2(1024);
    double sink = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < 20; ++rep)
        for (int64_t s = 0; s < 480000; s += 1024) { reference(env, s, 48000, 0, 1024, o.data()); sink += o[5]; }
    const double ref = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20;
    t0 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < 20; ++rep)
        for (int64_t s = 0; s < 480000; s += 1024) { envelopeSamples(env, s, 48000, 0, 1024, o2.data()); sink += o2[5]; }
    const double now = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20;
    std::printf("TIME 10 s of envelope: valueAt per sample %.2f ms, walked %.2f ms (%.1fx)%s\n", ref, now, ref / now, sink == 12345.678 ? " " : "");
    return bad ? 1 : 0;
}
