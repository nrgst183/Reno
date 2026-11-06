// The transitions as VEGAS draws them with its OpenCL kernels, on the GPU: for each, the host-side struct of the CPU port (vfxwipes.h, ...) works out the numbers exactly as the CPU
// does and the kernel (tools/gpukernels_oclt.py) does the per-pixel part float for float. In an 8-bit frame the result is cut with the card's 12-bit store inside the kernel
// (the 'q8' word), as the CPU port does with more::oclStore12.
#include "../core/rmath.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "vktransocl.h"
#include "vfxwipes.h"
#include "vfxswap.h"
#include "vfxspiral.h"
#include "vfxzoom.h"
#include "vfxsqueeze.h"
#include "vfxsplit.h"
#include "vfxcross.h"
#include "vfxpage.h"
#include "vfxflash.h"
#include "vfxiris.h"
#include "vfxstarwipe.h"
#include "vkshaders/push_layouts.h"

namespace reno {
namespace ofx {
namespace builtin {

namespace {

typedef gpu::Plan Plan;
typedef gpu::Pass Pass;
namespace P = gpu::push;

// The incoming frame as a picture buffer (the frame goes up with the plan as a table of w * h * 4 floats, top row first, as Ctx::src2 holds it).
bool incomingBuf(Ctx& c, Plan& p, int& buf)
{
    int t;
    if (c.src2Bytes) t = p.addFrameTable(c.src2Bytes, c.src2RowBytes, c.src2Bgra);
    else if (c.src2.px.size() == (size_t)p.w * p.h * 4) t = p.addTable(std::move(c.src2.px));
    else return false;
    if (t < 0) return false;
    buf = p.addBuf(p.w, p.h, 4);
    Pass& ps = p.add("table_to_buf", 0, buf, p.w, p.h);
    ps.table = t;
    ps.setPush(P::TableToBuf{p.w, p.h});
    return true;
}

// The border colour as the CPU ports hand it to their kernels (colorPx: the straight colour with alpha 1, premultiplied by that alpha).
void borderColour(Ctx& c, const char* name, float* v)
{
    const Color col = c.color(name);
    put(v, Color{col.r, col.g, col.b, 1.0});
}

inline uint32_t q8(const Ctx& c) { return c.host8 ? 1u : 0u; }

// One pass over the frame: 'first' and 'second' are the buffers to read (in0, in1).
Pass& frameStep(Plan& p, const char* shader, int first, int second)
{
    Pass& ps = p.add(shader, first, 1, p.w, p.h);
    ps.in1 = second;
    return ps;
}

// The 32 floats of the 2D transition library's kernels (tools/gpukernels_oclt.py rect_params): v31 .. v34 (4 each), v35 .. v38 (2 each), v39, v40 (4 each).
std::vector<float> rectTable(const float* v31, const float* v32, const float* v33, const float* v34, const float* v35, const float* v36, const float* v37, const float* v38,
                             const float* v39, const float* v40)
{
    std::vector<float> t;
    auto add = [&](const float* v, int n) { for (int i = 0; i < n; ++i) t.push_back(v ? v[i] : 0.0f); };
    add(v31, 4); add(v32, 4); add(v33, 4); add(v34, 4); add(v35, 2); add(v36, 2); add(v37, 2); add(v38, 2); add(v39, 4); add(v40, 4);
    return t;
}

// The picture 'src' smoothed by the 3x3 pass (or Squeeze's) into a new buffer.
int smoothed(Plan& p, int src, int m0, int m1)
{
    const int b = p.addBuf(p.w, p.h, 4);
    p.add("oclt_smooth", src, b, p.w, p.h).setPush(P::OcltSmooth{p.w, p.h, m0, m1});
    return b;
}

// The 25 floats of the recursive Gaussian's kernels (oclt_gauss): n, d, m, bn, bm.
std::vector<float> gaussTable(double sigma)
{
    const vfxgauss::Coefsf f = vfxgauss::toFloats(vfxgauss::build(sigma));
    std::vector<float> t;
    for (const float* a : {f.n, f.d, f.m, f.bn, f.bm}) for (int k = 0; k < 5; ++k) t.push_back(a[k]);
    return t;
}

// The picture 'src' blurred with the Gaussian of the two sigmas (vertical pass, then the row pass over the sum of its two runs): a buffer of two planes whose sum is the result.
int gaussPlanes(Ctx& c, Plan& p, int src, double sigmaX, double sigmaY)
{
    const int pl1 = p.addBuf(p.w, 2 * p.h, 4), pl2 = p.addBuf(p.w, 2 * p.h, 4);
    Pass& a = p.add("oclt_gauss", src, pl1, p.w, 2);
    a.table = p.addTable(gaussTable(sigmaY));
    a.setPush(P::OcltGauss{p.w, p.h, 0u, 0u, q8(c)});
    Pass& b = p.add("oclt_gauss", pl1, pl2, p.h, 2);
    b.table = p.addTable(gaussTable(sigmaX));
    b.setPush(P::OcltGauss{p.w, p.h, 1u, 1u, q8(c)});
    return pl2;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------- Dissolve (dissolveOpenCl in builtins_trans.cpp)
bool planOclDissolve(Ctx& c, Plan& p)
{
    int to;
    if (!incomingBuf(c, p, to)) return false;
    const int type = std::min(std::max(c.i("Type"), 0), 9);
    float col[4] = {0, 0, 0, 0};
    if (type == 3 || type == 4) {
        const char* nm[4] = {"ColorBleedSpeedRed", "ColorBleedSpeedGreen", "ColorBleedSpeedBlue", "ColorBleedSpeedAlpha"};
        const char* nmM[4] = {"ColorMorphSpeedRed", "ColorMorphSpeedGreen", "ColorMorphSpeedBlue", "ColorMorphSpeedAlpha"};
        for (int k = 0; k < 4; ++k) col[k] = (float)c.d(type == 3 ? nm[k] : nmM[k]);
    } else if (type == 8) {
        const Color fc = c.color("FadeThroughColor");
        col[0] = (float)fc.r; col[1] = (float)fc.g; col[2] = (float)fc.b; col[3] = (float)fc.a;
    }
    P::OcltDissolve pc = {p.w, p.h, type, q8(c), (float)c.progress, col[0], col[1], col[2], col[3]};
    frameStep(p, "oclt_dissolve", 0, to).setPush(pc);
    return true;
}

// ---------------------------------------------------------------------------------------------------- Barn Door, Push, Swap (vfxwipes.h, vfxswap.h)
bool planOclBarndoor(Ctx& c, Plan& p)
{
    int to;
    if (!incomingBuf(c, p, to)) return false;
    float bc[4];
    borderColour(c, "BorderColor", bc);
    const wipes::BarnDoor k(p.w, p.h, c.i("Orientation"), c.i("Direction"), c.progress, c.d("BorderSize"), c.d("BorderFeather"), bc);
    P::OcltBarndoor pc = {p.w, p.h, k.alongX ? 1u : 0u, q8(c), k.s0, k.s1, k.s2, k.s3, k.col[0], k.col[1], k.col[2], k.col[3]};
    frameStep(p, "oclt_barndoor", k.swapPics ? to : 0, k.swapPics ? 0 : to).setPush(pc);
    return true;
}

bool planOclPush(Ctx& c, Plan& p)
{
    int to;
    if (!incomingBuf(c, p, to)) return false;
    float bc[4];
    borderColour(c, "BorderColor", bc);
    const wipes::Push k(p.w, p.h, c.i("Direction"), c.progress, c.d("BorderSize"), c.d("BorderFeather"), c.b("PushOffPreviousImage"), bc);
    P::OcltPush pc = {p.w, p.h, (uint32_t)k.kernel, q8(c), k.L, k.pushOff, k.s0, k.s1, k.s2, k.s3, k.col[0], k.col[1], k.col[2], k.col[3]};
    frameStep(p, "oclt_push", 0, to).setPush(pc);
    return true;
}

bool planOclSwap(Ctx& c, Plan& p)
{
    int to;
    if (!incomingBuf(c, p, to)) return false;
    float bc[4];
    borderColour(c, "BorderColor", bc);
    const wipes::Swap k(p.w, p.h, c.i("Direction"), c.progress, c.d("BorderSize"), c.d("BorderFeather"), bc);
    P::OcltSwap pc = {p.w, p.h, (uint32_t)k.kernel, q8(c), k.L, k.p, k.s0, k.s1, k.s2, k.s3, k.col[0], k.col[1], k.col[2], k.col[3]};
    frameStep(p, "oclt_swap", 0, to).setPush(pc);
    return true;
}

// ---------------------------------------------------------------------------------------------------- Slide, Spiral, Zoom (wipes::Slide, Spiral, Zoom)
bool planOclSlide(Ctx& c, Plan& p)
{
    int to;
    if (!incomingBuf(c, p, to)) return false;
    float bc[4];
    borderColour(c, "BorderColor", bc);
    const wipes::Slide k(p.w, p.h, c.d("Angle"), c.i("Direction"), c.progress, c.d("BorderSize"), c.d("BorderFeather"), bc);
    Pass& ps = frameStep(p, "oclt_slide", k.swapPics ? to : 0, k.swapPics ? 0 : to);
    ps.table = p.addTable(rectTable(k.v31, k.v32, k.v33, k.v34, k.v35, k.v36, k.v37, k.v38, k.v39, k.v40));
    ps.setPush(P::OcltSlide{p.w, p.h, (uint32_t)k.variant, q8(c)});
    return true;
}

bool planOclSpiral(Ctx& c, Plan& p)
{
    int to;
    if (!incomingBuf(c, p, to)) return false;
    float bc[4];
    borderColour(c, "BorderColor", bc);
    const wipes::Spiral k(p.w, p.h, c.d("Turns"), c.d("Zoom"), c.i("Orientation"), c.i("Motion"), c.i("Direction"), c.progress, c.d("BorderSize"), c.d("BorderFeather"), bc);
    const int first = k.swapPics ? to : 0;
    int mover = k.swapPics ? 0 : to;
    if (k.smoothMoved) mover = smoothed(p, mover, 0, 0);
    Pass& ps = frameStep(p, "oclt_spiral", first, mover);
    ps.table = p.addTable(rectTable(k.v31, k.v32, k.v33, k.v34, k.v35, k.v36, k.v37, k.v38, k.v39, nullptr));
    ps.setPush(P::OcltSpiral{p.w, p.h, (uint32_t)k.variant, q8(c)});
    return true;
}

bool planOclZoom(Ctx& c, Plan& p)
{
    int to;
    if (!incomingBuf(c, p, to)) return false;
    float bc[4];
    borderColour(c, "BorderColor", bc);
    double cx = 0.5, cy = 0.5;
    c.xy("Center", cx, cy);
    const wipes::Zoom k(p.w, p.h, c.i("Direction"), c.progress, cx, cy, c.d("BorderSize"), c.d("BorderFeather"), bc);
    const int first = k.swapPics ? to : 0;
    int mover = k.swapPics ? 0 : to;
    if (k.smoothMoved) mover = smoothed(p, mover, 0, 0);
    Pass& ps = frameStep(p, "oclt_zoom", first, mover);
    ps.table = p.addTable(rectTable(k.v31, k.v32, k.v33, k.v34, k.v35, k.v36, k.v37, k.v38, k.v39, k.v40));
    ps.setPush(P::OcltZoom{p.w, p.h, (uint32_t)k.variant, q8(c)});
    return true;
}

// ---------------------------------------------------------------------------------------------------- Squeeze, Split
bool planOclSqueeze(Ctx& c, Plan& p)
{
    int to;
    if (!incomingBuf(c, p, to)) return false;
    float bc[4];
    borderColour(c, "BorderColor", bc);
    const wipes::Squeeze k(p.w, p.h, c.i("Start"), c.progress, c.b("SqueezePreviousImage"), c.d("BorderSize"), c.d("BorderFeather"), bc);
    int first = 0, second = to;
    if (k.smoothFirst) first = smoothed(p, first, k.mode1[0], k.mode1[1]);
    if (k.smoothSecond) second = smoothed(p, second, k.mode2[0], k.mode2[1]);
    P::OcltSqueeze pc = {p.w, p.h, (uint32_t)k.kernel, q8(c), k.flag, k.s0, k.s1, k.s2, k.s3, k.col[0], k.col[1], k.col[2], k.col[3]};
    frameStep(p, "oclt_squeeze", first, second).setPush(pc);
    return true;
}

bool planOclSplit(Ctx& c, Plan& p)
{
    int to;
    if (!incomingBuf(c, p, to)) return false;
    float bc[4];
    borderColour(c, "BorderColor", bc);
    double cx = 0.5, cy = 0.5;
    c.xy("Center", cx, cy);
    const wipes::Split k((int)p.w, (int)p.h, c.i("SplitMode"), c.i("Direction"), c.progress, cx, cy, c.d("BorderSize"), c.d("BorderFeather"), bc);
    const int first = k.swapPics ? to : 0;
    int other = k.swapPics ? 0 : to;
    if (k.smoothOther && !k.onlyFirst) other = smoothed(p, other, 0, 0);
    std::vector<float> t;
    for (int i = 0; i < 2; ++i) t.push_back(k.v29[i]);
    for (int i = 0; i < 4; ++i) t.push_back(k.v30[i]);
    const float* q[8] = {k.v31, k.v32, k.v33, k.v34, k.v35, k.v36, k.v37, k.v38};
    for (int j = 0; j < 8; ++j) for (int i = 0; i < 4; ++i) t.push_back(q[j][i]);
    Pass& ps = frameStep(p, "oclt_split", first, other);
    ps.table = p.addTable(std::move(t));
    ps.setPush(P::OcltSplit{p.w, p.h, (uint32_t)k.variant, (uint32_t)k.mode, k.onlyFirst ? 1u : 0u, q8(c)});
    return true;
}

// ---------------------------------------------------------------------------------------------------- Flash (flashk::render)
bool planOclFlash(Ctx& c, Plan& p)
{
    int to;
    if (!incomingBuf(c, p, to)) return false;
    flashk::Settings s;
    s.progress = (float)c.progress;
    s.diffX = clampd(c.d("HorizontalDiffusion"), 0, 1);
    s.diffY = clampd(c.d("VerticalDiffusion"), 0, 1);
    const Color tc = c.color("Tint");
    s.tint[0] = tc.r; s.tint[1] = tc.g; s.tint[2] = tc.b;
    const flashk::Host h = flashk::host((int)p.w, (int)p.h, s);
    const int src = h.useTo ? to : 0;
    int planes = src;
    if (h.blur) planes = gaussPlanes(c, p, src, (double)h.sigmaX, (double)h.sigmaY);
    P::OcltFlash pc = {p.w, p.h, h.blur ? 1u : 0u, q8(c), h.a2, h.S, (float)s.tint[0], (float)s.tint[1], (float)s.tint[2]};
    frameStep(p, "oclt_flash", src, planes).setPush(pc);
    return true;
}

// ---------------------------------------------------------------------------------------------------- Cross Effect (crossfx::render)
bool planOclCrossEffect(Ctx& c, Plan& p)
{
    int to;
    if (!incomingBuf(c, p, to)) return false;
    const int W = (int)p.w, H = (int)p.h;
    crossfx::Settings s;
    s.type = std::min(std::max(c.i("EffectType"), 0), 3);
    s.apply = std::min(std::max(c.i("ApplyTo"), 0), 2);
    s.progress = c.progress;
    s.fadeRange = c.d("FadeRange");
    s.maxZoom = c.d("EffectSettingsMaxZoom");
    s.maxX = c.d("EffectSettingsMaxX");
    s.maxY = c.d("EffectSettingsMaxY");
    s.maxBlur = c.d("EffectSettingsMaxBlur");
    c.xy("EffectSettingsSource", s.srcX, s.srcY);
    c.xy("EffectSettingsDestination", s.dstX, s.dstY);
    const int type = s.type, apply = s.apply;
    float pf = (float)s.progress;
    if (!(pf == pf)) pf = 0.0f;
    pf = pf < 0.0f ? 0.0f : std::fmin(1.0f, pf);
    const float fr = (float)s.fadeRange;
    auto mixPass = [&](int a, int b, float t, bool aPlanes, bool bPlanes, int out) {
        Pass& ps = p.add("oclt_cross_mix", a, out, p.w, p.h);
        ps.in1 = b;
        ps.setPush(P::OcltCrossMix{p.w, p.h, aPlanes ? 1u : 0u, bPlanes ? 1u : 0u, q8(c), t});
    };
    if (type == 0) {
        mixPass(0, to, crossfx::fadeKernel(pf, fr), false, false, 1);
        return true;
    }
    if (type == 1) {
        const float fade = crossfx::fadeHost(pf, s.fadeRange);
        const float Z = (float)s.maxZoom;
        const float sA = 1.0f / rm::pow(Z, pf);
        const float sB = 1.0f / rm::pow(Z, 1.0f - pf);
        const float cs[2] = {(float)s.srcX * (float)W, (float)s.srcY * (float)H};
        const float cd[2] = {(float)s.dstX * (float)W, (float)s.dstY * (float)H};
        P::OcltCrossZoom pc = {p.w, p.h, (uint32_t)apply, q8(c), fade, sA, sB, cs[0], cs[1], cd[0], cd[1]};
        frameStep(p, "oclt_cross_zoom", 0, to).setPush(pc);
        return true;
    }
    auto pixelate = [&](int src, int nx, int ny) {
        const int m1 = p.addBuf(p.w, p.h, 4), m2 = p.addBuf(p.w, p.h, 4);
        p.add("oclt_pixelate", src, m1, p.w, 1).setPush(P::OcltPixelate{p.w, p.h, 0u, (uint32_t)ny, q8(c)});
        p.add("oclt_pixelate", m1, m2, p.h, 1).setPush(P::OcltPixelate{p.w, p.h, 1u, (uint32_t)nx, q8(c)});
        return m2;
    };
    if (type == 2) {
        float pw;
        if (apply == 0) pw = rm::pow(pf, 0.25f);
        else if (apply == 1) pw = rm::pow(1.0f - pf, 0.25f);
        else {
            float q = pf;
            if (!(0.5f >= q)) q = 1.0f - q;
            q += q;
            pw = rm::pow(q, 0.25f);
        }
        int nx, ny;
        crossfx::pixelateCounts(W, H, pw, s.maxX, s.maxY, nx, ny);
        const float t = crossfx::fadeKernel(pf, fr);
        if (apply == 2) {
            const int base = p.addBuf(p.w, p.h, 4);
            mixPass(0, to, t, false, false, base);
            const int m1 = p.addBuf(p.w, p.h, 4);
            p.add("oclt_pixelate", base, m1, p.w, 1).setPush(P::OcltPixelate{p.w, p.h, 0u, (uint32_t)ny, q8(c)});
            p.add("oclt_pixelate", m1, 1, p.h, 1).setPush(P::OcltPixelate{p.w, p.h, 1u, (uint32_t)nx, q8(c)});
            return true;
        }
        const int pix = pixelate(apply == 0 ? 0 : to, nx, ny);
        if (apply == 0) mixPass(pix, to, t, false, false, 1);
        else mixPass(0, pix, t, false, false, 1);
        return true;
    }
    float sx, sy;
    crossfx::blurSigmas(W, H, apply, pf, s.maxBlur, sx, sy);
    int A = 0, B = to;
    if (apply != 1) A = gaussPlanes(c, p, 0, (double)sx, (double)sy);
    if (apply != 0) B = gaussPlanes(c, p, to, (double)sx, (double)sy);
    mixPass(A, B, crossfx::fadeKernel(pf, fr), apply != 1, apply != 0, 1);
    return true;
}

// ---------------------------------------------------------------------------------------------------- Page Peel / Page Loop / Page Roll (vfxpage.h, builtins_trans.cpp pageKernel)
static bool planOclPage(Ctx& c, Plan& p, int which)
{
    if (p.w < 1 || p.h < 1) return false;
    int to;
    if (!incomingBuf(c, p, to)) return false;
    const char* angleName = which == 0 ? "PeelAngle" : which == 1 ? "PaperAngle" : "RollAngle";
    const char* opacityName = which == 0 ? "PeelOpacity" : which == 1 ? "PaperOpacity" : "RollOpacity";
    const Color lc = c.color("LightColor");
    const double rgb[3] = {lc.r, lc.g, lc.b};
    const double angle = c.d(angleName), opacity = c.d(opacityName), persp = c.d("Perspective");
    const int w = (int)p.w, h = (int)p.h;
    std::vector<float> t;
    float sn, cs;
    int paper = 0, below = to;
    const char* shader;
    auto lightv = [&](const float* v) { for (int k = 0; k < 4; ++k) t.push_back(v[k]); };
    if (which == 1) {
        const vfxpage::LoopSetup s = vfxpage::loopSetup(w, h, c.progress, angle, c.d("LoopPosition"), c.d("LoopRadius"), opacity, persp, rgb);
        sn = s.sn; cs = s.cs;
        const float v[12] = {s.p, s.x0, s.R, s.circ, s.ramp, s.shift, s.lo, s.hi, s.alphaFlat, s.alphaEnd, s.persp, s.lightScale};
        t.assign(v, v + 12);
        lightv(s.lightCol);
        paper = to; below = 0;                     // (the incoming picture is the page here, the outgoing one lies below)
        shader = "oclt_page_loop";
    } else if (which == 0) {
        const vfxpage::PeelSetup s = vfxpage::peelSetup(w, h, c.progress, angle, c.d("FoldRadius"), c.d("SlideAmount"), opacity, persp, rgb);
        sn = s.sn; cs = s.cs;
        const float v[12] = {s.slide, s.flatEnd, s.c48, s.c4c, s.R, s.iR, s.edge, s.persp, s.opacity, s.inv, s.lightBase, s.lightScale};
        t.assign(v, v + 12);
        lightv(s.lightCol);
        shader = "oclt_page_peel";
    } else {
        const vfxpage::RollSetup s = vfxpage::rollSetup(w, h, c.progress, angle, c.d("FoldRadius"), c.d("SlideAmount"), opacity, persp, rgb);
        sn = s.sn; cs = s.cs;
        const float v[13] = {s.slide, s.R, s.R2, s.iR, s.iR2, s.D, s.c7c, s.edge, s.persp, s.opacity, s.inv, s.lightBase, s.lightScale};
        t.assign(v, v + 13);
        lightv(s.lightCol);
        shader = "oclt_page_roll";
    }
    Pass& ps = frameStep(p, shader, paper, below);
    ps.table = p.addTable(t);
    ps.setPush(P::OcltPagePeel{p.w, p.h, q8(c), sn, cs});
    return true;
}
bool planOclPagePeel(Ctx& c, Plan& p) { return c.openCl && planOclPage(c, p, 0); }
bool planOclPageLoop(Ctx& c, Plan& p) { return c.openCl && planOclPage(c, p, 1); }
bool planOclPageRoll(Ctx& c, Plan& p) { return c.openCl && planOclPage(c, p, 2); }

// ---------------------------------------------------------------------------------------------------- Star Wipe (wipes::StarWipe)
bool planOclStarWipe(Ctx& c, Plan& p)
{
    RENO_WIPES_NO_FMA
    int to;
    if (!incomingBuf(c, p, to)) return false;
    float bc[4];
    borderColour(c, "BorderColor", bc);
    double cx = 0.5, cy = 0.5;
    c.xy("Center", cx, cy);
    const wipes::StarWipe k((int)p.w, (int)p.h, c.i("Arms"), c.d("Ratio"), c.d("Angle"), c.d("Feather"), c.d("Border"), bc, cx, cy, c.i("Cycles"), c.i("Waveform"),
                            c.b("HorizontalFlip"), c.b("VerticalFlip"), c.b("HorizontalMirror"), c.b("VerticalMirror"), c.i("Direction"), c.progress);
    // the numbers StarWipe::pixel works out for every pixel from the settings alone
    float v47 = k.centerYInv, v46 = k.centerX, v45 = k.angle;
    if (k.flipY) { v47 = 1.0f - v47; v45 = 360.0f - v45; }
    if (k.flipX) {
        v46 = 1.0f - v46;
        v45 = 180.0f - v45;
        v45 = (v45 < 0.0f) ? (v45 + 360.0f) : v45;
    }
    const float cen0 = (float)(k.W - 1) * v46, cen1 = (float)(k.H - 1) * (1.0f - v47);
    v45 = v45 * 0.01745329252f;
    const float v3 = wipes::StarWipe::cosf_(v45), v4 = wipes::StarWipe::sinf_(v45);
    const float v49 = k.sawtooth ? 1.0f : 2.0f;
    const float v2 = 6.283185307f * (1.0f / (float)k.arms);
    const float v13 = v2 * 0.5f;
    const float v5 = -k.ratio * wipes::StarWipe::sinf_(v13) / (1.0f - wipes::StarWipe::cosf_(v13) * k.ratio);
    const float v52 = (float)k.cycles * (1.0f / k.maxDist);
    P::OcltStarwipe pc = {p.w, p.h, k.reverse ? 1u : 0u, k.sawtooth ? 1u : 0u, k.mirrorX ? 1u : 0u, k.mirrorY ? 1u : 0u, q8(c),
                          cen0, cen1, v2, v3, v4, v5, v49, v52, k.maxDist, k.p, k.border, k.feather, k.col[0], k.col[1], k.col[2], k.col[3]};
    frameStep(p, "oclt_starwipe", 0, to).setPush(pc);
    return true;
}

// ---------------------------------------------------------------------------------------------------- Iris (wipes::Iris)
bool planOclIris(Ctx& c, Plan& p)
{
    if (p.w < 2 || p.h < 2) return false;
    int to;
    if (!incomingBuf(c, p, to)) return false;
    double cx = 0.5, cy = 0.5;
    c.xy("Center", cx, cy);
    float bc[4];
    borderColour(c, "Color", bc);
    const wipes::Iris k((int)p.w, (int)p.h, c.i("Shape"), c.i("RepeatX"), c.i("RepeatY"), c.progress, c.i("Direction"), cx, cy, c.d("Feather"), c.d("Border"), bc);
    P::OcltIris pc = {p.w, p.h, k.shape, k.nx, k.ny, k.cellW, k.cellH, q8(c), k.ratio, k.asp[0], k.asp[1], k.ox, k.oy, k.maxDist, k.feather, k.border, k.threshold,
                      k.col[0], k.col[1], k.col[2], k.col[3]};
    frameStep(p, "oclt_iris", k.swapPics ? to : 0, k.swapPics ? 0 : to).setPush(pc);
    return true;
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
