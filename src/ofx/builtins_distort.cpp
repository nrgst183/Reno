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

// The ellipse round Center that Swirl and Pinch/Punch work inside (Scaling: size as a fraction of the frame; Proportional: the vertical scaling follows the horizontal one,
// so the ellipse keeps the frame's shape). VEGAS counts from the pixel's index, not its middle: the centre of a 320 x 180 frame is pixel (160, 90), so the area's
// centre is half a pixel further on than the frame's middle (measured on VEGAS's own pictures, exact to 0.01 pixel).
struct Area {
    double px0, py0, rx, ry;
    Area(Ctx& c, int W, int H)
    {
        double cx, cy;
        c.xy("Center", cx, cy);
        const double sh = clampd(c.d("ScalingHorizontal"), 0, 10);
        const double sv = c.b("ScalingProportional") ? sh : clampd(c.d("ScalingVertical"), 0, 10);
        rx = std::max(1e-3, sh * W * 0.5);
        ry = std::max(1e-3, sv * H * 0.5);
        px0 = cx * W + 0.5;
        py0 = (1 - cy) * H + 0.5;
    }
};

// ------------------------------------------------------------------ Swirl
// Turned about Center, most at the centre and not at all at the ellipse's edge: the angle is Amount x 15 radians x (1 - r)^2 for r the distance from the centre in
// ellipse radii (measured on VEGAS's presets: Maximum, Medium, Clockwise, Counter Clockwise, Half Scale; the error is a fraction of a level). Amount 1 turns clockwise.
void swirl(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const Area a(c, W, H);
    const double turn = clampd(c.d("Amount"), -1, 1) * -15.0;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double dx = (x + 0.5 - a.px0) / a.rx, dy = (y + 0.5 - a.py0) / a.ry;
            const double r = std::sqrt(dx * dx + dy * dy);
            float* o = c.out.at(x, y);
            if (r >= 1) { std::memcpy(o, c.src.at(x, y), sizeof(float) * 4); continue; }
            const double f = (1 - r) * (1 - r);
            const double ang = turn * f, ca = std::cos(ang), sa = std::sin(ang);
            // (turned in the ellipse's own units, so a frame-shaped ellipse is a circle for it: the picture is sheared, not rotated rigidly)
            c.src.sampleClamped(a.px0 + (dx * ca - dy * sa) * a.rx, a.py0 + (dx * sa + dy * ca) * a.ry, o);
        }
    });
}

// ------------------------------------------------------------------ Pinch/Punch
// Amount > 0 punches the middle of the ellipse out towards you (magnified), < 0 pinches it in; the edge stays put. The picture is taken from the point
// (1 - 0.8 x Amount x cos^2(pi r / 2)) times as far from the centre (r in ellipse radii): measured on VEGAS's Maximum / Medium Punch and Pinch (0.1 .. 12 levels mean squared).
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
            const double cs = std::cos(kPi * 0.5 * r);
            const double k = 1 - amount * 0.8 * cs * cs;
            c.src.sampleClamped(a.px0 + dx * k * a.rx, a.py0 + dy * k * a.ry, o);
        }
    });
}

// ------------------------------------------------------------------ Wave
// Measured on VEGAS's pictures of its Wave presets (Large, Medium, Small, Vertical Only, Horizontal Only; the sine fits to 0.01 pixel and 0.002 waves): the vertical
// waves move every pixel row sideways, by Amplitude x the frame's WIDTH x sin(pi x Waves x y / height + Phase); the horizontal waves move every column up and down,
// by Amplitude x the frame's HEIGHT x sin(pi x Waves x x / width + Phase). "Waves" counts half waves (Large is 5.4 = 2.7 waves over the frame), the phase is in
// radians, and x and y are the pixel's index (not its middle). The first picture of a pixel is taken from (x + sideways move, y + up/down move), both from the
// pixel's own place.
void wave(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double vn = clampd(c.d("VerticalWaves"), 0, 1000), hn = clampd(c.d("HorizontalWaves"), 0, 1000);
    const double va = clampd(c.d("VerticalAmplitude"), -1, 1) * W, ha = clampd(c.d("HorizontalAmplitude"), -1, 1) * H;
    const double vp = c.d("VerticalPhase"), hp = c.d("HorizontalPhase");
    forRows(H, W, [&](int y) {
        const double sx = va * std::sin(kPi * vn * y / std::max(1, H) + vp);
        for (int x = 0; x < W; ++x) {
            const double sy = ha * std::sin(kPi * hn * x / std::max(1, W) + hp);
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
    out.push_back(filter("swirl", "Swirl", "The picture twisted round a centre", areaParams(1.0), swirl));
    out.push_back(filter("pinchpunch", "Pinch/Punch", "The middle of the picture punched out or pinched in", areaParams(1.0), pinchPunch));
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

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
