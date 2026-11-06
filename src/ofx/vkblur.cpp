// Vulkan plans of the blur and light effects that follow VEGAS's OpenCL kernels; see vkblur.h.
#include "../core/rmath.h"
#include "vkfft.h"
#include "vkblur.h"
#include "vkfx2.h"
#include "builtins_more_internal.h"
#include "vfxblurs.h"
#include "vfxlinblur.h"
#include "vfxpolar.h"
#include "vfxfilllight.h"
#include "vfxflare.h"
#include "vfxdenoise.h"
#include "vfxnews.h"
#include "vfxcookie.h"
#include <cstdio>
#include "builtins_deriche.h"
#include "builtins_cgblur.h"
#include "vkfilters.h"
#include "vkshaders/push_layouts.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {

namespace {
typedef gpu::Plan Plan;
typedef gpu::Pass Pass;
namespace P = gpu::push;
// a per-pixel pass over two pictures
Pass& addPix(Plan& p, const char* shader, int in0, int in1, int out)
{
    Pass& ps = p.add(shader, in0, out, p.w, p.h);
    ps.in1 = in1;
    return ps;
}
}  // namespace

// ---------------------------------------------------------------------------------------------------- the recursive Gaussian (vfxgauss.h / vfxblurs.h)
void gpuAddGaussPass(Plan& p, int from, int to, const vfxgauss::Coefsf& cf, bool vert, unsigned flags, bool fused, uint32_t w, uint32_t h)
{
    if (!w) w = p.w;
    if (!h) h = p.h;
    P::VgLine pc;
    std::memset(&pc, 0, sizeof pc);
    pc.w = w; pc.h = h; pc.vert = vert ? 1u : 0u; pc.q8 = flags;
    float* dst = &pc.n0;
    for (int k = 0; k < 5; ++k) { dst[k] = cf.n[k]; dst[5 + k] = cf.d[k]; dst[10 + k] = cf.m[k]; dst[15 + k] = cf.bn[k]; dst[20 + k] = cf.bm[k]; }
    p.add(fused ? "vg_line" : "vg_line_mad", from, to, vert ? w : h, 1).setPush(pc);
}

void gpuAddGaussHalves(Plan& p, int from, int to, const vfxgauss::Coefsf& cx, const vfxgauss::Coefsf& cy, bool q8)
{
    const int mid = p.addBuf(p.w, p.h, 4);
    gpuAddGaussPass(p, from, mid, cy, true, q8 ? 1u : 0u, true);
    gpuAddGaussPass(p, mid, to, cx, false, q8 ? 1u : 0u, true);
}

// Radial Blur as VEGAS's OpenCL kernels draw it (radialBlurOpenCl in builtins_more_blur.cpp)
// the CPU's own std::sin / std::cos of each column's angle and std::atan2 of each pixel's (vx, vy) (glibc's sinf, cosf and atan2f are not always the correctly rounded
// values, so the kernels read the table instead of computing them); the atan2 table is in the GPU's row order
static void polarTables(int W, int H, float v56x, float v56y, std::vector<float>& sc, std::vector<float>& at)
{
    const float pi = 3.14159274f;
    sc.resize((size_t)W * 2);
    for (int x = 0; x < W; ++x) {
        const float xn = (float)x / (float)W, a = (xn / 0.5f - 1.f) * pi;
        sc[(size_t)x * 2] = rm::sin(a);
        sc[(size_t)x * 2 + 1] = rm::cos(a);
    }
    at.resize((size_t)W * H);
    for (int gy = 0; gy < H; ++gy) {
        const float yn = (float)(H - 1 - gy) / (float)H, vy = (yn / 0.5f - 1.f) - v56y;
        for (int x = 0; x < W; ++x) {
            const float xn = (float)x / (float)W, vx = (xn / 0.5f - 1.f) - v56x;
            at[(size_t)gy * W + x] = rm::atan2(vx, vy);
        }
    }
}

static void addPolar(Plan& p, int in, int polar, uint32_t q8polar, float v56x, float v56y, int& wrapTab)
{
    std::vector<float> sc, at;
    polarTables((int)p.w, (int)p.h, v56x, v56y, sc, at);
    P::RboPolar pp = {p.w, p.h, q8polar, 0u, v56x, v56y};
    Pass& a = p.add("rbo_polar", in, polar, p.w, p.h);
    a.table = p.addTable(std::move(sc));
    a.setPush(pp);
    wrapTab = p.addTable(std::move(at));
}

bool planRadialBlurOcl(Ctx& c, Plan& p, const RbSetup& g)
{
    more::RbOclParams rp;
    more::radialBlurOclParams(c, (int)p.h, rp);
    const bool q = c.host8;
    const int polar = p.addBuf(p.w, p.h, 4), blurred = p.addBuf(p.w, p.h, 4);
    int wtab;
    addPolar(p, 0, polar, q ? 1u : 0u, rp.v56x, rp.v56y, wtab);
    if (g.type == 2) {
        gpuAddGaussPass(p, polar, blurred, rp.cf, true, q ? 3u : 0u, false);
    } else {
        P::RboRec pr = {p.w, p.h, (uint32_t)g.type, q ? 1u : 0u, rp.s0, rp.s1, rp.w, 0u};
        p.add("rbo_rec", polar, blurred, p.w, 1).setPush(pr);
    }
    P::RboWrap pw = {p.w, p.h, q ? 1u : 0u, 0u, rp.v56x, rp.v56y};
    Pass& pwp = p.add("rbo_wrap", blurred, 1, p.w, p.h);
    pwp.table = wtab;
    pwp.setPush(pw);
    return true;
}

// Radial Pixelate as VEGAS's OpenCL kernels draw it (radialPixelate in builtins_more_creative.cpp, vfxpolar.h)
bool planRadialPixelateOcl(Ctx& c, Plan& p)
{
    double pcx, pcy;
    c.xy("Center", pcx, pcy);
    const double st = c.d("Strength");
    if (p.w < 1 || p.h < 1 || !(st > 0.0)) {
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    int nx, ny;
    more::vfxpolar::radialPixelateGroups((int)p.w, (int)p.h, st, nx, ny);
    const float cx = (float)(pcx * 2.0 - 1.0), cy = (float)(pcy * 2.0 - 1.0);
    const int type = c.i("Type");
    const float fw = (float)p.w, fh = (float)p.h;
    const bool q = c.host8;
    const int polar = p.addBuf(p.w, p.h, 4), grouped = p.addBuf(p.w, p.h, 4);
    int wtab;
    addPolar(p, 0, polar, q ? 2u : 0u, cx, cy, wtab);
    const float g = type == 1 ? fh / (float)ny : fw / (float)nx;
    P::RpoGroups pg = {p.w, p.h, type == 1 ? 1u : 0u, q ? 1u : 0u, g, 0u};
    p.add("rpo_groups", polar, grouped, type == 1 ? p.w : p.h, 1).setPush(pg);
    P::RboWrap pw = {p.w, p.h, 0u, 0u, cx, cy};
    Pass& pwp = p.add("rbo_wrap", grouped, 1, p.w, p.h);
    pwp.table = wtab;
    pwp.setPush(pw);
    return true;
}

// Layer Dimensionality as VEGAS's OpenCL kernels draw it (vfxlayerdim.h render, step for step): one pass per kernel, the blurs are the cookie feather's fused recursion
bool planLayerDimensionalityOcl(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    const vfxld::Settings s = more::ldOclSettings(c);
    const uint32_t q8 = c.host8 ? 1u : 0u;
    const float pairX = 1.0f > s.pixelAspect ? s.pixelAspect : 1.0f, pairY = 1.0f > s.pixelAspect ? 1.0f : 1.0f / s.pixelAspect;
    auto nb = [&]() { return p.addBuf(p.w, p.h, 4); };
    auto alphaOf = [&](int in) { const int o = nb(); p.add("ldo_alpha", in, o, p.w, p.h).setPush(P::LdoAlpha{p.w, p.h}); return o; };
    // blurAlpha: the alpha plane through the recursion (columns with the sigma of y, then rows with x), then the lanes written back (colour: nullptr = the blurred alpha in all)
    auto blur = [&](int in, float amount, const float* col) {
        float sx, sy;
        vfxld::blurSigmas(W, H, amount, pairX, pairY, sx, sy);
        const int mid = nb(), fin = nb(), o = nb();
        gpuAddGaussPass(p, in, mid, vfxgauss::toFloats(vfxgauss::build((double)sy)), true, 0u, true);
        gpuAddGaussPass(p, mid, fin, vfxgauss::toFloats(vfxgauss::build((double)sx)), false, 0u, true);
        p.add("ldo_fix", fin, o, p.w, p.h).setPush(P::LdoFix{p.w, p.h, col ? 1u : 0u, col ? col[0] : 0.f, col ? col[1] : 0.f, col ? col[2] : 0.f});
        return o;
    };
    auto glowK = [&](int bl, int fr, bool outer, const float* col) {
        const int o = nb();
        Pass& ps = p.add("ldo_glow", bl, o, p.w, p.h);
        ps.in1 = fr;
        ps.setPush(P::LdoGlow{p.w, p.h, outer ? 1u : 0u, 0u, col[0], col[1], col[2], 0u});
        return o;
    };
    auto addK = [&](int a, int b) { const int o = nb(); Pass& ps = p.add("ldo_add", a, o, p.w, p.h); ps.in1 = b; ps.setPush(P::LdoAdd{p.w, p.h}); return o; };
    auto maskK = [&](int alphaIn, int colIn, bool inv, float amount) {
        const int o = nb();
        Pass& ps = p.add("ldo_mask", alphaIn, o, p.w, p.h);
        ps.in1 = colIn;
        ps.setPush(P::LdoMask{p.w, p.h, inv ? 1u : 0u, amount});
        return o;
    };
    auto overK = [&](int top, int bottom, float scale, int mode, int to, uint32_t q) {
        const int o = to >= 0 ? to : nb();
        Pass& ps = p.add("ldo_over", top, o, p.w, p.h);
        ps.in1 = bottom;
        ps.setPush(P::LdoOver{p.w, p.h, q, 0u, mode > 0 ? scale : 1.0f, mode > 0 ? 1.0f : scale});
        return o;
    };
    const float zero3[3] = {0.f, 0.f, 0.f};
    // glow
    int G = -1;
    if (s.glowType >= 1 && s.glowType <= 4) {
        const float c1[3] = {s.col1[0], s.col1[1], s.col1[2]};
        const float* c2 = s.glowType == 3 ? c1 : s.col2;
        if (s.glowType == 1) {
            G = glowK(blur(alphaOf(0), s.glowAmount, nullptr), 0, true, c1);
        } else if (s.glowType == 2) {
            G = glowK(blur(alphaOf(0), s.glowAmount, nullptr), 0, false, c1);
        } else {
            const int D = blur(alphaOf(0), s.glowAmount, nullptr);
            const int A = glowK(D, 0, false, c1);
            const int B = glowK(D, 0, true, c2);
            G = addK(A, B);
        }
    }
    // emboss
    int E = -1;
    const bool fgOn = std::fabs(s.fg) > 9.999999747378752e-05f, bgOn = std::fabs(s.bg) > 9.999999747378752e-05f;
    if (fgOn || bgOn) {
        const int D = blur(alphaOf(0), s.roundness, nullptr);
        auto light = [&](float amt) {
            const vfxld::LightArgs a = vfxld::lightArgs(s, W, H, amt);
            const float flat = vfxld::flatLight(a);
            const int o = nb();
            p.add("ldo_light", D, o, p.w, p.h).setPush(P::LdoLight{p.w, p.h, a.v85, a.v86, a.v87, a.v88, a.v89, a.v90, a.L[0], a.L[1], a.L[2], a.L[3], flat, 0u});
            return o;
        };
        int Efg = -1, Ebg = -1;
        if (fgOn) Efg = maskK(0, light(s.fg), false, std::fabs(s.fg));
        if (bgOn) Ebg = maskK(0, light(s.bg), true, std::fabs(s.bg));
        E = (fgOn && bgOn) ? addK(Efg, Ebg) : (fgOn ? Efg : Ebg);
    }
    // glow over emboss
    int X = -1;
    if (G >= 0 && E >= 0) X = overK(G, E, 1.0f, 0, -1, 0u);
    else if (G >= 0) X = G;
    else if (E >= 0) X = E;
    // shadow
    int Sh = -1;
    if (s.shadowType == 1 || s.shadowType == 2) {
        float x = s.light[0], y = s.light[1], z = s.light[2];
        x -= 0.0f; y -= 0.0f; z -= 0.0f;
        const float len = std::sqrt(x * x + y * y + z * z);
        float nx, ny, nz;
        if (len > 1.0000000116860974e-07f) { const float inv = 1.0f / len; nx = x * inv; ny = y * inv; nz = z * inv; }
        else { nx = 0.f; ny = 0.f; nz = 1.0f; }
        float k8 = 0.f, k7 = 0.f;
        if ((float)std::fabs((double)nz) > 9.999999747378752e-05f) {
            k7 = s.height * 0.1f;
            k7 /= nz;
            k8 = k7 * nx;
            k7 = k7 * ny;
        }
        const float shx = (float)W * k8, shy = (float)H * k7;
        auto fill = [&](bool inv, float amount) {
            const int o = nb();
            p.add("ldo_fill", 0, o, p.w, p.h).setPush(P::LdoFill{p.w, p.h, inv ? 1u : 0u, 0u, 0.f, 0.f, 0.f, amount});
            return o;
        };
        auto shift = [&](int in) { const int o = nb(); p.add("ldo_shift", in, o, p.w, p.h).setPush(P::LdoShift{p.w, p.h, shx, shy}); return o; };
        if (s.shadowType == 1) Sh = blur(shift(fill(false, s.amount)), s.blur, zero3);
        else Sh = maskK(0, blur(shift(fill(true, 1.0f)), s.roundness, zero3), false, std::fabs(s.amount));
    }
    // the final composite
    int top = 0;
    float fscale = s.opacity;
    if (X >= 0) {
        top = overK(X, 0, s.opacity, 0, -1, 0u);
        fscale = 1.0f;
    }
    if (Sh >= 0) {
        if (s.shadowType == 1) overK(top, Sh, fscale, 1, 1, q8);
        else overK(Sh, top, fscale, 0, 1, q8);
    } else {
        p.add("ldo_scale", top, 1, p.w, p.h).setPush(P::LdoScale{p.w, p.h, q8, 0u, fscale, 0u});
    }
    return true;
}

// Lens Flare as VEGAS's OpenCL kernels draw it (lensFlareOpenCl in builtins_lensflare.cpp, vfxflare.h): the elements' boxes and colours from the CPU's own prepare(), one kernel
// walks the elements for every pixel
bool planLensFlareOcl(Ctx& c, Plan& p)
{
    more::vfxflare::Params P;
    const more::vfxflare::Row* rows;
    int nrows;
    float ray[64];
    if (!more::lensFlareOclSetup(c, P, rows, nrows, ray)) {
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    std::vector<more::vfxflare::Elem> els;
    more::vfxflare::prepare(els, (int)p.w, (int)p.h, rows, nrows, P);
    const more::vfxflare::Hex hex;
    std::vector<float> tab(88 + els.size() * 16 + 16, 0.f);
    for (int i = 0; i < 64; ++i) tab[(size_t)i] = ray[i];
    for (int i = 0; i < 6; ++i) {
        tab[64 + (size_t)i] = hex.vx[i];
        tab[70 + (size_t)i] = hex.vy[i];
        tab[76 + (size_t)i] = hex.n0[i];
        tab[82 + (size_t)i] = hex.n1[i];
    }
    for (size_t k = 0; k < els.size(); ++k) {
        float* e = &tab[88 + k * 16];
        const more::vfxflare::Elem& el = els[k];
        e[0] = (float)el.type; e[1] = el.strength; e[2] = el.ox; e[3] = el.oy; e[4] = el.R; e[5] = el.Rx;
        e[6] = (float)el.xa; e[7] = (float)el.xb; e[8] = (float)el.ya; e[9] = (float)el.yb;
        e[10] = el.col[0]; e[11] = el.col[1]; e[12] = el.col[2];
    }
    const bool q = c.host8 && nrows > 0;
    P::LfoDraw pc = {p.w, p.h, (uint32_t)els.size(), q ? 1u : 0u, P.blend, 1.0f - P.blend};
    Pass& ps = p.add("lfo_draw", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(tab));
    ps.setPush(pc);
    return true;
}

// Fill Light as VEGAS's OpenCL kernels draw it (fillLightOpenCl in builtins_velvet.cpp, vfxfilllight.h)
bool planFillLightOcl(Ctx& c, Plan& p)
{
    filllightk::Settings s;
    s.hue = c.d("Hue");
    s.strength = c.d("Strength");
    s.bias = c.d("Bias");
    s.tint = c.d("Tint");
    s.softness = c.d("Softness");
    filllightk::Prep pr;
    filllightk::prepare((int)p.w, (int)p.h, s, pr);
    if (pr.copy) {
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    std::vector<float> tab(256 * 4);
    for (int i = 0; i < 256; ++i)
        for (int k = 0; k < 4; ++k) tab[(size_t)i * 4 + k] = (float)pr.lut[i][k];
    int x = p.addBuf(p.w, p.h, 4);
    Pass& pm = p.add("flo_map", 0, x, p.w, p.h);
    pm.table = p.addTable(std::move(tab));
    pm.setPush(P::FloMap{p.w, p.h});
    if (pr.blur) {
        const int blurred = p.addBuf(p.w, p.h, 4), mid = p.addBuf(p.w, p.h, 4);
        const unsigned q = c.host8 ? 1u : 0u;
        gpuAddGaussPass(p, x, mid, pr.cy, true, q, false);
        gpuAddGaussPass(p, mid, blurred, pr.cx, false, q, false);
        x = blurred;
    }
    P::FloFinal pf = {p.w, p.h, pr.k, c.host8 ? 1u : 0u};
    addPix(p, "flo_final", 0, x, 1).setPush(pf);
    return true;
}

// Newsprint as VEGAS's OpenCL kernels draw it (newsprintOpenCl in builtins_newsprint.cpp, vfxnews.h)
bool planNewsprintOcl(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    if (W <= 0 || H <= 0) return false;
    double dot = clampd(c.d("DotSize"), 0, 1);
    if (!(dot == dot)) dot = 0;
    const int type = std::max(0, std::min(2, c.i("Type")));
    const bool q8 = c.host8;
    vfxnews::Opts o;
    const vfxnews::Host h = vfxnews::host(W, H, dot, 1.0, 1.0, 1.0, o);
    const int mid = p.addBuf(p.w, p.h, 4), blurred = p.addBuf(p.w, p.h, 4);
    gpuAddGaussPass(p, 0, mid, vfxgauss::toFloats(vfxgauss::build((double)h.sig1)), true, q8 ? 1u : 0u, false);
    gpuAddGaussPass(p, mid, blurred, vfxgauss::toFloats(vfxgauss::build((double)h.sig2)), false, q8 ? 3u : 0u, false);
    const float* ct = vfxnews::coverTable();
    std::vector<float> tab(ct, ct + 256);
    const bool fade = dot < 0.15;
    P::NwoDraw pd = {p.w, p.h, (uint32_t)type, q8 ? 1u : 0u, (uint32_t)h.par, fade ? 1u : 0u, h.v41x, h.v41y, h.v42x, h.v42y, h.v45x, h.v45y, fade ? 1.0f - (float)dot / 0.15f : 0.f, 0u};
    Pass& ps = addPix(p, "nwo_draw", 0, blurred, 1);
    ps.table = p.addTable(std::move(tab));
    ps.setPush(pd);
    return true;
}

// Cookie Cutter as VEGAS's OpenCL kernels draw it (cookieOpenCl in builtins_cookie.cpp, vfxcookie.h)
bool planCookieCutterOcl(Ctx& c, Plan& p)
{
    if (p.w == 0 || p.h == 0) return false;
    CookieSetup su;
    cookieSetup(c, (int)p.w, (int)p.h, su);
    const int mask = p.addBuf(p.w, p.h, 4);
    P::CookieMask0 pm = {p.w, p.h, (uint32_t)su.repX, (uint32_t)su.repY, (uint32_t)su.cw, (uint32_t)su.ch, su.noBorder ? 1u : 0u, 0u,
                         su.cxCell, su.cyCell, su.ratio, su.tIn, su.tOut, su.rIn, su.hx, su.hy, su.ix0, su.iy0, su.ovalW, su.ovalH, su.boxHi, su.boxLo, su.nTout};
    char name[32];
    if (su.shape == 0) std::snprintf(name, sizeof name, "cookie_mask0k");
    else std::snprintf(name, sizeof name, "cookie_mask%d", su.shape);
    p.add(name, 0, mask, p.w, p.h).setPush(pm);
    int m = mask;
    if (!su.noFeather) {
        float sigx, sigy;
        vfxcookie::featherSigmas(su.fw2, 1.0f, 1.0f, sigx, sigy);
        const int mid = p.addBuf(p.w, p.h, 4);
        m = p.addBuf(p.w, p.h, 4);
        gpuAddGaussPass(p, mask, mid, vfxgauss::toFloats(vfxgauss::build((double)sigy)), true, 0u, true);
        gpuAddGaussPass(p, mid, m, vfxgauss::toFloats(vfxgauss::build((double)sigx)), false, 0u, true);
    }
    P::CkoApply pa = {p.w, p.h, su.noBorder ? 1u : 0u, su.cutSection ? 1u : 0u, c.host8 ? 1u : 0u, 0u, su.br, su.bg, su.bb, 0u};
    addPix(p, "cko_apply", 0, m, 1).setPush(pa);
    return true;
}

// Linear Blur as VEGAS's OpenCL kernels draw it (vegasLinearBlurOpenCl in builtins_deriche.cpp, vfxlinblur.h)
bool planLinearBlurOcl(Ctx& c, Plan& p)
{
    double amount = c.d("Amount");
    const double angle = vegasLinearBlurOclAngle(c.d("Angle"), amount);
    const vfxlinblur::Setup su = vfxlinblur::prepare((int)p.w, (int)p.h, angle, amount);
    const unsigned q = c.host8 ? 1u : 0u;
    if (su.state < 0) return false;
    if (su.state == 1) {
        P::OclFinal pf = {p.w, p.h, q ? 1u : 2u, 0u};
        p.add("ocl_final", 0, 1, p.w, p.h).setPush(pf);
        return true;
    }
    const uint32_t Wc = (uint32_t)su.g.Wc, Hc = (uint32_t)su.g.Hc;
    const int canvas = p.addBuf(Wc, Hc, 4), blurred = p.addBuf(Wc, Hc, 4);
    P::LboTurn pin = {Wc, Hc, p.w, p.h, q, 0u, su.s1, su.c1, su.cx, su.cy, -su.offx, -su.offy};
    p.add("lbo_turn", 0, canvas, Wc, Hc).setPush(pin);
    gpuAddGaussPass(p, canvas, blurred, su.cf, true, q ? 3u : 0u, false, Wc, Hc);
    P::LboTurn pout = {p.w, p.h, Wc, Hc, q ? 1u : 2u, 0u, su.s2, su.c2, su.cx2, su.cy2, su.offx, su.offy};
    p.add("lbo_turn", blurred, 1, p.w, p.h).setPush(pout);
    return true;
}

// vfxblurs::gaussianBlur (gaussianBlurFx in builtins_filters.cpp)
bool planGaussianBlurOcl(Ctx& c, Plan& p)
{
    const vfxblurs::GaussSetup gs = vfxblurs::gaussianSetup((int)p.w, (int)p.h, more::oclSetting(c.d("HorizontalRange"), 0, 1), more::oclSetting(c.d("VerticalRange"), 0, 1));
    const int blurred = p.addBuf(p.w, p.h, 4);
    gpuAddGaussHalves(p, 0, blurred, gs.cx, gs.cy, c.host8);
    P::VgGauss pc = {p.w, p.h, (c.b("R") ? 1u : 0u) | (c.b("G") ? 2u : 0u) | (c.b("B") ? 4u : 0u) | (c.b("A") ? 8u : 0u), c.host8 ? 1u : 0u};
    addPix(p, "vg_gauss", 0, blurred, 1).setPush(pc);
    return true;
}

// vfxblurs::unsharpMask (unsharpMask in builtins_more_blur.cpp)
bool planUnsharpMaskOcl(Ctx& c, Plan& p)
{
    const vfxblurs::GaussSetup gs = vfxblurs::unsharpSetup((int)p.w, (int)p.h, more::oclSetting(c.d("Radius"), 0, 1));
    const int blurred = p.addBuf(p.w, p.h, 4);
    gpuAddGaussHalves(p, 0, blurred, gs.cx, gs.cy, c.host8);
    P::VgUnsharp pc = {p.w, p.h, (float)more::oclSetting(c.d("Amount"), -1e6, 1e6), (float)more::oclSetting(c.d("Threshold"), -1e6, 1e6), c.host8 ? 1u : 0u, 0u};
    addPix(p, "vg_unsharp", 0, blurred, 1).setPush(pc);
    return true;
}

// vfxblurs::glow (glow in builtins_filters.cpp)
bool planGlowOcl(Ctx& c, Plan& p)
{
    const vfxblurs::GaussSetup gs = vfxblurs::glowSetup((int)p.w, (int)p.h, more::oclSetting(c.d("GlowPercent"), 0, 1));
    const Color col = c.color("Color");
    const int blurred = p.addBuf(p.w, p.h, 4);
    gpuAddGaussHalves(p, 0, blurred, gs.cx, gs.cy, c.host8);
    P::VgGlow pc = {p.w, p.h, (float)more::oclSetting(c.d("Suppression"), -1e6, 1e6), (float)more::oclSetting(c.d("Intensity"), -1e6, 1e6),
                    (float)more::oclSetting(col.r, -1e6, 1e6), (float)more::oclSetting(col.g, -1e6, 1e6), (float)more::oclSetting(col.b, -1e6, 1e6), c.host8 ? 1u : 0u};
    addPix(p, "vg_glow", 0, blurred, 1).setPush(pc);
    return true;
}

// vfxblurs::chromaBlur (chromaBlur in builtins_more_blur.cpp)
bool planChromaBlurOcl(Ctx& c, Plan& p)
{
    const vfxblurs::GaussSetup gs = vfxblurs::chromaSetup(more::oclSetting(c.d("HorizontalPixels"), 0, 10), more::oclSetting(c.d("VerticalPixels"), 0, 10));
    const int yuv = p.addBuf(p.w, p.h, 4), blurred = p.addBuf(p.w, p.h, 4);
    P::VgYuv py = {p.w, p.h, c.host8 ? 1u : 0u, 0u};
    p.add("vg_yuv", 0, yuv, p.w, p.h).setPush(py);
    gpuAddGaussHalves(p, yuv, blurred, gs.cx, gs.cy, c.host8);
    P::VgChromaOut po = {p.w, p.h, c.host8 ? 1u : 0u, 0u};
    addPix(p, "vg_chroma_out", yuv, blurred, 1).setPush(po);
    return true;
}

// ---------------------------------------------------------------------------------------------------- the table
namespace {
typedef std::map<std::string, std::vector<double>> PM;

bool planLinearBlur(Ctx& c, Plan& p) { return c.openCl && planLinearBlurOcl(c, p); }     // (without OpenCL: VEGAS's CPU shear model, on the CPU)

// Denoise as VEGAS's OpenCL kernels draw it (denoisingNLM in builtins_analysis_nr.cpp, vfxdenoise.h): YUV bytes, the padded planes, the whole-number non-local means one plane at a time,
// back to RGB, the sharpening. The weights and the sampler's border rows and columns (texel) come from the CPU's own code.
bool planDenoise(Ctx& c, Plan& p)
{
    if (!c.openCl) return false;       // (the older model of the plug-in stays on the CPU)
    const double lum = std::min(100.0, std::max(0.0, c.d("Luminance"))), col = std::min(100.0, std::max(0.0, c.d("Color"))), amt = std::min(1.0, std::max(0.0, c.d("Amount")));
    const double eps = 1.1920929e-07;
    const int W = (int)p.w, H = (int)p.h;
    const uint32_t h8 = c.host8 ? 1u : 0u;
    int work;
    if (lum <= eps && col <= eps) {
        work = p.addBuf(p.w, p.h, 4);
        p.add("dn_yuv", 0, work, p.w, p.h).setPush(P::DnYuv{p.w, p.h, h8, 1u});
    } else {
        const int B = 13, EW = W + 2 * B, EH = H + 2 * B;
        const int yb = p.addBuf(p.w, p.h, 4);
        p.add("dn_yuv", 0, yb, p.w, p.h).setPush(P::DnYuv{p.w, p.h, h8, 0u});
        std::vector<float> mxy((size_t)EW + EH);
        for (int i = 0; i < EW; ++i) mxy[(size_t)i] = (float)vfxdn::texel(i - B, W);
        for (int j = 0; j < EH; ++j) mxy[(size_t)EW + j] = (float)vfxdn::texel(j - B, H);
        const int pad = p.addBuf((uint32_t)EW, (uint32_t)EH, 4);
        Pass& pp = p.add("dn_pad", yb, pad, (uint32_t)EW, (uint32_t)EH);
        pp.table = p.addTable(std::move(mxy));
        pp.setPush(P::DnPad{(uint32_t)EW, (uint32_t)EH, p.w, p.h});
        const double hs[3] = {lum, col, col};
        int prev = yb;
        for (int k = 0; k < 3; ++k) {
            const bool on = hs[k] > eps;
            int tab[196] = {0};
            if (on) vfxdn::weightTable(hs[k], tab);
            std::vector<float> wt(196);
            for (int i = 0; i < 196; ++i) wt[(size_t)i] = (float)tab[i];
            const int r = p.addBuf(p.w, p.h, 4);
            const int wtab = p.addTable(std::move(wt));
            int acc = -1;
            const int step = 3;                                     // search rows 3 at a time: a card cuts a long dispatch short
            for (int d0 = 0; d0 < 21; d0 += step) {
                const int a2 = p.addBuf(p.w, p.h, 4);
                Pass& pn = p.add("dn_nlm", pad, a2, p.w, p.h);
                pn.in1 = acc >= 0 ? acc : pad;
                pn.table = wtab;
                pn.setPush(P::DnNlm{p.w, p.h, (uint32_t)k, on ? 1u : 0u, (uint32_t)EW, (uint32_t)d0, (uint32_t)std::min(21, d0 + step), 0u});
                acc = a2;
                if (!on) break;
            }
            Pass& pf = p.add("dn_nlmfin", acc, r, p.w, p.h);
            pf.in1 = prev;
            pf.setPush(P::DnNlmfin{p.w, p.h, (uint32_t)k, 0u});
            prev = r;
        }
        work = p.addBuf(p.w, p.h, 4);
        p.add("dn_merge", prev, work, p.w, p.h).setPush(P::DnMerge{p.w, p.h, h8, 0u});
    }
    const float a = (float)amt;
    if (amt > eps && a >= 0.001f) {
        float c0 = 29.0f;
        if (a >= 0.2500f) c0 = 29.0f - a * 16.0f;
        else if (a >= 0.1250f) c0 = (-152.0f) * a + 63.0f;
        else if (a >= 0.0630f) c0 = (-269.230769f) * a + 88.961538f;
        else c0 = (-451.612903f) * a + 100.451613f;
        p.add("dn_sharp", work, 1, p.w, p.h).setPush(P::DnSharp{p.w, p.h, h8, 0u, c0, c0 - 12.0f});
    } else {
        p.add("copy_px", work, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
    }
    return true;
}

// Black Bar Fill as Reno's float path draws it (blackBarFill in builtins_more_creative.cpp: the OpenCL recursive Gaussian of builtins_cgblur.cpp on the background and on the shadow's alpha)
bool planBlackBarFill(Ctx& c, Plan& p)
{
    if (c.levels) return false;
    const int W = (int)p.w, H = (int)p.h;
    Bbf8Setup su;
    more::bbf8Setup(c, W, H, su);
    const int st = p.addBuf(p.w, p.h, 4), bg = p.addBuf(p.w, p.h, 4), fg = p.addBuf(p.w, p.h, 4), pl = p.addBuf(p.w, p.h, 4);
    p.add("bbo_straight", 0, st, p.w, p.h).setPush(P::BboStraight{p.w, p.h});
    auto rd = [&](uint32_t mode, const float* m, int to) {
        P::BboRead pc = {p.w, p.h, mode, 0u, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]};
        p.add("bbo_read", st, to, p.w, p.h).setPush(pc);
    };
    rd(0u, su.mBg, bg);
    rd(1u, su.mFg, fg);
    rd(2u, su.mFg, pl);
    // cgOclBlur: the rows, then the columns, with the same coefficients (the sigma is the size in pixels / 6)
    auto oclBlur = [&](int from, double size) {
        const OclBlurCoefs cf = oclBlurCoefs(oclBlurSigma(size / 6.0));
        const int b = p.addBuf(p.w, p.h, 4), b2 = p.addBuf(p.w, p.h, 4);
        auto oc = [&](uint32_t vert) {
            return P::OclBlur{p.w, p.h, vert, (float)cf.n[0], (float)cf.n[1], (float)cf.n[2], (float)cf.d[1], (float)cf.d[2], (float)cf.d[3], (float)cf.t[1], (float)cf.t[2], (float)cf.t[3], (float)cf.gc, (float)cf.ga};
        };
        p.add("ocl_blur", from, b, p.h, 1).setPush(oc(0u));
        p.add("ocl_blur", b, b2, p.w, 1).setPush(oc(1u));
        return b2;
    };
    const int bgb = oclBlur(bg, su.bsize), plb = oclBlur(pl, su.shadowSize);
    const int layer = p.addBuf(p.w, p.h, 4);
    Pass& pa = p.add("bbo_la", fg, layer, p.w, p.h);
    pa.in1 = plb;
    pa.setPush(P::BboLa{p.w, p.h});
    Pass& pf = p.add("bbo_final", layer, 1, p.w, p.h);
    pf.in1 = bgb;
    pf.setPush(P::BboFinal{p.w, p.h, c.host8 ? 1u : 0u, 0u});
    return true;
}

std::vector<FilterGpuEntry> blurEntries()
{
    std::vector<FilterGpuEntry> v;
    typedef std::map<std::string, std::vector<double>> PM;
    v.push_back({"linearblur", planLinearBlur, 0.0, {}});
    v.push_back({"blackbarfill", planBlackBarFill, 0.0,
                 {GpuCheckCase("defaults", PM{}), GpuCheckCase("no blur", PM{{"Foreground Shadow Blur", {0.0}}, {"Background Blur", {0.0}}}),
                  GpuCheckCase("scaled", PM{{"Foreground Scale", {0.6}}, {"Foreground Location", {0.3, 0.4}}, {"Background Scale", {1.5}}, {"Background Location", {0.6, 0.45}}, {"Foreground Shadow Blur", {0.5}}, {"Background Blur", {0.3}}}),
                  GpuCheckCase("big", PM{{"Foreground Scale", {1.8}}, {"Foreground Location", {0.7, 0.8}}, {"Background Blur", {1.0}}, {"Foreground Shadow Blur", {1.0}}}),
                  GpuCheckCase("small", PM{{"Foreground Scale", {0.1}}, {"Background Scale", {10}}, {"Background Blur", {0.02}}})}});
    v.push_back({"de.magix:DenoisingNLM", planDenoise, 0.0,
                 {GpuCheckCase("default", PM{}), GpuCheckCase("strong", PM{{"Luminance", {40}}, {"Color", {12}}, {"Amount", {0.3}}}), GpuCheckCase("luminance only", PM{{"Luminance", {8}}, {"Color", {0}}}),
                  GpuCheckCase("color only", PM{{"Luminance", {0}}, {"Color", {20}}, {"Amount", {0.15}}}), GpuCheckCase("sharpen only", PM{{"Luminance", {0}}, {"Color", {0}}, {"Amount", {0.09}}}),
                  GpuCheckCase("soft sharpen", PM{{"Luminance", {2}}, {"Color", {2}}, {"Amount", {0.03}}}), GpuCheckCase("off", PM{{"Luminance", {0}}, {"Color", {0}}, {"Amount", {0}}})}});
    return v;
}
}  // namespace

const std::vector<FilterGpuEntry>& filterGpuEntries()
{
    static const std::vector<FilterGpuEntry> all = [] {
        std::vector<FilterGpuEntry> v = filterGpuEntriesBase();
        for (const FilterGpuEntry& e : blurEntries()) v.push_back(e);
        for (const FilterGpuEntry& e : fx2GpuEntries()) v.push_back(e);
        for (const FilterGpuEntry& e : fftGpuEntries()) v.push_back(e);
        return v;
    }();
    return all;
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
