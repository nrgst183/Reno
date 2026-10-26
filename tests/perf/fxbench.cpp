// fxbench: calls the built-in OpenFX effects directly (Def::render on a Ctx) on a synthetic picture, to time them and to
// compare two builds byte for byte: each effect runs with its defaults and with several randomly chosen (but always the
// same) settings, and a hash of every output is printed ("HASH <effect> <variant> <hex>"). Diff the output of two builds.
//
//   fxbench [--size W H] [--variants N] [--time] [--reps N] [--only id1,id2,...] [--list] [--scale S]
//   --time: "TIME <effect> <ms>" (best of --reps, default 3) for the defaults and for every variant (the slowest)
//   Without --time only the hashes are printed.
#include "../../src/ofx/builtins_impl.h"
#include "../../src/core/pixrows.h"
#include "../../src/core/base.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace reno;
using namespace reno::ofx::builtin;

static double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
// CPU time of the whole process (all threads): the machine is shared, so wall time says little
static double cpuMs()
{
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static Buf syntheticPicture(int W, int H, uint32_t seed)
{
    Buf b;
    b.resize(W, H);
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.f; };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float* p = b.at(x, y);
            float r = 0.2f + 0.6f * x / W, g = 0.3f + 0.5f * y / H, bl = 0.5f + 0.3f * std::sin(x * 0.05f) * std::cos(y * 0.04f);
            if (((x / 40) + (y / 30)) % 2 == 0) { r *= 0.5f; bl = 1.f - bl; }
            const float dx = x - W * 0.7f, dy = y - H * 0.3f;
            if (dx * dx + dy * dy < (H * 0.12f) * (H * 0.12f)) { r = 1.f; g = 0.95f; bl = 0.8f; }
            r += (rnd() - 0.5f) * 0.1f; g += (rnd() - 0.5f) * 0.1f; bl += (rnd() - 0.5f) * 0.1f;
            r = std::min(1.f, std::max(0.f, r)); g = std::min(1.f, std::max(0.f, g)); bl = std::min(1.f, std::max(0.f, bl));
            r = std::floor(r * 255.f + 0.5f) / 255.f; g = std::floor(g * 255.f + 0.5f) / 255.f; bl = std::floor(bl * 255.f + 0.5f) / 255.f;
            float a = 1.f;
            if (x < W / 6 && y > H * 5 / 6) a = 0.25f + 0.5f * x / (W / 6.f);
            if (x > W * 5 / 6 && y < H / 6) a = 0.f;                  // a transparent corner too
            p[0] = r * a; p[1] = g * a; p[2] = bl * a; p[3] = a;
        }
    return b;
}

typedef std::map<std::string, std::vector<double>> Params;

static uint64_t fnv(uint64_t h, const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 2654435761ull + 88172645463325252ull) {}
    double next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (double)(s >> 11) / 9007199254740992.0; }
};

static double pick(Rng& r, double lo, double hi) { return lo + (hi - lo) * r.next(); }

static Params variant(const Def& d, int v)
{
    Params p;
    for (const PDef& pd : d.params) p[pd.name] = pd.def;
    if (v == 0) return p;
    Rng r((uint64_t)v * 7919 + std::hash<std::string>()(d.id) % 100003);
    for (const PDef& pd : d.params) {
        std::vector<double>& val = p[pd.name];
        const double lo = std::isnan(pd.dmin) ? pd.min : pd.dmin, hi = std::isnan(pd.dmax) ? pd.max : pd.dmax;
        const bool okRange = lo > -1e6 && hi < 1e6 && hi > lo;
        switch (pd.type) {
        case PT::Double: if (okRange) val[0] = r.next() < 0.15 ? (r.next() < 0.5 ? lo : hi) : pick(r, lo, hi); break;
        case PT::Int: if (okRange) val[0] = std::floor(pick(r, lo, hi + 0.999)); break;
        case PT::Bool: val[0] = r.next() < 0.5 ? 0 : 1; break;
        case PT::Choice: if (!pd.choices.empty()) val[0] = std::floor(r.next() * (double)pd.choices.size()); break;
        case PT::RGBA: case PT::RGB: for (size_t k = 0; k < val.size(); ++k) val[k] = pick(r, 0, 1); break;
        case PT::Double2D: for (size_t k = 0; k < val.size(); ++k) val[k] = pick(r, 0, 1); break;
        case PT::Double3D: if (okRange) for (size_t k = 0; k < val.size(); ++k) val[k] = pick(r, lo, hi); break;
        default: break;
        }
    }
    return p;
}

int main(int argc, char** argv)
{
    int W = 480, H = 270, variants = 6, reps = 3;
    double scale = 1.0;
    bool timing = false, list = false;
    std::set<std::string> only;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--size") && i + 2 < argc) { W = std::atoi(argv[i + 1]); H = std::atoi(argv[i + 2]); i += 2; }
        else if (!std::strcmp(argv[i], "--variants") && i + 1 < argc) variants = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--scale") && i + 1 < argc) scale = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--time")) timing = true;
        else if (!std::strcmp(argv[i], "--list")) list = true;
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) {
            std::string s = argv[++i];
            size_t a = 0;
            while (a <= s.size()) { size_t b = s.find(',', a); if (b == std::string::npos) b = s.size(); if (b > a) only.insert(s.substr(a, b - a)); a = b + 1; }
        }
    }
    const Buf src = syntheticPicture(W, H, 12345), src2 = syntheticPicture(W, H, 777);
    // all the built-ins: the plug-in list gives their identifiers
    std::vector<std::string> ids;
    for (const auto& bi : ofx::builtinInfos()) ids.push_back(bi.id);
    std::sort(ids.begin(), ids.end());
    for (const std::string& id : ids) {
        const Def* d = findDef(id);
        if (!d || !d->render) continue;
        std::string shortId = id.substr(id.find(':') == std::string::npos ? 0 : id.find(':') + 1);
        if (!only.empty() && !only.count(shortId) && !only.count(id)) continue;
        if (list) { std::printf("%s\n", id.c_str()); continue; }
        if (d->temporal || d->changed == nullptr && false) continue;
        double slowest = 0, dflt = 0, slowestCpu = 0, dfltCpu = 0, sumCpu = 0;
        for (int v = 0; v < variants; ++v) {
            Params store = variant(*d, v);
            Ctx c;
            c.testParams = &store;
            c.frameW = W; c.frameH = H;
            c.scale = scale;
            c.progress = 0.3 + 0.1 * v;
            double best = 1e30, bestCpu = 1e30;
            uint64_t h = 1469598103934665603ull;
            for (int rep = 0; rep < (timing ? reps : 1); ++rep) {
                c.src.px.clear(); c.src2.px.clear(); c.out.px.clear();
                if (!d->generator) c.src = src;
                if (d->transition) c.src2 = src2;
                c.out.resize(W, H);
                const double t0 = nowMs(), c0 = cpuMs();
                try { d->render(c); } catch (...) { std::printf("EXC %s %d\n", shortId.c_str(), v); }
                best = std::min(best, nowMs() - t0); bestCpu = std::min(bestCpu, cpuMs() - c0);
            }
            // HASH: the 8-bit frame the host would store (what the next effect and the picture see: must be the same);
            // FHASH: the floats (-0 taken as 0): a difference there that HASH does not show is below what a frame can hold
            std::vector<uint8_t> bytes((size_t)W * H * 4);
            for (int y = 0; y < H; ++y) pix::floatRowToBytes(c.out.at(0, y), &bytes[(size_t)y * W * 4], W, false);
            h = fnv(h, bytes.data(), bytes.size());
            uint64_t fh = 1469598103934665603ull;
            for (float f : c.out.px) { if (f == 0.f) f = 0.f; fh = fnv(fh, &f, sizeof f); }
            std::printf("HASH %s %d %016llx\nFHASH %s %d %016llx\n", shortId.c_str(), v, (unsigned long long)h, shortId.c_str(), v, (unsigned long long)fh);
            if (v == 0) { dflt = best; dfltCpu = bestCpu; }
            slowest = std::max(slowest, best); slowestCpu = std::max(slowestCpu, bestCpu); sumCpu += bestCpu;
        }
        if (timing) std::printf("TIME %-28s default %8.2f ms (cpu %8.2f)  slowest %8.2f ms (cpu %8.2f)  mean cpu %8.2f\n", shortId.c_str(), dflt, dfltCpu, slowest, slowestCpu, sumCpu / variants);
        std::fflush(stdout);
    }
    return 0;
}
