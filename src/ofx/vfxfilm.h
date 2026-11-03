// VEGAS's Film Effects (the DirectX transform in sftrans1.dll), re-implemented from its code. The effect is a small particle simulation that carries its state from frame to
// frame, plus per-frame colour work:
//   * every frame, in this order: the input is copied, turned grey (if asked), the particle list is made (first frame only), grain is added (if > 0), the tint is mixed in
//     (if > 0), every particle is drawn in list order, and then the simulation is stepped one frame;
//   * the random numbers all come from one shift-register generator (VegasShiftRandom) seeded with 123456 when the particle list is made, and it is never reseeded, so the
//     picture of a frame depends on every frame drawn before it (Reno replays from the start of the event, see FilmCache in builtins_more_creative.cpp);
//   * seven particle groups (the dialog shows five: Flicker 0, Scratches 1, Dust 2, Hair 3, Jitter 4; 5 and 6 make nothing), each with three numbers and an on switch;
//     the dialog sets the first number to Amount x 0.0002 and the third to Amount x 32 for the group of the chosen type.
// 'Rows' are in memory order; the 8-bit pictures are B, G, R, A bytes and the float ones B, G, R, A floats (premultiplied), as the code has them.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "vfxrand.h"

namespace reno {
namespace ofx {
namespace builtin {
namespace vfxfilm {

// 2^-15: the generator's low 16 bits less 0x8000 are turned into -1 .. 1 with it
const double kC6 = 3.0517578125e-05;
inline double unit(uint32_t w) { return (double)((int)(w & 0xffffu) - 0x8000) * kC6; }

struct Group { double a = 0, b = 0, c = 0; int enable = 0; };

struct Params {
    Group g[7];
    double grain = 0;
    bool grey = false;
    double tint = 0;
    double tintBgr[3] = {0, 0, 0};          // the tint colour, in memory order (blue first)
};

// A particle: place in -1 .. 1 across the frame, velocity, age and lifetime in seconds, type (the group that made it)
struct Obj {
    double x = 0, y = 0, vx = 0, vy = 0, age = 0, life = 0;
    int type = 0;
};

struct Sim {
    Sim() : rng(123456u) {}
    VegasShiftRandom rng;
    std::vector<Obj> objs;
    double acc[7] = {0, 0, 0, 0, 0, 0, 0};          // time since each group last made particles
    bool started = false;
};

// The picture a frame works on; with no buffers the pass only uses up the random numbers (the replay of earlier frames).
struct Surf {
    int W = 0, H = 0;
    bool bytes = true;
    uint8_t* b = nullptr;
    float* f = nullptr;
    bool live() const { return bytes ? b != nullptr : f != nullptr; }
    uint8_t* p8(int x, int y) const { return b + ((size_t)y * (size_t)W + (size_t)x) * 4; }
    float* pf(int x, int y) const { return f + ((size_t)y * (size_t)W + (size_t)x) * 4; }
};


// ---- the dialog's presets and how the OpenFX-style settings become the seven groups
// VEGAS's Film Effects dialog (sftrans1.dll, the table at 0x18056e130, 0x120 bytes a preset) sets every number of the effect for a preset, the five particle groups
// included: Reset to None, Very Old Film, Low Quality Color Film, Circa 1908, Circa 1980. The tint colour is stored blue first. The groups are the dialog's types:
// 0 Flicker, 1 Scratches, 2 Dust, 3 Hair, 4 Jitter (5 and 6 are never on). The Amount slider of the dialog sets a type's first number to Amount x 0.0002 and its third
// to Amount x 32 (both cut to that range) and leaves its second number and the other types as they are; the presets set all three numbers of every type.
struct Preset {
    double grain;
    int grey;
    double tint;
    double tintBgr[3];
    Group g[7];
};
inline const Preset* presets()
{
    static const Preset kPresets[5] = {
    {0.0, 0, 0.0, {0.35294117647058826, 0.7176470588235294, 0.7490196078431373}, {
        {0.0, 0.0, 0.0, 1},
        {0.0, 3e-05, 0.0, 1},
        {0.0, 0.0005, 0.0, 1},
        {0.0, 5e-05, 0.0, 1},
        {0.0, 0.0, 0.0, 1},
        {0.0, 0.0, 0.0, 0},
        {0.0, 0.0, 0.0, 0}}}, // Reset to None
    {0.15, 1, 0.25, {0.35294117647058826, 0.7176470588235294, 0.7490196078431373}, {
        {0.0001, 0.0, 3.2, 1},
        {0.0001, 3e-05, 3.2, 1},
        {0.0005, 0.0005, 16.0, 1},
        {0.0005, 5e-05, 16.0, 1},
        {0.0002, 0.0, 6.4, 1},
        {0.0, 0.0, 0.0, 0},
        {0.0, 0.0, 0.0, 0}}}, // Very Old Film
    {0.15, 0, 0.35, {0.7490196078431373, 0.7176470588235294, 0.35294117647058826}, {
        {0.0, 0.0, 0.0, 0},
        {2e-05, 3e-05, 0.64, 1},
        {0.0002, 0.0005, 6.4, 1},
        {0.0002, 5e-05, 6.4, 1},
        {0.0, 0.0, 0.0, 0},
        {0.0, 0.0, 0.0, 0},
        {0.0, 0.0, 0.0, 0}}}, // Low Quality Color Film
    {0.25, 1, 0.35, {0.35294117647058826, 0.7176470588235294, 0.7490196078431373}, {
        {0.0001, 0.0, 3.2, 1},
        {0.0002, 3e-05, 6.4, 1},
        {0.0005, 0.0005, 16.0, 1},
        {0.0005, 5e-05, 16.0, 1},
        {0.0008, 0.0, 25.6, 1},
        {0.0, 0.0, 0.0, 0},
        {0.0, 0.0, 0.0, 0}}}, // Circa 1908
    {0.1, 0, 0.2, {0.7490196078431373, 0.7176470588235294, 0.35294117647058826}, {
        {0.0, 0.0, 0.0, 0},
        {1e-05, 3e-05, 0.32, 1},
        {0.0001, 0.0005, 3.2, 1},
        {0.0001, 5e-05, 3.2, 1},
        {1e-05, 0.0, 0.32, 1},
        {0.0, 0.0, 0.0, 0},
        {0.0, 0.0, 0.0, 0}}}, // Circa 1980
    };
    return kPresets;
}

// The OpenFX settings (Type in the order Dust, Flicker, Hair, Jitter, Scratches; Amount 0..1; the colour red first, 0..1) as the groups: a setting that is exactly one of
// the presets (VEGAS's own preset package sets Type 0 and the Amount of the dust) is that preset with everything it sets; anything else starts from Reset to None
// and switches on only the chosen type, as the dialog's slider does.
inline Params paramsFor(double grain, double tint, double r, double g, double b, bool grey, bool particles, int type, double amount)
{
    static const int kGroupOfType[5] = {2, 0, 3, 4, 1};
    const Preset* P = presets();
    auto cl = [](double v, double lo, double hi) { return !(v > lo) ? lo : (v > hi ? hi : v); };
    Params p;
    grain = cl(grain, 0, 1); tint = cl(tint, 0, 1); amount = cl(amount, 0, 1);
    type = type < 0 ? 0 : (type > 4 ? 4 : type);
    if (particles && type == 0) {
        for (int k = 1; k < 5; ++k) {
            const Preset& q = P[k];
            if (std::fabs(grain - q.grain) < 1e-6 && std::fabs(tint - q.tint) < 1e-6 && grey == (q.grey != 0) && std::fabs(amount - q.g[2].c / 32.0) < 1e-6 &&
                std::fabs(r - q.tintBgr[2]) < 2e-3 && std::fabs(g - q.tintBgr[1]) < 2e-3 && std::fabs(b - q.tintBgr[0]) < 2e-3) {
                p.grain = q.grain; p.grey = q.grey != 0; p.tint = q.tint;
                for (int c = 0; c < 3; ++c) p.tintBgr[c] = q.tintBgr[c];
                for (int c = 0; c < 7; ++c) p.g[c] = q.g[c];
                return p;
            }
        }
    }
    p.grain = grain; p.grey = grey; p.tint = tint;
    p.tintBgr[0] = cl(b, 0, 1); p.tintBgr[1] = cl(g, 0, 1); p.tintBgr[2] = cl(r, 0, 1);
    for (int c = 0; c < 7; ++c) p.g[c] = P[0].g[c];
    if (particles) {
        Group& q = p.g[kGroupOfType[type]];
        q.a = amount * 0.0002;
        q.c = amount * 32.0;
        q.enable = 1;
    } else {
        for (int c = 0; c < 7; ++c) p.g[c].enable = 0;
    }
    return p;
}

// ---- particles made (the factories at 0x1801d2b50..0x1801d3040)
inline Obj makeObj(int type, VegasShiftRandom& rng)
{
    Obj o;
    switch (type) {
    case 0:                                         // flash: one number drawn and not used
        rng.next();
        o.life = 0.125;
        break;
    case 1:                                         // scratch: a place across, then the lifetime
        o.x = unit(rng.next());
        o.type = 1;
        o.life = unit(rng.next()) * 0.2 + 0.45;
        break;
    case 2:
    case 3:                                         // dust, hair: a place across and down, then the lifetime
        o.x = unit(rng.next());
        o.y = unit(rng.next());
        o.type = type;
        o.life = unit(rng.next()) * 0.05 + 0.1;
        break;
    case 4:                                         // jitter: the lifetime, the place later
        o.type = 4;
        o.life = unit(rng.next()) * 0.15 + 0.3;
        break;
    default:                                        // groups 5 and 6 make a blank particle that lives no time
        break;
    }
    return o;
}

// ---- the first frame: the particles the groups start with (WorkProc 0x1801d1a50, before the grain)
inline void startSim(Sim& s, const Params& p, int W, int H)
{
    s.started = true;
    const double wh = (double)H * (double)W;
    for (int k = 0; k < 7; ++k) {
        const Group& g = p.g[k];
        if (g.enable == 0 || !(g.c > 0)) continue;
        const int n = (int)((unit(s.rng.next()) * g.b + g.a) * wh);
        for (int i = 0; i < n; ++i) s.objs.push_back(makeObj(k, s.rng));
    }
    for (int k = 0; k < 7; ++k) s.acc[k] = 0;
}

// ---- one step of the simulation (0x1801d22e0); dt seconds, wh the frame's pixel count
inline void stepSim(Sim& s, const Params& p, double dt, double wh)
{
    size_t keep = 0;
    for (size_t i = 0; i < s.objs.size(); ++i) {
        Obj o = s.objs[i];
        const double ra = unit(s.rng.next()), rb = unit(s.rng.next());
        o.vx = ra / 50.0 * dt + o.vx;
        o.vy = rb / 50.0 * dt + o.vy;
        o.x = o.vx + o.x;
        o.y = o.vy + o.y;
        o.age = dt + o.age;
        if (!(o.life < 0) && o.age > o.life) o.age = -1.0;
        if (o.age > 0) s.objs[keep++] = o;          // (the others die)
    }
    s.objs.resize(keep);
    // new particles: every group that is on counts time up, and when the count passes a random wait it makes a batch. A later group's batch goes in front of an earlier one's
    // (each is inserted just after the old particles).
    std::vector<std::vector<Obj>> batches;
    for (int k = 0; k < 7; ++k) {
        const Group& g = p.g[k];
        if (g.enable) s.acc[k] += dt;
        const double v = (unit(s.rng.next()) + 1.0) * g.c * 0.5;
        if (g.enable && v > 0 && s.acc[k] > 1.0 / v) {
            const double m = (k == 0 || k == 1 || k == 4) ? 68000.0 : wh;
            const int n = (int)((unit(s.rng.next()) * g.b + g.a) * m);
            std::vector<Obj> batch;
            for (int i = 0; i < n; ++i) batch.push_back(makeObj(k, s.rng));
            batches.push_back(std::move(batch));
            s.acc[k] = 0;
        }
    }
    for (size_t b = batches.size(); b-- > 0;)
        s.objs.insert(s.objs.end(), batches[b].begin(), batches[b].end());
}

// ---- pixel work
inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
inline void add8(uint8_t* px, int d)
{
    px[0] = (uint8_t)clamp8(px[0] + d);
    px[1] = (uint8_t)clamp8(px[1] + d);
    px[2] = (uint8_t)clamp8(px[2] + d);
}
// adding d to a premultiplied float pixel as 0x1801d5050 does: through the colour when 0 < alpha < 1, plain (and alpha set to 1) from alpha 1 up, to black when alpha is 0
inline void addF(float* px, float d)
{
    const float a = px[3];
    if (a > 1e-6f) {
        if (1.0f > a) {
            px[0] = (px[0] / a + d) * a;
            px[1] = (px[1] / a + d) * a;
            px[2] = (px[2] / a + d) * a;
        } else {
            px[3] = 1.0f;
            px[0] = d + px[0];
            px[1] = d + px[1];
            px[2] = d + px[2];
        }
    } else {
        px[0] = px[1] = px[2] = px[3] = 0.f;
    }
}

inline void greyscale(Surf& S)
{
    if (!S.live()) return;
    for (int y = 0; y < S.H; ++y)
        for (int x = 0; x < S.W; ++x) {
            if (S.bytes) {
                uint8_t* p = S.p8(x, y);
                const unsigned g = ((unsigned)p[1] + (unsigned)p[2] + (unsigned)p[0]) / 3u;
                const uint8_t v = (uint8_t)(g < p[3] ? g : p[3]);
                p[0] = p[1] = p[2] = v;
            } else {
                float* p = S.pf(x, y);
                const float g = ((p[2] + p[1]) + p[0]) / 3.0f;
                p[0] = p[1] = p[2] = g;
            }
        }
}

// grain: one random number per pixel, in memory order (the numbers are used up when the picture is not live)
inline void grain(Surf& S, double amount, VegasShiftRandom& rng)
{
    if (!(amount > 0)) return;
    const int K = (int)(amount * 16384.0);
    const float amountF = (float)amount;
    for (int y = 0; y < S.H; ++y)
        for (int x = 0; x < S.W; ++x) {
            const uint32_t w = rng.next();
            if (!S.live()) continue;
            if (S.bytes) {
                const int d = (((int)(w & 0xffu) - 0x7f) * K) / 16384;
                uint8_t* p = S.p8(x, y);
                add8(p, d);
                if (p[3] < 255) p[3] = (uint8_t)clamp8(p[3] + d);
            } else {
                const float n = (float)unit(w) * 0.5f * amountF;
                addF(S.pf(x, y), n);
            }
        }
}

// tint toward a colour (blue, green, red, then the strength, all 0..1) over the whole picture (0x18025b720)
inline void tintPass(Surf& S, const double c4[4])
{
    if (!S.live()) return;
    if (S.bytes) {
        int c[3], a;
        for (int k = 0; k < 3; ++k) {
            const double v = c4[k] * 255.0;
            c[k] = (int)(0 > v ? 0.0 : std::min(255.0, v));
        }
        {
            const double v = c4[3] * 255.0;
            a = (int)(0 > v ? 0.0 : std::min(255.0, v));
        }
        for (int y = 0; y < S.H; ++y) {
            uint8_t* p = S.p8(0, y);
            for (int x = 0; x < S.W; ++x, p += 4)
                for (int k = 0; k < 3; ++k) {
                    const int e = (c[k] - (int)p[k]) * a;
                    p[k] = (uint8_t)((int)p[k] + ((e + (e < 0 ? 255 : 0)) >> 8));
                }
        }
    } else {
        float c[3], a;
        for (int k = 0; k < 3; ++k) c[k] = (float)(0 > c4[k] ? 0.0 : std::min(1.0, c4[k]));
        a = (float)(0 > c4[3] ? 0.0 : std::min(1.0, c4[3]));
        for (int y = 0; y < S.H; ++y) {
            float* p = S.pf(0, y);
            for (int x = 0; x < S.W; ++x, p += 4)
                for (int k = 0; k < 3; ++k) p[k] = (c[k] - p[k]) * a + p[k];
        }
    }
}

// C's remainder of a whole 32-bit word
inline int smod(uint32_t w, int m) { return (int)w % m; }

inline int clampIndex(double v, int n)       // 0 for anything below 0 (and NaN), else v cut to n - 1, whole
{
    return (int)(0 > v ? 0.0 : std::min((double)(n - 1), v));
}

// ---- drawing one particle (the renderer table at 0x180cf6990)
inline void drawObj(Surf& S, const Obj& o, VegasShiftRandom& rng)
{
    const int W = S.W, H = S.H;
    const double hw = (double)W * 0.5, hh = (double)H * 0.5;
    switch (o.type) {
    case 0: {                                       // flash: the whole picture toward white by 0 .. 0.08
        const double v = (unit(rng.next()) + 1.0) * 0.5 * 0.08;
        const double a = 0 > v ? 0.0 : std::min(1.0, v);
        const double c4[4] = {1.0, 1.0, 1.0, a};
        tintPass(S, c4);
        break;
    }
    case 1: {                                       // vertical scratch
        const int x = (int)(hw * o.x + hw);
        if (x < 0 || x >= W) return;
        if (0 > o.age) return;
        if (S.bytes) {
            const int line = smod(rng.next(), 16) + 0x20;
            for (int y = 0; y < H; ++y) {
                const int d = smod(rng.next(), 16) + line;
                if (S.live()) add8(S.p8(x, y), d);
            }
        } else {
            const float line = (float)unit(rng.next()) * 0.065f + 0.125f;
            for (int y = 0; y < H; ++y) {
                const float d = (float)unit(rng.next()) * 0.065f + line;
                if (S.live()) addF(S.pf(x, y), d);
            }
        }
        break;
    }
    case 2: {                                       // dust: a short wandering line of faint light
        const int x0 = (int)(hw * o.x + hw), y0 = (int)(hh * o.y + hh);
        const int n = (int)(unit(rng.next()) * 10.0 + 15.0);
        if (0 > o.age) return;
        double x = (double)x0, y = (double)y0;
        if (S.bytes) {
            const int d = (int)((unit(rng.next()) + 1.0) * 7.0);
            for (int i = 0; i < n; ++i) {
                const double dx = unit(rng.next()) * 0.97, dy = unit(rng.next());
                if (S.live()) add8(S.p8(clampIndex(x, W), clampIndex(y, H)), d);
                x += dx;
                y += dy;
            }
        } else {
            const float d = (float)((unit(rng.next()) + 1.0) * 0.027450980392156862);
            for (int i = 0; i < n; ++i) {
                const double dx = unit(rng.next()) * 0.97, dy = unit(rng.next());
                if (S.live()) addF(S.pf(clampIndex(x, W), clampIndex(y, H)), d);
                x += dx;
                y += dy;
            }
        }
        break;
    }
    case 3: {                                       // hair: a long weaving line, speckled
        const int x0 = (int)(hw * o.x + hw), y0 = (int)(hh * o.y + hh);
        const int n = (int)(unit(rng.next()) * 10.0 + 65.0);
        if (0 > o.age) return;
        double x = (double)x0, y = (double)y0;
        if (S.bytes) {
            const double ra = unit(rng.next());
            const double phase = unit(rng.next()) * 3.14;
            const int base = (int)((ra + 1.0) * 8.0);
            for (int j = 0; j < n; ++j) {
                const int d = smod(rng.next(), 8) + base;
                if (S.live()) add8(S.p8(clampIndex(x, W), clampIndex(y, H)), d);
                const double wc = unit(rng.next()), wd = unit(rng.next());
                const double s = std::sin((double)j / (double)n * 3.14 * 2.0 + phase);
                x += s * (wc * 1.65);
                y += (wd + 1.0) * 0.15;
            }
        } else {
            const double ra = unit(rng.next());
            const double phase = unit(rng.next()) * 3.14;
            const float base = (float)((ra + 1.0) * 0.0313725508749485);
            for (int j = 0; j < n; ++j) {
                const float d = (float)(unit(rng.next()) * 0.0313725508749485) + base;
                if (S.live()) addF(S.pf(clampIndex(x, W), clampIndex(y, H)), d);
                const double wc = unit(rng.next()), wd = unit(rng.next());
                const double s = std::sin((double)j / (double)n * 3.14 * 2.0 + phase);
                x += s * (wc * 1.65);
                y += (wd + 1.0) * 0.15;
            }
        }
        break;
    }
    case 4: {                                       // jitter: the whole picture moved, opaque black coming in
        if (0 > o.age) return;
        if (!S.live()) return;
        const int dx = (int)(hw * o.x * 0.1), dy = (int)(hh * o.y * 1.4);
        if (S.bytes) {
            std::vector<uint8_t> src(S.b, S.b + (size_t)W * (size_t)H * 4);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    uint8_t* d = S.p8(x, y);
                    const int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sx >= W || sy < 0 || sy >= H) { d[0] = d[1] = d[2] = 0; d[3] = 255; }
                    else std::memcpy(d, &src[((size_t)sy * (size_t)W + (size_t)sx) * 4], 4);
                }
        } else {
            std::vector<float> src(S.f, S.f + (size_t)W * (size_t)H * 4);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    float* d = S.pf(x, y);
                    const int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sx >= W || sy < 0 || sy >= H) { d[0] = d[1] = d[2] = 0.f; d[3] = 1.0f; }
                    else std::memcpy(d, &src[((size_t)sy * (size_t)W + (size_t)sx) * 4], 16);
                }
        }
        break;
    }
    default:
        break;
    }
}

// ---- one whole frame: the picture (or just the random numbers it uses), then the simulation step
inline void frame(Sim& s, const Params& p, Surf& S, double dt)
{
    if (S.W <= 0 || S.H <= 0) return;
    if (p.grey) greyscale(S);
    if (!s.started) startSim(s, p, S.W, S.H);
    grain(S, p.grain, s.rng);
    if (p.tint > 0) {
        const double c4[4] = {p.tintBgr[0], p.tintBgr[1], p.tintBgr[2], p.tint};
        tintPass(S, c4);
    }
    for (size_t i = 0; i < s.objs.size(); ++i) {
        const int t = s.objs[i].type;
        if (t < 0 || t > 4 || p.g[t].enable == 0) continue;
        drawObj(S, s.objs[i], s.rng);
    }
    stepSim(s, p, dt, (double)S.H * (double)S.W);
}

}
}
}
}
