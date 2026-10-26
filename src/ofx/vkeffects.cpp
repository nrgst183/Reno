// The built-in effects on the GPU: for each effect with Vulkan kernels, a function that plans the compute passes from the effect's settings. Each reads the
// settings exactly as the effect's CPU code does (same clamps, same float arithmetic) and hands the kernel (tools/gpukernels.py) the numbers in a
// push-constant block (shaders/push_layouts.h). See vkeffects.h.
#include <cstdio>
#include "vkeffects.h"
#include "vkshaders/push_layouts.h"
#include "builtins_deriche.h"
#include "../core/pixrows.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
namespace more { double unsharpSigma(double radius, int W, int H, bool& none); }

namespace {

typedef gpu::Plan Plan;
typedef gpu::Pass Pass;
namespace P = gpu::push;

const char* const kPrefix = "com.vegascreativesoftware:";

inline uint32_t u32(int v) { return (uint32_t)std::max(0, v); }

// ---------------------------------------------------------------------------------------------------- the planners
bool planInvert(Ctx& c, Plan& p)
{
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

bool planColorBalance(Ctx& c, Plan& p)
{
    P::Colorbalance pc = {p.w, p.h, (float)c.d("Red"), (float)c.d("Green"), (float)c.d("Blue"), u32(c.i("Type")), c.b("PreserveLuminosity") ? 1u : 0u};
    p.add("colorbalance", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planHslAdjust(Ctx& c, Plan& p)
{
    P::Hsladjust pc = {p.w, p.h, (float)c.d("AddToHue"), (float)std::max(0.0, c.d("Saturation")), (float)std::max(0.0, c.d("Luminance"))};
    p.add("hsladjust", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planLevels(Ctx& c, Plan& p)
{
    const double gamma = c.d("Gamma");
    std::vector<float> lut(4097);
    levelsGammaTable(gamma, lut.data());
    const int t = p.addTable(std::move(lut));
    P::Levels pc = {p.w, p.h, u32(c.i("Channel")), (float)c.d("InputStart"), (float)c.d("InputEnd"), (float)c.d("OutputStart"), (float)c.d("OutputEnd"),
                    std::fabs(gamma - 1.0) <= 1e-9 ? 0u : 1u};
    Pass& pass = p.add("levels", 0, 1, p.w, p.h);
    pass.table = t;
    pass.setPush(pc);
    return true;
}

bool planColorCurves(Ctx& c, Plan& p)
{
    std::vector<float> tables;
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
// VEGAS's Deriche Gaussian (vegasDericheBlurSigma() in builtins_deriche.cpp): rows into a scratch buffer (in levels), then columns
// into 'to' (back in 0..1, rounded to whole levels for 8-bit pictures).
void addDeriche(Plan& p, int from, int to, double sigmaX, double sigmaY, bool levels)
{
    const uint32_t W = p.bufs[(size_t)from].w, H = p.bufs[(size_t)from].h;
    const int mid = p.addBuf(W, H, 4);
    for (int pass = 0; pass < 2; ++pass) {
        const DericheCoefs k = vegasDericheCoefs(pass ? sigmaY : sigmaX);
        const double SN = k.n[0] + k.n[1] + k.n[2] + k.n[3], SM = k.m[1] + k.m[2] + k.m[3] + k.m[4];
        const double SD = 1.0 + k.d[1] + k.d[2] + k.d[3] + k.d[4];
        const double kn = std::fabs(SD) > 1e-17 ? SN / SD : 1.0, km = std::fabs(SD) > 1e-17 ? SM / SD : 1.0;
        P::Deriche pc = {W, H, pass ? 1u : 0u, pass ? 0u : (levels ? 2u : 1u), pass ? (levels ? 2u : 1u) : 0u, 0u,
                         (float)k.n[0], (float)k.n[1], (float)k.n[2], (float)k.n[3], (float)k.d[1], (float)k.d[2], (float)k.d[3], (float)k.d[4],
                         (float)k.m[1], (float)k.m[2], (float)k.m[3], (float)k.m[4], (float)kn, (float)km};
        p.add("deriche", pass ? mid : from, pass ? to : mid, pass ? W : H, 1).setPush(pc);
    }
}

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
    const double sx = gaussianBlurSigma(c.d("HorizontalRange"), (int)p.w), sy = gaussianBlurSigma(c.d("VerticalRange"), (int)p.w);
    const uint32_t mask = (c.b("R") ? 1u : 0u) | (c.b("G") ? 2u : 0u) | (c.b("B") ? 4u : 0u) | (c.b("A") ? 8u : 0u);
    if (addBlur(p, 0, 1, sx, sy, mask, false) != 1) addCopy(p, 0, 1);
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
    P::Mirror pc = {p.w, p.h, (float)kx, (float)ky, (float)(cx * p.w), (float)((1 - cy) * p.h), bgPix[0], bgPix[1], bgPix[2], bgPix[3],
                    (float)(0.4 * c.scale * kx * std::fabs(ky)), (float)(0.4 * c.scale * ky * std::fabs(kx)), (float)(0.5 * (kx + ky)), straight ? 1u : 0u};
    p.add("mirror", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planGlow(Ctx& c, Plan& p)
{
    const double pct = clampd(c.d("GlowPercent"), 0, 1), intensity = std::max(0.0, c.d("Intensity")), supp = clampd(c.d("Suppression"), 0, 1);
    if (c.levels && intensity >= 19.999) return false;
    const Color col = c.color("Color");
    const int m = p.addBuf(p.w, p.h, 1);
    {
        P::GlowMask pc = {p.w, p.h};
        p.add("glow_mask", 0, m, p.w, p.h).setPush(pc);
    }
    const double sigma = (pct * 50.0 + 0.3) * p.w / 320.0;
    int blurred = m;
    if (sigma > 0.3) {
        blurred = p.addBuf(p.w, p.h, 1);
        addBlur(p, m, blurred, sigma, sigma, 0, true);
    }
    P::GlowMix pc = {p.w, p.h, (float)intensity, (float)supp, (float)col.r, (float)col.g, (float)col.b};
    Pass& pass = p.add("glow_mix", 0, 1, p.w, p.h);
    pass.in1 = blurred;
    pass.setPush(pc);
    return true;
}

bool planChromaKeyer(Ctx& c, Plan& p)
{
    const Color key = c.color("color");
    const double low = clampd(c.d("LowThreshold"), 0, 1), high = clampd(c.d("HighThreshold"), 0, 1);
    const double kr = clampd(key.r, 0, 1), kg = clampd(key.g, 0, 1), kb = clampd(key.b, 0, 1);
    const double mx = std::max(kr, std::max(kg, kb)), mn = std::min(kr, std::min(kg, kb)), dd = mx - mn;
    double kh = 0, ks = 0;
    const double kl = (mx + mn) * 0.5;
    if (dd > 0) {
        if (mx == kr) { kh = (kg - kb) / dd; if (kh < 0) kh += 6; }
        else if (mx == kg) kh = (kb - kr) / dd + 2;
        else kh = (kr - kg) / dd + 4;
        kh *= 60;
        ks = kl <= 0.5 ? dd / (mx + mn) : dd / (2 - mx - mn);
    }
    const int m = p.addBuf(p.w, p.h, 1);
    {
        P::ChromaMask pc = {p.w, p.h, (float)key.r, (float)key.g, (float)key.b, (float)kh, (float)ks, (float)kl, (float)low, high <= low ? 1u : 0u};
        p.add("chroma_mask", 0, m, p.w, p.h).setPush(pc);
    }
    const double amount = std::max(0.0, c.d("BlurAmount"));
    const double sigma = amount > 0 ? (0.437 + 24.07 * amount) * p.h / 180.0 : 0.0;     // (chromaKeyBlurSigma)
    int mf = m;
    if (sigma > 0) {
        const int r = (int)std::min(4000.0, std::ceil(sigma * 4));
        std::vector<float> k((size_t)r + 1);
        double sum = 0;
        for (int i = 0; i <= r; ++i) { k[(size_t)i] = (float)std::exp(-0.5 * i * i / (sigma * sigma)); sum += (i ? 2 : 1) * k[(size_t)i]; }
        for (auto& v : k) v = (float)(v / sum);
        const int mid = p.addBuf(p.w, p.h, 1);
        mf = p.addBuf(p.w, p.h, 1);
        Pass& ph = p.add("blurp_h", m, mid, p.w, p.h);
        ph.table = p.addTable(k);
        { P::BlurpH pc = {p.w, p.h, (uint32_t)r}; ph.setPush(pc); }
        Pass& pv = p.add("blurp_v", mid, mf, p.w, p.h);
        pv.table = p.addTable(k);
        { P::BlurpV pc = {p.w, p.h, (uint32_t)r}; pv.setPush(pc); }
    }
    const unsigned despill = (key.b > key.g && key.b > key.r) ? 1u : ((key.g > key.r && key.g >= key.b) ? 2u : 0u);
    P::ChromaMix pc = {p.w, p.h, c.b("ShowMaskOnly") ? 1u : 0u, despill, sigma > 0 ? 1u : 0u};
    Pass& pass = p.add("chroma_mix", 0, 1, p.w, p.h);
    pass.in1 = mf;
    pass.setPush(pc);
    return true;
}

bool planConvolutionKernel(Ctx& c, Plan& p)
{
    double k[3][3];
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col) {
            char nm[32];
            std::snprintf(nm, sizeof nm, "Row%dColumn%d", r + 1, col + 1);
            k[r][col] = clampd(c.d(nm), -1e4, 1e4);
        }
    double sum = 0;
    for (auto& row : k) for (double v : row) sum += v;
    const double scaleIn = c.d("Scale");
    const bool scaleBad = !(scaleIn >= 0.0 && scaleIn <= 4.0);
    const bool norm = (c.b("AutoNormalize") || scaleBad) && std::fabs(sum) > 1e-9;
    const float mul = (float)(norm ? 1.0 : clampd(c.d("Scale"), -1e4, 1e4));
    const float off = (float)(clampd(c.d("Offset"), -100, 100) * 255.0);
    if (norm)
        for (auto& row : k) for (double& v : row) v = (double)((float)v / (float)sum);
    const double rot = c.d("Rotation") * kPi / 180.0;
    const bool turned = std::fabs(rot) > 1e-9;
    const bool between = c.scale < 0.999;
    const int tmp = p.addBuf(p.w, p.h, 4);
    {
        P::UnpremulPx pc = {p.w, p.h};
        p.add("unpremul_px", 0, tmp, p.w, p.h).setPush(pc);
    }
    P::Conv3x3 pc = {p.w, p.h, (float)k[0][0], (float)k[0][1], (float)k[0][2], (float)k[1][0], (float)k[1][1], (float)k[1][2], (float)k[2][0], (float)k[2][1], (float)k[2][2],
                     (float)std::cos(rot), (float)std::sin(rot), (float)c.scale, between ? 2u : (turned ? 1u : 0u), mul, off, c.levels ? 1u : 0u};
    p.add("conv3x3", tmp, 1, p.w, p.h).setPush(pc);
    return true;
}

// ---- more colour effects and distortions
bool planColorCorrector(Ctx& c, Plan& p)
{
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
    const float scale = 0.9272f;
    P::Saturationadjust pc = {p.w, p.h, (float)c.d("Amount"), (float)c.d("Center") * scale, (float)std::max(0.005, c.d("Spread")) * 0.5f * scale, (float)c.d("Low"), (float)c.d("High")};
    p.add("saturationadjust", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planLabAdjust(Ctx& c, Plan& p)
{
    P::Labadjust pc = {p.w, p.h, (float)c.d("Lightness"), (float)c.d("Channela"), (float)c.d("Channelb"), (float)clampd(c.d("Desaturate"), 0, 1), c.b("InvertLuminance") ? 1u : 0u};
    p.add("labadjust", 0, 1, p.w, p.h).setPush(pc);
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
    pc.flags = c.b("Normalize") ? 1u : 0u;
    pc.pad = 0;
    float* dst = &pc.w0;
    for (int o = 0; o < 4; ++o)
        for (int i = 0; i < 5; ++i) dst[o * 5 + i] = w[o][i];
    p.add("channelblend", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planVignette(Ctx& c, Plan& p)
{
    double cx, cy;
    c.xy("Center", cx, cy);
    const double outer = std::max(1e-6, c.d("Outer")), inner = clampd(c.d("Inner"), 0, 0.999999);
    const double aspect = std::max(0.0, c.d("Aspect")), rot = c.d("Rotation") * kPi / 180.0, feather = std::max(0.01, c.d("Feather"));
    const Color col = c.color("Color");
    const double W = p.w, H = p.h;
    const double half = 0.5 * std::sqrt(W * W + H * H) * outer, g = std::max(1e-3, (1.0 + aspect) * 0.5);
    P::Vignette pc = {p.w, p.h, (float)(cx * W), (float)((1 - cy) * H), (float)std::cos(rot), (float)std::sin(rot), (float)(1.0 / (half * g)), (float)(g / half),
                      (float)inner, (float)(1.0 / (1.0 - inner)), (float)((10.0 / feather) * (10.0 / feather)), c.b("Invert") ? 1u : 0u,
                      (float)col.r, (float)col.g, (float)col.b, (float)col.a};
    p.add("vignette", 0, 1, p.w, p.h).setPush(pc);
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
    addDeriche(p, 0, blurred, sigma, sigma, c.levels);
    P::UnsharpMix pc = {p.w, p.h, (float)c.d("Amount"), (float)thr8, c.levels ? 1u : 0u};
    Pass& pass = p.add("unsharp_mix", 0, 1, p.w, p.h);
    pass.in1 = blurred;
    pass.setPush(pc);
    return true;
}

bool planQuickBlur(Ctx& c, Plan& p)
{
    const float k = (float)clampd(c.d("BlendAmount"), 0, 1);
    const double sigma = 1.2 * (std::max(1u, p.h) / 1080.0) + c.px(0.4);
    int blurred = 0;
    if (sigma > 0.3) {
        blurred = p.addBuf(p.w, p.h, 4);
        addBlur(p, 0, blurred, sigma, sigma, 15u, false);
    }
    P::BlendMix pc = {p.w, p.h, k};
    Pass& pass = p.add("blend_mix", 0, 1, p.w, p.h);
    pass.in1 = blurred;
    pass.setPush(pc);
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
        P::PixelateBlocks pc = {p.w, p.h, (uint32_t)nbx, (uint32_t)nby, (uint32_t)kx, (uint32_t)ky, c.levels ? 1u : 0u, 0};
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
struct AreaPush { float px0, py0, rx, ry; };
AreaPush areaOf(Ctx& c, int W, int H)
{
    double cx, cy;
    c.xy("Center", cx, cy);
    const double sh = clampd(c.d("ScalingHorizontal"), 0, 10);
    const double sv = c.b("ScalingProportional") ? sh * W / std::max(1, H) : clampd(c.d("ScalingVertical"), 0, 10);
    AreaPush a;
    a.rx = (float)std::max(1e-3, sh * W * 0.5);
    a.ry = (float)std::max(1e-3, sv * H * 0.5);
    a.px0 = (float)(cx * W);
    a.py0 = (float)((1 - cy) * H);
    return a;
}

// (Spherize has no plan: VEGAS's sampling works out the read position in double precision and cuts it to whole pixels and 1/256 weights, which a float
// kernel misses at a few pixels; the "spherize" kernel follows the formula but is not used.)

bool planSwirl(Ctx& c, Plan& p)
{
    const AreaPush a = areaOf(c, (int)p.w, (int)p.h);
    P::Swirl pc = {p.w, p.h, a.px0, a.py0, a.rx, a.ry, (float)(clampd(c.d("Amount"), -1, 1) * 2 * 2 * kPi)};
    p.add("swirl", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planPinchPunch(Ctx& c, Plan& p)
{
    const AreaPush a = areaOf(c, (int)p.w, (int)p.h);
    P::Pinchpunch pc = {p.w, p.h, a.px0, a.py0, a.rx, a.ry, (float)clampd(c.d("Amount"), -1, 1)};
    p.add("pinchpunch", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

bool planWave(Ctx& c, Plan& p)
{
    const double W = p.w, H = p.h;
    P::Wave pc = {p.w, p.h, (float)clampd(c.d("VerticalWaves"), 0, 1000), (float)clampd(c.d("HorizontalWaves"), 0, 1000),
                  (float)(clampd(c.d("VerticalAmplitude"), -1, 1) * H), (float)(clampd(c.d("HorizontalAmplitude"), -1, 1) * W),
                  (float)c.d("VerticalPhase"), (float)c.d("HorizontalPhase"), (float)(1.0 / std::max(1.0, W)), (float)(1.0 / std::max(1.0, H))};
    p.add("wave", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

// Min and Max (neighbourhood() in builtins_more_blur.cpp, halfWidths() in builtins_fast.cpp)
bool planMinMax(Ctx& c, Plan& p)
{
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
    const std::vector<float> kx = chromaBlurKernel(false, c.px(clampd(c.d("HorizontalPixels"), 0, 10) + 1.0)), ky = chromaBlurKernel(true, c.px(2.4 * clampd(c.d("VerticalPixels"), 0, 10) + 1.0));
    const std::vector<float> hx(kx.begin() + kx.size() / 2, kx.end()), hy(ky.begin() + ky.size() / 2, ky.end());
    const int t = p.addBuf(p.w, p.h, 4), t2 = p.addBuf(p.w, p.h, 4), t3 = p.addBuf(p.w, p.h, 4);
    {
        P::ChromaSplit pc = {p.w, p.h};
        p.add("chroma_split", 0, t, p.w, p.h).setPush(pc);
    }
    {
        Pass& pass = p.add("blur_h", t, t2, p.w, p.h);
        pass.table = p.addTable(hx);
        P::BlurH pc = {p.w, p.h, (uint32_t)hx.size() - 1, 6u};
        pass.setPush(pc);
    }
    {
        Pass& pass = p.add("blur_v", t2, t3, p.w, p.h);
        pass.table = p.addTable(hy);
        P::BlurV pc = {p.w, p.h, (uint32_t)hy.size() - 1, 6u};
        pass.setPush(pc);
    }
    P::ChromaJoin pc = {p.w, p.h, c.levels ? 1u : 0u};
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


struct Entry { const char* id; bool (*plan)(Ctx&, Plan&); };
// (no entries, so the CPU renders them: sepia, add noise and white balance - their 8-bit steps, the shift-register generator and the frame histogram follow
// VEGAS exactly in builtins_filters.cpp / builtins_more_colour.cpp)
const Entry kEntries[] = {
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
    {"glow", planGlow},
    {"chromakeyer", planChromaKeyer},
    {"convolutionkernel", planConvolutionKernel},
    {"colorcorrector", planColorCorrector},
    {"saturationadjust", planSaturationAdjust},
    {"labadjust", planLabAdjust},
    {"blackrestore", planBlackRestore},
    {"channelblend", planChannelBlend},
    {"vignette", planVignette},
    {"unsharpmask", planUnsharpMask},
    {"quickblur", planQuickBlur},
    {"pixelate", planPixelate},
    // (Linear Blur: on the CPU - VEGAS's shear / Deriche blur / shear back, vegasLinearBlur() in builtins_deriche.cpp; the old Gaussian-taps kernel no longer matches)
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

const std::vector<CaseSet>& caseSets()
{
    static const std::vector<CaseSet> sets = {
        {"invert", {{"half", PM{{"BlendAmount", {0.5}}}}, {"off", PM{{"BlendAmount", {0.0}}}}, {"full", PM{{"BlendAmount", {1.0}}}}}},
        {"brightnessandcontrast", {{"brighter", PM{{"Brightness", {0.2}}, {"Contrast", {0.3}}}}, {"darker", PM{{"Brightness", {-0.3}}, {"Contrast", {-0.4}}}},
                                   {"harsh", PM{{"Contrast", {0.95}}, {"ContrastCenter", {0.4}}}}}},
        {"blackandwhite", {{"half", PM{{"BlendAmount", {0.5}}}}}},
        {"threshold", {{"low", PM{{"Threshold", {0.2}}}}, {"high", PM{{"Threshold", {0.8}}}}}},
        {"colorbalance", {{"cast", PM{{"Red", {0.3}}, {"Green", {-0.2}}, {"Blue", {0.1}}}}, {"cast1", PM{{"Red", {-0.5}}, {"Blue", {0.4}}, {"Type", {1}}}},
                          {"cast2", PM{{"Red", {0.4}}, {"Type", {2}}, {"PreserveLuminosity", {1}}}}}},
        {"hsladjust", {{"hue", PM{{"AddToHue", {120}}}}, {"sat", PM{{"Saturation", {1.6}}, {"Luminance", {0.7}}}}, {"grey", PM{{"Saturation", {0.0}}, {"AddToHue", {-45}}}}}},
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
          {"inverted", PM{{"InvertMask", {1}}, {"LimitHueCenter", {100}}, {"LimitHueWidth", {120}}, {"Alpha", {0.5}}, {"Chrominance", {200, 0.5}}}}}},
        {"gaussianblur", {{"wide", PM{{"HorizontalRange", {1.5}}, {"VerticalRange", {0.4}}}}, {"onlyx", PM{{"HorizontalRange", {0.8}}, {"VerticalRange", {0.0}}}},
                          {"channels", PM{{"HorizontalRange", {0.6}}, {"VerticalRange", {0.6}}, {"R", {0}}, {"A", {0}}}}, {"tiny", PM{{"HorizontalRange", {0.1}}, {"VerticalRange", {0.1}}}}}},
        {"sharpen", {{"more", PM{{"Amount", {1.0}}}}, {"little", PM{{"Amount", {0.15}}}}, {"none", PM{{"Amount", {0.0}}}}}},
        {"mirror", {{"tilted", PM{{"Angle", {30}}, {"Center", {0.4, 0.6}}}}, {"vertical", PM{{"Angle", {90}}, {"Center", {0.5, 0.5}}}}, {"back", PM{{"Angle", {-120}}, {"Center", {0.7, 0.3}}}}}},
        {"glow", {{"strong", PM{{"GlowPercent", {0.5}}, {"Intensity", {2.0}}, {"Suppression", {0.2}}}}, {"tinted", PM{{"Color", {1.0, 0.5, 0.2, 1.0}}, {"Intensity", {0.6}}}}}},
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
                          {"mix", PM{{"Normalize", {1}}, {"RED weight G", {0.8}}, {"GREEN weight R", {0.5}}, {"BLUE weight Offset", {0.1}}, {"ALPHA weight A", {0.7}}}}}},
        {"vignette", {{"dark", PM{{"Outer", {1.0}}, {"Inner", {0.3}}, {"Feather", {2.0}}, {"Aspect", {1.5}}, {"Rotation", {10}}}}, {"inverted", PM{{"Invert", {1}}, {"Center", {0.3, 0.7}}, {"Color", {1.0, 0.2, 0.2, 0.8}}}}}},
        {"unsharpmask", {{"strong", PM{{"Amount", {2.0}}, {"Radius", {0.05}}, {"Threshold", {0.1}}}}, {"narrow", PM{{"Amount", {1.0}}, {"Radius", {0.004}}}}}},
        {"quickblur", {{"half", PM{{"BlendAmount", {0.5}}}}}},
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
        {"broadcastcolors", {{"limits", PM{{"LumaMin", {10}}, {"LumaMax", {90}}, {"ChromaMax", {60}}}}, {"studio", PM{{"StudioRGB16To135", {1}}, {"75IREsetup", {1}}, {"ChromaMax", {40}}}},
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

double gpuFloatTolerance(const Def& d)
{
    static const char* const positional[] = {"mirror", "convolutionkernel", "wave", "swirl", "pinchpunch", "spherize"};
    for (const char* n : positional)
        if (d.id == std::string(kPrefix) + n) return 2e-3;
    // LAB Adjust truncates to 8-bit levels like VEGAS: a value a hair under a level on one side and over on the other
    // (pow/cbrt differ in the last bits between the CPU and a GPU) is a whole level apart
    if (d.id == std::string(kPrefix) + "labadjust") return 1.0 / 255.0 + 1e-4;
    // the Chroma Keyer's alpha is a whole 8-bit level like VEGAS's: a distance a hair either side of a level in float
    if (d.id == std::string(kPrefix) + "chromakeyer") return 1.0 / 255.0 + 1e-4;
    return 1e-4;
}

std::vector<GpuCheckCase> gpuCheckCases(const Def& d)
{
    std::vector<GpuCheckCase> v;
    v.push_back(GpuCheckCase{"defaults", PM()});
    for (const CaseSet& s : caseSets())
        if (d.id == std::string(kPrefix) + s.id) v.insert(v.end(), s.cases.begin(), s.cases.end());
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
            if (d.id == std::string(kPrefix) + e.id && !d.generator) d.gpu = e.plan;
}

std::vector<std::string> gpuEffectIds()
{
    std::vector<std::string> v;
    for (const Entry& e : kEntries) v.push_back(std::string(kPrefix) + e.id);
    return v;
}

bool gpuRender(const Def& d, Ctx& c, GpuFrame& f)
{
    if (!d.gpu || !gpu::enabled()) return false;
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

bool renderFrameGpu(const Def& d, Ctx& c, int w, int h, bool isFloat, bool bgra, const uint8_t* in, size_t inStride, uint8_t* out, size_t outStride, gpu::RunStats* stats, uint64_t inSerial, uint64_t outSerial)
{
    if (!d.gpu || !gpu::enabled() || w < 1 || h < 1) return false;
    c.frameW = w;
    c.frameH = h;
    if (d.transition) secondPicture(c, w, h, isFloat, bgra, in, inStride);
    c.levels = !isFloat;
    gpu::Plan plan;
    plan.begin((uint32_t)w, (uint32_t)h, d.id.c_str());
    bool ok = false;
    try { ok = d.gpu(c, plan); } catch (...) { ok = false; }
    if (!ok || plan.passes.empty()) return false;
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
    return gpu::run(plan, io, stats);
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
