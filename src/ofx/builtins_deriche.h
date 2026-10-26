// VEGAS's Gaussian (the blur of its "Vfx1" plug-ins under Unsharp Mask, Linear Blur and others): Deriche's 4th-order recursive filter in doubles, rows first (row result kept as floats),
// then columns; 8-bit pictures are rounded to the nearest level. See builtins_deriche.cpp.
#pragma once
#include "builtins_impl.h"

namespace reno {
namespace ofx {
namespace builtin {

// The filter's numbers for one sigma (pixels): n (causal), d (feedback), m (anti-causal), bn / bm (the edge terms).
struct DericheCoefs {
    double n[5], d[5], m[5], bn[5], bm[5];
};
DericheCoefs vegasDericheCoefs(double sigma);
// The sigma VEGAS gives a blur of "size" s pixels: (|s| + 1.5) / sqrt(2 ln 255) (as sqrt of the square over 2 ln 255).
double vegasBlurSigma(double s);
// The whole blur of b in place: rows with vegasBlurSigma(sizeX), then columns with vegasBlurSigma(sizeY) (each at least 0.6: size 0 still smooths a little, as in VEGAS).
// levels: b holds 8-bit values (levels / 255) and the result is rounded to whole levels; otherwise the same arithmetic without rounding.
void vegasDericheBlur(Buf& b, double sizeX, double sizeY, bool levels);
// The same with the sigmas themselves.
void vegasDericheBlurSigma(Buf& b, double sigmaX, double sigmaY, bool levels);
// VEGAS's Linear Blur of src into out (same size): the frame is sheared so the blur's direction lies along its rows (or columns), blurred by the Gaussian above along them (and its
// smallest one across), sheared back. Angle in degrees, Amount a part of the frame's larger side. levels: 8-bit pictures (VEGAS's 8-bit arithmetic to the level).
void vegasLinearBlur(const Buf& src, Buf& out, double angleDeg, double amount, bool levels);
// VEGAS's Median (builtins_vegasmedian.cpp): every channel (alpha too) of the 8-bit frame; the window reaches rx / ry pixels each way (rx = HorizontalRange * the larger side * 0.05),
// edge pixels weighted by the fraction inside (round: the share inside the ellipse (rx + 0.5, ry + 0.5)), in 14-bit weights; pixels outside the frame are left out. The result is
// the average of the values over one pixel's weight of the weighted histogram centred on the rank Offset of the way through it.
void vegasMedian(const Buf& src, Buf& out, double rx, double ry, bool round, double offset);

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
