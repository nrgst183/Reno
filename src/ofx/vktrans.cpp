// The transitions and generators on the GPU: for each, a function that plans the compute passes from the effect's settings, reading them exactly as the
// effect's CPU code (builtins_trans.cpp, builtins_gen.cpp) does, and the test cases that make vkcheck / vkfx_test reach every branch of that code. The
// kernels are in tools/gpukernels_trans.py. vkeffects.cpp lists these planners (extraGpuEntries) and they take over the ids they name.
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include "vkeffects.h"
#include "transrec.h"
#include "builtins_deriche.h"
#include "vkshaders/push_layouts.h"

namespace reno {
namespace ofx {
namespace builtin {

namespace {

typedef gpu::Plan Plan;
typedef gpu::Pass Pass;
namespace P = gpu::push;
typedef std::map<std::string, std::vector<double>> PM;

const char* const kPrefix = "com.vegascreativesoftware:";
std::string fullId(const char* id) { return std::strchr(id, ':') ? std::string(id) : std::string(kPrefix) + id; }

// The incoming frame goes up with the plan as a table of w * h * 4 floats (top row first), as Ctx::src2 holds it.
int toTable(Ctx& c, Plan& p)
{
    if (c.src2.px.size() != (size_t)p.w * p.h * 4) return -1;
    return p.addTable(std::move(c.src2.px));
}

// the words of a push block: floats, ints and doubles (two words, high half first) as the kernels' qsf / qf / qi read them
struct Words {
    uint32_t q[32];
    Words() { std::memset(q, 0, sizeof(q)); }
    void f(int k, float v) { std::memcpy(&q[k], &v, 4); }
    void i(int k, int v) { std::memcpy(&q[k], &v, 4); }
    void d(int k, double v)
    {
        uint64_t b;
        std::memcpy(&b, &v, 8);
        q[k] = (uint32_t)(b >> 32);
        q[k + 1] = (uint32_t)b;
    }
};

// ---------------------------------------------------------------------------------------------------- Dissolve (dissolve() / dis::run() in builtins_trans.cpp)
bool planDissolve(Ctx& c, Plan& p)
{
    const int t = toTable(c, p);
    if (t < 0) return false;
    const int type = c.i("Type");
    Words w;
    if (c.levels) {
        const float pf = (float)c.progress;
        const int w1024 = (int)((double)(pf * 1024.0f) + 0.5), t255 = (int)((double)(pf * 255.0f) + 0.5);
        auto clampThr = [](double v) { return v < 0 ? 0 : (int)std::min(255.0, v); };
        if (type == 3 || type == 4) {
            const char* nm[4] = {"ColorBleedSpeedRed", "ColorBleedSpeedGreen", "ColorBleedSpeedBlue", "ColorBleedSpeedAlpha"};
            const char* nmM[4] = {"ColorMorphSpeedRed", "ColorMorphSpeedGreen", "ColorMorphSpeedBlue", "ColorMorphSpeedAlpha"};
            double m[4];
            int M = 0;
            for (int k = 0; k < 4; ++k) {
                m[k] = c.d(type == 3 ? nm[k] : nmM[k]) + 1.0;
                M = std::max(M, ((int)(255.0 / m[k])) & 255);
            }
            int q = (int)((double)((float)M * pf) + 0.5);
            if (type == 4) q &= 255;
            const int r = (M - q) & 255;
            for (int k = 0; k < 4; ++k) { w.i(k, clampThr(q * m[k])); w.i(4 + k, clampThr(r * m[k])); }
        }
        if (type == 8) {
            const Color col = c.color("FadeThroughColor");
            const double cv[3] = {col.r, col.g, col.b};
            for (int k = 0; k < 3; ++k) w.i(8 + k, (int)(cv[k] * 255.0 * col.a));
            w.i(11, (int)(col.a * 255.0));
        }
        P::Dissolve8 pc;
        std::memset(&pc, 0, sizeof(pc));
        pc.w = p.w; pc.h = p.h; pc.type = type; pc.w1024 = w1024; pc.t255 = t255;
        std::memcpy(&pc.q0, w.q, 12 * 4);
        Pass& ps = p.add("dissolve8", 0, 1, p.w, p.h);
        ps.table = t;
        ps.setPush(pc);
        return true;
    }
    const double pr = c.progress;
    switch (type) {
    case 0: case 1:
        w.f(0, (float)std::min(1.0, 2 * (1 - pr)));
        w.f(1, (float)std::min(1.0, 2 * pr));
        break;
    case 2:
        w.f(0, (float)(1 - pr));
        w.f(1, (float)pr);
        break;
    case 3: case 4: {
        const char* nm[4] = {"ColorBleedSpeedRed", "ColorBleedSpeedGreen", "ColorBleedSpeedBlue", "ColorBleedSpeedAlpha"};
        const char* nmM[4] = {"ColorMorphSpeedRed", "ColorMorphSpeedGreen", "ColorMorphSpeedBlue", "ColorMorphSpeedAlpha"};
        for (int k = 0; k < 4; ++k) {
            const double m = 1.0 + clampd(c.d(type == 3 ? nm[k] : nmM[k]), 0, 1) * 0.996;
            if (type == 3) {
                w.d(2 * k, std::min(0.998, pr * m));
                w.d(8 + 2 * k, std::min(0.998, (1 - pr) * m));
            } else {
                w.d(2 * k, std::min(1.0, pr * m));
            }
        }
        break;
    }
    case 5: case 6:
        w.d(0, std::ceil(pr * 255.0 - 1e-6) + 1e-3);
        break;
    case 7: case 8: {
        w.d(0, pr * 2);
        w.d(2, pr * 2 - 1);
        if (type == 8) {
            float mid[4];
            const Color fc = c.color("FadeThroughColor");
            put(mid, Color{fc.r, fc.g, fc.b, 1.0});              // (colorPx(fc): the colour straight, alpha 1)
            for (int k = 0; k < 4; ++k) w.f(4 + k, mid[k]);
        }
        w.f(8, (float)(1 - pr));
        w.f(9, (float)pr);
        w.i(10, pr < 0.5 ? 1 : 0);
        break;
    }
    default:
        w.f(0, (float)clampd(pr * 3, 0, 1));
        w.f(1, (float)clampd(pr * 3 - 1, 0, 1));
        w.f(2, (float)clampd(pr * 3 - 2, 0, 1));
        w.f(3, (float)pr);
        break;
    }
    P::DissolveF pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h; pc.type = type;
    std::memcpy(&pc.q0, w.q, 24 * 4);
    Pass& ps = p.add("dissolve_f", 0, 1, p.w, p.h);
    ps.table = t;
    ps.setPush(pc);
    return true;
}


// ---------------------------------------------------------------------------------------------------- Linear Wipe and Clock Wipe on 8-bit pictures (linearWipe8 / clockWipe8)
bool planLinearWipe8(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    const float ang = (float)c.d("Angle");
    const float kx = 1.0f, ky = 1.0f;
    const float rad = 0.01745329238474369f;
    const float fw = (float)(W - 1), fh = (float)(H - 1);
    float x7 = 0, y12 = 0, o6 = 0, n2 = 0;
    auto tanq = [&](float a) { return (float)std::tan((double)a); };
    if (0.001f >= ang || ang > 359.999f) { x7 = kx; y12 = 0; n2 = fw * kx; o6 = 0; }
    else if (ang <= 89.999f) {
        const float t = tanq((90.0f - ang) * rad);
        x7 = t * kx; y12 = ky; n2 = fw * x7 + fh * ky; o6 = 0;
    } else if (ang <= 90.001f) { y12 = -ky; x7 = 0; n2 = (float)(1 - H) * y12; o6 = 0; }
    else if (ang <= 179.999f) {
        const float t = tanq((90.0f - ang) * rad);
        x7 = t * kx; y12 = ky; o6 = -(fw * x7); n2 = fh * ky + o6;
    } else if (ang <= 180.001f) { x7 = -kx; y12 = 0; o6 = fw * kx; n2 = o6; }
    else if (ang <= 269.999f) {
        const float t = tanq((270.0f - ang) * rad);
        x7 = t * kx; y12 = ky;
        const float a2 = fh * ky, a0 = fw * x7;
        o6 = -a2 - a0; n2 = a2 + a0;
    } else if (ang <= 270.001f) { x7 = 0; y12 = ky; o6 = (float)(1 - H) * ky; n2 = fh * ky; }
    else {
        const float t = tanq((270.0f - ang) * rad);
        x7 = t * kx; o6 = (float)(1 - H) * ky; y12 = ky;
        n2 = fh * ky - fw * x7;
    }
    const float inv = 1.0f / n2;
    const float fe = (float)c.d("Feather");
    const float q = fe * 0.25f, k2 = 2.0f / fe;
    float edge = q + 1.0f;
    edge += q;
    edge *= (float)c.progress;
    edge -= q;
    const float lo = edge - q, hi = q + edge;
    Words w;
    const float v[9] = {x7, y12, o6, inv, edge, lo, hi, q, k2};
    for (int k = 0; k < 9; ++k) w.f(k, v[k]);
    P::WipeLinear8 pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h;
    std::memcpy(&pc.q0, w.q, 9 * 4);
    Pass& ps = p.add("wipe_linear8", 0, 1, p.w, p.h);
    ps.table = toTable(c, p);
    ps.setPush(pc);
    return ps.table >= 0;
}

bool planClockWipe8(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    const bool ccw = c.i("Direction") > 0;
    const float pr = (float)c.progress;
    const float twoPi = 6.2831854820251465f;
    const float f = (float)c.d("FeatherAngle") * 0.01745329238474369f;
    const float f2 = f + twoPi;
    const float edge = f2 * pr;
    const float cx = (float)(W / 2), cy = (float)(H / 2);
    Words w;
    const float v[7] = {f, f2, edge, cx, cy + 0.5f, twoPi, 1.4142135381698608f};
    for (int k = 0; k < 7; ++k) w.f(k, v[k]);
    P::WipeClock8 pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h; pc.ccw = ccw ? 1 : 0; pc.done = pr >= 1.0f ? 1 : 0;
    std::memcpy(&pc.q0, w.q, 7 * 4);
    Pass& ps = p.add("wipe_clock8", 0, 1, p.w, p.h);
    ps.table = toTable(c, p);
    ps.setPush(pc);
    return ps.table >= 0;
}


bool planLinearWipe(Ctx& c, Plan& p)
{
    if (c.levels) return planLinearWipe8(c, p);
    const int t = toTable(c, p);
    if (t < 0) return false;
    const double ang = c.d("Angle") * kPi / 180.0, dx = std::cos(ang), dy = std::sin(ang);
    const double span = std::max(1e-9, std::fabs(dx) * p.w + std::fabs(dy) * p.h);
    const double w = clampd(c.d("Feather"), 0, 1) * 0.5;
    const double edge = c.progress * (1.0 + w);
    Words q;
    q.d(0, dx); q.d(2, dy); q.d(4, span); q.d(6, w); q.d(8, edge);
    P::WipeLinearF pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h;
    std::memcpy(&pc.q0, q.q, 10 * 4);
    Pass& ps = p.add("wipe_linear_f", 0, 1, p.w, p.h);
    ps.table = t;
    ps.setPush(pc);
    return true;
}

bool planVenetian(Ctx& c, Plan& p)
{
    const int t = toTable(c, p);
    if (t < 0) return false;
    const double ang = c.d("Angle") * kPi / 180.0, dx = std::cos(ang), dy = -std::sin(ang);
    const double n = std::max(1.0, c.d("NumberOfBlinds"));
    const double span = std::fabs(dx) + std::fabs(dy);
    const double f = std::max(1e-4, clampd(c.d("Feather"), 0, 1) * 0.5 + 0.002);
    const double edge = c.progress * (1.0 + f + 0.0);
    Words q;
    q.d(0, dx); q.d(2, dy); q.d(4, span); q.d(6, n); q.d(8, f); q.d(10, edge);
    P::WipeVenetianF pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h;
    std::memcpy(&pc.q0, q.q, 12 * 4);
    Pass& ps = p.add("wipe_venetian_f", 0, 1, p.w, p.h);
    ps.table = t;
    ps.setPush(pc);
    return true;
}

bool planPortals(Ctx& c, Plan& p)
{
    const int t = toTable(c, p);
    if (t < 0) return false;
    const int n = std::max(2, c.i("Squares")), seed = c.i("RandomPatternSeed");
    const double maxT = clampd(c.d("MaxTransparency"), 0, 1), maxO = c.d("MaxOffset");
    const double bs = clampd(c.d("BorderSize"), 0, 1) * 0.25 * 0.3;
    Words q;
    q.d(0, c.progress * 1.6); q.d(2, maxT); q.d(4, maxO); q.d(6, bs);
    float bc[4];
    const Color col = c.color("BorderColor");
    put(bc, Color{col.r, col.g, col.b, 1.0});
    for (int j = 0; j < 4; ++j) q.f(8 + j, bc[j]);
    P::PortalsF pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h; pc.n = n; pc.seed = (uint32_t)seed;
    std::memcpy(&pc.q0, q.q, 12 * 4);
    Pass& ps = p.add("portals_f", 0, 1, p.w, p.h);
    ps.table = t;
    ps.setPush(pc);
    return true;
}

// the incoming frame as a table, then doubles after it (the kernels read them with ld_sf)
int toTableDoubles(Ctx& c, Plan& p, const std::vector<double>& ds)
{
    if (c.src2.px.size() != (size_t)p.w * p.h * 4) return -1;
    std::vector<float> t = std::move(c.src2.px);
    for (double d : ds) {
        uint64_t b;
        std::memcpy(&b, &d, 8);
        const uint32_t hl[2] = {(uint32_t)(b >> 32), (uint32_t)b};
        float f[2];
        std::memcpy(f, hl, 8);
        t.push_back(f[0]);
        t.push_back(f[1]);
    }
    return p.addTable(std::move(t));
}

bool planBlindsLike(Ctx& c, Plan& p, bool cascade)
{
    const int dir = c.i("Direction");
    const bool horizontal = dir == 1 || dir == 3, reverse = dir == 2 || dir == 3;
    const int n = std::max(1, c.i("Divisions"));
    const double stagger = cascade ? 0.6 : clampd(c.d("Stagger"), 0, 1) * 0.6;
    const double pr = c.progress, light = clampd(c.d("SpecularLight"), 0, 1);
    std::vector<double> ds;
    for (int idx = 0; idx < n; ++idx) {
        const double order = (double)(reverse ? n - 1 - idx : idx) / std::max(1, n - 1);
        const double t = clampd((pr - order * stagger) / std::max(0.05, 1 - stagger), 0, 1);
        const double ang = t * kPi;
        const double sc = std::cos(ang);
        ds.push_back(t);
        ds.push_back(std::fabs(sc));
        ds.push_back(light * 0.3 * std::sin(ang));
    }
    const int t = toTableDoubles(c, p, ds);
    if (t < 0) return false;
    P::Blinds3dF pc = {p.w, p.h, horizontal ? 1u : 0u, (uint32_t)n};
    Pass& ps = p.add("blinds3d_f", 0, 1, p.w, p.h);
    ps.table = t;
    ps.setPush(pc);
    return true;
}
bool planBlinds3d(Ctx& c, Plan& p) { return planBlindsLike(c, p, false); }
bool planCascade3d(Ctx& c, Plan& p) { return planBlindsLike(c, p, true); }

bool planFly3d(Ctx& c, Plan& p)
{
    const int t0 = toTable(c, p);
    if (t0 < 0) return false;
    const int W = p.w, H = p.h;
    const bool out = c.i("Direction") == 1;
    const double pr = c.progress;
    const double fx = c.d("FarXPosition"), fy = c.d("FarYPosition"), fz = c.d("FarZPosition");
    const double zr = c.d("ZRotations") * 2 * kPi;
    const double t = out ? pr : 1 - pr;
    const double s = std::max(0.02, std::pow(1.0 - t * 0.98, 1.0 + std::fabs(fz) * 0.02));
    const double ang = zr * t, ca = std::cos(ang), sa = std::sin(ang);
    const double ox = fx * 0.01 * W * t, oy = -fy * 0.01 * H * t;
    Words q;
    q.d(0, s); q.d(2, ca); q.d(4, sa); q.d(6, ox); q.d(8, oy);
    P::Fly3dF pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h; pc.out = out;
    std::memcpy(&pc.q0, q.q, 12 * 4);
    Pass& ps = p.add("fly3d_f", 0, 1, p.w, p.h);
    ps.table = t0;
    ps.setPush(pc);
    return true;
}

bool planIris(Ctx& c, Plan& p)
{
    const int W = p.w, H = p.h;
    if (W < 2 || H < 2) return false;
    const int t = toTable(c, p);
    if (t < 0) return false;
    const int shape = std::min(13, std::max(0, c.i("Shape")));
    const bool dirOn = c.i("Direction") != 0;
    double cxn, cyn;
    c.xy("Center", cxn, cyn);
    cxn = clampd(cxn, 0, 1); cyn = clampd(cyn, 0, 1);
    const int nx = std::min(10, std::max(1, c.i("RepeatX"))), ny = std::min(10, std::max(1, c.i("RepeatY")));
    const double feather = clampd(c.d("Feather"), 0, 1), border = clampd(c.d("Border"), 0, 1);
    const int cw = (int)((float)W / (float)nx + 0.5f), ch = (int)((float)H / (float)ny + 0.5f);
    if (cw < 1 || ch < 1) return false;                         // (the CPU code divides by these)
    const float cxf = (float)((double)cw * cxn), cyf = (float)((double)ch * cyn);
    const float sx = 1.f, sy = 1.f;
    const float A = ((float)ch * sy) / ((float)cw * sx);
    float F = feather < 0.01 ? ((float)nx * 0.01f) * (float)ny : (float)feather;
    F *= 0.05f;
    const float B = (float)border * 0.05f;
    float pf = (float)clampd(c.progress, 0, 1);
    const bool swapSrc = !dirOn;
    if (swapSrc) pf = 1.0f - pf;
    const uint8_t bB = (uint8_t)(int)((float)c.color("Color").b * 255.f + 0.5f), bR = (uint8_t)(int)((float)c.color("Color").r * 255.f + 0.5f);
    const uint8_t bG = (uint8_t)(int)(c.color("Color").g * 255.0 + 0.5);
    const bool hardF = F < 1e-6f, noBorder = B < 1e-6f;

    P::IrisFinal fin;
    std::memset(&fin, 0, sizeof(fin));
    fin.w = W; fin.h = H; fin.swap = swapSrc; fin.bc0 = bR; fin.bc1 = bG; fin.bc2 = bB; fin.p = pf;
    if (noBorder) {
        if (hardF) fin.mode = 0;
        else {
            const float m = pf - F;
            const double md = (double)m;
            const float lo = (float)(md >= 0.0 ? std::min((double)pf, md) : 0.0);
            const double hd = (double)(F + pf);
            const float hi = (float)((double)pf > hd ? (double)pf : std::min(1.0, hd));
            fin.mode = 1; fin.f0 = lo; fin.f1 = hi; fin.k1 = 1.0f / (hi - lo);
        }
    } else {
        const double pd = (double)pf;
        const double md = (double)(pf - B), hd = (double)(B + pf);
        const double lob = md >= 0.0 ? std::min(pd, md) : 0.0;
        const double hib = pd > hd ? pd : std::min(1.0, hd);
        if (hardF) {
            fin.mode = 2; fin.f0 = (float)lob; fin.f1 = (float)hib;
        } else {
            const double Fd = (double)F;
            const double l2 = lob - Fd, h2 = hib + Fd;
            const double ad = l2 >= 0.0 ? std::min(lob, l2) : 0.0;
            const double bd = hib > h2 ? hib : std::min(1.0, h2);
            const float bf = (float)bd, hif = (float)hib, lof = (float)lob, af = (float)ad;
            fin.mode = 3; fin.f0 = af; fin.f1 = lof; fin.f2 = hif; fin.f3 = bf;
            fin.k1 = 1.0f / (bf - hif); fin.k2 = 1.0f / (lof - af);
        }
    }
    const int M = p.addBuf(W, H + 1);
    P::IrisMap mp;
    std::memset(&mp, 0, sizeof(mp));
    mp.w = W; mp.h = H; mp.nx = nx; mp.ny = ny; mp.cw = cw; mp.ch = ch; mp.shape = shape; mp.window = transrec::irisWindow(shape); mp.cxf = cxf; mp.cyf = cyf; mp.a = A;
    p.add("iris_map", 0, M, W, H).setPush(mp);
    P::IrisMax mx = {(uint32_t)W, (uint32_t)H};
    p.add("iris_max", M, M, 1, 1).setPush(mx);
    Pass& ps = p.add("iris_final", 0, 1, W, H);
    ps.in1 = M;
    ps.table = t;
    ps.setPush(fin);
    return true;
}

// Star Wipe: the mask (8-bit pictures) and the wave position (float pictures) come from the CPU code (they use libm and depend on the settings but not on the
// progress), cached between the frames of a transition; the kernels do the blending.
struct StarCache {
    std::mutex m;
    std::vector<double> key;
    std::vector<float> mask;
    std::vector<double> field;
    float amp = 1.0f;
};

bool planStarWipe(Ctx& c, Plan& p)
{
    static StarCache cache;
    const int W = p.w, H = p.h;
    if (c.src2.px.size() != (size_t)W * H * 4) return false;
    double cxd, cyd;
    c.xy("Center", cxd, cyd);
    std::vector<double> key = {(double)W, (double)H, c.levels ? 1.0 : 0.0, (double)c.i("Arms"), (double)c.i("Cycles"), c.d("Ratio"), c.d("Angle"), cxd, cyd,
                               c.b("HorizontalFlip") ? 1.0 : 0.0, c.b("VerticalFlip") ? 1.0 : 0.0, c.b("HorizontalMirror") ? 1.0 : 0.0, c.b("VerticalMirror") ? 1.0 : 0.0,
                               (double)c.i("Waveform"), (double)c.i("Direction")};
    std::vector<float> tab = std::move(c.src2.px);
    std::lock_guard<std::mutex> lock(cache.m);
    if (cache.key != key) {
        cache.key.clear();
        if (c.levels) transrec::starMask(c, cache.mask, cache.amp);
        else transrec::starField(c, cache.field);
        cache.key = key;
    }
    P::Star8 s8;
    std::memset(&s8, 0, sizeof(s8));
    if (c.levels) {
        tab.insert(tab.end(), cache.mask.begin(), cache.mask.end());
        const float amp = cache.amp;
        const float pf = (float)c.progress;
        const float feather = (float)c.d("Feather"), border = (float)c.d("Border");
        uint32_t eb = 0x358637bd;
        float eps;
        std::memcpy(&eps, &eb, 4);
        const bool noBorder = eps > border, hard = eps > feather;
        const Color bcol = c.color("BorderColor");
        s8.w = W; s8.h = H; s8.p = pf;
        s8.bc0 = (uint32_t)(int)((float)bcol.r * 255.0f + 0.5f); s8.bc1 = (uint32_t)(int)((float)bcol.g * 255.0f + 0.5f); s8.bc2 = (uint32_t)(int)((float)bcol.b * 255.0f + 0.5f);
        if (noBorder && !hard) {
            float fw = amp * 0.125f;
            fw = fw * feather;
            const float lo = pf - fw, hi = fw + pf;
            s8.mode = 1; s8.lo = lo; s8.hi = hi; s8.inv = 1.0f / (hi - lo);
        } else if (!noBorder && hard) {
            float bw = amp * 0.125f;
            bw = bw * border;
            s8.mode = 2; s8.lo = pf - bw; s8.hi = bw + pf;
        } else if (!noBorder) {
            float q = amp * 0.125f;
            const float fw = q * feather, bw = q * border;
            const float a = pf - bw, b2 = bw + pf;
            s8.mode = 3;
            s8.x9 = a;
            s8.x10 = b2;
            s8.lo = (float)((double)a - (double)fw);
            s8.x11 = (float)((double)fw + (double)b2);
            s8.inv13 = 1.0f / (s8.x11 - s8.x10);
            s8.inv12 = 1.0f / (s8.x9 - s8.lo);
        } else {
            s8.mode = 0;
        }
        const int t = p.addTable(std::move(tab));
        Pass& ps = p.add("star8", 0, 1, W, H);
        ps.table = t;
        ps.setPush(s8);
        return true;
    }
    for (double d : cache.field) {
        uint64_t b;
        std::memcpy(&b, &d, 8);
        const uint32_t hl[2] = {(uint32_t)(b >> 32), (uint32_t)b};
        float f[2];
        std::memcpy(f, hl, 8);
        tab.push_back(f[0]);
        tab.push_back(f[1]);
    }
    const bool rev = c.i("Direction") == 1, tooth = c.i("Waveform") == 1;
    const double pr = clampd(c.progress, 0, 1);
    const double fv = clampd(c.d("Feather"), 0, 1), bv = clampd(c.d("Border"), 0, 1);
    const double wf = fv * 0.25 * (tooth ? 2.0 : 1.0), hw = bv * 0.125 * (tooth ? 2.0 : 1.0);
    Words q;
    q.d(0, rev ? 1.0 - pr : pr); q.d(2, wf); q.d(4, hw);
    float bc[4];
    const Color col = c.color("BorderColor");
    put(bc, Color{col.r, col.g, col.b, 1.0});
    for (int j = 0; j < 4; ++j) q.f(6 + j, bc[j]);
    P::StarF pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = W; pc.h = H; pc.rev = rev; pc.hasf = fv > 0; pc.hasw = hw > 0;
    std::memcpy(&pc.q0, q.q, 18 * 4);
    const int t = p.addTable(std::move(tab));
    Pass& ps = p.add("star_f", 0, 1, W, H);
    ps.table = t;
    ps.setPush(pc);
    return true;
}

// Flash: 8-bit pictures with the exact Deriche blur for the diffusions; float pictures only without diffusion (their FIR blur in doubles is too slow in
// kernels), then the light is the same on every pixel.
bool planFlash(Ctx& c, Plan& p)
{
    const int W = p.w, H = p.h;
    if (c.levels) {
        float pf = (float)c.progress;
        bool fromB = false;
        if (!(0.5f > pf)) { fromB = true; pf = 1.0f - pf; }
        const float a = pf + pf;
        const float a2 = a * a;
        const float om = 1.0f - a;
        const float S = 1.0f - om * om;
        const int dim = (int)std::max((float)W * 1.0f, (float)H * 1.0f);
        const double Sd = (double)S;
        const double sx = (c.d("HorizontalDiffusion") * 0.25 * (double)dim * Sd) / 1.0;
        const double sy = c.d("VerticalDiffusion") * 0.25 * (double)dim * Sd;
        const bool diffuse = sx > 0.0 || sy > 0.0;
        const int t = toTable(c, p);
        if (t < 0) return false;
        const Color tc = c.color("Tint");
        P::Flash8 pc;
        std::memset(&pc, 0, sizeof(pc));
        pc.w = W; pc.h = H; pc.fromb = fromB; pc.diff = diffuse;
        pc.base = (uint32_t)(int)(a2 * 255.0f);
        pc.dscale = (uint32_t)(int)(S * 16384.0f);
        pc.t0 = (uint32_t)(int)std::min((float)tc.r * 255.0f, 255.0f);
        pc.t1 = (uint32_t)(int)std::min((float)tc.g * 255.0f, 255.0f);
        pc.t2 = (uint32_t)(int)std::min((float)tc.b * 255.0f, 255.0f);
        int blurred = -1;
        if (diffuse) {
            int from = 0;
            if (fromB) {
                from = p.addBuf(W, H);
                P::TableToBuf tb = {(uint32_t)W, (uint32_t)H};
                Pass& cp = p.add("table_to_buf", 0, from, W, H);
                cp.table = t;
                cp.setPush(tb);
            }
            blurred = p.addBuf(W, H);
            gpuDericheBlur(p, from, blurred, vegasBlurSigma(sx), vegasBlurSigma(sy), true);
        }
        Pass& ps = p.add("flash8", 0, 1, W, H);
        ps.in1 = blurred;
        ps.table = t;
        ps.setPush(pc);
        return true;
    }
    const double pr = clampd(c.progress, 0, 1);
    const double lv = (1 - std::fabs(1 - 2 * pr)) * (1 - std::fabs(1 - 2 * pr));
    const double dh = clampd(c.d("HorizontalDiffusion"), 0, 1), dv = clampd(c.d("VerticalDiffusion"), 0, 1);
    if (dh > 0 || dv > 0) return false;
    const int t = toTable(c, p);
    if (t < 0) return false;
    double add = 255.0;
    add = (add * lv - 0.5) / 255.0;
    P::FlashF pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = W; pc.h = H; pc.fromb = pr < 0.5 ? 0 : 1; pc.add = (float)add;
    Pass& ps = p.add("flash_f", 0, 1, W, H);
    ps.table = t;
    ps.setPush(pc);
    return true;
}

std::vector<float> intsAsTable(const std::vector<int>& t, size_t extra = 0);
void drawOps(Plan& p, const std::vector<const transrec::Op*>& ops, int bufA, int bufB, int out);

// ---------------------------------------------------------------------------------------------------- Cross Effect (crossEffect() in builtins_trans.cpp)
int crossTrunc(float v) { return (int)v; }

bool planCrossEffect(Ctx& c, Plan& p)
{
    const int W = p.w, H = p.h;
    const int type = c.i("EffectType"), apply = c.i("ApplyTo");
    const float pf = std::min(1.0f, std::max(0.0f, (float)c.progress));
    float tf;
    if (c.d("FadeRange") < 0.001) tf = pf > 0.5f ? 1.0f : 0.0f;
    else {
        const float f = (float)c.d("FadeRange");
        tf = (pf - (1.0f - f) * 0.5f) / f;
        tf = tf < 0.0f ? 0.0f : tf > 1.0f ? 1.0f : tf;
    }
    const int t8 = crossTrunc(tf * 255.0f + 0.5f);
    const bool fxA = apply != 1, fxB = apply != 0;
    const int tb = toTable(c, p);
    if (tb < 0) return false;
    // both pictures as the bytes the CPU takes
    const int A8 = p.addBuf(W, H), B8 = p.addBuf(W, H);
    {
        P::CrossPrep pc = {(uint32_t)W, (uint32_t)H, 0u};
        p.add("cross_prep", 0, A8, W, H).setPush(pc);
        P::CrossPrep pd = {(uint32_t)W, (uint32_t)H, 1u};
        Pass& ps = p.add("cross_prep", 0, B8, W, H);
        ps.table = tb;
        ps.setPush(pd);
    }
    auto mix = [&](int a, int b, int out) {
        P::CrossMix pc = {(uint32_t)W, (uint32_t)H, t8};
        Pass& ps = p.add("cross_mix", a, out, W, H);
        ps.in1 = b;
        ps.setPush(pc);
    };
    if (type == 2) {
        float pw;
        if (apply == 0) pw = std::pow(pf, 0.25f);
        else if (apply == 1) pw = std::pow(1.0f - pf, 0.25f);
        else {
            float q = pf;
            if (!(0.5f >= q)) q = 1.0f - q;
            q += q;
            pw = std::pow(q, 0.25f);
        }
        const float mxv = (float)c.d("EffectSettingsMaxX"), myv = (float)c.d("EffectSettingsMaxY");
        const float asx = 1.0f, asy = 1.0f;
        const float fx = (1.0f - (mxv * 0.25f + 0.75f) * pw) * asx;
        const float fy = (1.0f - (myv * 0.25f + 0.75f) * pw) * asy;
        const int nx = fx > 0.999f ? W : crossTrunc(fx * (float)W * 0.5f + 1.0f);
        const int ny = fy > 0.999f ? H : crossTrunc(fy * (float)H * 0.5f + 1.0f);
        if (nx < 1 || ny < 1) return false;
        const float sx = (float)W / (float)nx, sy = (float)H / (float)ny;
        std::vector<int> y0s((size_t)ny), hhs((size_t)ny), rowsv((size_t)ny), x0s((size_t)nx), wws((size_t)nx), colsv((size_t)nx);
        std::vector<int> jmax((size_t)H, -1), imax((size_t)W, -1);
        for (int j = 0; j < ny; ++j) {
            const float j0 = (float)j * sy, j1 = (float)(j + 1) * sy;
            const int y0 = crossTrunc(j0);
            const int hh = j != ny - 1 ? crossTrunc(j1 - j0) : H - y0;
            const int rows = y0 + 1 + hh >= H ? hh : hh + 1;
            y0s[(size_t)j] = y0; hhs[(size_t)j] = hh; rowsv[(size_t)j] = rows;
            for (int y = y0; y < y0 + rows && y < H; ++y) if (y >= 0) jmax[(size_t)y] = j;
        }
        for (int i = 0; i < nx; ++i) {
            const float i0 = (float)i * sx, i1 = (float)(i + 1) * sx;
            const int x0 = crossTrunc(i0);
            const int ww = i != nx - 1 ? crossTrunc(i1 - i0) : W - x0;
            const int cols = x0 + 1 + ww >= W ? ww : ww + 1;
            x0s[(size_t)i] = x0; wws[(size_t)i] = ww; colsv[(size_t)i] = cols;
            for (int x = x0; x < x0 + cols && x < W; ++x) if (x >= 0) imax[(size_t)x] = i;
        }
        for (int j = 0; j < ny; ++j) if (y0s[(size_t)j] < 0 || y0s[(size_t)j] + std::max(hhs[(size_t)j], 0) > H) return false;
        for (int i = 0; i < nx; ++i) if (x0s[(size_t)i] < 0 || x0s[(size_t)i] + std::max(wws[(size_t)i], 0) > W) return false;
        std::vector<int> ct;
        for (int i = 0; i < nx; ++i) ct.push_back(x0s[(size_t)i]);
        for (int i = 0; i < nx; ++i) ct.push_back(wws[(size_t)i]);
        for (int j = 0; j < ny; ++j) ct.push_back(y0s[(size_t)j]);
        for (int j = 0; j < ny; ++j) ct.push_back(hhs[(size_t)j]);
        const int cells = p.addBuf(nx, 2 * ny);
        P::CrossCells cc = {(uint32_t)W, (uint32_t)H, (uint32_t)nx, (uint32_t)ny, fxA ? 1u : 0u, fxB ? 1u : 0u};
        Pass& pc = p.add("cross_cells", A8, cells, nx, ny);
        pc.in1 = B8;
        pc.table = p.addTable(intsAsTable(ct));
        pc.setPush(cc);
        std::vector<int> pt = jmax;
        pt.insert(pt.end(), imax.begin(), imax.end());
        P::CrossPix cp = {(uint32_t)W, (uint32_t)H, (uint32_t)nx, (uint32_t)ny, fxA ? 1u : 0u, fxB ? 1u : 0u, t8};
        Pass& pp = p.add("cross_pix", cells, 1, W, H);
        pp.in1 = fxA ? B8 : A8;
        pp.table = p.addTable(intsAsTable(pt));
        pp.setPush(cp);
        return true;
    }
    if (type == 3) {
        float q = pf;
        if (apply == 1) q = 1.0f - pf;
        else if (apply == 2) q = 0.5f < pf ? 1.0f - pf + (1.0f - pf) : pf + pf;
        float sz = (float)std::max(W, H);
        sz = sz * ((float)c.d("EffectSettingsMaxBlur") * q);
        sz = sz * 0.25f;
        const bool doBlur = sz > 0.0f;
        const double sg = vegasBlurSigma((double)sz);
        if (!doBlur) { mix(A8, B8, 1); return true; }
        if (apply == 0) {
            const int bl = p.addBuf(W, H);
            gpuDericheBlur(p, A8, bl, sg, sg, true);
            mix(bl, B8, 1);
        } else if (apply == 1) {
            const int bl = p.addBuf(W, H);
            gpuDericheBlur(p, B8, bl, sg, sg, true);
            mix(A8, bl, 1);
        } else {
            const int o = p.addBuf(W, H), bl = p.addBuf(W, H);
            mix(A8, B8, o);
            gpuDericheBlur(p, o, bl, sg, sg, true);
            P::CrossOut co = {(uint32_t)W, (uint32_t)H};
            p.add("cross_out", bl, 1, W, H).setPush(co);
        }
        return true;
    }
    if (type == 1) {
        transrec::Rec rec;
        if (!transrec::record("crosszoom", c, rec)) return false;
        int slot[2];
        for (int s = 0; s < 2; ++s) {
            if (rec.resDst[s] == 0) { slot[s] = rec.resTag[s] == 1 ? A8 : B8; continue; }
            std::vector<const transrec::Op*> ops;
            for (const transrec::Op& o : rec.ops) if (o.dst == rec.resDst[s]) ops.push_back(&o);
            slot[s] = p.addBuf(W, H);
            drawOps(p, ops, A8, B8, slot[s]);
        }
        mix(slot[0], slot[1], 1);
        return true;
    }
    mix(A8, B8, 1);
    return true;
}

// ---------------------------------------------------------------------------------------------------- Checkerboard generator
bool planCheckerboardExact(Ctx& c, Plan& p)
{
    const int W = p.w, H = p.h;
    std::vector<int> t8;
    if (c.levels && checkerboard8Gpu(c, W, H, t8)) {
        P::Checker8 pc = {p.w, p.h};
        Pass& ps = p.add("checker8", 0, 1, p.w, p.h);
        ps.table = p.addTable(intsAsTable(t8));
        ps.setPush(pc);
        return true;
    }
    std::vector<double> sx, sy;
    double pa[4], pb[4];
    checkerboardGpuD(c, W, H, sx, sy, pa, pb);
    std::vector<float> tab;
    auto putD = [&](double d) {
        uint64_t b;
        std::memcpy(&b, &d, 8);
        const uint32_t hl[2] = {(uint32_t)(b >> 32), (uint32_t)b};
        float f[2];
        std::memcpy(f, hl, 8);
        tab.push_back(f[0]);
        tab.push_back(f[1]);
    };
    for (double d : sx) putD(d);
    for (double d : sy) putD(d);
    Words q;
    for (int k = 0; k < 4; ++k) { q.d(2 * k, pa[k]); q.d(8 + 2 * k, pb[k]); }
    P::CheckerF pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h;
    std::memcpy(&pc.q0, q.q, 16 * 4);
    Pass& ps = p.add("checker_f", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(tab));
    ps.setPush(pc);
    return true;
}

bool planClockWipe(Ctx& c, Plan& p)
{
    if (c.levels) return planClockWipe8(c, p);
    const int t = toTable(c, p);
    if (t < 0) return false;
    const double F = clampd(c.d("FeatherAngle"), 0, 720);
    const double edge = -F * 0.5 + clampd(c.progress, 0, 1) * (360.0 + F);
    const double er = edge * kPi / 180.0;
    Words q;
    q.d(0, F); q.d(2, edge); q.d(4, -std::cos(er)); q.d(6, -std::sin(er)); q.d(8, std::sin(er)); q.d(10, -std::cos(er));
    P::WipeClockF pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h;
    pc.ccw = c.i("Direction") == 1 ? 1 : 0;
    pc.mode = F > 1e-9 ? 0 : edge <= 0 ? 1 : edge >= 360 ? 2 : 3;
    std::memcpy(&pc.q0, q.q, 12 * 4);
    Pass& ps = p.add("wipe_clock_f", 0, 1, p.w, p.h);
    ps.table = t;
    ps.setPush(pc);
    return true;
}


// ---------------------------------------------------------------------------------------------------- the blit / border transitions on 8-bit pictures
// The CPU function runs in record mode (transrec.h) and the calls it makes become passes of blit8 / border8.
const std::vector<float>& smoothTableFloats()
{
    static const std::vector<float> t = [] {
        std::vector<float> v(1024);
        for (int i = 0; i < 1024; ++i) {
            const float x = (float)i * 0.0009765625f;
            const float a = ((2.0f * x) * x) * x, b = (3.0f * x) * x;
            v[(size_t)i] = (float)(uint8_t)(int)((double)((b - a) * 255.0f) + 0.5);
        }
        return v;
    }();
    return t;
}

std::vector<float> intsAsTable(const std::vector<int>& t, size_t extra)
{
    std::vector<float> f(t.size() + extra, 0.f);
    std::memcpy(f.data(), t.data(), t.size() * 4);
    return f;
}

// The recorded drawing calls (ops) of one picture as passes ending in buffer 'out'. bufA / bufB hold the outgoing / incoming picture the ops copy from.
void drawOps(Plan& p, const std::vector<const transrec::Op*>& ops, int bufA, int bufB, int out)
{
    const int n = (int)ops.size();
    if (n == 0) {
        P::FillPx pc = {p.w, p.h, 0.f, 0.f, 0.f, 0.f};
        p.add("fill_px", 0, out, p.w, p.h).setPush(pc);
        return;
    }
    const int tmp = n > 1 ? p.addBuf(p.w, p.h) : -1;
    int prev = -1;
    for (int i = 0; i < n; ++i) {
        const transrec::Op& o = *ops[(size_t)i];
        const int to = ((n - 1 - i) % 2 == 0) ? out : tmp;
        P::Blit8 pc = {p.w, p.h, i == 0 ? 1u : 0u, i == n - 1 ? 1u : 0u};
        Pass& ps = p.add(o.kind == 0 ? "blit8" : "border8", o.kind == 0 ? (o.src == 1 ? bufA : bufB) : 0, to, p.w, p.h);
        ps.in1 = prev;
        if (o.kind == 0) ps.table = p.addTable(intsAsTable(o.t));
        else {
            std::vector<float> tab = intsAsTable(o.t, 33 + 1024 - o.t.size());
            std::copy(smoothTableFloats().begin(), smoothTableFloats().end(), tab.begin() + 33);
            ps.table = p.addTable(std::move(tab));
        }
        ps.setPush(pc);
        prev = to;
    }
}

bool planBlit8(Ctx& c, Plan& p, const char* shortId)
{
    transrec::Rec rec;
    if (!transrec::record(shortId, c, rec)) return false;
    const int tb = toTable(c, p);
    if (tb < 0) return false;
    bool needB = false;
    std::vector<const transrec::Op*> ops;
    for (const transrec::Op& o : rec.ops) {
        if (o.dst != rec.finalDst) return false;                 // (only the one picture is made so far)
        if (o.kind == 0 && o.src == 2) needB = true;
        ops.push_back(&o);
    }
    int bufB = -1;
    if (needB) {
        bufB = p.addBuf(p.w, p.h);
        P::TableToBuf pc = {p.w, p.h};
        Pass& ps = p.add("table_to_buf", 0, bufB, p.w, p.h);
        ps.table = tb;
        ps.setPush(pc);
    }
    drawOps(p, ops, 0, bufB, 1);
    return true;
}

// ---------------------------------------------------------------------------------------------------- the list, the test cases
// the float pictures of Push, Swap (and 3D Shuffle), Squeeze and Barn Door: the positions are worked out in doubles as the CPU code does
void borderWords(Ctx& c, Words& w)
{
    float bc[4];
    const Color col = c.color("BorderColor");
    put(bc, Color{col.r, col.g, col.b, 1.0});   // (colorPx: the colour straight, alpha 1)
    for (int k = 0; k < 4; ++k) w.f(14 + k, bc[k]);
}

bool planPushF(Ctx& c, Plan& p)
{
    const int t = toTable(c, p);
    if (t < 0) return false;
    const int dir = c.i("Direction");
    const bool horiz = dir >= 2;
    const double L = horiz ? p.w : p.h, pr = clampd(c.progress, 0, 1);
    const double w = clampd(c.d("BorderSize"), 0, 1) * L, bw = w * 0.5, F = clampd(c.d("BorderFeather"), 0, 1) * L;
    const double Wd = std::max(F, 2.0), R = bw + F * 0.5;
    const bool border = w > 0 || F > 0;
    const bool neg = dir == 0 || dir == 2;
    const double sb = (L + 2 * w) - pr * (L + 3 * w);
    const double seam = neg ? sb : L - sb;
    const double centre = neg ? seam - bw : seam + bw;
    const double shift = L - sb + w;
    Words q;
    q.d(0, seam); q.d(2, centre); q.d(4, shift); q.d(6, L); q.d(8, R); q.d(10, Wd);
    q.d(12, border && F <= 0 ? 1.0 : 0.0);
    borderWords(c, q);
    P::PushF pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h; pc.horiz = horiz; pc.neg = neg; pc.pushoff = c.b("PushOffPreviousImage"); pc.border = border;
    std::memcpy(&pc.q0, q.q, 18 * 4);
    Pass& ps = p.add("push_f", 0, 1, p.w, p.h);
    ps.table = t;
    ps.setPush(pc);
    return true;
}

bool planSwapF(Ctx& c, Plan& p)
{
    const int t = toTable(c, p);
    if (t < 0) return false;
    const int dir = c.i("Direction");
    const bool horiz = dir >= 2, flip = dir == 1 || dir == 3;
    const double L = horiz ? p.w : p.h, pr = clampd(c.progress, 0, 1);
    const double w = clampd(c.d("BorderSize"), 0, 1) * L, bw = w * 0.5, F = clampd(c.d("BorderFeather"), 0, 1) * L;
    const double Wd = std::max(F, 2.0), R = bw + F * 0.5;
    const bool border = w > 0 || F > 0;
    const double O = std::max(0.0, std::min(pr, 1 - pr) * (L + 3 * w) - w);
    const bool aTop = pr < 0.5;
    const double Oi = O - (F <= 0 && w > 0 ? 1.0 : 0.0);
    const double cen = aTop ? (L - O) + bw : O - bw;
    Words q;
    q.d(0, L); q.d(2, Oi); q.d(4, cen); q.d(6, R); q.d(8, Wd);
    borderWords(c, q);
    P::SwapF pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h; pc.horiz = horiz; pc.flip = flip; pc.atop = aTop; pc.border = border;
    std::memcpy(&pc.q0, q.q, 18 * 4);
    Pass& ps = p.add("swap_f", 0, 1, p.w, p.h);
    ps.table = t;
    ps.setPush(pc);
    return true;
}

bool planSqueezeF(Ctx& c, Plan& p)
{
    const int t = toTable(c, p);
    if (t < 0) return false;
    const int start = c.i("Start");
    const bool prev = c.b("SqueezePreviousImage");
    const bool alongY = start == 0 || start == 1 || start == 4;
    const bool mid = start >= 4, far = start == 1 || start == 3;
    const double L = alongY ? p.h : p.w, S = mid ? L * 0.5 : L, pr = clampd(c.progress, 0, 1);
    const double w = clampd(c.d("BorderSize"), 0, 1) * S, bw = w * 0.5, F = clampd(c.d("BorderFeather"), 0, 1) * S;
    const double Wd = std::max(F, 2.0), R = bw + F * 0.5;
    const bool border = w > 0 || F > 0;
    const double sB = pr * (S + 3 * w) - 2 * w;
    const double cen = sB + bw;
    const double aw = S - sB - w;
    Words q;
    q.d(0, L); q.d(2, w); q.d(4, sB); q.d(6, aw); q.d(8, cen); q.d(10, R); q.d(12, Wd);
    borderWords(c, q);
    P::SqueezeF pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h; pc.alongy = alongY; pc.mid = mid; pc.far = far; pc.prev = prev; pc.border = border;
    std::memcpy(&pc.q0, q.q, 18 * 4);
    Pass& ps = p.add("squeeze_f", 0, 1, p.w, p.h);
    ps.table = t;
    ps.setPush(pc);
    return true;
}

bool planBarndoorF(Ctx& c, Plan& p)
{
    const int t = toTable(c, p);
    if (t < 0) return false;
    const bool alongX = c.i("Orientation") == 0, in = c.i("Direction") == 0;
    const double L = alongX ? p.w : p.h, T = L * 0.5, pr = clampd(c.progress, 0, 1);
    const double w = clampd(c.d("BorderSize"), 0, 1) * T, bw = w * 0.5, F = clampd(c.d("BorderFeather"), 0, 1) * T;
    const double Wd = std::max(F, 2.0), R = bw + F * 0.5;
    const bool border = w > 0 || F > 0;
    const double cen = in ? -1.5 * w + pr * (T + 3 * w) : T + 1.5 * w - pr * (T + 3 * w);
    const double e = cen - bw;
    Words q;
    q.d(0, L); q.d(2, T); q.d(4, e); q.d(6, cen); q.d(8, R); q.d(10, Wd);
    borderWords(c, q);
    P::BarndoorF pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h; pc.along = alongX; pc.inward = in; pc.border = border; pc.onec = w > 0; pc.onef = w > 0 && F <= 0;
    std::memcpy(&pc.q0, q.q, 18 * 4);
    Pass& ps = p.add("barndoor_f", 0, 1, p.w, p.h);
    ps.table = t;
    ps.setPush(pc);
    return true;
}

bool planZoomF(Ctx& c, Plan& p)
{
    const int t = toTable(c, p);
    if (t < 0) return false;
    const bool out = c.i("Direction") == 1;
    double cx, cy;
    c.xy("Center", cx, cy);
    const int W = p.w, H = p.h;
    const double pr = clampd(c.progress, 0, 1);
    const double K = std::max(W, H);
    const double w = clampd(c.d("BorderSize"), 0, 1) * K, F = clampd(c.d("BorderFeather"), 0, 1) * K;
    const bool border = w > 0 || F > 0;
    const double q = out ? 1 - pr : pr;
    const double virt = q * (1.0 + 2.0 * clampd(c.d("BorderFeather"), 0, 1));
    const double sc = std::max(1e-4, std::min(1.0, virt));
    const double sp = F <= 0 && w > 0 && sc < 1.0 ? sc + 2.0 / W : sc;
    const double px0 = cx * W, py0 = (1 - cy) * H;
    const double x0 = px0 - sc * px0, x1 = px0 + sc * (W - px0), y0 = py0 - sc * py0, y1 = py0 + sc * (H - py0);
    const double Wd = std::max(F * sc, 2.0), R = (w * 0.5 + F * 0.5) * sc;
    const double cen = ((virt - sc) * W * 0.5 + w * 0.5) * sc;
    Words k;
    k.d(0, px0); k.d(2, py0); k.d(4, x0); k.d(6, x1); k.d(8, y0); k.d(10, y1); k.d(12, sp); k.d(14, Wd); k.d(16, R); k.d(18, cen);
    float bc[4];
    const Color col = c.color("BorderColor");
    put(bc, Color{col.r, col.g, col.b, 1.0});
    for (int j = 0; j < 4; ++j) k.f(20 + j, bc[j]);
    P::ZoomF pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h; pc.out = out; pc.border = border;
    std::memcpy(&pc.q0, k.q, 24 * 4);
    Pass& ps = p.add("zoom_f", 0, 1, p.w, p.h);
    ps.table = t;
    ps.setPush(pc);
    return true;
}

bool planPush(Ctx& c, Plan& p) { return c.levels ? planBlit8(c, p, "push") : planPushF(c, p); }
bool planSwap(Ctx& c, Plan& p) { return c.levels ? planBlit8(c, p, "swap") : planSwapF(c, p); }
bool planShuffle3d(Ctx& c, Plan& p) { return c.levels ? planBlit8(c, p, "3dshuffle") : planSwapF(c, p); }
bool planSqueeze(Ctx& c, Plan& p) { return c.levels ? planBlit8(c, p, "squeeze") : planSqueezeF(c, p); }
bool planZoom(Ctx& c, Plan& p) { return c.levels ? planBlit8(c, p, "zoom") : planZoomF(c, p); }
bool planBarndoor(Ctx& c, Plan& p) { return c.levels ? planBlit8(c, p, "barndoor") : planBarndoorF(c, p); }
bool planSlide(Ctx& c, Plan& p) { return planBlit8(c, p, "slide"); }   // (the CPU code has only the 8-bit drawing: float pictures go through it too)
bool planSpiral(Ctx& c, Plan& p) { return planBlit8(c, p, "spiral"); }   // (the CPU code has only the 8-bit drawing: float pictures go through it too)
bool planSplit(Ctx& c, Plan& p) { return planBlit8(c, p, "split"); }   // (the CPU code has only the 8-bit drawing: float pictures go through it too)

const ExtraGpuEntry kExtra[] = {
    {"push", planPush}, {"swap", planSwap}, {"3dshuffle", planShuffle3d}, {"squeeze", planSqueeze}, {"zoom", planZoom}, {"barndoor", planBarndoor},
    {"venetianblinds", planVenetian}, {"portals", planPortals}, {"iris", planIris}, {"checkerboard", planCheckerboardExact}, {"crosseffect", planCrossEffect}, {"flash", planFlash}, {"starwipe", planStarWipe}, {"3dblinds", planBlinds3d}, {"3dflyinout", planFly3d}, {"3dcascade", planCascade3d}, {"slide", planSlide}, {"spiral", planSpiral}, {"split", planSplit},
    {"dissolve", planDissolve},
    {"linearwipe", planLinearWipe},
    {"clockwipe", planClockWipe},
};

struct CaseSet { const char* id; std::vector<GpuCheckCase> cases; };

const std::vector<CaseSet>& caseSets()
{
    static const std::vector<CaseSet> sets = [] {
        std::vector<CaseSet> v;
        {
            CaseSet s{"dissolve", {}};
            for (int ty = 0; ty < 10; ++ty) {
                s.cases.push_back(GpuCheckCase("type" + std::to_string(ty) + " p0.3", PM{{"Type", {(double)ty}}, {"Transition", {0.3}}}));
                s.cases.push_back(GpuCheckCase("type" + std::to_string(ty) + " p0.8", PM{{"Type", {(double)ty}}, {"Transition", {0.8}}}));
            }
            s.cases.push_back(GpuCheckCase("bleed speeds", PM{{"Type", {3}}, {"Transition", {0.45}}, {"ColorBleedSpeedRed", {0.5}}, {"ColorBleedSpeedGreen", {0.2}}, {"ColorBleedSpeedBlue", {0.9}}, {"ColorBleedSpeedAlpha", {0.3}}}));
            s.cases.push_back(GpuCheckCase("morph speeds", PM{{"Type", {4}}, {"Transition", {0.45}}, {"ColorMorphSpeedRed", {0.5}}, {"ColorMorphSpeedGreen", {0.2}}, {"ColorMorphSpeedBlue", {0.9}}, {"ColorMorphSpeedAlpha", {0.3}}}));
            s.cases.push_back(GpuCheckCase("fade colour", PM{{"Type", {8}}, {"Transition", {0.3}}, {"FadeThroughColor", {0.8, 0.3, 0.1, 0.7}}}));
            s.cases.push_back(GpuCheckCase("fade colour late", PM{{"Type", {8}}, {"Transition", {0.9}}, {"FadeThroughColor", {0.8, 0.3, 0.1, 0.7}}}));
            v.push_back(s);
        }

        const std::vector<double> blue = {0.2, 0.4, 0.9, 1.0};
        {
            CaseSet s{"push", {}};
            for (int d = 0; d < 4; ++d)
                for (double pr : {0.0, 0.37, 0.9, 1.0}) s.cases.push_back(GpuCheckCase("dir" + std::to_string(d) + " p" + std::to_string(pr), PM{{"Direction", {(double)d}}, {"Transition", {pr}}}));
            s.cases.push_back(GpuCheckCase("border off", PM{{"Transition", {0.5}}, {"Direction", {3}}, {"BorderSize", {0.1}}, {"BorderFeather", {0.1}}, {"BorderColor", blue}, {"PushOffPreviousImage", {1}}}));
            s.cases.push_back(GpuCheckCase("hard border", PM{{"Transition", {0.4}}, {"Direction", {1}}, {"BorderSize", {0.07}}, {"BorderColor", blue}}));
            s.cases.push_back(GpuCheckCase("big feather", PM{{"Transition", {0.7}}, {"Direction", {2}}, {"BorderSize", {0.0}}, {"BorderFeather", {0.6}}, {"BorderColor", blue}, {"PushOffPreviousImage", {1}}}));
            v.push_back(s);
        }
        {
            CaseSet s{"swap", {}};
            for (int d = 0; d < 4; ++d)
                for (double pr : {0.2, 0.5, 0.8}) s.cases.push_back(GpuCheckCase("dir" + std::to_string(d) + " p" + std::to_string(pr), PM{{"Direction", {(double)d}}, {"Transition", {pr}}}));
            s.cases.push_back(GpuCheckCase("border", PM{{"Transition", {0.3}}, {"Direction", {1}}, {"BorderSize", {0.1}}, {"BorderFeather", {0.1}}, {"BorderColor", blue}}));
            s.cases.push_back(GpuCheckCase("border late", PM{{"Transition", {0.7}}, {"Direction", {2}}, {"BorderSize", {0.1}}, {"BorderFeather", {0.2}}, {"BorderColor", blue}}));
            s.cases.push_back(GpuCheckCase("hard border", PM{{"Transition", {0.4}}, {"Direction", {3}}, {"BorderSize", {0.05}}, {"BorderColor", blue}}));
            v.push_back(s);
            CaseSet s2 = s;
            s2.id = "3dshuffle";
            v.push_back(s2);
        }
        {
            CaseSet s{"squeeze", {}};
            for (int st = 0; st < 6; ++st)
                for (double pr : {0.25, 0.7}) for (int prev : {0, 1})
                    s.cases.push_back(GpuCheckCase("start" + std::to_string(st) + " p" + std::to_string(pr) + " prev" + std::to_string(prev), PM{{"Start", {(double)st}}, {"Transition", {pr}}, {"SqueezePreviousImage", {(double)prev}}}));
            s.cases.push_back(GpuCheckCase("border", PM{{"Transition", {0.4}}, {"Start", {1}}, {"SqueezePreviousImage", {1}}, {"BorderSize", {0.1}}, {"BorderFeather", {0.1}}, {"BorderColor", blue}}));
            s.cases.push_back(GpuCheckCase("border lateral", PM{{"Transition", {0.4}}, {"Start", {4}}, {"SqueezePreviousImage", {1}}, {"BorderSize", {0.1}}, {"BorderFeather", {0.15}}, {"BorderColor", blue}}));
            s.cases.push_back(GpuCheckCase("border vertical", PM{{"Transition", {0.8}}, {"Start", {5}}, {"BorderSize", {0.05}}, {"BorderColor", blue}}));
            v.push_back(s);
        }
        {
            CaseSet s{"zoom", {}};
            for (int d = 0; d < 2; ++d)
                for (double pr : {0.15, 0.5, 0.9}) s.cases.push_back(GpuCheckCase("dir" + std::to_string(d) + " p" + std::to_string(pr), PM{{"Direction", {(double)d}}, {"Transition", {pr}}}));
            s.cases.push_back(GpuCheckCase("off centre", PM{{"Transition", {0.4}}, {"Center", {0.3, 0.7}}}));
            s.cases.push_back(GpuCheckCase("border", PM{{"Transition", {0.4}}, {"BorderSize", {0.05}}, {"BorderFeather", {0.1}}, {"BorderColor", blue}}));
            s.cases.push_back(GpuCheckCase("border late", PM{{"Transition", {0.95}}, {"BorderSize", {0.05}}, {"BorderFeather", {0.1}}, {"BorderColor", blue}, {"Direction", {1}}}));
            v.push_back(s);
        }
        {
            CaseSet s{"venetianblinds", {}};
            for (double ang : {0.0, 45.0, 90.0, 190.0})
                for (double pr : {0.3, 0.8}) s.cases.push_back(GpuCheckCase("angle" + std::to_string((int)ang) + " p" + std::to_string(pr), PM{{"Angle", {ang}}, {"Transition", {pr}}, {"NumberOfBlinds", {7}}, {"Feather", {0.1}}}));
            s.cases.push_back(GpuCheckCase("one blind", PM{{"Transition", {0.5}}, {"NumberOfBlinds", {0}}, {"Feather", {0.0}}}));
            s.cases.push_back(GpuCheckCase("soft", PM{{"Transition", {0.6}}, {"Angle", {105}}, {"NumberOfBlinds", {15}}, {"Feather", {0.8}}}));
            v.push_back(s);
        }
        {
            CaseSet s{"portals", {}};
            for (double pr : {0.1, 0.4, 0.7, 0.95})
                s.cases.push_back(GpuCheckCase("p" + std::to_string(pr), PM{{"Transition", {pr}}, {"Squares", {5}}, {"RandomPatternSeed", {17}}}));
            s.cases.push_back(GpuCheckCase("big offset no border", PM{{"Transition", {0.5}}, {"MaxOffset", {1.0}}, {"MaxTransparency", {1.0}}, {"BorderSize", {0.0}}}));
            s.cases.push_back(GpuCheckCase("border", PM{{"Transition", {0.45}}, {"BorderSize", {1.0}}, {"BorderColor", blue}, {"Squares", {3}}, {"RandomPatternSeed", {166}}}));
            v.push_back(s);
        }
        {
            CaseSet s{"3dblinds", {}};
            for (int d = 0; d < 4; ++d)
                for (double pr : {0.15, 0.5, 0.85}) s.cases.push_back(GpuCheckCase("dir" + std::to_string(d) + " p" + std::to_string(pr), PM{{"Direction", {(double)d}}, {"Transition", {pr}}, {"Stagger", {0.4}}}));
            s.cases.push_back(GpuCheckCase("one division", PM{{"Transition", {0.5}}, {"Divisions", {0}}}));
            s.cases.push_back(GpuCheckCase("no light", PM{{"Transition", {0.3}}, {"SpecularLight", {0.0}}, {"Divisions", {16}}}));
            v.push_back(s);
            CaseSet s2{"3dcascade", {}};
            for (int d = 0; d < 4; ++d)
                for (double pr : {0.15, 0.5, 0.85}) s2.cases.push_back(GpuCheckCase("dir" + std::to_string(d) + " p" + std::to_string(pr), PM{{"Direction", {(double)d}}, {"Transition", {pr}}}));
            s2.cases.push_back(GpuCheckCase("many", PM{{"Transition", {0.6}}, {"Divisions", {20}}, {"SpecularLight", {0.5}}}));
            v.push_back(s2);
        }
        {
            CaseSet s{"3dflyinout", {}};
            for (int d = 0; d < 2; ++d)
                for (double pr : {0.1, 0.5, 0.9}) s.cases.push_back(GpuCheckCase("dir" + std::to_string(d) + " p" + std::to_string(pr), PM{{"Direction", {(double)d}}, {"Transition", {pr}}}));
            s.cases.push_back(GpuCheckCase("spins", PM{{"Transition", {0.4}}, {"ZRotations", {0.7}}, {"FarXPosition", {0.3}}, {"FarYPosition", {0.9}}, {"FarZPosition", {16}}}));
            s.cases.push_back(GpuCheckCase("near", PM{{"Transition", {0.6}}, {"FarZPosition", {0}}, {"ZRotations", {0}}}));
            v.push_back(s);
        }
        {
            CaseSet s{"iris", {}};
            for (int sh = 0; sh < 14; ++sh)
                for (int d = 0; d < 2; ++d)
                    s.cases.push_back(GpuCheckCase("shape" + std::to_string(sh) + " dir" + std::to_string(d), PM{{"Shape", {(double)sh}}, {"Direction", {(double)d}}, {"Transition", {0.45}}, {"Feather", {0.3}}, {"Border", {0.2}}, {"Color", {0.9, 0.2, 0.4, 1.0}}}));
            for (int sh : {0, 3, 6, 10, 11, 12, 13}) {
                s.cases.push_back(GpuCheckCase("hard shape" + std::to_string(sh), PM{{"Shape", {(double)sh}}, {"Transition", {0.6}}, {"Feather", {0.0}}, {"Border", {0.0}}}));
                s.cases.push_back(GpuCheckCase("hard border shape" + std::to_string(sh), PM{{"Shape", {(double)sh}}, {"Transition", {0.35}}, {"Feather", {0.0}}, {"Border", {0.3}}}));
                s.cases.push_back(GpuCheckCase("soft shape" + std::to_string(sh), PM{{"Shape", {(double)sh}}, {"Transition", {0.7}}, {"Feather", {0.5}}, {"Border", {0.0}}, {"Center", {0.3, 0.6}}}));
                s.cases.push_back(GpuCheckCase("repeat shape" + std::to_string(sh), PM{{"Shape", {(double)sh}}, {"Transition", {0.5}}, {"Feather", {0.2}}, {"Border", {0.1}}, {"RepeatX", {3}}, {"RepeatY", {2}}, {"Center", {0.7, 0.2}}}));
            }
            s.cases.push_back(GpuCheckCase("one column", PM{{"Shape", {11}}, {"Transition", {0.5}}, {"RepeatX", {1}}, {"RepeatY", {4}}, {"Center", {0.0, 0.0}}}));
            s.cases.push_back(GpuCheckCase("many", PM{{"Shape", {1}}, {"Transition", {0.5}}, {"RepeatX", {10}}, {"RepeatY", {10}}, {"Feather", {0.1}}, {"Border", {0.1}}}));
            v.push_back(s);
        }
        {
            CaseSet s{"starwipe", {}};
            for (double pr : {0.1, 0.5, 0.9}) {
                s.cases.push_back(GpuCheckCase("plain p" + std::to_string(pr), PM{{"Transition", {pr}}, {"Feather", {0.0}}, {"Border", {0.0}}}));
                s.cases.push_back(GpuCheckCase("soft p" + std::to_string(pr), PM{{"Transition", {pr}}, {"Feather", {0.4}}, {"Border", {0.0}}}));
                s.cases.push_back(GpuCheckCase("hard border p" + std::to_string(pr), PM{{"Transition", {pr}}, {"Feather", {0.0}}, {"Border", {0.3}}, {"BorderColor", blue}}));
                s.cases.push_back(GpuCheckCase("soft border p" + std::to_string(pr), PM{{"Transition", {pr}}, {"Feather", {0.3}}, {"Border", {0.2}}, {"BorderColor", blue}}));
            }
            s.cases.push_back(GpuCheckCase("tooth reverse", PM{{"Transition", {0.45}}, {"Waveform", {1}}, {"Direction", {1}}, {"Cycles", {3}}, {"Feather", {0.2}}, {"Border", {0.1}}, {"BorderColor", blue}}));
            s.cases.push_back(GpuCheckCase("arms", PM{{"Transition", {0.5}}, {"Arms", {8}}, {"Ratio", {0.7}}, {"Angle", {40}}, {"Center", {0.3, 0.6}}, {"Feather", {0.1}}}));
            s.cases.push_back(GpuCheckCase("flips", PM{{"Transition", {0.5}}, {"HorizontalFlip", {1}}, {"VerticalFlip", {1}}, {"Angle", {75}}, {"Center", {0.3, 0.2}}, {"Feather", {0.1}}}));
            s.cases.push_back(GpuCheckCase("flip h", PM{{"Transition", {0.5}}, {"HorizontalFlip", {1}}, {"Feather", {0.1}}}));
            s.cases.push_back(GpuCheckCase("flip v", PM{{"Transition", {0.5}}, {"VerticalFlip", {1}}, {"Center", {0.8, 0.5}}, {"Feather", {0.1}}}));
            s.cases.push_back(GpuCheckCase("mirror both", PM{{"Transition", {0.5}}, {"HorizontalMirror", {1}}, {"VerticalMirror", {1}}, {"Feather", {0.2}}, {"Border", {0.1}}}));
            s.cases.push_back(GpuCheckCase("mirror h", PM{{"Transition", {0.5}}, {"HorizontalMirror", {1}}, {"Cycles", {2}}, {"Feather", {0.2}}}));
            s.cases.push_back(GpuCheckCase("mirror v tooth", PM{{"Transition", {0.5}}, {"VerticalMirror", {1}}, {"Waveform", {1}}, {"Feather", {0.2}}, {"Center", {0.5, 0.3}}}));
            s.cases.push_back(GpuCheckCase("mirror centred", PM{{"Transition", {0.6}}, {"HorizontalMirror", {1}}, {"VerticalMirror", {1}}, {"Center", {0.5, 0.5}}, {"Arms", {4}}, {"Feather", {0.2}}}));
            s.cases.push_back(GpuCheckCase("off picture", PM{{"Transition", {0.5}}, {"Center", {0.0, 0.0}}, {"Ratio", {1.0}}, {"Arms", {32}}, {"Feather", {0.2}}}));
            v.push_back(s);
        }
        {
            CaseSet s{"flash", {}};
            for (double pr : {0.05, 0.3, 0.5, 0.52, 0.8, 0.97}) s.cases.push_back(GpuCheckCase("p" + std::to_string(pr), PM{{"Transition", {pr}}}));
            s.cases.push_back(GpuCheckCase("tint", PM{{"Transition", {0.4}}, {"Tint", {0.9, 0.5, 0.1}}}));
            s.cases.push_back(GpuCheckCase("bright tint", PM{{"Transition", {0.7}}, {"Tint", {1.0, 1.0, 1.0}}}));
            s.cases.push_back(GpuCheckCase("diffusion (float on the CPU)", PM{{"Transition", {0.4}}, {"HorizontalDiffusion", {0.3}}, {"VerticalDiffusion", {0.1}}}));
            s.cases.push_back(GpuCheckCase("diffusion late (float on the CPU)", PM{{"Transition", {0.8}}, {"HorizontalDiffusion", {0.0}}, {"VerticalDiffusion", {0.5}}, {"Tint", {0.9, 0.5, 0.1}}}));
            v.push_back(s);
        }
        {
            CaseSet s{"crosseffect", {}};
            for (double pr : {0.0, 0.2, 0.5, 0.8, 1.0}) s.cases.push_back(GpuCheckCase("none p" + std::to_string(pr), PM{{"EffectType", {0}}, {"Transition", {pr}}}));
            s.cases.push_back(GpuCheckCase("narrow fade", PM{{"EffectType", {0}}, {"Transition", {0.45}}, {"FadeRange", {0.3}}}));
            s.cases.push_back(GpuCheckCase("hard cut", PM{{"EffectType", {0}}, {"Transition", {0.7}}, {"FadeRange", {0.0}}}));
            for (int ap = 0; ap < 3; ++ap)
                for (double pr : {0.15, 0.5, 0.9}) {
                    s.cases.push_back(GpuCheckCase("zoom apply" + std::to_string(ap) + " p" + std::to_string(pr), PM{{"EffectType", {1}}, {"ApplyTo", {(double)ap}}, {"Transition", {pr}}, {"EffectSettingsSource", {0.3, 0.6}}, {"EffectSettingsDestination", {0.7, 0.4}}}));
                    s.cases.push_back(GpuCheckCase("pixelate apply" + std::to_string(ap) + " p" + std::to_string(pr), PM{{"EffectType", {2}}, {"ApplyTo", {(double)ap}}, {"Transition", {pr}}, {"EffectSettingsMaxX", {0.8}}, {"EffectSettingsMaxY", {0.6}}}));
                    s.cases.push_back(GpuCheckCase("blur apply" + std::to_string(ap) + " p" + std::to_string(pr), PM{{"EffectType", {3}}, {"ApplyTo", {(double)ap}}, {"Transition", {pr}}, {"EffectSettingsMaxBlur", {0.5}}}));
                }
            s.cases.push_back(GpuCheckCase("big zoom", PM{{"EffectType", {1}}, {"Transition", {0.6}}, {"EffectSettingsMaxZoom", {40}}}));
            s.cases.push_back(GpuCheckCase("no blur", PM{{"EffectType", {3}}, {"Transition", {0.5}}, {"EffectSettingsMaxBlur", {0.0}}}));
            v.push_back(s);
        }
        {
            CaseSet s{"checkerboard", {}};
            const std::vector<double> red = {0.9, 0.1, 0.2, 1.0}, green = {0.1, 0.8, 0.3, 0.6};
            s.cases.push_back(GpuCheckCase("colours", PM{{"Color1", red}, {"Color2", green}}));
            s.cases.push_back(GpuCheckCase("small tiles", PM{{"TileDimensionsWidth", {0.05}}, {"TileDimensionsHeight", {0.08}}, {"TileDimensionsSquare", {0}}, {"Color1", red}, {"Color2", green}}));
            s.cases.push_back(GpuCheckCase("square", PM{{"TileDimensionsWidth", {0.13}}, {"TileDimensionsSquare", {1}}, {"Color1", red}, {"Color2", green}}));
            s.cases.push_back(GpuCheckCase("soft", PM{{"EdgeBlendingHorizontal", {0.3}}, {"EdgeBlendingVertical", {0.5}}, {"GridOffsetX", {0.13}}, {"TileDimensionsWidth", {0.07}}, {"TileDimensionsHeight", {0.11}}, {"TileDimensionsSquare", {0}}, {"Color1", red}, {"Color2", green}}));
            s.cases.push_back(GpuCheckCase("proportional", PM{{"EdgeBlendingProportional", {1}}, {"EdgeBlendingHorizontal", {0.6}}, {"GridOffsetX", {0.4}}, {"GridOffsetY", {0.27}}, {"TileDimensionsSquare", {0}}, {"Color1", red}, {"Color2", green}}));
            s.cases.push_back(GpuCheckCase("offset", PM{{"GridOffsetX", {-0.37}}, {"GridOffsetY", {1.21}}, {"TileDimensionsWidth", {0.2}}, {"TileDimensionsHeight", {0.3}}, {"TileDimensionsSquare", {0}}}));
            s.cases.push_back(GpuCheckCase("one pixel tiles", PM{{"TileDimensionsWidth", {0.001}}, {"TileDimensionsHeight", {0.001}}, {"TileDimensionsSquare", {0}}, {"Color1", red}, {"Color2", green}}));
            s.cases.push_back(GpuCheckCase("huge tiles", PM{{"TileDimensionsWidth", {2.0}}, {"TileDimensionsHeight", {3.0}}, {"TileDimensionsSquare", {0}}, {"EdgeBlendingHorizontal", {1.0}}, {"EdgeBlendingVertical", {1.0}}}));
            v.push_back(s);
        }
        {
            CaseSet s{"barndoor", {}};
            for (int o = 0; o < 2; ++o)
                for (int d = 0; d < 2; ++d)
                    for (double pr : {0.2, 0.6}) s.cases.push_back(GpuCheckCase("o" + std::to_string(o) + " d" + std::to_string(d) + " p" + std::to_string(pr), PM{{"Orientation", {(double)o}}, {"Direction", {(double)d}}, {"Transition", {pr}}}));
            s.cases.push_back(GpuCheckCase("border", PM{{"Transition", {0.4}}, {"BorderSize", {0.1}}, {"BorderFeather", {0.1}}, {"BorderColor", blue}}));
            s.cases.push_back(GpuCheckCase("border out", PM{{"Transition", {0.7}}, {"Direction", {1}}, {"Orientation", {1}}, {"BorderSize", {0.05}}, {"BorderFeather", {0.3}}, {"BorderColor", blue}}));
            v.push_back(s);
        }
        {
            CaseSet s{"slide", {}};
            for (double ang : {0.0, 45.0, 135.0, 200.0, 300.0})
                for (int d = 0; d < 2; ++d) s.cases.push_back(GpuCheckCase("a" + std::to_string((int)ang) + " d" + std::to_string(d), PM{{"Angle", {ang}}, {"Direction", {(double)d}}, {"Transition", {0.45}}}));
            s.cases.push_back(GpuCheckCase("border", PM{{"Transition", {0.3}}, {"BorderSize", {0.05}}, {"BorderFeather", {0.1}}, {"BorderColor", blue}}));
            s.cases.push_back(GpuCheckCase("border late", PM{{"Transition", {0.9}}, {"Angle", {90}}, {"BorderSize", {0.05}}, {"BorderFeather", {0.1}}, {"BorderColor", blue}}));
            v.push_back(s);
        }
        {
            CaseSet s{"spiral", {}};
            for (int o = 0; o < 4; ++o)
                for (int m = 0; m < 2; ++m) s.cases.push_back(GpuCheckCase("o" + std::to_string(o) + " m" + std::to_string(m), PM{{"Orientation", {(double)o}}, {"Motion", {(double)m}}, {"Transition", {0.4}}}));
            s.cases.push_back(GpuCheckCase("out", PM{{"Direction", {1}}, {"Turns", {2.5}}, {"Zoom", {30}}, {"Transition", {0.6}}}));
            s.cases.push_back(GpuCheckCase("border", PM{{"Transition", {0.3}}, {"BorderSize", {0.05}}, {"BorderFeather", {0.1}}, {"BorderColor", blue}}));
            s.cases.push_back(GpuCheckCase("border late", PM{{"Transition", {0.95}}, {"BorderSize", {0.05}}, {"BorderFeather", {0.1}}, {"BorderColor", blue}}));
            v.push_back(s);
        }
        {
            CaseSet s{"split", {}};
            for (int m = 0; m < 3; ++m)
                for (int d = 0; d < 2; ++d)
                    for (double pr : {0.2, 0.7}) s.cases.push_back(GpuCheckCase("mode" + std::to_string(m) + " d" + std::to_string(d) + " p" + std::to_string(pr), PM{{"SplitMode", {(double)m}}, {"Direction", {(double)d}}, {"Transition", {pr}}}));
            s.cases.push_back(GpuCheckCase("border", PM{{"Transition", {0.4}}, {"BorderSize", {0.05}}, {"BorderFeather", {0.1}}, {"BorderColor", blue}, {"Center", {0.3, 0.6}}}));
            s.cases.push_back(GpuCheckCase("border squeeze", PM{{"Transition", {0.4}}, {"SplitMode", {2}}, {"BorderSize", {0.05}}, {"BorderFeather", {0.1}}, {"BorderColor", blue}}));
            v.push_back(s);
        }
        return v;
    }();
    return sets;
}

}  // namespace

const std::vector<ExtraGpuEntry>& extraGpuEntries()
{
    static const std::vector<ExtraGpuEntry> v(std::begin(kExtra), std::end(kExtra));
    return v;
}

void extraGpuCases(const std::string& id, std::vector<GpuCheckCase>& out)
{
    for (const CaseSet& s : caseSets())
        if (id == fullId(s.id)) out.insert(out.end(), s.cases.begin(), s.cases.end());
}

// Float tolerance: 0 unless the CPU result depends on libm (listed here with the id).
bool extraGpuFloatTolerance(const std::string& id, double& tol)
{
    static const struct { const char* id; double tol; } known[] = {
        {"dissolve", 0.0},
        {"linearwipe", 0.0},
        {"clockwipe", 0.0},
        {"push", 0.0},
        {"swap", 0.0},
        {"3dshuffle", 0.0},
        {"squeeze", 0.0},
        {"barndoor", 0.0},
        {"zoom", 0.0},
        {"venetianblinds", 0.0},
        {"portals", 0.0},
        {"iris", 0.0},
        {"checkerboard", 0.0},
        {"crosseffect", 0.0},
        {"flash", 0.0},
        {"starwipe", 0.0},
        {"3dblinds", 0.0},
        {"3dflyinout", 0.0},
        {"3dcascade", 0.0},
    };
    for (const auto& k : known)
        if (id == fullId(k.id)) { tol = k.tol; return true; }
    return false;
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
