// HSL Adjust and White Balance as VEGAS's OpenCL kernels (Vfx1.ofx, prog26 and prog47) draw them with GPU acceleration on: float32 arithmetic on the premultiplied
// pixel values, one pixel at a time, nothing cut to whole levels between the steps. Re-implemented from the kernels' arithmetic (notes: HSL_Adjust.md, White_Balance.md).
// No Reno types in here, so the standalone tests can include it.
#pragma once
#include "../core/rmath.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace vhw {

// ---------------------------------------------------------------------------------------------------------------------------------- HSL Adjust
// The colour is taken as it is stored (premultiplied) and treated as a straight colour; the alpha is copied. Nothing is clamped before the final store.
struct Hsl { float h, s, l; };

inline Hsl rgbToHsl(float r, float g, float b)
{
    const float EPS = 0.000001f;
    float c[3] = {r, g, b};
    float mx = std::max(std::max(c[0], c[1]), c[2]);
    float mn = std::min(std::min(c[0], c[1]), c[2]);
    // The channel the max / min is written back to: the first that is strictly the biggest / smallest, else the green or the blue by their order.
    auto setMax = [&](float v) { if (c[0] > c[1] && c[0] > c[2]) c[0] = v; else if (c[1] > c[2]) c[1] = v; else c[2] = v; };
    auto setMin = [&](float v) { if (c[0] < c[1] && c[0] < c[2]) c[0] = v; else if (c[1] < c[2]) c[1] = v; else c[2] = v; };
    // A colour whose min + max is almost exactly 2 (white) or 0 (black) is nudged away from it.
    float sum = mn + mx;
    if (sum > 2.0f - EPS && sum < 2.0f) { mx = mx - EPS; setMax(mx); }
    else if (sum >= 2.0f && sum < 2.0f + EPS) { mx = mx + EPS; setMax(mx); }
    sum = mn + mx;
    if (sum > 0.0f - EPS && sum < 0.0f) { mn = mn - EPS; setMin(mn); }
    else if (sum >= 0.0f && sum < 0.0f + EPS) { mn = mn + EPS; setMin(mn); }
    const float L = (mn + mx) * 0.5f;
    float S = mx - mn;
    if (S < -EPS || S >= EPS) {
        S = (L <= 0.5f) ? S / (mn + mx) : S / ((2.0f - mx) - mn);
        if (L < 0.0f || L > 1.0f) S = S * -1.0f;
    } else {
        return Hsl{0.0f, S, L};
    }
    const float d = 1.0f / (mx - mn);
    const float cr = (mx - c[0]) * d, cg = (mx - c[1]) * d, cb = (mx - c[2]) * d;
    float H;
    if (c[0] == mx) H = (c[1] == mn) ? 5.0f + cb : 1.0f - cg;
    else if (c[1] == mx) H = (c[2] == mn) ? 1.0f + cr : 3.0f - cb;
    else H = (c[0] == mn) ? 3.0f + cg : 5.0f - cr;
    H = H * (1.0f / 6.0f);
    return Hsl{H, S, L};
}

inline void hslToRgb(float H, float S, float L, float* out)
{
    float q;
    if (L <= 0.5f) {
        if (L < 0.0f) S = S * -1.0f;
        q = L * (1.0f + S);
    } else {
        if (L > 1.0f) S = S * -1.0f;
        q = (L + S) - L * S;
    }
    const float t = H * 6.0f;
    const float p = (L + L) - q;
    const int i = (int)t;                       // (C conversion: towards zero)
    const float f = t - (float)i;
    const float q1 = (q - p) * f;
    const float up = p + q1, dn = q - q1;
    switch (i) {
    case 0: case 6: out[0] = q; out[1] = up; out[2] = p; break;
    case 1: out[0] = dn; out[1] = q; out[2] = p; break;
    case 2: out[0] = p; out[1] = q; out[2] = up; break;
    case 3: out[0] = p; out[1] = dn; out[2] = q; break;
    case 4: out[0] = up; out[1] = p; out[2] = q; break;
    case 5: out[0] = q; out[1] = p; out[2] = dn; break;
    default: out[0] = H; out[1] = S; out[2] = L; break;     // (the kernel leaves its input as it was)
    }
}

// The three settings as the host hands them to the kernel (each clamped, then a float).
struct HslSettings { float hue, sat, lum; };
inline HslSettings hslSettings(double addToHue, double saturation, double luminance)
{
    auto cl = [](double v, double hi) { return v < 0.0 ? 0.0 : (v > hi ? hi : v); };    // (NaN goes to hi's side like max/min would not: keep it at 0)
    HslSettings s;
    s.hue = (float)(addToHue == addToHue ? cl(addToHue, 1.0) : 0.0);
    s.sat = (float)(saturation == saturation ? cl(saturation, 2.0) : 0.0);
    s.lum = (float)(luminance == luminance ? cl(luminance, 2.0) : 0.0);
    return s;
}

// One pixel: p = premultiplied RGBA as stored, o gets the result (alpha copied, the colour not premultiplied again).
inline void hslPixel(const HslSettings& st, const float* p, float* o)
{
    Hsl a = rgbToHsl(p[0], p[1], p[2]);
    float h = a.h + st.hue;
    h = h - std::floor(h);
    const float s = a.s * st.sat, l = a.l * st.lum;
    hslToRgb(h, s, l, o);
    o[3] = p[3];
}

// ---------------------------------------------------------------------------------------------------------------------------------- White Balance
// The settings of the colour shift: Angle (degrees), Amount (0..1, squared inside), Brightness. The kernel works on the straight colour of the pixel.
struct WbShift { float angle, amount, bright; };

// Per-channel stretch of the HasData option: out = clamp(gain * (c - off), 0, 1) on the shifted, premultiplied values.
struct WbStretch { float gain[3], off[3]; };

inline void wbPixel(const WbShift& sh, bool stretch, const WbStretch& st, const float* p, float* o)
{
    float a = p[3] > 1.0f ? 1.0f : p[3];
    if (!(a > 0.000001f)) {
        o[0] = o[1] = o[2] = o[3] = 0.0f;
        return;
    }
    float r = p[0], g = p[1], b = p[2];
    if (a < 1.0f) { r /= a; g /= a; b /= a; }
    float y = (0.299f * r) + (0.587f * g) + (0.114f * b);
    float u = (0.492f) * (b - y);
    float v = (0.877f) * (r - y);
    const float m2 = sh.amount * sh.amount;
    const float ang = sh.angle * 3.1415926535897932384f / 180.0f;
    const float du = reno::rm::cos(ang) * m2 * 0.5f, dv = reno::rm::sin(ang) * m2 * 0.5f;
    if (sh.bright != 1.0f) {
        // f0(y, 1, 100, 0, 1): in the linear part (|y| below 100) y * 1, then the brightness
        y = y * 1.0f;
        y = y * sh.bright;
    }
    u += du;
    v += dv;
    const float R = y + (1.14025085518814f * v);
    const float G = y + (-0.394731374911705f * u) + (-0.580809209031098f * v);
    const float B = y + (2.032520325203250f * u);
    o[0] = R * a;
    o[1] = G * a;
    o[2] = B * a;
    o[3] = a;
    if (stretch) {
        for (int k = 0; k < 3; ++k) {
            const float t = st.gain[k] * (o[k] - st.off[k]);
            o[k] = std::max(0.0f, std::min(1.0f, t));
        }
    }
}

// The HasData stretch from a histogram of the picture's 8-bit planes (counts[256] of one channel): the running sum scaled to 1 in float, lo = the first bin whose sum reaches 0.005,
// hi = the last bin whose sum is at most 0.995; gain 256 / (hi - lo), offset lo / 256.
inline void wbStretchChannel(const int* counts, float& gain, float& off)
{
    float cum[256];
    float run = 0.0f;
    for (int i = 0; i < 256; ++i) { run += (float)counts[i]; cum[i] = run; }
    gain = 1.0f;
    off = 0.0f;
    if (!(cum[255] > 0.0f)) return;
    const float sc = (float)(1.0 / (double)cum[255]);
    for (int i = 0; i < 256; ++i) cum[i] *= sc;
    int lo = 0;
    while (lo < 255 && 0.005f > cum[lo]) ++lo;
    int hi = 255;
    while (hi > 0 && (double)cum[hi] > 0.9950000001117587) --hi;
    gain = (hi == lo) ? 1.0f : 256.0f / (float)(hi - lo);
    off = (float)lo * (1.0f / 256.0f);
}

// convert_uchar_sat(255 * v): truncated, held to 0..255 (NaN 0).
inline int wbPlaneByte(float v)
{
    const float t = 255.0f * v;
    if (!(t > 0.0f)) return 0;
    if (t >= 255.0f) return 255;
    return (int)t;
}

// The gain-only pass (kernel 995, run on the shift pass's picture when HasData is on): clamp(gain * (c - off), 0, 1) on the three colour values, alpha as it is.
inline void wbStretchPixel(const WbStretch& st, const float* p, float* o)
{
    for (int k = 0; k < 3; ++k) {
        const float t = st.gain[k] * (p[k] - st.off[k]);
        o[k] = std::max(0.0f, std::min(1.0f, t));
    }
    o[3] = p[3];
}

// How many pixels the histogram takes in, counted from the bottom row of the picture (VEGAS's host sums only part of the partial histograms; the capture card's compute units x work-group
// size x 4 would be 28 x 256 x 4 = 28672). The captures fit 28384..28676 when the colours are not shifted (HasData alone) and 30432..33568 (14 captures: Brightness 2 with a shift) and
// 30368..39776 (Brightness 1.05) when the shift pass's picture is analysed: 28672 and 32768 are used. A GUESS where it comes from; other frame sizes and cards are not captured.
inline int wbHistPixels(bool shifted) { return shifted ? 32768 : 28672; }

// ---------------------------------------------------------------------------------------------------------------------------------- Temperature and tint
// Robertson's isotherm table (mired, u, v, slope) as VEGAS keeps it, and the colour of a temperature and tint as the angle (degrees) and size of the correction that turns it grey.
const double kRob[31][4] = {
    {0.0, 0.18006, 0.26352, -0.24341},    {10.0, 0.18066, 0.26589, -0.25479},   {20.0, 0.18133, 0.26846, -0.26876},
    {30.0, 0.18208, 0.27119, -0.28539},   {40.0, 0.18293, 0.27407, -0.3047},    {50.0, 0.18388, 0.27709, -0.32675},
    {60.0, 0.18494, 0.28021, -0.35156},   {70.0, 0.18611, 0.28342, -0.37915},   {80.0, 0.1874, 0.28668, -0.40955},
    {90.0, 0.1888, 0.28997, -0.44278},    {100.0, 0.19032, 0.29326, -0.47888},  {125.0, 0.19462, 0.30141, -0.58204},
    {150.0, 0.19962, 0.30921, -0.70471},  {175.0, 0.20525, 0.31647, -0.84901},  {200.0, 0.21142, 0.32312, -1.0182},
    {225.0, 0.21807, 0.32909, -1.2168},   {250.0, 0.22511, 0.33439, -1.4512},   {275.0, 0.23247, 0.33904, -1.7298},
    {300.0, 0.2401, 0.34308, -2.0637},    {325.0, 0.24702, 0.34655, -2.4681},   {350.0, 0.25591, 0.34951, -2.9641},
    {375.0, 0.264, 0.352, -3.5814},       {400.0, 0.27218, 0.35407, -4.3633},   {425.0, 0.28039, 0.35577, -5.3762},
    {450.0, 0.28863, 0.35714, -6.7262},   {475.0, 0.29685, 0.35823, -8.5955},   {500.0, 0.30505, 0.35907, -11.324},
    {525.0, 0.3132, 0.35968, -15.628},    {550.0, 0.32129, 0.36011, -23.325},   {575.0, 0.32931, 0.36038, -40.77},
    {600.0, 0.33724, 0.36051, -116.45}};

// With the tint at 0 the dialog's neutral grey (0.5, 0.5, 0.5) stands for 6000 K / tint 10.0 (not 10.5): captures with only the temperature moved are exact with 10.0.
const double kWbNeutralTint = 10.0;

// The colour (R, G, B, in 0..1 and not held to it) that a temperature (Kelvin) and a tint stand for: Robertson's isotherm table, moved along the isotherm by tint / -3000,
// then x, y, XYZ and VEGAS's matrix to RGB (changedParam 0x180588220 and helper 0x180587cd0). 'lum' is the Y of the white: VEGAS keeps the luminance of the last neutral colour it was
// given (changedParam's +0xd8: 0.3439664498 R + 0.7281660966 G - 0.0721325464 B, 0.5 for its default grey) and makes the colour of a temperature that bright.
inline void tempTintToRgb(double kelvin, double tint, double* rgb, double lum = 1.0)
{
    const double t13 = tint * -0.0003333333333333333;
    const double m = 1000000.0 / kelvin;
    int i = 0;
    for (;; ++i)
        if (kRob[i + 1][0] > m || i == 29) break;
    const int k = i + 1;
    const double f = (kRob[k][0] - m) / (kRob[k][0] - kRob[i][0]);
    const double si = kRob[i][3], sk = kRob[k][3];
    const double qi = std::sqrt(si * si + 1.0), qk = std::sqrt(sk * sk + 1.0);
    const double g = 1.0 - f;
    const double dx = (1.0 / qk) * g + (1.0 / qi) * f;
    const double dy = (sk / qk) * g + (si / qi) * f;
    const double nrm = std::sqrt(dy * dy + dx * dx);
    double u = g * kRob[k][1] + f * kRob[i][1];
    double v = g * kRob[k][2] + f * kRob[i][2];
    v += (dy / nrm) * t13;
    u += (dx / nrm) * t13;
    const double den = (u - v * 4.0) + 2.0;
    const double y = v / den, x = (u * 1.5) / den;
    const double q = lum / y;
    const double X = q * x, Z = ((1.0 - x) - y) * q;
    rgb[0] = X * 1.0498110175 - Z * 9.74845e-05;
    rgb[1] = (lum * 1.3733130458 - X * 0.4959030231) + Z * 0.0982400361;
    rgb[2] = Z * 0.9912520182;
}

// The direction (degrees, 0..360) and size of the correction that turns a colour grey, as VEGAS keeps them in Angle and AmountOfCorrection (helper 0x180587dc0): the colour's BT.601 U and V,
// the angle of (-U, -V), and the fourth root of U^2 + V^2 as the 'Amount' (not held to 0..1 here; the parameter's range does that). Read from the code and checked against the DLL's own
// function (0x180587dc0, called through the oracle loader).
inline void rgbToAngleAmount(const double* rgb, double& angle, double& amount)
{
    const double r = rgb[0] * 255.0, g = rgb[1] * 255.0, b = rgb[2] * 255.0;       // (the host hands the colour over in 0..255)
    const double yl = (g * 0.587 + r * 0.299) + b * 0.114;
    const double U = ((b - yl) * 0.492000013589859) / 127.5, V = ((r - yl) * 0.8769999742507935) / 127.5;
    double ang;
    if (-U == 0.0) ang = (-V >= 0.0) ? 90.0 : 270.0;
    else {
        ang = reno::rm::atan2(-V, -U) * 57.29577951308232;
        if (ang < 0.0) ang += 360.0;
    }
    angle = ang;
    amount = std::sqrt(std::sqrt(U * U + V * V));
}

// The temperature and tint of a colour (the other way round, 0x18058f300): false when the colour has no chromaticity (black, or not a number), where VEGAS leaves the old temperature and tint alone.
inline bool rgbToTempTint(const double* rgb, double& kelvin, double& tint)
{
    const double Y = 0.3439664498 * rgb[0] + 0.7281660966 * rgb[1] - 0.0721325464 * rgb[2];
    const double X = 0.9525523959 * rgb[0] + 9.36786e-05 * rgb[2];
    const double Z = 1.0088251844 * rgb[2];
    const double sum = Y + (X + Z);
    const double x = X / sum, y = Y / sum;
    const double den = (1.5 - x) + 6.0 * y;
    const double u = (x + x) / den, v = (y * 3.0) / den;
    double prevD = 0.0, prevN = 0.0, prevS = 0.0;
    int i = 1;
    double d = 0.0, curN = 0.0, curS = 0.0;
    for (;;) {
        const double s = kRob[i][3], n = std::sqrt(s * s + 1.0);
        d = (v - kRob[i][2]) * (1.0 / n) - (u - kRob[i][1]) * (s / n);
        curN = 1.0 / n;
        curS = s / n;
        if (0.0 >= d || i == 30) break;
        prevD = d; prevN = curN; prevS = curS;
        ++i;
        if (i > 30) return false;
    }
    if (!(d == d)) return false;                 // (a number that is not one: the loop of VEGAS runs off its table)
    const double nd = (0.0 < d) ? 0.0 : d;
    const double w = -nd;
    double f = 0.0;
    if (i != 1) f = w / (w + prevD);
    const double g = 1.0 - f;
    const double mired = g * kRob[i][0] + f * kRob[i - 1][0];
    kelvin = 1000000.0 / mired;
    double dx = g * curN + f * prevN, dy = g * curS + f * prevS;
    const double nr = std::sqrt(dx * dx + dy * dy);
    dx /= nr; dy /= nr;
    const double uI = g * kRob[i][1] + f * kRob[i - 1][1], vI = g * kRob[i][2] + f * kRob[i - 1][2];
    tint = ((v - vI) * dy + (u - uI) * dx) * -3000.0;
    return kelvin == kelvin && tint == tint;
}

}  // namespace vhw
