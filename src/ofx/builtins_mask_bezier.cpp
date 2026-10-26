// Built-in stand-in for VEGAS's Bezier Masking (com.vegascreativesoftware:bzmasking, VEGAS Pro 18 and later): up to five masks, each an Oval, Rectangle, Diamond or free Curve,
// placed (Location, Angle, and for a curve AnchorScale), feathered and given an opacity; together they are the picture's transparency. Parameters are VEGAS's own (names, types,
// choices and the defaults its Circle / Square / Diamond presets set); a curve's points are kept in AnchorList_n, a hidden text in Reno's own format (builtins_mask_internal.h), not
// VEGAS's. Motion tracking (Tracking_n) is declared so projects keep their settings but does nothing; Mask FX (using the mask on the following effects) needs the host's effect
// chain and is not done. Blend is how much of the unmasked picture shows through the masks' outside.
//
// The shapes are drawn and edited on the preview (builtins_mask_overlay.cpp) while Edit Mode is on for a mask.
#include "builtins_mask_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {

// ------------------------------------------------------------------ exact coverage of a polygon
namespace {

// One edge's contribution to the coverage accumulation: the area it sweeps to the right in every pixel it passes (as font rasterisers do: a pixel's cover is the running sum along its row). 'a' has w + 2 values a row.
void accumulateEdge(float* a, int stride, int w, int h, double x0, double y0, double x1, double y1)
{
    if (y0 == y1) return;
    double dir = 1.0;
    if (y0 > y1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
        dir = -1.0;
    }
    if (y1 <= 0 || y0 >= h) return;
    const double dxdy = (x1 - x0) / (y1 - y0);
    double x = x0;
    if (y0 < 0) { x -= y0 * dxdy; y0 = 0; }
    const int yStart = (int)y0, yEnd = std::min(h, (int)std::ceil(y1));
    for (int y = yStart; y < yEnd; ++y) {
        float* row = a + (size_t)y * stride;
        const double dy = std::min((double)(y + 1), y1) - std::max((double)y, y0);
        const double xnext = x + dxdy * dy;
        const double d = dy * dir;
        double xa = std::min(x, xnext), xb = std::max(x, xnext);
        xa = std::min(std::max(xa, 0.0), (double)w);
        xb = std::min(std::max(xb, 0.0), (double)w);
        const double xmid = std::min(std::max(0.5 * (x + xnext), 0.0), (double)w);
        const double xaFloor = std::floor(xa);
        const int xai = (int)xaFloor;
        const double xbCeil = std::ceil(xb);
        const int xbi = (int)xbCeil;
        if (xbi <= xai + 1) {
            const double xmf = xmid - xaFloor;
            row[xai] += (float)(d - d * xmf);
            row[xai + 1] += (float)(d * xmf);
        } else {
            const double s = 1.0 / (xb - xa);
            const double xaf = xa - xaFloor;
            const double a0 = 0.5 * s * (1.0 - xaf) * (1.0 - xaf);
            const double xbf = xb - xbCeil + 1.0;
            const double am = 0.5 * s * xbf * xbf;
            row[xai] += (float)(d * a0);
            if (xbi == xai + 2) {
                row[xai + 1] += (float)(d * (1.0 - a0 - am));
            } else {
                const double a1 = s * (1.5 - xaf);
                row[xai + 1] += (float)(d * (a1 - a0));
                for (int xi = xai + 2; xi < xbi - 1; ++xi) row[xi] += (float)(d * s);
                const double a2 = a1 + (xbi - xai - 3) * s;
                row[xbi - 1] += (float)(d * (1.0 - a2 - am));
            }
            row[xbi] += (float)(d * am);
        }
        x = xnext;
    }
}

}  // namespace

void fillPolygonCoverage(const std::vector<double>& xy, int w, int h, std::vector<float>& cover)
{
    cover.assign((size_t)std::max(0, w) * std::max(0, h), 0.f);
    const size_t n = xy.size() / 2;
    if (n < 3 || w <= 0 || h <= 0) return;
    const int stride = w + 2;
    std::vector<float> acc((size_t)stride * h, 0.f);
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        const double x0 = xy[2 * i], y0 = xy[2 * i + 1], x1 = xy[2 * j], y1 = xy[2 * j + 1];
        if (!(std::isfinite(x0) && std::isfinite(y0) && std::isfinite(x1) && std::isfinite(y1))) continue;
        accumulateEdge(acc.data(), stride, w, h, x0, y0, x1, y1);
    }
    for (int y = 0; y < h; ++y) {
        const float* row = acc.data() + (size_t)y * stride;
        float sum = 0.f;
        float* o = &cover[(size_t)y * w];
        for (int x = 0; x < w; ++x) {
            sum += row[x];
            o[x] = std::min(1.f, std::fabs(sum));
        }
    }
}

namespace {

const char* const kGroup = "Reno Built-in";
const int kMasks = 5;

void bezierMasking(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    const size_t N = (size_t)W * H;
    std::vector<float> total;
    std::vector<float> cover, soft;
    bool any = false;
    std::vector<double> xy;
    for (int i = 0; i < kMasks; ++i) {
        if (!c.b(maskParam("Enable", i).c_str())) continue;
        MaskXf xf;
        xf.W = W;
        xf.H = H;
        c.xy(maskParam("Location", i).c_str(), xf.lx, xf.ly);
        xf.ang = c.d(maskParam("Angle", i).c_str()) * kPi / 180.0;
        const int type = std::max(0, std::min(3, c.i(maskParam("Type", i).c_str())));
        BezPath path;
        double bx0 = 0, bx1 = 0, bj0 = 0, bj1 = 0;
        bool empty = false;
        if (type == 3) {
            if (!parseBezPath(c.s(maskParam("AnchorList", i).c_str()), path) || path.pts.size() < 3) empty = true;
            else c.xy(maskParam("AnchorScale", i).c_str(), xf.sx, xf.sy);
        } else {
            const double w = clampd(c.d(maskParam("Width", i).c_str()), 0, 4), h = clampd(c.d(maskParam("Height", i).c_str()), 0, 4);
            if (w <= 0 || h <= 0) empty = true;
            else {
                // VEGAS fits the shape into whole pixels: the box of pixel indices [centre - half, centre + half) (rows from the bottom), drawn with soft edges for an oval or a diamond
                bx0 = std::ceil(xf.lx * W - w * W * 0.5); bx1 = std::ceil(xf.lx * W + w * W * 0.5);
                bj0 = std::ceil(xf.ly * H - h * H * 0.5); bj1 = std::ceil(xf.ly * H + h * H * 0.5);
                const double bw = std::max(1.0, bx1 - bx0), bh = std::max(1.0, bj1 - bj0);
                xf.lx = (bx0 + bx1) * 0.5 / W;
                xf.ly = (bj0 + bj1) * 0.5 / H;
                path = shapeBez(type, bw / W, bh / H);
            }
        }
        if (empty) {
            cover.assign(N, 0.f);
        } else if (type == 1 && std::fabs(std::fmod(xf.ang, 2 * kPi)) < 1e-9) {
            // VEGAS draws a plain rectangle without softened edges: a pixel is in when its index (row from the bottom) lies in [centre - half, centre + half)
            cover.assign(N, 0.f);
            for (int y = 0; y < H; ++y) {
                const double j = H - 1 - y;
                if (!(j >= bj0 && j < bj1)) continue;
                for (int x = 0; x < W; ++x)
                    if (x >= bx0 && x < bx1) cover[(size_t)y * W + x] = 1.f;
            }
        } else {
            flattenBezPath(xfPath(path, xf, false), W, H, xy);
            fillPolygonCoverage(xy, W, H, cover);
        }
        // the feather (0..50 as in VEGAS) is a Gaussian of 0.54 pixels a step at 180 rows (measured), a fraction of the frame's height, so it is the same at any render scale
        const double feather = clampd(c.d(maskParam("Feather", i).c_str()), 0, 50);
        if (feather > 0 && !empty) {
            soft = cover;
            const double sigma = feather * 0.003 * H;
            gaussianBlurPlane(soft, W, H, sigma, sigma);
            const int style = c.i(maskParam("FeatherStyle", i).c_str());       // In, Out, Both
            for (size_t k = 0; k < N; ++k) {
                if (style == 0) cover[k] = std::min(cover[k], soft[k]);
                else if (style == 1) cover[k] = std::max(cover[k], soft[k]);
                else cover[k] = soft[k];
            }
        }
        const float opacity = (float)clampd(c.d(maskParam("Opacity", i).c_str()), 0, 1);
        if (!any) {
            total.assign(N, 0.f);
            any = true;
        }
        for (size_t k = 0; k < N; ++k) {
            const float o = clamp01(cover[k]) * opacity;
            total[k] = 1.f - (1.f - total[k]) * (1.f - o);
        }
    }
    if (c.b("MaskFx")) {
        c.out.px = c.src.px;
        return;
    }
    if (!any) {
        std::fill(c.out.px.begin(), c.out.px.end(), 0.f);
        return;
    }
    const bool invert = c.b("Invert");
    const float blend = (float)clampd(c.d("Blend"), 0, 1);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float m = total[(size_t)y * W + x];
            if (invert) m = 1.f - m;
            m += (1.f - m) * blend;
            const float* s = c.src.at(x, y);
            float* o = c.out.at(x, y);
            o[0] = s[0] * m; o[1] = s[1] * m; o[2] = s[2] * m; o[3] = s[3] * m;
        }
    });
}

}  // namespace

void addMaskBezier(std::vector<Def>& out)
{
    Def d;
    d.id = "com.vegascreativesoftware:bzmasking";
    d.label = u8"VEGAS Bézier Masking";
    d.group = kGroup;
    d.description = "Up to five masks (oval, rectangle, diamond or a curve drawn with points and handles), feathered and given an opacity, make the "
                    "picture transparent outside (or inside) them. Turn on Edit Mode to move a mask on the preview. "
                    "Mask FX and tracking are not supported.";
    d.params.push_back(group("General Options", "General Options"));
    d.params.push_back(boolean("Invert", "Invert", false, "General Options"));
    d.params.push_back(boolean("MaskFx", "Mask FX", false, "General Options"));
    d.params.back().hint = "In VEGAS the effects that follow apply only inside the masks. Reno keeps the setting but does not do this.";
    d.params.push_back(dbl("Blend", "Blend", 0, 0, 1, "General Options"));
    d.params.back().hint = "How much of the picture outside the masks is kept.";
    for (int i = 0; i < kMasks; ++i) {
        const std::string g = "Mask_" + std::to_string(i), n = std::to_string(i + 1);
        auto P = [&](const char* base) { return maskParam(base, i); };
        d.params.push_back(group(g, "Mask " + n));
        d.params.push_back(boolean(P("Enable"), "Enable " + n, i == 0, g));
        d.params.back().hint = "To apply motion tracking using the area defined by this mask, open the Motion Tracking dialog and choose \"Bezier Mask\" from the Mask drop-down list.";
        d.params.push_back(choice(P("Type"), "Type", 1, {"Oval", "Rectangle", "Diamond", "Curve"}, g));
        d.params.push_back(boolean(P("EditMode"), "Edit Mode", false, g));
        d.params.back().hint = "Shows the mask's outline and handles on the preview to move them.";
        d.params.push_back(choice(P("EditTool"), "Edit Tool", 0, {"Normal", "Add", "Delete", "Split"}, g));
        d.params.push_back(dbl(P("Width"), "Width " + n, i == 0 ? 0.5 : 0.25, 0, 4, g));
        d.params.back().dmax = 1;
        d.params.push_back(dbl(P("Height"), "Height " + n, i == 0 ? 0.5 : 0.444, 0, 4, g));
        d.params.back().dmax = 1;
        d.params.push_back(dbl(P("Angle"), "Angle " + n, 0, -3600, 3600, g));
        d.params.back().dmin = -360; d.params.back().dmax = 360;
        d.params.push_back(dbl(P("Opacity"), "Opacity " + n, 1, 0, 1, g));
        d.params.push_back(dbl(P("Feather"), "Feather " + n, 0, 0, 50, g));
        d.params.push_back(choice(P("FeatherStyle"), "Feather Style " + n, 2, {"In", "Out", "Both"}, g));
        d.params.push_back(point(P("Location"), "Location " + n, 0.5, 0.5, g));
        d.params.back().min = -10; d.params.back().max = 10; d.params.back().dmin = 0; d.params.back().dmax = 1;
        d.params.push_back(point(P("AnchorScale"), "Anchor Scale", 1, 1, g));
        d.params.back().min = 0.01; d.params.back().max = 100; d.params.back().dmin = 0.1; d.params.back().dmax = 4;
        d.params.back().normalised = false;
        d.params.push_back(custom(P("AnchorList"), "Anchor List", ""));
        d.params.back().parent = g;
        d.params.push_back(group(P("Tracking"), "Tracking"));
        d.params.back().parent = g;
        d.params.push_back(group(P("OptionsTrk"), "Options"));
        d.params.back().parent = P("Tracking");
        d.params.push_back(choice(P("PrecisionTrk"), "Precision", 0, {"Normal", "High"}, P("OptionsTrk")));
        d.params.push_back(choice(P("ModeTrk"), "Mode", 0, {"Location", "Rotation & Location", "Shape & Location", "Perspective"}, P("OptionsTrk")));
        d.params.push_back(choice(P("DirectionTrk"), "Direction", 0, {"Forward", "Backward", "Both"}, P("OptionsTrk")));
        d.params.push_back(dbl(P("TrkShear"), "Shear", 0, 0, 1, P("OptionsTrk")));
        d.params.push_back(intp(P("KFIntervalTrk"), "Keyframe Interval", 5, 1, 1000, P("OptionsTrk")));
        d.params.push_back(button(P("StartTrk"), "Start"));
        d.params.back().parent = P("Tracking");
    }
    d.render = bezierMasking;
    d.overlay = maskOverlayBezier();
    out.push_back(d);
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
