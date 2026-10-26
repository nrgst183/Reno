// VEGAS Pro's stock transitions as built-in stand-ins (see builtins.h), used when VEGAS's own plug-ins are missing.
// Names, labels, defaults and presets come from builtins_vfxdata.cpp; 3D and page transitions are drawn flat.
// Ctx::src is the outgoing frame, Ctx::src2 the incoming one, progress runs 0..1.
#include "builtins_vfxdata.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {
namespace {

struct Px { float v[4]; };

Px fromAt(const Ctx& c, double x, double y) { Px p; c.src.sample(x, y, p.v); return p; }
Px toAt(const Ctx& c, double x, double y) { Px p; c.src2.sample(x, y, p.v); return p; }
Px fromClamped(const Ctx& c, double x, double y) { Px p; c.src.sampleClamped(x, y, p.v); return p; }
Px toClamped(const Ctx& c, double x, double y) { Px p; c.src2.sampleClamped(x, y, p.v); return p; }

void store(Ctx& c, int x, int y, const Px& p) { std::memcpy(c.out.at(x, y), p.v, sizeof(float) * 4); }

// Average of the box [x0, x1) x [y0, y1) in pixels (partial pixels weighted by coverage, edges extended); VEGAS shrinks with a box filter.
Px boxAt(const Buf& b, double x0, double x1, double y0, double y1)
{
    Px r = {{0, 0, 0, 0}};
    if (b.w <= 0 || b.h <= 0 || !(x1 > x0) || !(y1 > y0)) return r;
    double acc[4] = {0, 0, 0, 0}, tot = 0;
    const int ix0 = (int)std::floor(x0), ix1 = (int)std::ceil(x1), iy0 = (int)std::floor(y0), iy1 = (int)std::ceil(y1);
    if ((double)(ix1 - ix0) * (iy1 - iy0) > 1e6) return r;
    for (int y = iy0; y < iy1; ++y) {
        const double wy = std::min(y + 1.0, y1) - std::max((double)y, y0);
        if (wy <= 0) continue;
        const int yy = std::min(b.h - 1, std::max(0, y));
        for (int x = ix0; x < ix1; ++x) {
            const double wx = std::min(x + 1.0, x1) - std::max((double)x, x0);
            if (wx <= 0) continue;
            const float* q = b.at(std::min(b.w - 1, std::max(0, x)), yy);
            const double wgt = wx * wy;
            for (int k = 0; k < 4; ++k) acc[k] += q[k] * wgt;
            tot += wgt;
        }
    }
    if (tot > 0) for (int k = 0; k < 4; ++k) r.v[k] = (float)(acc[k] / tot);
    return r;
}

Px mixPx(const Px& a, const Px& b, double t)
{
    Px r;
    for (int k = 0; k < 4; ++k) r.v[k] = (float)(a.v[k] + (b.v[k] - a.v[k]) * t);
    return r;
}

Px colorPx(const Color& col, double a = 1.0)
{
    Px p;
    put(p.v, Color{col.r, col.g, col.b, a});
    return p;
}

// Wipe driven by s (0..1 per pixel): the incoming frame shows where s < edge, with a soft edge (feather) and border, as fractions of the run.
template <class S>
void wipe(Ctx& c, S s, double feather, double border, const Color& bc)
{
    const int W = c.out.w, H = c.out.h;
    const double f = std::max(1e-4, feather), b = std::max(0.0, border);
    const double edge = c.progress * (1.0 + f + b);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double sv = s((x + 0.5) / W, (y + 0.5) / H);
            const double toCover = smooth01((edge - b - sv) / f + 0.0 * 0);
            const double anyCover = b > 0 ? smooth01((edge - sv) / f) : toCover;
            Px o = fromAt(c, x + 0.5, y + 0.5);
            if (b > 0) o = mixPx(o, colorPx(bc), anyCover);
            o = mixPx(o, toAt(c, x + 0.5, y + 0.5), toCover);
            store(c, x, y, o);
        }
    });
}

double borderOf(const Ctx& c) { return clampd(c.d("BorderSize"), 0, 1) * 0.25; }

// ------------------------------------------------------------------ dissolves
void dissolve(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const int type = c.i("Type");
    const double p = c.progress;
    const Color fc = c.color("FadeThroughColor");
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const Px a = fromAt(c, x + 0.5, y + 0.5), b = toAt(c, x + 0.5, y + 0.5);
            Px o;
            switch (type) {
            case 0:
                for (int k = 0; k < 4; ++k) o.v[k] = std::min(1.f, a.v[k] * (float)std::min(1.0, 2 * (1 - p)) + b.v[k] * (float)std::min(1.0, 2 * p));
                break;
            case 1:
                for (int k = 0; k < 4; ++k) {
                    const float va = a.v[k] * (float)std::min(1.0, 2 * (1 - p)), vb = b.v[k] * (float)std::min(1.0, 2 * p);
                    o.v[k] = k == 3 ? std::min(1.f, va + vb) : std::fabs(va - vb);
                }
                break;
            case 2:
                for (int k = 0; k < 4; ++k) {
                    const float va = a.v[k] * (float)(1 - p), vb = b.v[k] * (float)p;
                    o.v[k] = k == 3 ? va + vb : std::fabs(va - vb);
                }
                break;
            case 3: case 4: {
                // Colour bleed: each channel keeps what is above a level that rises (outgoing) and falls (incoming) with progress.
                // Colour morph: each channel of the outgoing frame moves towards the incoming one by at most p.
                // Speeds scale the progress per channel; works on straight (not premultiplied) values, multiplied by alpha at the end.
                const char* nm[4] = {"ColorBleedSpeedRed", "ColorBleedSpeedGreen", "ColorBleedSpeedBlue", "ColorBleedSpeedAlpha"};
                const char* nmM[4] = {"ColorMorphSpeedRed", "ColorMorphSpeedGreen", "ColorMorphSpeedBlue", "ColorMorphSpeedAlpha"};
                float sa[4], sb[4];
                for (int k = 0; k < 4; ++k) {
                    sa[k] = a.v[k]; sb[k] = b.v[k];
                    if (k < 3) { if (a.v[3] > 1e-6f) sa[k] /= a.v[3]; if (b.v[3] > 1e-6f) sb[k] /= b.v[3]; }
                }
                float res[4];
                for (int k = 3; k >= 0; --k) {
                    const double m = 1.0 + clampd(c.d(type == 3 ? nm[k] : nmM[k]), 0, 1) * 0.996;
                    if (type == 3) {
                        const double ta = std::min(0.998, p * m), tb = std::min(0.998, (1 - p) * m);
                        res[k] = (float)(std::max(0.0, sa[k] - ta) + std::max(0.0, sb[k] - tb));
                    } else {
                        const double lim = std::min(1.0, p * m);
                        res[k] = sa[k] + (float)clampd(sb[k] - sa[k], -lim, lim);
                    }
                    res[k] = std::min(std::max(res[k], 0.f), 1.f);
                }
                for (int k = 0; k < 3; ++k) o.v[k] = res[k] * res[3];
                o.v[3] = res[3];
                break;
            }
            case 5:
            case 6: {
                const Px& t = type == 5 ? a : b;
                const double avg = (t.v[0] + t.v[1] + t.v[2]) * 255.0 / 3.0;
                o = avg <= std::ceil(p * 255.0 - 1e-6) + 1e-3 ? b : a;
                break;
            }
            case 7: case 8: {
                Px mid;
                if (type == 7) { const float l = luma(a.v[0], a.v[1], a.v[2]) * (float)(1 - p) + luma(b.v[0], b.v[1], b.v[2]) * (float)p; mid.v[0] = mid.v[1] = mid.v[2] = l; mid.v[3] = 1; }
                else mid = colorPx(fc);
                o = p < 0.5 ? mixPx(a, mid, p * 2) : mixPx(mid, b, p * 2 - 1);
                break;
            }
            default:                                        // R, then G, then B
                for (int k = 0; k < 4; ++k) {
                    const double t = k == 0 ? clampd(p * 3, 0, 1) : k == 1 ? clampd(p * 3 - 1, 0, 1) : k == 2 ? clampd(p * 3 - 2, 0, 1) : p;
                    o.v[k] = a.v[k] + (b.v[k] - a.v[k]) * (float)t;
                }
            }
            for (int k = 0; k < 4; ++k) o.v[k] = std::min(std::max(o.v[k], 0.f), 1.f);
            if (o.v[0] > o.v[3]) o.v[0] = o.v[3];
            if (o.v[1] > o.v[3]) o.v[1] = o.v[3];
            if (o.v[2] > o.v[3]) o.v[2] = o.v[3];
            store(c, x, y, o);
        }
    });
}

// Cross Effect: cross-fade with zoom, pixelate or blur on the outgoing and/or incoming frame, done on 8-bit pictures, bottom row first, as VEGAS does.
// The fade is the integer mix out = a + (b - a) * t8 / 256 (truncated), t8 = fade 0..255; FadeRange is the part of progress it runs over.
// Pixelate: nx x ny cells of integer-truncated box averages; the cell count follows 1 - (0.75 + Max / 4) * progress^0.25.
// Zoom and blur are Reno's own (VEGAS uses its image scaler and Gaussian).
inline int ntTrunc8(float v) { return (int)v; }
void crossEffect(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const int type = c.i("EffectType"), apply = c.i("ApplyTo");
    const float pf = std::min(1.0f, std::max(0.0f, (float)c.progress));
    const double fr = clampd(c.d("FadeRange"), 0, 1);
    float tf;
    if (c.d("FadeRange") < 0.001) tf = pf > 0.5f ? 1.0f : 0.0f;
    else {
        const float f = (float)c.d("FadeRange");
        tf = (pf - (1.0f - f) * 0.5f) / f;
        tf = tf < 0.0f ? 0.0f : tf > 1.0f ? 1.0f : tf;
    }
    (void)fr;
    const int t8 = ntTrunc8(tf * 255.0f + 0.5f);
    const bool fxA = apply != 1, fxB = apply != 0;
    const double tri = 1.0 - std::fabs(2.0 * c.progress - 1.0);

    // the 8-bit pictures, bottom row first
    const size_t n = (size_t)W * H * 4;
    std::vector<uint8_t> A(n), B(n), O(n);
    auto toBytes = [&](const Buf& s, std::vector<uint8_t>& d) {
        for (int y = 0; y < H; ++y) {
            const float* row = s.at(0, y);
            uint8_t* dst = &d[(size_t)(H - 1 - y) * W * 4];
            for (int i = 0; i < W * 4; ++i) {
                const float v = row[i] * 255.0f + 0.5f;
                dst[i] = (uint8_t)(v < 0.f ? 0 : v > 255.f ? 255 : (int)v);
            }
        }
    };
    toBytes(c.src, A);
    toBytes(c.src2, B);
    auto mix8 = [&](int a, int b) { return (uint8_t)(a + ((b - a) * t8) / 256); };

    if (type == 2) {
        // the number of cells and their sizes (floats, as VEGAS)
        float pw;
        if (apply == 0) pw = std::pow(pf, 0.25f);
        else if (apply == 1) pw = std::pow(1.0f - pf, 0.25f);
        else {
            float q = pf;
            if (!(0.5f >= q)) q = 1.0f - q;
            q += q;
            pw = std::pow(q, 0.25f);
        }
        const float mxv = (float)c.d("EffectSettingsMaxX"), myv = (float)c.d("EffectSettingsMaxY");
        const float asx = 1.0f, asy = 1.0f;
        const float fx = (1.0f - (mxv * 0.25f + 0.75f) * pw) * asx;
        const float fy = (1.0f - (myv * 0.25f + 0.75f) * pw) * asy;
        const int nx = fx > 0.999f ? W : ntTrunc8(fx * (float)W * 0.5f + 1.0f);
        const int ny = fy > 0.999f ? H : ntTrunc8(fy * (float)H * 0.5f + 1.0f);
        const float sx = (float)W / (float)nx, sy = (float)H / (float)ny;
        for (int j = 0; j < ny; ++j) {
            const float j0 = (float)j * sy, j1 = (float)(j + 1) * sy;
            const int y0 = ntTrunc8(j0);
            const int hh = j != ny - 1 ? ntTrunc8(j1 - j0) : H - y0;
            const int rows = y0 + 1 + hh >= H ? hh : hh + 1;
            for (int i = 0; i < nx; ++i) {
                const float i0 = (float)i * sx, i1 = (float)(i + 1) * sx;
                const int x0 = ntTrunc8(i0);
                const int ww = i != nx - 1 ? ntTrunc8(i1 - i0) : W - x0;
                const int cols = x0 + 1 + ww >= W ? ww : ww + 1;
                auto avg = [&](const std::vector<uint8_t>& s, int ch) {
                    long long sum = 0;
                    for (int y = 0; y < hh; ++y)
                        for (int x = 0; x < ww; ++x) sum += s[((size_t)(y0 + y) * W + x0 + x) * 4 + ch];
                    return (int)((sum / ww) / hh);
                };
                int cellA[4] = {0, 0, 0, 0}, cellB[4] = {0, 0, 0, 0};
                if (hh > 0 && ww > 0)
                    for (int ch = 0; ch < 4; ++ch) {
                        if (fxA) cellA[ch] = avg(A, ch);
                        if (fxB) cellB[ch] = avg(B, ch);
                    }
                for (int y = y0; y < y0 + rows && y < H; ++y)
                    for (int x = x0; x < x0 + cols && x < W; ++x) {
                        const size_t k = ((size_t)y * W + x) * 4;
                        for (int ch = 0; ch < 4; ++ch) {
                            const int a = fxA ? cellA[ch] : A[k + ch], b = fxB ? cellB[ch] : B[k + ch];
                            O[k + ch] = mix8(a, b);
                        }
                    }
            }
        }
    } else if (type == 1 || type == 3) {
        // Reno's own zoom and blur pictures, then VEGAS's whole-number mix
        Buf blurA, blurB;
        if (type == 3) {
            const double maxBlur = clampd(c.d("EffectSettingsMaxBlur"), 0, 1);
            const double sg = maxBlur * 12.45 * c.out.w / 320.0 * tri * (apply == 2 ? 1.964 : 1.0);
            blurA = c.src; blurB = c.src2;
            if (fxA && sg > 0.01) gaussianBlur(blurA, sg, sg);
            if (fxB && sg > 0.01) gaussianBlur(blurB, sg, sg);
        }
        double sax = 0.5, say = 0.5, dbx = 0.5, dby = 0.5;
        c.xy("EffectSettingsSource", sax, say);
        c.xy("EffectSettingsDestination", dbx, dby);
        const double maxZoom = std::max(1.0, c.d("EffectSettingsMaxZoom"));
        const double za = fxA ? std::pow(maxZoom, c.progress) : 1.0, zb = fxB ? std::pow(maxZoom, 1.0 - c.progress) : 1.0;
        forRows(H, W, [&](int y) {
            for (int x = 0; x < W; ++x) {
                Px a, b;
                if (type == 1) {                                // zoom
                    const double pya = (1 - say) * H - (za > 3 ? 0.5 : 0.0), pyb = (1 - dby) * H - (zb > 3 ? 0.5 : 0.0);
                    const double ax = sax * W + (x + 0.5 - sax * W) / za, ay = pya + (y + 0.5 - pya) / za;
                    const double bx = dbx * W + (x + 0.5 - dbx * W) / zb, by = pyb + (y + 0.5 - pyb) / zb;
                    a = fromClamped(c, ax, ay); b = toClamped(c, bx, by);
                } else {
                    blurA.sampleClamped(x + 0.5, y + 0.5, a.v); blurB.sampleClamped(x + 0.5, y + 0.5, b.v);
                }
                uint8_t* o = &O[(size_t)(H - 1 - y) * W * 4 + (size_t)x * 4];
                for (int ch = 0; ch < 4; ++ch) {
                    const float va = a.v[ch] * 255.0f + 0.5f, vb = b.v[ch] * 255.0f + 0.5f;
                    o[ch] = mix8(va < 0.f ? 0 : va > 255.f ? 255 : (int)va, vb < 0.f ? 0 : vb > 255.f ? 255 : (int)vb);
                }
            }
        });
    } else {
        for (size_t k = 0; k < n; ++k) O[k] = mix8(A[k], B[k]);
    }
    for (int y = 0; y < H; ++y) {
        const uint8_t* src = &O[(size_t)(H - 1 - y) * W * 4];
        float* o = c.out.at(0, y);
        for (int i = 0; i < W * 4; ++i) o[i] = (float)(src[i] / 255.0);
    }
}

// ------------------------------------------------------------------ wipes
void clockWipe(Ctx& c)
{
    // The hand sweeps from 12 o'clock; FeatherAngle is the soft edge width in degrees (smoothstep) and the hand travels
    // 360 + feather. VEGAS pivots about a point half a pixel right of and a pixel below the middle; counter-clockwise mirrors it.
    // Hard edges are anti-aliased: the wedge start is offset half a pixel and the hand 0.71 pixel (measured).
    const int W = c.out.w, H = c.out.h;
    const bool ccw = c.i("Direction") == 1;
    const double F = clampd(c.d("FeatherAngle"), 0, 720);
    const double edge = -F * 0.5 + clampd(c.progress, 0, 1) * (360.0 + F);
    const double cx = W * 0.5 + 0.5, cy = H * 0.5 + 1.0, sh0 = 0.5, sh1 = 0.71;
    const double cxs = W * 0.5 + 0.5, cys = H * 0.5 + 1.0;                                // (the soft edge's centre)
    const double er = edge * kPi / 180.0, nex = -std::cos(er), ney = -std::sin(er);
    const double uex = std::sin(er), uey = -std::cos(er);                                 // and its direction
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double dx = ccw ? cx - (x + 0.5) : x + 0.5 - cx, dy = y + 0.5 - cy;
            double t;
            if (F > 1e-9) {
                const double sx = ccw ? cxs - (x + 0.5) : x + 0.5 - cxs, sy = y + 0.5 - cys;
                double a = std::atan2(sx, -sy) * (180.0 / kPi);
                if (a < 0) a += 360.0;
                t = smooth01((edge - a) / F + 0.5);
            } else if (edge <= 0) t = 0;
            else if (edge >= 360) t = 1;
            else {
                double a = std::atan2(dx, -dy) * (180.0 / kPi);
                if (a < 0) a += 360.0;
                t = a < edge ? 1.0 : 0.0;
                const double d0 = dx + sh0, d1 = dx * nex + dy * ney - sh1;
                const double w0 = std::fabs(d0) < 1.0 ? clampd(-dy - 0.5, 0, 1) : 0.0, w1 = std::fabs(d1) < 1.0 ? clampd(dx * uex + dy * uey - 0.5, 0, 1) : 0.0;
                const double t0 = t + (clampd(0.5 + d0, 0, 1) - t) * w0, t1 = t + (clampd(0.5 + d1, 0, 1) - t) * w1;
                if (w0 > 0 && w1 > 0) t = edge <= 180 ? std::min(t0, t1) : std::max(t0, t1);
                else if (w0 > 0) t = t0;
                else if (w1 > 0) t = t1;
            }
            store(c, x, y, mixPx(fromAt(c, x + 0.5, y + 0.5), toAt(c, x + 0.5, y + 0.5), t));
        }
    });
}

void linearWipe(Ctx& c)
{
    // VEGAS measures the wipe in pixels along the angle: position runs 0..1 across the frame's extent in that direction,
    // the soft edge is Feather/2 wide (smoothstep) and travels from just outside one side to just outside the other.
    const int W = c.out.w, H = c.out.h;
    const double ang = c.d("Angle") * kPi / 180.0, dx = std::cos(ang), dy = std::sin(ang);
    const double span = std::max(1e-9, std::fabs(dx) * W + std::fabs(dy) * H);
    const double w = clampd(c.d("Feather"), 0, 1) * 0.5;
    const double edge = c.progress * (1.0 + w);
    forRows(H, W, [&](int y) {
        const double yd = y + 0.5;
        for (int x = 0; x < W; ++x) {
            const double s = ((x + 0.5 - W * 0.5) * dx + (yd - H * 0.5) * dy) / span + 0.5;
            const double t = w <= 1e-9 ? (s < edge ? 1.0 : 0.0) : smooth01((edge - s) / w);
            store(c, x, y, mixPx(fromAt(c, x + 0.5, y + 0.5), toAt(c, x + 0.5, y + 0.5), t));
        }
    });
}

// Barn Door: two half-picture doors slide in over the outgoing frame ("In"), or its halves slide out ("Out"). The border is a strip
// of Size * half extent with Feather * half extent soft ramps; its centre runs from -1.5 strips to half extent + 1.5 strips.
void barndoor(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const bool alongX = c.i("Orientation") == 0, in = c.i("Direction") == 0;
    const double L = alongX ? W : H, T = L * 0.5, p = clampd(c.progress, 0, 1);
    const double w = clampd(c.d("BorderSize"), 0, 1) * T, bw = w * 0.5, F = clampd(c.d("BorderFeather"), 0, 1) * T;
    const double Wd = std::max(F, 2.0), R = bw + F * 0.5;
    const bool border = w > 0 || F > 0;
    const Px bc = colorPx(c.color("BorderColor"));
    const double cen = in ? -1.5 * w + p * (T + 3 * w) : T + 1.5 * w - p * (T + 3 * w);
    const double e = cen - bw;
    const double one = border ? 1.0 : 0.0, oneC = w > 0 ? 1.0 : 0.0;
    const double oneF = w > 0 && F <= 0 ? 1.0 : 0.0;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double a = (alongX ? x : y) + 0.5;
            const bool far = a > T;
            const double am = far ? L - a : a;
            const bool moving = am < e;
            double sa = a;
            if (moving) sa = clampd(far ? a - (T - e) + oneF : a + (T - e) - oneC, 0.5, L - 0.5);
            const bool fromPic = in ? !moving : moving;
            const double sx = alongX ? sa : x + 0.5, sy = alongX ? y + 0.5 : sa;
            Px o = fromPic ? fromAt(c, sx, sy) : toAt(c, sx, sy);
            if (border) o = mixPx(o, bc, smooth01((R - std::fabs(am - (cen - one))) / Wd + 0.5));
            store(c, x, y, o);
        }
    });
}

// ---- Iris. VEGAS builds a distance map (distance from the nearest iris centre in the shape's metric), divides it by its largest
// value on the frame's four edges and blends by it; progress moves a soft edge and border band over the map. Single-precision
// arithmetic and 8-bit truncating mixes follow VEGAS.
// irisDistance is generated by notes/decode/iris/gen_dist.py from VEGAS's distance function (do not edit by hand).
struct IrisReg { float f; double d; };
float irisDistance(int xi, int yi, float cx, float cy, int shape, float A, float sx, float sy)
{
    IrisReg x0 = {sy, 0}, x1 = {sx, 0}, x2 = {cx, 0}, x3 = {cy, 0}, x4 = {0, 0}, x5 = {0, 0}, x6 = {1.0f, 0};
    float cmpl_ = 0, cmpr_ = 0;
    const float ci = (float)xi, ei = (float)yi;
    if (shape < 0 || shape > 13) return 1.0f;
     x5.f = (float)ci;
     x4.f = (float)ei;
     ;
     ;
     ;
     x5.f = x5.f - x2.f;
     x4.f = x4.f - x3.f;
     x5.f = x5.f * x1.f;
     x4.f = x4.f * x0.f;
     ;
     ;
     ;
     ;
     ;
    switch (shape) {
    case 0: goto L1805d4ebf;
    case 1: goto L1805d4edb;
    case 2: goto L1805d4efd;
    case 3: goto L1805d4f1f;
    case 4: goto L1805d4f52;
    case 5: goto L1805d4f77;
    case 6: goto L1805d4fa4;
    case 7: goto L1805d5060;
    case 8: goto L1805d5120;
    case 9: goto L1805d51bb;
    case 10: goto L1805d528b;
    case 11: goto L1805d5372;
    case 12: goto L1805d5435;
    case 13: goto L1805d551c;
    default: return 1.0f;
    }
    L1805d4ebf:; x4.f = x4.f * x4.f;
     x5.f = x5.f * x5.f;
     x4.f = x4.f + x5.f;
     x0 = x4;
     x0.f = sqrtf(x0.f);
     x6 = x0;
     goto Lend;
    L1805d4edb:; x5.f = x5.f * A;
     x4.f = x4.f * x4.f;
     x5.f = x5.f * x5.f;
     x4.f = x4.f + x5.f;
     x0 = x4;
     x0.f = sqrtf(x0.f);
     x6 = x0;
     goto Lend;
    L1805d4efd:; x4.f = x4.f * A;
     x5.f = x5.f * x5.f;
     x4.f = x4.f * x4.f;
     x4.f = x4.f + x5.f;
     x0 = x4;
     x0.f = sqrtf(x0.f);
     x6 = x0;
     goto Lend;
    L1805d4f1f:; x0.d = (double)x4.f;
     x0.d = std::fabs(x0.d);
     x6.f = (float)x0.d;
     x0.d = (double)x5.f;
     x0.d = std::fabs(x0.d);
     x2.f = (float)x0.d;
     x2.f = x2.f * A;
     cmpl_ = x6.f; cmpr_ = x2.f;
     if (cmpl_ > cmpr_) goto Lend;
     x6 = x2;
     goto Lend;
    L1805d4f52:; x0.d = (double)x4.f;
     x0.d = std::fabs(x0.d);
     x6.f = (float)x0.d;
     x0.d = (double)x5.f;
     x0.d = std::fabs(x0.d);
     x0.f = (float)x0.d;
     x6.f = x6.f + x0.f;
     goto Lend;
    L1805d4f77:; x0.d = (double)x5.f;
     x0.d = std::fabs(x0.d);
     x6.f = (float)x0.d;
     x0.d = (double)x4.f;
     x0.d = std::fabs(x0.d);
     x2.f = (float)x0.d;
     cmpl_ = x6.f; cmpr_ = x2.f;
     if (cmpl_ > cmpr_) goto Lend;
     x6 = x2;
     goto Lend;
    L1805d4fa4:; x1.f = 0.5773502588272095f;
     x2.f = 0.0f; x2.d = 0.0;
     cmpl_ = x2.f; cmpr_ = x5.f;
     x0 = x5;
     x0.f = x0.f * x1.f;
     if (cmpl_ < cmpr_) goto L1805d4ff7;
     cmpl_ = x4.f; cmpr_ = x0.f;
     if (cmpl_ <= cmpr_) goto L1805d4ff7;
     x0.f = 0.6666666865348816f;
     x2.f = x2.f - x5.f;
     x0.f = x0.f - x4.f;
     x2.f = x2.f * -1.0f;
     x0.f = x0.f * -0.5773502588272095f;
     x0.f = x0.f - x2.f;
     x0.d = (double)x0.f;
     x0.d = std::fabs(x0.d);
     x6.f = (float)x0.d;
     goto Lend;
    L1805d4ff7:; cmpl_ = x0.f; cmpr_ = x4.f;
     if (cmpl_ < cmpr_) goto L1805d5031;
     x0.f = -x0.f;
     cmpl_ = x0.f; cmpr_ = x4.f;
     if (cmpl_ <= cmpr_) goto L1805d5031;
     x1.f = -0.3333333432674408f;
     x0.f = -0.5773502588272095f;
     x1.f = x1.f - x4.f;
     x0.f = x0.f - x5.f;
     x1.f = x1.f * 1.154700517654419f;
     x0.f = x0.f * x2.f;
     if (1) goto L1805d5109;
    L1805d5031:; x0.f = -0.3333333432674408f;
     x0.f = x0.f - x4.f;
     x0.f = x0.f * -0.5773502588272095f;
    L1805d5045:; x1.f = x1.f - x5.f;
     x0.f = x0.f - x1.f;
     x0.d = (double)x0.f;
     x0.d = std::fabs(x0.d);
     x6.f = (float)x0.d;
     goto Lend;
    L1805d5060:; x0 = x5;
     x2.f = 0.0f; x2.d = 0.0;
     x0.f = x0.f * 1.7320507764816284f;
     x1 = x0;
     x1.f = -x1.f;
     cmpl_ = x4.f; cmpr_ = x1.f;
     if (cmpl_ <= cmpr_) goto L1805d50b9;
     cmpl_ = x4.f; cmpr_ = x2.f;
     if (cmpl_ < cmpr_) goto L1805d50b9;
     x0.f = 0.6666666865348816f;
     x2.f = x2.f - x4.f;
     x0.f = x0.f - x5.f;
     x2.f = x2.f * -1.0f;
     x0.f = x0.f * 0.5773502588272095f;
     x2.f = x2.f - x0.f;
     x0.d = (double)x2.f;
     x0.d = std::fabs(x0.d);
     x6.f = (float)x0.d;
     goto Lend;
    L1805d50b9:; cmpl_ = x1.f; cmpr_ = x4.f;
     if (cmpl_ < cmpr_) goto L1805d50e9;
     cmpl_ = x4.f; cmpr_ = x0.f;
     if (cmpl_ <= cmpr_) goto L1805d50e9;
     x1.f = 0.5773502588272095f;
     x0.f = -0.3333333432674408f;
     x1.f = x1.f - x4.f;
     x0.f = x0.f - x5.f;
     x1.f = x1.f * x2.f;
     x0.f = x0.f * -1.154700517654419f;
     if (1) goto L1805d5109;
    L1805d50e9:; x0.f = -0.3333333432674408f;
     x1.f = -0.5773502588272095f;
     x0.f = x0.f - x5.f;
     x1.f = x1.f - x4.f;
     x0.f = x0.f * 0.5773502588272095f;
    L1805d5109:; x1.f = x1.f - x0.f;
     x0.d = (double)x1.f;
     x0.d = std::fabs(x0.d);
     x6.f = (float)x0.d;
     goto Lend;
    L1805d5120:; x2.f = 0.5773502588272095f;
     x1.f = 0.0f; x1.d = 0.0;
     cmpl_ = x5.f; cmpr_ = x1.f;
     x0 = x5;
     x0.f = x0.f * x2.f;
     if (cmpl_ < cmpr_) goto L1805d5151;
     cmpl_ = x0.f; cmpr_ = x4.f;
     if (cmpl_ <= cmpr_) goto L1805d5151;
     x0.f = -0.6666666865348816f;
     x0.f = x0.f - x4.f;
     x0.f = x0.f * x2.f;
     if (1) goto L1805d5045;
    L1805d5151:; cmpl_ = x1.f; cmpr_ = x5.f;
     if (cmpl_ <= cmpr_) goto L1805d5188;
     x0.f = -x0.f;
     cmpl_ = x0.f; cmpr_ = x4.f;
     if (cmpl_ < cmpr_) goto L1805d5188;
     x1.f = 0.3333333432674408f;
     x0.f = -0.5773502588272095f;
     x1.f = x1.f - x4.f;
     x0.f = x0.f - x5.f;
     x1.f = x1.f * x2.f;
     x0.f = x0.f * -1.0f;
     if (1) goto L1805d5109;
    L1805d5188:; x0.f = 0.3333333432674408f;
     x2.f = x2.f - x5.f;
     x0.f = x0.f - x4.f;
     x2.f = x2.f * x1.f;
     x0.f = x0.f * -1.154700517654419f;
     x0.f = x0.f - x2.f;
     x0.d = (double)x0.f;
     x0.d = std::fabs(x0.d);
     x6.f = (float)x0.d;
     goto Lend;
    L1805d51bb:; x2.f = 0.0f; x2.d = 0.0;
     x0 = x5;
     cmpl_ = x4.f; cmpr_ = x2.f;
     x0.f = x0.f * 1.7320507764816284f;
     if (cmpl_ <= cmpr_) goto L1805d5212;
     cmpl_ = x4.f; cmpr_ = x0.f;
     if (cmpl_ < cmpr_) goto L1805d5212;
     x1.f = 0.5773502588272095f;
     x0.f = 0.3333333432674408f;
     x1.f = x1.f - x4.f;
     x0.f = x0.f - x5.f;
     x1.f = x1.f * -1.0f;
     x0.f = x0.f * -0.5773502588272095f;
     x1.f = x1.f - x0.f;
     x0.d = (double)x1.f;
     x0.d = std::fabs(x0.d);
     x6.f = (float)x0.d;
     goto Lend;
    L1805d5212:; cmpl_ = x2.f; cmpr_ = x4.f;
     if (cmpl_ < cmpr_) goto L1805d5262;
     x0.f = -x0.f;
     cmpl_ = x0.f; cmpr_ = x4.f;
     if (cmpl_ <= cmpr_) goto L1805d5262;
     x1.f = -0.5773502588272095f;
     x0.f = 0.3333333432674408f;
     x1.f = x1.f - x4.f;
     x0.f = x0.f - x5.f;
     x1.f = x1.f * -1.0f;
     x0.f = x0.f * 0.5773502588272095f;
     x1.f = x1.f - x0.f;
     x0.d = (double)x1.f;
     x0.d = std::fabs(x0.d);
     x6.f = (float)x0.d;
     goto Lend;
    L1805d5262:; x1.f = -0.5773502588272095f;
     x0.f = 0.3333333432674408f;
     x1.f = x1.f - x4.f;
     x0.f = x0.f - x5.f;
     x1.f = x1.f * x2.f;
     x0.f = x0.f * 1.154700517654419f;
     if (1) goto L1805d5109;
    L1805d528b:; x1.f = -0.0f;
     x2.f = 0.0f; x2.d = 0.0;
     cmpl_ = x5.f; cmpr_ = x2.f;
     if (cmpl_ < cmpr_) goto L1805d530d;
     x0 = x5;
     x0.f = -x0.f;
     cmpl_ = x0.f; cmpr_ = x4.f;
     if (cmpl_ <= cmpr_) goto L1805d52d1;
     x0.f = -0.4000000059604645f;
     x2.f = x2.f - x5.f;
     x1 = x0;
     x1.f = x1.f - x4.f;
     x2.f = x2.f * x0.f;
     x1.f = x1.f * 0.800000011920929f;
     x1.f = x1.f - x2.f;
     x0.d = (double)x1.f;
     if (1) goto L1805d5601;
    L1805d52d1:; cmpl_ = x5.f; cmpr_ = x2.f;
     if (cmpl_ <= cmpr_) goto L1805d530d;
     x0 = x5;
     x0.f = -x0.f;
     cmpl_ = x4.f; cmpr_ = x0.f;
     if (cmpl_ < cmpr_) goto L1805d530d;
     x0.f = -0.800000011920929f;
     x1.f = 0.800000011920929f;
     x2 = x0;
     x2.f = x2.f - x4.f;
     x1.f = x1.f - x5.f;
     x2.f = x2.f * x0.f;
     x1.f = x1.f * 1.600000023841858f;
     if (1) goto L1805d55b7;
    L1805d530d:; cmpl_ = x2.f; cmpr_ = x5.f;
     if (cmpl_ < cmpr_) goto L1805d5343;
     cmpl_ = x4.f; cmpr_ = x5.f;
     if (cmpl_ <= cmpr_) goto L1805d5343;
     x0.f = 0.800000011920929f;
     x2.f = x2.f - x5.f;
     x0.f = x0.f - x4.f;
     x2.f = x2.f * -1.600000023841858f;
     x0.f = x0.f * -0.800000011920929f;
     x0.f = x0.f - x2.f;
     x0.d = (double)x0.f;
     if (1) goto L1805d55be;
    L1805d5343:; x0.f = -0.800000011920929f;
     x1 = x0;
     x0.f = x0.f - x5.f;
     x1.f = x1.f - x4.f;
     x0.f = x0.f * 0.4000000059604645f;
     x1.f = x1.f * 0.800000011920929f;
     x1.f = x1.f - x0.f;
     x0.d = (double)x1.f;
     if (1) goto L1805d5601;
    L1805d5372:; x1.f = -0.0f;
     x2.f = 0.0f; x2.d = 0.0;
     cmpl_ = x4.f; cmpr_ = x2.f;
     if (cmpl_ < cmpr_) goto L1805d53d7;
     x0 = x5;
     x0.f = -x0.f;
     cmpl_ = x4.f; cmpr_ = x0.f;
     if (cmpl_ <= cmpr_) goto L1805d53a6;
     x0.f = 0.800000011920929f;
     x2.f = x2.f - x4.f;
     x2.f = x2.f * -1.600000023841858f;
     if (1) goto L1805d55ac;
    L1805d53a6:; cmpl_ = x4.f; cmpr_ = x2.f;
     if (cmpl_ <= cmpr_) goto L1805d53d7;
     x0 = x5;
     x0.f = -x0.f;
     cmpl_ = x0.f; cmpr_ = x4.f;
     if (cmpl_ < cmpr_) goto L1805d53d7;
     x2.f = 0.800000011920929f;
     x0.f = -0.800000011920929f;
     x2.f = x2.f - x4.f;
     x2.f = x2.f * 0.4000000059604645f;
     if (1) goto L1805d55ef;
    L1805d53d7:; cmpl_ = x2.f; cmpr_ = x4.f;
     if (cmpl_ < cmpr_) goto L1805d5406;
     cmpl_ = x4.f; cmpr_ = x5.f;
     if (cmpl_ <= cmpr_) goto L1805d5406;
     x0.f = -0.4000000059604645f;
     x2.f = x2.f - x4.f;
     x2.f = x2.f * x0.f;
     x0.f = x0.f - x5.f;
     x0.f = x0.f * -0.800000011920929f;
     x2.f = x2.f - x0.f;
     if (1) goto L1805d55fe;
    L1805d5406:; x0.f = -0.800000011920929f;
     x1 = x0;
     x0.f = x0.f - x5.f;
     x1.f = x1.f - x4.f;
     x0.f = x0.f * 0.800000011920929f;
     x1.f = x1.f * 1.600000023841858f;
     x1.f = x1.f - x0.f;
     x0.d = (double)x1.f;
     if (1) goto L1805d55be;
    L1805d5435:; x2.f = 0.0f; x2.d = 0.0;
     cmpl_ = x5.f; cmpr_ = x2.f;
     if (cmpl_ <= cmpr_) goto L1805d5471;
     cmpl_ = x4.f; cmpr_ = x5.f;
     if (cmpl_ < cmpr_) goto L1805d5471;
     x0.f = 0.800000011920929f;
     x1 = x0;
     x0.f = x0.f - x5.f;
     x1.f = x1.f - x4.f;
     x0.f = x0.f * -0.4000000059604645f;
     x1.f = x1.f * -0.800000011920929f;
     x1.f = x1.f - x0.f;
     x0.d = (double)x1.f;
     if (1) goto L1805d5601;
    L1805d5471:; cmpl_ = x2.f; cmpr_ = x5.f;
     x1.f = -0.0f;
     if (cmpl_ < cmpr_) goto L1805d54f0;
     x0 = x5;
     x0.f = -x0.f;
     cmpl_ = x4.f; cmpr_ = x0.f;
     if (cmpl_ <= cmpr_) goto L1805d54b4;
     x0.f = 0.4000000059604645f;
     x2.f = x2.f - x5.f;
     x1 = x0;
     x1.f = x1.f - x4.f;
     x2.f = x2.f * x0.f;
     x1.f = x1.f * -0.800000011920929f;
     x1.f = x1.f - x2.f;
     x0.d = (double)x1.f;
     if (1) goto L1805d5601;
    L1805d54b4:; cmpl_ = x2.f; cmpr_ = x5.f;
     if (cmpl_ <= cmpr_) goto L1805d54f0;
     x0 = x5;
     x0.f = -x0.f;
     cmpl_ = x0.f; cmpr_ = x4.f;
     if (cmpl_ < cmpr_) goto L1805d54f0;
     x0.f = 0.800000011920929f;
     x1.f = -0.800000011920929f;
     x2 = x0;
     x2.f = x2.f - x4.f;
     x1.f = x1.f - x5.f;
     x2.f = x2.f * x0.f;
     x1.f = x1.f * -1.600000023841858f;
     if (1) goto L1805d55b7;
    L1805d54f0:; x0.f = -0.800000011920929f;
     x2.f = x2.f - x5.f;
     x0.f = x0.f - x4.f;
     x2.f = x2.f * 1.600000023841858f;
     x0.f = x0.f * 0.800000011920929f;
     x0.f = x0.f - x2.f;
     x0.d = (double)x0.f;
     if (1) goto L1805d55be;
    L1805d551c:; x2.f = 0.0f; x2.d = 0.0;
     cmpl_ = x4.f; cmpr_ = x2.f;
     if (cmpl_ < cmpr_) goto L1805d5584;
     cmpl_ = x5.f; cmpr_ = x4.f;
     if (cmpl_ <= cmpr_) goto L1805d554e;
     x0.f = 0.4000000059604645f;
     x2.f = x2.f - x4.f;
     x2.f = x2.f * x0.f;
     x0.f = x0.f - x5.f;
     x0.f = x0.f * 0.800000011920929f;
     x2.f = x2.f - x0.f;
     if (1) goto L1805d55fe;
    L1805d554e:; cmpl_ = x4.f; cmpr_ = x2.f;
     if (cmpl_ <= cmpr_) goto L1805d5584;
     cmpl_ = x4.f; cmpr_ = x5.f;
     if (cmpl_ < cmpr_) goto L1805d5584;
     x0.f = 0.800000011920929f;
     x1 = x0;
     x0.f = x0.f - x5.f;
     x1.f = x1.f - x4.f;
     x0.f = x0.f * -0.800000011920929f;
     x1.f = x1.f * -1.600000023841858f;
     x1.f = x1.f - x0.f;
     x0.d = (double)x1.f;
     if (1) goto L1805d55be;
    L1805d5584:; cmpl_ = x2.f; cmpr_ = x4.f;
     if (cmpl_ < cmpr_) goto L1805d55d3;
     x0 = x5;
     x0.f = -x0.f;
     cmpl_ = x0.f; cmpr_ = x4.f;
     if (cmpl_ <= cmpr_) goto L1805d55d3;
     x0.f = -0.800000011920929f;
     x2.f = x2.f - x4.f;
     x2.f = x2.f * 1.600000023841858f;
    L1805d55ac:; x1 = x0;
     x1.f = x1.f - x5.f;
     x1.f = x1.f * x0.f;
    L1805d55b7:; x2.f = x2.f - x1.f;
     x0.d = (double)x2.f;
    L1805d55be:; x0.d = std::fabs(x0.d);
     x6.f = (float)x0.d;
     x6.f = x6.f * 0.55901700258255f;
     goto Lend;
    L1805d55d3:; x2.f = -0.800000011920929f;
     x0.f = 0.800000011920929f;
     x2.f = x2.f - x4.f;
     x2.f = x2.f * -0.4000000059604645f;
    L1805d55ef:; x1 = x0;
     x1.f = x1.f - x5.f;
     x1.f = x1.f * x0.f;
     x2.f = x2.f - x1.f;
    L1805d55fe:; x0.d = (double)x2.f;
    L1805d5601:; x0.d = std::fabs(x0.d);
     x6.f = (float)x0.d;
     x6.f = x6.f * 1.1180340051651f;
Lend:
    return x6.f;
}

// Centres that can hold a pixel of cell (ix, iy) (-1: left of / below the first centre): VEGAS takes the smallest distance over a
// small window of cells, as a table per shape. Bit (dx + 1) * 4 + (dy + 1) says cell (ix + dx, iy + dy) counts.
const uint16_t kIrisWindow[14] = {
    0x0660, 0x0660, 0x0660, 0x0660, 0x0660, 0x0660, 0x0660, 0x0660, 0x0660, 0x0660,
    0, 0, 0, 0,
};

uint16_t irisWindowMask(int shape)
{
    // (dx, dy) lists measured from VEGAS's code, with dx and dy in -1..2
    static const int win10[][2] = {{-1, 1}, {-1, 2}, {0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}, {2, 1}, {2, 2}};
    static const int win11[][2] = {{0, 0}, {0, 1}, {1, -1}, {1, 0}, {1, 1}, {1, 2}, {2, -1}, {2, 0}, {2, 1}, {2, 2}};
    static const int win12[][2] = {{-1, -1}, {-1, 0}, {0, -1}, {0, 0}, {0, 1}, {1, -1}, {1, 0}, {1, 1}, {2, -1}, {2, 0}};
    static const int win13[][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {-1, 2}, {0, -1}, {0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}};
    const int(*w)[2] = nullptr;
    if (shape == 10) w = win10; else if (shape == 11) w = win11; else if (shape == 12) w = win12; else if (shape == 13) w = win13;
    if (!w) return kIrisWindow[shape < 0 ? 0 : shape > 9 ? 9 : shape];
    uint16_t m = 0;
    for (int i = 0; i < 10; ++i) m |= (uint16_t)(1u << ((w[i][0] + 1) * 4 + (w[i][1] + 1)));
    return m;
}

void irisFx(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W < 2 || H < 2) { forRows(H, W, [&](int y) { for (int x = 0; x < W; ++x) store(c, x, y, toAt(c, x + 0.5, y + 0.5)); }); return; }
    const int shape = std::min(13, std::max(0, c.i("Shape")));
    const bool dirOn = c.i("Direction") != 0;
    double cxn, cyn;
    c.xy("Center", cxn, cyn);
    cxn = clampd(cxn, 0, 1); cyn = clampd(cyn, 0, 1);
    const int nx = std::min(10, std::max(1, c.i("RepeatX"))), ny = std::min(10, std::max(1, c.i("RepeatY")));
    const double feather = clampd(c.d("Feather"), 0, 1), border = clampd(c.d("Border"), 0, 1);
    const int cw = (int)((float)W / (float)nx + 0.5f), ch = (int)((float)H / (float)ny + 0.5f);      // whole-pixel cells
    const float cxf = (float)((double)cw * cxn), cyf = (float)((double)ch * cyn);
    const float sx = 1.f, sy = 1.f;                                         // (the pixel aspect)
    const float A = ((float)ch * sy) / ((float)cw * sx);
    float F = feather < 0.01 ? ((float)nx * 0.01f) * (float)ny : (float)feather;
    F *= 0.05f;
    const float B = (float)border * 0.05f;
    const uint16_t window = irisWindowMask(shape);

    std::vector<float> map((size_t)W * H);                                  // row j counts from the bottom, as VEGAS's rows do
    forRows(H, W, [&](int y) {
        const int j = H - 1 - y;
        float* mrow = &map[(size_t)j * W];
        int iy;
        if (cyf > (float)j) iy = -1;
        else iy = std::min((int)((float)j - cyf) / ch, ny - 1);
        for (int x = 0; x < W; ++x) {
            int ix;
            if (cxf > (float)x) ix = -1;
            else ix = std::min((int)((float)x - cxf) / cw, nx - 1);
            float best = 0;
            bool have = false;
            auto cand = [&](int kx, int ky) {
                const float d = irisDistance(x, j, (float)(kx * cw) + cxf, (float)(ky * ch) + cyf, shape, A, sx, sy);
                best = have ? std::min(d, best) : d;
                have = true;
            };
            for (int dx = -1; dx <= 2; ++dx)
                for (int dy = -1; dy <= 2; ++dy) {
                    if (!(window >> ((dx + 1) * 4 + (dy + 1)) & 1)) continue;
                    const int kx = ix + dx, ky = iy + dy;
                    if (kx < 0 || kx >= nx || ky < 0 || ky >= ny) continue;
                    if (shape == 11 && ix == -1 && iy >= 0 && iy + 2 < ny && kx == 0 && ky == iy + 2) continue;       // (VEGAS leaves this one out)
                    cand(kx, ky);
                }
            if (shape == 11 && ix == -1 && iy >= 0 && iy + 2 < ny && nx == 1) cand(1, iy + 2);                          // (and with one column asks for a cell that is not there)
            if (!have) cand(std::max(ix, 0), std::max(iy, 0));
            mrow[x] = best;
        }
    });
    float mx = 1.0f;
    for (int j = 0; j < H; ++j) {
        const float* mrow = &map[(size_t)j * W];
        if (j == 0 || j == H - 1) { for (int x = 0; x < W; ++x) mx = std::max(mx, mrow[x]); }
        else { mx = std::max(mx, mrow[0]); mx = std::max(mx, mrow[W - 1]); }
    }
    // (VEGAS fills the top row from a pointer that has only moved one row up: its two end pixels land in the second row)
    map[(size_t)W] = map[(size_t)(H - 1) * W];
    map[(size_t)W + W - 1] = map[(size_t)(H - 1) * W + W - 1];
    const float inv = 1.0f / mx;
    float p = (float)clampd(c.progress, 0, 1);
    const bool swapSrc = !dirOn;
    if (swapSrc) p = 1.0f - p;
    const uint8_t bB = (uint8_t)(int)((float)c.color("Color").b * 255.f + 0.5f), bR = (uint8_t)(int)((float)c.color("Color").r * 255.f + 0.5f);
    const uint8_t bG = (uint8_t)(int)(c.color("Color").g * 255.0 + 0.5);
    const uint8_t bc[4] = {bR, bG, bB, 255};
    const bool hardF = F < 1e-6f, noBorder = B < 1e-6f;

    auto byteOf = [](float v) { return (uint8_t)std::lround(std::min(std::max(std::isfinite(v) ? v : 0.f, 0.f), 1.f) * 255.f); };
    forRows(H, W, [&](int y) {
        const int j = H - 1 - y;
        for (int x = 0; x < W; ++x) {
            const float d = inv * map[(size_t)j * W + x];
            const float* pa = c.src.at(x, y);
            const float* pb = c.src2.at(x, y);
            uint8_t a8[4], b8[4];
            for (int k = 0; k < 4; ++k) { a8[k] = byteOf(pa[k]); b8[k] = byteOf(pb[k]); }
            const uint8_t* S0 = swapSrc ? b8 : a8;
            const uint8_t* S1 = swapSrc ? a8 : b8;                          // the one inside
            uint8_t o[4];
            auto copy = [&](const uint8_t* s) { for (int k = 0; k < 4; ++k) o[k] = s[k]; };
            if (noBorder) {
                if (hardF) copy(p > d ? S1 : S0);
                else {
                    const float m = p - F;
                    const double md = (double)m;
                    const float lo = (float)(md >= 0.0 ? std::min((double)p, md) : 0.0);
                    const double hd = (double)(F + p);
                    const float hi = (float)((double)p > hd ? (double)p : std::min(1.0, hd));
                    if (lo > d) copy(S1);
                    else if (d > hi) copy(S0);
                    else {
                        const float q = (1.0f / (hi - lo)) * (d - lo);
                        const float t = 0.0f > q ? 0.0f : std::min(1.0f, q);
                        const float u = 1.0f - t;
                        for (int k = 0; k < 3; ++k) o[k] = (uint8_t)(int)((float)S1[k] * u + (float)S0[k] * t);
                        o[3] = 0.5f <= t ? S0[3] : S1[3];
                    }
                }
            } else {
                const double pd = (double)p;
                const double md = (double)(p - B), hd = (double)(B + p);
                const double lob = md >= 0.0 ? std::min(pd, md) : 0.0;
                const double hib = pd > hd ? pd : std::min(1.0, hd);
                if (hardF) {
                    if ((float)lob > d) copy(S1);
                    else if ((float)hib <= d) copy(S0);
                    else copy(bc);
                } else {
                    const double Fd = (double)F;
                    const double l2 = lob - Fd, h2 = hib + Fd;
                    const double ad = l2 >= 0.0 ? std::min(lob, l2) : 0.0;
                    const double bd = hib > h2 ? hib : std::min(1.0, h2);
                    const float bf = (float)bd, hif = (float)hib, lof = (float)lob, af = (float)ad;
                    const float invO = 1.0f / (bf - hif);
                    if (af > d) copy(S1);
                    else if (d > bf) copy(S0);
                    else if (af <= d && lof >= d) {
                        const float q = (1.0f / (lof - af)) * (d - af);
                        const float t = 0.0f > q ? 0.0f : std::min(1.0f, q);
                        const float u = 1.0f - t;
                        for (int k = 0; k < 3; ++k) o[k] = (uint8_t)(int)((float)S1[k] * u + (float)bc[k] * t);
                        o[3] = S1[3];
                    } else if (hif > d) copy(bc);
                    else {
                        const float q = (d - hif) * invO;
                        const float t = 0.0f > q ? 0.0f : std::min(1.0f, q);
                        const float u = 1.0f - t;
                        for (int k = 0; k < 3; ++k) o[k] = (uint8_t)(int)((float)S0[k] * t + (float)bc[k] * u);
                        o[3] = S0[3];
                    }
                }
            }
            float* q = c.out.at(x, y);
            for (int k = 0; k < 4; ++k) q[k] = o[k] * (1.f / 255.f);
        }
    });
}

// Distance of a point in units of the star's tip radius; the star alternates tips (radius 1) and valleys (radius Ratio).
// dx runs right, dy up, ang0 is the first tip's angle.
double starMetric(double dx, double dy, int arms, double ratio, double ang0)
{
    const double seg = 2 * kPi / arms;
    double u = std::fmod(std::atan2(dy, dx) - ang0, seg);
    if (u < 0) u += seg;
    if (u > seg * 0.5) u = seg - u;
    const double ey = ratio * std::sin(seg * 0.5), ex = ratio * std::cos(seg * 0.5) - 1.0;
    const double den = std::cos(u) * ey - std::sin(u) * ex;
    const double rb = den > 1e-12 ? ey / den : 1e12;
    return std::sqrt(dx * dx + dy * dy) / rb;
}

void starWipe(Ctx& c)
{
    const int arms = std::max(2, c.i("Arms")), cyc = std::max(1, c.i("Cycles"));
    const double ratio = clampd(c.d("Ratio"), 1e-4, 1), ang0 = c.d("Angle") * kPi / 180.0;
    const bool rev = c.i("Direction") == 1, tooth = c.i("Waveform") == 1;
    // VEGAS names the flips after the axis they turn about: "horizontal" turns the picture upside down
    const bool flipY = c.b("HorizontalFlip"), flipX = c.b("VerticalFlip"), mirY = c.b("HorizontalMirror"), mirX = c.b("VerticalMirror");
    double cxn, cyn;
    c.xy("Center", cxn, cyn);
    const int W = c.out.w, H = c.out.h;
    const double cx = cxn * W, cy = cyn * H;
    const double p = clampd(c.progress, 0, 1);
    // Flips turn the star about its own centre; a mirror folds the picture about its middle (right/upper half kept).
    // VEGAS carries the centre along with the vertical turn but not the horizontal one.
    auto metric = [&](double px, double py, bool foldX, bool foldY) {
        double X = px, Py = H - py;                                         // Py runs up
        double Cxx = cx, Cyy = cy;
        if (flipX) X = W - X;
        if (flipY) { Py = H - Py; Cyy = H - cy; }
        if (foldX) X = W * 0.5 + std::fabs(X - W * 0.5);
        if (foldY) Py = H * 0.5 + std::fabs(Py - H * 0.5);
        double dx = X - Cxx;
        if (flipX && !foldX) dx = -(px - cx);                          // (turned about the centre, not the picture)
        double dy = Py - Cyy;
        if (flipY && !foldY) dy = -((H - py) - cy);
        return starMetric(dx, dy, arms, ratio, ang0);
    };
    const bool foldRx = mirX && std::fabs(cxn - 0.5) < 1e-6, foldRy = mirY && std::fabs(cyn - 0.5) < 1e-6;
    double rmax = 0;                                                        // the star must cover the whole picture at the end
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) rmax = std::max(rmax, metric(x + 0.5, y + 0.5, foldRx, foldRy));
    const double q0 = rmax > 0 ? 1.0 / rmax : 0.0;
    const double fv = clampd(c.d("Feather"), 0, 1), bv = clampd(c.d("Border"), 0, 1);
    const double wf = fv * 0.25 * (tooth ? 2.0 : 1.0), hw = bv * 0.125 * (tooth ? 2.0 : 1.0);
    const Px bc = colorPx(c.color("BorderColor"));
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double q = metric(x + 0.5, y + 0.5, mirX, mirY) * q0 * cyc;
            double f = q - std::floor(q);
            if (q >= cyc) f = 1.0;
            if (tooth) f = f < 0.5 ? 2 * f : 2 - 2 * f;
            const double dd = (rev ? 1.0 - p : p) - f;
            double t = fv > 0 ? clampd(0.5 + dd / wf, 0, 1) : (dd >= 0 ? 1.0 : 0.0);
            double bet = 0;
            if (hw > 0) bet = fv > 0 ? clampd((hw + wf * 0.5 - std::fabs(dd)) / (wf * 0.5), 0, 1) : (std::fabs(dd) <= hw ? 1.0 : 0.0);
            if (rev) t = 1.0 - t;
            Px a = fromAt(c, x + 0.5, y + 0.5), b = toAt(c, x + 0.5, y + 0.5);
            Px o = mixPx(a, b, t);
            if (bet > 0) o = mixPx(o, bc, bet);
            store(c, x, y, o);
        }
    });
}

void venetian(Ctx& c)
{
    const double ang = c.d("Angle") * kPi / 180.0, dx = std::cos(ang), dy = -std::sin(ang);
    const double n = std::max(1.0, c.d("NumberOfBlinds"));
    const double span = std::fabs(dx) + std::fabs(dy);
    wipe(c, [&](double u, double v) {
        const double pos = ((u - 0.5) * dx + (v - 0.5) * dy) / span * n + 0.5;
        return pos - std::floor(pos);
    }, clampd(c.d("Feather"), 0, 1) * 0.5 + 0.002, 0.0, Color());
}


// ------------------------------------------------------------------ motion
// Push: outgoing frame, border strip (Size * extent) and incoming frame are laid end to end and pulled across; strip ends are
// soft by Feather * extent. The seam runs from extent + 2 * strip to -strip. Without Push off previous image the outgoing frame is pinned to the strip.
void pushFx(Ctx& c)
{
    const int dir = c.i("Direction");
    const bool pushOff = c.b("PushOffPreviousImage");
    const int W = c.out.w, H = c.out.h;
    const bool horiz = dir >= 2;
    const double L = horiz ? W : H, p = clampd(c.progress, 0, 1);
    const double w = clampd(c.d("BorderSize"), 0, 1) * L, bw = w * 0.5, F = clampd(c.d("BorderFeather"), 0, 1) * L;
    const double Wd = std::max(F, 2.0), R = bw + F * 0.5;
    const bool border = w > 0 || F > 0;
    const Px bc = colorPx(c.color("BorderColor"));
    const bool neg = dir == 0 || dir == 2;
    const double sb = (L + 2 * w) - p * (L + 3 * w);
    const double seam = neg ? sb : L - sb;
    const double centre = neg ? seam - bw : seam + bw;
    const double shift = L - sb + w;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double a = (horiz ? x : y) + 0.5;
            const double early = border && F <= 0 ? 1.0 : 0.0;
            const bool inB = neg ? a >= seam - early : a < seam;
            Px o;
            if (inB) {
                const double ib = neg ? a - seam + early : a - seam + L;
                o = horiz ? toAt(c, ib, y + 0.5) : toAt(c, x + 0.5, ib);
            } else {
                const double fa = pushOff ? (neg ? a + shift : a - shift) : a;
                o = horiz ? fromAt(c, fa, y + 0.5) : fromAt(c, x + 0.5, fa);
            }
            if (border) o = mixPx(o, bc, smooth01((R - std::fabs(a - centre)) / Wd + 0.5));
            store(c, x, y, o);
        }
    });
}



// Swap: the two pictures slide apart and back, the one on top first the outgoing, then the incoming. The border strip sits on the
// top picture's facing edge; each picture is shifted by min(p, 1-p)(L+3w) - w.
void swapFx(Ctx& c)
{
    const int dir = c.i("Direction");
    const int W = c.out.w, H = c.out.h;
    const bool horiz = dir >= 2, flip = dir == 1 || dir == 3;
    const double L = horiz ? W : H, p = clampd(c.progress, 0, 1);
    const double w = clampd(c.d("BorderSize"), 0, 1) * L, bw = w * 0.5, F = clampd(c.d("BorderFeather"), 0, 1) * L;
    const double Wd = std::max(F, 2.0), R = bw + F * 0.5;
    const bool border = w > 0 || F > 0;
    const Px bc = colorPx(c.color("BorderColor"));
    const double O = std::max(0.0, std::min(p, 1 - p) * (L + 3 * w) - w);
    const bool aTop = p < 0.5;
    const double Oi = O - (F <= 0 && w > 0 ? 1.0 : 0.0);
    const double cen = aTop ? (L - O) + bw : O - bw;                         // the strip's middle
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double pos0 = (horiz ? x : y) + 0.5, pos = flip ? L - pos0 : pos0;
            const double sa = pos + Oi, sb = pos - Oi;
            const bool inA = sa >= 0 && sa < L, inB = sb >= 0 && sb < L;
            Px o = bc;
            const auto pa = [&]() { const double q = flip ? L - sa : sa; return horiz ? fromAt(c, q, y + 0.5) : fromAt(c, x + 0.5, q); };
            const auto pb = [&]() { const double q = flip ? L - sb : sb; return horiz ? toAt(c, q, y + 0.5) : toAt(c, x + 0.5, q); };
            if (aTop) { if (inA) o = pa(); else if (inB) o = pb(); }
            else { if (inB) o = pb(); else if (inA) o = pa(); }
            if (border) o = mixPx(o, bc, smooth01((R - std::fabs(pos - cen)) / Wd + 0.5));
            store(c, x, y, o);
        }
    });
}

// Squeeze: the incoming frame is squeezed into a band growing from the start side (or the middle for 4 and 5) over the outgoing
// frame, which is squeezed too with Squeeze previous image. The incoming band is sB = p(S+3w)-2w wide, then the border w, then
// the outgoing frame.
void squeezeFx(Ctx& c)
{
    const int start = c.i("Start");
    const bool prev = c.b("SqueezePreviousImage");
    const int W = c.out.w, H = c.out.h;
    const bool alongY = start == 0 || start == 1 || start == 4;
    const bool mid = start >= 4, far = start == 1 || start == 3;
    const double L = alongY ? H : W, S = mid ? L * 0.5 : L, p = clampd(c.progress, 0, 1);
    const double w = clampd(c.d("BorderSize"), 0, 1) * S, bw = w * 0.5, F = clampd(c.d("BorderFeather"), 0, 1) * S;
    const double Wd = std::max(F, 2.0), R = bw + F * 0.5;
    const bool border = w > 0 || F > 0;
    const Px bc = colorPx(c.color("BorderColor"));
    const double sB = p * (S + 3 * w) - 2 * w;
    const double cen = sB + bw;                                               // the border strip's middle
    const double aw = S - sB - w;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double pos = (alongY ? y : x) + 0.5;
            const bool right = pos > L * 0.5;
            const double s = mid ? std::fabs(pos - L * 0.5) : (far ? L - pos : pos);
            double t;
            bool inB = s < sB;
            if (inB) {
                const double f = s / std::max(sB, 1e-6);
                t = mid ? L * 0.5 + (right ? f : -f) * L * 0.5 : (far ? L - f * L : f * L);
            } else if (!prev) t = pos;
            else {
                const double f = (s - sB - w) / std::max(aw, 1e-6);
                if (mid) t = right ? L * 0.5 + f * L * 0.5 : L * 0.5 - f * L * 0.5;
                else t = far ? (1 - f) * L : f * L;
            }
            Px o;
            if (!inB && s < sB + w) o = bc;
            else o = inB ? (alongY ? toAt(c, x + 0.5, t) : toAt(c, t, y + 0.5))
                         : (alongY ? fromAt(c, x + 0.5, t) : fromAt(c, t, y + 0.5));
            if (border) o = mixPx(o, bc, smooth01((R - std::fabs(s - cen)) / Wd + 0.5));
            store(c, x, y, o);
        }
    });
}

// Zoom: the incoming picture grows out of the centre point ("Zoom in") or the outgoing one shrinks into it ("Zoom out"). Scale is
// q (p, or 1-p) times 1 + 2 * Feather, capped at 1. The border strip rides on the rectangle's edge, shrinks with it, and keeps
// moving while the picture is held at full size.
void zoomFx(Ctx& c)
{
    const bool out = c.i("Direction") == 1;
    double cx, cy;
    c.xy("Center", cx, cy);
    const int W = c.out.w, H = c.out.h;
    const double p = clampd(c.progress, 0, 1);
    const double K = std::max(W, H);
    const double w = clampd(c.d("BorderSize"), 0, 1) * K, F = clampd(c.d("BorderFeather"), 0, 1) * K;
    const bool border = w > 0 || F > 0;
    const Px bc = colorPx(c.color("BorderColor"));
    const double q = out ? 1 - p : p;
    const double virt = q * (1.0 + 2.0 * clampd(c.d("BorderFeather"), 0, 1));
    const double sc = std::max(1e-4, std::min(1.0, virt));
    const double sp = F <= 0 && w > 0 && sc < 1.0 ? sc + 2.0 / W : sc;
    const double px0 = cx * W, py0 = (1 - cy) * H;
    const double x0 = px0 - sc * px0, x1 = px0 + sc * (W - px0), y0 = py0 - sc * py0, y1 = py0 + sc * (H - py0);
    // the strip shrinks with the scale (soft edge never under 2 pixels)
    const double Wd = std::max(F * sc, 2.0), R = (w * 0.5 + F * 0.5) * sc;
    const double cen = ((virt - sc) * W * 0.5 + w * 0.5) * sc;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double fx = x + 0.5, fy = y + 0.5;
            // how much of the pixel the rectangle covers (its edges are not snapped to pixels)
            const double cov = std::max(0.0, std::min(fx + 0.5, x1) - std::max(fx - 0.5, x0)) * std::max(0.0, std::min(fy + 0.5, y1) - std::max(fy - 0.5, y0));
            const double qx = (fx - px0) / sp + px0, qy = (fy - py0) / sp + py0;
            auto pic = [&]() {
                const Buf& b = out ? c.src : c.src2;
                if (sp >= 1.0) { Px m; b.sampleClamped(qx, qy, m.v); return m; }
                return boxAt(b, (fx - 0.5 - px0) / sp + px0, (fx + 0.5 - px0) / sp + px0, (fy - 0.5 - py0) / sp + py0, (fy + 0.5 - py0) / sp + py0);
            };
            double ring = 0;
            if (border) {
                const double ex = std::max(x0 - fx, fx - x1), ey = std::max(y0 - fy, fy - y1);
                const double d = ex > 0 && ey > 0 ? std::sqrt(ex * ex + ey * ey) : std::max(ex, ey);
                ring = smooth01((R - std::fabs(d - cen)) / Wd + 0.5);
            }
            Px o;
            if (cov >= 1.0) {
                o = pic();
                if (ring > 0) o = mixPx(o, bc, ring);
            } else {
                o = out ? toAt(c, fx, fy) : fromAt(c, fx, fy);
                if (ring > 0) o = mixPx(o, bc, ring);
                if (cov > 0.0) o = mixPx(o, pic(), cov);
            }
            store(c, x, y, o);
        }
    });
}

void flashFx(Ctx& c)
{
    // White light added to the outgoing picture (the incoming one after the midpoint): the amount follows the square of a
    // triangle; the diffusions add the Rec.601 luma of the picture blurred by a Gaussian of 5% of the diagonal times the diffusion.
    // Adding truncates the fraction; the tint does not change VEGAS's picture.
    const int W = c.out.w, H = c.out.h;
    const double p = clampd(c.progress, 0, 1);
    const double lv = (1 - std::fabs(1 - 2 * p)) * (1 - std::fabs(1 - 2 * p));
    const double dh = clampd(c.d("HorizontalDiffusion"), 0, 1), dv = clampd(c.d("VerticalDiffusion"), 0, 1);
    const bool diffuse = dh > 0 || dv > 0;
    const Buf& gs = p < 0.5 ? c.src : c.src2;
    const double diag = std::sqrt((double)W * W + (double)H * H);
    std::vector<float> lum;
    if (diffuse) {
        lum.resize((size_t)W * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float* q = gs.at(x, y);
                lum[(size_t)y * W + x] = (0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2]) * 255.f;
            }
        std::vector<float> tmp(lum.size());
        for (int pass = 0; pass < 2; ++pass) {
            const bool horiz = pass == 0;
            const double sg = std::max(0.01, (horiz ? dh : dv) * 0.05 * diag);
            const int rad = std::max(1, (int)std::ceil(sg * 4));
            std::vector<double> k((size_t)rad * 2 + 1);
            double sum = 0;
            for (int i = -rad; i <= rad; ++i) sum += k[(size_t)(i + rad)] = std::exp(-0.5 * i * i / (sg * sg));
            for (double& v : k) v /= sum;
            const int n = horiz ? W : H, m = horiz ? H : W;
            for (int j = 0; j < m; ++j)
                for (int i = 0; i < n; ++i) {
                    double acc = 0;
                    for (int t = -rad; t <= rad; ++t) {
                        const int ii = std::min(std::max(i + t, 0), n - 1);
                        acc += k[(size_t)(t + rad)] * (horiz ? lum[(size_t)j * W + ii] : lum[(size_t)ii * W + j]);
                    }
                    (horiz ? tmp[(size_t)j * W + i] : tmp[(size_t)i * W + j]) = (float)acc;
                }
            lum.swap(tmp);
        }
    }
    const double gain = (dh + dv) * 0.5 * 2.947, base = diffuse ? 251.6 : 255.0;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            Px o = p < 0.5 ? fromAt(c, x + 0.5, y + 0.5) : toAt(c, x + 0.5, y + 0.5);
            double add = base;
            if (diffuse) add += gain * lum[(size_t)y * W + x];
            add = (add * lv - 0.5) / 255.0;
            for (int k = 0; k < 3; ++k) o.v[k] = std::min(o.v[3], o.v[k] + (float)add);
            store(c, x, y, o);
        }
    });
}

void portalsFx(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const int n = std::max(2, c.i("Squares")), seed = c.i("RandomPatternSeed");
    const double maxT = clampd(c.d("MaxTransparency"), 0, 1), maxO = c.d("MaxOffset");
    const double bs = borderOf(c) * 0.3;
    const Color bc = c.color("BorderColor");
    auto rnd = [&](int a, int b, int k) {
        uint32_t h = (uint32_t)a * 374761393u + (uint32_t)b * 668265263u + (uint32_t)k * 1013904223u + (uint32_t)seed * 1664525u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0;
    };
    const double p = c.progress;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double u = (x + 0.5) / W * n, v = (y + 0.5) / H * n;
            const int gx = (int)std::floor(u), gy = (int)std::floor(v);
            const double when = rnd(gx, gy, 1) * 0.6;
            const double t = clampd((p * 1.6 - when) / 0.4, 0, 1);
            Px o = fromAt(c, x + 0.5, y + 0.5);
            if (t > 0) {
                const double fx = u - gx, fy = v - gy;
                const double ox = (rnd(gx, gy, 3) - 0.5) * maxO * (1 - t) * W * 0.1, oy = (rnd(gx, gy, 4) - 0.5) * maxO * (1 - t) * H * 0.1;
                const Px b = toAt(c, x + 0.5 + ox, y + 0.5 + oy);
                o = mixPx(o, b, (1.0 - maxT * (1 - t)) * smooth01(t * 2));
                if (bs > 0 && t < 1 && (fx < bs * 0.2 || fx > 1 - bs * 0.2 || fy < bs * 0.2 || fy > 1 - bs * 0.2)) o = mixPx(o, colorPx(bc), 0.8 * (1 - t));
            }
            store(c, x, y, o);
        }
    });
}

// ------------------------------------------------------------------ 3D and page transitions, drawn flat
// 3D Blinds / 3D Cascade: strips that turn over, showing the outgoing frame on one face and the incoming on the other.
void blindsLike(Ctx& c, bool cascade)
{
    const int W = c.out.w, H = c.out.h;
    const int dir = c.i("Direction");
    const bool horizontal = dir == 1 || dir == 3;                   // strips run across, arranged top to bottom
    const bool reverse = dir == 2 || dir == 3;
    const int n = std::max(1, c.i("Divisions"));
    const double stagger = cascade ? 0.6 : clampd(c.d("Stagger"), 0, 1) * 0.6;
    const double p = c.progress;
    const double light = clampd(c.d("SpecularLight"), 0, 1);
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double u = (x + 0.5) / W, v = (y + 0.5) / H;
            const double along = horizontal ? v : u;
            const int idx = std::min(n - 1, (int)(along * n));
            const double cell = along * n - idx;
            const double order = (double)(reverse ? n - 1 - idx : idx) / std::max(1, n - 1);
            const double t = clampd((p - order * stagger) / std::max(0.05, 1 - stagger), 0, 1);
            const double ang = t * kPi;                              // half a turn
            const double sc = std::cos(ang);
            const double width = std::fabs(sc);
            const double across = horizontal ? u : v;
            (void)across;
            const double mid = (idx + 0.5) / n;
            const double a = (along - mid) * (horizontal ? H : W);
            const double a2 = a / std::max(0.02, width);
            const double limit = (horizontal ? H : W) * 0.5 / n;
            Px o = colorPx(Color{0, 0, 0, 0});
            if (std::fabs(a2) <= limit) {
                const double q = (mid * (horizontal ? H : W)) + a2;
                const double qx = horizontal ? x + 0.5 : q, qy = horizontal ? q : y + 0.5;
                o = t < 0.5 ? fromAt(c, qx, qy) : toAt(c, qx, qy);
                const float shade = 1.f - (float)(0.55 * (1 - width)) + (float)(light * 0.3 * std::sin(ang) * (cell - 0.5));
                for (int k = 0; k < 3; ++k) o.v[k] = std::min(o.v[k] * shade, o.v[3]);
            }
            (void)cell;
            if (o.v[3] < 1.f) { Px under = t < 0.5 ? toAt(c, x + 0.5, y + 0.5) : fromAt(c, x + 0.5, y + 0.5); under = mixPx(colorPx(Color{0, 0, 0, 1}), under, 0.15); for (int k = 0; k < 4; ++k) o.v[k] = o.v[k] + under.v[k] * (1 - o.v[3]); }
            store(c, x, y, o);
        }
    });
}
void blinds3d(Ctx& c) { blindsLike(c, false); }
void cascade3d(Ctx& c) { blindsLike(c, true); }

// 3D Fly In/Out: the incoming frame flies in from far away, scaling up and turning; or the outgoing flies away.
void fly3d(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const bool out = c.i("Direction") == 1;
    const double p = c.progress;
    const double fx = c.d("FarXPosition"), fy = c.d("FarYPosition"), fz = c.d("FarZPosition");
    const double zr = c.d("ZRotations") * 2 * kPi;
    const double t = out ? p : 1 - p;
    const double s = std::max(0.02, std::pow(1.0 - t * 0.98, 1.0 + std::fabs(fz) * 0.02));
    const double ang = zr * t, ca = std::cos(ang), sa = std::sin(ang);
    const double ox = fx * 0.01 * W * t, oy = -fy * 0.01 * H * t;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double dx = (x + 0.5 - W * 0.5 - ox) / s, dy = (y + 0.5 - H * 0.5 - oy) / s;
            const double qx = dx * ca + dy * sa + W * 0.5, qy = -dx * sa + dy * ca + H * 0.5;
            Px o = out ? toAt(c, x + 0.5, y + 0.5) : fromAt(c, x + 0.5, y + 0.5);
            const Px f = out ? fromAt(c, qx, qy) : toAt(c, qx, qy);
            for (int k = 0; k < 4; ++k) o.v[k] = f.v[k] * (float)(1 - 0.0) + o.v[k] * (1 - f.v[3]);
            store(c, x, y, o);
        }
    });
}

// 3D Shuffle: the two frames swap places, one passing in front of the other.
void shuffle3d(Ctx& c) { swapFx(c); }

// Page Peel / Page Roll / Page Loop: the outgoing frame lifts from one side and curls over, revealing the incoming one.
// Page Loop, Page Peel and Page Roll are VEGAS's CPU processors (Vfx1.ofx) sharing one renderer: 8-bit pixels, rows bottom first.
// Each output pixel's offset from the centre is rotated by the angle; a per-transition hit test gives the layers drawn in order,
// the page position (u, v), light/black mix and opacity. Pixels whose four corners show the same layer use an 8-bit bilinear
// read, others average 2 x 2 nearest samples. Single precision throughout, as VEGAS.
namespace page {

struct Img8 {
    int w = 0, h = 0;
    std::vector<uint8_t> px;            // RGBA, rows bottom first (VEGAS's order)
    const uint8_t* at(int x, int y) const { return &px[((size_t)y * w + x) * 4]; }
    uint8_t* at(int x, int y) { return &px[((size_t)y * w + x) * 4]; }
};

Img8 toBytes(const Buf& b)
{
    Img8 r;
    r.w = b.w;
    r.h = b.h;
    r.px.resize((size_t)b.w * b.h * 4);
    for (int y = 0; y < b.h; ++y) {
        const float* s = b.at(0, b.h - 1 - y);
        uint8_t* d = r.at(0, y);
        for (int k = 0; k < b.w * 4; ++k) {
            const float v = std::isfinite(s[k]) ? std::min(std::max(s[k], 0.f), 1.f) : 0.f;
            d[k] = (uint8_t)std::lround(v * 255.f);
        }
    }
    return r;
}

// cvttss2si: NaN and values out of range give INT_MIN (which then fails every bounds test, as in VEGAS)
inline int cvtt(float x) { return (x > -2147483648.f && x < 2147483648.f) ? (int)x : INT_MIN; }
inline int cvtt(double x) { return (x > -2147483648.0 && x < 2147483648.0) ? (int)x : INT_MIN; }

// VEGAS's 8-bit mix: p + (q - p) * t / 255, rounded its way and clamped
inline uint8_t mix8(int p, int q, int t)
{
    const int m = (q - p) * t + 128;
    const int d = p + ((m + (m >> 8)) >> 8);
    return (uint8_t)(d < 0 ? 0 : d > 255 ? 255 : d);
}

struct Hit {
    float u = 0, v = 0;
    uint8_t light = 0, alpha = 255;
    uint8_t col[4] = {255, 255, 255, 255};
};

// sinf / cosf as VEGAS's runtime gives them: double result rounded to float, but results within a thousandth of a float step
// of the halfway point go to the larger magnitude (sinf of 210 degrees is -0.5, not -0.49999997).
inline float roundLikeCrt(double t)
{
    const float f = (float)t;
    if (!std::isfinite(f) || (double)f == t) return f;
    const float o = std::nextafter(f, t > (double)f ? HUGE_VALF : -HUGE_VALF);
    const double mid = 0.5 * ((double)f + (double)o);
    if (std::fabs(t - mid) < 1e-3 * std::fabs((double)o - (double)f)) return std::fabs(o) > std::fabs(f) ? o : f;
    return f;
}
inline float crtSin(float a) { return roundLikeCrt(std::sin((double)a)); }
inline float crtCos(float a) { return roundLikeCrt(std::cos((double)a)); }

// The direction of travel: (360 - angle) degrees, turned for the pixel aspect (square here) as VEGAS does
inline float travelAngle(float a)
{
    const float cs = crtCos(a);
    if (cs > 1e-6f) {
        float r = std::atan2(crtSin(a), cs);
        if (r < 0) r += 6.2831854820251465f;
        return r;
    }
    return a;
}

// The renderer (Vfx1.ofx 0x1805e42c0 and its twins). 'paper' is the page that moves, 'bg' what lies under it.
template <class HitFn>
void render(const Img8& paper, const Img8& bg, Img8& out, int layers, float ang, const uint8_t lightCol[4], HitFn hit)
{
    const int w = bg.w, h = bg.h;
    out = bg;
    if (w <= 0 || h <= 0) return;
    const float sn = crtSin(ang), cs = crtCos(ang), msn = -sn;
    const float W2 = (float)w * 0.5f, H2 = (float)h * 0.5f;
    const float c28 = 1.0f, ic28 = 1.0f / c28;
    auto coords = [&](float row, float col, float& a, float& b) {
        const float yy = (row - H2) * c28;
        const float ys = yy * sn, yc = yy * cs;
        const float xx = col - W2;
        a = xx * cs + ys;
        b = xx * msn + yc;
    };
    auto back = [&](const Hit& t, float& row, float& col) {
        const float r0 = t.u * sn;
        float r = t.v * cs;
        r += r0;
        float c0 = t.v * msn;
        c0 += t.u * cs;
        row = r * ic28 + H2;
        col = c0 + W2;
    };
    std::vector<uint8_t> mask((size_t)w * h, 0x7f);
    forRows(h, w, [&](int r) {
        for (int x = 0; x < w; ++x) {
            float a, b;
            coords((float)r, (float)x, a, b);
            uint8_t* d = out.at(x, r);
            for (int L = 0; L < layers; ++L) {
                Hit t;
                std::memcpy(t.col, lightCol, 4);
                if (!hit(a, b, L, t)) continue;
                float row, col;
                back(t, row, col);
                const float fr = std::floor(row), fc = std::floor(col);
                const int ri = cvtt(fr), ci = cvtt(fc);
                if (ri == INT_MIN || ci == INT_MIN || ri + 1 < 0 || ri >= h || ci + 1 < 0 || ci >= w) continue;
                const int wy = cvtt(std::floor((row - (float)ri) * 256.f)), wx = cvtt(std::floor((col - (float)ci) * 256.f));
                int flag = L;
                const uint8_t* p00 = (ri >= 0 && ci >= 0) ? paper.at(ci, ri) : d;
                const uint8_t* p01 = (ri >= 0 && ci + 1 < w) ? paper.at(ci + 1, ri) : d;
                const uint8_t* p10 = (ri + 1 < h && ci >= 0) ? paper.at(ci, ri + 1) : d;
                const uint8_t* p11 = (ri + 1 < h && ci + 1 < w) ? paper.at(ci + 1, ri + 1) : d;
                if (ci < 0 || ci + 1 >= w || ri < 0 || ri + 1 >= h) flag = 0x80 + L;
                uint8_t P[4];
                for (int k = 0; k < 4; ++k)
                    P[k] = (uint8_t)((unsigned)(((p11[k] * wx + p10[k] * (256 - wx)) * wy + (p01[k] * wx + p00[k] * (256 - wx)) * (256 - wy))) >> 16);
                if (t.light) {
                    for (int k = 0; k < 3; ++k) P[k] = mix8(P[k], t.col[k], t.light);
                    P[3] = mix8(P[3], 255, t.light);
                }
                for (int k = 0; k < 4; ++k) d[k] = mix8(d[k], P[k], t.alpha);
                mask[(size_t)r * w + x] = (uint8_t)flag;
            }
        }
    });
    Img8 fin = out;
    auto m = [&](int r, int x) { return mask[(size_t)std::min(r, h - 1) * w + std::min(x, w - 1)]; };
    forRows(h, w, [&](int r) {
        for (int x = 0; x < w; ++x) {
            const uint8_t m0 = m(r, x);
            if (m0 < 0x80) {
                uint8_t m3 = m(r + 1, x + 1);
                // (VEGAS's last column reads a byte its row buffer kept from three rows before)
                if (x == w - 1 && r + 1 < h) m3 = r >= 2 ? mask[(size_t)(r - 2) * w + w - 1] : 0;
                const uint8_t a1 = m(r, x + 1), a2 = m(r + 1, x);
                if (m0 == a1 && m0 == a2 && m0 == m3) continue;
            }
            unsigned acc[4] = {0, 0, 0, 0};
            const float r1 = (float)(r + 1);
            for (float sy = r1 - 0.666f; sy < r1 - 0.1f; sy += 0.333f)
                for (float sx = (float)x + 0.333f; sx < (float)x + 0.7f; sx += 0.333f) {
                    uint8_t px[4];
                    std::memcpy(px, bg.at(x, r), 4);
                    float a, b;
                    coords(sy, sx, a, b);
                    for (int L = 0; L < layers; ++L) {
                        Hit t;
                        std::memcpy(t.col, lightCol, 4);
                        if (!hit(a, b, L, t)) continue;
                        float row, col;
                        back(t, row, col);
                        const int ri = cvtt(std::floor(row));
                        if ((unsigned)ri >= (unsigned)h) continue;
                        const int ci = cvtt(std::floor(col));
                        if ((unsigned)ci >= (unsigned)w) continue;
                        uint8_t P[4];
                        std::memcpy(P, paper.at(ci, ri), 4);
                        if (t.light) {
                            for (int k = 0; k < 3; ++k) P[k] = mix8(P[k], t.col[k], t.light);
                            P[3] = mix8(P[3], 255, t.light);
                        }
                        for (int k = 0; k < 4; ++k) px[k] = mix8(px[k], P[k], t.alpha);
                    }
                    for (int k = 0; k < 4; ++k) acc[k] += px[k];
                }
            uint8_t* o = fin.at(x, r);
            for (int k = 0; k < 4; ++k) o[k] = (uint8_t)(int)((float)acc[k] * 0.25f);
        }
    });
    out.px.swap(fin.px);
}

void store(Ctx& c, const Img8& img)
{
    const int W = c.out.w, H = c.out.h;
    for (int y = 0; y < H && y < img.h; ++y) {
        const uint8_t* s = img.at(0, H - 1 - y);
        float* d = c.out.at(0, y);
        for (int k = 0; k < std::min(W, img.w) * 4; ++k) d[k] = s[k] * (1.f / 255.f);
    }
}

// How far the frame reaches along the direction of travel (W cos + H sin, angle folded into 0..90), computed as each processor does
// (Page Loop in tenths of a degree with float steps, the others in double/float).
float spanLoop(float ang, float W, float H)
{
    const float diag = std::sqrt(W * W + H * H);
    const int ic = cvtt(ang * 572.9578247070312f);
    const int m = ic - (ic / 1800) * 1800;
    float phi = (float)m * 0.001745329238474369f;
    if (phi > 1.5707963705062866f) phi = 3.1415927410125732f - phi;
    phi -= std::atan2(H, W);
    return std::cos(phi) * diag;
}

}  // namespace page

// Page Loop: the incoming page comes in lying flat from the PaperAngle side, rises along an 80 degree ramp and runs round a loop of
// LoopRadius; the loop position eases from LoopPosition to the far side as progress^2, as does the paper opacity. Three layers:
// flat part and ramp, near half of the loop, far half. Perspective shrinks the far side.
void pageLoop(Ctx& c)
{
    using namespace page;
    const Img8 A = toBytes(c.src), B = toBytes(c.src2);
    const int w = B.w, h = B.h;
    const float ang = travelAngle((float)((360.0 - c.d("PaperAngle")) * 0.01745329238474369));
    const float span = spanLoop(ang, (float)w, (float)h);
    const float p = (float)c.progress, L = (float)c.d("LoopPosition"), O = (float)c.d("PaperOpacity");
    const float Rr = (float)c.d("LoopRadius");
    const float x0 = (((1.f - L) * (p * p) + L) - 0.5f) * span;
    const float half = span * 0.5f;
    const float R = std::max(half * Rr, 0.001f), Rset = half * Rr;
    const float circ = Rset * 6.2831854820251465f;
    const float ramp = (half - x0) / std::sin(1.3962634f);
    const float op = ((1.f - O) * (p * p) + O) * 255.f;
    const float shift = (((half + x0) + circ) + ramp * 1.3962634f) * (1.f - p);
    const float lo = x0 - Rset, hi = x0 + Rset, persp = (float)c.d("Perspective") * 0.125f;
    const Color lc = c.color("LightColor");
    const uint8_t light[4] = {(uint8_t)cvtt(lc.r * 255.0 + 0.5), (uint8_t)cvtt(lc.g * 255.0 + 0.5), (uint8_t)cvtt(lc.b * 255.0 + 0.5), 255};
    const float k2pi = 0.6366197466850281f, k4pi = 1.2732394933700562f, q = 0.7853981852531433f;
    Img8 out;
    render(B, A, out, 3, ang, light, [&](float a, float b, int layer, Hit& t) {
        if (layer == 0) {
            if (x0 > a || !(1.f > p)) {                               // lying flat
                t.u = (x0 > a ? a : a + circ) - shift;
                t.v = b;
                t.light = 0;
                t.alpha = (uint8_t)cvtt(op + 0.5f);
                return true;
            }
            const float th = std::asin((a - x0) / ramp);                 // on the ramp
            const float g = 1.f - persp * (1.f - std::cos(th));
            t.u = ((x0 + circ) + ramp * th - shift) * g;
            t.v = g * b;
            t.alpha = (uint8_t)cvtt((255.f - op) * (th * k2pi) + op + 0.5f);
            t.light = (uint8_t)cvtt((1.f - (float)std::fabs((double)((th - q) * k4pi))) * 200.f + 0.5f);
            return true;
        }
        if (!(lo < a) || !(a < hi)) return false;
        const bool pos = a >= x0;
        const float th = std::asin(pos ? (a - x0) / R : (x0 - a) / R);
        t.alpha = (uint8_t)cvtt((255.f - op) * (th * k2pi) + op + 0.5f);
        const float cs = std::cos(th);
        const float ab = (float)std::fabs((double)((th - q) * k4pi));
        if (layer == 1) {
            const float g = 1.f - persp * (1.f - cs);
            t.u = (pos ? (x0 + R * th) - shift : ((6.2831854820251465f - th) * R + x0) - shift) * g;
            t.v = b * g;
            t.light = pos ? (uint8_t)cvtt((1.f - ab) * 200.f) : 0;
        } else {
            const float g = 1.f - persp * (cs + 1.f);
            t.u = (pos ? ((3.1415927410125732f - th) * R + x0) - shift : ((th + 3.1415927410125732f) * R + x0) - shift) * g;
            t.v = b * g;
            t.light = pos ? 0 : (uint8_t)cvtt((1.f - ab) * 200.f + 0.5f);
        }
        return true;
    });
    store(c, out);
}

// Page Peel: the outgoing page is lifted along the PeelAngle side round a fold of radius FoldRadius * min(W, H) and laid back over
// itself (opacity PeelOpacity, back lit towards the light colour). The fold travels the run plus its radius; Slide amount drags
// the page. Two layers: the page, and the turned-over part.
void pagePeel(Ctx& c)
{
    using namespace page;
    const Img8 A = toBytes(c.src), B = toBytes(c.src2);
    const int w = A.w, h = A.h;
    const float ang = travelAngle((360.f - (float)c.d("PeelAngle")) * 0.01745329238474369f);
    const float W = (float)w, Hh = (float)h;
    float span;
    {
        const float diag = std::sqrt(W * W + Hh * Hh);
        const int ic = cvtt((double)(ang * 1800.f) / kPi);
        const int m = ic - (ic / 1800) * 1800;
        float phi = (float)(m * kPi / 1800.0);
        if (phi > 1.5707963705062866f) phi = (float)(kPi - (double)phi);
        phi -= std::atan2(Hh, W);
        span = diag * std::cos(phi);
    }
    const float p = (float)c.progress, FR = (float)c.d("FoldRadius"), sl = (float)c.d("SlideAmount");
    const float mn = std::min(W, Hh), R = mn * FR;
    float slide = (p - FR * 1.5707963705062866f) * sl * mn;
    if (0.f > slide) slide = 0.f;
    const float x3 = span * 0.5f - (R + span) * p - slide;          // where the fold starts
    const float piR = (float)((double)R * kPi);
    const float c48 = (x3 + x3) + piR, c4c = piR + x3, c54 = x3 + R;
    const float iR = R > 0.f ? 1.f / R : 1e14f;
    const float persp = (float)c.d("Perspective") * 0.125f;
    const int op = cvtt((float)c.d("PeelOpacity") * 255.f) & 255, k = (255 - op) & 255;
    const float lk = 200.f * 1.2732394933700562f;
    const Color lc = c.color("LightColor");
    const uint8_t light[4] = {(uint8_t)cvtt((float)lc.r * 255.f + 0.5f), (uint8_t)cvtt((float)lc.g * 255.f + 0.5f), (uint8_t)cvtt((float)lc.b * 255.f + 0.5f), 255};
    Img8 out;
    render(A, B, out, 2, ang, light, [&](float a, float b, int layer, Hit& t) {
        const float a1 = a - slide;
        t.light = 0;
        if (x3 > a1) {
            if (layer == 0) { t.u = a1; t.v = b; t.alpha = 255; return true; }
            const float s = 1.f - (persp + persp);
            t.u = (c48 - a1) * s;
            t.v = s * b;
            t.alpha = (uint8_t)op;
            return true;
        }
        if (!(c54 > a1)) return false;
        const float th = std::asin((a1 - x3) * iR);
        const float cs = std::cos(th);
        if (layer == 0) {
            const float s = (cs - 1.f) * persp + 1.f;
            t.alpha = (uint8_t)(cvtt((kPi - (double)th) * (double)k * 0.3183098861837907) + op);
            t.u = (th * R + x3) * s;
            t.v = s * b;
        } else {
            t.light = (uint8_t)(200 - cvtt(std::fabs((double)th - 0.7853981633974483) * (double)lk));
            t.alpha = (uint8_t)(cvtt((double)((float)k * th) * 0.3183098861837907) + op);
            const float s = 1.f - (cs + 1.f) * persp;
            t.u = (c4c - th * R) * s;
            t.v = s * b;
        }
        return true;
    });
    store(c, out);
}

// Page Roll: the outgoing page rolls up into a cylinder of radius FoldRadius * min(W, H) travelling from the RollAngle side. Four
// layers: the page, the roll's back (shaded), its inner turn (radius 0.9 R) and its front (lit, darkened on the shadow side).
void pageRoll(Ctx& c)
{
    using namespace page;
    const Img8 A = toBytes(c.src), B = toBytes(c.src2);
    const int w = A.w, h = A.h;
    const float ang = travelAngle((float)((360.0 - c.d("RollAngle")) * 0.01745329238474369));
    const float W = (float)w, Hh = (float)h;
    const float PIf = 3.1415927410125732f;
    float span;
    {
        const float diag = std::sqrt(W * W + Hh * Hh);
        const int ic = cvtt((ang * 1800.f) / PIf);
        const int m = ic - (ic / 1800) * 1800;
        float phi = ((float)m * PIf) / 1800.f;
        if (phi > 1.5707963705062866f) phi = PIf - phi;
        phi -= std::atan2(Hh, W);
        span = diag * std::cos(phi);
    }
    const float p = (float)c.progress, FR = (float)c.d("FoldRadius"), sl = (float)c.d("SlideAmount");
    const float mn = std::min(W, Hh), R = mn * FR, R2 = R * 0.9f;
    float slide = (p - FR * 1.5707963705062866f) * sl * mn;
    if (0.f > slide) slide = 0.f;
    const float D = span * 0.5f - (R + span) * p - slide;
    const float c7c = R * PIf + D, c88 = D + R;
    const float iR = R > 0.f ? 1.f / R : 1e14f, iR2 = R2 > 0.f ? 1.f / R2 : 1e14f;
    const float persp = (float)c.d("Perspective") * 0.125f;
    const int op = cvtt(c.d("RollOpacity") * 255.0) & 255;
    const float k = (float)((255 - op) & 255), lk = 200.f * 1.2732394933700562f, invpi = 0.31830987334251404f;
    const Color lc = c.color("LightColor");
    const uint8_t light[4] = {(uint8_t)cvtt(lc.r * 255.0 + 0.5), (uint8_t)cvtt(lc.g * 255.0 + 0.5), (uint8_t)cvtt(lc.b * 255.0 + 0.5), 255};
    static const uint8_t black[4] = {0, 0, 0, 255};
    auto alpha = [&](float x) { return (uint8_t)(cvtt(x * invpi) + op); };
    Img8 out;
    render(A, B, out, 4, ang, light, [&](float a, float b, int layer, Hit& t) {
        const float a1 = a - slide;
        t.light = 0;
        if (layer == 0) {
            if (D > a1) { t.u = a1; t.v = b; t.alpha = 255; return true; }
            if (!(c88 > a1)) return false;
            const float th = std::asin((a1 - D) * iR);
            const float s = (std::cos(th) - 1.f) * persp + 1.f;
            t.alpha = alpha(k * (PIf - th));
            t.u = (th * R + D) * s;
            t.v = b * s;
            return true;
        }
        if (layer == 1) {
            if ((D - R) > a1 || !(a1 < D + R2)) return false;
            const bool neg = D > a1;
            const float s = neg ? std::asin((D - a1) * iR) : std::asin((a1 - D) * iR2);
            const float g = (std::cos(s) - 1.f) * persp + 1.f;
            t.u = (neg ? (R * PIf + c7c) - R * s : (R * s + D) + R * 6.2831854820251465f) * g;
            t.v = b * g;
            t.alpha = alpha(k * (PIf - s));
            return true;
        }
        const float Rl = layer == 2 ? R2 : R;
        if ((D - Rl) > a1 || !(a1 < (layer == 2 ? R2 + D : c88))) return false;
        const bool neg = D > a1;
        const float s = std::asin(neg ? (D - a1) * (layer == 2 ? iR2 : iR) : (a1 - D) * (layer == 2 ? iR2 : iR));
        if (neg) {
            t.light = (uint8_t)(200 - cvtt((1.5707963705062866f - s) * lk * 0.5f));
            std::memcpy(t.col, black, 4);
        } else {
            t.light = (uint8_t)(200 - cvtt((float)std::fabs((double)(s - 0.7853981852531433f)) * lk));
        }
        float u;
        if (layer == 2) u = neg ? (R2 * s + D) + R2 * 9.42477798461914f : (D - R2 * s) + R2 * 9.42477798461914f;
        else u = neg ? R * s + c7c : c7c - R * s;
        const float g = 1.f - (std::cos(s) + 1.f) * persp;
        t.u = u * g;
        t.v = b * g;
        t.alpha = alpha(k * s);
        return true;
    });
    store(c, out);
}

// ------------------------------------------------------------------ VEGAS's 2D transition library
// Spiral, Slide and Split are VEGAS's CPU processors (Vfx1.ofx) built on one small library: 8-bit pixels, rows top first,
// positions in 18.14 fixed point; a bilinear rectangle blit with coverage-mixed edge pixels, and a border round axis-aligned
// edges (solid out to the size, then a smoothstep fade).
namespace sf {

using page::Img8;
using page::mix8;

struct Rect { int x0, y0, x1, y1; };

Img8 toBytesTop(const Buf& b)
{
    Img8 r;
    r.w = b.w;
    r.h = b.h;
    r.px.resize((size_t)b.w * b.h * 4);
    for (size_t k = 0; k < r.px.size(); ++k) {
        const float v = std::isfinite(b.px[k]) ? std::min(std::max(b.px[k], 0.f), 1.f) : 0.f;
        r.px[k] = (uint8_t)std::lround(v * 255.f);
    }
    return r;
}

void storeTop(Ctx& c, const Img8& img)
{
    const size_t n = std::min(c.out.px.size(), img.px.size());
    for (size_t k = 0; k < n; ++k) c.out.px[k] = img.px[k] * (1.f / 255.f);
}

Rect isect(const Rect& a, const Rect& b)
{
    const Rect r = {std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    if (r.x0 >= r.x1 || r.y0 >= r.y1) return Rect{0, 0, 0, 0};
    return r;
}

inline int fl14(int v) { return v >= 0 ? v >> 14 : -((-v + 16383) >> 14); }       // floor(v / 16384)
inline int ce14(int v) { return -fl14(-v); }                                        // ceil(v / 16384)
inline int dtoi(double v) { return (v > -2147483648.0 && v < 2147483648.0) ? (int)v : INT_MIN; }

// 14-bit bilinear of the four pixels, VEGAS's scalar rounding
inline void bil(const uint8_t* r0, const uint8_t* r1, int i0, int i1, int fx, int fy, uint8_t* out)
{
    const unsigned w00 = (unsigned)((0x4000 - fy) * (0x4000 - fx)) >> 4, w10 = (unsigned)((0x4000 - fx) * fy) >> 4;
    const unsigned w01 = (unsigned)((0x4000 - fy) * fx) >> 4, w11 = (unsigned)(fx * fy) >> 4;
    for (int k = 0; k < 4; ++k)
        out[k] = (uint8_t)((r0[i0 * 4 + k] * w00 + r1[i0 * 4 + k] * w10 + r0[i1 * 4 + k] * w01 + r1[i1 * 4 + k] * w11 + 0x800000u) >> 24);
}

// same as VEGAS's SSE2 code (16-bit weights from 0x3fff, products kept to 16 bits, +4, >> 3)
inline void bilSse(const uint8_t* r0, const uint8_t* r1, int i0, int i1, int fx, int fy, uint8_t* out)
{
    const unsigned A = (unsigned)((0x3fff - fy) << 2) & 0xffff, F = (unsigned)(fy << 2) & 0xffff, a = (unsigned)(0x3fff - fx);
    const int w00 = (int)((a * A) >> 16), w01 = (int)(((unsigned)fx * A) >> 16), w10 = (int)((a * F) >> 16), w11 = (int)(((unsigned)fx * F) >> 16);
    for (int k = 0; k < 4; ++k) {
        auto mh = [](int v, int w) { return ((v << 5) * w) >> 16; };
        int s = std::min(mh(r1[i0 * 4 + k], w10) + 4, 65535);
        s = std::min(s + mh(r1[i1 * 4 + k], w11), 65535);
        const int t = std::min(mh(r0[i1 * 4 + k], w01) + mh(r0[i0 * 4 + k], w00), 65535);
        s = std::min(s + t, 65535);
        out[k] = (uint8_t)std::min(s >> 3, 255);
    }
}

inline void mixInto(uint8_t* d, const uint8_t* p, int alpha)
{
    for (int k = 0; k < 4; ++k) d[k] = mix8(d[k], p[k], alpha & 255);
}

// Vfx1.ofx 0x1806c8f10 with filtering off (0x1806ceba0): the source rectangle S into the destination rectangle D
void blit(Img8& dst, const Img8& src, const Rect& D, const Rect& Sr)
{
    const int dw = dst.w, dh = dst.h;
    const Rect S1 = isect(Sr, Rect{0, 0, src.w << 14, src.h << 14});
    if (S1.x1 - S1.x0 < 0x4000 || S1.y1 - S1.y0 < 0x4000) return;
    if (D.x1 - D.x0 < 0x400 || D.y1 - D.y0 < 0x400) return;
    const int scx = dtoi((S1.x1 - S1.x0) * 16384.0 / (D.x1 - D.x0)), scy = dtoi((S1.y1 - S1.y0) * 16384.0 / (D.y1 - D.y0));
    if (scx <= 0 || scy <= 0) return;
    const Rect D1 = isect(D, Rect{0, 0, dw << 14, dh << 14});
    if (D1.x1 - D1.x0 < 0x400 || D1.y1 - D1.y0 < 0x400) return;
    const Rect M = {D.x0, D.y0, dtoi((S1.x1 - S1.x0) * 16384.0 / scx) + D.x0, dtoi((S1.y1 - S1.y0) * 16384.0 / scy) + D.y0};
    const Rect Fr = isect(M, D1);
    const double k14 = 6.103515625e-05;
    const Rect sr = {dtoi((Fr.x0 - D.x0) * (double)scx * k14) + S1.x0, dtoi((Fr.y0 - D.y0) * (double)scy * k14) + S1.y0,
                     dtoi((Fr.x1 - D.x0) * (double)scx * k14) + S1.x0, dtoi((Fr.y1 - D.y0) * (double)scy * k14) + S1.y0};
    const Rect S = isect(sr, S1);
    const int sx = D1.x1 - D1.x0 < 0x8000 ? 0 : std::max(0, dtoi((S.x1 - S.x0 - 0x4000) * 16384.0 / (D1.x1 - D1.x0 - 0x4000)));
    const int sy = D1.y1 - D1.y0 < 0x8000 ? 0 : std::max(0, dtoi((S.y1 - S.y0 - 0x4000) * 16384.0 / (D1.y1 - D1.y0 - 0x4000)));
    const int px0 = fl14(D1.x0), px1 = ce14(D1.x1), py0 = fl14(D1.y0), py1 = ce14(D1.y1);
    int frc = ((py0 + 1) << 14) - D1.y0;
    const int lrc = D1.y1 - (py1 << 14) + 0x4000;
    int fcc = ((px0 + 1) << 14) - D1.x0;
    const int lcc = D1.x1 - (px1 << 14) + 0x4000;
    const int scol = ce14(S.x1), srow = ce14(S.y1);
    if (px1 - px0 < 2) fcc = D1.x1 - D1.x0;
    const int nrows = py1 - py0;
    if (nrows < 2) frc = D1.y1 - D1.y0;
    const int last = scol - 1;
    if (S.x0 < 0 || S.y0 < 0 || scol > src.w || srow > src.h || px0 < 0 || px1 > dw || py0 < 0 || py1 > dh) return;   // (never: both are clipped)
    auto srcRows = [&](int y, const uint8_t*& r0, const uint8_t*& r1, int& fy) {
        const int r = y / 16384;
        fy = y % 16384;
        r0 = src.at(0, r);
        r1 = src.at(0, r + (r != srow - 1 ? 1 : 0));
    };
    auto colOf = [&](int x, int& i0, int& i1, int& fx) {
        i0 = x / 16384;
        fx = x % 16384;
        i1 = i0 + (i0 != last ? 1 : 0);
    };
    // the first and last rows: every pixel mixed in by its coverage (0x1806cf380)
    auto edgeRow = [&](int drow, int y, int rcov) {
        const uint8_t *r0, *r1;
        int fy;
        srcRows(y, r0, r1, fy);
        int x = S.x0;
        int alpha = (dtoi(rcov * (double)fcc * k14) * 255) >> 14;
        uint8_t P[4];
        int i0, i1, fx;
        for (int c = px0; c < px1 - 1; ++c) {
            colOf(x, i0, i1, fx);
            bil(r0, r1, i0, i1, fx, fy, P);
            mixInto(dst.at(c, drow), P, alpha);
            if (c == px0) {
                x += dtoi(sx * (double)fcc * k14);
                alpha = (rcov * 255) >> 14;
            } else x += sx;
        }
        const int n = px1 - px0;
        if (n <= 1) return;
        if (n > 2) x += -sx - dtoi(lcc * (double)sx * -k14);
        colOf(x, i0, i1, fx);
        bil(r0, r1, i0, i1, fx, fy, P);
        mixInto(dst.at(px1 - 1, drow), P, (dtoi(lcc * (double)rcov * k14) * 255) >> 14);
    };
    // the rows between (0x1806cff80)
    auto midRow = [&](int drow, int y) {
        const uint8_t *r0, *r1;
        int fy;
        srcRows(y, r0, r1, fy);
        int x = S.x0;
        uint8_t P[4];
        int i0, i1, fx;
        colOf(x, i0, i1, fx);
        bil(r0, r1, i0, i1, fx, fy, P);
        mixInto(dst.at(px0, drow), P, (fcc * 255) >> 14);
        x += dtoi(fcc * (double)sx * k14);
        const int n = px1 - px0 - 2;
        int c = px0 + 1;
        if (n > 0) {
            for (int k = 0; k < (n & ~1); ++k, ++c, x += sx) {
                const int j0 = x >> 14, f = x & 0x3fff;
                bilSse(r0, r1, j0, j0 + (j0 != last ? 1 : 0), f, fy, dst.at(c, drow));
            }
            if (n & 1) {
                colOf(x, i0, i1, fx);
                bil(r0, r1, i0, i1, fx, fy, dst.at(c, drow));
                x += sx;
                ++c;
            }
        }
        if (px1 - px0 <= 1) return;
        if (px1 - px0 > 2) x += -sx - dtoi(lcc * (double)sx * -k14);
        i0 = x / 16384;
        fx = x % 16384;
        if (i0 == last)
            for (int k = 0; k < 4; ++k) P[k] = (uint8_t)((r0[i0 * 4 + k] * (0x4000 - fy) + r1[i0 * 4 + k] * fy + 0x2000) >> 14);
        else
            bil(r0, r1, i0, i0 + 1, fx, fy, P);
        mixInto(dst.at(px1 - 1, drow), P, (lcc * 255) >> 14);
    };
    edgeRow(py0, S.y0, frc);
    const int fs = dtoi(frc * (double)sy * k14);
    if (py1 - 1 > py0 + 1) {
        const int n = py1 - 1 - (py0 + 1);
        forRows(n, std::max(1, px1 - px0), [&](int k) { midRow(py0 + 1 + k, k * sy + fs + S.y0); });
    }
    if (nrows > 1) {
        int y = (nrows - 2) * sy + fs + S.y0;
        if (nrows > 2) y += -sy - dtoi(lrc * (double)sy * -k14);
        edgeRow(py1 - 1, y, lrc);
    }
}

struct Edge { int a0, a1, pos, flags; };

struct SmoothTable {
    uint8_t v[1024];
    SmoothTable()
    {
        for (int i = 0; i < 1024; ++i) {
            const float x = (float)i * 0.0009765625f;
            const float a = ((2.0f * x) * x) * x, b = (3.0f * x) * x;
            v[i] = (uint8_t)(int)((double)((b - a) * 255.0f) + 0.5);
        }
    }
};

// VEGAS's smoothstep table: 255 (3x^2 - 2x^3) at x = i / 1024, rounded
const uint8_t* smoothTable()
{
    static const SmoothTable t;        // (built once, thread-safe)
    return t.v;
}

// Vfx1.ofx 0x18059b6e0: the border round the edges (border and feather in 18.14 fixed point), colour in the picture's order
void border(Img8& dst, const std::vector<Edge>& edges, int bsize, int feather, const uint8_t col[4])
{
    const uint8_t* tab = smoothTable();
    if ((bsize || feather) && 0x8000 - feather > 0) {
        bsize -= 0x8000 - feather;
        feather = 0x8000;
        if (bsize < 0) bsize = 0;
    }
    const int total = bsize + feather;
    if (edges.empty() || total == 0) return;
    int minx = INT_MAX, maxx = INT_MIN, miny = INT_MAX, maxy = INT_MIN;
    for (const Edge& e : edges) {
        const int ex0 = e.flags & 1 ? e.pos : e.a0, ex1 = e.flags & 1 ? e.pos : e.a1;
        const int ey0 = e.flags & 1 ? e.a0 : e.pos, ey1 = e.flags & 1 ? e.a1 : e.pos;
        minx = std::min(minx, ex0);
        maxx = std::max(maxx, ex1);
        miny = std::min(miny, ey0);
        maxy = std::max(maxy, ey1);
    }
    auto ceilm = [](int v) { return fl14((v | 0x3fff) + 1); };
    const int ys = fl14((miny - total) & ~0x3fff), xs = fl14((minx - total) & ~0x3fff);
    const int iye = fl14((maxy - feather) & ~0x3fff), ixe = fl14((maxx - feather) & ~0x3fff);
    const int ye = ceilm(maxy - 1 + total), xe = ceilm(maxx - 1 + total);
    const int iys = ceilm(miny + feather - 1), ixs = ceilm(minx - 1 + feather);
    const int x0 = std::max(xs, 0), y0 = std::max(ys, 0), x1 = std::min(xe, dst.w), y1 = std::min(ye, dst.h);
    if (x1 <= x0 || y1 <= y0) return;
    forRows(y1 - y0, x1 - x0, [&](int k) {
        const int row = y0 + k;
        const int Y = (row << 14) + 0x2000;
        const bool inner = iys < row && row < iye;
        for (int c = x0; c < x1; ++c) {
            if (inner && ixs < c && c < ixe) continue;
            const int X = (c << 14) + 0x2000;
            bool found = false;
            int dout = total, din = feather;
            for (const Edge& e : edges) {
                int d;
                if (e.flags & 1) {
                    if (!(e.a0 <= Y && Y <= e.a1)) continue;
                    d = e.flags & 2 ? X - e.pos : e.pos - X;
                } else {
                    if (!(e.a0 <= X && X <= e.a1)) continue;
                    d = e.flags & 2 ? Y - e.pos : e.pos - Y;
                }
                if (d >= 0) {
                    if (d < dout) { dout = d; found = true; }
                } else if (-d < din) {
                    din = -d;
                    found = true;
                }
            }
            if (!found)
                for (const Edge& e : edges) {
                    if (e.flags & 1) continue;
                    int ea = e.a0 - X;
                    if (ea < 0) ea = dout;
                    int ec = X - e.a1;
                    if (ec < 0) ec = dout;
                    const int dx = std::min(ea, ec);
                    if (dx < 0 || dx >= dout) continue;
                    const int dy = e.flags & 2 ? Y - e.pos : e.pos - Y;
                    if (dy <= 0 || dy >= dout) continue;
                    const int dd = dtoi(std::sqrt((double)dx * dx + (double)dy * dy) + 0.5);
                    if (dd < dout) { dout = dd; found = true; }
                }
            if (!found) continue;
            uint8_t* px = dst.at(c, row);
            if (dout < bsize) { std::memcpy(px, col, 4); continue; }
            int al = 0;
            if (feather != 0) {
                const int dd = std::min(din, dout - bsize);
                al = (~tab[std::min(1023, std::max(0, dtoi(dd * 1023.0 / feather)))]) & 255;
                if (al == 255) { std::memcpy(px, col, 4); continue; }
            }
            for (int q = 0; q < 4; ++q) {
                int v = (col[q] - px[q]) * al;
                if (v < 0) v += 255;
                px[q] = (uint8_t)(px[q] + (v >> 8));
            }
        }
    });
}

void colourBytes(const Color& bc, uint8_t col[4])
{
    col[0] = (uint8_t)dtoi((double)((float)bc.r * 255.f) + 0.5);
    col[1] = (uint8_t)dtoi((double)((float)bc.g * 255.f) + 0.5);
    col[2] = (uint8_t)dtoi((double)((float)bc.b * 255.f) + 0.5);
    col[3] = 255;
}

// Spiral and Slide (0x180603700 and 0x1805fd6a0): outgoing frame drawn whole, incoming shrunk by the zoom and carried in along
// the spiral (or straight along the angle), then the border.
void moveIn(Ctx& c, double angleDeg, double zoom, bool slide)
{
    Img8 A = toBytesTop(c.src), B = toBytesTop(c.src2);
    const int w = A.w, h = A.h;
    const double p = (double)(float)c.progress;
    const Img8* src0 = &A;
    const Img8* src1 = &B;
    double t = p;
    if (c.i("Direction") != 0) {
        t = 1.0 - p;
        std::swap(src0, src1);
    }
    t = 0.0 > t ? 0.0 : std::min(1.0, t);
    Img8 dst;
    dst.w = w;
    dst.h = h;
    dst.px.assign((size_t)w * h * 4, 0);
    const Rect full = {0, 0, w << 14, h << 14};
    blit(dst, *src0, full, full);
    const int W = w << 14, H = h << 14, mn = std::min(W, H), mx = std::max(W, H);
    int bsz = dtoi((double)((float)mn * (float)c.d("BorderSize")) + 0.5), feather = dtoi((double)((float)mn * (float)c.d("BorderFeather")) + 0.5);
    if ((bsz || feather) && 0x8000 - feather > 0) {
        bsz -= 0x8000 - feather;
        feather = 0x8000;
        if (bsz < 0) bsz = 0;
    }
    const int r10 = -(W / 2), edx = -(H / 2);
    const double x1 = (double)feather / mx;
    const double s = (x1 + 1.0) * (1.0 - t) - x1;
    const double z = (zoom - 1.0) * s + 1.0;
    Rect rect = full;
    int e0, e1, e2, e3;
    if (s >= 0) {
        rect.x1 = dtoi((rect.x1 - rect.x0) * z) + rect.x0;
        rect.y1 = dtoi((rect.y1 - rect.y0) * z) + rect.y0;
        const double a = angleDeg * 0.017453292519943;
        const double sa = std::sin(a), ca = std::cos(a);
        const double k = slide ? s / std::max(std::fabs(sa), std::fabs(ca)) : s;
        const double ax = (r10 - (W * z * 0.5 + bsz + feather)) * k, ay = (edx - (H * z * 0.5 + bsz + feather)) * k;
        const int bx = dtoi(ca * ax + 0.5), by = dtoi(sa * ay + 0.5);
        const int dx = dtoi((1.0 - z) * W * 0.5 + 0.5) + bx, dy = dtoi((1.0 - z) * H * 0.5 + 0.5) + by;
        rect = Rect{rect.x0 + dx, rect.y0 + dy, rect.x1 + dx, rect.y1 + dy};
        e0 = rect.x0; e1 = rect.y0; e2 = rect.x1; e3 = rect.y1;
    } else {
        const int e = dtoi(-(s / x1) * feather);
        e0 = rect.x0 - e; e2 = rect.x1 + e; e1 = rect.y0 - e; e3 = rect.y1 + e;
    }
    blit(dst, *src1, rect, full);
    uint8_t col[4];
    colourBytes(c.color("BorderColor"), col);
    border(dst, {{e1, e3, e2, 3}, {e1, e3, e0, 1}, {e0, e2, e1, 0}, {e0, e2, e3, 2}}, bsz, feather, col);
    storeTop(c, dst);
}

}  // namespace sf

// Spiral: the incoming frame (outgoing with Spiral out) shrunk by Zoom comes in along a spiral of Turns turns from the Orientation side.
void spiral(Ctx& c)
{
    const double p = (double)(float)c.progress;
    static const double orient[4] = {0.0, 90.0, 180.0, 270.0};
    const double ang = p * c.d("Turns") * (c.i("Motion") ? -360.0 : 360.0) + orient[std::min(3, std::max(0, c.i("Orientation")))];
    sf::moveIn(c, ang, c.d("Zoom") * 0.01, false);
}

// Slide: the incoming frame (outgoing with Out) slides in along Angle.
void slideFx(Ctx& c) { sf::moveIn(c, (double)(float)c.d("Angle"), 1.0, true); }

// Split: one frame cut into four quadrants at Center that move apart (Push), are cropped (Wipe) or squeezed (Squeeze) over the other,
// with a border on the two inner edges (Vfx1.ofx 0x180609e60).
void splitFx(Ctx& c)
{
    using namespace sf;
    Img8 A = toBytesTop(c.src), B = toBytesTop(c.src2);
    const int w = A.w, h = A.h;
    const double p = (double)(float)c.progress;
    double t = 0.0 > p ? 0.0 : std::min(1.0, p);
    const Img8* first = &A;
    const Img8* other = &B;
    if (c.i("Direction") == 0) {
        t = 1.0 - t;
        std::swap(first, other);
    }
    double cxd, cyd;
    c.xy("Center", cxd, cyd);
    const float cx = (float)cxd, cy = 1.0f - (float)cyd;
    const int W = w << 14, H = h << 14, mn = std::min(W, H);
    int bsz = dtoi((double)((float)mn * (float)c.d("BorderSize") * 0.5f) + 0.5);
    int feather = dtoi((double)((float)mn * (float)c.d("BorderFeather") * 0.5f) + 0.5);
    const double x3 = (double)feather / std::max(W, H);
    const double x2 = (x3 + 1.0) * t - x3;
    int ox, oy, ob;
    if (x2 >= 0) {
        ox = dtoi(W * x2);
        oy = dtoi(H * x2);
        ob = dtoi((feather + bsz) * x2);
    } else {
        ox = oy = ob = 0;
        feather = dtoi((1.0 - (-(x2 / x3))) * feather);
    }
    ox = std::max(ox, 0);
    oy = std::max(oy, 0);
    ob = std::max(ob, 0);
    if (bsz < 0) bsz = 0;
    else {
        int c1 = (ox / 2 + ob) * 2 - 1, c2 = (oy / 2 + ob) * 2 - 1;
        if (c1 <= 0) c1 = 0;
        if (c2 <= 0) c2 = 0;
        bsz = std::min(bsz, c1 < c2 ? c1 : c2);
    }
    Img8 dst;
    dst.w = w;
    dst.h = h;
    dst.px.assign((size_t)w * h * 4, 0);
    const Rect full = {0, 0, W, H};
    if (-x3 >= x2) {
        blit(dst, *first, full, full);
        storeTop(c, dst);
        return;
    }
    blit(dst, *other, full, full);
    uint8_t col[4];
    colourBytes(c.color("BorderColor"), col);
    const int mode = c.i("SplitMode");
    const int cxw = dtoi((double)((float)W * cx)), chh = dtoi((double)((float)H * cy));
    const float x7 = (float)ob;
    auto quad = [&](Rect s, int dx0, int dy0, int dx1, int dy1, bool rightEdge, bool bottomEdge) {
        Rect d;
        if (mode == 0) d = Rect{s.x0 + dx0 + dx1, s.y0 + dy0 + dy1, s.x1 + dx0 + dx1, s.y1 + dy0 + dy1};
        else d = Rect{s.x0 + dx0, s.y0 + dy0, s.x1 + dx1, s.y1 + dy1};
        if (mode == 1) s = d;
        blit(dst, *first, d, s);
        std::vector<Edge> e;
        e.push_back(rightEdge ? Edge{d.y0, d.y1, d.x1, 3} : Edge{d.y0, d.y1, d.x0, 1});
        e.push_back(bottomEdge ? Edge{d.x0, d.x1, d.y1, 2} : Edge{d.x0, d.x1, d.y0, 0});
        border(dst, e, bsz, feather, col);
    };
    const int r13 = dtoi((double)((float)(-ox) * cx - x7)), eb = dtoi((double)((float)(-oy) * cy - x7));
    const int es = dtoi((double)((1.0f - cx) * (float)ox + x7)), eb2 = dtoi((double)((1.0f - cy) * (float)oy + x7));
    quad(Rect{0, 0, cxw, chh}, 0, 0, r13, eb, true, true);
    quad(Rect{cxw, 0, W, chh}, es, 0, 0, eb, false, true);
    quad(Rect{0, chh, cxw, H}, 0, eb2, r13, 0, true, false);
    quad(Rect{cxw, chh, W, H}, es, eb2, 0, 0, false, false);
    storeTop(c, dst);
}

}  // namespace

void addTransitions(std::vector<Def>& out)
{
    struct Row { const char* id; void (*fn)(Ctx&); };
    const std::string P = "com.vegascreativesoftware:";
    const Row rows[] = {
        {"dissolve", dissolve}, {"crosseffect", crossEffect}, {"clockwipe", clockWipe}, {"linearwipe", linearWipe}, {"barndoor", barndoor}, {"iris", irisFx},
        {"push", pushFx}, {"slide", slideFx}, {"split", splitFx}, {"swap", swapFx}, {"squeeze", squeezeFx}, {"spiral", spiral}, {"starwipe", starWipe},
        {"venetianblinds", venetian}, {"zoom", zoomFx}, {"flash", flashFx}, {"portals", portalsFx}, {"3dblinds", blinds3d}, {"3dcascade", cascade3d},
        {"3dflyinout", fly3d}, {"3dshuffle", shuffle3d}, {"pageloop", pageLoop}, {"pagepeel", pagePeel}, {"pageroll", pageRoll},
    };
    for (const Row& r : rows) {
        Def d = vfxDef(P + r.id, r.fn);
        if (d.id.empty()) continue;
        d.transition = true;
        if (d.id == P + "iris")
            for (PDef& pd : d.params)
                if (pd.name == "RepeatX" || pd.name == "RepeatY") { pd.dmin = pd.min; pd.dmax = pd.max; pd.max = 64; }
        out.push_back(d);
    }
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
