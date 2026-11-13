// Built-in reproductions of four VEGAS Pro 18 effects that are common in projects, with VEGAS's identifiers and parameter names (read from .veg files, VEGAS's resource and
// preset files and the plug-ins' strings) so projects load with their saved settings and save them back as they were:
//
//   com.vegascreativesoftware:colorcurves        Color Curves: red, green, blue and master curves. "Property" holds the curves' points and Bezier handles as VEGAS wrote them (decoded
//                                                 from its presets and projects: see readCurves).
//   de.magix:stereographicProjectionOCL          Tiny Planet: the frame taken as a 360 degree equirectangular one and shown as a stereographic projection (Latitude, Longitude, Z).
//   de.magix:MeshWarp                            Mesh Warp: a grid of points (WarpData: columns, rows, then x y pairs as fractions of the frame) that bends the frame.
//   com.sonycreativesoftware:smart_upscale       Smart Upscale (SPICA): Reno only sharpens, since the frame stays the project's size.
//
// Guessed (VEGAS's maths are not published): which of the four curves is the master and that it works after the channels' curves, the direction and unit of Tiny Planet's Z (larger =
// nearer to the planet), which way up Mesh Warp's y runs, and everything Smart Upscale does. All are correct at any render scale (Ctx::scale), thread safe and work row by row on all cores.
#include "../core/rmath.h"
#include "builtins_impl.h"
#include "builtins_mask_internal.h"
#include "vegascurves.h"
#include "vkeffects.h"
#include "builtins_magixdata.h"
#include "../core/base.h"
#include "vegas8.h"
#include "vfxtinyplanet.h"
#include "builtins_more_internal.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
namespace {

const char* const kGroup = "Reno Built-in";

Def make(const char* id, const char* label, const char* what, std::vector<PDef> params, void (*fn)(Ctx&))
{
    Def d;
    d.id = id;
    d.label = label;
    d.group = kGroup;
    d.description = std::string(what) + " (Reno's own, standing in for VEGAS " + label + ").";
    d.params = std::move(params);
    d.render = fn;
    return d;
}

inline void copyFrame(Ctx& c)
{
    if (c.out.px.size() == c.src.px.size()) c.out.px = c.src.px;
}

// ------------------------------------------------------------------ Color Curves
// "Property" is base64 of: an int 1, the point count of each of the four curves (red, green, blue, master) as four ints, then per point 104 bytes: x and y (doubles, 0..255), 8 bytes of flags,
// the incoming handle (four doubles: share of the gap to the previous point, slope, offset dx, dy; an int) and the outgoing one (same). Between points: cubic Bezier with control points at
// point + out-handle and next point + in-handle.
struct CurvePoint {
    double x = 0, y = 0, inx = 0, iny = 0, outx = 0, outy = 0;
    unsigned char inFlag = 0, outFlag = 0;    // the second byte of each handle's trailing int (VEGAS's special cases for flat / step handles)
};

bool readCurves(const std::string& text, std::vector<CurvePoint> curves[4])
{
    const std::string b = base64Decode(text);
    if (b.size() < 20) return false;
    auto i32 = [&](size_t o) { int32_t v; std::memcpy(&v, b.data() + o, 4); return v; };
    auto f64 = [&](size_t o) { double v; std::memcpy(&v, b.data() + o, 8); return v; };
    int n[4];
    size_t total = 0;
    for (int k = 0; k < 4; ++k) {
        n[k] = i32(4 + 4 * (size_t)k);
        if (n[k] < 0 || n[k] > 1000) return false;
        total += (size_t)n[k];
    }
    if (b.size() < 20 + total * 104) return false;
    size_t o = 20;
    for (int k = 0; k < 4; ++k) {
        curves[k].clear();
        for (int i = 0; i < n[k]; ++i, o += 104) {
            CurvePoint p;
            p.x = f64(o); p.y = f64(o + 8);
            p.inx = f64(o + 40); p.iny = f64(o + 48);
            p.outx = f64(o + 80); p.outy = f64(o + 88);
            p.inFlag = (unsigned char)b[o + 57]; p.outFlag = (unsigned char)b[o + 97];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.inx) || !std::isfinite(p.iny) ||
                !std::isfinite(p.outx) || !std::isfinite(p.outy))
                return false;
            curves[k].push_back(p);
        }
    }
    return true;
}

constexpr int kLut = 1024;

// A curve as a table of its output (0..1) over the input 0..1.
void curveTable(const std::vector<CurvePoint>& pts, float* lut)
{
    const double step = 255.0 / (kLut - 1);
    std::vector<double> t((size_t)kLut, NAN);
    if (pts.empty()) {
        for (int i = 0; i < kLut; ++i) t[(size_t)i] = i * step;
    } else if (pts.size() == 1) {
        for (int i = 0; i < kLut; ++i) t[(size_t)i] = pts[0].y;
    } else {
        const int S = 96;       // steps along each segment
        for (size_t s = 0; s + 1 < pts.size(); ++s) {
            const CurvePoint &a = pts[s], &b = pts[s + 1];
            const double px[4] = {a.x, a.x + a.outx, b.x + b.inx, b.x}, py[4] = {a.y, a.y + a.outy, b.y + b.iny, b.y};
            double x0 = a.x, y0 = a.y;
            for (int k = 1; k <= S; ++k) {
                const double u = (double)k / S, v = 1 - u;
                const double w0 = v * v * v, w1 = 3 * v * v * u, w2 = 3 * v * u * u, w3 = u * u * u;
                const double x1 = w0 * px[0] + w1 * px[1] + w2 * px[2] + w3 * px[3], y1 = w0 * py[0] + w1 * py[1] + w2 * py[2] + w3 * py[3];
                if (x1 != x0) {
                    const double lo = std::min(x0, x1), hi = std::max(x0, x1);
                    const int k0 = std::max(0, (int)std::ceil(lo / step - 1e-9)), k1 = std::min(kLut - 1, (int)std::floor(hi / step + 1e-9));
                    for (int i = k0; i <= k1; ++i) t[(size_t)i] = y0 + (y1 - y0) * (i * step - x0) / (x1 - x0);
                }
                x0 = x1; y0 = y1;
            }
        }
        const double first = pts.front().x, last = pts.back().x;
        for (int i = 0; i < kLut; ++i) {
            const double x = i * step;
            if (x < first) t[(size_t)i] = pts.front().y;
            else if (x > last) t[(size_t)i] = pts.back().y;
        }
        int prev = -1;
        for (int i = 0; i < kLut; ++i) {
            if (std::isnan(t[(size_t)i])) continue;
            if (prev >= 0 && i - prev > 1)
                for (int j = prev + 1; j < i; ++j) t[(size_t)j] = t[(size_t)prev] + (t[(size_t)i] - t[(size_t)prev]) * (j - prev) / (i - prev);
            prev = i;
        }
        for (int i = 0; i < kLut; ++i) if (std::isnan(t[(size_t)i])) t[(size_t)i] = i * step;
    }
    for (int i = 0; i < kLut; ++i) lut[i] = (float)clampd(t[(size_t)i] / 255.0, 0.0, 1.0);
}

inline float look(const float* lut, float v)
{
    if (!(v > 0.f)) return lut[0];
    if (v >= 1.f) return lut[kLut - 1];
    const float f = v * (kLut - 1);
    const int i = (int)f;
    return lut[i] + (lut[i + 1] - lut[i]) * (f - (float)i);
}

// The 8-bit table VEGAS builds for a curve (one value per input level 0..255), transcribed from its curve sampler: each Bezier segment is cut into
// max(3, (|dx| + |dy|) / 2) pieces (parameters in float, Bernstein weights with pow), each level is interpolated linearly inside the piece that holds it;
// levels outside the first / last point stay as they are; values go through round() and are held to 0..255.
bool curveBytes8(const std::vector<CurvePoint>& P, unsigned char out[256])
{
    const int n = (int)P.size();
    if (n < 2) return false;
    double val[256];
    int flag[256];
    size_t ai = 0, bi = 1, next = 2;
    double s = 0.0;
    int i = 0;
    const double binom[4] = {1.0, 3.0, 3.0, 1.0};
    while (i < 256) {
        flag[i] = 1;
        const CurvePoint& a = P[ai];
        const CurvePoint& b = P[bi];
        if (a.x > s) { s += 1.0; ++i; continue; }
        if (s == a.x) { val[i] = a.y; flag[i] = 0; s += 1.0; ++i; continue; }
        if (!(b.x > s)) {
            if (b.x + 1e-8 > s) { val[i] = b.y; flag[i] = 0; s += 1.0; ++i; continue; }
            if ((int)next >= n) {
                for (int j = i; j < 256; ++j) flag[j] = 1;
                break;
            }
            ai = bi;
            bi = next++;
            continue;
        }
        int fl = 1;
        if (a.outFlag) {
            if (b.inFlag) {
                const double ay = a.outy;
                if (ay * b.iny < 1.0) {
                    const double mid = (b.x + a.x) * 0.5;
                    if (mid > s) fl = 2 + (0.0 > ay ? 1 : 0);
                    else if (s > mid) fl = 2 + (0.0 > a.iny ? 1 : 0);
                    else { val[i] = (b.y + a.y) * 0.5; fl = 0; }
                } else fl = 2 + (0.0 > ay ? 1 : 0);
            } else fl = 2 + (0.0 > a.outy ? 1 : 0);
        } else if (b.inFlag) fl = 2 + (0.0 > b.iny ? 1 : 0);
        if (fl == 3) { val[i] = 0.0; fl = 0; }
        else if (fl == 2) { val[i] = 255.0; fl = 0; }
        flag[i] = fl;
        if (fl != 1) { s += 1.0; ++i; continue; }
        const double X[4] = {a.x, a.x + a.outx, b.x + b.inx, b.x};
        const double Y[4] = {a.y, a.y + a.outy, b.y + b.iny, b.y};
        const double dist = std::fabs(a.y - b.y) + std::fabs(a.x - b.x);
        unsigned pieces = (unsigned)(int64_t)dist >> 1;
        if (pieces < 3u) pieces = 3u;
        const int saved = i;
        for (unsigned k = 0; k < pieces && i < 256; ++k) {
            const float t0 = (float)k / (float)pieces, t1 = (float)(k + 1) / (float)pieces;
            const float o0 = 1.0f - t0, o1 = 1.0f - t1;
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            for (int j = 0; j < 4; ++j) {
                const double w0 = rm::pow((double)t0, (double)j) * rm::pow((double)o0, (double)(3 - j)) * binom[j];
                x0 += X[j] * w0;
                y0 += Y[j] * w0;
                const double w1 = rm::pow((double)t1, (double)j) * rm::pow((double)o1, (double)(3 - j)) * binom[j];
                y1 += Y[j] * w1;
                x1 += X[j] * w1;
            }
            if (s < x0) continue;
            while (!(x1 < s)) {
                double v = ((y1 - y0) / (x1 - x0)) * (s - x0) + y0;
                if (0.0 > v) v = 0.0;
                else if (v > 255.0) v = 255.0;
                val[i] = v;
                flag[i] = 0;
                s += 1.0;
                ++i;
                if (i >= 256) break;
                flag[i] = 1;
                if (!(s >= x0)) break;
            }
        }
        if (i == saved && i < 256) { val[i] = b.y; flag[i] = 0; s += 1.0; ++i; }
    }
    for (int j = 0; j < 256; ++j) {
        if (flag[j]) { out[j] = (unsigned char)j; continue; }
        const double r = std::round(val[j] * 256.0 * 0.00390625);
        out[j] = r < 0.0 ? 0 : (unsigned char)(int)std::min(255.0, std::round(val[j] * 256.0 * 0.00390625));
    }
    return true;
}

// The row function (see Def::rows): the three tables with the master curve folded in, then each pixel's straight colour looked up.
struct CurveTables { std::vector<float> ch[3]; };

bool colorCurvesRows(Ctx& c, RowFn& fn)
{
    std::vector<CurvePoint> curves[4];
    if (!readCurves(c.s("Property"), curves)) {
        fn = [](const float* s, float* o, int n, int) { std::memcpy(o, s, (size_t)n * 4 * sizeof(float)); };
        return true;
    }
    if (c.levels) {
        // 8-bit: three byte tables (the master curve folded in), applied to the premultiplied bytes and held to the alpha.
        auto lut = std::make_shared<std::array<unsigned char, 768>>();
        unsigned char m[256], t[256];
        const bool haveM = curveBytes8(curves[3], m);
        for (int k = 0; k < 3; ++k) {
            const bool ok = curveBytes8(curves[k], t);
            for (int v = 0; v < 256; ++v) {
                unsigned char r = ok ? t[v] : (unsigned char)v;
                if (haveM) r = m[r];
                (*lut)[(size_t)k * 256 + (size_t)v] = r;
            }
        }
        fn = [lut](const float* sp, float* o, int n, int) {
            uint8_t lv[4 * 1024];
            for (int base = 0; base < n; base += 1024) {
                const int m = std::min(1024, n - base);
                v8::levelsRow(sp, lv, (size_t)m * 4);
                for (int x = 0; x < m; ++x, sp += 4, o += 4) {
                    const int a = lv[x * 4 + 3];
                    for (int k = 0; k < 3; ++k) {
                        const int r = (*lut)[(size_t)k * 256 + lv[x * 4 + k]];
                        o[k] = v8::fromLevel(r < a ? r : a);
                    }
                    o[3] = sp[3];
                }
            }
        };
        return true;
    }
    std::vector<float> master((size_t)kLut);
    auto tables = std::make_shared<CurveTables>();
    curveTable(curves[3], master.data());
    for (int k = 0; k < 3; ++k) {
        tables->ch[k].resize((size_t)kLut);
        curveTable(curves[k], tables->ch[k].data());
        for (int i = 0; i < kLut; ++i) tables->ch[k][(size_t)i] = look(master.data(), tables->ch[k][(size_t)i]);
    }
    fn = [tables](const float* s, float* o, int n, int) {
        const float* t0 = tables->ch[0].data();
        const float* t1 = tables->ch[1].data();
        const float* t2 = tables->ch[2].data();
        for (int x = 0; x < n; ++x, s += 4, o += 4) {
            const float a = s[3];
            if (!(a > 1e-6f)) { o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = a; continue; }
            const float inv = 1.f / a;
            o[0] = look(t0, s[0] * inv) * a;
            o[1] = look(t1, s[1] * inv) * a;
            o[2] = look(t2, s[2] * inv) * a;
            o[3] = a;
        }
    };
    return true;
}

void colorCurves(Ctx& c) { runRows(c, colorCurvesRows); }

// Calculate (DoAnalysis) in Auto mode 1, Color Levels: red, green and blue each stretched so the darkest and brightest half percent of the
// frame at the cursor reach black and white (a point at each end of the stretch on that channel's curve); the master curve is kept.
void colorCurvesChanged(Change& ch)
{
    if (ch.param != "DoAnalysis" || !ch.b("DoAnalysis")) return;
    ch.setI("DoAnalysis", 0);
    Buf frame;
    if (ch.i("AutoMode") != 1 || !ch.source(ch.time, frame)) return;
    double bins[3][256] = {};
    double total = 0;
    for (size_t i = 0; i + 3 < frame.px.size(); i += 4) {
        const float a = frame.px[i + 3];
        if (!(a > 1e-4f)) continue;
        for (int k = 0; k < 3; ++k) bins[k][(int)(clampd(frame.px[i + (size_t)k] / a, 0, 1) * 255.0 + 0.5)] += 1;
        total += 1;
    }
    if (total < 1) return;
    BezCurve curves[4];
    if (!decodeColorCurves(ch.s("Property"), curves))
        for (int k = 0; k < 4; ++k) curves[k] = bezStraight(255);
    const double cut = 0.005 * total;
    for (int k = 0; k < 3; ++k) {
        double acc = 0;
        int lo = 0, hi = 255;
        for (; lo < 255; ++lo) { acc += bins[k][lo]; if (acc > cut) break; }
        acc = 0;
        for (; hi > 0; --hi) { acc += bins[k][hi]; if (acc > cut) break; }
        BezCurve c = bezStraight(255);
        if (hi - lo >= 8 && (lo > 0 || hi < 255)) {
            if (lo > 0) bezInsert(c, lo, 0, 255);
            if (hi < 255) bezInsert(c, hi, 255, 255);
            bezNormalise(c);
        }
        curves[k] = c;
    }
    ch.setS("Property", encodeColorCurves(curves));
}

// ------------------------------------------------------------------ Tiny Planet (stereographic projection)
// VEGAS's own, from its OpenCL kernel (vfxtinyplanet.h, float like the kernel): the source is a whole 360 x 180 degree equirectangular frame. Output pixel (x, y) (rows counted from the
// bottom) is at angle 2 atan(r / (2 W / Z)) from the view's axis, tilted by Latitude - 90 degrees and turned by Longitude; that gives the source row (a fraction) and column (cut to a
// whole number). The kernel reads the row with the card's linear filter (the bilinear mode VEGAS's pictures show), so the 8-bit frame gets the float result with exact halves down
// (vegasDrawsFloat).
void tinyPlanet(Ctx& c)
{
    const int W = c.out.w, H = c.out.h, SW = c.src.w, SH = c.src.h;
    if (W <= 0 || H <= 0 || SW <= 0 || SH <= 0) return;
    vftp::Params p;
    p.w = SW; p.h = SH;
    p.latitude = (float)std::fmod(c.d("Latitude"), 360.0);
    p.longitude = (float)std::fmod(c.d("Longitude"), 360.0);
    p.z = (float)c.d("Z");
    std::vector<float> img((size_t)SW * SH * 4);        // rows bottom first, as the kernel reads them
    for (int yb = 0; yb < SH; ++yb) std::memcpy(&img[(size_t)yb * SW * 4], c.src.at(0, SH - 1 - yb), sizeof(float) * 4 * (size_t)SW);
    forRows(H, W, [&](int yt) {
        const int y = H - 1 - yt;
        for (int x = 0; x < W; ++x) {
            float sx, sy;
            vftp::sourcePos(p, x, y, sx, sy);
            if (!std::isfinite(sx)) sx = 0.f;       // (the middle pixel divides 0 by 0; VEGAS's picture shows the first row there)
            if (!std::isfinite(sy)) sy = 0.f;
            more::oclLinear4(img, SW, SH, sx, sy, c.out.at(x, yt));
        }
    });
}

// ------------------------------------------------------------------ Mesh Warp
// "WarpData" is base64 of: four ints (columns, rows, the number of points, 0), then every point (row by row, top row first) as two floats: where it is, as fractions of the frame.
// Mesh point refers to Target: the frame at the mesh's regular position is moved to the point; Source: the point says where in the frame the regular position takes its colour.
struct Mesh {
    int cols = 0, rows = 0;
    std::vector<float> pt;      // x, y per point
    bool identity = true;
};

bool readMesh(const std::string& text, Mesh& m)
{
    const std::string b = base64Decode(text);
    if (b.size() < 16) return false;
    int32_t hdr[4];
    std::memcpy(hdr, b.data(), 16);
    if (hdr[0] < 2 || hdr[1] < 2 || hdr[0] > 200 || hdr[1] > 200) return false;
    const size_t n = (size_t)hdr[0] * (size_t)hdr[1];
    if (b.size() < 16 + n * 8) return false;
    m.cols = hdr[0];
    m.rows = hdr[1];
    m.pt.resize(n * 2);
    std::memcpy(m.pt.data(), b.data() + 16, n * 8);
    m.identity = true;
    for (int j = 0; j < m.rows; ++j)
        for (int i = 0; i < m.cols; ++i) {
            float* p = &m.pt[((size_t)j * m.cols + i) * 2];
            if (!std::isfinite(p[0]) || !std::isfinite(p[1])) return false;
            p[0] = std::min(std::max(p[0], -8.f), 9.f);
            p[1] = std::min(std::max(p[1], -8.f), 9.f);
            if (std::fabs(p[0] - (float)i / (m.cols - 1)) > 1e-4f || std::fabs(p[1] - (float)j / (m.rows - 1)) > 1e-4f) m.identity = false;
        }
    return true;
}

void meshWarp(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    Mesh m;
    if (!c.b("EnableWarp") || !readMesh(c.s("WarpData"), m) || m.identity) { copyFrame(c); return; }
    const int cols = m.cols, rows = m.rows;
    if (c.i("WarpDirection") == 0) {
        struct Col { int i; double fx, omfx; };
        std::vector<Col> col((size_t)W);
        for (int x = 0; x < W; ++x) {
            const double u = clampd((x + 0.5) / W, 0, 1) * (cols - 1);
            const int i = std::min(cols - 2, (int)u);
            const double fx = u - i;
            col[(size_t)x] = Col{i, fx, 1 - fx};
        }
        parallelRows(H, (size_t)W * 8, [&](int first, int end) {
            std::vector<double> tab((size_t)W * 4);
            int tabJ = -1;
            for (int y = first; y < end; ++y) {
                const double v = clampd((y + 0.5) / H, 0, 1) * (rows - 1);
                const int j = std::min(rows - 2, (int)v);
                const double fy = v - j;
                const double omfy = 1 - fy;
                if (j != tabJ) {
                    tabJ = j;
                    for (int x = 0; x < W; ++x) {
                        const Col& cl = col[(size_t)x];
                        const float *a = &m.pt[((size_t)j * cols + cl.i) * 2], *b = a + 2, *cc = a + (size_t)cols * 2, *d = cc + 2;
                        double* t = &tab[(size_t)x * 4];
                        t[0] = a[0] * cl.omfx + b[0] * cl.fx;
                        t[1] = cc[0] * cl.omfx + d[0] * cl.fx;
                        t[2] = a[1] * cl.omfx + b[1] * cl.fx;
                        t[3] = cc[1] * cl.omfx + d[1] * cl.fx;
                    }
                }
                for (int x = 0; x < W; ++x) {
                    const double* t = &tab[(size_t)x * 4];
                    const double sx = t[0] * omfy + t[1] * fy;
                    const double sy = t[2] * omfy + t[3] * fy;
                    c.src.sampleClamped(sx * W, sy * H, c.out.at(x, y));
                }
            }
        });
        return;
    }
    struct Tri { double x[3], y[3], sx[3], sy[3], den; int y0, y1; double a0, a1, a2, a3, xmin, xmax; };
    std::vector<Tri> tris;
    tris.reserve((size_t)(cols - 1) * (rows - 1) * 2);
    auto add = [&](int a0, int a1, int a2) {
        Tri t;
        const int id[3] = {a0, a1, a2};
        for (int k = 0; k < 3; ++k) {
            const int i = id[k] % cols, j = id[k] / cols;
            t.x[k] = m.pt[(size_t)id[k] * 2] * W;
            t.y[k] = m.pt[(size_t)id[k] * 2 + 1] * H;
            t.sx[k] = (double)i / (cols - 1) * W;
            t.sy[k] = (double)j / (rows - 1) * H;
        }
        t.den = (t.y[1] - t.y[2]) * (t.x[0] - t.x[2]) + (t.x[2] - t.x[1]) * (t.y[0] - t.y[2]);
        if (std::fabs(t.den) < 1e-9) return;
        t.y0 = (int)std::floor(std::min(t.y[0], std::min(t.y[1], t.y[2])) - 0.5);
        t.y1 = (int)std::ceil(std::max(t.y[0], std::max(t.y[1], t.y[2])) - 0.5);
        if (t.y1 < 0 || t.y0 >= H) return;
        // (the parts of the barycentric weights that do not depend on the pixel)
        t.a0 = t.y[1] - t.y[2];
        t.a1 = t.x[2] - t.x[1];
        t.a2 = t.y[2] - t.y[0];
        t.a3 = t.x[0] - t.x[2];
        t.xmin = std::min(t.x[0], std::min(t.x[1], t.x[2]));
        t.xmax = std::max(t.x[0], std::max(t.x[1], t.x[2]));
        tris.push_back(t);
    };
    for (int j = 0; j + 1 < rows; ++j)
        for (int i = 0; i + 1 < cols; ++i) {
            const int a = j * cols + i, b = a + 1, cc = a + cols, d = cc + 1;
            add(a, b, cc);
            add(b, d, cc);
        }
    std::vector<std::vector<int>> perRow((size_t)H);
    for (size_t k = 0; k < tris.size(); ++k)
        for (int y = std::max(0, tris[k].y0); y <= std::min(H - 1, tris[k].y1); ++y) perRow[(size_t)y].push_back((int)k);
    forRows(H, W, [&](int y) {
        float* row = c.out.at(0, y);
        std::memset(row, 0, sizeof(float) * 4 * (size_t)W);
        const double yc = y + 0.5;
        for (int k : perRow[(size_t)y]) {
            const Tri& t = tris[(size_t)k];
            const int xa = std::max(0, (int)std::floor(t.xmin - 0.5)), xb = std::min(W - 1, (int)std::ceil(t.xmax - 0.5));
            const double eps = -1e-7;
            const double dy = yc - t.y[2], r0 = t.a1 * dy, r1 = t.a3 * dy;
            for (int x = xa; x <= xb; ++x) {
                const double dx = (x + 0.5) - t.x[2];
                const double w0 = (t.a0 * dx + r0) / t.den;
                const double w1 = (t.a2 * dx + r1) / t.den;
                const double w2 = 1 - w0 - w1;
                if (w0 < eps || w1 < eps || w2 < eps) continue;
                c.src.sampleClamped(w0 * t.sx[0] + w1 * t.sx[1] + w2 * t.sx[2], w0 * t.sy[0] + w1 * t.sy[1] + w2 * t.sy[2], row + (size_t)x * 4);
            }
        }
    });
}

// The mesh as WarpData's text (readMesh's format).
std::string writeMesh(const Mesh& m)
{
    std::string b(16 + m.pt.size() * 4, '\0');
    const int32_t hdr[4] = {m.cols, m.rows, m.cols * m.rows, 0};
    std::memcpy(&b[0], hdr, 16);
    if (!m.pt.empty()) std::memcpy(&b[16], m.pt.data(), m.pt.size() * 4);
    return base64Encode(b.data(), b.size());
}

// A regular mesh of cols x rows points, or the given mesh sampled at that many places (the warp kept, on a finer or coarser grid).
Mesh gridOf(int cols, int rows, const Mesh* from)
{
    Mesh m;
    m.cols = cols;
    m.rows = rows;
    m.pt.resize((size_t)cols * rows * 2);
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols; ++i) {
            const double fx = (double)i / (cols - 1), fy = (double)j / (rows - 1);
            double x = fx, y = fy;
            if (from && from->cols >= 2 && from->rows >= 2) {
                const double u = fx * (from->cols - 1), v = fy * (from->rows - 1);
                const int i0 = std::min(from->cols - 2, (int)u), j0 = std::min(from->rows - 2, (int)v);
                const double tx = u - i0, ty = v - j0;
                const float *a = &from->pt[((size_t)j0 * from->cols + i0) * 2], *b = a + 2, *c = a + (size_t)from->cols * 2, *d = c + 2;
                x = (a[0] * (1 - tx) + b[0] * tx) * (1 - ty) + (c[0] * (1 - tx) + d[0] * tx) * ty;
                y = (a[1] * (1 - tx) + b[1] * tx) * (1 - ty) + (c[1] * (1 - tx) + d[1] * tx) * ty;
            }
            m.pt[((size_t)j * cols + i) * 2] = (float)x;
            m.pt[((size_t)j * cols + i) * 2 + 1] = (float)y;
        }
    return m;
}

// Mesh Warp's settings that act on the mesh: grid size (Grid size in X / Y times the multipliers: that many cells, one more point than cells across), Lock (Y follows X),
// Preserve Mesh Structure (the warp kept when the grid changes, else the mesh starts again as a regular grid) and the Reset Mesh button. An empty WarpData is a regular grid at any size.
void meshChanged(Change& ch)
{
    const std::string& p = ch.param;
    if (p == "GridCountX" && ch.b("GridCountLink")) ch.setI("GridCountY", ch.i("GridCountX"));
    else if (p == "GridCountY" && ch.b("GridCountLink")) ch.setI("GridCountX", ch.i("GridCountY"));
    else if (p == "ScaleGridX" && ch.b("GridCountLink")) ch.setI("ScaleGridY", ch.i("ScaleGridX"));
    else if (p == "ScaleGridY" && ch.b("GridCountLink")) ch.setI("ScaleGridX", ch.i("ScaleGridY"));
    else if (p == "GridCountLink" && ch.b("GridCountLink")) { ch.setI("GridCountY", ch.i("GridCountX")); ch.setI("ScaleGridY", ch.i("ScaleGridX")); }
    const bool sizeSetting = p == "GridCountX" || p == "GridCountY" || p == "ScaleGridX" || p == "ScaleGridY" || p == "GridCountLink";
    if (!sizeSetting && p != "ResetMesh") return;
    const int cols = std::max(1, std::min(200, ch.i("GridCountX") * std::max(1, ch.i("ScaleGridX")))) + 1;
    const int rows = std::max(1, std::min(200, ch.i("GridCountY") * std::max(1, ch.i("ScaleGridY")))) + 1;
    if (p == "ResetMesh") { ch.setS("WarpData", ""); return; }
    Mesh old;
    if (!readMesh(ch.s("WarpData"), old) || (old.cols == cols && old.rows == rows)) return;
    ch.setS("WarpData", writeMesh(ch.b("KeepMeshStructure") ? gridOf(cols, rows, &old) : gridOf(cols, rows, nullptr)));
}

// ------------------------------------------------------------------ Smart Upscale
// VEGAS's (Sony SPICA, spica_resizer.ofx) works on the picture as Y, U, V and alpha planes in float on the card (OpenCL kernels read from the DLL): RGB to YUV (Y = .299 R + .587 G
// + .114 B, U and V centred on 127.5 with the matrix -.169 -.331 .5 / .5 -.419 -.081 on levels 0..255), the Lanczos resizer, the "mapping" kernel on Y (its Y arrives as a whole number:
// the kernel casts the plane to unsigned short, which cuts it down), and back with 1.402 / .344 / .714 / 1.772 (not the exact inverse, hence the one-level changes everywhere);
// the result is stored as the other kernels do (more::oclStore12). That whole round trip is done here and is bit for bit VEGAS's when Enable enhancement is off (checked on the
// capture's pictures: zero levels off). The frame in a Filter effect keeps its size in VEGAS whatever Scale says (the captures with Scale 1, 2, 2.5 and 4 are the same picture),
// so Scale only sets how strong Reno's own enhancement is. The enhancement itself is a table-driven (class adaptive) filter whose coefficient tables are in the DLL and are not read
// yet: what follows is Reno's own stand-in, the fine detail (frame minus a soft copy) added back, only the part above the noise level (Noise).
void smartUpscale(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) { copyFrame(c); return; }
    const bool enhance = c.b("EnableEnhancement");
    double sx, sy, sz;
    c.xyz("Scale", sx, sy, sz);
    const double scale = clampd((sx + sy) * 0.5, 1, 16);
    const double amount = enhance ? 0.6 * std::min(1.0, (scale - 1) / 3.0) : 0.0;
    Buf soft;
    if (amount > 0) {
        soft = c.src;
        gaussianBlur(soft, c.px(0.9), c.px(0.9));
    }
    const float t0 = (float)(0.004 * clampd(c.d("Noise"), 0, 20000) / 2000.0), t1 = t0 * 3.f + 0.004f;
    const bool lumaOnly = c.b("LumaOnly");
    const bool q8 = c.host8;
    const float amt = (float)amount;
    auto gate = [&](float d) {
        const float m = std::fabs(d);
        if (m <= t0) return 0.f;
        return d * std::min(1.f, (m - t0) / (t1 - t0));
    };
    forRows(H, W, [&](int y) {
        const float* s = c.src.at(0, y);
        const float* b = amount > 0 ? soft.at(0, y) : s;
        float* o = c.out.at(0, y);
        for (int x = 0; x < W; ++x, s += 4, b += 4, o += 4) {
            const float a = s[3];
            // Reno's stand-in enhancement goes in before VEGAS's own conversion (the enhancement is VEGAS's mapping stage, between the planes and the way back)
            float in[3] = {s[0], s[1], s[2]};
            if (amount > 0) {
                if (lumaOnly) {
                    const float d = gate(luma(s[0], s[1], s[2]) - luma(b[0], b[1], b[2])) * amt;
                    for (int k = 0; k < 3; ++k) in[k] += d;
                } else {
                    for (int k = 0; k < 3; ++k) in[k] += gate(s[k] - b[k]) * amt;
                }
                for (int k = 0; k < 3; ++k) in[k] = std::min(std::max(in[k], 0.f), std::max(a, 0.f));
            }
            const float r = in[0] * 255.f, g = in[1] * 255.f, bl = in[2] * 255.f;
            float Y = 0.299f * r + 0.587f * g + 0.114f * bl;
            const float U = -0.169f * r - 0.331f * g + 0.500f * bl + 127.5f;
            const float V = 0.500f * r - 0.419f * g - 0.081f * bl + 127.5f;
            Y = std::floor(std::min(std::max(Y, 0.f), 255.f));
            const float y1 = Y / 255.f, u1 = U / 255.f - 0.5f, v1 = V / 255.f - 0.5f;
            const float rgb[3] = {y1 + 1.402f * v1, y1 - 0.344f * u1 - 0.714f * v1, y1 + 1.772f * u1};
            for (int k = 0; k < 3; ++k) {
                const float v = std::min(std::max(rgb[k], 0.f), 1.f);
                o[k] = q8 ? more::oclStore12(v) : v;
            }
            o[3] = a;
        }
    });
}

}  // namespace

void meshWarpPoints(const std::string& warpData, int cellsX, int cellsY, int& cols, int& rows, std::vector<float>& pts)
{
    cols = std::max(1, std::min(200, cellsX)) + 1;
    rows = std::max(1, std::min(200, cellsY)) + 1;
    Mesh m;
    if (readMesh(warpData, m)) {
        cols = m.cols;
        rows = m.rows;
    } else {
        m = gridOf(cols, rows, nullptr);
    }
    pts = m.pt;
}

std::string meshWarpText(int cols, int rows, const std::vector<float>& pts)
{
    Mesh m;
    m.cols = cols;
    m.rows = rows;
    m.pt = pts;
    return writeMesh(m);
}

// For the GPU (vkeffects.cpp): the red, green and blue tables of the effect's curves (the master curve folded in) one after the other, 1024
// values each, the very tables colorCurvesRows looks up. False when the effect has no curves (it then leaves the frame as it is).
bool colorCurvesGpuTables(Ctx& c, std::vector<float>& out)
{
    std::vector<CurvePoint> curves[4];
    if (!readCurves(c.s("Property"), curves)) return false;
    std::vector<float> master((size_t)kLut);
    curveTable(curves[3], master.data());
    out.assign((size_t)kLut * 3, 0.f);
    for (int k = 0; k < 3; ++k) {
        float* t = &out[(size_t)k * kLut];
        curveTable(curves[k], t);
        for (int i = 0; i < kLut; ++i) t[i] = look(master.data(), t[i]);
    }
    return true;
}

// Color Curves, 8-bit: the 768 byte-table entries colorCurvesRows() looks up (false: no curves, the frame is copied)
bool colorCurves8GpuTables(Ctx& c, std::vector<float>& out)
{
    std::vector<CurvePoint> curves[4];
    if (!readCurves(c.s("Property"), curves)) return false;
    out.assign(768, 0.f);
    unsigned char m[256], t[256];
    const bool haveM = curveBytes8(curves[3], m);
    for (int k = 0; k < 3; ++k) {
        const bool ok = curveBytes8(curves[k], t);
        for (int v = 0; v < 256; ++v) {
            unsigned char r = ok ? t[v] : (unsigned char)v;
            if (haveM) r = m[r];
            out[(size_t)k * 256 + (size_t)v] = (float)r;
        }
    }
    return true;
}

void addFiltersMagix(std::vector<Def>& out)
{
    {
        std::vector<PDef> ps = {boolean("ShowAllChannels", "Show all channels", true), choice("SelectedChannel", "Channel", 0, {"Red", "Green", "Blue", "Master", "All"}),
                                custom("Property", "Curves", kMagixDefaultCurves), intp("AutoMode", "Auto mode", 1, 0, 8), intp("ReferenceMode", "Reference mode", 0, 0, 8),
                                boolean("DoAnalysis", "Do analysis", false)};
        for (auto& p : ps) if (p.name != "Property") p.secret = true;
        Def d = make("com.vegascreativesoftware:colorcurves", "Color Curves", "Red, green, blue and master curves", ps, colorCurves);
        d.rows = colorCurvesRows;
        d.changed = colorCurvesChanged;
        out.push_back(d);
    }
    {
        std::vector<PDef> ps = {dbl("Latitude", "Latitude", 0, -1e4, 1e4), dbl("Longitude", "Longitude", 0, -1e4, 1e4), dbl("Z", "Z", 20, 0, 1e6)};
        ps[0].dmin = -180; ps[0].dmax = 180;
        ps[1].dmin = -180; ps[1].dmax = 180;
        ps[2].dmin = 1; ps[2].dmax = 1000;
        ps[0].hint = "Rotate the coordinate that specifies the top/bottom position of a point on the surface.";
        ps[1].hint = "Rotate the coordinate that specifies the left/right position of a point on the surface.";
        ps[2].hint = "Move the position of the sphere in z direction.";
        out.push_back(make("de.magix:stereographicProjectionOCL", "Tiny Planet", "A 360 degree picture as a tiny planet (a stereographic projection)", ps, tinyPlanet));
    }
    {
        std::vector<PDef> ps = {custom("WarpData", "Mesh", ""), boolean("ForceReload", "Force reload", true), choice("WarpDirection", "Mesh point refers to", 1, {"Source", "Target"}),
                                intp("GridCountX", "Grid size in X", 4, 1, 100), intp("GridCountY", "Grid size in Y", 4, 1, 100),
                                intp("ScaleGridX", "Grid size multiplier in X", 1, 1, 10), intp("ScaleGridY", "Grid size multiplier in Y", 1, 1, 10),
                                boolean("GridCountLink", "Lock Grid size in X and Y", true), boolean("KeepMeshStructure", "Preserve Mesh Structure", true),
                                button("ResetMesh", "Reset Mesh"), boolean("EnableWarp", "Enable Warp Effect", true)};
        ps[0].secret = true;
        ps[1].secret = true;
        ps[3].dmax = 30;
        ps[4].dmax = 30;
        ps[5].dmax = 4;
        ps[6].dmax = 4;
        for (auto& p : ps) {
            if (p.name == "GridCountX") p.hint = "Grid size in X";
            if (p.name == "GridCountY") p.hint = "Grid size in Y";
            if (p.name == "ScaleGridX") p.hint = "Grid size multiplier in X";
            if (p.name == "ScaleGridY") p.hint = "Grid size multiplier in Y";
            if (p.name == "GridCountLink") p.hint = "Keeps grid size in X and Y equal.";
            if (p.name == "KeepMeshStructure") p.hint = "Trys to keep structure of grid while resizing.";
            if (p.name == "ResetMesh") p.hint = "Resets the mesh.";
            if (p.name == "EnableWarp") p.hint = "Enable Warp Effect";
        }
        Def d = make("de.magix:MeshWarp", "Mesh Warp", "The picture bent by a grid of points", ps, meshWarp);
        d.changed = meshChanged;
        d.overlay = meshWarpOverlay();
        out.push_back(d);
    }
    {
        std::vector<PDef> ps = {point3("Scale", "Scale", 2, 2, 2, 1, 16), boolean("LumaOnly", "Luma only", true), dbl("Noise", "Noise sensitivity", 2000, 0, 20000),
                                boolean("AlphaModEnabled", "Alpha mode", true), dbl("ExternalAlpha", "External alpha", 0.5, 0, 1),
                                intp("CoeffIndexChoice", "Coefficient set", 0, 0, 16), boolean("EnableEnhancement", "Enable enhancement", true)};
        ps[0].dmax = 8;
        ps[0].hint = "Sets the resize scale value.";
        ps[2].dmax = 5000;
        ps[2].hint = "The noise level of the image.";
        ps[6].hint = "Enable or disable the smart enhancement.";
        out.push_back(make("com.sonycreativesoftware:smart_upscale", "Smart Upscale", "The picture sharpened as an upscaler would leave it", ps, smartUpscale));
    }
}


// Mesh Warp for the GPU: 0 = the CPU does it (the triangle mode), 1 = it copies the frame, 2 = pictures drawn through the points ('pts', x y per point)
int meshWarpGpu(Ctx& c, int& cols, int& rows, std::vector<float>& pts)
{
    Mesh m;
    if (!c.b("EnableWarp") || !readMesh(c.s("WarpData"), m) || m.identity) return 1;
    if (c.i("WarpDirection") != 0) return 0;
    cols = m.cols;
    rows = m.rows;
    pts = m.pt;
    return 2;
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
