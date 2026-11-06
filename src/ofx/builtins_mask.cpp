// Built-in stand-ins for seven VEGAS video effects (they run from VEGAS's own plug-in files when installed):
//
//   com.vegascreativesoftware:bzmasking                  VEGAS Bezier Masking      builtins_mask_bezier.cpp
//   com.vegascreativesoftware:bumpmap                    VEGAS Bump Map (the compositing plug-in; the Video FX one is ...:bumpmapfilter)
//   com.vegascreativesoftware:Filters.Stereoscopic3DAdjust  VEGAS Stereoscopic 3D Adjust (parameters here; the picture: builtins_stereo.cpp)
//   de.magix:LensCorrection                              VEGAS Lens Correction (this file)
//   com.sonycreativesoftware:smart_zoom                  VEGAS Smart Zoom (this file)
//   de.magix:WarpFlowTransition                          VEGAS Warp Flow           builtins_mask_flow.cpp
//   de.magix:TimeWarp                                    VEGAS Slow Motion (VEGAS Pro 18's id for it) (this file)
//
// Identifiers, parameter names, types, choices and labels come from VEGAS's plug-in descriptions (resource files) and presets; defaults come from its
// presets where it has them, otherwise Reno's own, as are ranges, maths and pictures. Parameters needing what Reno has no counterpart of (a lens
// database, motion analysis, auto-align) are declared with the same name and kept in the project, and do nothing.
#include "../core/rmath.h"
#include "vkfilters.h"
#include "builtins_mask_internal.h"
#include "builtins_more_internal.h"
#include "vfxlens.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
namespace more { void bumpMapRender(Ctx& c, const char* srcName, const char* dstName); }
namespace {

const char* const kGroup = "Reno Built-in";

// ------------------------------------------------------------------ Lens Correction
// VEGAS's MagixCV Lens Correction, in single precision on the frame as it lies in memory (bottom row first; the maths is in vfxlens.h): a point at distance r from the centre
// (the index W * x, H * y) comes from f = (k3 u^3 + k2 u^2 + k1 u + 1 - k1 - k2 - k3) / Scale times as far, u = r / (half the shorter side), k1..k3 = RadialDistParam3..1; the picture is
// read bilinearly (the card's 1/256 weights) with transparent black outside the frame. VEGAS's lens database is not there when it ran, so any QueryResult but 0 shows the picture unchanged
// (as do Show Distortion and all-zero settings with Scale 1 do by the same maths). The 8-bit store is the OpenCL one (12-bit step, more::oclStore12).
void lensCorrection(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    double pcx = 0.5, pcy = 0.5;
    c.xy("DistCenter", pcx, pcy);
    if (c.i("QueryResult") != 0 || c.b("ShowMesh")) { c.out.px = c.src.px; return; }
    const double p1 = clampd(c.d("RadialDistParam1"), -2, 2), p2 = clampd(c.d("RadialDistParam2"), -2, 2), p3 = clampd(c.d("RadialDistParam3"), -2, 2);
    const double scale = clampd(c.d("Scale"), 0.05, 20);
    const LensMapSetup su = lensMapMake(W, H, pcx, pcy, p1, p2, p3, scale);
    const bool store8 = c.host8;
    forRows(H, W, [&](int m) {
        float* orow = c.out.at(0, H - 1 - m);
        auto get = [&](int xx, int yb, float* v) { std::memcpy(v, c.src.at(xx, H - 1 - yb), sizeof(float) * 4); };
        for (int x = 0; x < W; ++x) {
            float* o = orow + (size_t)x * 4;
            float mx, my;
            if (!lensMapPlace(su, W, H, x, m, mx, my)) { o[0] = o[1] = o[2] = o[3] = 0.f; continue; }
            lensMapRead(W, H, mx, my, get, o);
            if (store8) for (int k = 0; k < 4; ++k) o[k] = more::oclStore12(o[k]);
        }
    });
}

// ------------------------------------------------------------------ Smart Zoom
// A zoom into the picture that resamples with a Lanczos filter (sharper than bilinear or bicubic) and restores some crispness. The shown part is kept inside the picture: never an empty border.
double lanczos3(double x)
{
    x = std::fabs(x);
    if (x < 1e-8) return 1.0;
    if (x >= 3.0) return 0.0;
    const double px = kPi * x;
    return 3.0 * rm::sin(px) * rm::sin(px / 3.0) / (px * px);
}

// The taps for the output positions 0..n-1 of a zoom: for each the first source index and 6 weights (normalised).
struct Taps {
    std::vector<int> first;
    std::vector<float> w;       // 6 per position
};
void makeTaps(Taps& t, int n, int srcN, double origin, double zoom, double support)
{
    t.first.assign((size_t)n, 0);
    t.w.assign((size_t)n * 6, 0.f);
    // when shrinking the filter would need to widen; the zoom is never below 1 here, so it is the plain 6-tap one
    (void)support;
    for (int i = 0; i < n; ++i) {
        const double s = origin + (i + 0.5) / zoom - 0.5;       // in source pixel indices
        const int f = (int)std::floor(s) - 2;
        double sum = 0;
        float* w = &t.w[(size_t)i * 6];
        for (int k = 0; k < 6; ++k) {
            w[k] = (float)lanczos3(s - (f + k));
            sum += w[k];
        }
        if (sum != 0) for (int k = 0; k < 6; ++k) w[k] = (float)(w[k] / sum);
        t.first[(size_t)i] = f;
        (void)srcN;
    }
}

void smartZoom(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    double cx = 0.5, cy = 0.5, zz = 1;
    c.xyz("LocationAndZoom", cx, cy, zz);
    const double zoom = clampd(zz, 1, 32);
    if (zoom <= 1.0 + 1e-9) { c.out.px = c.src.px; return; }
    cy = 1.0 - cy;                                               // from the top row
    const double rw = W / zoom, rh = H / zoom;
    const double ox = clampd(cx * W - rw * 0.5, 0, W - rw), oy = clampd(cy * H - rh * 0.5, 0, H - rh);
    Taps tx, ty;
    makeTaps(tx, W, W, ox, zoom, 3);
    makeTaps(ty, H, H, oy, zoom, 3);
    const int rowFirst = std::max(0, ty.first.front()), rowLast = std::min(H - 1, ty.first.back() + 5);
    const int rows = std::max(0, rowLast - rowFirst + 1);
    Buf mid;
    mid.ensure(W, std::max(1, rows));
    forRows(rows, W, [&](int r) {
        const float* srow = c.src.at(0, rowFirst + r);
        float* orow = mid.at(0, r);
        for (int x = 0; x < W; ++x) {
            const float* w = &tx.w[(size_t)x * 6];
            const int f = tx.first[(size_t)x];
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < 6; ++k) {
                const int sx = std::min(W - 1, std::max(0, f + k));
                const float* p = srow + (size_t)sx * 4;
                acc[0] += p[0] * w[k]; acc[1] += p[1] * w[k]; acc[2] += p[2] * w[k]; acc[3] += p[3] * w[k];
            }
            std::memcpy(orow + (size_t)x * 4, acc, sizeof acc);
        }
    });
    forRows(H, W, [&](int y) {
        const float* w = &ty.w[(size_t)y * 6];
        const int f = ty.first[(size_t)y];
        float* o = c.out.at(0, y);
        for (int x = 0; x < W; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < 6; ++k) {
                const int sy = std::min(rowLast, std::max(rowFirst, f + k)) - rowFirst;
                const float* p = mid.at(x, sy);
                acc[0] += p[0] * w[k]; acc[1] += p[1] * w[k]; acc[2] += p[2] * w[k]; acc[3] += p[3] * w[k];
            }
            acc[3] = clamp01(acc[3]);
            for (int i = 0; i < 3; ++i) acc[i] = std::min(std::max(acc[i], 0.f), acc[3]);
            std::memcpy(o + (size_t)x * 4, acc, sizeof acc);
        }
    });
    const double sharp = c.b("EnableEnhancement") ? 0.8 * (1.0 - clampd(c.d("Noise"), 0, 1)) : 0.0;
    if (sharp > 0) {
        Buf soft = c.out;
        const double sigma = c.px(0.5 + 0.3 * std::min(zoom, 6.0));
        gaussianBlur(soft, sigma, sigma);
        const float amt = (float)sharp;
        forRows(H, W, [&](int y) {
            float* o = c.out.at(0, y);
            const float* b = soft.at(0, y);
            for (int x = 0; x < W; ++x, o += 4, b += 4) {
                for (int i = 0; i < 3; ++i) o[i] = std::min(std::max(o[i] + (o[i] - b[i]) * amt, 0.f), o[3]);
            }
        });
    }
}

// ------------------------------------------------------------------ Time Warp
// The effect does nothing to a frame: the event plays at the "Slow Motion Ratio" (the engine reads it, eventFxRate in project.cpp, as for Slow Motion) and the engine makes the
// frames in between from the Method.
void timeWarp(Ctx& c) { c.out.px = c.src.px; }

// ------------------------------------------------------------------ Bump Map
// The compositing plug-in's Bump Map lights the frame like the Video FX one (more::bumpMapRender, VEGAS's kernel); only the settings' names differ.
void bumpMapGeneral(Ctx& c) { more::bumpMapRender(c, "DualLocation3DAnd2DSrc", "DualLocation3DAnd2DDst"); }

}  // namespace

void addFiltersMask(std::vector<Def>& out)
{
    addMaskBezier(out);
    {
        // The compositing plug-in's Bump Map (General context in VEGAS): the surface lit is the frame, so Flip Input Surfaces has nothing to swap and is only kept.
        Def d;
        d.id = "com.vegascreativesoftware:bumpmap";
        d.label = "VEGAS Bump Map";
        d.group = kGroup;
        d.description = "Lights the picture as if one of its channels were a relief, with a directional, spot or omnidirectional light.";
        d.params = {group("DualLocation3DAnd2D", "Location"),
                    point3("DualLocation3DAnd2DSrc", "Source", -1, 1, 3, -10, 10),
                    point("DualLocation3DAnd2DDst", "Destination", 0, 0),
                    choice("Type", "Light type", 0, {"Directional", "Spotlight", "Omnidirectional"}),
                    dbl("Intensity", "Intensity", 0.25, 0, 1), dbl("Focus", "Focus", 0.5, 0, 1), dbl("Ambience", "Ambience", 0.1, 0, 1),
                    dbl("Shininess", "Shininess", 0.5, 0, 1), dbl("BumpHeight", "Bump height", 0.5, 0, 1),
                    choice("BumpChannel", "Bump channel", 1, {"Red", "Green", "Blue", "Alpha"}),
                    boolean("WhiteIsHigh", "White is high", true), boolean("FlipInputSurfaces", "Flip input surfaces", false)};
        d.params[1].parent = "DualLocation3DAnd2D";
        d.params[2].parent = "DualLocation3DAnd2D";
        d.params[2].normalised = false; d.params[2].min = -10; d.params[2].max = 10; d.params[2].dmin = -3; d.params[2].dmax = 3;
        d.params[1].dmin = -3; d.params[1].dmax = 6;
        d.params[1].hint = "Light position: across, up and out of the frame, in half frame heights.";
        d.params[2].hint = "Where the light is aimed (the middle of the frame is 0, 0).";
        d.render = bumpMapGeneral;
        d.general = true;
        out.push_back(d);
    }
    {
        Def d;
        d.id = "com.vegascreativesoftware:Filters.Stereoscopic3DAdjust";
        d.label = "VEGAS Stereoscopic 3D Adjust";
        d.group = kGroup;
        d.description = "Lines up the two views of stereoscopic 3D footage: offset, zoom, keystone, rotation, flipping, cropping and floating windows. "
                        "Reno has one picture, the left view, so it adjusts that one.";
        d.params = {dbl("Horizontal", "Horizontal offset", 0, -0.1, 0.1), group("AdjustmentsGroup", "Corrections"),
                    dbl("Vertical", "Vertical offset", 0, -0.1, 0.1, "AdjustmentsGroup"), dbl("Zoom", "Zoom", 0, -0.1, 0.1, "AdjustmentsGroup"),
                    dbl("Keystone", "Keystone", 0, -0.1, 0.1, "AdjustmentsGroup"), dbl("Rotation", "Rotation", 0, -15, 15, "AdjustmentsGroup"),
                    choice("FlipHorizontal", "Flip horizontal", 0, {"Off", "Left", "Right", "Both"}, "AdjustmentsGroup"),
                    choice("FlipVertical", "Flip vertical", 0, {"Off", "Left", "Right", "Both"}, "AdjustmentsGroup"),
                    button("AutoAlignButton", "Auto correct"),
                    boolean("AutoCrop", "Automatic crop", true), dbl("Crop", "Crop", 0, 0, 0.5),
                    group("FloatingWindowsGroup", "Floating windows"),
                    dbl("FloatingWindowLeft", "Left", 0, 0, 0.25, "FloatingWindowsGroup"), dbl("FloatingWindowRight", "Right", 0, 0, 0.25, "FloatingWindowsGroup")};
        d.params[6].hint = "Reno has the one picture, which is the left view: Left and Both flip it.";
        d.params[7].hint = "Reno has the one picture, which is the left view: Left and Both flip it.";
        d.params[8].parent = "AdjustmentsGroup";
        d.params[8].hint = "Reno cannot measure the two views: the button does nothing.";
        d.params[10].hint = "Used when Automatic crop is off: how much of each side to cut away (the picture is enlarged to fill the frame).";
        d.params[13].hint = "The other view's floating window: does nothing to the one picture.";
        d.render = vegasStereo3dAdjust;
        out.push_back(d);
    }
    {
        Def d;
        d.id = "de.magix:LensCorrection";
        d.label = "VEGAS Lens Correction";
        d.group = kGroup;
        d.description = "Removes barrel or pincushion distortion with the PTLens radial model (k1, k2, k3) about a distortion centre; "
                        "Scale hides the empty corners. Reno has no lens database, so enter the numbers.";
        d.params = {choice("Localization", "", 0, {"Select lens", "Nothing found"}), group("GroupDb", "Lens Model"),
                    text("Query", "Search for", "", false), choice("QueryResult", "Found Lenses", 0, {"Nothing found"}, "GroupDb"),
                    boolean("ShowMesh", "Show Distortion", false), point("DistCenter", "Distortion Center", 0.5, 0.5),
                    dbl("Scale", "Scale", 1, 0.05, 20), dbl("RadialDistParam1", "Radial Distortion k1", 0, -2, 2),
                    dbl("RadialDistParam2", "Radial Distortion k2", 0, -2, 2), dbl("RadialDistParam3", "Radial Distortion k3", 0, -2, 2)};
        d.params[1].hint = "Contains a set of lens models.";
        d.params[2].parent = "GroupDb";
        d.params[2].hint = "Enter a lens model name to search for. (Reno has no lens database: the search does nothing.)";
        d.params[3].hint = "Enumerates all lenses fulling query.";
        d.params[4].hint = "Show the distortion which is corrected by the specified parameters.";
        d.params[5].hint = "Adjust the center in case your lens is off-centered.";
        d.params[6].dmin = 0.5; d.params[6].dmax = 2;
        for (int k = 7; k <= 9; ++k) { d.params[(size_t)k].dmin = -0.5; d.params[(size_t)k].dmax = 0.5; }
        d.params[7].hint = "This is the parameter a of ptlens model.";
        d.params[8].hint = "This is the parameter b of ptlens model.";
        d.params[9].hint = "This is the parameter c of ptlens model.";
        d.render = lensCorrection;
        d.overlay = maskOverlayPoint("DistCenter");
        out.push_back(d);
    }
    {
        Def d;
        d.id = "com.sonycreativesoftware:smart_zoom";
        d.label = "VEGAS Smart Zoom";
        d.group = kGroup;
        d.description = "Zooms into a part of the picture with a high-quality (Lanczos) enlargement and sharpens what the enlargement softens. "
                        "The part shown stays inside the picture.";
        d.params = {point3("LocationAndZoom", "Cutout", 0.5, 0.5, 2, -10, 10), dbl("Noise", "Noise sensitivity", 0.2, 0, 1),
                    boolean("EnableEnhancement", "Enable enhancement", true)};
        d.params[0].dmin = 0; d.params[0].dmax = 8;
        d.params[0].hint = "The location (x, y) and zoom (z) of the cut out.";
        d.params[1].hint = "The noise level of the image.";
        d.params[2].hint = "Enable or disable the smart enhancement.";
        d.render = smartZoom;
        d.overlay = maskOverlayPoint("LocationAndZoom");
        out.push_back(d);
    }
    addMaskFlow(out);
    {
        Def d;
        d.id = "de.magix:TimeWarp";
        d.label = "VEGAS Slow Motion";
        d.group = kGroup;
        d.description = "Plays the event slower (or faster) with new in-between frames made by optical flow or blending (VEGAS Pro 18's Time Warp).";
        d.params = {group("Group Parameter", "Parameter"),
                    choice("Choice Box Algorithm", "Method", 1, {"Morph", "Optical Flow"}, "Group Parameter"),
                    choice("Choice Box Quality", "Quality", 1, {"Coarse", "Medium", "Fine"}, "Group Parameter"),
                    dbl("Slow Motion Ratio", "Playback Rate", 0.5, 0.01, 4.0, "Group Parameter"),
                    button("Analyse Button", "Analyze Motion"), button("Clear Button", "Clear Motion Data")};
        d.params[1].hint = "Switches between the different methods for creating intermediate frames.";
        d.params[2].hint = "Switches between the different qualities for creating intermediate frames.";
        d.params[3].hint = "This parameter change the speed ratio of the clip.";
        d.params[4].parent = "Group Parameter";
        d.params[4].hint = "Reno makes the in-between frames as it plays: there is no motion analysis to start.";
        d.params[5].parent = "Group Parameter";
        d.params[5].hint = "Reno keeps no motion analysis: nothing to clear.";
        d.render = timeWarp;
        out.push_back(d);
    }
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
