// Fast algorithms behind the built-in effects that were too slow to preview at full size (see builtins_fast.cpp): sliding-window Min/Max and Median, box filters along a line,
// polar-coordinate smears from a centre, a disc blur by row sums, and a blur by way of a reduced-size copy. They work on the framework's Buf (premultiplied RGBA floats).
#pragma once
#include "builtins_impl.h"
#include <functional>

namespace reno {
namespace ofx {
namespace builtin {

// The smallest / largest value in the (2 rx + 1) x (2 ry + 1) neighbourhood of every pixel, all four channels, edges
// clamped. 'round': only the offsets inside the ellipse (i / rx)^2 + (j / ry)^2 <= 1. Exactly what the plain loop gives.
void extremeFilter(const Buf& src, Buf& out, int rx, int ry, bool round, bool maximum, double ax = 0, double ay = 0);

// The median of each colour channel over the same neighbourhoods (element at sorted position count / 2), plus 'offset', then limited to 0 .. the pixel's alpha; alpha is the source's.
// Returns false (out untouched) when the frame has colour values outside 0..1, which the histograms cannot take (the caller does it the slow way). Exact when the values are 8-bit steps
// (what video gives), else within 1/65535.
bool medianFilter(const Buf& src, Buf& out, int rx, int ry, bool round, float rankFrac);

// Smears 'in' along the line with direction (ux, uy) (a unit vector, image coordinates) by a chain of box filters of
// the given lengths in pixels (1 box: a plain box average; 2 equal ones: a triangle). out = result * scale, or out +=
// when 'add'. 'out' must be the size of 'in' (and not the same frame).
void lineBoxBlur(const Buf& in, Buf& out, double ux, double uy, const double* lens, int nbox, float scale, bool add);

// Radial Blur: Type 0 smears each pixel along the line to the centre (px0, py0) (pixels) over +-strength*0.25 of its
// distance, 1 over a fixed +-strength*0.06*minWH, 2 the same with a Gaussian fall-off.
void radialBlurFast(const Buf& src, Buf& out, double px0, double py0, int type, double strength, int minWH);

// What Light Rays / Rays add: for every pixel, 1/taps of the sum over t = 1 .. taps of g(centre + (p - centre) * f_t) * (1 - t / taps), f_t = 1 - t / taps * span, g the frame's (bilinear)
// sample put through 'fxPixel' (in: 4 floats, out: 4 floats; rgb is summed, alpha ignored; empty: colours as they are). out has the sums in rgb.
void marchTowardsCentre(const Buf& src, Buf& out, double px0, double py0, double span, int taps, const std::function<void(const float* in, float* out)>& fxPixel);

// Defocus (builtins_defocus.cpp), as the DLL does it. defocusIris: the iris as the DLL draws it: weights (bytes: 200 for a flat iris) of the offsets (u, v), u and v from -half to half, row by row.
// 'radiusPx' is the iris radius in pixels (Radius / 100 x the frame's larger side), 'sides' 0 for a circle else the polygon's number of sides, 'turn' its turn in radians (clockwise),
// 'curvature' the Disc curvature (-1 .. 0, below 0 the rim is dimmer). defocusRender: the frame's 8-bit values with the bloom (above 'thresh' (0..1) the part above it gains 'strength' times
// as much), convolved with the iris, normalised and rounded to bytes; fills out. 'reduceFlicker': the 8-bit frame is first blurred by [1 2 1; 2 4 2; 1 2 1] / 16 (truncated).
struct DefocusIris {
    int half = 0;
    std::vector<int> w;
};
DefocusIris defocusIris(double radiusPx, int sides, double turn, double curvature);
void defocusRender(const Buf& src, Buf& out, double radiusPx, int sides, double turn, double curvature, double thresh, double strength, bool reduceFlicker);

// A Gaussian blur of every channel that, for a large sigma, is done on a smaller copy of the frame.
void softBlur(Buf& b, double sigma);
// The reduction factor softBlur uses for this sigma (1: the blur is done at full size).
int softBlurFactor(double sigma);
// 'low' = 'in' reduced by averaging blocks of f x f pixels (the blocks at the right and bottom edges may be smaller).
void reduceBox(const Buf& in, Buf& low, int f);
// Upsamples a w x h frame (with c = 1 or 4 floats per pixel) of 'lowW' x 'lowH' that holds the f-times reduced
// frame's pixels (block centres at (i + 0.5) f), smoothly, into out (w x h).
void upsampleBilinear(const std::vector<float>& low, int lowW, int lowH, int c, int f, std::vector<float>& out, int w, int h);

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
