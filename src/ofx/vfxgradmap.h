// VEGAS Gradient Map, as its older (DirectX) filter draws it: a re-implementation of what its render function does (read from the user's own VEGAS install, vfx1.dll, and checked
// bit for bit against that function on random gradients and pictures, 8-bit and float), not a fit. No dependencies, so a test program can include it alone.
//
// The gradient is a list of stops (colour, alpha, position). Two key-frame curves are laid through the stops sorted by position: the alpha, and the colour times the alpha. Their
// interpolation is the DLL's key-frame spline (a cubic Bezier per segment whose handles come from the neighbouring keys, tension 0.5, handle bias 0.8), not a straight line; the ends
// repeat the first and last key. An 8-bit picture goes through a table of 256 entries (the curves read at i / 255, cut to bytes); a pixel's grey (weights .114 B + .587 G + .299 R,
// in 14-bit fixed point) picks the entry, whose alpha is scaled by Amount, and the picture is blended with (Blend) or has added (Add) the entry's colour. The alpha of the picture
// grows by the entry's alpha. A float picture reads the curves at the grey itself.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace reno {
namespace ofx {
namespace vfxgm {

struct Stop {
    float c[4] = {0, 0, 0, 0};      // blue, green, red, alpha (straight colour, as VEGAS keeps them)
    double pos = 0;
    double extra = 1.0;             // the second value of the alpha key; 1 in all of VEGAS's own data
};
struct Gradient {
    double amount = 1.0;
    int mode = 0;                   // 0 Blend, 1 Add
    std::vector<Stop> stops;        // at most 9
};

// one key of a curve: time, three values, the handle bias (the DLL's points have five doubles)
struct Key { double d[5]; };

// the handle on the far side of key B for the keys A, B, C around it, as the spline builds them (tension t)
inline Key handle(const Key& A, const Key& B, const Key& C, double t)
{
    double da[5], db[5];
    for (int k = 0; k < 5; ++k) { da[k] = B.d[k] - A.d[k]; db[k] = C.d[k] - B.d[k]; }
    const double la = std::sqrt(da[1] * da[1] + da[2] * da[2] + da[3] * da[3]);
    const double lb = std::sqrt(db[1] * db[1] + db[2] * db[2] + db[3] * db[3]);
    if (la == 0 && lb == 0) return B;
    double sa, sc;
    if (lb > la) { sc = (la / lb) * t; sa = t + 1.0; }
    else { sa = (lb / la) * t + 1.0; sc = t; }
    Key h;
    for (int k = 0; k < 5; ++k) {
        const double pa = da[k] * sa + A.d[k];
        const double pc = db[k] * sc + B.d[k];
        h.d[k] = (pc - pa) * 0.5 + pa;
    }
    return h;
}

// the cubic Bezier through P0 .. P3 at u (de Casteljau, as the DLL orders it)
inline Key bezier(const Key& P0, const Key& P1, const Key& P2, const Key& P3, double u)
{
    Key a, b, c, d, e, f;
    for (int k = 0; k < 5; ++k) {
        a.d[k] = (P1.d[k] - P0.d[k]) * u + P0.d[k];
        b.d[k] = (P2.d[k] - P1.d[k]) * u + P1.d[k];
        c.d[k] = (P3.d[k] - P2.d[k]) * u + P2.d[k];
    }
    for (int k = 0; k < 5; ++k) {
        d.d[k] = (b.d[k] - a.d[k]) * u + a.d[k];
        e.d[k] = (c.d[k] - b.d[k]) * u + b.d[k];
    }
    for (int k = 0; k < 5; ++k) f.d[k] = (e.d[k] - d.d[k]) * u + d.d[k];
    return f;
}

// the curve through keys sorted by time, read at t
inline Key evalCurve(const std::vector<Key>& K, double t)
{
    const double tension = 0.5;
    const int n = (int)K.size();
    int lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const int mid = (hi + lo) >> 1;
        if (K[mid].d[0] <= t) lo = mid; else hi = mid;
    }
    int i = lo;
    while (i < n && K[i].d[0] <= t) ++i;
    auto at = [&](int j) -> const Key& { return K[j < 0 ? 0 : (j > n - 1 ? n - 1 : j)]; };
    const Key& m2 = at(i - 2);
    const Key& m1 = at(i - 1);
    const Key& p0 = at(i);
    const Key& p1 = at(i + 1);
    Key h1 = handle(m2, m1, p0, tension), h2 = handle(p1, p0, m1, tension);
    const double third = 1.0 / 3.0;
    const double w1 = m1.d[4], w2 = p0.d[4];
    if (1.0 > w1)
        for (int k = 0; k < 5; ++k) {
            const double q = (p0.d[k] - m1.d[k]) * third + m1.d[k];
            h1.d[k] = (h1.d[k] - q) * w1 + q;
        }
    if (1.0 > w2)
        for (int k = 0; k < 5; ++k) {
            const double r = (m1.d[k] - p0.d[k]) * third + p0.d[k];
            h2.d[k] = (h2.d[k] - r) * w2 + r;
        }
    const double dt = p0.d[0] - m1.d[0];
    double u = 0;
    if (dt > 0) u = (t - m1.d[0]) / dt;
    return bezier(m1, h1, h2, p0, u);
}

// the two curves of a gradient
struct Curves {
    std::vector<Key> alpha, colour;
    bool empty() const { return alpha.empty(); }
};

inline Curves prepare(const Gradient& g)
{
    Curves cv;
    const int n = (int)std::min<size_t>(g.stops.size(), 9);
    // sorted by position: the smallest of those not taken yet, the earlier one first on a tie (the DLL starts its scan at 100000)
    std::vector<int> order;
    std::vector<char> used((size_t)n, 0);
    for (int k = 0; k < n; ++k) {
        double best = 100000.0;
        int pick = -1;
        for (int j = 0; j < n; ++j)
            if (!used[(size_t)j] && best > g.stops[(size_t)j].pos) { best = g.stops[(size_t)j].pos; pick = j; }
        if (pick < 0)
            for (int j = 0; j < n; ++j)
                if (!used[(size_t)j]) { pick = j; break; }
        used[(size_t)pick] = 1;
        order.push_back(pick);
    }
    cv.alpha.resize((size_t)n);
    cv.colour.resize((size_t)n);
    for (int k = 0; k < n; ++k) {
        const Stop& s = g.stops[(size_t)order[(size_t)k]];
        Key& a = cv.alpha[(size_t)k];
        a.d[0] = s.pos; a.d[1] = (double)s.c[3]; a.d[2] = s.extra; a.d[3] = 0; a.d[4] = 0.8;
        Key& c = cv.colour[(size_t)k];
        c.d[0] = s.pos;
        for (int q = 0; q < 3; ++q) c.d[1 + q] = (double)(s.c[q] * s.c[3]);      // (the product is made in float)
        c.d[4] = 0.8;
    }
    return cv;
}

// the gradient at t: premultiplied blue, green, red (not above the alpha) and the alpha, floats 0 .. 1
inline void eval(const Curves& cv, double t, float out[4])
{
    const Key a = evalCurve(cv.alpha, t), c = evalCurve(cv.colour, t);
    const double av = a.d[1];
    const float al = av < 0 ? 0.0f : (float)std::min(1.0, av);
    for (int k = 0; k < 3; ++k) {
        const double v = c.d[1 + k];
        out[k] = v < 0 ? 0.0f : (float)std::min((double)al, v);
    }
    out[3] = al;
}

// the 8-bit table: 256 entries of blue, green, red, alpha bytes
inline void buildTable(const Curves& cv, uint8_t lut[256][4])
{
    for (int i = 0; i < 256; ++i) {
        float o[4];
        eval(cv, (double)i / 255.0, o);
        for (int k = 0; k < 4; ++k) {
            const float x = o[k] < 0.f ? 0.f : std::min(1.0f, o[k]);
            lut[i][k] = (uint8_t)(int)(x * 255.0f);
        }
    }
}

// 8-bit pictures: bytes B, G, R, A premultiplied, w x h, rows pitch bytes apart; in place
inline void run8(uint8_t* px, int pitch, int w, int h, const Gradient& g)
{
    const Curves cv = prepare(g);
    if (cv.empty()) return;
    uint8_t lut[256][4];
    buildTable(cv, lut);
    const int amtQ = (int)(g.amount * 16384.0);
    for (int y = 0; y < h; ++y) {
        uint8_t* p = px + (size_t)y * (size_t)pitch;
        for (int x = 0; x < w; ++x, p += 4) {
            const unsigned lum = ((unsigned)p[0] * 0x74bu + (unsigned)p[1] * 0x2591u + (unsigned)p[2] * 0x1322u + 0x2000u) >> 14;
            const uint8_t* L = lut[lum];
            const int t = (int)((double)((int)L[3] << 14) * (double)amtQ * 6.103515625e-05);
            const int an = std::min(255, (t >> 14) + (int)p[3]);
            const int wq = t / 255;
            p[3] = (uint8_t)an;
            for (int k = 0; k < 3; ++k) {
                const int v = g.mode ? ((int)L[k] * wq) / 16384 + (int)p[k]
                                     : ((int)L[k] * wq + (int)p[k] * (0x4000 - wq)) / 16384;
                p[k] = (uint8_t)std::min(v, an);
            }
        }
    }
}

// float pictures: floats B, G, R, A premultiplied; in place
inline void runFloat(float* px, int pitchBytes, int w, int h, const Gradient& g)
{
    const Curves cv = prepare(g);
    if (cv.empty()) return;
    const float amt = (float)g.amount;
    for (int y = 0; y < h; ++y) {
        float* p = (float*)((uint8_t*)px + (size_t)y * (size_t)pitchBytes);
        for (int x = 0; x < w; ++x, p += 4) {
            double lum = (double)p[0] * 0.114;
            lum = lum + (double)p[1] * 0.587;
            lum = lum + (double)p[2] * 0.299;
            float o[4];
            eval(cv, lum, o);
            const float ta = o[3] * amt;
            if (g.mode) {
                const float r0 = o[0] * ta + p[0], r1 = o[1] * ta + p[1], r2 = o[2] * ta + p[2];
                p[0] = r0; p[1] = r1; p[2] = r2;
            } else {
                const float om = 1.0f - ta;
                const float r0 = om * p[0] + o[0] * ta, r1 = om * p[1] + o[1] * ta, r2 = om * p[2] + o[2] * ta;
                p[0] = r0; p[1] = r1; p[2] = r2;
            }
            const float a = ta + p[3];
            p[3] = a < 1.0f ? a : 1.0f;
        }
    }
}

// The settings text "position r g b a;position r g b a;..." (straight colour 0 .. 1, as Color Gradient's stops). Empty or unreadable: no stops, which VEGAS's "Reset to None" also is
// in effect (every stop transparent).
inline std::vector<Stop> parseStops(const std::string& s)
{
    std::vector<Stop> v;
    size_t i = 0;
    while (i < s.size() && v.size() < 9) {
        size_t j = s.find(';', i);
        if (j == std::string::npos) j = s.size();
        double n[5];
        int k = 0;
        const char* p = s.c_str() + i;
        const char* end = s.c_str() + j;
        while (k < 5) {
            char* q = nullptr;
            const double x = std::strtod(p, &q);
            if (q == p || q > end) break;
            n[k++] = x;
            p = q;
        }
        if (k == 5 && std::isfinite(n[0]) && std::isfinite(n[1]) && std::isfinite(n[2]) && std::isfinite(n[3]) && std::isfinite(n[4])) {
            Stop st;
            st.pos = n[0];
            st.c[2] = (float)n[1];      // red
            st.c[1] = (float)n[2];
            st.c[0] = (float)n[3];      // blue
            st.c[3] = (float)n[4];
            v.push_back(st);
        }
        i = j + 1;
    }
    return v;
}

}  // namespace vfxgm
}  // namespace ofx
}  // namespace reno
