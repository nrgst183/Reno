// Fast algorithms behind the slow built-in effects, part 2: box filters along a line at any angle (Linear Blur, Glint, Starburst) and smears towards or away
// from a centre in polar coordinates (Radial Blur, Light Rays, Rays); see builtins_fast.h.
#include "builtins_fast.h"
#include "builtins_vec.h"
#include "../core/base.h"
#include <cmath>
#include <cstring>
#include <memory>

namespace reno {
namespace ofx {
namespace builtin {

using namespace v4;

namespace {

// ------------------------------------------------------------------ boxes along a line
// The frame swapped across its diagonal (x and y exchanged), into 'out'; 'add': added to it instead.
void transposeInto(const Buf& in, Buf& out, bool accum)
{
    const int W = in.w, H = in.h;                    // out is H x W
    if (!accum) out.ensure(H, W);
    const int B = 16;
    const int blocks = (H + B - 1) / B;
    parallelRows(blocks, (size_t)W * B * 8, [&](int b0, int b1) {
        for (int b = b0; b < b1; ++b) {
            const int ya = b * B, yb = std::min(H, ya + B);
            for (int xa = 0; xa < W; xa += B) {
                const int xb = std::min(W, xa + B);
                for (int y = ya; y < yb; ++y)
                    for (int x = xa; x < xb; ++x) {
                        const float* s = &in.px[((size_t)y * W + (size_t)x) * 4];
                        float* d = &out.px[((size_t)x * H + (size_t)y) * 4];
                        st(d, accum ? add(ld(d), ld(s)) : ld(s));
                    }
            }
        }
    });
}

// One row of the sheared copy: L pixels of 'cur' (columns xs .. xs + L - 1, taken along the line that is 'slope' rows
// down per column and passes row R at column xa), then the chain of boxes over it; leaves Tn pixels in cur.
struct BoxChain {
    int boxes = 0;
    double wc[4];
    int pad[4];
    int padTot = 0;
    int li[4], hi[4];
    float lf[4], hf[4], inv[4];
    void add(double w)
    {
        if (boxes >= 4 || w < 0.25) return;          // a box narrower than a quarter of a pixel changes nothing
        wc[boxes] = w;
        pad[boxes] = (int)std::ceil(w / 2) + 2;
        padTot += pad[boxes];
        const double loOff = 0.5 - w / 2, hiOff = 0.5 + w / 2;
        li[boxes] = (int)std::floor(loOff);
        hi[boxes] = (int)std::floor(hiOff);
        lf[boxes] = (float)(loOff - li[boxes]);
        hf[boxes] = (float)(hiOff - hi[boxes]);
        inv[boxes] = (float)(1.0 / w);
        ++boxes;
    }
    void run(float*& cur, float*& nxt, float* P, int L) const
    {
        int len = L;
        for (int b = 0; b < boxes; ++b) {
            V4 acc = zero();
            st(P, acc);
            for (int j = 0; j < len; ++j) {
                acc = v4::add(acc, ld(cur + (size_t)j * 4));
                st(P + (size_t)(j + 1) * 4, acc);
            }
            const V4 vlf = set(lf[b]), vhf = set(hf[b]), vinv = set(inv[b]);
            const int n = len - 2 * pad[b];
            const float* ph = P + (size_t)(pad[b] + hi[b]) * 4;
            const float* ch = cur + (size_t)(pad[b] + hi[b]) * 4;
            const float* pl = P + (size_t)(pad[b] + li[b]) * 4;
            const float* cl = cur + (size_t)(pad[b] + li[b]) * 4;
            for (int j = 0; j < n; ++j) {
                const V4 top = v4::add(ld(ph + (size_t)j * 4), mul(vhf, ld(ch + (size_t)j * 4)));
                const V4 bot = v4::add(ld(pl + (size_t)j * 4), mul(vlf, ld(cl + (size_t)j * 4)));
                st(nxt + (size_t)j * 4, mul(sub(top, bot), vinv));
            }
            std::swap(cur, nxt);
            len = n;
        }
    }
};

// Box filters along lines of slope 'slope' (-1 .. 1: rows per column, going right). The frame is read along the line (linear interpolation between the two rows
// it falls between) into a sheared copy where the line is a row, the boxes are running sums along that row, and the result is read back between the two sheared rows the
// pixel falls between. Done in strips of columns (a strip needs only its own shear). Box lengths are in columns; edges clamp like a clamped sample.
void majorX(const Buf& in, Buf& out, double slope, const double* wcols, int nbox, float scale, bool accum)
{
    const int W = in.w, H = in.h;
    if (!accum) out.ensure(W, H);
    BoxChain chain;
    for (int i = 0; i < nbox; ++i) chain.add(wcols[i]);
    const int padTot = chain.padTot;
    const V4 vs = set(scale);
    if (std::fabs(slope) < 1e-7) {
        parallelRows(H, (size_t)(W + 2 * padTot) * 30, [&](int y0, int y1) {
            const int L = W + 2 * padTot;
            std::vector<float> a((size_t)L * 4), b((size_t)L * 4), P((size_t)(L + 1) * 4);
            for (int y = y0; y < y1; ++y) {
                const float* row = &in.px[(size_t)y * W * 4];
                float *cur = a.data(), *nxt = b.data();
                for (int j = 0; j < padTot; ++j) {
                    std::memcpy(cur + (size_t)j * 4, row, 16);
                    std::memcpy(cur + (size_t)(padTot + W + j) * 4, row + (size_t)(W - 1) * 4, 16);
                }
                std::memcpy(cur + (size_t)padTot * 4, row, (size_t)W * 16);
                chain.run(cur, nxt, P.data(), L);
                float* o = &out.px[(size_t)y * W * 4];
                for (int x = 0; x < W; ++x) {
                    const V4 v = mul(ld(cur + (size_t)x * 4), vs);
                    st(o + (size_t)x * 4, accum ? v4::add(ld(o + (size_t)x * 4), v) : v);
                }
            }
        });
        return;
    }
    const int T = std::max(64, std::min(512, (W + 3) / 4));
    for (int xa = 0; xa < W; xa += T) {
        const int Tn = std::min(T, W - xa);
        const int L = Tn + 2 * padTot;
        const double span = slope * (Tn - 1);
        const int Rlo = (int)std::floor(std::min(0.0, -span)) - 1, Rhi = (int)std::ceil((H - 1) + std::max(0.0, -span)) + 1;
        const int nR = Rhi - Rlo + 1;
        std::unique_ptr<float[]> Bu(new float[(size_t)nR * Tn * 4]);
        parallelRows(nR, (size_t)L * 40, [&](int r0, int r1) {
            std::vector<float> a((size_t)L * 4), b((size_t)L * 4), P((size_t)(L + 1) * 4);
            std::vector<int> xcol((size_t)L);
            for (int j = 0; j < L; ++j) xcol[(size_t)j] = std::min(W - 1, std::max(0, xa - padTot + j));
            for (int rr = r0; rr < r1; ++rr) {
                const int R = Rlo + rr;
                float *cur = a.data(), *nxt = b.data();
                float Y = (float)(R + slope * (double)(-padTot));
                const float dY = (float)slope, maxY = (float)(H - 1);
                for (int j = 0; j < L; ++j, Y += dY) {
                    const float yc = clampf(Y, 0.f, maxY);
                    const int y0 = (int)yc, y1 = std::min(H - 1, y0 + 1);
                    const int xc = xcol[(size_t)j];
                    st(cur + (size_t)j * 4, lerp(pix(in.px.data(), W, xc, y0), pix(in.px.data(), W, xc, y1), yc - (float)y0));
                }
                chain.run(cur, nxt, P.data(), L);
                std::memcpy(&Bu[(size_t)rr * Tn * 4], cur, (size_t)Tn * 16);
            }
        });
        const int BW = 16;
        parallelRows(H, (size_t)Tn * 12, [&](int y0, int y1) {
            for (int xb = 0; xb < Tn; xb += BW) {
                const int xe = std::min(Tn, xb + BW);
                for (int y = y0; y < y1; ++y) {
                    float* o = &out.px[((size_t)y * W + (size_t)xa) * 4];
                    for (int xo = xb; xo < xe; ++xo) {
                        const float Rf = (float)(y - slope * xo) + 4096.f;
                        const int Ri = (int)Rf;
                        const float f = Rf - (float)Ri;
                        const int Ra = Ri - 4096 - Rlo;
                        const float* p0 = &Bu[((size_t)Ra * Tn + (size_t)xo) * 4];
                        const V4 v = mul(lerp(ld(p0), ld(p0 + (size_t)Tn * 4), f), vs);
                        st(o + (size_t)xo * 4, accum ? v4::add(ld(o + (size_t)xo * 4), v) : v);
                    }
                }
            }
        });
    }
}

// ------------------------------------------------------------------ polar coordinates about a centre
struct PolarGeom {
    int W = 0, H = 0, nT = 0, rN = 0;            // angles, radii 0 .. rN - 1
    double px0 = 0, py0 = 0;
    std::vector<float> cs, sn;
    std::vector<int> rEnd;
};

PolarGeom polarGeom(int W, int H, double px0, double py0, double density)
{
    PolarGeom g;
    const double lim = 8.0 * std::max(W, H);
    px0 = std::max(-lim, std::min(lim, px0));
    py0 = std::max(-lim, std::min(lim, py0));
    g.W = W; g.H = H; g.px0 = px0; g.py0 = py0;
    double far = 0;
    const double xs[2] = {0.0, (double)W}, ys[2] = {0.0, (double)H};
    for (double x : xs)
        for (double y : ys) far = std::max(far, std::hypot(x - px0, y - py0));
    g.rN = (int)std::ceil(far) + 3;
    double nt = 2 * kPi * g.rN * density;
    const double cap = 12e6 / g.rN;
    nt = std::min(nt, cap);
    g.nT = std::max(64, (int)std::ceil(nt));
    g.cs.resize((size_t)g.nT);
    g.sn.resize((size_t)g.nT);
    g.rEnd.assign((size_t)g.nT, g.rN);
    const bool inside = px0 >= 0 && px0 <= W && py0 >= 0 && py0 <= H;
    for (int i = 0; i < g.nT; ++i) {
        const double a = 2 * kPi * i / g.nT;
        const double c = std::cos(a), s = std::sin(a);
        g.cs[(size_t)i] = (float)c;
        g.sn[(size_t)i] = (float)s;
        if (inside) {
            double t = 1e18;
            if (c > 1e-9) t = std::min(t, (W + 2 - px0) / c); else if (c < -1e-9) t = std::min(t, (-2 - px0) / c);
            if (s > 1e-9) t = std::min(t, (H + 2 - py0) / s); else if (s < -1e-9) t = std::min(t, (-2 - py0) / s);
            g.rEnd[(size_t)i] = (int)std::min((double)g.rN, std::ceil(t) + 3);
        }
    }
    return g;
}

// The frame at radius r = r0 .. r0 + n - 1 along ray ti (clamped like a clamped sample), through fx, into q (4 floats each).
template <class Fx>
void sampleRay(const Buf& src, const PolarGeom& g, int ti, int r0, int n, float* q, const Fx& fx)
{
    const float cx = (float)g.px0 - 0.5f, cy = (float)g.py0 - 0.5f, c = g.cs[(size_t)ti], s = g.sn[(size_t)ti];
    const float mx = (float)(g.W - 1), my = (float)(g.H - 1);
    float x = cx + (float)r0 * c, y = cy + (float)r0 * s;
    for (int i = 0; i < n; ++i, x += c, y += s) {
        const float xc = clampf(x, 0.f, mx), yc = clampf(y, 0.f, my);
        st(q + (size_t)i * 4, fx(bilinear(src.px.data(), g.W, g.H, xc, yc)));
    }
}

// atan2 to about 1e-5 radians
inline float fastAtan2(float y, float x)
{
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float mx = std::max(ax, ay), mn = std::min(ax, ay);
    if (mx <= 0.f) return 0.f;
    const float a = mn / mx, s = a * a;
    float r = a * (0.99986600f + s * (-0.33029950f + s * (0.18014100f + s * (-0.08513300f + s * 0.02083510f))));
    if (ay > ax) r = 1.57079633f - r;
    if (x < 0.f) r = 3.14159265f - r;
    return y < 0.f ? -r : r;
}

// Runs 'op' on every ray: it gets q (the frame, through fx, along the ray at radii -margin .. count - 1 + margin, 4 floats each, radius r at q + (margin + r) * 4)
// and writes the result for radii 0 .. count - 1 to res (4 floats each). Results are then read back for every pixel (bilinear between rays and radii) into 'out'.
template <class Fx, class Op>
void polarRun(const Buf& src, Buf& out, double px0, double py0, double density, int margin, const Fx& fx, const Op& op)
{
    const int W = src.w, H = src.h;
    out.ensure(W, H);
    const PolarGeom g = polarGeom(W, H, px0, py0, density);
    px0 = g.px0;
    py0 = g.py0;
    std::unique_ptr<float[]> res(new float[(size_t)g.nT * g.rN * 4]);
    parallelRows(g.nT, (size_t)g.rN * 40, [&](int t0, int t1) {
        std::vector<float> q((size_t)(g.rN + 2 * margin) * 4);
        for (int t = t0; t < t1; ++t) {
            const int count = g.rEnd[(size_t)t];
            sampleRay(src, g, t, -margin, count + 2 * margin, q.data(), fx);
            op(q.data(), margin, count, res.get() + (size_t)t * g.rN * 4);
        }
    });
    const float tScale = (float)g.nT / 6.28318531f;
    parallelRows(H, (size_t)W * 30, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* o = &out.px[(size_t)y * W * 4];
            const float dy = (float)(y + 0.5 - py0);
            for (int x = 0; x < W; ++x, o += 4) {
                const float dx = (float)(x + 0.5 - px0);
                const float r = std::min((float)(g.rN - 2), std::sqrt(dx * dx + dy * dy));
                float a = fastAtan2(dy, dx);
                if (a < 0.f) a += 6.28318531f;
                const float tf = a * tScale;
                int t0i = (int)tf;
                if (t0i >= g.nT) t0i = g.nT - 1;
                const float ft = tf - (float)t0i;
                const int t1i = t0i + 1 >= g.nT ? 0 : t0i + 1;
                const int ri = (int)r;
                const float fr = r - (float)ri;
                const float* a0 = res.get() + ((size_t)t0i * g.rN + (size_t)ri) * 4;
                const float* a1 = res.get() + ((size_t)t1i * g.rN + (size_t)ri) * 4;
                const V4 s0 = lerp(ld(a0), ld(a0 + 4), fr), s1 = lerp(ld(a1), ld(a1 + 4), fr);
                st(o, lerp(s0, s1, ft));
            }
        }
    });
}

// Box average of the (4-float) samples x[0 .. n) over the window [lo, hi] given in cell coordinates (sample j occupies
// [j, j + 1)), by the running sums p (n + 1 entries).
inline V4 windowAverage(const float* x, const float* p, int n, double lo, double hi)
{
    lo = std::max(0.0, std::min((double)n, lo));
    hi = std::max(0.0, std::min((double)n, hi));
    const int li = std::min(n - 1, (int)lo), hi_i = std::min(n - 1, (int)hi);
    const float lf = (float)(lo - li), hf = (float)(hi - hi_i);
    const V4 top = add(ld(p + (size_t)hi_i * 4), mul(set(hf), ld(x + (size_t)hi_i * 4)));
    const V4 bot = add(ld(p + (size_t)li * 4), mul(set(lf), ld(x + (size_t)li * 4)));
    const float w = (float)(hi - lo);
    return mul(sub(top, bot), set(w > 1e-6f ? 1.f / w : 0.f));
}

}  // namespace

void lineBoxBlur(const Buf& in, Buf& out, double ux, double uy, const double* lens, int nbox, float scale, bool add)
{
    if (in.w <= 0 || in.h <= 0 || in.px.size() < (size_t)in.w * in.h * 4) return;
    double capped[4];
    for (int i = 0; i < nbox && i < 4; ++i) capped[i] = std::max(0.0, std::min(lens[i], 3.0 * std::max(in.w, in.h)));
    lens = capped;
    const double n = std::hypot(ux, uy);
    if (n < 1e-9) { ux = 1; uy = 0; } else { ux /= n; uy /= n; }
    if (ux < 0) { ux = -ux; uy = -uy; }
    if (std::fabs(uy) > ux) {
        Buf tin, tout;
        transposeInto(in, tin, false);
        double cols[4];
        for (int i = 0; i < nbox && i < 4; ++i) cols[i] = lens[i] * std::fabs(uy);
        majorX(tin, tout, ux / uy, cols, std::min(nbox, 4), scale, false);
        transposeInto(tout, out, add);
        return;
    }
    double cols[4];
    for (int i = 0; i < nbox && i < 4; ++i) cols[i] = lens[i] * ux;
    majorX(in, out, uy / ux, cols, std::min(nbox, 4), scale, add);
}

void radialBlurFast(const Buf& src, Buf& out, double px0, double py0, int type, double strength, int minWH)
{
    const int W = src.w, H = src.h;
    const double len0 = strength * 0.25;
    const double fixedH = strength * 0.06 * minWH;
    const PolarGeom probe = polarGeom(W, H, px0, py0, 1.0);
    px0 = probe.px0;
    py0 = probe.py0;
    const double hMax = type == 0 ? len0 * probe.rN : fixedH;
    double boxW = 0;
    if (type == 2) {
        double s0 = 0, s2 = 0;
        for (int i = -500; i <= 500; ++i) {
            const double u = i / 500.0, wgt = std::exp(-2 * u * u);
            s0 += wgt; s2 += wgt * u * u;
        }
        boxW = std::sqrt(12.0 * (s2 / s0) * fixedH * fixedH / 3.0);
    }
    const int margin = (int)std::ceil(type == 2 ? 2.0 * boxW + fixedH : hMax) + 12;
    const auto same = [](V4 v) { return v; };
    polarRun(src, out, px0, py0, 1.0, margin, same, [&](const float* q, int m, int rN, float* res) {
        const int n = rN + 2 * m;
        // scratch kept from ray to ray: only grown, never cleared
        thread_local std::vector<float> a, p, b;
        if (a.size() < (size_t)n * 4) { a.resize((size_t)n * 4); b.resize((size_t)n * 4); p.resize((size_t)(n + 1) * 4); }
        std::memcpy(a.data(), q, (size_t)n * 16);
        auto prefix = [&](const std::vector<float>& x) {
            V4 run = zero();
            st(&p[0], run);
            for (int j = 0; j < n; ++j) { run = add(run, ld(&x[(size_t)j * 4])); st(&p[(size_t)(j + 1) * 4], run); }
        };
        if (type == 2) {
            int lo = 0, hi = n;
            for (int pass = 0; pass < 3; ++pass) {
                prefix(a);
                const int sh = (int)std::ceil(boxW / 2) + 2;
                for (int j = lo + sh; j < hi - sh; ++j) st(&b[(size_t)j * 4], windowAverage(a.data(), p.data(), n, j + 0.5 - boxW / 2, j + 0.5 + boxW / 2));
                lo += sh; hi -= sh;
                a.swap(b);
            }
            for (int r = 0; r < rN; ++r) std::memcpy(res + (size_t)r * 4, &a[(size_t)(m + r) * 4], 16);
        } else if (type == 1 || type > 2) {
            prefix(a);
            if (fixedH < 0.01) { for (int r = 0; r < rN; ++r) std::memcpy(res + (size_t)r * 4, &a[(size_t)(m + r) * 4], 16); return; }
            const double loOff = 0.5 - fixedH, hiOff = 0.5 + fixedH;
            const int li = (int)std::floor(loOff), hi = (int)std::floor(hiOff);
            const V4 vlf = set((float)(loOff - li)), vhf = set((float)(hiOff - hi)), vinv = set((float)(1.0 / (2 * fixedH)));
            for (int r = 0; r < rN; ++r) {
                const int j = m + r;
                const V4 top = add(ld(&p[(size_t)(j + hi) * 4]), mul(vhf, ld(&a[(size_t)(j + hi) * 4])));
                const V4 bot = add(ld(&p[(size_t)(j + li) * 4]), mul(vlf, ld(&a[(size_t)(j + li) * 4])));
                st(res + (size_t)r * 4, mul(sub(top, bot), vinv));
            }
        } else {
            prefix(a);
            for (int r = 0; r < rN; ++r) {
                const double h = len0 * r;
                if (h < 0.01) { std::memcpy(res + (size_t)r * 4, &a[(size_t)(m + r) * 4], 16); continue; }
                st(res + (size_t)r * 4, windowAverage(a.data(), p.data(), n, m + r + 0.5 - h, m + r + 0.5 + h));
            }
        }
    });
}

void marchTowardsCentre(const Buf& g, Buf& out, double px0, double py0, double span, int taps, const std::function<void(const float* in, float* out)>& fxPixel)
{
    const double fmin = 1.0 - span;
    const auto fx = [&](V4 v) {
        float in[4], o[4];
        st(in, v);
        if (fxPixel) fxPixel(in, o); else { o[0] = in[0]; o[1] = in[1]; o[2] = in[2]; o[3] = 0.f; }
        return ld(o);
    };
    polarRun(g, out, px0, py0, 0.35, 4, fx, [&](const float* q, int m, int rN, float* res) {
        const int n = rN + 2 * m;
        thread_local std::vector<double> s0, s1;
        if (s0.size() < (size_t)(n + 1) * 3) { s0.resize((size_t)(n + 1) * 3); s1.resize((size_t)(n + 1) * 3); }
        s0[0] = s0[1] = s0[2] = s1[0] = s1[1] = s1[2] = 0;
        for (int j = 0; j < n; ++j) {
            const double r = (double)(j - m);
            for (int k = 0; k < 3; ++k) {
                const double qv = q[(size_t)j * 4 + (size_t)k];
                s0[(size_t)(j + 1) * 3 + (size_t)k] = s0[(size_t)j * 3 + (size_t)k] + qv;
                s1[(size_t)(j + 1) * 3 + (size_t)k] = s1[(size_t)j * 3 + (size_t)k] + r * qv;
            }
        }
        for (int r = 0; r < rN; ++r) {
            float* o = res + (size_t)r * 4;
            o[3] = 0.f;
            if (r <= 4) {
                for (int k = 0; k < 3; ++k) {
                    double acc = 0;
                    for (int t = 1; t <= taps; ++t) {
                        const double pos = m + r * (1.0 - (double)t / taps * span);
                        const int j = std::max(0, std::min(n - 2, (int)pos));
                        const double f = pos - j;
                        const double v = q[(size_t)j * 4 + (size_t)k] * (1 - f) + q[(size_t)(j + 1) * 4 + (size_t)k] * f;
                        acc += v * (1.0 - (double)t / taps);
                    }
                    o[k] = (float)(acc / taps);
                }
                continue;
            }
            const double a = m + r * fmin + 0.5;
            const int ja = (int)a, jb = m + r;
            const double fa = a - ja, fb = 0.5;
            const double den = span * span * (double)r * r;
            for (int k = 0; k < 3; ++k) {
                const size_t ka = (size_t)ja * 3 + (size_t)k, kb = (size_t)jb * 3 + (size_t)k;
                const double qa = q[(size_t)ja * 4 + (size_t)k], qb = q[(size_t)jb * 4 + (size_t)k];
                const double i0 = (s0[kb] + fb * qb) - (s0[ka] + fa * qa);
                const double i1 = (s1[kb] + fb * (jb - m) * qb) - (s1[ka] + fa * (ja - m) * qa);
                // less half the weight-1 tap at t = 0, which the sum over t = 1 .. taps does not have
                o[k] = (float)((i1 - fmin * r * i0) / den - qb / (2.0 * taps));
            }
        }
    });
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
