// Checks that the SSE2 blend-mode rows in src/engine/blendrows.h give exactly the bytes of the scalar loop they replace.
#include "../../src/engine/blendrows.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace reno;

static uint32_t rs = 777;
static uint32_t rnd() { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

static float chan(Blend m, float s, float d)
{
    switch (m) {
    case Blend::Add: return std::min(1.f, s + d);
    case Blend::Subtract: return std::max(0.f, d - s);
    case Blend::Multiply: return s * d;
    case Blend::Screen: return s + d - s * d;
    case Blend::Overlay: return d <= 0.5f ? 2.f * s * d : 1.f - 2.f * (1.f - s) * (1.f - d);
    case Blend::HardLight: return s <= 0.5f ? 2.f * s * d : 1.f - 2.f * (1.f - s) * (1.f - d);
    case Blend::Dodge: return d <= 0.f ? 0.f : (s >= 1.f ? 1.f : std::min(1.f, d / (1.f - s)));
    case Blend::Burn: return d >= 1.f ? 1.f : (s <= 0.f ? 0.f : 1.f - std::min(1.f, (1.f - d) / s));
    case Blend::Darken: return std::min(s, d);
    case Blend::Lighten: return std::max(s, d);
    case Blend::Difference: return std::fabs(s - d);
    case Blend::DiffSquared: return (s - d) * (s - d);
    default: return s;
    }
}

static void scalarRow(Blend M, uint8_t* d, const uint8_t* s, int n, float opacity)
{
    const float k = 1.f / 255.f;
    for (int x = 0; x < n; ++x, d += 4, s += 4) {
        const float sa = s[3] * k * opacity;
        if (sa <= 0.f) continue;
        const float da = d[3] * k;
        float out[4];
        if (M == Blend::Cut) {
            for (int c = 0; c < 4; ++c) out[c] = d[c] * k * (1.f - sa);
        } else {
            for (int c = 0; c < 3; ++c) {
                const float sp = s[c] * k * opacity, dp = d[c] * k;
                const float sc = std::min(1.f, sp / sa);
                const float dc = da > 0.f ? std::min(1.f, dp / da) : 0.f;
                out[c] = sp * (1.f - da) + dp * (1.f - sa) + sa * da * chan(M, sc, dc);
            }
            out[3] = sa + da - sa * da;
        }
        for (int c = 0; c < 4; ++c) {
            const float v = out[c] * 255.f + 0.5f;
            d[c] = (uint8_t)(v >= 255.f ? 255 : (v <= 0.f ? 0 : (int)v));
        }
        for (int c = 0; c < 3; ++c) if (d[c] > d[3]) d[c] = d[3];
    }
}

template <Blend M>
static int check()
{
    int bad = 0;
    for (int iter = 0; iter < 3000; ++iter) {
        const int n = 1 + (int)(rnd() % 70);
        std::vector<uint8_t> s(n * 4), d(n * 4);
        for (auto& v : s) v = (uint8_t)rnd();
        for (auto& v : d) v = (uint8_t)rnd();
        if (iter & 1) for (int i = 0; i < n; ++i) for (int c = 0; c < 3; ++c) { s[i*4+c] = std::min(s[i*4+c], s[i*4+3]); d[i*4+c] = std::min(d[i*4+c], d[i*4+3]); }
        if (iter % 5 == 0) for (int i = 0; i < n; ++i) if (rnd() & 1) s[i*4+3] = 0;
        if (iter % 7 == 0) for (int i = 0; i < n; ++i) if (rnd() & 1) d[i*4+3] = 0;
        if (iter % 11 == 0) for (int i = 0; i < n; ++i) s[i*4+3] = (rnd() & 1) ? 255 : 0;
        if (iter % 13 == 0) for (int i = 0; i < n; ++i) { d[i*4+3] = 255; for (int c = 0; c < 3; ++c) d[i*4+c] = (rnd() & 1) ? 0 : 255; }
        if (iter % 4 == 2) for (int i = 0; i < n; ++i) { s[i*4+3] = 255; d[i*4+3] = 255; }          // solid over solid
        if (iter % 9 == 4) for (int i = 0; i < n; ++i) if (rnd() % 8) { s[i*4+3] = 255; d[i*4+3] = 255; }
        const float op = iter % 3 == 0 ? 1.f : (rnd() % 1000 + 1) / 1000.f;
        std::vector<uint8_t> ref = d, out = d;
        scalarRow(M, ref.data(), s.data(), n, op);
        const int done = pix::blendModeRow<M>(out.data(), s.data(), n, op);
        scalarRow(M, out.data() + (size_t)done * 4, s.data() + (size_t)done * 4, n - done, op);
        if (ref != out) { if (++bad < 4) std::printf("mismatch mode %d n=%d iter=%d\n", (int)M, n, iter); }
    }
    return bad;
}

int main()
{
    int bad = 0;
    bad += check<Blend::Add>(); bad += check<Blend::Subtract>(); bad += check<Blend::Multiply>(); bad += check<Blend::Screen>();
    bad += check<Blend::Overlay>(); bad += check<Blend::HardLight>(); bad += check<Blend::Dodge>(); bad += check<Blend::Burn>();
    bad += check<Blend::Darken>(); bad += check<Blend::Lighten>(); bad += check<Blend::Difference>(); bad += check<Blend::DiffSquared>();
    bad += check<Blend::Cut>();
    const int W = 1920, H = 540;
    std::vector<uint8_t> a((size_t)W * 4 * H), b(a.size());
    for (auto& v : a) v = (uint8_t)rnd();
    for (auto& v : b) v = (uint8_t)rnd();
    for (int solid = 0; solid < 2; ++solid) {
        if (solid) for (size_t i = 3; i < a.size(); i += 4) a[i] = b[i] = 255;
        const float op = solid ? 1.f : 0.9f;
        const auto t0 = std::chrono::steady_clock::now();
        for (int y = 0; y < H; ++y) scalarRow(Blend::Difference, a.data() + (size_t)y * W * 4, b.data() + (size_t)y * W * 4, W, op);
        const auto t1 = std::chrono::steady_clock::now();
        for (int y = 0; y < H; ++y) pix::blendModeRow<Blend::Difference>(a.data() + (size_t)y * W * 4, b.data() + (size_t)y * W * 4, W, op);
        const auto t2 = std::chrono::steady_clock::now();
        std::printf("difference %dx%d%s: scalar %.1f ms, sse2 %.1f ms\n", W, H, solid ? " (solid over solid)" : "", std::chrono::duration<double, std::milli>(t1 - t0).count(),
                    std::chrono::duration<double, std::milli>(t2 - t1).count());
    }
    std::printf(bad ? "FAIL %d\n" : "blendrows OK\n", bad);
    return bad != 0;
}
