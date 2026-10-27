// The filter design routines of VEGAS's sfppack2 DLL (its "SfSig" library: band-pass, low and high shelf coefficients), as
// the DLL has them, with the same constants, tables and order of floating point operations. The Paragraphic EQ uses them;
// they are kept apart so other effects that use the same routines can share them.
//
// A "stage" is the DLL's 7 doubles: [0] and [1] the two states, [2] [3] [4] the coefficients of a two pole resonator
// (found by shelfBase) and [5] [6] [7] what the routine adds for its kind of filter.
#pragma once
#include <cmath>

namespace reno {
namespace vegsig {

// The width compensation table of the band-pass: q (rows), frequency (columns) and the factors
inline const double* tableQ()
{
    static const double t[7] = {0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 101.0};
    return t;
}
inline const double* tableF()
{
    static const double t[7] = {0.0, 0.0011337868480725624, 0.0022675736961451248, 0.022675736961451247, 0.11337868480725624, 0.22675736961451248, 0.5};
    return t;
}
inline const double* tableV()
{
    static const double t[49] = {
        4.5351473922902495e-05, 0.0016553287981859411, 0.003197278911564626, 0.034013605442176874, 0.22675736961451248, 0.45351473922902497, 1.0,
        3.401360544217687e-05, 0.0016553287981859411, 0.003197278911564626, 0.034013605442176874, 0.22675736961451248, 0.45351473922902497, 1.0,
        2.2675736961451248e-05, 0.001179138321995465, 0.002494331065759637, 0.026077097505668934, 0.1473922902494331, 0.36281179138321995, 0.8,
        2.2675736961451248e-05, 0.001179138321995465, 0.002494331065759637, 0.024263038548752835, 0.13038548752834467, 0.2947845804988662, 0.6518820861678004,
        2.2675736961451248e-05, 0.001179138321995465, 0.00235827664399093, 0.023582766439909298, 0.12471655328798185, 0.2494331065759637, 0.5500453514739229,
        2.2675736961451248e-05, 0.0011564625850340137, 0.0023129251700680273, 0.023356009070294784, 0.12018140589569161, 0.2494331065759637, 0.5500453514739229,
        2.2675736961451248e-05, 0.0011564625850340137, 0.0023129251700680273, 0.023356009070294784, 0.12018140589569161, 0.2494331065759637, 0.5500453514739229};
    return t;
}
// the table of the high shelf's tilt, in dB
inline const double* tableTilt()
{
    static const double t[16] = {0.0, -1.0, -2.0, -3.0, -4.0, -5.0, -5.0, -6.0, -7.0, -8.0, -9.0, -10.0, -11.0, -12.0, -12.0, -13.0};
    return t;
}

// SfSig 0x18008c600: the frequency of a band-pass, corrected by its q (a bilinear look up in the table)
inline double widthComp(double f, double q)
{
    const double* T1 = tableQ();
    const double* T2 = tableF();
    const double* V = tableV();
    int i = 0;
    {
        const double* ptr = T1;
        do {
            if (*ptr >= q) break;
            ++ptr;
            ++i;
        } while (ptr < T1 + 7);
    }
    int j = 0;
    if (i < 7) {
        const double* row = V + i * 7;
        long rcx = 0;
        const double* ptr = row;
        do {
            if (*ptr >= f) break;
            ++rcx;
            ++j;
            ++ptr;
        } while (rcx < 7);
    }
    if ((unsigned)(i - 1) > 5u || (unsigned)(j - 1) > 5u) return 1.0 * f;
    const double Va = V[(i - 1) * 7 + j - 1], Vb = V[(i - 1) * 7 + j];
    const double a3 = T2[j - 1] / Va, b3 = T2[j] / Vb;
    double r = a3;
    if (!(Va == Vb) && !(a3 == b3) && !(f == Va)) {
        if (f == Vb) r = b3;
        else r = ((b3 - a3) * (f - Va)) / (Vb - Va) + a3;
    }
    const double Va2 = V[i * 7 + j - 1], Vb2 = V[i * 7 + j];
    const double a1 = T2[j - 1] / Va2, b2 = T2[j] / Vb2;
    double r1 = a1;
    if (!(Va2 == Vb2) && !(a1 == b2) && !(f == Va2)) {
        if (f == Vb2) r1 = b2;
        else r1 = ((b2 - a1) * (f - Va2)) / (Vb2 - Va2) + a1;
    }
    const double q0 = T1[i - 1], q1 = T1[i];
    if (!(q0 == q1) && !(r == r1) && !(q == q0)) {
        if (q == q1) r = r1;
        else r = ((r1 - r) * (q - q0)) / (q1 - q0) + r;
    }
    return r * f;
}

// SfSig 0x18008c3b0: the tilt of the high shelf for a frequency, as a gain (10^(dB/20)) from the table
inline double tiltGain(double f)
{
    const double s = 0.022675736961451247;
    double x = f;
    if (0.0003 > x) x = 0.0003;
    else if (x > 0.49) x = 0.49;
    double acc = s;
    unsigned ecx = 1;
    while (true) {
        if (acc >= x) { break; }
        acc += s;
        if (acc >= x) { ecx += 1; break; }
        acc += s;
        if (acc >= x) { ecx += 2; break; }
        acc += s;
        if (acc >= x) { ecx += 3; break; }
        acc += s;
        if (acc >= x) { ecx += 4; break; }
        ecx += 5;
        acc += s;
        if (!(ecx < 0x10)) break;
    }
    if (ecx >= 0x10) {
        ecx -= 1;
        acc = acc + (0.0 - s);
    }
    const double* P = tableTilt();
    const double lo = acc - s;
    const double p1 = P[ecx], p0 = P[ecx - 1];
    double r = p0;
    if (!(lo == acc) && !(p0 == p1) && !(x == lo)) {
        if (x == acc) r = p1;
        else r = ((p1 - p0) * (x - lo)) / (acc - lo) + p0;
    }
    return std::pow(10.0, r / 20.0);
}

// SfSig 0x18008c2c0: the two pole resonator of a frequency (f of the rate) and q; out[2..4]. 'reset' clears the states.
inline void shelfBase(double* out, double f, double q, int reset)
{
    if (0.0003 > f) f = 0.0003;
    else if (f > 0.49) f = 0.49;
    const double e1 = std::exp((f * (-3.141592653589793)) / q);
    const double c = std::cos(f * 6.283185307179586);
    const double b1 = e1 * (c * 2.0);
    out[2] = b1;
    const double e2 = std::exp(-((f * 6.283185307179586) / q));
    out[3] = e2;
    out[4] = (1.0 - b1) + e2;
    if (reset) {
        out[0] = 0.0;
        out[1] = 0.0;
    }
}

// SfSig 0x18008c520: the gain of a band-pass: p = {frequency, gain in dB, q}; out[5]
inline void bandGain(double* out, const double* p)
{
    const double g = p[1];
    if (g == 0.0) return;
    const double G = std::pow(10.0, g / 20.0);
    const double d = G - 1.0;
    if (g > 0.0) out[5] = d / p[2];
    const double c = p[0] > 0.001814 ? 0.85 : 0.65;
    double k;
    if (1.0 > p[2]) k = (1.0 - p[2]) * 0.9 + c;
    else k = c / p[2];
    out[5] = k * d;
}

// SfSig_FindBandPassCoeffs (0x18008cab0): p = {frequency / rate, gain in dB, q}
inline void bandPass(double* out, const double* p, int reset)
{
    double f = p[0];
    if (0.0003 > f) f = 0.0003;
    else if (f > 0.49) f = 0.49;
    const double q = p[2];
    const double w = widthComp(f, q);
    const double pm[3] = {w, p[1], p[2]};
    bandGain(out, pm);
    double g;
    if (0.0003 > w) g = 0.0003;
    else if (w > 0.49) g = 0.49;
    else g = w;
    const double a = g * 3.141592653589793;
    const double b = g * 6.283185307179586;
    const double e1 = std::exp(-(a / q));
    const double c2 = e1 * (std::cos(b) * 2.0);
    out[2] = c2;
    const double e2 = std::exp(-(b / q));
    out[3] = e2;
    const double t = (1.0 - c2) + e2;
    out[4] = std::sqrt(t) * std::cos(a);
    if (reset) {
        out[0] = 0.0;
        out[1] = 0.0;
    }
}

// SfSig_FindLowShelfBoostCoeffs (0x18008c890)
inline void lowShelfBoost(double* out, const double* p, int reset)
{
    shelfBase(out, p[0] / 1.5, p[2], reset);
    out[6] = 1.0;
    out[7] = std::pow(10.0, p[1] / 40.0) - 1.0;
}

// SfSig_FindLowShelfCutCoeffs (0x18008c7f0): minGain is the gain (dB) at and below which the shelf cuts everything
inline void lowShelfCut(double* out, const double* p, double minGain, int reset)
{
    const double f = 0.011337868480725623 > p[0] ? p[0] / 2.2 : p[0] / 3.3;
    shelfBase(out, f, p[2], reset);
    if (minGain < p[1]) out[6] = std::pow(10.0, p[1] / 40.0);
    else out[6] = 0.0;
    out[7] = 1.0 - out[6];
}

// SfSig_FindHiShelfBoostCoeffs (0x18008ca20)
inline void hiShelfBoost(double* out, const double* p, int reset)
{
    shelfBase(out, p[0], p[2], reset);
    out[4] = 1.0;
    out[6] = 1.0;
    out[5] = tiltGain(p[0]);
    out[7] = std::pow(10.0, p[1] / 40.0) - 1.0;
}

// SfSig_FindHiShelfCutCoeffs (0x18008c910)
inline void hiShelfCut(double* out, const double* p, double minGain, int reset)
{
    double f = p[0];
    if (0.0004 > f) {
        f = 0.0004 * 0.4;
    } else {
        if (f <= 0.49 && 0.125 > f) {
            f = f * 0.4;
        } else {
            if (f > 0.49) f = 0.49;
            f = f * ((f - 0.125) * 1.6 + 0.4);
        }
    }
    shelfBase(out, f, p[2], reset);
    out[5] = tiltGain(f);
    out[6] = 1.0;
    if (minGain < p[1]) out[7] = std::pow(10.0, p[1] / 40.0) - 1.0;
    else out[7] = -1.0;
}

}  // namespace vegsig
}  // namespace reno
