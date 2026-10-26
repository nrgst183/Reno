// Shared by the files of builtins_mask.cpp's group (Bezier Masking, Bump Map, Stereoscopic 3D Adjust, Lens Correction, Smart Zoom, Warp Flow, Time Warp): each adds its effects to the list.
#pragma once
#include "builtins_impl.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace reno {
namespace ofx {

// The host Reno's built-ins were given (null before the first plug-in is set up); builtins.cpp.
const OfxHost* builtinHostPtr();

namespace builtin {

void addMaskBezier(std::vector<Def>& out);
void addMaskFlow(std::vector<Def>& out);
void vegasStereo3dAdjust(Ctx& c);
// Tests: the Warp Flow render with its quality (0..2), smoothness (0..1) and strength (0..2) given, and the flow field shown on request.
void warpFlowForTest(Ctx& c, int quality, double smoothness, double strength, bool showField);

// On-screen controls (builtins_mask_overlay.cpp): OpenFX overlay entry points for the built-ins that have them.
OfxPluginEntryPoint* maskOverlayPoint(const char* param);
OfxPluginEntryPoint* maskOverlayBezier();

#ifndef _WIN32
// Tests: the OpenGL functions the on-screen controls draw with, replaced by these (null: the real ones again).
struct MaskOverlayGL {
    void (*Begin)(unsigned) = nullptr;
    void (*End)() = nullptr;
    void (*Vertex2d)(double, double) = nullptr;
    void (*Color4f)(float, float, float, float) = nullptr;
    void (*LineWidth)(float) = nullptr;
    void (*Enable)(unsigned) = nullptr;
    void (*BlendFunc)(unsigned, unsigned) = nullptr;
};
void maskOverlayUseGL(const MaskOverlayGL* g);
#endif

// A point (a Double2D parameter in the frame's own fractions, 0..1 from the bottom left) as a place in the frame's pixels
// (x across, y down from the top row, as Buf has it).
inline void pointInBuf(const Ctx& c, const char* name, double defX, double defY, double& px, double& py)
{
    double x = defX, y = defY;
    c.xy(name, x, y);
    px = x * c.out.w;
    py = (1.0 - y) * c.out.h;
}

// ------------------------------------------------------------------ Bezier masks
// VEGAS's names for a mask's settings: the setting's name and _ and the mask's number, 0..4 ("Width_0").
inline std::string maskParam(const char* base, int i) { return std::string(base) + "_" + std::to_string(i); }
// A mask's placement: its outline (drawn in the frame's own fractions) is scaled by AnchorScale about the frame's middle, turned by Angle (degrees, anticlockwise) and moved so the
// middle comes to Location. Turning and scaling are done in pixels of the W x H frame, so a circle stays a circle.
struct MaskXf {
    double lx = 0.5, ly = 0.5, sx = 1, sy = 1, ang = 0, W = 1, H = 1;
    void apply(double x, double y, double& ox, double& oy) const
    {
        const double dx = (x - 0.5) * W * sx, dy = (y - 0.5) * H * sy, c = std::cos(ang), s = std::sin(ang);
        ox = lx + (dx * c - dy * s) / W;
        oy = ly + (dx * s + dy * c) / H;
    }
    void invert(double x, double y, double& ox, double& oy) const
    {
        const double rx = (x - lx) * W, ry = (y - ly) * H, c = std::cos(ang), s = std::sin(ang);
        const double ax = std::fabs(sx) < 0.01 ? 0.01 : sx, ay = std::fabs(sy) < 0.01 ? 0.01 : sy;
        ox = 0.5 + (rx * c + ry * s) / (W * ax);
        oy = 0.5 + (-rx * s + ry * c) / (H * ay);
    }
};


// A mask's outline: points with in and out handles, all as fractions of the frame (0..1 from the bottom left, handles as
// places, not offsets). A segment is straight when both handles on it sit on their points.
struct BezPoint {
    double x = 0, y = 0, ix = 0, iy = 0, ox = 0, oy = 0;
};
struct BezPath {
    std::vector<BezPoint> pts;
    bool closed = true;
};

// The text a path is kept in (a hidden parameter): "bz1 C;x,y,ix,iy,ox,oy;..." (C closed, O open). Bad text gives an empty path.
inline bool parseBezPath(const std::string& s, BezPath& p)
{
    p.pts.clear();
    p.closed = true;
    if (s.compare(0, 4, "bz1 ") != 0 || s.size() < 5) return false;
    p.closed = s[4] != 'O';
    size_t at = 5;
    while (at < s.size() && p.pts.size() < 4096) {
        if (s[at] == ';') ++at;
        double v[6];
        int n = 0;
        const char* c = s.c_str() + at;
        while (n < 6) {
            char* end = nullptr;
            const double d = std::strtod(c, &end);
            if (end == c) break;
            v[n++] = d;
            c = end;
            if (*c == ',') ++c;
        }
        if (n == 6) {
            bool finite = true;
            for (int k = 0; k < 6; ++k) finite = finite && v[k] == v[k] && v[k] > -1e6 && v[k] < 1e6;
            if (finite) {
                BezPoint b;
                b.x = v[0]; b.y = v[1]; b.ix = v[2]; b.iy = v[3]; b.ox = v[4]; b.oy = v[5];
                p.pts.push_back(b);
            }
        } else if (n == 0) {
            break;
        }
        at = (size_t)(c - s.c_str());
        while (at < s.size() && s[at] != ';') ++at;
    }
    return !p.pts.empty();
}

inline std::string formatBezPath(const BezPath& p)
{
    std::string s = p.closed ? "bz1 C" : "bz1 O";
    char buf[160];
    for (const BezPoint& b : p.pts) {
        std::snprintf(buf, sizeof buf, ";%.5g,%.5g,%.5g,%.5g,%.5g,%.5g", b.x, b.y, b.ix, b.iy, b.ox, b.oy);
        s += buf;
    }
    return s;
}

// The path with every point and handle moved (or moved back) by a placement.
inline BezPath xfPath(const BezPath& p, const MaskXf& m, bool inverse)
{
    BezPath q = p;
    for (BezPoint& b : q.pts) {
        if (inverse) {
            m.invert(b.x, b.y, b.x, b.y);
            m.invert(b.ix, b.iy, b.ix, b.iy);
            m.invert(b.ox, b.oy, b.ox, b.oy);
        } else {
            m.apply(b.x, b.y, b.x, b.y);
            m.apply(b.ix, b.iy, b.ix, b.iy);
            m.apply(b.ox, b.oy, b.ox, b.oy);
        }
    }
    return q;
}

// The outline of an Oval (0), Rectangle (1) or Diamond (2): w and h the whole width and height, as fractions of the frame, centred on the
// frame's middle (xfPath then places it).
inline BezPath shapeBez(int type, double w, double h)
{
    BezPath p;
    p.closed = true;
    const double hw = w * 0.5, hh = h * 0.5;
    auto corner = [&](double x, double y) { BezPoint b; b.x = b.ix = b.ox = 0.5 + x; b.y = b.iy = b.oy = 0.5 + y; p.pts.push_back(b); };
    if (type == 1) {
        corner(-hw, hh); corner(hw, hh); corner(hw, -hh); corner(-hw, -hh);
    } else if (type == 2) {
        corner(0, hh); corner(hw, 0); corner(0, -hh); corner(-hw, 0);
    } else {
        const double k = 0.5522847498;
        auto sm = [&](double x, double y, double dx, double dy) {
            BezPoint b;
            b.x = 0.5 + x; b.y = 0.5 + y;
            b.ix = b.x - dx; b.iy = b.y - dy;
            b.ox = b.x + dx; b.oy = b.y + dy;
            p.pts.push_back(b);
        };
        sm(0, hh, k * hw, 0);
        sm(hw, 0, 0, -k * hh);
        sm(0, -hh, -k * hw, 0);
        sm(-hw, 0, 0, k * hh);
    }
    return p;
}

// The outline as a polygon in a W x H frame's pixels (y down from the top row): curves cut into short straight pieces.
inline void flattenBezPath(const BezPath& p, double W, double H, std::vector<double>& xy)
{
    xy.clear();
    const size_t n = p.pts.size();
    if (n == 0) return;
    const size_t segs = p.closed ? n : n - 1;
    auto X = [&](double v) { return v * W; };
    auto Y = [&](double v) { return (1.0 - v) * H; };
    for (size_t i = 0; i < segs; ++i) {
        const BezPoint& a = p.pts[i];
        const BezPoint& b = p.pts[(i + 1) % n];
        const double x0 = X(a.x), y0 = Y(a.y), x1 = X(a.ox), y1 = Y(a.oy), x2 = X(b.ix), y2 = Y(b.iy), x3 = X(b.x), y3 = Y(b.y);
        xy.push_back(x0);
        xy.push_back(y0);
        const bool straight = x1 == x0 && y1 == y0 && x2 == x3 && y2 == y3;
        if (straight) continue;
        const double len = std::hypot(x1 - x0, y1 - y0) + std::hypot(x2 - x1, y2 - y1) + std::hypot(x3 - x2, y3 - y2);
        int steps = (int)std::ceil(len / 3.0);
        steps = steps < 8 ? 8 : (steps > 96 ? 96 : steps);
        for (int k = 1; k < steps; ++k) {
            const double t = (double)k / steps, u = 1 - t;
            xy.push_back(u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3);
            xy.push_back(u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3);
        }
    }
    if (!p.closed) {
        xy.push_back(X(p.pts.back().x));
        xy.push_back(Y(p.pts.back().y));
    }
}

// How much of each pixel of a w x h frame a polygon (x, y pairs, y down) covers, 0..1: exact for a polygon that does not cross itself (the swept areas of the edges are added; where
// the outline overlaps itself the count is capped at 1).
void fillPolygonCoverage(const std::vector<double>& xy, int w, int h, std::vector<float>& cover);

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
