// Built-in stand-ins for VEGAS Pro 18's distortion effects (see builtins.h): Swirl, Pinch/Punch and Wave, with VEGAS's identifiers, parameter names and presets (from Vfx1Presets.xml).
// Swirl, Pinch/Punch and Wave are VEGAS's own code (vfxswirl.h, vfxwave.h)
#include "builtins_impl.h"
#include "builtins_mask_internal.h"
#include "vegas8.h"
#include "vfxswirl.h"
#include "vkfilters.h"
#include "vfxwave.h"
#include "../core/base.h"
#include <algorithm>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
namespace {

const char* const kGroup = "Reno Built-in";

// ------------------------------------------------------------------ Swirl and Pinch/Punch
// Both from VEGAS's own code (sftrans1.dll, read and checked against all 32 of its pictures; see vfxswirl.h). They work inside an ellipse round Center (Scaling: its size as a
// fraction of the frame; Proportional: the vertical scaling follows the horizontal one) and copy the rest. VEGAS counts from the pixel's index and truncates the centre to a whole
// pixel. An 8-bit frame goes through the 8-bit mixers (bytes blue, green, red, alpha), a float frame through the float ones.
vfswirl::Area areaOf(Ctx& c, int W, int H)
{
    double cx, cy;
    c.xy("Center", cx, cy);
    const double sh = clampd(c.d("ScalingHorizontal"), 0, 10);
    const double sv = c.b("ScalingProportional") ? sh : clampd(c.d("ScalingVertical"), 0, 10);
    return vfswirl::setup(W, H, clampd(c.d("Amount"), -1, 1), cx, 1 - cy, sh, sv);
}

// mode 0: Swirl, 1: Pinch/Punch
void legacyWarp(Ctx& c, int mode)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    const vfswirl::Area a = areaOf(c, W, H);
    if (!(c.host8 || c.levels)) {
        float* d = reinterpret_cast<float*>(c.out.px.data());
        const float* s = reinterpret_cast<const float*>(c.src.px.data());
        if (mode == 0) vfswirl::swirlF(d, (ptrdiff_t)W * 16, s, (ptrdiff_t)W * 16, a);
        else vfswirl::pinchF(d, (ptrdiff_t)W * 16, s, (ptrdiff_t)W * 16, a);
        return;
    }
    std::vector<uint8_t> in((size_t)W * H * 4), out((size_t)W * H * 4, 0);
    for (int y = 0; y < H; ++y) {
        const float* s = c.src.at(0, y);
        uint8_t* d = &in[(size_t)y * W * 4];
        for (int x = 0; x < W; ++x, s += 4, d += 4) {
            d[0] = (uint8_t)v8::toLevel(s[2]);
            d[1] = (uint8_t)v8::toLevel(s[1]);
            d[2] = (uint8_t)v8::toLevel(s[0]);
            d[3] = (uint8_t)v8::toLevel(s[3]);
        }
    }
    if (mode == 0) vfswirl::swirl8(out.data(), (ptrdiff_t)W * 4, in.data(), (ptrdiff_t)W * 4, a);
    else vfswirl::pinch8(out.data(), (ptrdiff_t)W * 4, in.data(), (ptrdiff_t)W * 4, a);
    for (int y = 0; y < H; ++y) {
        float* o = c.out.at(0, y);
        const uint8_t* s = &out[(size_t)y * W * 4];
        for (int x = 0; x < W; ++x, o += 4, s += 4) {
            o[0] = v8::fromLevel(s[2]);
            o[1] = v8::fromLevel(s[1]);
            o[2] = v8::fromLevel(s[0]);
            o[3] = v8::fromLevel(s[3]);
        }
    }
}
void swirl(Ctx& c) { legacyWarp(c, 0); }
void pinchPunch(Ctx& c) { legacyWarp(c, 1); }

// ------------------------------------------------------------------ Wave
// Wave, from VEGAS's own code (vidpcore.dll, read and checked bit for bit against its render function; see vfxwave.h): the vertical waves shift every row sideways, the
// horizontal waves move every column up and down, with VEGAS's "pi" of 3.141, 14-bit positions and no half-pixel offset. An 8-bit frame goes through its 8-bit mixer
// (blue, green, red, alpha bytes), a float frame through its float one.
void wave(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    if (W < 2 || H < 2) { c.out = c.src; return; }     // (VEGAS's function reads past the picture here)
    double P[6];
    P[0] = clampd(c.d("VerticalWaves"), 0, 1000);
    P[1] = clampd(c.d("HorizontalWaves"), 0, 1000);
    P[2] = clampd(c.d("VerticalAmplitude"), -1, 1);
    P[3] = clampd(c.d("HorizontalAmplitude"), -1, 1);
    P[4] = c.d("VerticalPhase");
    P[5] = c.d("HorizontalPhase");
    if (!(c.host8 || c.levels)) {
        vfwave::render(reinterpret_cast<uint8_t*>(c.out.px.data()), W * 16, W, H, reinterpret_cast<const uint8_t*>(c.src.px.data()), W * 16, P, 16);
        return;
    }
    std::vector<uint8_t> in((size_t)W * H * 4), out((size_t)W * H * 4, 0);
    for (int y = 0; y < H; ++y) {
        const float* s = c.src.at(0, y);
        uint8_t* d = &in[(size_t)y * W * 4];
        for (int x = 0; x < W; ++x, s += 4, d += 4) {
            d[0] = (uint8_t)v8::toLevel(s[2]);
            d[1] = (uint8_t)v8::toLevel(s[1]);
            d[2] = (uint8_t)v8::toLevel(s[0]);
            d[3] = (uint8_t)v8::toLevel(s[3]);
        }
    }
    vfwave::render(out.data(), W * 4, W, H, in.data(), W * 4, P, 4);
    for (int y = 0; y < H; ++y) {
        float* o = c.out.at(0, y);
        const uint8_t* s = &out[(size_t)y * W * 4];
        for (int x = 0; x < W; ++x, o += 4, s += 4) {
            o[0] = v8::fromLevel(s[2]);
            o[1] = v8::fromLevel(s[1]);
            o[2] = v8::fromLevel(s[0]);
            o[3] = v8::fromLevel(s[3]);
        }
    }
}

Def filter(const char* id, const char* label, const char* what, std::vector<PDef> params, void (*fn)(Ctx&))
{
    Def d;
    d.id = std::string("com.vegascreativesoftware:") + id;
    d.label = label;
    d.group = kGroup;
    d.description = std::string(what) + " (Reno's own, standing in for VEGAS " + label + ").";
    d.params = std::move(params);
    d.render = fn;
    return d;
}

std::vector<PDef> areaParams(double amount)
{
    std::vector<PDef> ps = {point("Center", "Center", 0.5, 0.5), dbl("Amount", "Amount", amount, -1, 1), group("Scaling", "Scaling"),
                            dbl("ScalingHorizontal", "Horizontal", 1, 0, 10, "Scaling"), dbl("ScalingVertical", "Vertical", 1, 0, 10, "Scaling"),
                            boolean("ScalingProportional", "Proportional", true, "Scaling")};
    for (auto& p : ps) if (p.name == "ScalingHorizontal" || p.name == "ScalingVertical") p.dmax = 2;
    return ps;
}

}  // namespace

void addFiltersDistort(std::vector<Def>& out)
{
    out.push_back(filter("swirl", "Swirl", "The picture twisted round a centre", areaParams(1.0), swirl));
    out.push_back(filter("pinchpunch", "Pinch/Punch", "The middle of the picture punched out or pinched in", areaParams(1.0), pinchPunch));
    // (a cross over the picture to drag the centre, as VEGAS has)
    out[out.size() - 2].overlay = maskOverlayPoint("Center");
    out.back().overlay = maskOverlayPoint("Center");
    {
        std::vector<PDef> ps = {dbl("VerticalWaves", "Vertical waves", 14, 0, 1000), dbl("HorizontalWaves", "Horizontal waves", 14, 0, 1000),
                                dbl("VerticalAmplitude", "Vertical amplitude", 0, -1, 1), dbl("HorizontalAmplitude", "Horizontal amplitude", 0, -1, 1),
                                dbl("VerticalPhase", "Vertical phase", 0, -1000, 1000), dbl("HorizontalPhase", "Horizontal phase", 0, -1000, 1000)};
        for (auto& p : ps) {
            if (p.name.find("Waves") != std::string::npos) p.dmax = 50;
            if (p.name.find("Amplitude") != std::string::npos) { p.dmin = 0; p.dmax = 0.2; }
            if (p.name.find("Phase") != std::string::npos) { p.dmin = 0; p.dmax = 6.2832; }
        }
        out.push_back(filter("wave", "Wave", "The picture rippled up and down and side to side (VEGAS: the defaults leave it as it is)", ps, wave));
    }
}

// ---- for the Vulkan plans (vkgeom.cpp): the numbers the CPU code works out in doubles
void swirlAreaGpu(Ctx& c, int W, int H, SwirlAreaGpu& o)
{
    const vfswirl::Area a = areaOf(c, W, H);
    o.cx = a.cxPix; o.cy = a.cyPix; o.k13 = a.k13; o.syInv = a.syInv; o.rx = a.rx; o.ry = a.ry; o.amount = a.amount;
}
bool waveGpu(Ctx& c, int W, int H, double P[6])
{
    if (W < 2 || H < 2) return false;
    P[0] = clampd(c.d("VerticalWaves"), 0, 1000);
    P[1] = clampd(c.d("HorizontalWaves"), 0, 1000);
    P[2] = clampd(c.d("VerticalAmplitude"), -1, 1);
    P[3] = clampd(c.d("HorizontalAmplitude"), -1, 1);
    P[4] = c.d("VerticalPhase");
    P[5] = c.d("HorizontalPhase");
    return true;
}
void waveKernelConstants(const double P[6], int W, int H, double k[4])
{
    k[0] = P[0] * vfwave::kPi; k[1] = P[1] * vfwave::kPi; k[2] = (double)W * P[2]; k[3] = (double)H * P[3];
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
