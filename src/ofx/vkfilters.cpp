// Vulkan plans of the built-in filters, bit-identical to their CPU code (see vkfilters.h). Kernels: tools/gpukernels.py.
#include "vkfilters.h"
#include "builtins_cgblur.h"
#include "vkshaders/push_layouts.h"
#include "builtins_deriche.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
namespace {

typedef gpu::Plan Plan;
typedef gpu::Pass Pass;
namespace P = gpu::push;
typedef std::map<std::string, std::vector<double>> PM;

struct D2 { uint32_t h, l; };
D2 d2(double d)
{
    uint64_t b;
    std::memcpy(&b, &d, 8);
    return D2{(uint32_t)(b >> 32), (uint32_t)b};
}

bool planSepia(Ctx& c, Plan& p)
{
    SepiaSettings st;
    sepiaSettingsGpu(c, st);
    std::vector<float> lut(256);
    for (int i = 0; i < 256; ++i) lut[i] = (float)st.lut[i];
    P::Sepia pc = {p.w, p.h, st.greyMix ? 1u : 0u, st.useLut ? 1u : 0u, st.toGrey, st.colour[0], st.colour[1], st.colour[2]};
    Pass& ps = p.add("sepia", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(lut));
    ps.setPush(pc);
    return true;
}

void pushDouble(std::vector<float>& tab, double v)
{
    const D2 d = d2(v);
    float f[2];
    std::memcpy(&f[0], &d.h, 4);
    std::memcpy(&f[1], &d.l, 4);
    tab.push_back(f[0]);
    tab.push_back(f[1]);
}

// Deform (deform() in builtins_more_creative.cpp): the sines come from the host's own libm, as the table of doubles
bool planDeform(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    const double amount = c.d("Amount"), sl = c.d("SqueezeLeft") * amount, sr = c.d("SqueezeRight") * amount, st = c.d("SqueezeTop") * amount, sb = c.d("SqueezeBottom") * amount;
    const double sh = (c.d("ShearHorizontal") - 1.0) * amount, sv = c.d("ShearVertical") * amount;
    const double cl = c.d("CurveLeft") * amount, cr = c.d("CurveRight") * amount, ct = c.d("CurveTop") * amount, cb = c.d("CurveBottom") * amount;
    float bg[4];
    put(bg, c.color("Background"));
    std::vector<float> tab;
    for (int y = 0; y < H; ++y) pushDouble(tab, std::sin(kPi * ((y + 0.5) / H)));
    for (int x = 0; x < W; ++x) pushDouble(tab, std::sin(kPi * ((x + 0.5) / W)));
    const D2 ST = d2(st), SB = d2(sb), SL = d2(sl), SR = d2(sr), SH = d2(sh), SV = d2(sv), CT = d2(ct), CB = d2(cb), CL = d2(cl), CR = d2(cr);
    P::Deform pc = {p.w, p.h, ST.h, ST.l, SB.h, SB.l, SL.h, SL.l, SR.h, SR.l, SH.h, SH.l, SV.h, SV.l, CT.h, CT.l, CB.h, CB.l, CL.h, CL.l, CR.h, CR.l, bg[0], bg[1], bg[2], bg[3]};
    Pass& ps = p.add("deform", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(tab));
    ps.setPush(pc);
    return true;
}

// Spherize (spherize() in builtins_filters2.cpp)
bool planSpherize(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    double pcx, pcy;
    c.xy("Center", pcx, pcy);
    const double amount = c.d("Amount"), sh = c.d("ScalingHorizontal"), sv = c.d("ScalingVertical");
    if (W <= 0 || H <= 0 || std::fabs((float)amount) <= 1e-6f || std::fabs((float)sh) <= 1e-6f || std::fabs((float)sv) <= 1e-6f) {
        P::CopyPx cp = {p.w, p.h};
        p.add("copy_px", 0, 1, p.w, p.h).setPush(cp);
        return true;
    }
    const int cxp = (int)((pcx * 2 - 0.5) * W), cyp = (int)((pcy * 2 - 0.5) * H);
    const double ky = 1.0 / (sv + 0.0001), kx = 1.0 / (sh + 0.0001) * 2.0 / W;
    const D2 KX = d2(kx), KY = d2(ky), AM = d2(amount);
    P::Spherize pc = {p.w, p.h, KX.h, KX.l, KY.h, KY.l, AM.h, AM.l, cxp, cyp, c.levels ? 1u : 0u, 0u};
    p.add("spherize", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

// Convolution Kernel (convolutionKernelRaw() in builtins_filters2.cpp, read the same way)
bool planConvolutionKernel(Ctx& c, Plan& p)
{
    if (c.levels) {         // 8-bit: VEGAS's own byte routine (convolutionKernel8() in builtins_filters2.cpp)
        float A[9], off;
        bool alphaPlain;
        convolutionKernel8Setup(c, A, off, alphaPlain);
        P::Conv8 q = {p.w, p.h, A[0], A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[8], off, alphaPlain ? 1u : 0u};
        p.add("conv8", 0, 1, p.w, p.h).setPush(q);
        return true;
    }
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
    const double rot = c.d("Rotation") * kPi / 180.0;
    const double cr = std::cos(rot), sr = std::sin(rot);
    const bool turned = std::fabs(rot) > 1e-9;
    const bool between = c.scale < 0.999;
    float kv[9] = {0};
    uint32_t mask = 0;
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col)
            if (k[r][col] != 0) {
                kv[r * 3 + col] = norm ? (float)k[r][col] / (float)sum : (float)k[r][col];
                mask |= 1u << (r * 3 + col);
            }
    const D2 CR = d2(cr), SR = d2(sr), SC = d2(c.scale);
    const int prep = p.addBuf(p.w, p.h, 4);
    P::ConvPrep pp = {p.w, p.h, c.levels ? 1u : 0u, 0u};
    p.add("conv_prep", 0, prep, p.w, p.h).setPush(pp);
    P::ConvMain pc = {p.w, p.h, kv[0], kv[1], kv[2], kv[3], kv[4], kv[5], kv[6], kv[7], kv[8], mask, between ? 2u : (turned ? 1u : 0u), mul, off, c.levels ? 1u : 0u,
                      CR.h, CR.l, SR.h, SR.l, SC.h, SC.l};
    p.add("conv_main", prep, 1, p.w, p.h).setPush(pc);
    return true;
}

// LAB Adjust (builtins_more_colour.cpp)
bool planLabAdjust(Ctx& c, Plan& p)
{
    if (c.levels) {         // 8-bit: VEGAS's byte routine (labAdjust8() in builtins_more_colour.cpp)
        std::vector<float> t8;
        more::labAdjust8GpuTable(t8);
        const D2 L8 = d2(c.d("Lightness")), A8 = d2(c.d("Channela") * 100.0), B8 = d2(c.d("Channelb") * 100.0), K8 = d2(1.0 - c.d("Desaturate"));
        P::Labadjust8 q = {p.w, p.h, L8.h, L8.l, A8.h, A8.l, B8.h, B8.l, K8.h, K8.l, c.b("InvertLuminance") ? 1u : 0u, 0u};
        Pass& ps8 = p.add("labadjust8", 0, 1, p.w, p.h);
        ps8.table = p.addTable(std::move(t8));
        ps8.setPush(q);
        return true;
    }
    std::vector<float> tab;
    more::labAdjustGpuTables(tab);
    const D2 L = d2(c.d("Lightness")), A = d2(c.d("Channela")), B = d2(c.d("Channelb")), D = d2(clampd(c.d("Desaturate"), 0, 1));
    P::Labadjust pc = {p.w, p.h, L.h, L.l, A.h, A.l, B.h, B.l, D.h, D.l, c.b("InvertLuminance") ? 1u : 0u, 0u};
    Pass& ps = p.add("labadjust", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(tab));
    ps.setPush(pc);
    return true;
}

// White Balance (builtins_more_colour.cpp): bytes after the YUV shift, then (HasData) the histogram stretch, then the picture
bool planWhiteBalance(Ctx& c, Plan& p)
{
    std::vector<float> tab;
    bool shift, hasData;
    more::whiteBalanceSettings(c, tab, shift, hasData);
    if (!shift && !hasData) {
        P::CopyPx cp = {p.w, p.h};
        p.add("copy_px", 0, 1, p.w, p.h).setPush(cp);
        return true;
    }
    if (tab.empty()) tab.assign((size_t)1024 * 3, 0.f);
    const int bytes = p.addBuf(p.w, p.h, 4);
    {
        P::WbBytes pc = {p.w, p.h, shift ? 1u : 0u, 0u};
        Pass& ps = p.add("wb_bytes", 0, bytes, p.w, p.h);
        ps.table = p.addTable(std::move(tab));
        ps.setPush(pc);
    }
    int stats = bytes;
    if (hasData) {
        const int hist = p.addBuf(768, 1, 1);
        stats = p.addBuf(8, 1, 1);
        P::WbClear cl = {768u, 1u};
        p.add("wb_clear", hist, hist, 768, 1).setPush(cl);
        P::WbHist ph = {p.w, p.h};
        p.add("wb_hist", bytes, hist, p.w, p.h).setPush(ph);
        P::WbStats pst = {3u, 1u};
        p.add("wb_stats", hist, stats, 3, 1).setPush(pst);
    }
    P::WbFinal pf = {p.w, p.h, hasData ? 1u : 0u, 0u};
    Pass& fin = p.add("wb_final", bytes, 1, p.w, p.h);
    fin.in1 = stats;
    fin.setPush(pf);
    return true;
}

// Linear Blur (vegasLinearBlur() in builtins_deriche.cpp): the picture in levels, sheared, VEGAS's Deriche blur, sheared back
bool planLinearBlur(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    LinearBlurParams lp;
    if (!vegasLinearBlurParams(W, H, c.d("Angle"), c.d("Amount"), lp)) {
        P::CopyPx cp = {p.w, p.h};
        p.add("copy_px", 0, 1, p.w, p.h).setPush(cp);
        return true;
    }
    const bool memory = lp.branch <= 2, across = lp.branch == 2 || lp.branch == 3;
    const uint32_t shear = (uint32_t)lp.shear;
    const uint32_t FW = across ? H : W, FH = across ? W : H;
    const uint32_t bufH = FH + (shear > 0 ? (shear + 16383) / 16384 : 0);
    const uint32_t frW = across ? bufH : FW, frH = across ? FW : bufH;
    const uint32_t inc = FW > 1 ? shear / (FW - 1) : 0;
    const uint32_t s0 = shear >> 14;
    std::vector<float> tab;
    if (inc == 0) tab.assign(1, 0.f);
    for (uint32_t r = 0; inc != 0 && r <= s0; ++r) {
        const int X = (int)((double)(((int)r << 14) + 0x2000) * 16384.0 / (double)inc);
        float f;
        std::memcpy(&f, &X, 4);
        tab.push_back(f);
    }
    const int pic = p.addBuf(p.w, p.h, 4), frame = p.addBuf(frW, frH, 4), blurred = p.addBuf(frW, frH, 4);
    P::LbPic pp = {p.w, p.h, c.levels ? 1u : 0u, 0u};
    p.add("lb_pic", 0, pic, p.w, p.h).setPush(pp);
    P::LbIn pi = {frW, frH, p.w, p.h, memory ? 1u : 0u, across ? 1u : 0u, shear, bufH, c.levels ? 1u : 0u, s0, 0u, 0u};
    Pass& pin = p.add("lb_in", pic, frame, frW, frH);
    pin.table = p.addTable(std::move(tab));
    pin.setPush(pi);
    gpuAddDeriche(p, frame, blurred, vegasBlurSigma(lp.sx), vegasBlurSigma(lp.sy), c.levels);
    P::LbOut po = {p.w, p.h, frW, frH, memory ? 1u : 0u, across ? 1u : 0u, shear, bufH, c.levels ? 1u : 0u, 0u};
    p.add("lb_out", blurred, 1, p.w, p.h).setPush(po);
    return true;
}

// Cookie Cutter (cookieCutter() in builtins_cookie.cpp): the mask by shape, VEGAS's Gaussian over it (feather), the mixed picture
bool planCookieCutter(Ctx& c, Plan& p)
{
    if (p.w == 0 || p.h == 0) return false;
    CookieSetup su;
    cookieSetup(c, (int)p.w, (int)p.h, su);
    const int mask = p.addBuf(p.w, p.h, 4);
    P::CookieMask0 pm = {p.w, p.h, (uint32_t)su.repX, (uint32_t)su.repY, (uint32_t)su.cw, (uint32_t)su.ch, su.noBorder ? 1u : 0u, 0u,
                         su.cxCell, su.cyCell, su.ratio, su.tIn, su.tOut, su.rIn, su.hx, su.hy, su.ix0, su.iy0, su.ovalW, su.ovalH, su.boxHi, su.boxLo, su.nTout};
    char name[32];
    std::snprintf(name, sizeof name, "cookie_mask%d", su.shape);
    p.add(name, 0, mask, p.w, p.h).setPush(pm);
    int m = mask;
    if (!su.noFeather) {
        const float s = su.fw2 + 1.5f;
        m = p.addBuf(p.w, p.h, 4);
        gpuAddDeriche(p, mask, m, vegasBlurSigma((double)(s * 1.0f)), vegasBlurSigma((double)(s * 1.0f)), false);
    }
    P::CookieApply pa = {p.w, p.h, su.noBorder ? 1u : 0u, su.cutSection ? 1u : 0u, c.levels ? 1u : 0u, su.colour[0], su.colour[1], su.colour[2], su.colour[3], su.br, su.bg, su.bb, 0u};
    Pass& ps = p.add("cookie_apply", 0, 1, p.w, p.h);
    ps.in1 = m;
    ps.setPush(pa);
    return true;
}

// Chroma Keyer (chromaKeyer() in builtins_filters.cpp): the mask bytes, VEGAS's float recursive blur along the rows then the columns (bottom row first), the keyed bytes
bool planChromaKeyer(Ctx& c, Plan& p)
{
    ChromaKeyerSetup su;
    chromaKeyerGpuSetup(c, (int)p.w, (int)p.h, su);
    std::vector<float> tab;
    for (int i = 0; i < 256; ++i) pushDouble(tab, (double)i * 0.00392156862745098);
    for (int i = 0; i < 256; ++i) pushDouble(tab, i ? 255.0 / (double)i : 0.0);
    const D2 KH = d2(su.kh), KS = d2(su.ks), KL = d2(su.kl);
    int m = p.addBuf(p.w, p.h, 1);
    P::CkMask pm = {p.w, p.h, su.krF, su.kgF, su.kbF, su.lowF, su.hiF, su.invRange, 0u, 0u, KH.h, KH.l, KS.h, KS.l, KL.h, KL.l};
    Pass& pmask = p.add("ck_mask", 0, m, p.w, p.h);
    pmask.table = p.addTable(std::move(tab));
    pmask.setPush(pm);
    if (su.blur) {
        const int plane = p.addBuf(p.w, p.h, 1), blurred = p.addBuf(p.w, p.h, 1), state = p.addBuf(p.w * 2, p.h, 1);
        for (int pass = 0; pass < 2; ++pass) {
            P::CkLine pl = {p.w, p.h, pass ? 1u : 0u, pass ? 1u : 0u};
            Pass& ps = p.add("ck_line", pass ? plane : m, pass ? blurred : plane, pass ? p.w : p.h, 1);
            ps.in1 = state;
            ps.table = p.addTable(std::vector<float>(su.coefs, su.coefs + 25));
            ps.setPush(pl);
        }
        m = blurred;
    }
    P::CkMix px = {p.w, p.h, su.maskOnly ? 1u : 0u, (uint32_t)su.despill};
    Pass& pm2 = p.add("ck_mix", 0, 1, p.w, p.h);
    pm2.in1 = m;
    pm2.setPush(px);
    return true;
}

// Glow (glow() in builtins_filters.cpp): VEGAS's Deriche Gaussian (8-bit: floats of 0..255 out), then the mix of Color by the blur's mean
bool planGlow(Ctx& c, Plan& p)
{
    const double pct = clampd(c.d("GlowPercent"), 0, 1);
    const float intensity = (float)c.d("Intensity"), supp = (float)clampd(c.d("Suppression"), 0, 1);
    const Color col = c.color("Color");
    const double size = (double)std::max(p.w, p.h) * pct * 0.5;
    const int blurred = p.addBuf(p.w, p.h, 4);
    gpuAddDeriche(p, 0, blurred, vegasBlurSigma(size), vegasBlurSigma(size), c.levels, c.levels);
    const double cc[4] = {col.r, col.g, col.b, col.a};
    int cb[4];
    for (int k = 0; k < 4; ++k) cb[k] = (int)(cc[k] * 255.0);
    P::GlowApply pc = {p.w, p.h, supp, intensity, c.levels ? 1u : 0u, cb[0], cb[1], cb[2], cb[3], (float)cc[0], (float)cc[1], (float)cc[2], (float)cc[3]};
    Pass& ps = p.add("glow_apply", 0, 1, p.w, p.h);
    ps.in1 = blurred;
    ps.setPush(pc);
    return true;
}

// Vignette (vignette() in builtins_more_creative.cpp)
bool planVignette(Ctx& c, Plan& p)
{
    VignetteSetup su;
    more::vignetteSetup(c, (int)p.w, (int)p.h, su);
    P::Vignette pc = {p.w, p.h, su.invert ? 1u : 0u, c.levels ? 1u : 0u, su.cxp, su.cyp, su.cs, su.sn, su.g, su.expo, su.rInner, su.ramp, su.ca, su.cb[0], su.cb[1], su.cb[2],
                      su.cf[0], su.cf[1], su.cf[2]};
    p.add("vignette", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

// Picture in Picture (pictureInPicture() in builtins_more_creative.cpp)
bool planPictureInPicture(Ctx& c, Plan& p)
{
    PipSetup su;
    more::pipSetup(c, (int)p.w, (int)p.h, su);
    if (c.levels) {          // VEGAS's fixed-point 8-bit renderer: the sampling matrix in floats
        P::Pip8 q = {p.w, p.h, su.M[0], su.M[1], su.M[2], su.M[3], su.M[4], su.M[5], su.M[6], su.M[7], su.M[8], (float)su.op};
        p.add("pip8", 0, 1, p.w, p.h).setPush(q);
        return true;
    }
    std::vector<float> tab;
    for (int i = 0; i < 9; ++i) pushDouble(tab, su.hm[i]);
    const double rest[9] = {su.sx, su.sy, su.cs, su.sn, su.shear, su.lxW, su.lyH, su.axW, su.ayH};
    for (int i = 0; i < 9; ++i) pushDouble(tab, rest[i]);
    P::Pip pc = {p.w, p.h, (uint32_t)su.mode, c.levels ? 1u : 0u, (float)su.op, 0u};
    Pass& ps = p.add("pip", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(tab));
    ps.setPush(pc);
    return true;
}

// Border (border() in builtins_filters.cpp): Blurred (type 0), Beveled (1) or Solid (2)
bool planBorder(Ctx& c, Plan& p)
{
    BorderGeo g;
    borderGeometry(c, (int)p.w, (int)p.h, g);
    if (g.W <= 0 || g.H <= 0) return false;
    if (g.type == 2) {
        P::BorderSolid pc = {p.w, p.h, g.xi, g.xr, g.yb, g.yt, g.cc[0], g.cc[1], g.cc[2], g.cc[3]};
        p.add("border_solid", 0, 1, p.w, p.h).setPush(pc);
        return true;
    }
    if (g.type == 0) {
        for (int i = 0; i < 3; ++i)
            if (g.rads[i] < 0 || 2 * g.rads[i] + 1 > std::min(g.W, g.H)) return false;      // (the CPU's edge cases of a window wider than the frame stay there)
        const int a = p.addBuf(p.w, p.h, 4), b = p.addBuf(p.w, p.h, 4);
        P::BorderMask mk = {p.w, p.h, g.xi, g.xr, g.yb, g.yt};
        p.add("border_mask", a, a, p.w, p.h).setPush(mk);
        int from = a, to = b;
        for (int i = 0; i < 3; ++i)
            for (int vert = 0; vert < 2; ++vert) {
                const float iarr = 1.f / (float)(g.rads[i] + g.rads[i] + 1);
                P::BorderBox bx = {p.w, p.h, (uint32_t)vert, (uint32_t)g.rads[i], iarr, 0u};
                p.add("border_box", from, to, vert ? p.w : p.h, 1).setPush(bx);
                std::swap(from, to);
            }
        P::BorderApply ap = {p.w, p.h, c.levels ? 1u : 0u, 0u};
        Pass& ps = p.add("border_apply", 0, 1, p.w, p.h);
        ps.in1 = from;
        ps.setPush(ap);
        return true;
    }
    std::vector<float> tab;
    tab.reserve((size_t)g.H * 25);
    for (int y = 0; y < g.H; ++y) {
        BorderSpans xs;
        borderBevelRow(g, y, xs);
        for (int k = 0; k < 5; ++k) tab.push_back((float)xs.end[k]);
        for (int k = 0; k < 5; ++k) tab.push_back((float)xs.cov[k]);
        for (int k = 0; k < 5; ++k) for (int ch = 0; ch < 3; ++ch) tab.push_back((float)xs.face[k][ch]);
    }
    P::BorderBevel pc = {p.w, p.h, c.levels ? 1u : 0u, 0u};
    Pass& ps = p.add("border_bevel", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(tab));
    ps.setPush(pc);
    return true;
}

// The generic Gaussian (gaussianBlur() in builtins.cpp) as float FIR passes: rows when sigma > 0.3, then columns
int firGaussPasses(Plan& p, int from, uint32_t w, uint32_t h, double sigmaX, double sigmaY)
{
    int cur = from;
    for (int vert = 0; vert < 2; ++vert) {
        const double sg = vert ? sigmaY : sigmaX;
        if (!(sg > 0.3)) continue;
        std::vector<float> k = gaussKernelTable(sg);
        const int to = p.addBuf(w, h, 4);
        P::FirGauss pc = {w, h, (uint32_t)vert, (uint32_t)(k.size() - 1)};
        Pass& ps = p.add("fir_gauss", cur, to, w, h);
        ps.table = p.addTable(std::move(k));
        ps.setPush(pc);
        cur = to;
    }
    return cur;
}

std::vector<float> bbfPosTable(double cx, double cy, double z, int W, int H)
{
    std::vector<float> tab;
    pushDouble(tab, cx * W);
    pushDouble(tab, cy * H);
    pushDouble(tab, z);
    return tab;
}

// Layer Dimensionality (layerDimensionality() in builtins_more_creative.cpp): the shadow stage, then glow, then emboss, each reading the picture the last left
// and the source frame's alpha (a plane in all four lanes)
bool planLayerDimensionality(Ctx& c, Plan& p)
{
    LdSetup su;
    more::ldSetup(c, (int)p.w, (int)p.h, su);
    const bool doGlow = su.glowType != 0;
    const int stages = 1 + (doGlow ? 1 : 0) + (su.emboss ? 1 : 0);
    int stage = 0;
    auto target = [&]() { return ++stage == stages ? 1 : p.addBuf(p.w, p.h, 4); };
    auto alphaPlane = [&](uint32_t mode, double sx, double sy, int ox, int oy) {
        const int pl = p.addBuf(p.w, p.h, 4);
        const D2 SX = d2(sx), SY = d2(sy);
        P::LdPlane pc = {p.w, p.h, SX.h, SX.l, SY.h, SY.l, mode, ox, oy};
        p.add("ld_plane", 0, pl, p.w, p.h).setPush(pc);
        return pl;
    };
    int cur = 0;
    {
        const int to = target();
        if (su.shadowType == 2) {
            const int pl = alphaPlane(1, su.sx, su.sy, 0, 0);
            const int bl = p.addBuf(p.w, p.h, 4);
            gpuAddDeriche(p, pl, bl, vegasBlurSigma(su.bsz), vegasBlurSigma(su.bsz), false);
            P::LdInner pc = {p.w, p.h, su.fgOp, su.amount};
            Pass& ps = p.add("ld_inner", 0, to, p.w, p.h);
            ps.in1 = bl;
            ps.setPush(pc);
        } else if (su.shadowType != 0) {
            const int pl = alphaPlane(2, 0, 0, su.ox, su.oy);
            const int bl = firGaussPasses(p, pl, p.w, p.h, su.blur, su.blur);
            P::LdDrop pc = {p.w, p.h, su.fgOp, su.amount};
            Pass& ps = p.add("ld_drop", 0, to, p.w, p.h);
            ps.in1 = bl;
            ps.setPush(pc);
        } else {
            P::LdBase pc = {p.w, p.h, su.fgOp};
            p.add("ld_base", 0, to, p.w, p.h).setPush(pc);
        }
        cur = to;
    }
    if (doGlow) {
        const int pl = alphaPlane(0, 0, 0, 0, 0);
        const int g = firGaussPasses(p, pl, p.w, p.h, su.glowRad, su.glowRad);
        const int a = p.addBuf(p.w, p.h, 4);
        P::LdGdiff pgd = {p.w, p.h, (su.glowType == 2 || su.glowType == 1) ? 1u : 0u, 0u};
        Pass& pg = p.add("ld_gdiff", 0, a, p.w, p.h);
        pg.in1 = g;
        pg.setPush(pgd);
        const int to = target();
        P::LdGlow pc = {p.w, p.h, su.gc[0], su.gc[1], su.gc[2], 0u};
        Pass& ps = p.add("ld_glow", cur, to, p.w, p.h);
        ps.in1 = a;
        ps.setPush(pc);
        cur = to;
    }
    if (su.emboss) {
        const int pl = alphaPlane(0, 0, 0, 0, 0);
        const int sm = firGaussPasses(p, pl, p.w, p.h, su.sigma, su.sigma);
        const int to = target();
        const D2 OX = d2(su.eox), OY = d2(su.eoy);
        P::LdEmboss pc = {p.w, p.h, OX.h, OX.l, OY.h, OY.l, su.gain, 0u};
        Pass& ps = p.add("ld_emboss", cur, to, p.w, p.h);
        ps.in1 = sm;
        ps.setPush(pc);
    }
    return true;
}

// Radial Pixelate (radialPixelate() in builtins_more_creative.cpp): unroll about the centre, block means, roll up again
bool planRadialPixelate(Ctx& c, Plan& p)
{
    RpixSetup su;
    more::rpixSetup(c, (int)p.w, (int)p.h, su);
    if (su.pass) {
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    const int W = (int)p.w;
    std::vector<float> tab;
    for (int i = 0; i < W; ++i) {
        const float a = (float)((double)(((float)i / (float)p.w) * 2.f - 1.f) * 3.141592653589793);
        tab.push_back((float)std::sin((double)a));
        tab.push_back((float)std::cos((double)a));
    }
    const int pol = p.addBuf(p.w, p.h, 4);
    P::RpixPolar pp = {p.w, p.h, su.c0, su.c1, su.q8 ? 1u : 0u, 0u};
    Pass& a = p.add("rpix_polar", 0, pol, p.w, p.h);
    a.table = p.addTable(std::move(tab));
    a.setPush(pp);
    const int smallBuf = p.addBuf((uint32_t)su.gw, (uint32_t)su.gh, 4);
    const D2 INV = d2(su.inv);
    P::RpixSmall ps = {p.w, p.h, (uint32_t)su.gw, (uint32_t)su.gh, su.sx, su.sy, su.bw, su.bh, su.q8 ? 1u : 0u, 0u, INV.h, INV.l};
    p.add("rpix_small", pol, smallBuf, (uint32_t)su.gw, (uint32_t)su.gh).setPush(ps);
    const int tx = (int)((double)W / su.gw * 16384.0), ty = (int)((double)(int)p.h / su.gh * 16384.0);
    P::RpixOut po = {p.w, p.h, (uint32_t)su.gw, (uint32_t)su.gh, tx, ty, su.c0, su.c1};
    p.add("rpix_out", smallBuf, 1, p.w, p.h).setPush(po);
    return true;
}

// AutoLooks (autoLooks() in builtins_color.cpp)
bool planAutoLooks(Ctx& c, Plan& p)
{
    int n = 0;
    float dmin[3], dmax[3], g8 = 0;
    std::vector<float> tab;
    if (!autoLooksGpu(c, n, dmin, dmax, g8, tab) || tab.size() < (size_t)n * n * n * 3) {
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    P::Autolooks pc = {p.w, p.h, (uint32_t)n, g8, dmin[0], dmin[1], dmin[2], dmax[0], dmax[1], dmax[2]};
    Pass& ps = p.add("autolooks", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(tab));
    ps.setPush(pc);
    return true;
}

// Tiny Planet, VEGAS's own (tinyPlanet() in builtins_magix.cpp, "de.magix:stereographicProjectionOCL") and Reno's (builtins_more.cpp)
// (the source pixel of every output pixel comes from the CPU's own libm code, cached while the settings and size stay: it does not depend on the picture)
bool planTinyPlanetMagix(Ctx& c, Plan& p)
{
    static std::mutex m;
    static std::vector<double> key;
    static std::vector<float> map;
    std::lock_guard<std::mutex> lock(m);
    const std::vector<double> k = {(double)p.w, (double)p.h, c.d("Z"), c.d("Latitude"), c.d("Longitude")};
    if (key != k) {
        key.clear();
        std::vector<uint32_t> idx;
        tinyPlanetMapGpu(c, (int)p.w, (int)p.h, idx);
        map.resize(idx.size());
        std::memcpy(map.data(), idx.data(), idx.size() * 4);
        key = k;
    }
    p.add("gather_tab", 0, 1, p.w, p.h).table = p.addTable(std::vector<float>(map));
    p.passes.back().setPush(P::GatherTab{p.w, p.h});
    return true;
}

bool planTinyPlanet(Ctx& c, Plan& p)
{
    const double tilt = (c.d("Latitude") - 90.0) * kPi / 180.0, lon = c.d("Longitude") * kPi / 180.0, zoom = std::max(0.05, c.d("Z"));
    const double ct = std::cos(tilt), st = std::sin(tilt);
    const double R = 0.5 * c.frameH * zoom;
    if (!(R > 0)) return false;
    const D2 A = d2(ct), B = d2(st), C = d2(R), D = d2(lon);
    P::Tpr pc = {p.w, p.h, A.h, A.l, B.h, B.l, C.h, C.l, D.h, D.l};
    p.add("tpr", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

// Radial Blur (radialBlur() in builtins_more_blur.cpp): an in-place pass, run in dependency steps worked out from the CPU's loops (cached by the geometry)
bool planRadialBlur(Ctx& c, Plan& p)
{
    RbSetup g;
    more::radialBlurSetup(c, (int)p.w, (int)p.h, g);
    if (g.pass) {
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    static std::mutex m;
    static std::vector<double> key;
    static bool okSteps = false;
    static std::vector<uint32_t> rec, starts;
    static std::vector<float> tab;
    std::lock_guard<std::mutex> lock(m);
    const std::vector<double> k = {(double)p.w, (double)p.h, (double)g.type, (double)g.cx, (double)g.cy, g.s0, g.s1};
    if (key != k) {
        key.clear();
        okSteps = more::radialBlurSteps(g, (int)p.w, (int)p.h, rec, starts, 1000);
        tab.resize(rec.size());
        if (okSteps) std::memcpy(tab.data(), rec.data(), rec.size() * 4);
        key = k;
    }
    if (!okSteps) return false;        // (more than 1000 steps: about HD and up, where the host replay (~100 ms at 1080p, ~350 ms at 4K) costs as much as the CPU effect)
    const int S = p.addBuf(p.w, p.h, 4), D = p.addBuf(p.w, p.h, 4);
    p.add("rb_prep", 0, S, p.w, p.h).setPush(P::RbPrep{p.w, p.h, g.q8 ? 1u : 0u, 0u});
    p.add("copy_px", S, D, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
    const int tb = p.addTable(std::vector<float>(tab));
    const D2 A = d2(g.s0), B = d2(g.s1), C = d2(g.wf), E = d2(g.iwf);
    uint32_t from = 0;
    for (size_t st = 0; st < starts.size(); ++st) {
        const uint32_t to = starts[st], cnt = to - from;
        if (cnt) {
            P::RbStep pc = {p.w, p.h, (uint32_t)g.type, g.q8 ? 1u : 0u, g.cx, g.cy, from, cnt, (int32_t)g.wi, (int32_t)g.iwi, A.h, A.l, B.h, B.l, C.h, C.l, E.h, E.l};
            Pass& ps = p.add("rb_step", D, D, cnt, 1);
            ps.in1 = S;
            ps.table = tb;
            ps.setPush(pc);
        }
        from = to;
    }
    p.add("rb_fin", D, 1, p.w, p.h).setPush(P::RbFin{p.w, p.h});
    return true;
}

// Histogram (histogramFx() in builtins_vegasrest.cpp): the counts of 128 bins (whole numbers, so any order), then the box drawn over the picture. The Lab types and the
// logarithmic scale use libm and stay on the CPU.
bool planHistogram(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    const int type = c.i("Type");
    if (type > 4 || c.b("LogarithmicScale")) return false;
    if (W < 8 || H < 8) {
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    const double hdr = std::max(1.0, c.d("HDR"));
    const int hist = p.addBuf(128, 1, 1);
    p.add("wb_clear", hist, hist, 128, 1).setPush(P::WbClear{128u, 1u});
    const D2 HD = d2(hdr);
    P::HistCount pc = {p.w, p.h, (uint32_t)type, 0u, HD.h, HD.l};
    p.add("hist_count", 0, hist, p.w, p.h).setPush(pc);
    double cxn, cyn;
    c.xy("Center", cxn, cyn);
    const int bw = std::max(16, W / 3), bh = std::max(12, H / 4);
    const int x0 = (int)std::lround(cxn * W - bw * 0.5), y0 = (int)std::lround((1.0 - cyn) * H - bh * 0.5);
    const float back = 1.f - (float)clampd(c.d("Transparency"), 0, 1);
    const float cr = type == 1 ? 1.f : (type == 2 ? 0.3f : (type == 3 ? 0.3f : 1.f));
    const float cg = type == 2 ? 1.f : (type == 1 ? 0.3f : (type == 3 ? 0.3f : 1.f));
    const float cb = type == 3 ? 1.f : (type == 1 ? 0.3f : (type == 2 ? 0.3f : 1.f));
    P::HistDraw pd = {p.w, p.h, x0, y0, bw, bh, back, cr, cg, cb};
    Pass& ps = p.add("hist_draw", 0, 1, p.w, p.h);
    ps.in1 = hist;
    ps.setPush(pd);
    return true;
}

// Slow Motion: the frame is not changed by the effect (the engine reads the settings; slowMotion() in builtins_more.cpp copies the frame)
bool planSlowMotion(Ctx&, Plan& p)
{
    p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
    return true;
}

// Bezier Masking (bezierMasking() in builtins_mask_bezier.cpp): the masks do not depend on the picture, so the CPU's code makes their total (cached while the settings stay)
bool planBezierMasking(Ctx& c, Plan& p)
{
    int mode = 0;
    bool invert = false;
    float blend = 0;
    std::string key;
    bezierMaskGpu(c, (int)p.w, (int)p.h, mode, invert, blend, key, nullptr);
    if (mode == 0) {
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    if (mode == 1) {        // (no mask on: nothing shows)
        P::BzApply pc = {p.w, p.h, 0u, 0.f};
        p.add("bz_apply", 0, 1, p.w, p.h).table = p.addTable(std::vector<float>((size_t)p.w * p.h, 0.f));
        p.passes.back().setPush(pc);
        return true;
    }
    static std::mutex m;
    static std::string cached;
    static std::vector<float> total;
    std::lock_guard<std::mutex> lock(m);
    if (cached != key || total.size() != (size_t)p.w * p.h) {
        cached.clear();
        int md; bool inv; float bl; std::string k2;
        bezierMaskGpu(c, (int)p.w, (int)p.h, md, inv, bl, k2, &total);
        if (total.size() != (size_t)p.w * p.h) total.assign((size_t)p.w * p.h, 0.f);
        cached = key;
    }
    P::BzApply pc = {p.w, p.h, invert ? 1u : 0u, blend};
    Pass& ps = p.add("bz_apply", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::vector<float>(total));
    ps.setPush(pc);
    return true;
}

// Stereoscopic 3D Adjust (vegasStereo3dAdjust() in builtins_stereo.cpp): the CPU's code works out where every pixel is taken from (VEGAS's stretch or OpenCV's warp, integers and doubles),
// the kernel does the 8-bit sampling
bool planStereo3d(Ctx& c, Plan& p)
{
    std::vector<float> rec;
    unsigned full = 0, part = 0;
    if ((size_t)p.w * p.h > (size_t)4000000) return false;
    if (!stereoTapsGpu(c, (int)p.w, (int)p.h, rec, full, part)) {
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    Pass& ps = p.add("stereo3d", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(rec));
    ps.setPush(P::Stereo3d{p.w, p.h, full, part});
    return true;
}

// Dual Fish Eye Stitching (dualFishEye() in builtins_vr.cpp): the CPU's code makes where every pixel comes from (libm geometry, independent of the colours), the kernel samples and blends
bool planDualFishEye(Ctx& c, Plan& p)
{
    std::vector<float> rec;
    bool levels = false;
    float ng = 0;
    if ((size_t)p.w * p.h > (size_t)5000000) return false;
    if (!stitchMapGpu(c, (int)p.w, (int)p.h, rec, levels, ng)) {
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    Pass& ps = p.add("vr_stitch", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(rec));
    ps.setPush(P::VrStitch{p.w, p.h, levels ? 1u : 0u, 0u});
    return true;
}

// Lens Correction (lensCorrection() in builtins_mask.cpp)
bool planLensCorrection(Ctx& c, Plan& p)
{
    LensSetup su;
    if (!lensSetupGpu(c, (int)p.w, (int)p.h, su)) return false;
    if (su.plain) {
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    P::Lens pc = {p.w, p.h, c.levels ? 1u : 0u, 0u, su.cxp, su.cyp, su.d, su.x14, su.x3, su.x13, su.s, su.iw, su.ih};
    p.add("lens", 0, 1, p.w, p.h).setPush(pc);
    return true;
}

// Fill Light (fillLightVelvet() in builtins_velvet.cpp): the luma's colour as bytes, CoreGraphics' float Gaussian blur, the Add
bool planFillLight(Ctx& c, Plan& p)
{
    FillLightSetup su;
    more::fillLightSetup(c, (int)p.w, su);
    if (su.copy) {
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    std::vector<float> tab(256 * 4);
    for (int i = 0; i < 256; ++i)
        for (int k = 0; k < 4; ++k) tab[(size_t)i * 4 + k] = (float)su.lut[i][k];
    const int m = p.addBuf(p.w, p.h, 4);
    {
        Pass& ps = p.add("fl_mask", 0, m, p.w, p.h);
        ps.table = p.addTable(tab);
        ps.setPush(P::FlMask{p.w, p.h});
    }
    if (su.blur) {
        // GaussianBlurOCL's recursive Gaussian (oclBlurCoefs() in builtins_cgblur.cpp), rows then columns, each into a new buffer
        const OclBlurCoefs cf = oclBlurCoefs(oclBlurSigma(su.sigma));
        const int b = p.addBuf(p.w, p.h, 4), b2 = p.addBuf(p.w, p.h, 4);
        auto oc = [&](uint32_t vert) {
            return P::OclBlur{p.w, p.h, vert, (float)cf.n[0], (float)cf.n[1], (float)cf.n[2], (float)cf.d[1], (float)cf.d[2], (float)cf.d[3], (float)cf.t[1], (float)cf.t[2], (float)cf.t[3], (float)cf.gc, (float)cf.ga};
        };
        p.add("ocl_blur", m, b, p.h, 1).setPush(oc(0u));
        p.add("ocl_blur", b, b2, p.w, 1).setPush(oc(1u));
        Pass& ps = p.add("fl_final", 0, 1, p.w, p.h);
        ps.in1 = b2;
        ps.setPush(P::FlFinal{p.w, p.h, su.fop, 1u});
    } else {
        Pass& ps = p.add("fl_final", 0, 1, p.w, p.h);
        ps.in1 = m;
        ps.setPush(P::FlFinal{p.w, p.h, su.fop, 0u});
    }
    return true;
}

// Soft Contrast (softContrast() in builtins_velvet.cpp); the vignette's blurred exterior (Exterior effect: Blur) stays on the CPU
bool planSoftContrast(Ctx& c, Plan& p)
{
    ScSetup su;
    more::softContrastSetup(c, (int)p.w, (int)p.h, su);
    if (su.ext == 1) return false;
    std::vector<float> st(256);
    for (int i = 0; i < 256; ++i) st[(size_t)i] = (float)su.stl[i];
    const int a = p.addBuf(p.w, p.h, 4), g = p.addBuf(p.w, p.h, 4);
    {
        Pass& ps = p.add("sc_stretch", 0, a, p.w, p.h);
        ps.table = p.addTable(st);
        ps.setPush(P::ScStretch{p.w, p.h});
    }
    p.add("sc_gray", a, g, p.w, p.h).setPush(P::ScGray{p.w, p.h});
    if (su.blur) {
        const CgBlurCoefs cf = cgBlurCoefs(su.sigma);
        p.add("cg_blur", g, g, p.h, 1).setPush(P::CgBlur{p.w, p.h, 0u, cf.gain, cf.c1, cf.c2, cf.c3});
        p.add("cg_blur", g, g, p.w, 1).setPush(P::CgBlur{p.w, p.h, 1u, cf.gain, cf.c1, cf.c2, cf.c3});
    }
    std::vector<float> tab(1024 + (su.ext != 0 ? su.vmask.size() : 1), 0.f);
    if (su.trim)
        for (int i = 0; i < 256; ++i) tab[(size_t)i] = (float)su.tl[i];
    if (su.tint)
        for (int i = 0; i < 256; ++i)
            for (int k = 0; k < 3; ++k) tab[256 + (size_t)k * 256 + i] = (float)su.tl3[i][k];
    if (su.ext != 0) std::copy(su.vmask.begin(), su.vmask.end(), tab.begin() + 1024);
    const uint32_t flags = (su.blur ? 1u : 0u) | (su.trim ? 2u : 0u) | (su.tint ? 4u : 0u) | (su.vscale ? 8u : 0u);
    Pass& ps = p.add("sc_final", a, 1, p.w, p.h);
    ps.in1 = g;
    ps.table = p.addTable(tab);
    ps.setPush(P::ScFinal{p.w, p.h, su.oq, su.ext, su.vq, flags});
    return true;
}

// Glint (glintVelvet() in builtins_velvet.cpp): the streak planes are byte planes in one stack; streak i shears every plane, blurs plane i across the shear (the float Gaussian)
// and shears them all back; Screen of the planes in order. The mask-only mode stays on the CPU.
bool planGlint(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    GlintSetup su;
    if (!more::glintSetup(c, W, H, su)) return false;
    const size_t limit = (size_t)64 * 1024 * 1024;
    size_t maxShear = (size_t)W * H;
    for (const DbGeom& d : su.streaks)
        if (d.mode == 3) maxShear = std::max(maxShear, (size_t)d.SW * (size_t)d.SH);
    if ((size_t)su.n * maxShear > limit) return false;
    std::vector<float> mtab = su.mask;
    if (mtab.empty()) mtab.push_back(0.f);
    const uint32_t mode = (uint32_t)su.maskMode;
    const int mt = p.addTable(mtab);
    const int g = p.addBuf(p.w, p.h, 4);
    {
        Pass& ps = p.add("gl_g", 0, g, p.w, p.h);
        ps.table = mt;
        ps.setPush(P::GlG{p.w, p.h, mode, 0u});
    }
    if (su.flicker) {
        const int t = p.addBuf(p.w, p.h, 4);
        p.add("gl_b3h", g, t, p.w, p.h).setPush(P::GlB3h{p.w, p.h});
        p.add("gl_b3v", t, g, p.w, p.h).setPush(P::GlB3v{p.w, p.h});
    }
    std::vector<float> clutTab(1024 + mtab.size());
    for (int i = 0; i < 256; ++i)
        for (int k = 0; k < 4; ++k) clutTab[(size_t)i * 4 + k] = (float)su.clut[i][k];
    std::copy(mtab.begin(), mtab.end(), clutTab.begin() + 1024);
    const int ct = p.addTable(clutTab);
    const uint32_t n = (uint32_t)su.n;
    int stack = -1;
    if (n > 0) {
        std::vector<float> tl(su.tlut, su.tlut + 256);
        auto words = [&](size_t w) { return p.addBuf(1024, (uint32_t)((w + 4095) / 4096), 4); };
        stack = words((size_t)n * W * H);
        {
            Pass& ps = p.add("gl2_init", g, stack, p.w, p.h * n);
            ps.table = p.addTable(tl);
            ps.setPush(P::Gl2Init{p.w, p.h, n, 0u});
        }
        const int sh = maxShear > (size_t)W * H ? words((size_t)n * maxShear) : -1;
        const int fl = p.addBuf(1024, (uint32_t)((maxShear + 1023) / 1024), 4);
        auto cg = [&](int buf, uint32_t w, uint32_t h, double sx, double sy) {
            const CgBlurCoefs cx = cgBlurCoefs(sx), cy = cgBlurCoefs(sy);
            p.add("cg_blur", buf, buf, h, 1).setPush(P::CgBlur{w, h, 0u, cx.gain, cx.c1, cx.c2, cx.c3});
            p.add("cg_blur", buf, buf, w, 1).setPush(P::CgBlur{w, h, 1u, cy.gain, cy.c1, cy.c2, cy.c3});
        };
        for (uint32_t i = 0; i < n; ++i) {
            const DbGeom& d = su.streaks[i];
            if (d.mode == 0) continue;
            if (d.mode == 1 || d.mode == 2) {
                const uint32_t off = i * p.w * p.h;
                p.add("gl2_tof", stack, fl, p.w, p.h).setPush(P::Gl2Tof{p.w, p.h, off, 0u});
                if (d.mode == 1) cg(fl, p.w, p.h, 0.0, d.sigma); else cg(fl, p.w, p.h, d.sigma, 0.0);
                p.add("gl2_fromf", fl, stack, p.w, p.h).setPush(P::Gl2Fromf{p.w, p.h, off, su.scale});
                continue;
            }
            const uint32_t SW = (uint32_t)d.SW, SH = (uint32_t)d.SH, rv = d.rev ? 1u : 0u, W32 = p.w, H32 = p.h;
            const uint32_t lines = d.cols ? W32 : H32;
            for (int dir = 0; dir < 2; ++dir) {
                // dir 0: the planes (W x H) to the sheared ones (SW x SH) by t; dir 1: back by -t
                const double t = dir == 0 ? d.t : -d.t;
                std::vector<float> tab;
                more::shearParamsGpu((int)lines, t, tab);
                uint32_t len, lineStep, posStep, dlen, dLineStep, dPosStep, srcPlane, dstPlane;
                if (dir == 0) {
                    srcPlane = W32 * H32; dstPlane = SW * SH;
                    if (d.cols) { len = H32; lineStep = 1; posStep = W32; dlen = SH; dLineStep = 1; dPosStep = SW; }
                    else { len = W32; lineStep = W32; posStep = 1; dlen = SW; dLineStep = SW; dPosStep = 1; }
                } else {
                    srcPlane = SW * SH; dstPlane = W32 * H32;
                    if (d.cols) { len = SH; lineStep = 1; posStep = SW; dlen = H32; dLineStep = 1; dPosStep = W32; }
                    else { len = SW; lineStep = SW; posStep = 1; dlen = W32; dLineStep = W32; dPosStep = 1; }
                }
                Pass& ps = p.add("gl2_shear", dir == 0 ? stack : sh, dir == 0 ? sh : stack, dlen, lines * n);
                ps.table = p.addTable(std::move(tab));
                ps.setPush(P::Gl2Shear{dlen, lines * n, lines, len, lineStep, posStep, dLineStep, dPosStep, rv, (t >= 0) ? 0u : 1u, srcPlane, dstPlane});
                if (dir == 0) {
                    const uint32_t off = i * SW * SH;
                    p.add("gl2_tof", sh, fl, SW, SH).setPush(P::Gl2Tof{SW, SH, off, 0u});
                    if (d.cols) cg(fl, SW, SH, d.sigma, 0.0); else cg(fl, SW, SH, 0.0, d.sigma);
                    p.add("gl2_fromf", fl, sh, SW, SH).setPush(P::Gl2Fromf{SW, SH, off, su.scale});
                }
            }
        }
    }
    Pass& ps = p.add("gl2_final", 0, 1, p.w, p.h);
    ps.in1 = n > 0 ? stack : 0;
    ps.table = ct;
    ps.setPush(P::Gl2Final{p.w, p.h, n, mode, su.effectOnly ? 1u : 0u, 0u});
    return true;
}

// Rays (raysVelvet() in builtins_velvet.cpp): the byte luma (or alpha) through the threshold table, the noise, the blur passes, Screen through the colour table. Mask-only mode stays on the CPU.
bool planRays(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    RaysSetup su;
    if (!more::raysSetup(c, W, H, su)) return false;
    std::vector<float> mtab = su.mask;
    if (mtab.empty()) mtab.push_back(0.f);
    const uint32_t mode = (uint32_t)su.maskMode;
    int g = p.addBuf(p.w, p.h, 4);
    {
        Pass& ps = p.add("gl_g", 0, g, p.w, p.h);
        ps.table = p.addTable(mtab);
        ps.setPush(P::GlG{p.w, p.h, mode, su.alpha ? 1u : 0u});
    }
    if (su.flicker) {
        const int t = p.addBuf(p.w, p.h, 4);
        p.add("gl_b3h", g, t, p.w, p.h).setPush(P::GlB3h{p.w, p.h});
        p.add("gl_b3v", t, g, p.w, p.h).setPush(P::GlB3v{p.w, p.h});
    }
    std::vector<float> tl(su.tlut, su.tlut + 256);
    int m = p.addBuf(p.w, p.h, 4);
    {
        Pass& ps = p.add("gl_plane", g, m, p.w, p.h);
        ps.table = p.addTable(tl);
        ps.setPush(P::GlPlane{p.w, p.h});
    }
    auto axisTab = [&](const std::vector<int>& xi, const std::vector<float>& xf, const std::vector<int>& yi, const std::vector<float>& yf) {
        std::vector<float> t;
        for (int v : xi) t.push_back((float)v);
        t.insert(t.end(), xf.begin(), xf.end());
        for (int v : yi) t.push_back((float)v);
        t.insert(t.end(), yf.begin(), yf.end());
        return t;
    };
    if (su.noise) {
        std::vector<float> t = axisTab(su.nxi, su.nxf, su.nyi, su.nyf);
        t.insert(t.end(), su.tex.begin(), su.tex.end());
        Pass& ps = p.add("ry_noise", m, m, p.w, p.h);
        ps.table = p.addTable(std::move(t));
        ps.setPush(P::RyNoise{p.w, p.h});
    }
    if (!su.passes.empty()) {
        int other = p.addBuf(p.w, p.h, 4);
        const float w1 = 1.f - 0.35f, w2 = 0.35f;
        for (const RaysSetup::Pass& rp : su.passes) {
            Pass& ps = p.add("ry_pass", m, other, p.w, p.h);
            ps.table = p.addTable(axisTab(rp.xi, rp.xf, rp.yi, rp.yf));
            ps.setPush(P::RyPass{p.w, p.h, w1, w2});
            std::swap(m, other);
        }
    }
    std::vector<float> t(1024 + mtab.size());
    for (int i = 0; i < 256; ++i)
        for (int k = 0; k < 4; ++k) t[(size_t)i * 4 + k] = (float)su.clut[i][k];
    std::copy(mtab.begin(), mtab.end(), t.begin() + 1024);
    Pass& ps = p.add("ry_final", 0, 1, p.w, p.h);
    ps.in1 = m;
    ps.table = p.addTable(std::move(t));
    ps.setPush(P::RyFinal{p.w, p.h, mode, su.effectOnly ? 1u : 0u});
    return true;
}

// 360 Stabilization (rotatePanorama() in builtins_vegasrest.cpp): the taps of every pixel from the CPU's own map, the pole colours summed in order
bool planStabilization360(Ctx& c, Plan& p)
{
    PanoSetup su;
    if (!panoramaSetupGpu(c, (int)p.w, (int)p.h, su)) return false;
    if (su.copy) {
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    const int pol = p.addBuf(4, 1, 4);
    const int tab = p.addTable(su.map);
    for (int k = 0; k < su.n; ++k) {
        const uint32_t pw = (uint32_t)su.pw, ph = (uint32_t)su.ph, px = (uint32_t)su.px[k], py = (uint32_t)su.py[k], part = (uint32_t)k;
        p.add("pano_pole", 0, pol, 2, 1).setPush(P::PanoPole{p.w, p.h, pw, ph, px, py, part, 0u});
        Pass& ps = p.add("pano_gather", 0, 1, pw, ph);
        ps.in1 = pol;
        ps.table = tab;
        ps.setPush(P::PanoGather{p.w, p.h, pw, ph, px, py, part, 0u});
    }
    return true;
}

// Scene Rotation (sceneRotateKernel() in builtins_vegasrest.cpp, the plug-in's OpenCL kernel): the taps of every pixel from the CPU's own code, then the sampler
bool planSceneRotation(Ctx& c, Plan& p)
{
    SceneSetup su;
    if (!sceneRotationSetupGpu(c, (int)p.w, (int)p.h, su)) return false;
    Pass& ps = p.add("scene_gather", 0, 1, p.w, p.h);
    ps.table = p.addTable(su.map);
    ps.setPush(P::SceneGather{p.w, p.h, su.stereo != 0 ? 1u : 0u, su.levels ? 1u : 0u});
    return true;
}

// Motion Tracker (motionTracker() in builtins_analysis.cpp): the picture as it is; the region's outline (Show motion vectors with a region set) is drawn on the CPU
bool planMotionTracker(Ctx& c, Plan& p)
{
    bool draw = c.b("ShowMotionVectors");
    if (draw) {
        static const char* const names[4] = {"CornerTL", "CornerTR", "CornerBR", "CornerBL"};
        for (int k = 0; k < 4; ++k) {
            double x, y;
            c.xy(names[k], x, y);
            if (!(x >= 0 && x <= 1 && y >= 0 && y <= 1)) draw = false;
        }
    }
    if (draw) return false;
    p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
    return true;
}

// Light Rays (lightRays() in builtins_more_light.cpp, marchTowardsCentre() in builtins_fast2.cpp): rays of samples and running sums in doubles, a chunk of rays a pass
bool planLightRays(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    LrSetup su;
    more::lightRaysSetup(c, W, H, su);
    PolarGpu g;
    if (!polarGeomGpu(W, H, su.px0, su.py0, 0.35, g)) return false;
    if ((size_t)g.nT * (size_t)g.rN > (size_t)24 * 1024 * 1024) return false;
    const uint32_t nT = (uint32_t)g.nT, rN = (uint32_t)g.rN, chunk = 128u;
    std::vector<float> tab;
    tab.insert(tab.end(), g.cs.begin(), g.cs.end());
    tab.insert(tab.end(), g.sn.begin(), g.sn.end());
    for (int v : g.rEnd) tab.push_back((float)v);
    const int tb = p.addTable(std::move(tab));
    const size_t words = (size_t)chunk * (((size_t)rN + 8) * 4 + ((size_t)rN + 9) * 12);
    const int st = p.addBuf(1024, (uint32_t)((words + 4095) / 4096), 4);
    const int res = p.addBuf(rN, nT, 4);
    const double span = 0.6, fmin = 1.0 - span;
    const D2 sp = d2(span), fm = d2(fmin);
    const float cx = (float)g.px0 - 0.5f, cy = (float)g.py0 - 0.5f;
    for (uint32_t t0 = 0; t0 < nT; t0 += chunk) {
        Pass& ps = p.add("lr_ray", 0, res, chunk, 1);
        ps.in1 = st;
        ps.table = tb;
        ps.setPush(P::LrRay{p.w, p.h, nT, rN, t0, chunk, cx, cy, su.sensF, 0u, sp.h, sp.l, fm.h, fm.l});
    }
    const D2 px = d2(g.px0), py = d2(g.py0);
    Pass& ps = p.add("lr_read", 0, 1, p.w, p.h);
    ps.in1 = res;
    ps.setPush(P::LrRead{p.w, p.h, nT, rN, (float)g.nT / 6.28318531f, su.kk[0], su.kk[1], su.kk[2], su.fb, 0u, px.h, px.l, py.h, py.l});
    return true;
}

// Black Bar Fill on 8-bit pictures (blackBarFill8() in builtins_more_creative.cpp): bottom row first, byte values
bool planBlackBarFill8(Ctx& c, Plan& p)
{
    (void)c;
    Bbf8Setup su;
    more::bbf8Setup(c, (int)p.w, (int)p.h, su);
    std::vector<float> tab(su.tab, su.tab + 256);
    auto sample = [&](const float* m) {
        const int o = p.addBuf(p.w, p.h, 4);
        P::Bbf8Sample pc = {p.w, p.h, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]};
        p.add("bbf8_sample", 0, o, p.w, p.h).setPush(pc);
        return o;
    };
    int bg = sample(su.mBg);
    if (su.bsize != 0.0) {
        const int b2 = p.addBuf(p.w, p.h, 4);
        gpuAddDeriche(p, bg, b2, vegasBlurSigma(su.bsize), vegasBlurSigma(su.bsize), true);
        bg = b2;
    }
    const int fg = sample(su.mFg);
    int pl = p.addBuf(p.w, p.h, 4);
    {
        Pass& ps = p.add("bbf8_pl", fg, pl, p.w, p.h);
        ps.table = p.addTable(tab);
        ps.setPush(P::Bbf8Pl{p.w, p.h});
    }
    if (su.s8 > 0.f) {
        const int b2 = p.addBuf(p.w, p.h, 4);
        gpuAddDeriche(p, pl, b2, vegasBlurSigma(su.shadowSize), vegasBlurSigma(su.shadowSize), false);
        pl = b2;
    }
    const int lay = p.addBuf(p.w, p.h, 4);
    {
        Pass& ps = p.add("bbf8_lay", fg, lay, p.w, p.h);
        ps.in1 = pl;
        ps.table = p.addTable(tab);
        ps.setPush(P::Bbf8Lay{p.w, p.h});
    }
    Pass& ps = p.add("bbf8_over", lay, 1, p.w, p.h);
    ps.in1 = bg;
    ps.table = p.addTable(tab);
    ps.setPush(P::Bbf8Over{p.w, p.h});
    return true;
}

// Black Bar Fill (blackBarFill() in builtins_more_creative.cpp)
bool planBlackBarFill(Ctx& c, Plan& p)
{
    if (c.levels) return planBlackBarFill8(c, p);
    const int W = (int)p.w, H = (int)p.h;
    BbfSetup su;
    more::bbfSetup(c, W, H, su);
    int cur = p.addBuf(p.w, p.h, 4);
    {
        P::BbfBg pc = {p.w, p.h};
        Pass& ps = p.add("bbf_bg", 0, cur, p.w, p.h);
        ps.table = p.addTable(bbfPosTable(su.bx, su.by, su.bs, W, H));
        ps.setPush(pc);
    }
    if (su.bgBlurred) {
        if (su.bf > 1) {
            const int bf = su.bf, lw = (W + bf - 1) / bf, lh = (H + bf - 1) / bf;
            const int low = p.addBuf((uint32_t)lw, (uint32_t)lh, 4);
            P::BbfDecim dc = {(uint32_t)lw, (uint32_t)lh, p.w, p.h, (uint32_t)bf, 0u};
            p.add("bbf_decim", cur, low, (uint32_t)lw, (uint32_t)lh).setPush(dc);
            const int blurred = firGaussPasses(p, low, (uint32_t)lw, (uint32_t)lh, su.blur / bf, su.blur / bf);
            const int up = p.addBuf(p.w, p.h, 4);
            P::BbfUp uc = {p.w, p.h, (uint32_t)lw, (uint32_t)lh, (uint32_t)bf, 0u};
            p.add("bbf_up", blurred, up, p.w, p.h).setPush(uc);
            cur = up;
        } else cur = firGaussPasses(p, cur, p.w, p.h, su.blur, su.blur);
    }
    if (c.levels && (su.bgBlurred || !su.bgWhole)) {
        const int fixed = p.addBuf(p.w, p.h, 4);
        P::BbfFix fc = {p.w, p.h, su.bgBlurred ? 1u : 0u, 0u};
        p.add("bbf_fix", cur, fixed, p.w, p.h).setPush(fc);
        cur = fixed;
    }
    if (su.shadow > 0.001) {
        const int sf = su.sf, lw = (W + sf - 1) / sf, lh = (H + sf - 1) / sf;
        const int mask = p.addBuf((uint32_t)lw, (uint32_t)lh, 4);
        P::BbfMask mc = {(uint32_t)lw, (uint32_t)lh, p.w, p.h, (uint32_t)sf, 0u};
        Pass& pm = p.add("bbf_mask", mask, mask, (uint32_t)lw, (uint32_t)lh);
        pm.table = p.addTable(bbfPosTable(su.fx, su.fy, su.fs, W, H));
        pm.setPush(mc);
        int sh = firGaussPasses(p, mask, (uint32_t)lw, (uint32_t)lh, su.sigma / sf, su.sigma / sf);
        if (sf > 1) {
            const int up = p.addBuf(p.w, p.h, 4);
            P::BbfUp uc = {p.w, p.h, (uint32_t)lw, (uint32_t)lh, (uint32_t)sf, 0u};
            p.add("bbf_up", sh, up, p.w, p.h).setPush(uc);
            sh = up;
        }
        const int shaded = p.addBuf(p.w, p.h, 4);
        P::BbfShade sc = {p.w, p.h, c.levels ? 1u : 0u, 0u};
        Pass& ps = p.add("bbf_shade", cur, shaded, p.w, p.h);
        ps.in1 = sh;
        ps.setPush(sc);
        cur = shaded;
    }
    P::BbfFinal fc = {p.w, p.h, c.levels ? 1u : 0u, su.fgWhole ? 1u : 0u};
    Pass& ps = p.add("bbf_final", 0, 1, p.w, p.h);
    ps.in1 = cur;
    ps.table = p.addTable(bbfPosTable(su.fx, su.fy, su.fs, W, H));
    ps.setPush(fc);
    return true;
}

// Newsprint (vegasNewsprint() in builtins_newsprint.cpp)
bool planNewsprint(Ctx& c, Plan& p)
{
    NewsGeo g;
    newsprintGeometry(c, (int)p.w, (int)p.h, g);
    if (!g.ok) {            // the CPU hands the picture back (no dots, a frame too small for cells)
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    if (g.dRx <= 0 || g.dRy <= 0 || g.nrm <= 0) return false;
    const int bytes = p.addBuf(p.w, p.h, 4);
    p.add("news_bytes", 0, bytes, p.w, p.h).setPush(P::NewsBytes{p.w, p.h});
    const int smallBuf = p.addBuf((uint32_t)g.sw, (uint32_t)g.sh, 4);
    {
        std::vector<float> tab;
        pushDouble(tab, 1.0 / (double)g.nrm);
        std::vector<NewsAxisCell> ax, ay;
        newsprintAxis((int)g.dRx, (int)g.sx, g.sw, ax);
        newsprintAxis((int)g.dRy, (int)g.sy, g.sh, ay);
        for (const NewsAxisCell& a : ax) { tab.push_back((float)a.S); tab.push_back((float)a.E); tab.push_back((float)a.F); tab.push_back((float)a.L); }
        for (const NewsAxisCell& a : ay) { tab.push_back((float)a.S); tab.push_back((float)a.E); tab.push_back((float)a.F); tab.push_back((float)a.L); }
        P::NewsShrink pc = {(uint32_t)g.sw, (uint32_t)g.sh, p.w, p.h, (uint32_t)ax.size(), (uint32_t)ay.size()};
        Pass& ps = p.add("news_shrink", bytes, smallBuf, (uint32_t)g.sw, (uint32_t)g.sh);
        ps.table = p.addTable(std::move(tab));
        ps.setPush(pc);
    }
    std::vector<float> tab;
    pushDouble(tab, g.P);
    pushDouble(tab, g.fxo);
    pushDouble(tab, g.fyo);
    tab.push_back(0.f);
    tab.push_back(0.f);
    int cov[256];
    newsprintCoverTable(cov);
    for (int k = 0; k < 256; ++k) tab.push_back((float)cov[k]);
    P::NewsDots0 pc = {p.w, p.h, (uint32_t)g.sw, (uint32_t)g.sh, g.nbx, g.nby, g.par, (int)g.maxX, (int)g.maxY, g.fadeT};
    const char* names[3] = {"news_dots0", "news_dots1", "news_dots2"};
    Pass& ps = p.add(names[g.mode], 0, 1, p.w, p.h);
    ps.in1 = smallBuf;
    ps.table = p.addTable(std::move(tab));
    ps.setPush(pc);
    return true;
}

// 8-bit Min and Max / Median: VEGAS's weighted histogram rank (rankFilter8() in builtins_more_blur.cpp)
bool planRank8(Ctx& c, Plan& p, float rank)
{
    std::vector<float> q;
    int nx = 0, ny = 0;
    more::rank8GpuKernel(c, (int)p.w, (int)p.h, q, nx, ny);
    P::Rank8 pc = {p.w, p.h, (uint32_t)nx, (uint32_t)ny, rank, 0u};
    Pass& ps = p.add("rank8", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(q));
    ps.setPush(pc);
    return true;
}

// Median (neighbourhood() in builtins_more_blur.cpp, vegasMedian() in builtins_vegasmedian.cpp)
bool planMedian(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    const double hr = clampd(c.d("HorizontalRange"), 0, 1), vr = clampd(c.d("VerticalRange"), 0, 1);
    const float big = (float)std::max(W, H);
    const double offset = clampd(c.d("Offset"), 0, 1);
    if (c.levels && more::rank8Applies(W, H)) return planRank8(c, p, (float)offset);
    MedianSetup m;
    medianIntSetup(W, H, (double)((float)hr * big * 0.05f), (double)((float)vr * big * 0.05f), c.b("CircularAreaProcessing"), offset, m);
    if (m.whole >= (1ll << 30)) return false;
    const int lv = p.addBuf(p.w, p.h, 4);
    p.add("med_levels", 0, lv, p.w, p.h).setPush(P::MedLevels{p.w, p.h});
    std::vector<float> tab;
    for (int v : m.cells) tab.push_back((float)v);
    for (int v : m.steps) tab.push_back((float)v);
    P::MedInt pc = {p.w, p.h, (uint32_t)(m.cells.size() / 3), (uint32_t)(m.steps.size() / 3), m.r12, (int)m.whole};
    Pass& ps = p.add("med_int", lv, 1, p.h, 1);
    ps.table = p.addTable(std::move(tab));
    ps.setPush(pc);
    return true;
}

}  // namespace

bool gpuPlanMinMax8(Ctx& c, Plan& p)
{
    if (!more::rank8Applies((int)p.w, (int)p.h)) return false;
    return planRank8(c, p, c.i("Operation") == 1 ? 1.f : 0.f);
}

void gpuPushDouble(std::vector<float>& tab, double v) { pushDouble(tab, v); }

// VEGAS's Deriche Gaussian (vegasDericheBlurSigma() in builtins_deriche.cpp): rows into a scratch buffer (in levels), then columns
// into 'to' (back in 0..1, rounded to whole levels for 8-bit pictures).
void gpuAddDeriche(Plan& p, int from, int to, double sigmaX, double sigmaY, bool levels, bool rawOut)
{
    const uint32_t W = p.bufs[(size_t)from].w, H = p.bufs[(size_t)from].h;
    const int mid = p.addBuf(W, H, 4);
    const int state = p.addBuf(W * 2, H, 4);        // the causal half's doubles: 8 words a pixel
    for (int pass = 0; pass < 2; ++pass) {
        const DericheCoefs k = vegasDericheCoefs(pass ? sigmaY : sigmaX);
        std::vector<float> tab;                       // n0..n3, m1..m4, d1..d4, n - bn (5), m - bm (5): the kernel's DER_* order
        for (int i = 0; i < 4; ++i) pushDouble(tab, k.n[i]);
        for (int i = 1; i < 5; ++i) pushDouble(tab, k.m[i]);
        for (int i = 1; i < 5; ++i) pushDouble(tab, k.d[i]);
        for (int i = 0; i < 5; ++i) pushDouble(tab, k.n[i] - k.bn[i]);
        for (int i = 0; i < 5; ++i) pushDouble(tab, k.m[i] - k.bm[i]);
        P::Deriche pc = {W, H, pass ? 1u : 0u, pass ? 0u : (levels ? 2u : 1u), pass ? (rawOut ? 0u : (levels ? 2u : 1u)) : 0u, 0u};
        Pass& ps = p.add("deriche", pass ? mid : from, pass ? to : mid, (pass ? W : H) * 4, 1);     // a thread per line and channel
        ps.in1 = state;
        ps.table = p.addTable(std::move(tab));
        ps.setPush(pc);
        Pass& pk = p.add("deriche_pack", from, pass ? to : mid, W, H);                              // the four channels' words back into the picture
        pk.in1 = state;
        pk.setPush(P::DerichePack{W, H});
    }
}

const std::vector<FilterGpuEntry>& filterGpuEntries()
{
    static const std::vector<FilterGpuEntry> v = {
        {"median", planMedian, 0.0,
         {GpuCheckCase("small", PM{{"HorizontalRange", {0.2}}, {"VerticalRange", {0.2}}}), GpuCheckCase("offset", PM{{"HorizontalRange", {0.3}}, {"VerticalRange", {0.1}}, {"Offset", {0.8}}}),
          GpuCheckCase("round", PM{{"HorizontalRange", {0.4}}, {"VerticalRange", {0.4}}, {"CircularAreaProcessing", {1}}, {"Offset", {0.3}}}), GpuCheckCase("flat", PM{{"HorizontalRange", {0.0}}, {"VerticalRange", {0.5}}}),
          GpuCheckCase("none", PM{{"HorizontalRange", {0.0}}, {"VerticalRange", {0.0}}})}},
        {"newsprint", planNewsprint, 0.0,
         {GpuCheckCase("mono", PM{{"DotSize", {0.5}}, {"Type", {0}}}), GpuCheckCase("colour", PM{{"DotSize", {0.3}}, {"Type", {1}}}), GpuCheckCase("offset", PM{{"DotSize", {0.4}}, {"Type", {2}}}),
          GpuCheckCase("fine", PM{{"DotSize", {0.1}}, {"Type", {2}}}), GpuCheckCase("fade", PM{{"DotSize", {0.05}}, {"Type", {0}}}), GpuCheckCase("fade colour", PM{{"DotSize", {0.12}}, {"Type", {1}}}),
          GpuCheckCase("big", PM{{"DotSize", {1.0}}, {"Type", {1}}})}},
        {"blackbarfill", planBlackBarFill, 0.0,
         {GpuCheckCase("default", PM{}), GpuCheckCase("blur", PM{{"Background Blur", {0.3}}, {"Foreground Scale", {0.6}}}),
          GpuCheckCase("big blur shadow", PM{{"Background Blur", {0.9}}, {"Foreground Shadow Blur", {0.8}}, {"Foreground Scale", {0.5}}, {"Foreground Location", {0.4, 0.6}}}),
          GpuCheckCase("scaled bg", PM{{"Background Scale", {1.7}}, {"Background Location", {0.3, 0.4}}, {"Background Blur", {0.1}}, {"Foreground Shadow Blur", {0.2}}}),
          GpuCheckCase("whole", PM{{"Background Scale", {1.0}}, {"Foreground Scale", {1.0}}, {"Foreground Shadow Blur", {0.5}}}),
          GpuCheckCase("small fg", PM{{"Foreground Scale", {0.3}}, {"Background Blur", {0.5}}, {"Foreground Shadow Blur", {0.4}}})}},
        {"layerdimensionality", planLayerDimensionality, 0.0,
         {GpuCheckCase("defaults", PM{}), GpuCheckCase("inner", PM{{"ShadowType", {2}}, {"ShadowHeight", {0.5}}, {"ShadowBlur", {0.3}}, {"ShadowAmount", {0.8}}}),
          GpuCheckCase("inner light", PM{{"ShadowType", {2}}, {"ShadowHeight", {0.9}}, {"ShadowBlur", {0.1}}, {"ShadowAmount", {1.0}}, {"LightLocation", {-0.6, 0.4, 0.8}}}),
          GpuCheckCase("drop", PM{{"ShadowType", {1}}, {"ShadowHeight", {0.4}}, {"ShadowBlur", {0.5}}, {"ShadowAmount", {0.7}}}),
          GpuCheckCase("drop thin", PM{{"ShadowType", {1}}, {"ShadowHeight", {0.2}}, {"ShadowBlur", {0.05}}, {"ShadowAmount", {0.9}}, {"ForegroundOpacity", {0.5}}}),
          GpuCheckCase("opacity", PM{{"ForegroundOpacity", {0.4}}}),
          GpuCheckCase("glow", PM{{"GlowType", {1}}, {"GlowAmount", {0.5}}, {"GlowColor", {0.9, 0.7, 0.1, 1.0}}}),
          GpuCheckCase("glow outer", PM{{"GlowType", {3}}, {"GlowAmount", {0.8}}}),
          GpuCheckCase("emboss", PM{{"ForegroundAmount", {0.8}}, {"LightIntensity", {0.6}}, {"EmbossRoundness", {0.3}}}),
          GpuCheckCase("emboss light", PM{{"ForegroundAmount", {-0.6}}, {"LightLocation", {0.5, -0.7, 2.0}}, {"EmbossRoundness", {0.0}}}),
          GpuCheckCase("all", PM{{"ShadowType", {2}}, {"ShadowHeight", {0.6}}, {"ShadowBlur", {0.2}}, {"ShadowAmount", {0.6}}, {"GlowType", {2}}, {"GlowAmount", {0.4}}, {"ForegroundAmount", {0.5}}, {"ForegroundOpacity", {0.8}}})}},
        {"radialpixelate", planRadialPixelate, 0.0,
         {GpuCheckCase("radial", PM{{"Strength", {0.4}}, {"Type", {0}}}), GpuCheckCase("circular", PM{{"Strength", {0.5}}, {"Type", {1}}}), GpuCheckCase("strong", PM{{"Strength", {0.9}}, {"Type", {0}}, {"Center", {0.3, 0.6}}}),
          GpuCheckCase("strong circular", PM{{"Strength", {0.95}}, {"Type", {1}}, {"Center", {0.7, 0.2}}}), GpuCheckCase("weak", PM{{"Strength", {0.05}}, {"Type", {0}}}), GpuCheckCase("zero", PM{{"Strength", {0.0}}}),
          GpuCheckCase("negative", PM{{"Strength", {-0.5}}, {"Type", {1}}})}},
        {"autolooks", planAutoLooks, 0.0,
         {GpuCheckCase("look 1", PM{{"autolooksLook", {1}}, {"Gain", {1.0}}}), GpuCheckCase("look 6 half", PM{{"autolooksLook", {6}}, {"Gain", {0.5}}}), GpuCheckCase("look 13", PM{{"autolooksLook", {13}}, {"Gain", {0.83}}}),
          GpuCheckCase("look 21", PM{{"autolooksLook", {21}}, {"Gain", {0.3}}}), GpuCheckCase("zero", PM{{"autolooksLook", {4}}, {"Gain", {0.0}}}), GpuCheckCase("none", PM{{"autolooksLook", {0}}})}},
        {"de.magix:stereographicProjectionOCL", planTinyPlanetMagix, 0.0,
         {GpuCheckCase("default", PM{}), GpuCheckCase("tilted", PM{{"Latitude", {40}}, {"Longitude", {75}}, {"Z", {5}}}), GpuCheckCase("from below", PM{{"Latitude", {150}}, {"Longitude", {-120}}, {"Z", {60}}}),
          GpuCheckCase("far", PM{{"Latitude", {-30}}, {"Z", {1}}})}},
        {"tinyplanet", planTinyPlanet, 0.0,
         {GpuCheckCase("default", PM{}), GpuCheckCase("tilted", PM{{"Latitude", {40}}, {"Longitude", {75}}, {"Z", {1.5}}}), GpuCheckCase("from below", PM{{"Latitude", {150}}, {"Longitude", {-120}}, {"Z", {0.5}}}),
          GpuCheckCase("far", PM{{"Latitude", {90}}, {"Z", {4}}})}},
        {"radialblur", planRadialBlur, 1.2e-38,   /* (a blend that comes out below the smallest normal float is flushed to 0 on the GPU) */
         {GpuCheckCase("proportional", PM{{"Type", {0}}, {"Strength", {0.6}}}), GpuCheckCase("fixed", PM{{"Type", {1}}, {"Strength", {0.5}}}), GpuCheckCase("gaussian", PM{{"Type", {2}}, {"Strength", {0.9}}, {"Center", {0.3, 0.7}}}),
          GpuCheckCase("corner", PM{{"Type", {1}}, {"Strength", {1.0}}, {"Center", {0.0, 0.0}}}), GpuCheckCase("far corner", PM{{"Type", {0}}, {"Strength", {0.3}}, {"Center", {1.0, 1.0}}}),
          GpuCheckCase("edge", PM{{"Type", {1}}, {"Strength", {0.2}}, {"Center", {1.0, 0.4}}}), GpuCheckCase("weak", PM{{"Type", {0}}, {"Strength", {0.02}}}), GpuCheckCase("zero", PM{{"Strength", {0.0}}})}},
        {"histogram", planHistogram, 0.0,
         {GpuCheckCase("luma", PM{{"Type", {0}}}), GpuCheckCase("red", PM{{"Type", {1}}, {"Center", {0.3, 0.7}}, {"Transparency", {0.2}}}), GpuCheckCase("green hdr", PM{{"Type", {2}}, {"HDR", {2.5}}}),
          GpuCheckCase("blue", PM{{"Type", {3}}, {"Center", {0.9, 0.1}}}), GpuCheckCase("alpha", PM{{"Type", {4}}, {"Transparency", {1.0}}}), GpuCheckCase("lab (float on the CPU)", PM{{"Type", {5}}}),
          GpuCheckCase("log (float on the CPU)", PM{{"LogarithmicScale", {1}}})}},
        {"slowmotion", planSlowMotion, 0.0, {GpuCheckCase("defaults", PM{})}},
        {"de.magix:TimeWarp", planSlowMotion, 0.0, {GpuCheckCase("defaults", PM{})}},
        {"bzmasking", planBezierMasking, 0.0,
         {GpuCheckCase("oval", PM{}), GpuCheckCase("rect rotated", PM{{"Type_0", {1}}, {"Angle_0", {25}}, {"Feather_0", {6}}, {"Opacity_0", {0.8}}}),
          GpuCheckCase("diamond inverted", PM{{"Type_0", {2}}, {"Invert", {1}}, {"Blend", {0.3}}, {"Width_0", {0.7}}, {"Location_0", {0.4, 0.6}}}),
          GpuCheckCase("plain rect", PM{{"Type_0", {1}}, {"Width_0", {0.4}}, {"Height_0", {0.6}}}),
          GpuCheckCase("two masks", PM{{"Enable_1", {1}}, {"Type_1", {0}}, {"Location_1", {0.2, 0.3}}, {"Feather_1", {20}}, {"FeatherStyle_1", {0}}, {"Feather_0", {12}}, {"FeatherStyle_0", {1}}}),
          GpuCheckCase("none", PM{{"Enable_0", {0}}}), GpuCheckCase("mask fx", PM{{"MaskFx", {1}}})}},
        {"com.magix.ofx.vr.dual.stich", planDualFishEye, 0.0,
         {GpuCheckCase("default", PM{}), GpuCheckCase("no vignetting", PM{{"mx.ofx.360.stitch.fisheye.falloff.use", {0}}}), GpuCheckCase("strong vignetting", PM{{"mx.ofx.360.stitch.fisheye.falloff.factor", {80}}, {"mx.ofx.360.stitch.fisheye.falloff.range", {60}}}),
          GpuCheckCase("wide fov", PM{{"mx.ofx.360.stitch.fisheye.fov", {215}}}), GpuCheckCase("narrow fov", PM{{"mx.ofx.360.stitch.fisheye.fov", {185}}}), GpuCheckCase("turned lenses", PM{{"mx.ofx.360.stitch.fisheye.orient.left", {1}}, {"mx.ofx.360.stitch.fisheye.orient.right", {3}}}),
          GpuCheckCase("rotated 180", PM{{"mx.ofx.360.stitch.fisheye.orient.left", {2}}, {"mx.ofx.360.stitch.fisheye.rotation", {40}}}), GpuCheckCase("centre and radius", PM{{"mx.ofx.360.stitch.fisheye.center", {6}}, {"mx.ofx.360.stitch.fisheye.narrowing", {85}}})}},
        {"Filters.Stereoscopic3DAdjust", planStereo3d, 0.0,
         {GpuCheckCase("default", PM{}), GpuCheckCase("offset", PM{{"Horizontal", {0.05}}, {"Vertical", {-0.03}}}), GpuCheckCase("zoom", PM{{"Zoom", {0.4}}}), GpuCheckCase("zoom out manual crop", PM{{"Zoom", {-0.3}}, {"AutoCrop", {0}}, {"Crop", {0.1}}}),
          GpuCheckCase("flip h", PM{{"FlipHorizontal", {1}}, {"Horizontal", {0.02}}}), GpuCheckCase("flip both", PM{{"FlipHorizontal", {3}}, {"FlipVertical", {3}}, {"Zoom", {0.2}}}), GpuCheckCase("flip v", PM{{"FlipVertical", {1}}}),
          GpuCheckCase("keystone", PM{{"Keystone", {0.3}}}), GpuCheckCase("rotation", PM{{"Rotation", {8}}}), GpuCheckCase("keystone rotation flip", PM{{"Keystone", {-0.2}}, {"Rotation", {-12}}, {"FlipHorizontal", {1}}, {"Horizontal", {0.04}}}),
          GpuCheckCase("floating window", PM{{"FloatingWindowLeft", {0.13}}}), GpuCheckCase("floating window rotated", PM{{"FloatingWindowLeft", {0.2}}, {"Rotation", {3}}, {"Zoom", {0.1}}}),
          GpuCheckCase("manual crop rotated", PM{{"AutoCrop", {0}}, {"Crop", {0.3}}, {"Rotation", {5}}}), GpuCheckCase("big", PM{{"Zoom", {3}}, {"Horizontal", {0.9}}}), GpuCheckCase("tiny", PM{{"Zoom", {-0.9}}}),
          GpuCheckCase("flip v keystone", PM{{"FlipVertical", {2}}, {"Keystone", {0.5}}, {"Vertical", {0.1}}})}},
        {"de.magix:LensCorrection", planLensCorrection, 0.0,
         {GpuCheckCase("barrel", PM{{"RadialDistParam1", {0.1}}, {"RadialDistParam2", {-0.05}}, {"RadialDistParam3", {0.02}}}), GpuCheckCase("pincushion scale", PM{{"RadialDistParam1", {-0.2}}, {"Scale", {1.3}}, {"DistCenter", {0.4, 0.6}}}),
          GpuCheckCase("strong", PM{{"RadialDistParam1", {0.5}}, {"RadialDistParam2", {0.3}}, {"RadialDistParam3", {-0.4}}, {"Scale", {0.7}}}), GpuCheckCase("scale only", PM{{"Scale", {2.0}}}),
          GpuCheckCase("plain", PM{}), GpuCheckCase("mesh", PM{{"ShowMesh", {1}}, {"RadialDistParam1", {0.2}}})}},
        {"com.vegascreativesoftware:filllightvelvetmatter", planFillLight, 0.0,
         {GpuCheckCase("default", PM{}), GpuCheckCase("strong", PM{{"Strength", {100}}, {"Softness", {20}}, {"Bias", {40}}}), GpuCheckCase("tinted", PM{{"Hue", {200}}, {"Tint", {70}}, {"Softness", {0.5}}}),
          GpuCheckCase("no blur", PM{{"Softness", {0}}, {"Strength", {80}}}), GpuCheckCase("weak", PM{{"Strength", {0.5}}}), GpuCheckCase("off", PM{{"Strength", {0}}})}},
        {"com.vegascreativesoftware:softcontrastvelvetmatter", planSoftContrast, 0.0,
         {GpuCheckCase("default", PM{}), GpuCheckCase("heavy", PM{{"EffectStretchRange", {32.63}}, {"EffectContrast", {100}}, {"EffectDiffusion", {0}}, {"EffectLowTrim", {5.51}}}),
          GpuCheckCase("trims", PM{{"EffectLowTrim", {8}}, {"EffectHighTrim", {6}}, {"EffectDiffusion", {5}}}), GpuCheckCase("tint", PM{{"EffectTint", {40}}, {"EffectCorrectionHue", {120}}}),
          GpuCheckCase("weak", PM{{"EffectContrast", {20}}, {"EffectStretchRange", {0}}}), GpuCheckCase("vig black", PM{{"VignetteExteriorEffect", {2}}, {"VignetteStrength", {80}}}),
          GpuCheckCase("vig white", PM{{"VignetteExteriorEffect", {3}}, {"VignetteShape", {1}}, {"VignetteSoftness", {40}}, {"VignettePosition", {0.4, 0.6}}}),
          GpuCheckCase("vig clear", PM{{"VignetteExteriorEffect", {4}}, {"VignetteWidth", {50}}, {"VignetteHeight", {60}}, {"VignetteCornerRadius", {20}}}),
          GpuCheckCase("vig blur (float on the CPU)", PM{{"VignetteExteriorEffect", {1}}, {"VignetteHorizontalBlur", {1}}, {"VignetteVerticalBlur", {1}}})}},
        {"com.vegascreativesoftware:glintvelvetmatter", planGlint, 0.0,
         {GpuCheckCase("default", PM{{"EffectThreshold", {30}}}), GpuCheckCase("one streak", PM{{"EffectStreaks", {1}}, {"EffectThreshold", {40}}}), GpuCheckCase("two streaks", PM{{"EffectStreaks", {2}}, {"EffectThreshold", {30}}, {"EffectHorizontalRadius", {30}}}),
          GpuCheckCase("six", PM{{"EffectStreaks", {6}}, {"EffectThreshold", {20}}, {"EffectOrientation", {20}}}), GpuCheckCase("flicker", PM{{"EffectReduceFlicker", {1}}, {"EffectThreshold", {30}}}),
          GpuCheckCase("effect only", PM{{"EffectEffectOnly", {1}}, {"EffectThreshold", {30}}, {"EffectBoost", {100}}}), GpuCheckCase("negative boost", PM{{"EffectBoost", {-30}}, {"EffectThreshold", {25}}}),
          GpuCheckCase("five odd", PM{{"EffectStreaks", {5}}, {"EffectThreshold", {25}}, {"EffectOrientation", {37}}, {"EffectVerticalRadius", {40}}}),
          GpuCheckCase("mask rect", PM{{"MaskMaskMode", {1}}, {"MaskMaskType", {0}}, {"EffectThreshold", {30}}}), GpuCheckCase("mask source", PM{{"MaskMaskMode", {2}}, {"MaskMaskType", {3}}, {"EffectThreshold", {30}}}),
          GpuCheckCase("mask wipe", PM{{"MaskMaskMode", {1}}, {"MaskMaskType", {2}}, {"MaskSoftness", {0.1}}}), GpuCheckCase("mask only (float on the CPU)", PM{{"MaskMaskMode", {3}}})}},
        {"com.vegascreativesoftware:raysvelvetmatter", planRays, 0.0,
         {GpuCheckCase("default", PM{}), GpuCheckCase("low threshold", PM{{"EffectThreshold", {20}}, {"EffectAmount", {60}}}), GpuCheckCase("centre off", PM{{"EffectThreshold", {25}}, {"EffectPosition", {0.2, 0.7}}, {"EffectAspect", {1.5}}}),
          GpuCheckCase("noise", PM{{"EffectThreshold", {25}}, {"EffectNoise", {30}}}), GpuCheckCase("alpha", PM{{"EffectKeyAlphaChannel", {1}}, {"EffectThreshold", {10}}}), GpuCheckCase("flicker", PM{{"EffectReduceFlicker", {1}}, {"EffectThreshold", {25}}}),
          GpuCheckCase("effect only", PM{{"EffectEffectOnly", {1}}, {"EffectThreshold", {25}}, {"EffectQuality", {1}}}), GpuCheckCase("no amount", PM{{"EffectAmount", {0}}, {"EffectThreshold", {25}}}), GpuCheckCase("big", PM{{"EffectAmount", {76}}, {"EffectThreshold", {10}}, {"EffectQuality", {0}}}),
          GpuCheckCase("mask effect", PM{{"MaskMaskMode", {1}}, {"MaskMaskType", {1}}, {"EffectThreshold", {25}}}), GpuCheckCase("mask source", PM{{"MaskMaskMode", {2}}, {"MaskMaskType", {3}}, {"EffectThreshold", {25}}}),
          GpuCheckCase("mask wipe", PM{{"MaskMaskMode", {1}}, {"MaskMaskType", {0}}, {"MaskSoftness", {0.1}}}), GpuCheckCase("mask only (float on the CPU)", PM{{"MaskMaskMode", {3}}})}},
        {"com.magixsoftware:panorotation", planSceneRotation, 0.0,
         {GpuCheckCase("yaw", PM{{"mx.ofx.plugin.rotate.y.angle", {40}}}), GpuCheckCase("all", PM{{"mx.ofx.plugin.rotate.x.angle", {15}}, {"mx.ofx.plugin.rotate.y.angle", {-70}}, {"mx.ofx.plugin.rotate.z.angle", {25}}}),
          GpuCheckCase("pitch", PM{{"mx.ofx.plugin.rotate.z.angle", {60}}}), GpuCheckCase("roll", PM{{"mx.ofx.plugin.rotate.x.angle", {90}}}), GpuCheckCase("none", PM{}), GpuCheckCase("all large", PM{{"mx.ofx.plugin.rotate.x.angle", {-120}}, {"mx.ofx.plugin.rotate.y.angle", {200}}, {"mx.ofx.plugin.rotate.z.angle", {80}}}),
          GpuCheckCase("top bottom (float on the CPU)", PM{{"mx.ofx.plugin.rotate.stereo", {1}}, {"mx.ofx.plugin.rotate.y.angle", {30}}}), GpuCheckCase("side by side (float on the CPU)", PM{{"mx.ofx.plugin.rotate.stereo", {2}}, {"mx.ofx.plugin.rotate.z.angle", {20}}})}},
        {"com.magix.ofx.vr.stabilization", planStabilization360, 0.0, {GpuCheckCase("no motion data", PM{})}},
        {"de.magix:MotionTracker", planMotionTracker, 0.0,
         {GpuCheckCase("plain", PM{}), GpuCheckCase("shown without region", PM{{"ShowMotionVectors", {1}}}),
          GpuCheckCase("region drawn (float on the CPU)", PM{{"ShowMotionVectors", {1}}, {"CornerTL", {0.2, 0.2}}, {"CornerTR", {0.8, 0.2}}, {"CornerBR", {0.8, 0.8}}, {"CornerBL", {0.2, 0.8}}})}},
        {"lightrays", planLightRays, 0.0,
         {GpuCheckCase("default", PM{}), GpuCheckCase("inside", PM{{"LightSource", {0.4, 0.7}}, {"group0Sensitivity", {0.8}}, {"group0Strength", {1.0}}}),
          GpuCheckCase("outside", PM{{"LightSource", {1.4, -0.3}}, {"group0Sensitivity", {0.7}}, {"group0Strength", {0.8}}, {"group2Blend", {0.5}}}),
          GpuCheckCase("corner", PM{{"LightSource", {0.0, 1.0}}, {"group0Sensitivity", {0.6}}, {"group0Strength", {1.5}}}), GpuCheckCase("tinted", PM{{"LightSource", {0.5, 0.5}}, {"TintColor", {1.0, 0.6, 0.2}}, {"group0Sensitivity", {0.5}}})}},
        {"border", planBorder, 0.0,
         {GpuCheckCase("blurred", PM{{"Type", {0}}, {"Size", {0.2}}}), GpuCheckCase("blurred wide", PM{{"Type", {0}}, {"Size", {0.5}}}), GpuCheckCase("blurred thin", PM{{"Type", {0}}, {"Size", {0.06}}}),
          GpuCheckCase("bevel", PM{{"Type", {1}}, {"Size", {0.15}}, {"Angle", {40}}, {"color", {0.9, 0.5, 0.2, 1.0}}}), GpuCheckCase("bevel thin", PM{{"Type", {1}}, {"Size", {0.04}}, {"Angle", {200}}}),
          GpuCheckCase("bevel big", PM{{"Type", {1}}, {"Size", {0.7}}, {"Angle", {315}}, {"color", {0.3, 0.8, 0.9, 1.0}}}),
          GpuCheckCase("solid", PM{{"Type", {2}}, {"Size", {0.15}}, {"color", {0.8, 0.2, 0.3, 0.6}}}), GpuCheckCase("solid full", PM{{"Type", {2}}, {"Size", {0.5}}, {"color", {0.1, 0.9, 0.4, 1.0}}})}},
        {"pictureinpicture", planPictureInPicture, 0.0,
         {GpuCheckCase("default", PM{{"Scale", {0.5}}}), GpuCheckCase("shifted", PM{{"Scale", {0.4}}, {"Location", {0.3, 0.6}}, {"Opacity", {0.7}}}),
          GpuCheckCase("rotated", PM{{"Scale", {0.6}}, {"Angle", {23}}, {"Anchor", {4}}, {"Opacity", {0.55}}}),
          GpuCheckCase("anchor", PM{{"Scale", {0.7}}, {"Anchor", {8}}, {"Location", {0.8, 0.2}}, {"Angle", {-40}}}),
          GpuCheckCase("free shear", PM{{"KeepProportions", {1}}, {"Scale", {0.5}}, {"DistortionScaleY", {0.3}}, {"DistortionShear", {0.4}}, {"Angle", {10}}}),
          GpuCheckCase("free form", PM{{"KeepProportions", {2}}, {"CornerBL", {0.1, 0.1}}, {"CornerBR", {0.9, 0.2}}, {"CornerTR", {0.8, 0.9}}, {"CornerTL", {0.2, 0.7}}}),
          GpuCheckCase("free form 2", PM{{"KeepProportions", {2}}, {"CornerBL", {0.0, 0.0}}, {"CornerBR", {1.0, 0.1}}, {"CornerTR", {0.7, 0.6}}, {"CornerTL", {0.1, 1.0}}, {"Opacity", {0.8}}})}},
        {"vignette", planVignette, 5e-5, {}},          // (powf: the library's last bit)
        {"glow", planGlow, 0.0, {}},
        {"chromakeyer", planChromaKeyer, 0.0, {}},
        {"cookiecutter", planCookieCutter, 0.0,
         {GpuCheckCase("circle", PM{{"Shape", {0}}, {"Size", {0.3}}}), GpuCheckCase("circle border", PM{{"Shape", {0}}, {"Size", {0.3}}, {"Border", {0.5}}, {"Color", {0.9, 0.2, 0.1}}}),
          GpuCheckCase("repeat", PM{{"Shape", {0}}, {"Size", {0.4}}, {"RepeatX", {3}}, {"RepeatY", {2}}, {"Feather", {0.3}}}), GpuCheckCase("oval", PM{{"Shape", {1}}, {"Size", {0.3}}, {"Border", {0.2}}}),
          GpuCheckCase("oval side", PM{{"Shape", {2}}, {"Size", {0.3}}, {"RepeatX", {2}}, {"Method", {1}}}), GpuCheckCase("rect", PM{{"Shape", {3}}, {"Size", {0.3}}, {"Border", {0.3}}, {"Feather", {0.5}}}),
          GpuCheckCase("diamond", PM{{"Shape", {4}}, {"Size", {0.3}}, {"RepeatX", {2}}, {"RepeatY", {2}}}), GpuCheckCase("square", PM{{"Shape", {5}}, {"Size", {0.3}}, {"Center", {0.3, 0.6}}}),
          GpuCheckCase("tri up", PM{{"Shape", {6}}, {"Size", {0.5}}, {"RepeatX", {2}}}), GpuCheckCase("tri right", PM{{"Shape", {7}}, {"Size", {0.5}}, {"Border", {0.2}}}),
          GpuCheckCase("tri down", PM{{"Shape", {8}}, {"Size", {0.5}}, {"RepeatY", {3}}}), GpuCheckCase("tri left", PM{{"Shape", {9}}, {"Size", {0.5}}, {"RepeatX", {3}}, {"RepeatY", {2}}}),
          GpuCheckCase("arrow up", PM{{"Shape", {10}}, {"Size", {0.5}}, {"RepeatX", {3}}, {"RepeatY", {3}}, {"Center", {0.2, 0.3}}}), GpuCheckCase("arrow right", PM{{"Shape", {11}}, {"Size", {0.5}}, {"RepeatX", {3}}, {"RepeatY", {3}}, {"Border", {0.2}}}),
          GpuCheckCase("arrow down", PM{{"Shape", {12}}, {"Size", {0.5}}, {"RepeatX", {3}}, {"RepeatY", {3}}, {"Center", {0.7, 0.8}}}), GpuCheckCase("arrow left", PM{{"Shape", {13}}, {"Size", {0.5}}, {"RepeatX", {3}}, {"RepeatY", {3}}, {"Feather", {0.2}}})}},
        {"linearblur", planLinearBlur, 0.0,
         {GpuCheckCase("a0", PM{{"Angle", {0}}, {"Amount", {0.05}}}), GpuCheckCase("a30", PM{{"Angle", {30}}, {"Amount", {0.1}}}), GpuCheckCase("a60", PM{{"Angle", {60}}, {"Amount", {0.1}}}),
          GpuCheckCase("a90", PM{{"Angle", {90}}, {"Amount", {0.08}}}), GpuCheckCase("a120", PM{{"Angle", {120}}, {"Amount", {0.12}}}), GpuCheckCase("a150", PM{{"Angle", {150}}, {"Amount", {0.07}}}),
          GpuCheckCase("a200", PM{{"Angle", {200}}, {"Amount", {0.2}}}), GpuCheckCase("a315", PM{{"Angle", {315}}, {"Amount", {0.3}}}), GpuCheckCase("a45", PM{{"Angle", {45}}, {"Amount", {0.02}}}),
          GpuCheckCase("a135", PM{{"Angle", {135}}, {"Amount", {0.4}}}), GpuCheckCase("tiny", PM{{"Angle", {10}}, {"Amount", {0.0}}})}},
        {"sepia", planSepia, 0.0,
         {GpuCheckCase("weak", PM{{"BlendingStrength", {0.2}}}), GpuCheckCase("mid", PM{{"BlendingStrength", {0.5}}, {"Color", {0.8, 0.45, 0.1}}}),
          GpuCheckCase("strong", PM{{"BlendingStrength", {0.9}}, {"BlendingFalloff", {0.4}}}), GpuCheckCase("full", PM{{"BlendingStrength", {1.0}}, {"BlendingFalloff", {1.0}}})}},
        {"pixelate", nullptr, 0.0,
         {GpuCheckCase("one cell", PM{{"HorizontalPixelization", {1.0}}, {"VerticalPixelization", {1.0}}}), GpuCheckCase("none", PM{{"HorizontalPixelization", {0.0}}, {"VerticalPixelization", {0.0}}}),
          GpuCheckCase("tall", PM{{"HorizontalPixelization", {0.0}}, {"VerticalPixelization", {0.7}}})}},
        {"deform", planDeform, 0.0,
         {GpuCheckCase("big", PM{{"Amount", {1.0}}, {"SqueezeTop", {0.9}}, {"ShearHorizontal", {-1.5}}, {"CurveBottom", {-0.8}}})}},
        {"spherize", planSpherize, 5e-5,        // (cos in doubles: the cell a position lands in is decided by a library cos)
         {GpuCheckCase("default", PM{{"Amount", {1.0}}})}},
        {"convolutionkernel", planConvolutionKernel, 0.0,
         {GpuCheckCase("scaled", PM{{"Row1Column2", {-1}}, {"Row2Column1", {-1}}, {"Row2Column2", {5}}, {"Row2Column3", {-1}}, {"Row3Column2", {-1}}, {"Rotation", {20}}}),
          GpuCheckCase("preview", PM{{"Row1Column1", {1}}, {"Row2Column2", {2}}, {"Row3Column3", {1}}, {"Rotation", {90}}, {"Scale", {1.5}}})}},
        {"labadjust", planLabAdjust, 1.0 / 255.0 + 1e-4,        // (libm pow / cbrt in doubles: a value a hair either side of a level is a whole level apart in float)
         {GpuCheckCase("grey", PM{{"Desaturate", {1.0}}}), GpuCheckCase("shift", PM{{"Lightness", {0.7}}, {"Channela", {-0.5}}, {"Channelb", {0.4}}})}},
        {"whitebalance", planWhiteBalance, 0.0,
         {GpuCheckCase("warm", PM{{"AmountOfCorrection", {0.6}}, {"ColorTemperature", {4000}}}), GpuCheckCase("tint", PM{{"AmountOfCorrection", {1.0}}, {"ColorTint", {50}}, {"Brightness", {1.2}}}),
          GpuCheckCase("equalize", PM{{"HasData", {1}}}), GpuCheckCase("both", PM{{"AmountOfCorrection", {0.4}}, {"ColorTemperature", {9000}}, {"HasData", {1}}}),
          GpuCheckCase("dark", PM{{"AmountOfCorrection", {0.0}}, {"Brightness", {0.5}}})}},
    };
    return v;
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
