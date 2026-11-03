// Vulkan plans of the text effects (see vktext.h). What stays on the CPU is the glyph rasterisation and everything that decides WHERE and WHAT is drawn (the layout, the animation
// engine, GDI/GDI+ on Windows, Reno's own scan converter and the application's fonts elsewhere); the card gets the rasterised layer as a table and does the per-pixel passes after it.
// The kernels are in tools/gpukernels_text.py.
#include "vktext.h"
#include "vfxtitlerich.h"
#include "vfxcredit.h"
#include "builtins_cgblur.h"
#include "vkshaders/push_layouts.h"
#include <algorithm>
#include <cmath>
#include <cstring>

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

// a double as the two words of a push block (high half first)
void halves(double d, uint32_t& h, uint32_t& l)
{
    uint64_t b;
    std::memcpy(&b, &d, 8);
    h = (uint32_t)(b >> 32);
    l = (uint32_t)b;
}

double clamp01d(double v) { return std::max(0.0, std::min(1.0, v)); }

// cgGaussianBlurPlane on the buffer's four lanes (the same recursion per lane): the rows with sigmaX, then the columns with sigmaY, in place
void addCgBlur(Plan& p, int buf, double sx, double sy)
{
    const CgBlurCoefs cx = cgBlurCoefs(sx), cy = cgBlurCoefs(sy);
    p.add("cg_blur", buf, buf, p.h, 1).setPush(P::CgBlur{p.w, p.h, 0u, cx.gain, cx.c1, cx.c2, cx.c3});
    p.add("cg_blur", buf, buf, p.w, 1).setPush(P::CgBlur{p.w, p.h, 1u, cy.gain, cy.c1, cy.c2, cy.c3});
}

// the layer's alpha in all lanes of a new buffer
int alphaPlane(Plan& p, int layer)
{
    const int a = p.addBuf(p.w, p.h, 4);
    p.add("ldo_alpha", layer, a, p.w, p.h).setPush(P::LdoAlpha{p.w, p.h});
    return a;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------- Titles & Text (titlesAndText in builtins_gen.cpp)
// On the CPU: the glyphs (the layer of bytes, with the per-letter blur of the animations drawn into it). On the card: the node blur (blurLayer), the glow (glowLayer), the drop shadow
// (dropShadow) and the composite over the background, each the CPU function's arithmetic (floats, and the doubles in tools/sf64.py where the CPU uses doubles).
// Reno's older moving text (titlesAndTextMoving: a bitmap mask moved, outlined with a max filter and blurred by Reno's own kernels) stays entirely on the CPU: it is not VEGAS's code.
bool planTitlesAndText(Ctx& c, Plan& p)
{
    TitleGpu g;
    if (!titlesAndTextGpu(c, g)) return false;
    if (!g.layer) {                                             // (no text: the background alone)
        p.add("fill_px", 0, 1, p.w, p.h).setPush(P::FillPx{p.w, p.h, g.bg[0], g.bg[1], g.bg[2], g.bg[3]});
        return true;
    }
    if (g.w != (int)p.w || g.h != (int)p.h || g.bytes.size() != (size_t)p.w * p.h * 4) return false;
    const double W = (double)p.w;
    if (g.shadow && (std::fabs(g.shadowDx * W) > 1.0e6 || std::fabs(g.shadowDy * W) > 1.0e6)) return false;      // (nonsense: the CPU's int cast of it is not defined)
    std::vector<float> tab(g.bytes.size());
    for (size_t i = 0; i < tab.size(); ++i) tab[i] = (float)g.bytes[i];
    int cur = p.addBuf(p.w, p.h, 4);
    {
        Pass& ps = p.add("tt_load", 0, cur, p.w, p.h);
        ps.table = p.addTable(std::move(tab));
        ps.setPush(P::TtLoad{p.w, p.h});
    }
    if (g.blur) {                                               // blurLayer: each of the four planes through the recursion, back to bytes
        addCgBlur(p, cur, g.blurSx, g.blurSy);
        const int r = p.addBuf(p.w, p.h, 4);
        p.add("tt_round", cur, r, p.w, p.h).setPush(P::TtRound{p.w, p.h});
        cur = r;
    }
    if (g.glow) {                                               // glowLayer
        const double amount = std::min(g.glowAmount, 1.0);
        const int a = alphaPlane(p, cur);
        addCgBlur(p, a, builtin::more::cgSigmaForDiameter(amount * vfxtt::kBlurRef * p.w), builtin::more::cgSigmaForDiameter(amount * vfxtt::kBlurRef * p.h));
        P::TtGlow pc = {p.w, p.h, g.glowInner ? 1u : 0u, 0u, 0, 0, 0, 0, 0, 0, 0, 0};
        halves(clamp01d(g.glowColor[0]), pc.ca_h, pc.ca_l);
        halves(clamp01d(g.glowColor[3]), pc.c0_h, pc.c0_l);     // (bytes are b g r)
        halves(clamp01d(g.glowColor[2]), pc.c1_h, pc.c1_l);
        halves(clamp01d(g.glowColor[1]), pc.c2_h, pc.c2_l);
        const int o = p.addBuf(p.w, p.h, 4);
        Pass& ps = p.add("tt_glow", cur, o, p.w, p.h);
        ps.in1 = a;
        ps.setPush(pc);
        cur = o;
    }
    if (g.shadow) {                                             // dropShadow
        const double diameter = g.shadowBlur * W;
        const double sigma = diameter > 0 ? builtin::more::cgSigmaForDiameter(diameter) : 0.0;
        const int a = alphaPlane(p, cur);
        addCgBlur(p, a, sigma, sigma);
        const int ab = p.addBuf(p.w, p.h, 4);
        p.add("tt_sbyte", a, ab, p.w, p.h).setPush(P::TtSbyte{p.w, p.h});
        const double sa = clamp01d(g.shadowColor[3]);
        float col[4];
        for (int k = 0; k < 3; ++k) col[k] = (float)(clamp01d(g.shadowColor[k]) * sa);
        col[3] = (float)sa;
        P::TtShadow pc = {p.w, p.h, 0u, 0u, col[2], col[1], col[0], col[3], 0, 0, 0, 0};
        halves(g.shadowDx * W, pc.ox_h, pc.ox_l);
        halves(g.shadowDy * W, pc.oy_h, pc.oy_l);
        const int o = p.addBuf(p.w, p.h, 4);
        Pass& ps = p.add("tt_shadow", cur, o, p.w, p.h);
        ps.in1 = ab;
        ps.setPush(pc);
        cur = o;
    }
    Pass& ps = p.add("tt_comp", cur, 1, p.w, p.h);
    ps.setPush(P::TtComp{p.w, p.h, (float)g.opacity, g.bg[0], g.bg[1], g.bg[2], g.bg[3], 0u});
    return true;
}

// ---------------------------------------------------------------------------------------------------- Timecode (timecodeFx in builtins_more_creative.cpp)
// On the CPU: the time string, the glyph bitmaps (GDI's GetGlyphOutline on Windows) and the box's layout. On the card: the picture, with the grey box where the bitmap is.
bool planTimecode(Ctx& c, Plan& p)
{
    vfxtc::Layout L;
    if (!more::timecodeGpuLayout(c, L)) {                             // (nothing to draw: the picture goes through)
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    std::vector<float> tab(L.bits.size());
    for (size_t i = 0; i < tab.size(); ++i) tab[i] = (float)L.bits[i];
    Pass& ps = p.add("tc_box", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(tab));
    ps.setPush(P::TcBox{p.w, p.h, L.ox, L.oy, (uint32_t)L.wb, (uint32_t)L.hb});
    return true;
}

// ---------------------------------------------------------------------------------------------------- Credit Roll (creditPlay in builtins_gen.cpp)
// On the CPU: the lines' layout, the pages and effects' timing, the glyph masks (GDI on Windows) and where each goes (creditLine). On the card: the blits in order over the background,
// then the wipe of the box, or the zoom of the box (VEGAS's 8-bit cubic resampler and smoothing: integer arithmetic, the same on the card). Only the glyph masks and their places are the CPU's.
bool planCreditRoll(Ctx& c, Plan& p)
{
    CreditGpu g;
    if (!creditRollGpu(c, g)) return false;
    size_t total = g.blits.size() * 16;
    for (const CreditGpu::Blit& b : g.blits) total += b.a.size();
    std::vector<float> tab(std::max<size_t>(total, 1), 0.f);
    auto putI = [&](size_t at, int v) { std::memcpy(&tab[at], &v, 4); };
    size_t mask = g.blits.size() * 16;
    for (size_t i = 0; i < g.blits.size(); ++i) {
        const CreditGpu::Blit& b = g.blits[i];
        const size_t r = i * 16;
        putI(r + 0, b.x0); putI(r + 1, b.y0); putI(r + 2, b.w); putI(r + 3, b.h);
        putI(r + 4, b.l); putI(r + 5, b.t); putI(r + 6, b.r); putI(r + 7, b.b);
        for (int k = 0; k < 4; ++k) tab[r + 8 + (size_t)k] = b.col[k];
        putI(r + 12, (int)mask);
        for (size_t j = 0; j < b.a.size(); ++j) tab[mask + j] = (float)b.a[j];
        mask += b.a.size();
    }
    const int to = g.postKind != 0 ? p.addBuf(p.w, p.h, 4) : 1;
    {
        Pass& ps = p.add("cr_blit", 0, to, p.w, p.h);
        ps.table = p.addTable(std::move(tab));
        ps.setPush(P::CrBlit{p.w, p.h, (uint32_t)g.blits.size(), 0u, g.bg[0], g.bg[1], g.bg[2], g.bg[3]});
    }
    if (g.postKind == 2) {                                      // vfxcredit::wipe
        const int lo = g.vertical ? g.by0 : g.bx0, hi = g.vertical ? g.by1 : g.bx1;
        const int len = hi - lo;
        const int hw = (int)((double)len * 0.05 * 0.5);
        const int cx = (int)((double)(len + 2 * hw) * g.t) + lo - hw;
        const int a0 = std::max(lo, cx - hw), a1 = std::min(hi, cx + hw);
        Pass& ps = p.add("cr_wipe", to, 1, p.w, p.h);
        ps.setPush(P::CrWipe{p.w, p.h, g.bx0, g.by0, g.bx1, g.by1, g.vertical ? 1u : 0u, g.clearFirst ? 1u : 0u, a0, a1, hw, cx, g.bg[0], g.bg[1], g.bg[2], g.bg[3]});
    }
    if (g.postKind == 1) {                                      // vfxcredit::zoomBox
        const int bw = g.bx1 - g.bx0, bh = g.by1 - g.by0;
        const double z = g.t;
        const int hx = (int)((double)bw * z) / 2, hy = (int)((double)bh * z) / 2;
        const int ix0 = g.bx0 + hx, ix1 = g.bx1 - hx, iy0 = g.by0 + hy, iy1 = g.by1 - hy;
        const bool inner = ix1 > ix0 && iy1 > iy0;
        int sm = to;
        if (inner) {
            const int iw = ix1 - ix0, ih = iy1 - iy0;
            const int stepX = (int)(((int64_t)bw << 14) / iw), stepY = (int)(((int64_t)bh << 14) / ih);
            const int A = (int)(z + 0.5), B = (int)((z + z) + 0.5), C = (int)((1.0 - z) * 20.0 + 0.5) + 4;
            const int q = p.addBuf(p.w, p.h, 4), tm = p.addBuf(p.w, p.h, 4);
            sm = p.addBuf(p.w, p.h, 4);
            p.add("cr_zq", to, q, p.w, p.h).setPush(P::CrZq{p.w, p.h});
            {
                const vfxcredit::CubicTable& T = vfxcredit::cubicTable();
                std::vector<float> w(16384 * 4);
                for (int i = 0; i < 16384; ++i)
                    for (int k = 0; k < 4; ++k) w[(size_t)i * 4 + (size_t)k] = (float)T.w[i][k];
                Pass& ps = p.add("cr_zshrink", q, tm, p.w, p.h);
                ps.table = p.addTable(std::move(w));
                ps.setPush(P::CrZshrink{p.w, p.h, g.bx0, g.by0, bw, bh, ix0, iy0, iw, ih, stepX, stepY});
            }
            p.add("cr_zsmooth", tm, sm, p.w, p.h).setPush(P::CrZsmooth{p.w, p.h, ix0, iy0, iw, ih, A, B, C, C + 4 * (A + B)});
        }
        Pass& ps = p.add("cr_zfinal", to, 1, p.w, p.h);
        ps.in1 = sm;
        ps.setPush(P::CrZfinal{p.w, p.h, g.bx0, g.by0, g.bx1, g.by1, ix0, iy0, ix1, iy1, inner ? 1u : 0u, g.bg[0], g.bg[1], g.bg[2], g.bg[3]});
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------- test cases
void textGpuCases(const std::string& id, std::vector<GpuCheckCase>& out)
{
    typedef std::map<std::string, std::string> SM;
    if (id == fullId("timecode")) {
        for (int f = 0; f < 12; ++f) out.push_back(GpuCheckCase("format " + std::to_string(f), PM{{"TimecodeFormat", {(double)f}}}));
        for (int h = 0; h < 3; ++h)
            for (int v = 0; v < 3; ++v) out.push_back(GpuCheckCase("align " + std::to_string(h) + std::to_string(v), PM{{"HorizontalAlignment", {(double)h}}, {"VerticalAlignment", {(double)v}}}));
        out.push_back(GpuCheckCase("big", PM{{"Width", {2.0}}, {"Height", {1.7}}, {"HorizontalAlignment", {1}}, {"VerticalAlignment", {1}}}));
        out.push_back(GpuCheckCase("small", PM{{"Width", {0.5}}, {"Height", {0.5}}}));
    }
    if (id == fullId("creditroll")) {
        auto at = [](GpuCheckCase cs, double frac) { cs.time = frac * 10.0 * 25.0; return cs; };          // (the roll lasts 10 s: Duration; 25 frames a second)
        for (double fr : {0.0, 0.2, 0.5, 0.8}) out.push_back(at(GpuCheckCase("scroll " + std::to_string(fr), PM{}), fr));
        out.push_back(at(GpuCheckCase("scroll back", PM{{"Backward", {1}}}), 0.35));
        out.push_back(at(GpuCheckCase("leader", PM{{"Left", {0.05}}, {"Right", {0.95}}}, std::map<std::string, std::string>{{"Leader", ". "}}), 0.45));
        out.push_back(at(GpuCheckCase("leader wide gap", PM{{"ColumnGap", {0.2}}, {"S3Align", {2}}}, std::map<std::string, std::string>{{"Leader", "-"}}), 0.55));
        out.push_back(at(GpuCheckCase("styles", PM{{"S0Size", {30}}, {"S0Bold", {1}}, {"S0Color", {1.0, 0.5, 0.2, 0.8}}, {"S1Italic", {1}}, {"S1Align", {0}}, {"S2Align", {2}}, {"S4Align", {1}}, {"Background", {0.0, 0.1, 0.3, 0.7}}}), 0.4));
        for (int id = 1; id <= 15; ++id)
            for (double fr : {0.02, 0.13, 0.4}) {
                GpuCheckCase cs("timed fx" + std::to_string(id) + " " + std::to_string(fr), PM{{"Mode", {1}}, {"InEffect", {(double)id}}, {"OutEffect", {(double)id}}, {"PageMode", {1}}, {"Background", {0.1, 0.1, 0.2, 1.0}}});
                out.push_back(at(cs, fr));
            }
        out.push_back(at(GpuCheckCase("timed pages", PM{{"Mode", {1}}, {"PageMode", {2}}, {"TimedY", {0.5}}, {"InEffect", {12}}, {"OutEffect", {13}}}), 0.3));
    }
    if (id == fullId("titlesandtext")) {
        const std::vector<double> red = {0.9, 0.1, 0.1, 1.0};
        out.push_back(GpuCheckCase("plain", PM{{"FontSize", {60}}}, SM{{"Text", "Hello"}}));
        out.push_back(GpuCheckCase("lines and background", PM{{"FontSize", {40}}, {"Background", {0.1, 0.3, 0.6, 0.5}}, {"TextColor", {1.0, 0.8, 0.2, 0.7}}, {"Alignment", {1}}}, SM{{"Text", "Line one\nLine two"}}));
        out.push_back(GpuCheckCase("opaque background", PM{{"FontSize", {50}}, {"Background", {0.2, 0.2, 0.3, 1.0}}}, SM{{"Text", "Solid"}}));
        out.push_back(GpuCheckCase("outline", PM{{"FontSize", {60}}, {"OutlineWidth", {6}}, {"OutlineColor", red}}, SM{{"Text", "Edge"}}));
        out.push_back(GpuCheckCase("shadow", PM{{"FontSize", {60}}, {"ShadowEnable", {1}}}, SM{{"Text", "Shade"}}));
        out.push_back(GpuCheckCase("shadow sharp", PM{{"FontSize", {60}}, {"ShadowEnable", {1}}, {"ShadowBlur", {0.0}}, {"ShadowOffsetX", {-0.05}}, {"ShadowOffsetY", {0.0731}}, {"ShadowColor", {0.0, 0.0, 0.5, 0.6}}}, SM{{"Text", "Sharp"}}));
        out.push_back(GpuCheckCase("shadow soft", PM{{"FontSize", {70}}, {"ShadowEnable", {1}}, {"ShadowBlur", {1.5}}, {"ShadowOffsetX", {0.2}}, {"ShadowOffsetY", {-0.2}}, {"ShadowColor", {0.2, 0.0, 0.0, 1.0}}, {"Background", {0.9, 0.9, 0.5, 0.8}}}, SM{{"Text", "Soft"}}));
        out.push_back(GpuCheckCase("shadow outline", PM{{"FontSize", {55}}, {"ShadowEnable", {1}}, {"ShadowBlur", {0.2}}, {"OutlineWidth", {4}}, {"OutlineColor", red}, {"Location", {0.3, 0.6}}}, SM{{"Text", "Both"}}));
        // VEGAS's rich text with two sizes and a bold word (the rich path: letters placed one by one)
        const std::string rtf = "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fswiss Arial;}}\\pard\\qc\\fs40 small \\b\\fs80 BIG\\b0\\fs40  small\\par}";
        out.push_back(GpuCheckCase("rich", PM{}, SM{{"Text", rtf}}));
        out.push_back(GpuCheckCase("rich shadow", PM{{"ShadowEnable", {1}}, {"ShadowBlur", {0.3}}, {"Background", {0.0, 0.2, 0.0, 0.6}}}, SM{{"Text", rtf}}));
        // the animations of VEGAS (its assembly's data): the opacity, the per-letter blur and the node glow come from these
        // (the per-letter opacity, the node blur and the node glow come from these; _Scroll and _Slide play Reno's older moving text, which the CPU draws whole)
        static const char* const names[] = {"_Popup", "_Fly_in", "_Bounce", "_Double_Flash_Glow", "_Rolling_Glow_and_Enlarge", "_Drop_split", "_Twist_In", "_Earthquake", "_Title07", "_Menace",
                                            "_Scroll", "_Slide"};
        for (const char* nm : names)
            for (double t : {0.2, 0.45, 0.65}) {
                const bool cpu = !std::strcmp(nm, "_Scroll") || !std::strcmp(nm, "_Slide");
                GpuCheckCase cs(std::string("anim ") + nm + " " + std::to_string(t) + (cpu ? " (float on the CPU)" : ""), PM{{"FontSize", {70}}, {"ShadowEnable", {1}}, {"ShadowBlur", {0.2}}},
                                SM{{"Text", "Animate"}, {"AnimationName", nm}});
                cs.duration = 100;
                cs.time = 100 * t;
                out.push_back(cs);
            }
    }
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
