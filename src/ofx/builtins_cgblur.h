// CoreGraphics.Native's Gaussian blur (GaussianBlurNative, used by VEGAS's GaussianBlur filter and so by Glint, Fill Light, Soft Contrast ...), worked out from its machine code:
// a third-order recursive filter after Young and van Vliet in single precision, rows first then columns. See builtins_cgblur.cpp.
#pragma once
#include <cstddef>

namespace reno {
namespace ofx {
namespace builtin {

// The filter's three feedback numbers and its gain for a standard deviation (clamped to 0.5..54 pixels first, as the caller does).
struct CgBlurCoefs {
    float c1, c2, c3, gain;
};
CgBlurCoefs cgBlurCoefs(double sigma);
// Blurs px (w x h pixels of four interleaved floats, any unit; byte pictures hold 0..255) in place: every row with sigmaX, then every column with sigmaY (each at least 0.5, so
// even 0 smooths a little). The result is not rounded: byte pictures round with cgBlurByte().
void cgGaussianBlur(float* px, int w, int h, double sigmaX, double sigmaY);
// GaussianBlurOCL (the OpenCL path): the recursive filter's numbers for a (builder) deviation, and the blur itself on the same kind of picture (0..255 floats, not rounded).
struct OclBlurCoefs {
    double n[5], d[5], t[5], gc, ga;
};
OclBlurCoefs oclBlurCoefs(double sigma);
double oclBlurSigma(double deviation);
void cgOclBlur(float* px, int w, int h, double sigmaX, double sigmaY);
// The same for a single plane of floats (0..255), one channel instead of four (alpha-only blurs).
void cgGaussianBlurPlane(float* px, int w, int h, double sigmaX, double sigmaY);
// The deviation VEGAS's GaussianBlur takes from a pixel diameter (its spline's reverse lookup of diameter / 2); 0 for no blur. In builtin::more.
namespace more {
double cgSigmaForDiameter(double px);
}
// The native byte conversion: nearest (ties to even) of v + 1.5e-8, held to 0..255.
int cgBlurByte(float v);

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
