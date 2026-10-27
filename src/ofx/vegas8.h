// Small helpers for the built-ins that reproduce VEGAS's 8-bit processing exactly (the integer/float steps are copied from VEGAS's own code,
// so the order of the operations and the roundings matter: do not "simplify" them).
#pragma once
#include <cstdint>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
namespace v8 {

// A float picture value (0..1, from an 8-bit picture) as the 8-bit level it came from.
inline int toLevel(float v)
{
    v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    const float s = v * 255.f;
    const int i = (int)s;
    return i + ((s - (float)i) >= 0.5f ? 1 : 0);
}
inline float fromLevel(int b) { return (float)b * (1.f / 255.f); }

// VEGAS's way of rounding a float to a whole number: x + 1.5e-8 in double, then the 2^52 + 2^51 trick (round to nearest, ties to even, on the nudged value).
// The nudge lifts exact halves up. Valid for |x| < 2^31.
inline int roundMagic(double x)
{
    double d = x + 1.5e-8;
    d = d + 6755399441055744.0;
    int64_t bits;
    std::memcpy(&bits, &d, 8);
    return (int)(int32_t)(bits & 0xffffffff);
}

}  // namespace v8
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
