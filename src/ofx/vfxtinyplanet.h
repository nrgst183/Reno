// VEGAS's Tiny Planet (stereographic projection) as its OpenCL kernel draws it, written from the decoded kernel and checked against it and against VEGAS's own pictures.
// All in 32-bit floats in the kernel's order. The kernel's rows run bottom first, so y is counted from the bottom of the frame.
#pragma once
#include <cmath>
#include "../core/rmath.h"

namespace reno {
namespace ofx {
namespace vftp {

struct Params {
    int w = 0, h = 0;               // size of the source picture (the kernel measures the source, not the output)
    float latitude = 0.f;           // degrees, already reduced with fmod(, 360)
    float longitude = 0.f;
    float z = 20.f;
};

// Where output pixel (x, y) reads the source: sx is a whole number (the kernel cuts the column to an int), sy is the row to blend between. Both are NaN where the kernel divides 0 by 0 (the middle pixel).
inline void sourcePos(const Params& p, int x, int y, float& sx, float& sy)
{
    const float pi = 3.14159265358979323846f;
    const float v28 = (float)p.w / p.z;
    // (fitted to VEGAS's pictures: the card's multiply-add in the angle, which lands the seams of Latitude 180 on the same pixels; and cos of the float -pi/2 (Latitude 0) is 0 there, not -4.4e-8,
    // which with Z = 0 puts every pixel on the middle column)
    const float v29 = std::fma(p.latitude * pi, 1.0f / 180, -(pi / 2));
    const float v30 = p.longitude * pi * (1.0f / 180);
    const float cos29 = v29 == -(pi / 2) ? 0.f : rm::cosf(v29);
    const int v31 = x - p.w / 2;
    const int v32 = y - p.h / 2;
    const float v33 = std::sqrt((float)(v31 * v31 + v32 * v32));
    const float v34 = 2 * rm::atanf(v33 / (2 * v28));
    const float v35 = pi / 2 + rm::asinf(rm::cosf(v34) * rm::sinf(v29) + (float)v32 * rm::sinf(v34) * cos29 / v33);
    float v36 = v30 + rm::atan2f((float)v31 * rm::sinf(v34), v33 * cos29 * rm::cosf(v34) - (float)v32 * rm::sinf(v29) * rm::sinf(v34));
    v36 = v36 + pi;
    if (v36 < 0) v36 += 2 * pi;
    else if (v36 > 2 * pi) v36 -= 2 * pi;
    sy = (float)(p.h - 1) * v35 / pi;
    const float col = (float)(p.w - 1) * v36 / (pi * 2);
    sx = std::trunc(col);       // (the kernel's (int) cast; col stays within a few frame widths)
}

}  // namespace vftp
}  // namespace ofx
}  // namespace reno
