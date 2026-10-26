// See vegascurves.h.
#include "vegascurves.h"
#include "../core/base.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace reno {
namespace ofx {

namespace {

const double kSlopeLimit = 1.0e4;

double clampSlope(double dy, double dx)
{
    if (std::fabs(dx) < 1e-9) return dy >= 0 ? kSlopeLimit : -kSlopeLimit;
    return std::min(kSlopeLimit, std::max(-kSlopeLimit, dy / dx));
}

}  // namespace

void bezHandleIn(const BezCurve& c, size_t i, double* dx, double* dy)
{
    if (i == 0 || i >= c.size()) {
        *dx = i < c.size() ? c[i].rawIn[0] : 0;
        *dy = i < c.size() ? c[i].rawIn[1] : 0;
        return;
    }
    const double gap = c[i].x - c[i - 1].x;
    *dx = -c[i].inShare * gap;
    *dy = c[i].inSlope * *dx;
}

void bezHandleOut(const BezCurve& c, size_t i, double* dx, double* dy)
{
    if (i + 1 >= c.size()) {
        *dx = i < c.size() ? c[i].rawOut[0] : 0;
        *dy = i < c.size() ? c[i].rawOut[1] : 0;
        return;
    }
    const double gap = c[i + 1].x - c[i].x;
    *dx = c[i].outShare * gap;
    *dy = c[i].outSlope * *dx;
}

void bezSetHandleIn(BezCurve& c, size_t i, double dx, double dy)
{
    if (i == 0 || i >= c.size()) return;
    BezPoint& p = c[i];
    const double gap = p.x - c[i - 1].x;
    const double len = std::min(gap, std::max(0.0, -dx));
    p.inShare = gap > 0 ? len / gap : 0;
    p.inSlope = clampSlope(dy, -len);
    if (p.smooth && i + 1 < c.size()) p.outSlope = p.inSlope;
}

void bezSetHandleOut(BezCurve& c, size_t i, double dx, double dy)
{
    if (i + 1 >= c.size()) return;
    BezPoint& p = c[i];
    const double gap = c[i + 1].x - p.x;
    const double len = std::min(gap, std::max(0.0, dx));
    p.outShare = gap > 0 ? len / gap : 0;
    p.outSlope = clampSlope(dy, len);
    if (p.smooth && i > 0) p.inSlope = p.outSlope;
}

size_t bezInsert(BezCurve& c, double x, double y, double range)
{
    if (c.size() < 2) c = bezStraight(range);
    const double eps = range * 0.002;
    x = std::min(c.back().x - eps, std::max(c.front().x + eps, x));
    y = std::min(range, std::max(0.0, y));
    size_t at = 1;
    while (at < c.size() - 1 && c[at].x < x) ++at;
    const BezPoint &a = c[at - 1], &b = c[at];
    BezPoint p;
    p.x = x;
    p.y = y;
    const double span = b.x - a.x;
    const double slope = span > 1e-9 ? (b.y - a.y) / span : 1.0;
    p.inShare = p.outShare = 0.25;
    p.inSlope = p.outSlope = std::min(kSlopeLimit, std::max(-kSlopeLimit, slope));
    p.inOn = p.outOn = true;
    p.smooth = true;
    const unsigned char fl[8] = {1, 0, 1, 1, 1, 1, 0, 0};
    std::memcpy(p.flags, fl, 8);
    c.insert(c.begin() + (std::ptrdiff_t)at, p);
    return at;
}

bool bezRemove(BezCurve& c, size_t i)
{
    if (i == 0 || i + 1 >= c.size()) return false;
    c.erase(c.begin() + (std::ptrdiff_t)i);
    return true;
}

void bezMove(BezCurve& c, size_t i, double x, double y, double range)
{
    if (i >= c.size()) return;
    const double eps = range * 0.002;
    if (i == 0 || i + 1 == c.size()) x = c[i].x;
    else x = std::min(c[i + 1].x - eps, std::max(c[i - 1].x + eps, x));
    c[i].x = x;
    c[i].y = std::min(range, std::max(0.0, y));
}

void bezPolyline(const BezCurve& c, int steps, std::vector<std::pair<double, double>>& out)
{
    out.clear();
    if (c.empty()) return;
    out.push_back({c[0].x, c[0].y});
    for (size_t i = 0; i + 1 < c.size(); ++i) {
        const BezPoint &a = c[i], &b = c[i + 1];
        double odx, ody, idx, idy;
        bezHandleOut(c, i, &odx, &ody);
        bezHandleIn(c, i + 1, &idx, &idy);
        const double px[4] = {a.x, a.x + odx, b.x + idx, b.x}, py[4] = {a.y, a.y + ody, b.y + idy, b.y};
        for (int k = 1; k <= steps; ++k) {
            const double u = (double)k / steps, v = 1 - u;
            const double w0 = v * v * v, w1 = 3 * v * v * u, w2 = 3 * v * u * u, w3 = u * u * u;
            const double x = std::max(out.back().first, w0 * px[0] + w1 * px[1] + w2 * px[2] + w3 * px[3]);
            out.push_back({x, w0 * py[0] + w1 * py[1] + w2 * py[2] + w3 * py[3]});
        }
    }
}

void bezNormalise(BezCurve& c)
{
    for (size_t i = 0; i < c.size(); ++i) {
        BezPoint& p = c[i];
        const bool first = i == 0, last = i + 1 == c.size();
        p.inOn = !first;
        p.outOn = !last;
        p.flags[0] = 1;
        p.flags[2] = first ? 0 : (last ? 1 : (p.smooth ? 1 : 0));
        p.flags[3] = (first || last) ? 0 : 1;
        p.flags[4] = p.flags[5] = 1;
    }
}

BezCurve bezStraight(double range)
{
    BezCurve c(2);
    const double h = range * 0.2;
    c[0].x = c[0].y = 0;
    c[1].x = c[1].y = range;
    c[0].inOn = false;
    c[0].outOn = true;
    c[0].rawIn[0] = -h; c[0].rawIn[1] = 0;
    c[0].rawOut[0] = h; c[0].rawOut[1] = h;
    c[1].inOn = true;
    c[1].outOn = false;
    c[1].rawIn[0] = -h; c[1].rawIn[1] = -h;
    c[1].rawOut[0] = h; c[1].rawOut[1] = 0;
    const unsigned char f0[8] = {1, 0, 0, 0, 1, 1, 0, 0}, f1[8] = {1, 0, 1, 0, 1, 1, 0, 0};
    std::memcpy(c[0].flags, f0, 8);
    std::memcpy(c[1].flags, f1, 8);
    c[0].smooth = c[1].smooth = true;
    return c;
}

// ------------------------------------------------------------------ Color Curves' "Property"
bool decodeColorCurves(const std::string& text, BezCurve out[4])
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
    BezCurve tmp[4];
    for (int k = 0; k < 4; ++k) {
        for (int i = 0; i < n[k]; ++i, o += 104) {
            BezPoint p;
            p.x = f64(o);
            p.y = f64(o + 8);
            std::memcpy(p.flags, b.data() + o + 16, 8);
            p.inShare = f64(o + 24);
            p.inSlope = f64(o + 32);
            p.rawIn[0] = f64(o + 40);
            p.rawIn[1] = f64(o + 48);
            p.inOn = i32(o + 56) != 0;
            p.outShare = f64(o + 64);
            p.outSlope = f64(o + 72);
            p.rawOut[0] = f64(o + 80);
            p.rawOut[1] = f64(o + 88);
            p.outOn = i32(o + 96) != 0;
            p.smooth = p.flags[2] != 0;
            const double all[10] = {p.x, p.y, p.inShare, p.inSlope, p.rawIn[0], p.rawIn[1], p.outShare, p.outSlope, p.rawOut[0], p.rawOut[1]};
            for (double v : all)
                if (!std::isfinite(v)) return false;
            tmp[k].push_back(p);
        }
    }
    for (int k = 0; k < 4; ++k) out[k] = std::move(tmp[k]);
    return true;
}

std::string encodeColorCurves(const BezCurve in[4])
{
    size_t total = 0;
    for (int k = 0; k < 4; ++k) total += in[k].size();
    std::string b(20 + total * 104, '\0');
    auto put32 = [&](size_t o, int32_t v) { std::memcpy(&b[o], &v, 4); };
    auto put64 = [&](size_t o, double v) { std::memcpy(&b[o], &v, 8); };
    put32(0, 1);
    for (int k = 0; k < 4; ++k) put32(4 + 4 * (size_t)k, (int32_t)in[k].size());
    size_t o = 20;
    for (int k = 0; k < 4; ++k) {
        const BezCurve& c = in[k];
        for (size_t i = 0; i < c.size(); ++i, o += 104) {
            const BezPoint& p = c[i];
            double idx, idy, odx, ody;
            bezHandleIn(c, i, &idx, &idy);
            bezHandleOut(c, i, &odx, &ody);
            put64(o, p.x);
            put64(o + 8, p.y);
            std::memcpy(&b[o + 16], p.flags, 8);
            put64(o + 24, p.inShare);
            put64(o + 32, p.inSlope);
            put64(o + 40, idx);
            put64(o + 48, idy);
            put32(o + 56, p.inOn ? 1 : 0);
            put64(o + 64, p.outShare);
            put64(o + 72, p.outSlope);
            put64(o + 80, odx);
            put64(o + 88, ody);
            put32(o + 96, p.outOn ? 1 : 0);
        }
    }
    return base64Encode(b.data(), b.size());
}

// ------------------------------------------------------------------ Color Grading's Curve_R ...
bool decodeGradingCurve(const std::string& text, BezCurve& out)
{
    const char* s = text.c_str();
    char* e = nullptr;
    const long n = std::strtol(s, &e, 10);
    if (e == s || *e != ':' || n < 2 || n > 256) return false;
    struct Raw { double v[6]; };
    std::vector<Raw> raw((size_t)n);
    s = e;
    for (long i = 0; i < n; ++i) {
        for (int k = 0; k < 6; ++k) {
            if (*s != ':') return false;
            ++s;
            raw[(size_t)i].v[k] = std::strtod(s, &e);
            if (e == s || !std::isfinite(raw[(size_t)i].v[k])) return false;
            s = e;
        }
    }
    BezCurve c((size_t)n);
    for (size_t i = 0; i < c.size(); ++i) {
        const double* v = raw[i].v;
        BezPoint& p = c[i];
        p.x = v[0];
        p.y = v[1];
        p.rawIn[0] = v[2]; p.rawIn[1] = v[3];
        p.rawOut[0] = v[4]; p.rawOut[1] = v[5];
        p.inOn = i > 0;
        p.outOn = i + 1 < c.size();
        const unsigned char fl[8] = {1, 0, 1, 1, 1, 1, 0, 0};
        std::memcpy(p.flags, fl, 8);
    }
    for (size_t i = 0; i < c.size(); ++i) {
        BezPoint& p = c[i];
        if (i > 0) {
            const double gap = p.x - c[i - 1].x;
            p.inShare = gap > 0 ? std::min(1.0, std::max(0.0, -p.rawIn[0] / gap)) : 0;
            p.inSlope = clampSlope(p.rawIn[1], p.rawIn[0]);
        }
        if (i + 1 < c.size()) {
            const double gap = c[i + 1].x - p.x;
            p.outShare = gap > 0 ? std::min(1.0, std::max(0.0, p.rawOut[0] / gap)) : 0;
            p.outSlope = clampSlope(p.rawOut[1], p.rawOut[0]);
        }
        p.smooth = (i == 0 || i + 1 == c.size()) ? true : std::fabs(p.inSlope - p.outSlope) < 0.02 * std::max(1.0, std::fabs(p.outSlope));
    }
    out = std::move(c);
    return true;
}

std::string encodeGradingCurve(const BezCurve& c)
{
    std::string out = std::to_string(c.size());
    char buf[48];
    auto add = [&](double v) {
        std::snprintf(buf, sizeof buf, ":%.6f", v);
        out += buf;
    };
    for (size_t i = 0; i < c.size(); ++i) {
        double idx, idy, odx, ody;
        bezHandleIn(c, i, &idx, &idy);
        bezHandleOut(c, i, &odx, &ody);
        add(c[i].x); add(c[i].y); add(idx); add(idy); add(odx); add(ody);
    }
    return out;
}

}  // namespace ofx
}  // namespace reno
