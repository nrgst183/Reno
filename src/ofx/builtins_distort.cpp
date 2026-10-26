// Built-in stand-ins for VEGAS Pro 18's distortion effects (see builtins.h): Swirl, Pinch/Punch and Wave, with VEGAS's identifiers, parameter names and presets (from Vfx1Presets.xml).
// The frames are Reno's approximation. Guessed: how far Swirl turns at Amount 1 (two turns at the centre, none at the edge of its ellipse), Pinch/Punch's curve, and Wave's units
// (Waves: whole waves across the frame; Amplitude: a fraction of the frame; Phase: in waves, so animating it 0 to 1 moves the waves along by one).
#include "builtins_impl.h"
#include "../core/base.h"
#include <algorithm>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
namespace {

const char* const kGroup = "Reno Built-in";

// The ellipse round Center that Swirl, Pinch/Punch and Spherize work inside (Scaling: size as a fraction of the frame; Proportional: a circle as wide as the horizontal scaling).
struct Area {
    double px0, py0, rx, ry;
    Area(Ctx& c, int W, int H)
    {
        double cx, cy;
        c.xy("Center", cx, cy);
        const double sh = clampd(c.d("ScalingHorizontal"), 0, 10);
        const double sv = c.b("ScalingProportional") ? sh * W / std::max(1, H) : clampd(c.d("ScalingVertical"), 0, 10);
        rx = std::max(1e-3, sh * W * 0.5);
        ry = std::max(1e-3, sv * H * 0.5);
        px0 = cx * W;
        py0 = (1 - cy) * H;
    }
};

// ------------------------------------------------------------------ Swirl
// Turned about Center, most at the centre and not at all at the ellipse's edge: Amount 1 is two turns clockwise, -1 two turns the other way.
void swirl(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const Area a(c, W, H);
    const double turn = clampd(c.d("Amount"), -1, 1) * 2 * 2 * kPi;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double dx = (x + 0.5 - a.px0) / a.rx, dy = (y + 0.5 - a.py0) / a.ry;
            const double r = std::sqrt(dx * dx + dy * dy);
            float* o = c.out.at(x, y);
            if (r >= 1) { std::memcpy(o, c.src.at(x, y), sizeof(float) * 4); continue; }
            const double f = (1 - r) * (1 - r);
            const double ang = turn * f, ca = std::cos(ang), sa = std::sin(ang);
            const double ux = dx * a.rx, uy = dy * a.ry;
            c.src.sampleClamped(a.px0 + ux * ca - uy * sa, a.py0 + ux * sa + uy * ca, o);
        }
    });
}

// ------------------------------------------------------------------ Pinch/Punch
// Amount > 0 punches the middle of the ellipse out towards you (magnified), < 0 pinches it in; the edge stays put.
void pinchPunch(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const Area a(c, W, H);
    const double amount = clampd(c.d("Amount"), -1, 1);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double dx = (x + 0.5 - a.px0) / a.rx, dy = (y + 0.5 - a.py0) / a.ry;
            const double r = std::sqrt(dx * dx + dy * dy);
            float* o = c.out.at(x, y);
            if (r >= 1 || r < 1e-12) { std::memcpy(o, c.src.at(x, y), sizeof(float) * 4); continue; }
            const double s = std::sin(kPi * r);
            const double k = 1 - amount * 0.6 * s * s * (1 - r);
            c.src.sampleClamped(a.px0 + dx * k * a.rx, a.py0 + dy * k * a.ry, o);
        }
    });
}

// ------------------------------------------------------------------ Wave
// Vertical waves move the frame up and down (a wave along its width), horizontal waves left and right (along its height).
void wave(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double vn = clampd(c.d("VerticalWaves"), 0, 1000), hn = clampd(c.d("HorizontalWaves"), 0, 1000);
    const double va = clampd(c.d("VerticalAmplitude"), -1, 1) * H, ha = clampd(c.d("HorizontalAmplitude"), -1, 1) * W;
    const double vp = c.d("VerticalPhase"), hp = c.d("HorizontalPhase");
    forRows(H, W, [&](int y) {
        const double fy = (y + 0.5) / std::max(1, H);
        const double sx = ha * std::sin(2 * kPi * (hn * fy + hp));
        for (int x = 0; x < W; ++x) {
            const double fx = (x + 0.5) / std::max(1, W);
            const double sy = va * std::sin(2 * kPi * (vn * fx + vp));
            c.src.sampleClamped(x + 0.5 + sx, y + 0.5 + sy, c.out.at(x, y));
        }
    });
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
    out.push_back(filter("swirl", "Swirl", "The picture twisted round a centre", areaParams(0.5), swirl));
    out.push_back(filter("pinchpunch", "Pinch/Punch", "The middle of the picture punched out or pinched in", areaParams(0.5), pinchPunch));
    {
        std::vector<PDef> ps = {dbl("VerticalWaves", "Vertical waves", 14, 0, 1000), dbl("HorizontalWaves", "Horizontal waves", 14, 0, 1000),
                                dbl("VerticalAmplitude", "Vertical amplitude", 0.05, -1, 1), dbl("HorizontalAmplitude", "Horizontal amplitude", 0.05, -1, 1),
                                dbl("VerticalPhase", "Vertical phase", 0, -1000, 1000), dbl("HorizontalPhase", "Horizontal phase", 0, -1000, 1000)};
        for (auto& p : ps) {
            if (p.name.find("Waves") != std::string::npos) p.dmax = 50;
            if (p.name.find("Amplitude") != std::string::npos) { p.dmin = 0; p.dmax = 0.2; }
            if (p.name.find("Phase") != std::string::npos) { p.dmin = 0; p.dmax = 1; }
        }
        out.push_back(filter("wave", "Wave", "The picture rippled up and down and side to side", ps, wave));
    }
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
