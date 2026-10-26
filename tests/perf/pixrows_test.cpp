// Checks that the SSE2 row kernels in src/core/pixrows.h give exactly the bytes of the plain loops they replace.
#include "../../src/core/pixrows.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cmath>
#include <cstring>
#include <limits>
#include <algorithm>
using namespace reno::pix;

static uint32_t rs = 12345;
static uint32_t rnd() { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

int main()
{
    int bad = 0;
    for (int iter = 0; iter < 4000; ++iter) {
        int n = 1 + (int)(rnd() % 67);
        std::vector<uint8_t> s(n * 4), d(n * 4), a(n * 4), ref(n * 4), out(n * 4);
        for (auto& v : s) v = (uint8_t)rnd();
        for (auto& v : d) v = (uint8_t)rnd();
        for (auto& v : a) v = (uint8_t)rnd();
        if (iter & 1) for (int i = 0; i < n; ++i) { s[i*4] = std::min<int>(s[i*4], s[i*4+3]); }  // some premultiplied-like
        if (iter % 7 == 0) for (int i = 0; i < n; ++i) s[i*4+3] = (rnd() & 1) ? 255 : 0;
        if (iter % 11 == 0) for (int i = 0; i < n; ++i) s[i*4+3] = 255;           // (solid: the whole-group shortcut)
        unsigned op = (iter % 3 == 0) ? 255 : (rnd() % 256);
        // over
        ref = d; out = d;
        for (int x = 0; x < n; ++x) {
            const uint8_t* p = &s[x*4]; uint8_t* q = &ref[x*4];
            if (op == 255) {
                if (p[3] == 255) { for (int k = 0; k < 4; ++k) q[k] = p[k]; continue; }
                if (p[3] == 0 && !(p[0] | p[1] | p[2])) continue;
                unsigned inv = 255 - p[3];
                for (int k = 0; k < 4; ++k) { unsigned v = p[k] + div255(q[k] * inv); q[k] = v > 255 ? 255 : v; }
            } else {
                unsigned al = div255(p[3] * op), inv = 255 - al;
                for (int k = 0; k < 4; ++k) { unsigned v = div255(p[k] * op) + div255(q[k] * inv); q[k] = v > 255 ? 255 : v; }
            }
        }
        overRow(out.data(), s.data(), n, op);
        if (ref != out) { ++bad; if (bad < 5) { int i = 0; while (ref[i] == out[i]) ++i; printf("over mismatch n=%d op=%u i=%d ref=%d out=%d s=%d d=%d sa=%d\n", n, op, i, ref[i], out[i], s[i], d[i], s[i|3]); } }
        // blend
        unsigned tb = rnd() % 257, ta = 256 - tb;
        for (int i = 0; i < n * 4; ++i) ref[i] = (uint8_t)((a[i] * ta + s[i] * tb + 128) >> 8);
        blendRow(out.data(), a.data(), s.data(), n * 4, ta, tb);
        if (ref != out) { ++bad; if (bad < 5) printf("blend mismatch\n"); }
        // add
        unsigned m = rnd() % 257; ref = d; out = d;
        for (int i = 0; i < n * 4; ++i) { unsigned v = ref[i] + ((s[i] * m + 128) >> 8); ref[i] = v > 255 ? 255 : v; }
        addScaledRow(out.data(), s.data(), n * 4, m);
        if (ref != out) { ++bad; if (bad < 5) printf("add mismatch\n"); }
        // opaque
        ref = d; out = d;
        for (int i = 0; i < n; ++i) ref[i*4+3] = 255;
        opaqueRow(out.data(), n);
        if (ref != out) { ++bad; if (bad < 5) printf("opaque mismatch\n"); }
    }

    // the OpenFX built-ins' conversions (builtins.cpp's store and load before they were vectorised), on awkward floats too
    {
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        const float odd[] = {0.f, -0.f, 1.f, 0.5f, 0.49999997f, 0.5019608f, 1.0000001f, -1.f, 2.f, nan, inf, -inf, 1e30f, 0.0019607843f, 0.0058823531f};
        for (int iter = 0; iter < 3000; ++iter) {
            const int n = 1 + (int)(rnd() % 45);
            std::vector<float> f((size_t)n * 4);
            for (auto& v : f) {
                const unsigned r = rnd();
                if (r % 5 == 0) v = odd[(r >> 4) % (sizeof odd / sizeof *odd)];
                else if (r % 5 == 1) v = (float)((r >> 4) % 256) / 255.f + ((r & 0x100) ? 1e-7f : -1e-7f);
                else if (r % 5 == 2) v = (float)((r >> 4) % 511) / 510.f;
                else v = (float)((r >> 4) % 100000) / 99999.f * 1.2f - 0.1f;
            }
            for (int bgra = 0; bgra < 2; ++bgra) {
                std::vector<uint8_t> ref((size_t)n * 4), out((size_t)n * 4);
                for (int x = 0; x < n; ++x) {
                    float p[4];
                    std::memcpy(p, &f[(size_t)x * 4], sizeof p);
                    if (!(p[3] >= 0.f)) p[3] = 0.f;
                    uint8_t* o = &ref[(size_t)x * 4];
                    const float a = p[3] < 0.f ? 0.f : (p[3] > 1.f ? 1.f : p[3]);
                    auto rn = [](float v) { const int i = (int)v; return (uint8_t)(i + ((v - (float)i) >= 0.5f ? 1 : 0)); };
                    auto q = [&](float c) { c = std::isfinite(c) ? std::min(std::max(c, 0.f), a) : 0.f; return rn(c * 255.f); };
                    o[bgra ? 2 : 0] = q(p[0]); o[1] = q(p[1]); o[bgra ? 0 : 2] = q(p[2]); o[3] = rn(a * 255.f);
                }
                floatRowToBytes(f.data(), out.data(), n, bgra != 0);
                if (ref != out) { ++bad; if (bad < 5) printf("floatRowToBytes mismatch n=%d bgra=%d\n", n, bgra); }
                std::vector<float> g((size_t)n * 4), gr((size_t)n * 4);
                const float k = 1.f / 255.f;
                for (int x = 0; x < n; ++x) {
                    const uint8_t* s2 = &ref[(size_t)x * 4];
                    gr[(size_t)x * 4] = s2[bgra ? 2 : 0] * k; gr[(size_t)x * 4 + 1] = s2[1] * k; gr[(size_t)x * 4 + 2] = s2[bgra ? 0 : 2] * k; gr[(size_t)x * 4 + 3] = s2[3] * k;
                }
                bytesRowToFloat(ref.data(), g.data(), n, bgra != 0);
                if (std::memcmp(g.data(), gr.data(), g.size() * 4)) { ++bad; if (bad < 5) printf("bytesRowToFloat mismatch n=%d bgra=%d\n", n, bgra); }
            }
        }
    }
    // exhaustive div255 lanes
    printf(bad ? "FAIL %d\n" : "pixrows OK\n", bad);
    return bad != 0;
}
