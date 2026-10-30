// The built-in effects on the GPU: for each effect with Vulkan kernels, a function that plans the compute passes from the effect's settings. Each reads the
// settings exactly as the effect's CPU code does (same clamps, same float arithmetic) and hands the kernel (tools/gpukernels.py) the numbers in a
// push-constant block (shaders/push_layouts.h). See vkeffects.h.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include "vkeffects.h"
#include "gpupick.h"
#include "vegas8.h"
#include "vkfilters.h"
#include "vkshaders/push_layouts.h"
#include "builtins_deriche.h"
#include "../core/pixrows.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
namespace more { double unsharpSigma(double radius, int W, int H, bool& none); void chromaBlur8Widths(Ctx& c, int& rh, int bv[3]); }

namespace {

typedef gpu::Plan Plan;
typedef gpu::Pass Pass;
namespace P = gpu::push;

const char* const kPrefix = "com.vegascreativesoftware:";
// An effect's full identifier from the table's: VEGAS's own get the prefix, the others (de.magix:...) already have a colon
std::string fullId(const char* id) { return (std::strchr(id, ':') || std::strncmp(id, "com.magix.ofx.", 14) == 0) ? std::string(id) : std::string(kPrefix) + id; }

inline uint32_t u32(int v) { return (uint32_t)std::max(0, v); }

// ---------------------------------------------------------------------------------------------------- the planners
bool planInvert(Ctx& c, Plan& p)
{
    if (c.levels) {
        // 8-bit: invertRows() (builtins_filters.cpp) works on the levels with VEGAS's integer steps; its weight is worked out here exactly as there
        const float amount = (float)clampd(c.d("BlendAmount"), 0, 1);
        const float wf = (float)(1.0 - (double)amount) * 256.f;
        const int w = std::min(255, v8::roundMagic((double)wf));
        P::Invert8 pc8 = {p.w, p.h, w};
        p.add("invert8", 0, 1, p.w, p.h).setPush(pc8);
        return true;
    }
    P::Invert pc = {p.w, p.h, (float)clampd(c.d("BlendAmount"), 0, 1)};
    p.add("invert", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

// Brightness and Contrast (brightnessContrastRows)
bool planBrightnessContrast(Ctx& c, Plan& p)
{
    const float bright = (float)c.d("Brightness");
    const float contrast = (float)clampd(c.d("Contrast"), -1, 1);
    const float center = (float)c.d("ContrastCenter");
    const float f = contrast >= 0 ? 1.f / std::max(1e-6f, 1.f - contrast) : 1.f + contrast;
    P::Brightnessandcontrast pc = {p.w, p.h, bright, center, f};
    p.add("brightnessandcontrast", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planBlackAndWhite(Ctx& c, Plan& p)
{
    float k = (float)clampd(c.d("BlendAmount"), 0, 1);
    if (k > 0.f && k < 0.375f) k += 1.f / 256.f;           // (as blackAndWhiteRows)
    P::Blackandwhite pc = {p.w, p.h, k};
    p.add("blackandwhite", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planThreshold(Ctx& c, Plan& p)
{
    P::Threshold pc = {p.w, p.h, (float)c.d("Threshold")};
    p.add("threshold", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

// a double as the two 32-bit halves of its bits (the push constants of the kernels that work in doubles: tools/sf64.py)
struct D2 { uint32_t h, l; };
D2 d2(double d)
{
    uint64_t b;
    std::memcpy(&b, &d, 8);
    return D2{(uint32_t)(b >> 32), (uint32_t)b};
}

bool planColorBalance(Ctx& c, Plan& p)
{
    if (c.levels) {         // 8-bit: VEGAS's own byte routine (colorBalance8() in builtins_filters.cpp)
        const D2 R = d2((double)(float)c.d("Red")), G = d2((double)(float)c.d("Green")), B = d2((double)(float)c.d("Blue"));
        P::Colorbalance8 q = {p.w, p.h, R.h, R.l, G.h, G.l, B.h, B.l, u32(c.i("Type")), c.b("PreserveLuminosity") ? 1u : 0u};
        p.add("colorbalance8", 0, 1, p.w, p.h).setPush(q);
        return true;
    }
    P::Colorbalance pc = {p.w, p.h, (float)c.d("Red"), (float)c.d("Green"), (float)c.d("Blue"), u32(c.i("Type")), c.b("PreserveLuminosity") ? 1u : 0u};
    p.add("colorbalance", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planHslAdjust(Ctx& c, Plan& p)
{
    if (c.levels) {         // 8-bit: VEGAS's byte HSL routines (hslAdjust() in builtins_filters.cpp)
        const D2 HU = d2(c.d("AddToHue")), SA = d2(c.d("Saturation")), LU = d2(c.d("Luminance"));
        P::Hsladjust8 pc8 = {p.w, p.h, HU.h, HU.l, SA.h, SA.l, LU.h, LU.l};
        p.add("hsladjust8", 0, 1, p.w, p.h).setPush(pc8);
        return true;
    }
    P::Hsladjust pc = {p.w, p.h, (float)c.d("AddToHue"), (float)std::max(0.0, c.d("Saturation")), (float)std::max(0.0, c.d("Luminance"))};
    p.add("hsladjust", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planLevels(Ctx& c, Plan& p)
{
    // levels() in builtins_filters.cpp: the same constants, worked out with the same calls
    const double i0 = c.d("InputStart"), o0 = c.d("OutputStart"), o1 = c.d("OutputEnd"), gamma = c.d("Gamma");
    double i1 = c.d("InputEnd"), width = i1 - i0;
    if (std::fabs(width) < 0.003921568859368563) { i1 = i0 + 0.003921568859368563; width = 0.003921568859368563; }
    const bool straight = std::fabs(gamma - 1.0) < 1e-6;
    const double G = 1.0 / gamma;
    double k0 = 0, slope = 0, a = 0, inv = 0;
    if (!straight) {
        k0 = std::pow(1.0 / G, -0.9418051973077521) * 0.03818376618407356;
        slope = std::pow(1.0 / G, 1.8836103946155043);
        if (G < 1.0) {
            const double t1 = std::pow(k0, G);
            a = (slope * k0 - t1) / (t1 - 1.0);
        } else {
            const double u = slope * k0, w = std::pow(u, 1.0 / G);
            a = (k0 - w) / (w - 1.0);
            inv = 1.0 / (a + 1.0);
        }
    }
    const D2 I0 = d2(i0), I1 = d2(i1), O0 = d2(o0), DO = d2(o1 - o0), WI = d2(width), K0 = d2(k0), SL = d2(slope), A1 = d2(a + 1.0), A = d2(a), IN = d2(inv), GG = d2(G);
    P::Levels pc = {p.w, p.h, u32(c.i("Channel")), straight ? 1u : 0u, G < 1.0 ? 1u : 0u, I0.h, I0.l, I1.h, I1.l, O0.h, O0.l, DO.h, DO.l, WI.h, WI.l, K0.h, K0.l, SL.h, SL.l,
                    A1.h, A1.l, A.h, A.l, IN.h, IN.l, GG.h, GG.l};
    p.add("levels", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planColorCurves(Ctx& c, Plan& p)
{
    std::vector<float> tables;
    if (c.levels) {
        if (!colorCurves8GpuTables(c, tables)) {
            P::CopyPx cp = {p.w, p.h};
            p.add("copy_px", 0, 1, p.w, p.h).setPush(cp);
            return true;
        }
        P::Colorcurves8 pc8 = {p.w, p.h};
        Pass& pass8 = p.add("colorcurves8", 0, 1, p.w, p.h);
        pass8.table = p.addTable(std::move(tables));
        pass8.setPush(pc8);
        return true;
    }
    if (!colorCurvesGpuTables(c, tables)) {
        P::CopyPx cp = {p.w, p.h};
        p.add("copy_px", 0, 1, p.w, p.h).setPush(cp);
        return true;
    }
    const int t = p.addTable(std::move(tables));
    P::Colorcurves pc = {p.w, p.h};
    Pass& pass = p.add("colorcurves", 0, 1, p.w, p.h);
    pass.table = t;
    pass.setPush(pc);
    return true;
}

// Color Corrector (Secondary) (colorCorrectorSecondaryRows in builtins_filters2.cpp: the settings are read the same way)
bool planColorCorrectorSecondary(Ctx& c, Plan& p)
{
    if (c.levels) {         // 8-bit: VEGAS's own byte processor (colorCorrectorSecondary8() in builtins_filters2.cpp), its tables from the CPU's code
        Ccs8Gpu g;
        ccs8Gpu(c, g);
        if (g.mode == 0) {
            P::Colorcorrector8 pc8 = {p.w, p.h, (uint32_t)g.flags};
            Pass& ps8 = p.add("colorcorrector8", 0, 1, p.w, p.h);
            ps8.table = p.addTable(std::move(g.tab));
            ps8.setPush(pc8);
        } else {
            P::Ccs8 pc = {p.w, p.h, (uint32_t)g.flags, g.show ? 1u : 0u, g.inv ? 1u : 0u, g.tintU, g.tintV, g.satF, g.alphaF, g.rot[0], g.rot[1], g.rot[2], g.rot[3]};
            Pass& ps = p.add("ccs8", 0, 1, p.w, p.h);
            ps.table = p.addTable(std::move(g.tab));
            ps.setPush(pc);
        }
        return true;
    }
    double ca, cm;
    c.xy("Chrominance", ca, cm);
    const double a = ca * kPi / 180.0, cam = clampd(cm, -2, 2), k = (cam < 0 ? -cam * cam : cam * cam) * 0.5;
    const float cu = (float)(std::cos(a) * k), cv = (float)(std::sin(a) * k);
    const double rot = c.d("RotateHue") * kPi / 180.0;
    const float cr = (float)std::cos(rot), sr = (float)std::sin(rot);
    const float sat = (float)clampd(c.d("Saturation"), 0, 100);
    float gc, ga;
    secondaryGammaCurve(c.d("Gamma"), gc, ga);
    const bool gammaOn = std::fabs(ga - 1.f) > 1e-6f || std::fabs(gc) > 1e-6f;
    const float gain = (float)clampd(c.d("Gain"), -100, 100);
    const float offset = (float)(clampd(c.d("Offset"), -1000, 1000) / 255.0);
    const float alpha = (float)clampd(c.d("Alpha"), 0, 1);
    const bool showMask = c.b("ShowMask"), invert = c.b("InvertMask");
    const bool useL = c.b("EnableLimitLuminance"), useS = c.b("EnableLimitSaturation"), useH = c.b("EnableLimitHue");
    float l0 = (float)c.d("LimitLuminanceLow"), l1 = (float)c.d("LimitLuminanceHigh");
    const float ls = (float)std::max(0.0, c.d("LimitLuminanceSmooth"));
    float s0 = (float)c.d("LimitSaturationLow"), s1 = (float)c.d("LimitSaturationHigh");
    const float ss = (float)std::max(0.0, c.d("LimitSaturationSmooth"));
    const float hc = (float)c.d("LimitHueCenter"), hw = (float)std::max(0.0, c.d("LimitHueWidth")), hs = (float)std::max(0.0, c.d("LimitHueSmooth"));
    if (l1 < l0) std::swap(l0, l1);
    if (s1 < s0) std::swap(s0, s1);
    const bool hueLimit = useH && hw < 360.f;
    uint32_t flags = 0;
    if (useL) flags |= 1u;
    if (useS) flags |= 2u;
    if (hueLimit) flags |= 4u;
    if (showMask) flags |= 8u;
    if (invert) flags |= 16u;
    if (gammaOn) flags |= 32u;
    P::Colorcorrectorsecondary pc = {p.w, p.h, cr, sr, sat, cu, cv, gain, offset, alpha, gc, ga, flags, l0, l1, ls, s0, s1, ss, hc, hw, hs};
    p.add("colorcorrectorsecondary", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

// ---- blurs
// The Gaussian's weights: the one-sided kernel gaussKernel() in builtins.cpp makes (taps to 3 sigma, normalised).
std::vector<float> gaussWeights(double sigma)
{
    const int r = (int)std::min(2000.0, std::ceil(sigma * 3));
    std::vector<float> k((size_t)r + 1);
    double sum = 0;
    for (int i = 0; i <= r; ++i) { k[(size_t)i] = (float)std::exp(-0.5 * i * i / (sigma * sigma)); sum += (i ? 2 : 1) * k[(size_t)i]; }
    for (auto& v : k) v = (float)(v / sum);
    return k;
}

// Blur passes over a picture (comps 4) or a plane (comps 1), across (sigmaX) and down (sigmaY) as the CPU does (an axis with sigma 0.3 or less is left out).
// 'from' holds the picture, 'to' is where the result is wanted; returns the buffer holding the blurred picture (the frame itself when no pass was needed);
// 'scratch' is used between passes.
int addBlur(Plan& p, int from, int to, double sigmaX, double sigmaY, uint32_t mask, bool plane)
{
    const uint32_t W = p.bufs[(size_t)from].w, H = p.bufs[(size_t)from].h;
    const bool bx = sigmaX > 0.3, by = sigmaY > 0.3;
    if (!bx && !by) return from;
    int cur = from;
    if (bx) {
        const std::vector<float> k = gaussWeights(sigmaX);
        const int dst = by ? p.addBuf(W, H, plane ? 1 : 4) : to;
        Pass& pass = p.add(plane ? "blurp_h" : "blur_h", cur, dst, W, H);
        pass.table = p.addTable(k);
        if (plane) { P::BlurpH pc = {W, H, (uint32_t)k.size() - 1}; pass.setPush(pc); }
        else { P::BlurH pc = {W, H, (uint32_t)k.size() - 1, mask}; pass.setPush(pc); }
        cur = dst;
    }
    if (by) {
        const std::vector<float> k = gaussWeights(sigmaY);
        Pass& pass = p.add(plane ? "blurp_v" : "blur_v", cur, to, W, H);
        pass.table = p.addTable(k);
        if (plane) { P::BlurpV pc = {W, H, (uint32_t)k.size() - 1}; pass.setPush(pc); }
        else { P::BlurV pc = {W, H, (uint32_t)k.size() - 1, mask}; pass.setPush(pc); }
        cur = to;
    }
    return cur;
}

void addCopy(Plan& p, int from, int to)
{
    P::CopyPx pc = {p.w, p.h};
    p.add("copy_px", from, to, p.w, p.h).setPush(pc);
}

bool planGaussianBlur(Ctx& c, Plan& p)
{
    // gaussianBlurFx() in builtins_filters.cpp: VEGAS's Deriche Gaussian of size Range x the larger side; channels switched off keep the source
    const double big = (double)std::max(p.w, p.h);
    const uint32_t mask = (c.b("R") ? 1u : 0u) | (c.b("G") ? 2u : 0u) | (c.b("B") ? 4u : 0u) | (c.b("A") ? 8u : 0u);
    const int blurred = p.addBuf(p.w, p.h, 4);
    gpuAddDeriche(p, 0, blurred, vegasBlurSigma(c.d("HorizontalRange") * big), vegasBlurSigma(c.d("VerticalRange") * big), c.levels);
    P::MaskMix pc = {p.w, p.h, mask, 0u};
    Pass& ps = p.add("mask_mix", 0, 1, p.w, p.h);
    ps.in1 = blurred;
    ps.setPush(pc);
    return true;
}

bool planSharpen(Ctx& c, Plan& p)
{
    const double amount = clampd(c.d("Amount"), 0, 1);
    P::SharpenMix pc = {p.w, p.h, amount <= 0 ? 0.f : (float)(1.0 / (17.0 - 16.0 * amount)), c.levels ? 1u : 0u};
    p.add("sharpen_mix", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planMirror(Ctx& c, Plan& p)
{
    double cx, cy;
    c.xy("Center", cx, cy);
    const double a = c.d("Angle") * kPi / 180.0;
    const Color bg = c.color("Background");
    float bgPix[4];
    put(bgPix, bg);
    const double kx = -std::sin(a), ky = -std::cos(a);
    const bool straight = std::fabs(kx) < 1e-9 || std::fabs(ky) < 1e-9;
    if (c.levels) {
        // 8-bit: mirror() in builtins_filters.cpp sends angles away from the axes to mirror8General() (VEGAS's float steps on the bytes)
        const float th = (float)std::fmod(c.d("Angle") * 0.01745329252, 6.28318530718);
        static const float bands[8][2] = {{-0.008726646f, 0.008726646f}, {3.1328659f, 3.1503193f}, {-3.1503193f, -3.1328659f}, {1.5620697f, 1.5795230f},
                                          {-4.7211156f, -4.7036624f}, {4.7036624f, 4.7211156f}, {-1.5795230f, -1.5620697f}, {0.f, -1.f}};
        bool axis = false;
        for (int i = 0; i < 7; ++i) axis = axis || (th >= bands[i][0] && th <= bands[i][1]);
        if (!axis && std::isfinite(th)) {
            const int W = (int)p.w, H = (int)p.h;
            const float sx = (float)c.scale, sy = (float)c.scale;
            const float cxp = (float)(cx * (double)(W - 1)), cyp = (float)((1.0 - cy) * (double)(H - 1));
            const float u = (th > 0.f ? 3.14159274101257f : -3.14159274101257f) - th;
            const float t = (float)std::tan((double)u);
            const float isx = 1.0f / sx, isy = 1.0f / sy;
            const float cxs = cxp * isx, cys = cyp * isy;
            const float m1t = -1.0f / t;
            const float tcx = t * cxs, den = t - m1t;
            uint32_t bgp = 0;
            const double v[3] = {bg.r, bg.g, bg.b};
            for (int k = 0; k < 3; ++k) {
                const int b = (int)((float)v[k] * 255.0f + 0.5f);
                bgp |= (uint32_t)(uint8_t)b << (8 * k);
            }
            if (sx != sy) return false;
            P::Mirror8 pc8 = {p.w, p.h, sx, isx, isy, cxs, cys, t, m1t, tcx, den, u > 0.f ? 1u : 0u, bgp, 0u};
            p.add("mirror8", 0, 1, p.w, p.h).setPush(pc8);
            return true;
        }
        if (!straight) return false;            // (a line within half a degree of an axis, but not on it: the CPU's double code)
    }
    P::Mirror pc = {p.w, p.h, (float)kx, (float)ky, (float)(cx * p.w), (float)((1 - cy) * p.h), bgPix[0], bgPix[1], bgPix[2], bgPix[3],
                    (float)(0.4 * c.scale * kx * std::fabs(ky)), (float)(0.4 * c.scale * ky * std::fabs(kx)), (float)(0.5 * (kx + ky)), straight ? 1u : 0u};
    p.add("mirror", 0, 1, p.w, p.h).setPush(pc);
    return true;
}


// ---- more colour effects and distortions
bool planColorCorrector(Ctx& c, Plan& p)
{
    if (c.levels) {
        std::vector<float> tab;
        const int flags = colorCorrector8Gpu(c, tab);
        P::Colorcorrector8 pc8 = {p.w, p.h, (uint32_t)flags};
        Pass& ps8 = p.add("colorcorrector8", 0, 1, p.w, p.h);
        ps8.table = p.addTable(std::move(tab));
        ps8.setPush(pc8);
        return true;
    }
    float du[3], dv[3];
    const char* names[3] = {"Low", "Mid", "High"};
    for (int k = 0; k < 3; ++k) {
        double an, m;
        c.xy(names[k], an, m);
        const double am = clampd(m, -2, 2), t = (am < 0 ? -am * am : am * am) * 0.5, r = an * kPi / 180.0;
        du[k] = (float)(std::cos(r) * t);
        dv[k] = (float)(std::sin(r) * t);
    }
    const float sat = (float)clampd(c.d("Saturation"), 0, 100);
    float gc, ga;
    secondaryGammaCurve(c.d("Gamma"), gc, ga);
    const bool gammaOn = std::fabs(ga - 1.f) > 1e-6f || std::fabs(gc) > 1e-6f;
    const float gain = (float)clampd(c.d("Gain"), -100, 100);
    const float offset = (float)(clampd(c.d("Offset"), -1000, 1000) / 255.0);
    P::Colorcorrector pc = {p.w, p.h, sat, gain, gc, ga, gammaOn ? 1u : 0u, offset, du[0], du[1], du[2], dv[0], dv[1], dv[2]};
    p.add("colorcorrector", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planSaturationAdjust(Ctx& c, Plan& p)
{
    if (c.levels) {
        std::vector<float> tab;
        more::saturationAdjust8Gpu(c, tab);
        P::Saturationadjust8 pc8 = {p.w, p.h};
        Pass& ps8 = p.add("saturationadjust8", 0, 1, p.w, p.h);
        ps8.table = p.addTable(std::move(tab));
        ps8.setPush(pc8);
        return true;
    }
    const float scale = 0.9272f;
    P::Saturationadjust pc = {p.w, p.h, (float)c.d("Amount"), (float)c.d("Center") * scale, (float)std::max(0.005, c.d("Spread")) * 0.5f * scale, (float)c.d("Low"), (float)c.d("High")};
    p.add("saturationadjust", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planBlackRestore(Ctx& c, Plan& p)
{
    P::Blackrestore pc = {p.w, p.h, (float)clampd(c.d("Threshold"), 0, 1) + 1e-6f};
    p.add("blackrestore", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planChannelBlend(Ctx& c, Plan& p)
{
    float w[4][5];
    const char* names[4] = {"BLUE", "GREEN", "RED", "ALPHA"};      // VEGAS numbers the channels blue first (see channelBlend)
    const char* parts[5] = {" weight B", " weight G", " weight R", " weight A", " weight Offset"};
    for (int o = 0; o < 4; ++o)
        for (int i = 0; i < 5; ++i) w[o][i] = (float)c.d((std::string(names[o]) + parts[i]).c_str());
    P::Channelblend pc;
    pc.w = p.w; pc.h = p.h;
    pc.flags = (c.b("Normalize") ? 1u : 0u) | (c.levels ? 2u : 0u);
    pc.pad = 0;
    float* dst = &pc.w0;
    for (int o = 0; o < 4; ++o)
        for (int i = 0; i < 5; ++i) dst[o * 5 + i] = w[o][i];
    p.add("channelblend", 0, 1, p.w, p.h).setPush(pc);
    return true;
}


bool planUnsharpMask(Ctx& c, Plan& p)
{
    bool none = false;
    const double sigma = more::unsharpSigma(c.d("Radius"), (int)p.w, (int)p.h, none);
    const int thr8 = (int)(c.d("Threshold") * 255.0 + 0.5);
    if (none || (thr8 & 255) == 255) {
        // nothing to sharpen: the frame as it is
        P::CopyPx cp = {p.w, p.h};
        p.add("copy_px", 0, 1, p.w, p.h).setPush(cp);
        return true;
    }
    const int blurred = p.addBuf(p.w, p.h, 4);
    gpuAddDeriche(p, 0, blurred, sigma, sigma, c.levels);
    const D2 AM = d2(c.d("Amount"));
    P::UnsharpMix pc = {p.w, p.h, AM.h, AM.l, (float)thr8, c.levels ? 1u : 0u};
    Pass& pass = p.add("unsharp_mix", 0, 1, p.w, p.h);
    pass.in1 = blurred;
    pass.setPush(pc);
    return true;
}

bool planQuickBlur(Ctx& c, Plan& p)
{
    // quickBlur() in builtins_more_blur.cpp
    const float blend = (float)c.d("BlendAmount");
    if (std::fabs(blend) <= 9.999999974752427e-07f) {
        P::CopyPx cp = {p.w, p.h};
        p.add("copy_px", 0, 1, p.w, p.h).setPush(cp);
        return true;
    }
    const float wf = (float)(1.0 - (double)blend);
    const int wb = v8::roundMagic((double)(wf * 255.f)) & 255;
    P::Quickblur pc = {p.w, p.h, wb, wf, c.levels ? 1u : 0u, 0u};
    p.add("quickblur", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planPixelate(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    std::vector<int> sx, sy;
    int kx = 1, ky = 1;
    pixelateAxis(W, clampd(c.d("HorizontalPixelization"), 0, 1), c.scale, sx, kx);
    pixelateAxis(H, clampd(c.d("VerticalPixelization"), 0, 1), c.scale, sy, ky);
    const int nbx = (int)sx.size() - 1, nby = (int)sy.size() - 1;
    std::vector<float> starts, cells;
    for (int v : sx) starts.push_back((float)v);
    for (int v : sy) starts.push_back((float)v);
    for (int j = 0, x = 0; x < W; ++x) { while (j + 1 < nbx && sx[(size_t)j + 1] <= x) ++j; cells.push_back((float)j); }
    for (int y = 0; y < H; ++y) {
        const int yb = H - 1 - y;
        int j = 0;
        while (j + 1 < nby && sy[(size_t)j + 1] <= yb) ++j;
        cells.push_back((float)j);
    }
    const int blocks = p.addBuf((uint32_t)nbx, (uint32_t)nby, 4);
    {
        const bool copyOnly = std::fabs((float)c.d("HorizontalPixelization")) <= 1e-6f && std::fabs((float)c.d("VerticalPixelization")) <= 1e-6f;
        P::PixelateBlocks pc = {p.w, p.h, (uint32_t)nbx, (uint32_t)nby, (uint32_t)kx, (uint32_t)ky, c.levels ? 1u : 0u, copyOnly ? 1u : 0u};
        Pass& pass = p.add("pixelate_blocks", 0, blocks, (uint32_t)nbx, (uint32_t)nby);
        pass.table = p.addTable(starts);
        pass.setPush(pc);
    }
    P::PixelateApply pc = {p.w, p.h, (uint32_t)nbx, 0};
    Pass& pass = p.add("pixelate_apply", blocks, 1, p.w, p.h);
    pass.table = p.addTable(cells);
    pass.setPush(pc);
    return true;
}

// The ellipse round Center that Swirl, Pinch/Punch and Spherize work inside (Area in builtins_distort.cpp)
struct AreaPush { double px0, py0, rx, ry; };
AreaPush areaOf(Ctx& c, int W, int H)
{
    double cx, cy;
    c.xy("Center", cx, cy);
    const double sh = clampd(c.d("ScalingHorizontal"), 0, 10);
    const double sv = c.b("ScalingProportional") ? sh * W / std::max(1, H) : clampd(c.d("ScalingVertical"), 0, 10);
    AreaPush a;
    a.rx = std::max(1e-3, sh * W * 0.5);
    a.ry = std::max(1e-3, sv * H * 0.5);
    a.px0 = cx * W;
    a.py0 = (1 - cy) * H;
    return a;
}

// (Spherize is planned in vkfilters.cpp)

bool planSwirl(Ctx& c, Plan& p)
{
    const AreaPush a = areaOf(c, (int)p.w, (int)p.h);
    const D2 X = d2(a.px0), Y = d2(a.py0), RX = d2(a.rx), RY = d2(a.ry), T = d2(clampd(c.d("Amount"), -1, 1) * 2 * 2 * kPi);
    P::Swirl pc = {p.w, p.h, X.h, X.l, Y.h, Y.l, RX.h, RX.l, RY.h, RY.l, T.h, T.l};
    p.add("swirl", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planPinchPunch(Ctx& c, Plan& p)
{
    const AreaPush a = areaOf(c, (int)p.w, (int)p.h);
    const D2 X = d2(a.px0), Y = d2(a.py0), RX = d2(a.rx), RY = d2(a.ry), A = d2(clampd(c.d("Amount"), -1, 1));
    P::Pinchpunch pc = {p.w, p.h, X.h, X.l, Y.h, Y.l, RX.h, RX.l, RY.h, RY.l, A.h, A.l};
    p.add("pinchpunch", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planWave(Ctx& c, Plan& p)
{
    const double W = p.w, H = p.h;
    const D2 vn = d2(clampd(c.d("VerticalWaves"), 0, 1000)), hn = d2(clampd(c.d("HorizontalWaves"), 0, 1000)), va = d2(clampd(c.d("VerticalAmplitude"), -1, 1) * H),
             ha = d2(clampd(c.d("HorizontalAmplitude"), -1, 1) * W), vp = d2(c.d("VerticalPhase")), hp = d2(c.d("HorizontalPhase"));
    P::Wave pc = {p.w, p.h, vn.h, vn.l, hn.h, hn.l, va.h, va.l, ha.h, ha.l, vp.h, vp.l, hp.h, hp.l};
    p.add("wave", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

// Min and Max (neighbourhood() in builtins_more_blur.cpp, halfWidths() in builtins_fast.cpp)
bool planMinMax(Ctx& c, Plan& p)
{
    if (c.levels) return gpuPlanMinMax8(c, p);          // the 8-bit path is VEGAS's weighted histogram rank (builtins_more_blur.cpp)
    const double hr = clampd(c.d("HorizontalRange"), 0, 1), vr = clampd(c.d("VerticalRange"), 0, 1);
    const double rxf = c.px(hr * 16), ryf = c.px(vr * 16);
    const int RX = (int)std::lround(rxf), RY = (int)std::lround(ryf);
    const bool round = c.b("CircularAreaProcessing");
    if (RX == 0 && RY == 0) {
        P::CopyPx cp = {p.w, p.h};
        p.add("copy_px", 0, 1, p.w, p.h).setPush(cp);
        return true;
    }
    std::vector<float> hw((size_t)(2 * RY + 1), (float)RX);
    if (round) {
        for (int j = -RY; j <= RY; ++j) {
            const double v = (double)j / (ryf + 0.5);
            int h = 0;
            for (int i = RX; i >= 0; --i) {
                const double u = (double)i / (rxf + 0.5);
                if (!(u * u + v * v > 1.0 + 1e-9)) { h = i; break; }
            }
            hw[(size_t)(j + RY)] = (float)h;
        }
    }
    P::Minmax pc = {p.w, p.h, (uint32_t)RY, c.i("Operation") == 1 ? 1u : 0u};
    Pass& pass = p.add("minmax", 0, 1, p.w, p.h);
    pass.table = p.addTable(hw);
    pass.setPush(pc);
    return true;
}

bool planGradientMap(Ctx& c, Plan& p)
{
    P::Gradientmap pc = {p.w, p.h, (float)clampd(c.d("Amount"), 0, 1), c.i("Mode") == 1 ? 1u : 0u};
    p.add("gradientmap", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planMaskGenerator(Ctx& c, Plan& p)
{
    P::Maskgenerator pc = {p.w, p.h, u32(c.i("Type")), c.b("Invert") ? 1u : 0u, (float)c.d("LowIn"), (float)c.d("HighIn"), (float)c.d("LowOut"), (float)c.d("HighOut"), c.levels ? 1u : 0u};
    if (c.i("Type") < 0) return false;
    p.add("maskgenerator", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planBroadcastColors(Ctx& c, Plan& p)
{
    if (c.levels) {          // 8-bit: VEGAS's own table pass (broadcastColors8() in builtins_more_colour.cpp), tables and constants as doubles
        std::vector<float> tab;
        more::broadcastColors8GpuTable(c, tab);
        P::Broadcastcolors8 pc8 = {p.w, p.h};
        Pass& ps = p.add("broadcastcolors8", 0, 1, p.w, p.h);
        ps.setPush(pc8);
        ps.table = p.addTable(std::move(tab));
        return true;
    }
    const bool studio = c.b("StudioRGB16To135");
    const float k1 = studio ? 2.37f / 255.f : 2.55f / 255.f, k0 = studio ? -1.75f / 255.f : 1.f / 255.f;
    auto ire = [&](const char* n) { return (float)c.d(n) * k1 + k0; };
    auto sm = [&](const char* n) { return (float)clampd(c.d(n), 0, 1); };
    P::Broadcastcolors pc = {p.w, p.h, ire("LumaMin"), ire("LumaMax"), sm("LumaMinSmoothness"), sm("LumaMaxSmoothness"),
                             std::max(0.f, (float)c.d("ChromaMax") / 100.f * 0.465f), sm("ChromaSmoothness"), ire("CompositeMin"), ire("CompositeMax"),
                             sm("CompositeMinSmoothness"), sm("CompositeMaxSmoothness")};
    p.add("broadcastcolors", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planChromaBlur(Ctx& c, Plan& p)
{
    if (c.levels) {
        // 8-bit: chromaBlur8() in builtins_more_blur.cpp - three box passes across (widths 1, 1, 2 rh + 1) and three down on U and V, in the CPU's float steps
        int rh, bv[3];
        more::chromaBlur8Widths(c, rh, bv);
        const int rad[6] = {0, 0, rh, (bv[0] - 1) / 2, (bv[1] - 1) / 2, (bv[2] - 1) / 2};
        for (int k = 0; k < 6; ++k)
            if (rad[k] < 0 || 2 * rad[k] + 1 > (int)(k < 3 ? p.w : p.h)) return false;      // (a window wider than the frame: the CPU's reads run off the row)
        const int a = p.addBuf(p.w, p.h, 4), b = p.addBuf(p.w, p.h, 4);
        P::Chroma8Split sp = {p.w, p.h};
        p.add("chroma8_split", 0, a, p.w, p.h).setPush(sp);
        int from = a, to = b;
        for (int k = 0; k < 6; ++k) {
            P::Chroma8BoxH pc = {p.w, p.h, (uint32_t)rad[k], 0u};
            if (k < 3) p.add("chroma8_box_h", from, to, p.h, 1).setPush(pc);
            else p.add("chroma8_box_v", from, to, p.w, 1).setPush(pc);
            std::swap(from, to);
        }
        P::Chroma8Join jn = {p.w, p.h};
        p.add("chroma8_join", from, 1, p.w, p.h).setPush(jn);
        return true;
    }
    const std::vector<float> kx = chromaBlurKernel(false, c.px(clampd(c.d("HorizontalPixels"), 0, 10) + 1.0)), ky = chromaBlurKernel(true, c.px(2.4 * clampd(c.d("VerticalPixels"), 0, 10) + 1.0));
    const int t = p.addBuf(p.w, p.h, 4), t2 = p.addBuf(p.w, p.h, 4), t3 = p.addBuf(p.w, p.h, 4);
    {
        P::ChromaSplit pc = {p.w, p.h};
        p.add("chroma_split", 0, t, p.w, p.h).setPush(pc);
    }
    {
        Pass& pass = p.add("chroma_conv", t, t2, p.w, p.h);
        pass.table = p.addTable(kx);
        P::ChromaConv pc = {p.w, p.h, 0u, (uint32_t)kx.size() / 2};
        pass.setPush(pc);
    }
    {
        Pass& pass = p.add("chroma_conv", t2, t3, p.w, p.h);
        pass.table = p.addTable(ky);
        P::ChromaConv pc = {p.w, p.h, 1u, (uint32_t)ky.size() / 2};
        pass.setPush(pc);
    }
    P::ChromaJoin pc = {p.w, p.h, 0u};
    p.add("chroma_join", t3, 1, p.w, p.h).setPush(pc);
    return true;
}

// ---- transitions: the outgoing frame is buffer 0; the incoming one goes up with the plan as a table of w * h * 4 floats (top row first), as Ctx::src2 holds it
// (builtins.cpp loads it for the plan, which takes it over). Each planner reads the settings as its function in builtins_trans.cpp does.
int toTable(Ctx& c, Plan& p)
{
    if (c.src2.px.size() != (size_t)p.w * p.h * 4) return -1;
    return p.addTable(std::move(c.src2.px));
}

bool planWipeLinear(Ctx& c, Plan& p)
{
    if (c.levels) return false;                 // the 8-bit picture is VEGAS's own whole-number maths on the CPU (builtins_trans.cpp)
    const int t = toTable(c, p);
    if (t < 0) return false;
    const double ang = c.d("Angle") * kPi / 180.0, dx = std::cos(ang), dy = std::sin(ang);
    const double span = std::max(1e-9, std::fabs(dx) * p.w + std::fabs(dy) * p.h);
    const double w = clampd(c.d("Feather"), 0, 1) * 0.5;
    const double edge = c.progress * (1.0 + w);
    P::WipeLinear pc = {p.w, p.h, (float)dx, (float)dy, (float)span, (float)w, (float)edge};
    Pass& ps = p.add("wipe_linear", 0, 1, p.w, p.h);
    ps.table = t;
    ps.setPush(pc);
    return true;
}

bool planWipeClock(Ctx& c, Plan& p)
{
    if (c.levels) return false;                 // the 8-bit picture is VEGAS's own whole-number maths on the CPU (builtins_trans.cpp)
    const int t = toTable(c, p);
    if (t < 0) return false;
    const double F = clampd(c.d("FeatherAngle"), 0, 720);
    const double edge = -F * 0.5 + clampd(c.progress, 0, 1) * (360.0 + F);
    const double er = edge * kPi / 180.0;
    P::WipeClock pc = {p.w, p.h, c.i("Direction") == 1 ? 1u : 0u, 0u, (float)F, (float)edge,
                       (float)-std::cos(er), (float)-std::sin(er), (float)std::sin(er), (float)-std::cos(er)};
    Pass& ps = p.add("wipe_clock", 0, 1, p.w, p.h);
    ps.table = t;
    ps.setPush(pc);
    return true;
}

bool planPushFrames(Ctx& c, Plan& p)
{
    if (c.levels) return false;                 // the 8-bit picture is VEGAS's fixed-point maths on the CPU (builtins_trans.cpp)
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
    float bc[4];
    put(bc, c.color("BorderColor"));
    P::PushFrames pc = {p.w, p.h, horiz ? 1u : 0u, neg ? 1u : 0u, c.b("PushOffPreviousImage") ? 1u : 0u, border ? 1u : 0u, (float)seam, (float)centre, (float)shift, (float)L, (float)R, (float)Wd,
                        bc[0], bc[1], bc[2], bc[3], border && F <= 0 ? 1u : 0u};
    Pass& ps = p.add("push_frames", 0, 1, p.w, p.h);
    ps.table = t;
    ps.setPush(pc);
    return true;
}

bool planBarndoorFrames(Ctx& c, Plan& p)
{
    if (c.levels) return false;     // 8-bit pictures use the exact CPU path
    const int t = toTable(c, p);
    if (t < 0) return false;
    const bool alongX = c.i("Orientation") == 0, in = c.i("Direction") == 0;
    const double L = alongX ? p.w : p.h, T = L * 0.5, pr = clampd(c.progress, 0, 1);
    const double w = clampd(c.d("BorderSize"), 0, 1) * T, bw = w * 0.5, F = clampd(c.d("BorderFeather"), 0, 1) * T;
    const double Wd = std::max(F, 2.0), R = bw + F * 0.5;
    const bool border = w > 0 || F > 0;
    const double cen = in ? -1.5 * w + pr * (T + 3 * w) : T + 1.5 * w - pr * (T + 3 * w);
    const double e = cen - bw;
    float bc[4];
    put(bc, c.color("BorderColor"));
    P::BarndoorFrames pc = {p.w, p.h, alongX ? 1u : 0u, in ? 1u : 0u, border ? 1u : 0u, (w > 0 ? 1u : 0u) | (w > 0 && F <= 0 ? 2u : 0u), (float)T, (float)L, (float)e, (float)cen, (float)R, (float)Wd, bc[0], bc[1], bc[2], bc[3]};
    Pass& ps = p.add("barndoor_frames", 0, 1, p.w, p.h);
    ps.table = t;
    ps.setPush(pc);
    return true;
}


// ---- generators: no input picture (buffer 0 is not filled); the numbers the CPU code works out in double precision come from builtins_gen.cpp
bool planSolidColor(Ctx& c, Plan& p)
{
    float v[4];
    solidColorGpu(c, v);
    P::FillPx pc = {p.w, p.h, v[0], v[1], v[2], v[3]};
    p.add("fill_px", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

// a double as two table words (the bits of its halves, held in floats): the kernels read them with ld_sf
void tabD(std::vector<float>& t, double d)
{
    const D2 b = d2(d);
    float f[2];
    std::memcpy(&f[0], &b.h, 4);
    std::memcpy(&f[1], &b.l, 4);
    t.push_back(f[0]);
    t.push_back(f[1]);
}

bool planColorGradient(Ctx& c, Plan& p)
{
    const Color a = c.color("Color1"), b = c.color("Color2");
    double cx, cy;
    c.xy("Center", cx, cy);
    const double ang = c.d("AspectRatioAngle") * kPi / 180.0;
    const double size = std::max(1e-4, c.d("Size")), aspect = std::max(0.01, c.d("Aspect"));
    std::vector<float> tab;
    const double ds[16] = {cx * p.w, cy * p.h, std::cos(ang), std::sin(ang), size, aspect, aspect * aspect, size * 0.5, a.r, a.g, a.b, a.a, b.r, b.g, b.b, b.a};
    for (double d : ds) tabD(tab, d);
    P::Colorgradient pc = {p.w, p.h, u32(c.i("GradientType"))};
    Pass& ps = p.add("colorgradient", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(tab));
    ps.setPush(pc);
    return true;
}

bool planCheckerboard(Ctx& c, Plan& p)
{
    std::vector<float> tab;
    float pa[4], pb[4];
    checkerboardGpu(c, (int)p.w, (int)p.h, tab, pa, pb);
    P::Checkerboard pc = {p.w, p.h, pa[0], pa[1], pa[2], pa[3], pb[0], pb[1], pb[2], pb[3]};
    Pass& ps = p.add("checkerboard", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(tab));
    ps.setPush(pc);
    return true;
}

bool planTestPattern(Ctx& c, Plan& p)
{
    std::vector<float> tab;
    int type, barW;
    testPatternGpu(c, (int)p.w, (int)p.h, type, barW, tab);
    P::Testpattern pc = {p.w, p.h, u32(type), u32(barW)};
    Pass& ps = p.add("testpattern", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(tab));
    ps.setPush(pc);
    return true;
}


// ---- creative effects (builtins_more_creative.cpp)
bool planCrop(Ctx& c, Plan& p)
{
    double lx, ly;
    c.xy("Location", lx, ly);
    const double xs = clampd(c.d("XScale"), 0, 1), ys = clampd(c.d("YScale"), 0, 1), ang = c.d("Angle") * kPi / 180.0;
    const double W = p.w, H = p.h;
    P::Crop pc = {p.w, p.h, (float)(lx * W), (float)((1 - ly) * H), (float)std::cos(ang), (float)std::sin(ang), (float)(xs * W * 0.5), (float)(ys * H * 0.5),
                  (float)clampd(c.d("Opacity"), 0, 1)};
    p.add("crop", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planFilmGrain(Ctx& c, Plan& p)
{
    const double amount0 = clampd(c.d("Amount"), 0, 1), gran = clampd(c.d("Granularity"), 0, 1), chroma = clampd(c.d("Chormaticity"), 0, 1);
    const double fs = std::max(1.0, (double)p.h) / 1080.0, fsFull = std::max(1.0, (double)p.h) / c.scale / 1080.0;
    const double cellFull = (double)std::max(1, (int)std::lround(1 + gran * 3 * std::max(1.0, fsFull * 2)));
    const double cellHere = cellFull * c.scale;
    const int cell = c.scale >= 0.999 ? std::max(1, (int)std::lround(1 + gran * 3 * std::max(1.0, fs * 2))) : std::max(1, (int)std::lround(cellHere));
    const double weaken = c.scale >= 0.999 ? 1.0 : std::min(1.0, cellHere);
    const double amount = amount0 * weaken;
    const uint32_t seed = (uint32_t)std::lround(c.time) * 2654435761u;
    P::Filmgrain pc = {p.w, p.h, (uint32_t)cell, seed, (float)amount, (float)(amount * chroma)};
    p.add("filmgrain", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planFilmEffects(Ctx& c, Plan& p)
{
    const double grain = clampd(c.d("Grain"), 0, 1), tint = clampd(c.d("Tint"), 0, 1);
    const Color col = c.color("Color");
    const double amount = clampd(c.d("EnableParticlesAmount"), 0, 1);
    const uint32_t frame = (uint32_t)std::lround(c.time);
    auto cellOf = [&](int full) { return (uint32_t)std::max(1, (int)std::lround(full * c.scale)); };
    P::Filmeffects pc = {p.w, p.h, frame, c.b("ConvertToGreyscale") ? 1u : 0u, c.b("EnableEnableParticles") ? 1u : 0u, u32(c.i("EnableParticlesType")), cellOf(2), cellOf(3), cellOf(4), cellOf(40),
                         (float)tint, (float)col.r, (float)col.g, (float)col.b, (float)grain, (float)c.scale, (float)(amount * 0.01), (float)(1.0 - amount * 0.0015), (float)(1.0 - amount * 0.0006), (float)amount};
    p.add("filmeffects", 0, 1, p.w, p.h).setPush(pc);
    return true;
}


// Bump Map (bumpMapRender in builtins_more_creative.cpp: the same float expressions)
bool planBumpMapNamed(Ctx& c, Plan& p, const char* srcName, const char* dstName)
{
    const int W = (int)p.w, H = (int)p.h;
    double psx, psy, psz, pdx, pdy;
    c.xyz(srcName, psx, psy, psz);
    c.xy(dstName, pdx, pdy);
    const int type = std::max(0, std::min(2, c.i("Type")));
    const int chan = std::max(0, std::min(3, c.i("BumpChannel")));
    const float sx = (float)psx, sy = (float)-psy, sz = (float)psz;
    float dx = (float)pdx, dy = (float)-pdy;
    if (type != 1) dx = dy = 0.f;
    float ax = sx - dx, ay = sy - dy, az = sz;
    {
        const float l = std::sqrt(ax * ax + ay * ay + az * az);
        if (l > 1e-7f) { ax /= l; ay /= l; az /= l; }
        else { ax = 0.f; ay = 0.f; az = 1.f; }
    }
    const float mx = (float)std::max(W, H);
    const double bh = c.d("BumpHeight");
    const float hs = (float)((double)((float)(bh * bh) * 0.0625f) * ((double)mx * 0.0015625)) * (c.b("WhiteIsHigh") ? 1.f : -1.f) * 255.f;
    const float amb2 = (float)c.d("Ambience") * 2.f, i8 = (float)c.d("Intensity") * 8.f;
    const float shin = (float)c.d("Shininess");
    const float kd = 0.6f - shin * 0.2f, ks = shin * 0.5f, power = shin * 8.f + 4.f;
    const float cosCut = std::cos((float)c.d("Focus") * 0.7853981852531433f);
    const float invCut = 1.f / std::max(1.f - cosCut, 1e-5f);
    P::Bumpmap pc = {p.w, p.h, (uint32_t)type, (uint32_t)chan, sx, sy, sz, ax, ay, az, hs, amb2, i8, kd, ks, power, cosCut, invCut, 2.f / mx, W * 0.5f, H * 0.5f};
    p.add("bumpmap", 0, 1, p.w, p.h).setPush(pc);
    return true;
}
bool planBumpMap(Ctx& c, Plan& p) { return planBumpMapNamed(c, p, "LocationSrc", "LocationDst"); }
// the compositing plug-in's Bump Map (builtins_mask.cpp): the same render, other setting names
bool planBumpMapGeneral(Ctx& c, Plan& p) { return planBumpMapNamed(c, p, "DualLocation3DAnd2DSrc", "DualLocation3DAnd2DDst"); }


bool planLutFilter(Ctx& c, Plan& p)
{
    int n = 0;
    float dmin[3], dmax[3], gain = 0;
    bool tetra = false;
    std::vector<float> tab;
    if (!lutFilterGpu(c, n, dmin, dmax, gain, tetra, tab) || n < 2 || tab.size() < (size_t)n * n * n * 3) {
        addCopy(p, 0, 1);
        return true;
    }
    P::Lutfilter pc = {p.w, p.h, (uint32_t)n, tetra ? 1u : 0u, gain, dmin[0], dmin[1], dmin[2], dmax[0], dmax[1], dmax[2]};
    Pass& ps = p.add("lutfilter", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(tab));
    ps.setPush(pc);
    return true;
}

bool planColorGrading(Ctx& c, Plan& p)
{
    ColorGradingGpu g;
    if (!colorGradingGpu(c, g)) {
        addCopy(p, 0, 1);
        return true;
    }
    P::Colorgrading pc = {p.w, p.h, g.flags, g.f[0], g.f[1], g.f[2], g.f[3], g.f[4], g.f[5], g.f[6], g.f[7], g.f[8], g.f[9], g.f[10], g.f[11], g.f[12], g.f[13], g.f[14], g.f[15], g.f[16], g.f[17], g.f[18], g.f[19], g.f[20]};
    Pass& ps = p.add("colorgrading", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(g.tab));
    ps.setPush(pc);
    return true;
}


bool planColorMatch(Ctx& c, Plan& p)
{
    ColorMatchGpu g;
    if (!colorMatchGpu(c, (int)p.w, (int)p.h, g)) {
        addCopy(p, 0, 1);
        return true;
    }
    const int part = p.addBuf(2, 90, 4), params = p.addBuf(4, 1, 4);
    const int tRows = p.addTable(std::move(g.rowsTable)), tParams = p.addTable(std::move(g.paramTable));
    {
        P::ColormatchRows pc = {p.w, p.h};
        Pass& ps = p.add("colormatch_rows", 0, part, 1, 90);
        ps.table = tRows;
        ps.setPush(pc);
    }
    {
        P::ColormatchParams pc = {p.w, p.h};
        Pass& ps = p.add("colormatch_params", part, params, 1, 1);
        ps.table = tParams;
        ps.setPush(pc);
    }
    P::Colormatch pc = {p.w, p.h};
    Pass& ps = p.add("colormatch", 0, 1, p.w, p.h);
    ps.in1 = params;
    ps.table = tRows;
    ps.setPush(pc);
    return true;
}


// Displacement Map, Height Map (builtins_vegasrest.cpp) and Mesh Warp (builtins_magix.cpp)
bool planDisplacementMap(Ctx& c, Plan& p)
{
    const D2 SH = d2(c.d("ScaleHorizontal") * (double)p.w), SV = d2(c.d("ScaleVertical") * (double)p.h);
    P::Displacementmap pc = {p.w, p.h, SH.h, SH.l, SV.h, SV.l, c.i("EdgePixelHandling") == 1 ? 1u : 0u,
                             (uint32_t)std::min(2, std::max(0, c.i("ChannelsXaxisChannel"))), (uint32_t)std::min(2, std::max(0, c.i("ChannelsYaxisChannel"))),
                             c.b("ChannelsXaxisInvert") ? 1u : 0u, c.b("ChannelsYaxisInvert") ? 1u : 0u, c.b("AlphaChannelScalesRGBChannels") ? 1u : 0u, c.b("FlipInputSurfaces") ? 1u : 0u};
    p.add("displacementmap", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planHeightMap(Ctx& c, Plan& p)
{
    const double W = p.w, H = p.h;
    const double amp = c.d("HeightModifiersAmplitude"), elev = c.d("HeightModifiersElevation"), scl = c.d("HeightModifiersHeightScale");
    const int plane = p.addBuf(p.w, p.h, 1);
    {
        P::Heightplane pc = {p.w, p.h, u32(c.i("SourceChannel")), c.b("InvertDataSource") ? 1u : 0u, c.b("AlphaChannelScalesRGBChannels") ? 1u : 0u, c.b("FlipInputSurfaces") ? 1u : 0u};
        p.add("heightplane", 0, plane, p.w, p.h).setPush(pc);
    }
    const double sigma = std::max(0, c.i("Smoothness")) * 0.001 * W;
    int blurred = plane;
    if (sigma > 0.3) {
        blurred = p.addBuf(p.w, p.h, 1);
        addBlur(p, plane, blurred, sigma, sigma, 0, true);
    }
    const D2 LIFT = d2(amp * scl * 0.2 * H), BASE = d2(elev * scl * 0.2 * H);
    P::Heightmap pc = {p.w, p.h, LIFT.h, LIFT.l, BASE.h, BASE.l, c.i("EdgePixelHandling") == 1 ? 1u : 0u, 0u};
    Pass& ps = p.add("heightmap", 0, 1, p.w, p.h);
    ps.in1 = blurred;
    ps.setPush(pc);
    return true;
}

bool planMeshWarp(Ctx& c, Plan& p)
{
    int cols = 0, rows = 0;
    std::vector<float> pts;
    const int mode = meshWarpGpu(c, cols, rows, pts);
    if (mode == 0) return false;
    if (mode == 1) {
        addCopy(p, 0, 1);
        return true;
    }
    P::Meshwarp pc = {p.w, p.h, (uint32_t)cols, (uint32_t)rows};
    Pass& ps = p.add("meshwarp", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(pts));
    ps.setPush(pc);
    return true;
}


// TV Simulator (tvSimulator in builtins_more_creative.cpp)
bool planTvSimulator(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    const double zoom = 1.0 + clampd(c.d("DetailZoom"), 0, 1) * 1.5, grill = clampd(c.d("ApertureGrill"), 0, 1), inter = clampd(c.d("Interlacing"), 0, 1);
    const double lineSync = clampd(c.d("LineSync"), 0, 1), vSync = clampd(c.d("VerticalSync"), 0, 1), scan = clampd(c.d("ScanPhasing"), 0, 1);
    const double phos = clampd(c.d("Phosphorescence"), 0, 1), stat = clampd(c.d("Static"), 0, 1);
    const double t = c.time;
    if (!(t >= 0 && t < 1e8)) return false;
    auto rnd = [&](uint32_t a, uint32_t b) {
        uint32_t h = a * 374761393u + b * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return ((h ^ (h >> 16)) & 0xFFFF) / 65535.f;
    };
    std::vector<float> tab((size_t)H * 2);
    for (int y = 0; y < H; ++y) {
        const double yFull = y / c.scale;
        const double wob = (1.0 - lineSync) * 0.03 * W * std::sin(yFull * 0.07 + t * 0.9) * (rnd((uint32_t)yFull, (uint32_t)t) > 0.8 ? 3 : 1);
        tab[(size_t)y] = (float)wob;
        tab[(size_t)H + (size_t)y] = 1.f - (float)scan * 0.25f * (0.5f + 0.5f * (float)std::sin(yFull * 0.02 - t * 0.4));
    }
    const double roll = (1.0 - vSync) * H * std::fmod(t * 0.07, 1.0);
    int glow = 0;
    if (phos > 0.01) {
        const double sig = 2.0 * (std::max(1.0, (double)H) / 1080.0) + c.px(1);
        glow = addBlur(p, 0, p.addBuf(p.w, p.h, 4), sig, sig, 15u, false);
    }
    P::Tvsimulator pc = {p.w, p.h, (float)zoom, (float)roll, (float)phos, (float)grill, (float)inter, (float)scan, (float)stat, (float)c.scale, c.scale >= 0.999 ? 1u : 0u, (uint32_t)(t * 7)};
    Pass& ps = p.add("tvsimulator", 0, 1, p.w, p.h);
    ps.in1 = glow;
    ps.table = p.addTable(std::move(tab));
    ps.setPush(pc);
    return true;
}


// Lens Flare (lensFlare in builtins_lensflare.cpp)
bool planLensFlare(Ctx& c, Plan& p)
{
    std::vector<float> tab;
    int n = 0;
    float blend = 0.f;
    const int mode = more::lensFlareGpu(c, (int)p.w, (int)p.h, tab, n, blend);
    if (mode == 0) return false;
    if (mode == 1) {
        addCopy(p, 0, 1);
        return true;
    }
    P::Lensflare pc = {p.w, p.h, (uint32_t)n, blend};
    Pass& ps = p.add("lensflare", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(tab));
    ps.setPush(pc);
    return true;
}


struct Entry { const char* id; bool (*plan)(Ctx&, Plan&); };
// (no entries, so the CPU renders them: sepia, add noise and white balance - their 8-bit steps, the shift-register generator and the frame histogram follow
// VEGAS exactly in builtins_filters.cpp / builtins_more_colour.cpp)
const Entry kEntries[] = {
    {"solidcolor", planSolidColor},
    {"colorgradient", planColorGradient},
    {"checkerboard", planCheckerboard},
    {"testpattern", planTestPattern},
    {"crop", planCrop},
    {"filmgrain", planFilmGrain},
    {"filmeffects", planFilmEffects},
    {"bumpmapfilter", planBumpMap},
    {"bumpmap", planBumpMapGeneral},
    {"lutfilter", planLutFilter},
    {"colorgrading", planColorGrading},
    {"colormatch", planColorMatch},
    {"displacementmap", planDisplacementMap},
    {"heightmap", planHeightMap},
    {"de.magix:MeshWarp", planMeshWarp},
    {"tvsimulator", planTvSimulator},
    {"lensflare", planLensFlare},
    {"invert", planInvert},
    {"brightnessandcontrast", planBrightnessContrast},
    {"blackandwhite", planBlackAndWhite},
    {"threshold", planThreshold},
    {"colorbalance", planColorBalance},
    {"hsladjust", planHslAdjust},
    {"levels", planLevels},
    {"colorcurves", planColorCurves},
    {"colorcorrectorsecondary", planColorCorrectorSecondary},
    {"gaussianblur", planGaussianBlur},
    {"sharpen", planSharpen},
    {"mirror", planMirror},
    {"colorcorrector", planColorCorrector},
    {"saturationadjust", planSaturationAdjust},
    {"blackrestore", planBlackRestore},
    {"channelblend", planChannelBlend},
    {"unsharpmask", planUnsharpMask},
    {"quickblur", planQuickBlur},
    {"pixelate", planPixelate},
        // (Spherize: on the CPU, see the note above planSwirl)
    {"swirl", planSwirl},
    {"pinchpunch", planPinchPunch},
    {"wave", planWave},
    // (Border has no Vulkan plan: VEGAS's bevel faces, solid frame and blurred frame are worked out on the CPU, builtins_filters.cpp border())
    {"minandmax", planMinMax},
    {"gradientmap", planGradientMap},
    {"maskgenerator", planMaskGenerator},
    {"broadcastcolors", planBroadcastColors},
    {"chromablur", planChromaBlur},
    {"linearwipe", planWipeLinear},
    {"clockwipe", planWipeClock},
    {"push", planPushFrames},
    {"barndoor", planBarndoorFrames},
    // (Slide has no Vulkan plan now: VEGAS's 18.14 fixed-point blit and border are worked out on the CPU, builtins_trans.cpp sf::moveIn)
};

// ---------------------------------------------------------------------------------------------------- check cases
typedef std::map<std::string, std::vector<double>> PM;
typedef std::map<std::string, std::string> SM;
struct CaseSet { const char* id; std::vector<GpuCheckCase> cases; };

// A Color Curves setting (the blob readCurves reads): per curve a list of points x, y with their handles.
std::string curvesBlob(const std::vector<std::vector<std::pair<double, double>>>& curves)
{
    std::string b(20, '\0');
    const int32_t one = 1;
    std::memcpy(&b[0], &one, 4);
    for (size_t k = 0; k < 4; ++k) {
        const int32_t n = k < curves.size() ? (int32_t)curves[k].size() : 0;
        std::memcpy(&b[4 + 4 * k], &n, 4);
    }
    for (size_t k = 0; k < curves.size() && k < 4; ++k)
        for (const auto& pt : curves[k]) {
            double rec[13] = {};
            rec[0] = pt.first; rec[1] = pt.second;
            rec[5] = pt.first - 0.1; rec[6] = pt.second - 0.1;
            rec[10] = pt.first + 0.1; rec[11] = pt.second + 0.1;
            b.append((const char*)rec, 104);
        }
    static const char* const T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < b.size(); i += 3) {
        const uint32_t v = ((uint8_t)b[i] << 16) | (i + 1 < b.size() ? (uint8_t)b[i + 1] << 8 : 0) | (i + 2 < b.size() ? (uint8_t)b[i + 2] : 0);
        o += T[(v >> 18) & 63];
        o += T[(v >> 12) & 63];
        o += i + 1 < b.size() ? T[(v >> 6) & 63] : '=';
        o += i + 2 < b.size() ? T[v & 63] : '=';
    }
    return o;
}


// A small colour table in a file for the LUT checks (a 3D .cube with some bends, and a 1D one), written once to the temporary folder
std::string testCubeFile(bool oneD)
{
    static std::string paths[2];
    std::string& path = paths[oneD ? 1 : 0];
    if (path.empty()) {
        std::string dir = ".";
        try { dir = std::filesystem::temp_directory_path().string(); } catch (...) {}
        path = dir + (dir.empty() || dir.back() == '/' || dir.back() == '\\' ? "" : "/") + (oneD ? "reno_vkcheck_1d.cube" : "reno_vkcheck_3d.cube");
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            if (oneD) {
                std::fprintf(f, "TITLE \"vkcheck\"\nLUT_1D_SIZE 16\n");
                for (int i = 0; i < 16; ++i) { const double t = i / 15.0; std::fprintf(f, "%.6f %.6f %.6f\n", std::pow(t, 0.8), t * t, 1.0 - 0.5 * t); }
            } else {
                const int n = 9;
                std::fprintf(f, "TITLE \"vkcheck\"\nLUT_3D_SIZE %d\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n", n);
                for (int b = 0; b < n; ++b)
                    for (int g = 0; g < n; ++g)
                        for (int r = 0; r < n; ++r) {
                            const double R = r / (n - 1.0), G = g / (n - 1.0), B = b / (n - 1.0);
                            std::fprintf(f, "%.6f %.6f %.6f\n", std::pow(R, 0.8) * 0.9 + 0.1 * G, G * (1.0 - 0.3 * B) + 0.05, 1.0 - 0.6 * R * B - 0.2 * G);
                        }
            }
            std::fclose(f);
        }
    }
    return path;
}


// "internaldata" of Color Match: the statistics as 12 little-endian doubles in base64
std::string statsBlobBytes(const std::string& b)
{
    static const char* const T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < b.size(); i += 3) {
        const uint32_t x = ((uint8_t)b[i] << 16) | (i + 1 < b.size() ? (uint8_t)b[i + 1] << 8 : 0) | (i + 2 < b.size() ? (uint8_t)b[i + 2] : 0);
        o += T[(x >> 18) & 63];
        o += T[(x >> 12) & 63];
        o += i + 1 < b.size() ? T[(x >> 6) & 63] : '=';
        o += i + 2 < b.size() ? T[x & 63] : '=';
    }
    return o;
}
std::string statsBlob(const std::vector<double>& v) { return statsBlobBytes(std::string((const char*)v.data(), v.size() * 8)); }

// Mesh Warp's "WarpData": columns, rows, two more numbers, then x y (floats) per point
std::string meshBlob(int cols, int rows, float bend)
{
    std::string b(16, '\0');
    const int32_t hdr[4] = {cols, rows, 0, 0};
    std::memcpy(&b[0], hdr, 16);
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols; ++i) {
            float xy[2] = {(float)i / (cols - 1) + bend * std::sin(0.7f * j + 0.3f), (float)j / (rows - 1) + bend * std::cos(0.9f * i)};
            b.append((const char*)xy, 8);
        }
    return statsBlobBytes(b);
}

const std::vector<CaseSet>& caseSets()
{
    static const std::vector<CaseSet> sets = {
        {"colorgradient", {{"elliptical", PM{{"GradientType", {1}}, {"Aspect", {1.7}}, {"AspectRatioAngle", {25}}, {"Center", {0.4, 0.6}}, {"Color1", {1.0, 0.3, 0.1, 0.8}}}},
                           {"rectangular", PM{{"GradientType", {2}}, {"Size", {0.7}}, {"Color2", {0.1, 0.4, 1.0, 1.0}}}}}},
        {"checkerboard", {{"soft", PM{{"EdgeBlendingHorizontal", {0.3}}, {"EdgeBlendingVertical", {0.5}}, {"GridOffsetX", {0.13}}, {"TileDimensionsWidth", {0.07}}, {"TileDimensionsHeight", {0.11}}, {"TileDimensionsSquare", {0}}}},
                          {"hard", PM{{"GridOffsetX", {0.31}}, {"GridOffsetY", {0.2}}, {"Color1", {1.0, 0.2, 0.2, 1.0}}, {"Color2", {0.0, 0.3, 0.9, 0.6}}}}}},
        {"testpattern", {{"t1", PM{{"TestPatternType", {1}}}}, {"t2", PM{{"TestPatternType", {2}}}}, {"t3", PM{{"TestPatternType", {3}}}}, {"t4", PM{{"TestPatternType", {4}}}},
                         {"t5", PM{{"TestPatternType", {5}}}}, {"t6", PM{{"TestPatternType", {6}}}}, {"t7", PM{{"TestPatternType", {7}}}}, {"t8", PM{{"TestPatternType", {8}}}}}},
        {"crop", {{"turned", PM{{"XScale", {0.6}}, {"YScale", {0.7}}, {"Angle", {23}}, {"Location", {0.4, 0.55}}, {"Opacity", {0.8}}}}, {"whole", PM{{"XScale", {1.0}}, {"YScale", {1.0}}}}}},
        {"filmgrain", {{"coarse", PM{{"Amount", {0.8}}, {"Granularity", {0.9}}, {"Chormaticity", {0.7}}}}, {"fine", PM{{"Amount", {0.3}}, {"Granularity", {0.0}}}}}},
        {"filmeffects", {{"grey", PM{{"ConvertToGreyscale", {1}}, {"Tint", {0.6}}, {"Color", {0.9, 0.7, 0.4, 1.0}}, {"Grain", {0.7}}}},
                         {"dust", PM{{"EnableEnableParticles", {1}}, {"EnableParticlesType", {0}}, {"EnableParticlesAmount", {1.0}}}},
                         {"flicker", PM{{"EnableEnableParticles", {1}}, {"EnableParticlesType", {1}}, {"EnableParticlesAmount", {0.8}}}},
                         {"hair", PM{{"EnableEnableParticles", {1}}, {"EnableParticlesType", {2}}, {"EnableParticlesAmount", {1.0}}}},
                         {"scratch", PM{{"EnableEnableParticles", {1}}, {"EnableParticlesType", {4}}, {"EnableParticlesAmount", {1.0}}}}}},
        {"bumpmap", {{"spot", PM{{"Type", {1}}, {"Intensity", {0.5}}, {"Shininess", {0.7}}, {"BumpHeight", {0.6}}, {"BumpChannel", {2}}}},
                     {"omni", PM{{"Type", {2}}, {"Intensity", {0.4}}, {"Ambience", {0.2}}, {"BumpHeight", {0.9}}, {"WhiteIsHigh", {0}}, {"DualLocation3DAnd2DSrc", {0.3, -0.4, 1.5}}}},
                     {"directional", PM{{"Type", {0}}, {"BumpChannel", {0}}}}}},
        {"bumpmapfilter", {{"spot", PM{{"Type", {1}}, {"Intensity", {0.5}}, {"Shininess", {0.7}}, {"BumpHeight", {0.6}}, {"BumpChannel", {2}}}},
                           {"omni", PM{{"Type", {2}}, {"Intensity", {0.4}}, {"Ambience", {0.2}}, {"BumpHeight", {0.9}}, {"WhiteIsHigh", {0}}, {"LocationSrc", {0.3, -0.4, 1.5}}}},
                           {"dir", PM{{"Type", {0}}, {"Intensity", {0.7}}, {"Shininess", {0.3}}, {"BumpHeight", {0.3}}, {"BumpChannel", {3}}}}}},
        {"deform", {{"squeeze", PM{{"Amount", {0.8}}, {"SqueezeLeft", {0.3}}, {"SqueezeTop", {0.2}}, {"SqueezeRight", {-0.2}}, {"SqueezeBottom", {0.5}}}},
                    {"shear", PM{{"Amount", {1.0}}, {"ShearHorizontal", {1.4}}, {"ShearVertical", {0.3}}, {"Background", {0.2, 0.5, 0.3, 0.7}}}},
                    {"curve", PM{{"Amount", {1.0}}, {"CurveLeft", {0.4}}, {"CurveRight", {-0.3}}, {"CurveTop", {0.5}}, {"CurveBottom", {0.2}}}}}},
        {"lutfilter", {{"linear", PM{{"Gain", {0.8}}}, SM{{"LUTFilename", testCubeFile(false)}}}, {"tetra", PM{{"Interpolation", {1}}}, SM{{"LUTFilename", testCubeFile(false)}}},
                       {"1d", PM{{"Interpolation", {1}}, {"Gain", {0.6}}}, SM{{"LUTFilename", testCubeFile(true)}}}, {"no file", PM{}, SM{{"LUTFilename", "/nonexistent.cube"}}}}},
        {"colorgrading", {{"wheels", PM{{"Gain_R", {1.3}}, {"Lift_Y", {0.05}}, {"Gamma_G", {1.4}}, {"Offset_B", {0.02}}, {"ExposureAdjust", {0.4}}}},
                          {"sat hue", PM{{"SaturationAdjust", {1.5}}, {"HueAdjust", {40}}, {"SatAdjust", {1.2}}, {"ValAdjust", {0.9}}}},
                          {"levels", PM{{"InMinAdjust", {0.1}}, {"InMaxAdjust", {0.9}}, {"OutMinAdjust", {0.05}}, {"OutMaxAdjust", {0.95}}}},
                          {"curves", PM(), SM{{"Curve_R", "2:0.0:0.1:-0.2:0.0:0.2:0.2:1.0:0.9:-0.2:-0.2:0.2:0.0"}, {"Curve_Y", "3:0.0:0.0:-0.2:0.0:0.2:0.0:0.5:0.65:-0.1:-0.1:0.1:0.1:1.0:1.0:-0.2:-0.2:0.2:0.0"}}},
                          {"luts", PM{{"LUTEnable_0", {1}}, {"LUTGain_0", {0.7}}, {"LUTEnable_1", {1}}, {"LUTInterpolation_1", {1}}, {"Gain_G", {1.2}}},
                           SM{{"LUTFilename_0", testCubeFile(false)}, {"LUTFilename_1", testCubeFile(true)}}}}},
        {"colormatch", {{"frame stats", PM{{"SourceCaptured", {1}}, {"Strength", {0.8}}}, SM{{"internaldata", statsBlob({55, 20, 5, 15, -3, 12, 0, 0, 0, 0, 0, 0})}}},
                        {"both", PM{{"SourceCaptured", {1}}, {"TargetCaptured", {1}}, {"Processlightness", {1}}}, SM{{"internaldata", statsBlob({35, 12, -4, 9, 8, 14, 60, 25, 2, 10, 4, 8})}}}}},
        {"displacementmap", {{"scaled", PM{{"ScaleHorizontal", {0.05}}, {"ScaleVertical", {0.03}}, {"ChannelsXaxisInvert", {1}}}}, {"wrap", PM{{"ScaleHorizontal", {0.2}}, {"ScaleVertical", {0.2}}, {"EdgePixelHandling", {1}}, {"AlphaChannelScalesRGBChannels", {1}}, {"FlipInputSurfaces", {1}}, {"ChannelsXaxisChannel", {1}}, {"ChannelsYaxisChannel", {2}}}}}},
        {"heightmap", {{"lift", PM{{"HeightModifiersAmplitude", {1.0}}, {"HeightModifiersHeightScale", {1.0}}, {"HeightModifiersElevation", {0.2}}}}, {"wrap", PM{{"HeightModifiersAmplitude", {1.5}}, {"HeightModifiersHeightScale", {1.0}}, {"EdgePixelHandling", {1}}, {"Smoothness", {5}}, {"SourceChannel", {2}}, {"InvertDataSource", {1}}}}}},
        {"de.magix:MeshWarp", {{"bent", PM{{"EnableWarp", {1}}, {"WarpDirection", {0}}}, SM{{"WarpData", meshBlob(5, 4, 0.04f)}}}}},
        {"lensflare", {{"t0", PM{{"Intensity", {0.8}}}}, {"t1", PM{{"Intensity", {1.0}}, {"LensType", {1}}, {"Tint", {0.5}}, {"LightColor", {1.0, 0.7, 0.3}}, {"Perspective", {0.4}}}},
                       {"t2", PM{{"Intensity", {1.0}}, {"LensType", {2}}, {"LightPosition", {-0.3, 0.4}}, {"Blend", {0.6}}}},
                       {"t3", PM{{"Intensity", {1.2}}, {"LensType", {3}}, {"Size", {0.7}}}}, {"t4", PM{{"Intensity", {1.0}}, {"LensType", {4}}, {"LightPosition", {0.2, -0.1}}}}}},
        {"tvsimulator", {{"all", PM{{"DetailZoom", {0.4}}, {"ApertureGrill", {0.7}}, {"Interlacing", {0.6}}, {"LineSync", {0.5}}, {"VerticalSync", {0.6}}, {"ScanPhasing", {0.8}}, {"Phosphorescence", {0.5}}, {"Static", {0.4}}}},
                         {"plain", PM{{"ApertureGrill", {0.0}}, {"Phosphorescence", {0.0}}, {"Static", {0.0}}}}}},
        {"invert", {{"half", PM{{"BlendAmount", {0.5}}}}, {"off", PM{{"BlendAmount", {0.0}}}}, {"full", PM{{"BlendAmount", {1.0}}}}}},
        {"brightnessandcontrast", {{"brighter", PM{{"Brightness", {0.2}}, {"Contrast", {0.3}}}}, {"darker", PM{{"Brightness", {-0.3}}, {"Contrast", {-0.4}}}},
                                   {"harsh", PM{{"Contrast", {0.95}}, {"ContrastCenter", {0.4}}}}}},
        {"blackandwhite", {{"half", PM{{"BlendAmount", {0.5}}}}}},
        {"threshold", {{"low", PM{{"Threshold", {0.2}}}}, {"high", PM{{"Threshold", {0.8}}}}}},
        {"colorbalance", {{"cast", PM{{"Red", {0.3}}, {"Green", {-0.2}}, {"Blue", {0.1}}}}, {"cast1", PM{{"Red", {-0.5}}, {"Blue", {0.4}}, {"Type", {1}}}},
                          {"cast2", PM{{"Red", {0.4}}, {"Type", {2}}, {"PreserveLuminosity", {1}}}}}},
        {"hsladjust", {{"hue", PM{{"AddToHue", {0.37}}}}, {"sat", PM{{"Saturation", {1.6}}, {"Luminance", {0.7}}}}, {"grey", PM{{"Saturation", {0.0}}, {"AddToHue", {0.9}}}},
                        {"full", PM{{"AddToHue", {1.0}}, {"Saturation", {2.0}}}}, {"half", PM{{"AddToHue", {0.5}}, {"Luminance", {1.8}}}}, {"edge", PM{{"AddToHue", {0.0}}, {"Saturation", {5.0}}, {"Luminance", {0.2}}}}}},
        {"levels", {{"stretch", PM{{"InputStart", {0.1}}, {"InputEnd", {0.8}}}}, {"gamma", PM{{"Gamma", {2.0}}, {"OutputStart", {0.1}}, {"OutputEnd", {0.9}}}},
                    {"red", PM{{"Channel", {1}}, {"Gamma", {0.6}}, {"InputStart", {0.05}}}}}},
        {"colorcurves", {{"master", PM(), SM{{"Property", curvesBlob({{}, {}, {}, {{0, 0}, {0.5, 0.65}, {1, 1}}})}}},
                         {"channels", PM(), SM{{"Property", curvesBlob({{{0, 0.1}, {1, 0.9}}, {{0, 0}, {0.3, 0.5}, {1, 1}}, {{0, 0}, {1, 0.6}}, {{0, 0}, {1, 1}}})}}}}},
        {"colorcorrectorsecondary",
         {{"rotate", PM{{"RotateHue", {90}}, {"Saturation", {1.5}}, {"Gamma", {1.4}}, {"Gain", {1.2}}, {"Offset", {10}}}},
          {"hue range", PM{{"LimitHueCenter", {20}}, {"LimitHueWidth", {80}}, {"LimitHueSmooth", {30}}, {"RotateHue", {120}}}},
          {"limits", PM{{"LimitLuminanceLow", {60}}, {"LimitLuminanceHigh", {200}}, {"LimitLuminanceSmooth", {20}}, {"LimitSaturationLow", {20}},
                        {"LimitSaturationHigh", {120}}, {"LimitSaturationSmooth", {15}}, {"Saturation", {0.2}}}},
          {"mask", PM{{"ShowMask", {1}}, {"LimitHueCenter", {100}}, {"LimitHueWidth", {120}}, {"LimitHueSmooth", {40}}}},
          {"inverted", PM{{"InvertMask", {1}}, {"LimitHueCenter", {100}}, {"LimitHueWidth", {120}}, {"Alpha", {0.5}}, {"Chrominance", {200, 0.5}}}},
          {"lum only", PM{{"LimitLuminanceLow", {40}}, {"LimitLuminanceHigh", {180}}, {"LimitLuminanceSmooth", {25}}, {"Saturation", {1.4}}, {"Gamma", {1.2}}}},
          {"lum chroma", PM{{"LimitLuminanceLow", {80}}, {"LimitLuminanceHigh", {200}}, {"LimitLuminanceSmooth", {10}}, {"Chrominance", {200, 0.7}}}},
          {"lum invert", PM{{"InvertMask", {1}}, {"LimitLuminanceLow", {60}}, {"LimitLuminanceHigh", {160}}, {"LimitLuminanceSmooth", {10}}, {"Gain", {1.3}}, {"Offset", {-8}}}},
          {"sat only", PM{{"EnableLimitLuminance", {0}}, {"EnableLimitSaturation", {1}}, {"LimitSaturationLow", {10}}, {"LimitSaturationHigh", {90}}, {"LimitSaturationSmooth", {10}}, {"RotateHue", {40}}}},
          {"show lum", PM{{"ShowMask", {1}}, {"LimitLuminanceLow", {50}}, {"LimitLuminanceHigh", {150}}, {"LimitLuminanceSmooth", {30}}}},
          {"show inverted", PM{{"ShowMask", {1}}, {"InvertMask", {1}}, {"EnableLimitHue", {1}}, {"LimitHueCenter", {40}}, {"LimitHueWidth", {90}}, {"LimitHueSmooth", {20}}}},
          {"alpha", PM{{"Alpha", {0.4}}, {"LimitLuminanceLow", {100}}, {"LimitLuminanceHigh", {255}}, {"LimitLuminanceSmooth", {30}}}},
          {"nothing to apply", PM{{"LimitLuminanceLow", {70}}, {"LimitLuminanceHigh", {140}}, {"LimitLuminanceSmooth", {5}}}},
          {"hard window", PM{{"LimitLuminanceLow", {100}}, {"LimitLuminanceHigh", {100}}, {"LimitLuminanceSmooth", {0}}, {"Saturation", {2}}}},
          {"all limits", PM{{"LimitLuminanceLow", {30}}, {"LimitLuminanceHigh", {220}}, {"LimitLuminanceSmooth", {12}}, {"EnableLimitSaturation", {1}}, {"LimitSaturationLow", {5}}, {"LimitSaturationHigh", {100}},
                            {"LimitSaturationSmooth", {8}}, {"EnableLimitHue", {1}}, {"LimitHueCenter", {200}}, {"LimitHueWidth", {200}}, {"LimitHueSmooth", {25}}, {"RotateHue", {-60}}, {"Saturation", {1.3}},
                            {"Alpha", {0.8}}, {"Gamma", {0.8}}}}}},
        {"gaussianblur", {{"wide", PM{{"HorizontalRange", {1.5}}, {"VerticalRange", {0.4}}}}, {"onlyx", PM{{"HorizontalRange", {0.8}}, {"VerticalRange", {0.0}}}},
                          {"channels", PM{{"HorizontalRange", {0.6}}, {"VerticalRange", {0.6}}, {"R", {0}}, {"A", {0}}}}, {"tiny", PM{{"HorizontalRange", {0.1}}, {"VerticalRange", {0.1}}}}}},
        {"sharpen", {{"more", PM{{"Amount", {1.0}}}}, {"little", PM{{"Amount", {0.15}}}}, {"none", PM{{"Amount", {0.0}}}}}},
        {"mirror", {{"tilted", PM{{"Angle", {30}}, {"Center", {0.4, 0.6}}}}, {"vertical", PM{{"Angle", {90}}, {"Center", {0.5, 0.5}}}}, {"back", PM{{"Angle", {-120}}, {"Center", {0.7, 0.3}}}}}},
        {"glow", {{"strong", PM{{"GlowPercent", {0.5}}, {"Intensity", {2.0}}, {"Suppression", {0.2}}}}, {"tinted", PM{{"Color", {1.0, 0.5, 0.2, 1.0}}, {"Intensity", {0.6}}}},
                  {"wide", PM{{"GlowPercent", {1.0}}, {"Intensity", {5.0}}, {"Suppression", {0.0}}, {"Color", {0.2, 0.9, 0.4, 0.5}}}}, {"none", PM{{"GlowPercent", {0.0}}, {"Intensity", {1.0}}}},
                  {"strong", PM{{"GlowPercent", {0.3}}, {"Intensity", {25.0}}, {"Suppression", {0.9}}, {"Color", {1.0, 1.0, 1.0, 1.0}}}}}},
        {"chromakeyer", {{"red key", PM{{"color", {1.0, 0.0, 0.0, 1.0}}, {"LowThreshold", {0.3}}, {"HighThreshold", {0.6}}}},
                         {"blurred", PM{{"color", {0.0, 1.0, 0.0, 1.0}}, {"BlurAmount", {0.5}}, {"LowThreshold", {0.2}}, {"HighThreshold", {0.9}}}},
                         {"cut mask", PM{{"color", {0.5, 0.5, 0.5, 1.0}}, {"LowThreshold", {0.5}}, {"HighThreshold", {0.3}}, {"ShowMaskOnly", {1}}}},
                         {"mask only", PM{{"color", {0.0, 0.0, 1.0, 1.0}}, {"ShowMaskOnly", {1}}, {"BlurAmount", {0.2}}}}}},
        {"convolutionkernel", {{"emboss", PM{{"Row1Column1", {2}}, {"Row2Column2", {0}}, {"Row3Column3", {-2}}, {"AutoNormalize", {0}}, {"Scale", {1}}, {"Offset", {0.5}}}},
                               {"blur", PM{{"Row1Column1", {4}}, {"Row1Column2", {4}}, {"Row1Column3", {4}}, {"Row2Column1", {4}}, {"Row2Column2", {5}}, {"Row2Column3", {4}},
                                           {"Row3Column1", {4}}, {"Row3Column2", {4}}, {"Row3Column3", {4}}}},
                               {"turned", PM{{"Row1Column2", {-1}}, {"Row2Column1", {-1}}, {"Row2Column2", {5}}, {"Row2Column3", {-1}}, {"Row3Column2", {-1}}, {"Rotation", {33}}}}}},
        {"colorcorrector", {{"wheels", PM{{"Low", {200, 0.8}}, {"Mid", {40, 0.5}}, {"High", {120, 1.0}}}}, {"tone", PM{{"Saturation", {1.4}}, {"Gamma", {1.5}}, {"Gain", {0.8}}, {"Offset", {12}}}}}},
        {"saturationadjust", {{"boost", PM{{"Amount", {0.6}}, {"Center", {0.4}}, {"Spread", {0.5}}}}, {"edges", PM{{"Low", {-0.7}}, {"High", {0.9}}, {"Center", {0.5}}, {"Spread", {0.3}}}}}},
        {"labadjust", {{"light", PM{{"Lightness", {1.3}}, {"Channela", {0.3}}, {"Channelb", {-0.2}}}}, {"inverted", PM{{"InvertLuminance", {1}}, {"Desaturate", {0.4}}}}}},
        {"blackrestore", {{"high", PM{{"Threshold", {0.6}}}}, {"low", PM{{"Threshold", {0.1}}}}}},
        {"channelblend", {{"swap", PM{{"RED weight R", {0}}, {"RED weight B", {1}}, {"BLUE weight B", {0}}, {"BLUE weight R", {1}}}},
                          {"mix", PM{{"Normalize", {1}}, {"RED weight G", {0.8}}, {"GREEN weight R", {0.5}}, {"BLUE weight Offset", {0.1}}, {"ALPHA weight A", {0.7}}}},
                          {"neg", PM{{"RED weight G", {-0.6}}, {"RED weight B", {1.3}}, {"GREEN weight Offset", {-0.2}}, {"BLUE weight A", {0.37}}}},
                          {"neg norm", PM{{"Normalize", {1}}, {"RED weight G", {-0.6}}, {"RED weight B", {1.3}}, {"GREEN weight Offset", {0.25}}, {"BLUE weight R", {0.33}}, {"BLUE weight G", {0.33}}, {"BLUE weight B", {0.33}}}}}},
        {"vignette", {{"dark", PM{{"Outer", {1.0}}, {"Inner", {0.3}}, {"Feather", {2.0}}, {"Aspect", {1.5}}, {"Rotation", {10}}}}, {"inverted", PM{{"Invert", {1}}, {"Center", {0.3, 0.7}}, {"Color", {1.0, 0.2, 0.2, 0.8}}}},
                     {"soft", PM{{"Outer", {0.7}}, {"Inner", {0.0}}, {"Feather", {0.3}}, {"Aspect", {0.4}}, {"Rotation", {200}}, {"Color", {0.1, 0.3, 0.9, 1.0}}}},
                     {"tight", PM{{"Outer", {0.4}}, {"Inner", {0.9}}, {"Feather", {5.0}}}}}},
        {"unsharpmask", {{"strong", PM{{"Amount", {2.0}}, {"Radius", {0.05}}, {"Threshold", {0.1}}}}, {"narrow", PM{{"Amount", {1.0}}, {"Radius", {0.004}}}}}},
        {"quickblur", {{"half", PM{{"BlendAmount", {0.5}}}}, {"mostly", PM{{"BlendAmount", {0.93}}}}, {"little", PM{{"BlendAmount", {0.07}}}}, {"none", PM{{"BlendAmount", {0.0}}}}}},
        {"pixelate", {{"coarse", PM{{"HorizontalPixelization", {0.5}}, {"VerticalPixelization", {0.5}}}}, {"wide", PM{{"HorizontalPixelization", {0.9}}, {"VerticalPixelization", {0.2}}}},
                      {"fine", PM{{"HorizontalPixelization", {0.1}}, {"VerticalPixelization", {0.1}}}}}},
        {"spherize", {{"bulge", PM{{"Amount", {0.8}}, {"Center", {0.4, 0.6}}, {"ScalingHorizontal", {1.2}}}}, {"pinch", PM{{"Amount", {-0.7}}, {"ScalingProportional", {0}}, {"ScalingVertical", {0.6}}}}}},
        {"swirl", {{"strong", PM{{"Amount", {1.0}}}}, {"back", PM{{"Amount", {-0.6}}, {"Center", {0.3, 0.4}}, {"ScalingHorizontal", {0.8}}}}}},
        {"pinchpunch", {{"punch", PM{{"Amount", {1.0}}}}, {"pinch", PM{{"Amount", {-0.8}}, {"Center", {0.6, 0.3}}}}}},
        {"wave", {{"strong", PM{{"VerticalWaves", {6}}, {"HorizontalWaves", {9}}, {"VerticalAmplitude", {0.15}}, {"HorizontalAmplitude", {0.1}}, {"VerticalPhase", {0.3}}, {"HorizontalPhase", {0.7}}}}}},
        {"minandmax", {{"max", PM{{"Operation", {1}}, {"HorizontalRange", {0.5}}, {"VerticalRange", {0.7}}}}, {"min round", PM{{"Operation", {0}}, {"HorizontalRange", {0.8}}, {"VerticalRange", {0.8}}, {"CircularAreaProcessing", {1}}}},
                       {"max round", PM{{"Operation", {1}}, {"HorizontalRange", {1.0}}, {"VerticalRange", {0.6}}, {"CircularAreaProcessing", {1}}}}}},
        {"gradientmap", {{"mix", PM{{"Amount", {0.7}}, {"Mode", {0}}}}, {"add", PM{{"Amount", {0.9}}, {"Mode", {1}}}}}},
        {"maskgenerator", {{"luma", PM{{"Type", {0}}, {"LowIn", {0.2}}, {"HighIn", {0.8}}}}, {"red inverted", PM{{"Type", {1}}, {"Invert", {1}}, {"LowOut", {0.1}}, {"HighOut", {0.9}}}},
                           {"alpha", PM{{"Type", {4}}}}, {"blue", PM{{"Type", {3}}, {"LowIn", {0.1}}, {"HighIn", {0.5}}}}}},
        {"broadcastcolors", {{"studio no setup", PM{{"StudioRGB16To135", {1}}, {"75IREsetup", {0}}, {"LumaMin", {15}}, {"LumaMax", {85}}, {"ChromaMax", {50}}}}, {"smooth", PM{{"LumaMin", {20}}, {"LumaMinSmoothness", {0.6}}, {"LumaMax", {80}}, {"LumaMaxSmoothness", {0.7}}, {"ChromaMax", {30}}, {"ChromaSmoothness", {0.5}}, {"CompositeMin", {30}}, {"CompositeMinSmoothness", {0.4}}, {"CompositeMax", {90}}, {"CompositeMaxSmoothness", {0.8}}}}, {"limits", PM{{"LumaMin", {10}}, {"LumaMax", {90}}, {"ChromaMax", {60}}}}, {"studio", PM{{"StudioRGB16To135", {1}}, {"75IREsetup", {1}}, {"ChromaMax", {40}}}},
                            {"composite", PM{{"CompositeMax", {100}}, {"CompositeMin", {-8}}, {"ChromaMax", {100}}}},
                            {"smooth", PM{{"LumaMin", {16}}, {"LumaMax", {85}}, {"ChromaMax", {74}}, {"CompositeMin", {-27.5}}, {"CompositeMax", {124}}, {"LumaMinSmoothness", {0.5}},
                                          {"LumaMaxSmoothness", {1}}, {"ChromaSmoothness", {0.5}}, {"CompositeMinSmoothness", {0.5}}, {"CompositeMaxSmoothness", {1}}, {"StudioRGB16To135", {0}}}}}},
        {"chromablur", {{"wide", PM{{"HorizontalPixels", {6}}, {"VerticalPixels", {3}}}}, {"horizontal", PM{{"HorizontalPixels", {10}}, {"VerticalPixels", {0}}}}}},
        {"linearwipe", {{"soft", PM{{"Transition", {0.4}}, {"Angle", {30}}, {"Feather", {0.5}}}}, {"hard", PM{{"Transition", {0.7}}, {"Angle", {200}}, {"Feather", {0.0}}}},
                        {"vertical", PM{{"Transition", {0.25}}, {"Angle", {90}}, {"Feather", {1.0}}}}}},
        {"clockwipe", {{"clockwise", PM{{"Transition", {0.3}}, {"FeatherAngle", {30}}}}, {"counter", PM{{"Transition", {0.65}}, {"Direction", {1}}, {"FeatherAngle", {90}}}},
                       {"hard", PM{{"Transition", {0.8}}, {"FeatherAngle", {0}}}}, {"hard counter", PM{{"Transition", {0.4}}, {"Direction", {1}}, {"FeatherAngle", {0}}}}}},
        {"push", {{"up", PM{{"Transition", {0.37}}, {"Direction", {0}}}}, {"right border", PM{{"Transition", {0.5}}, {"Direction", {3}}, {"BorderSize", {0.1}}, {"BorderFeather", {0.1}}, {"BorderColor", {0.2, 0.4, 0.9, 1.0}}}},
                  {"left in place", PM{{"Transition", {0.62}}, {"Direction", {2}}, {"PushOffPreviousImage", {0}}, {"BorderSize", {0.05}}, {"BorderColor", {1.0, 0.0, 0.0, 1.0}}}},
                  {"down soft", PM{{"Transition", {0.81}}, {"Direction", {1}}, {"BorderFeather", {0.3}}}}}},
        {"barndoor", {{"in", PM{{"Transition", {0.4}}}}, {"out vertical", PM{{"Transition", {0.55}}, {"Orientation", {1}}, {"Direction", {1}}, {"BorderSize", {0.1}}, {"BorderFeather", {0.1}}}},
                      {"in border", PM{{"Transition", {0.75}}, {"BorderSize", {0.15}}, {"BorderColor", {0.0, 0.8, 0.2, 1.0}}}}}},
    };
    return sets;
}

}  // namespace

void gpuDericheBlur(gpu::Plan& p, int from, int to, double sigmaX, double sigmaY, bool levels) { gpuAddDeriche(p, from, to, sigmaX, sigmaY, levels); }

// Effects whose GPU picture is not yet bit-identical to the CPU's: they stay on the CPU (the planner is not attached).
// Their kernels are kept in this file for when each is made exact.
static bool gpuDeclinedForExactness(const std::string& id)
{
    static const char* const names[] = {"checkerboard"};
    for (const char* n : names)
        if (id == fullId(n)) return true;
    return false;
}

// Float tolerance: 0 (bit-identical) except where the CPU result depends on libm / the compiler's float contraction,
// which a GPU cannot reproduce to the last bit: there a few ulps are allowed (8-bit pictures are always exact).
double gpuFloatTolerance(const Def& d)
{
    { double t; if (extraGpuFloatTolerance(d.id, t)) return t; }      // (the transitions and generators: vktrans.cpp)
    static const char* const ulp[] = {"colorcorrector", "colorgrading", "colormatch", "tvsimulator", "mirror", "clockwipe",
                                      "linearwipe", "push", "barndoor", "broadcastcolors", "lensflare", "colorcorrectorsecondary"};
    for (const char* n : ulp)
        if (d.id == fullId(n)) return 5e-5;
    for (const FilterGpuEntry& e : filterGpuEntries())
        if (d.id == fullId(e.id)) return e.floatTol;
    return 0.0;
}

std::vector<GpuCheckCase> gpuCheckCases(const Def& d)
{
    std::vector<GpuCheckCase> v;
    v.push_back(GpuCheckCase{"defaults", PM()});
    for (const CaseSet& s : caseSets())
        if (d.id == fullId(s.id)) v.insert(v.end(), s.cases.begin(), s.cases.end());
    extraGpuCases(d.id, v);
    for (const FilterGpuEntry& e : filterGpuEntries())
        if (d.id == fullId(e.id)) v.insert(v.end(), e.cases.begin(), e.cases.end());
    return v;
}

void makeGpuTestPicture(int w, int h, bool isFloat, bool bgra, bool hdr, uint8_t* out, size_t stride)
{
    uint32_t seed = 12345u;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.f; };
    for (int row = 0; row < h; ++row) {              // row 0 is the bottom
        const int y = h - 1 - row;                    // from the top
        for (int x = 0; x < w; ++x) {
            const float fx = (float)x / (float)std::max(1, w - 1), fy = (float)y / (float)std::max(1, h - 1);
            float r = 0.5f + 0.5f * std::sin(fx * 9.f + fy * 2.f), g = 0.5f + 0.5f * std::sin(fy * 7.f + 1.f), b = fx * (1.f - fy);
            if (((x / 16) + (y / 16)) % 7 == 0) { r = 1.f; g = 0.f; b = 0.f; }                       // saturated blocks
            if (((x / 24) + (y / 24)) % 11 == 3) { r = g = b = (float)((x + y) % 2); }              // hard black / white
            const float n = (rnd() - 0.5f) * 0.1f;
            r = std::min(1.f, std::max(0.f, r + n)); g = std::min(1.f, std::max(0.f, g + n)); b = std::min(1.f, std::max(0.f, b + n));
            float a = 1.f;
            if (fy > 0.66f) a = 1.f - (fy - 0.66f) / 0.34f;                                      // a soft ramp at the bottom
            if (x > w * 3 / 4 && y < h / 5) a = 0.f;                                             // a transparent corner
            if (x > w / 8 && x < w / 4 && y > h / 3 && y < h / 2) a = 0.5f;
            float p[4] = {r * a, g * a, b * a, a};
            if (hdr && isFloat && ((x * 7 + y * 3) % 31) == 0) { p[0] *= 3.f; p[1] *= 2.f; p[2] *= 1.5f; }
            uint8_t* o = out + (size_t)row * stride;
            if (isFloat) {
                std::memcpy(o + (size_t)x * 16, p, 16);
            } else {
                uint8_t q[4];
                for (int k = 0; k < 4; ++k) q[k] = (uint8_t)std::lround(std::min(1.f, std::max(0.f, p[k])) * 255.f);
                for (int k = 0; k < 3; ++k) q[k] = std::min(q[k], q[3]);
                uint8_t* px = o + (size_t)x * 4;
                px[bgra ? 2 : 0] = q[0]; px[1] = q[1]; px[bgra ? 0 : 2] = q[2]; px[3] = q[3];
            }
        }
    }
}

void attachGpuPlanners(std::vector<Def>& all)
{
    for (Def& d : all)
        for (const Entry& e : kEntries)
            if (d.id == fullId(e.id) && !gpuDeclinedForExactness(d.id)) d.gpu = e.plan;
    for (Def& d : all)
        for (const FilterGpuEntry& e : filterGpuEntries())
            if (e.plan && d.id == fullId(e.id)) d.gpu = e.plan;
    for (Def& d : all)                      // (the transitions and generators of vktrans.cpp take over the ids they list)
        for (const ExtraGpuEntry& e : extraGpuEntries())
            if (d.id == fullId(e.id)) d.gpu = e.plan;
}

std::vector<std::string> gpuEffectIds()
{
    std::vector<std::string> v;
    for (const Entry& e : kEntries)
        if (!gpuDeclinedForExactness(fullId(e.id))) v.push_back(fullId(e.id));
    for (const FilterGpuEntry& e : filterGpuEntries())
        if (e.plan && std::find(v.begin(), v.end(), fullId(e.id)) == v.end()) v.push_back(fullId(e.id));
    for (const ExtraGpuEntry& e : extraGpuEntries())
        if (std::find(v.begin(), v.end(), fullId(e.id)) == v.end()) v.push_back(fullId(e.id));
    return v;
}

bool gpuRender(const Def& d, Ctx& c, GpuFrame& f)
{
    if (!d.gpu || !gpu::enabled() || !pick::useGpu(d)) return false;
    // (a plan per thread, its vectors kept between frames)
    thread_local gpu::Plan plan;
    plan.begin((uint32_t)c.frameW, (uint32_t)c.frameH, d.id.c_str());
    bool ok = false;
    try { ok = d.gpu(c, plan); } catch (...) { ok = false; }
    if (!ok || plan.passes.empty()) return false;
    gpu::FrameIO io;
    io.w = (uint32_t)c.frameW;
    io.h = (uint32_t)c.frameH;
    io.isFloat = f.isFloat;
    io.bgra = f.bgra;
    io.in = f.in;
    io.inRowBytes = f.inRowBytes;
    io.out = f.out;
    io.outRowBytes = f.outRowBytes;
    io.inSerial = f.inSerial;
    io.outSerial = f.outSerial;
    io.noInput = f.noInput;
    io.deferOut = f.deferOut;
    return gpu::run(plan, io);
}

GpuTestSettings::GpuTestSettings(const Def& d, const GpuCheckCase& cs)
{
    for (const PDef& p : d.params) {
        if (!p.def.empty()) params[p.name] = p.def;
        if (!p.sdef.empty()) strings[p.name] = p.sdef;
    }
    for (const auto& kv : cs.params) params[kv.first] = kv.second;
    for (const auto& kv : cs.strings) strings[kv.first] = kv.second;
}

std::map<std::string, std::vector<double>> defaultTestParams(const Def& d)
{
    std::map<std::string, std::vector<double>> m;
    for (const PDef& p : d.params)
        if (!p.def.empty()) m[p.name] = p.def;
    return m;
}

// ---------------------------------------------------------------------------------------------------- test helpers
// A transition's incoming picture for the tests: the outgoing one turned half way round (as a float picture, top row first).
static void secondPicture(Ctx& c, int w, int h, bool isFloat, bool bgra, const uint8_t* in, size_t inStride)
{
    c.src2.ensure(w, h);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = in + (size_t)y * inStride;
        float* dst = c.src2.at(0, y);
        std::vector<float> tmp((size_t)w * 4);
        if (isFloat) std::memcpy(tmp.data(), row, (size_t)w * 16);
        else pix::bytesRowToFloat(row, tmp.data(), w, bgra);
        for (int x = 0; x < w; ++x) std::memcpy(dst + (size_t)x * 4, tmp.data() + (size_t)(w - 1 - x) * 4, 16);
    }
    const auto it = c.testParams ? c.testParams->find("Transition") : std::map<std::string, std::vector<double>>::const_iterator();
    c.progress = (c.testParams && it != c.testParams->end() && !it->second.empty()) ? clampd(it->second[0], 0, 1) : 0.5;
}

bool renderFrameCpu(const Def& d, Ctx& c, int w, int h, bool isFloat, bool bgra, const uint8_t* in, size_t inStride, uint8_t* out, size_t outStride)
{
    if (!d.render || w < 1 || h < 1) return false;
    c.frameW = w;
    c.frameH = h;
    c.levels = !isFloat;
    c.src.ensure(w, h);
    if (d.transition) secondPicture(c, w, h, isFloat, bgra, in, inStride);
    // the host's frame into the float picture, top row first (load() in builtins.cpp)
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = in + (size_t)(h - 1 - y) * inStride;
        float* dst = c.src.at(0, y);
        if (isFloat) std::memcpy(dst, row, (size_t)w * 16);
        else pix::bytesRowToFloat(row, dst, w, bgra);
    }
    if (d.overwritesOut || d.rows) c.out.ensure(w, h);
    else c.out.resize(w, h);
    d.render(c);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = out + (size_t)(h - 1 - y) * outStride;
        const float* src = c.out.at(0, y);
        if (isFloat) {
            float* o = (float*)row;
            for (int x = 0; x < w; ++x) {
                float p[4] = {src[x * 4], src[x * 4 + 1], src[x * 4 + 2], src[x * 4 + 3]};
                if (!(p[3] >= 0.f)) p[3] = 0.f;
                for (int k = 0; k < 4; ++k) o[x * 4 + k] = std::isfinite(p[k]) ? p[k] : 0.f;
            }
        } else {
            pix::floatRowToBytes(src, row, w, bgra);
        }
    }
    return true;
}

static thread_local std::string tGpuWhy;
std::string lastGpuFailure() { return tGpuWhy; }

bool renderFrameGpu(const Def& d, Ctx& c, int w, int h, bool isFloat, bool bgra, const uint8_t* in, size_t inStride, uint8_t* out, size_t outStride, gpu::RunStats* stats, uint64_t inSerial, uint64_t outSerial)
{
    tGpuWhy = "not run (no GPU plan, Vulkan off, or an empty frame)";
    if (!d.gpu || !gpu::enabled() || w < 1 || h < 1) return false;
    c.frameW = w;
    c.frameH = h;
    if (d.transition) secondPicture(c, w, h, isFloat, bgra, in, inStride);
    c.levels = !isFloat;
    gpu::Plan plan;
    plan.begin((uint32_t)w, (uint32_t)h, d.id.c_str());
    bool ok = false;
    try { ok = d.gpu(c, plan); } catch (...) { ok = false; }
    if (!ok || plan.passes.empty()) {
        tGpuWhy = "declined";
        return false;
    }
    gpu::FrameIO io;
    io.w = (uint32_t)w;
    io.h = (uint32_t)h;
    io.isFloat = isFloat;
    io.bgra = bgra;
    io.in = in;
    io.inRowBytes = inStride;
    io.out = out;
    io.outRowBytes = outStride;
    io.inSerial = inSerial;
    io.outSerial = outSerial;
    io.noInput = d.generator;
    io.deferOut = gpu::deferOutputRequested();
    if (gpu::run(plan, io, stats)) {
        tGpuWhy.clear();
        return true;
    }
    tGpuWhy = "the GPU run failed: " + gpu::lastRunFailure();
    return false;
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno

namespace reno {
namespace ofx {
bool builtinGpuChainable(const std::string& id)
{
    if (!gpu::enabled()) return false;
    const builtin::Def* d = builtin::findDef(id);
    return d && d->gpu && !d->generator && !d->transition && !d->temporal && builtin::pick::useGpu(*d);
}
}  // namespace ofx
}  // namespace reno
