// How the movement of the whole picture between two frames is found (see builtins_analysis_internal.h), used by Stabilize and the Flicker Reducer. Reno's own method
// (VEGAS also works from feature points and a model fit, but its code is not published): block matching of textured patches on a grid, a coarse whole-picture shift to
// start the search for fast movement, Lucas-Kanade refinement between pixels, and a robust (re-weighted least squares) fit of a shift / similarity / affine map to the vectors.
#include "builtins_analysis_internal.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace reno {
namespace ofx {
namespace builtin {
namespace analysis {

// ------------------------------------------------------------------ Xf
Xf Xf::operator*(const Xf& o) const
{
    Xf r;
    r.a = a * o.a + b * o.c;
    r.b = a * o.b + b * o.d;
    r.c = c * o.a + d * o.c;
    r.d = c * o.b + d * o.d;
    r.tx = a * o.tx + b * o.ty + tx;
    r.ty = c * o.tx + d * o.ty + ty;
    return r;
}

Xf Xf::inverse() const
{
    const double det = a * d - b * c;
    if (!(std::fabs(det) > 1e-9) || !std::isfinite(det)) return Xf();
    Xf r;
    r.a = d / det;
    r.b = -b / det;
    r.c = -c / det;
    r.d = a / det;
    r.tx = -(r.a * tx + r.b * ty);
    r.ty = -(r.c * tx + r.d * ty);
    return r;
}

// ------------------------------------------------------------------ frames in grey
float Gray::sample(double x, double y) const
{
    if (w <= 0 || h <= 0) return 0.f;
    x = x < 0 ? 0 : (x > w - 1 ? w - 1 : x);
    y = y < 0 ? 0 : (y > h - 1 ? h - 1 : y);
    const int x0 = (int)x, y0 = (int)y;
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float fx = (float)(x - x0), fy = (float)(y - y0);
    const float* r0 = &p[(size_t)y0 * w];
    const float* r1 = &p[(size_t)y1 * w];
    return (r0[x0] * (1 - fx) + r0[x1] * fx) * (1 - fy) + (r1[x0] * (1 - fx) + r1[x1] * fx) * fy;
}

Gray makeGray(const Buf& b, int maxW)
{
    Gray g;
    if (b.w <= 0 || b.h <= 0 || b.px.size() < (size_t)b.w * b.h * 4) return g;
    int w = std::min(std::max(maxW, 8), b.w);
    int h = std::max(1, (int)std::lround((double)b.h * w / b.w));
    h = std::min(h, b.h);
    g.w = w;
    g.h = h;
    g.p.assign((size_t)w * h, 0.f);
    std::vector<double> alphaRow((size_t)h, 0.0);
    forRows(h, w, [&](int y) {
        const int y0 = (int)((int64_t)y * b.h / h), y1 = std::max(y0 + 1, (int)((int64_t)(y + 1) * b.h / h));
        double aSum = 0;
        for (int x = 0; x < w; ++x) {
            const int x0 = (int)((int64_t)x * b.w / w), x1 = std::max(x0 + 1, (int)((int64_t)(x + 1) * b.w / w));
            double s = 0, a = 0;
            for (int yy = y0; yy < y1; ++yy) {
                const float* p = b.at(x0, yy);
                for (int xx = x0; xx < x1; ++xx, p += 4) {
                    s += 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
                    a += p[3];
                }
            }
            const double n = (double)(y1 - y0) * (x1 - x0);
            g.p[(size_t)y * w + x] = (float)(s / n);
            aSum += a / n;
        }
        alphaRow[(size_t)y] = aSum / w;
    });
    double a = 0;
    for (double v : alphaRow) a += v;
    g.alpha = a / h;
    return g;
}

Gray halfGray(const Gray& s)
{
    Gray g;
    g.w = std::max(1, s.w / 2);
    g.h = std::max(1, s.h / 2);
    g.alpha = s.alpha;
    g.p.assign((size_t)g.w * g.h, 0.f);
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x) {
            const int x0 = std::min(2 * x, s.w - 1), x1 = std::min(2 * x + 1, s.w - 1), y0 = std::min(2 * y, s.h - 1), y1 = std::min(2 * y + 1, s.h - 1);
            g.p[(size_t)y * g.w + x] = 0.25f * (s.at(x0, y0) + s.at(x1, y0) + s.at(x0, y1) + s.at(x1, y1));
        }
    return g;
}

Signature signatureOf(const Gray& g)
{
    Signature s;
    for (int j = 0; j < 9; ++j)
        for (int i = 0; i < 16; ++i) {
            const int x0 = i * g.w / 16, x1 = std::max(x0 + 1, (i + 1) * g.w / 16), y0 = j * g.h / 9, y1 = std::max(y0 + 1, (j + 1) * g.h / 9);
            double sum = 0;
            int n = 0;
            for (int y = y0; y < std::min(y1, g.h); ++y)
                for (int x = x0; x < std::min(x1, g.w); ++x, ++n) sum += g.at(x, y);
            s.v[j * 16 + i] = n ? (float)(sum / n) : 0.f;
        }
    return s;
}

bool sameSignature(const Signature& a, const Signature& b)
{
    double worst = 0, mean = 0;
    for (int k = 0; k < 16 * 9; ++k) {
        const double d = std::fabs((double)a.v[k] - b.v[k]);
        worst = std::max(worst, d);
        mean += d;
    }
    return worst < 0.12 && mean / (16 * 9) < 0.03;
}

// ------------------------------------------------------------------ shifts
namespace {

// Where A's content is in B, searched over the rectangle [x0,x1) x [y0,y1) of A: integer shifts within 'range', cost the squared difference with the mean taken off,
// a parabola through the costs round the best for the sub-pixel part.
bool shiftSearch(const Gray& A, const Gray& B, int x0, int y0, int x1, int y1, int range, double& dx, double& dy, double* costOut = nullptr)
{
    dx = dy = 0;
    x0 = std::max(x0, range);
    y0 = std::max(y0, range);
    x1 = std::min(x1, A.w - range);
    y1 = std::min(y1, A.h - range);
    if (x1 - x0 < 6 || y1 - y0 < 6) return false;
    const int n = (2 * range + 1);
    std::vector<double> cost((size_t)n * n, 0.0);
    const double cnt = (double)(x1 - x0) * (y1 - y0);
    double best = 1e30;
    int bi = 0, bj = 0;
    for (int j = -range; j <= range; ++j)
        for (int i = -range; i <= range; ++i) {
            double s = 0, s2 = 0;
            for (int y = y0; y < y1; ++y) {
                const float* a = &A.p[(size_t)y * A.w];
                const float* b = &B.p[(size_t)(y + j) * B.w + i];
                for (int x = x0; x < x1; ++x) {
                    const double d = (double)b[x] - a[x];
                    s += d;
                    s2 += d * d;
                }
            }
            const double c = (s2 - s * s / cnt) / cnt;
            cost[(size_t)(j + range) * n + (i + range)] = c;
            if (c < best) { best = c; bi = i; bj = j; }
        }
    double sx = 0, sy = 0;
    auto parab = [](double cm, double c0, double cp) {
        const double den = cm - 2 * c0 + cp;
        if (!(den > 1e-12)) return 0.0;
        return std::max(-0.5, std::min(0.5, 0.5 * (cm - cp) / den));
    };
    if (bi > -range && bi < range) sx = parab(cost[(size_t)(bj + range) * n + (bi - 1 + range)], best, cost[(size_t)(bj + range) * n + (bi + 1 + range)]);
    if (bj > -range && bj < range) sy = parab(cost[(size_t)(bj - 1 + range) * n + (bi + range)], best, cost[(size_t)(bj + 1 + range) * n + (bi + range)]);
    dx = bi + sx;
    dy = bj + sy;
    if (costOut) *costOut = best;
    return true;
}

}  // namespace

bool globalShift(const Gray& A, const Gray& B, int range, double& dx, double& dy)
{
    dx = dy = 0;
    if (A.w != B.w || A.h != B.h || A.w < 16 || A.h < 16) return false;
    return shiftSearch(A, B, 0, 0, A.w, A.h, std::max(1, range), dx, dy);
}

// ------------------------------------------------------------------ patches
namespace {

struct Patch {
    double x = 0, y = 0;
    double vx = 0, vy = 0;          // how far it moved
    double tex = 0;                 // how much detail it has
    bool ok = false;
};

double patchTexture(const Gray& A, int px, int py, int ps)
{
    double t = 0;
    for (int y = py; y < py + ps; ++y) {
        const float* r = &A.p[(size_t)y * A.w];
        const float* r2 = &A.p[(size_t)(y + 1) * A.w];
        for (int x = px; x < px + ps; ++x) t += std::fabs(r[x + 1] - r[x]) + std::fabs(r2[x] - r[x]);
    }
    return t / ((double)ps * ps);
}

// The cost of the patch of A at (px, py) against B displaced by (ox, oy): the mean squared difference with the mean taken off
// (a change of brightness does not matter), or the mean absolute difference.
inline double patchCost(const Gray& A, const Gray& B, int px, int py, int ps, int ox, int oy, bool sad)
{
    double s = 0, s2 = 0;
    for (int y = 0; y < ps; ++y) {
        const float* a = &A.p[(size_t)(py + y) * A.w + px];
        const float* b = &B.p[(size_t)(py + y + oy) * B.w + px + ox];
        if (sad) {
            for (int x = 0; x < ps; ++x) s += std::fabs((double)b[x] - a[x]);
        } else {
            for (int x = 0; x < ps; ++x) {
                const double d = (double)b[x] - a[x];
                s += d;
                s2 += d * d;
            }
        }
    }
    const double n = (double)ps * ps;
    return sad ? s / n : (s2 - s * s / n) / n;
}

// Lucas-Kanade refinement of the vector of a patch (a few steps of the Gauss-Newton fit of B displaced to A, the mean difference taken off).
void lkRefine(const Gray& A, const Gray& B, int px, int py, int ps, double& vx, double& vy, int iters)
{
    const double n = (double)ps * ps;
    for (int it = 0; it < iters; ++it) {
        double me = 0;
        for (int y = 0; y < ps; ++y)
            for (int x = 0; x < ps; ++x) me += B.sample(px + x + vx, py + y + vy) - A.at(px + x, py + y);
        me /= n;
        double sxx = 0, sxy = 0, syy = 0, bx = 0, by = 0;
        for (int y = 0; y < ps; ++y)
            for (int x = 0; x < ps; ++x) {
                const double bxp = px + x + vx, byp = py + y + vy;
                const double e = B.sample(bxp, byp) - A.at(px + x, py + y) - me;
                const double gx = 0.5 * (B.sample(bxp + 1, byp) - B.sample(bxp - 1, byp));
                const double gy = 0.5 * (B.sample(bxp, byp + 1) - B.sample(bxp, byp - 1));
                sxx += gx * gx; sxy += gx * gy; syy += gy * gy;
                bx += gx * e; by += gy * e;
            }
        const double reg = 1e-4 * n;
        sxx += reg; syy += reg;
        const double det = sxx * syy - sxy * sxy;
        if (!(det > 1e-12)) return;
        double dx = -(syy * bx - sxy * by) / det, dy = -(-sxy * bx + sxx * by) / det;
        dx = std::max(-1.0, std::min(1.0, dx));
        dy = std::max(-1.0, std::min(1.0, dy));
        vx += dx;
        vy += dy;
        if (std::fabs(dx) < 0.01 && std::fabs(dy) < 0.01) return;
    }
}

double medianOf(std::vector<double> v)
{
    if (v.empty()) return 0;
    const size_t k = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + (ptrdiff_t)k, v.end());
    return v[k];
}

// Weighted least squares fit of the model to p -> q (p, q in Xf's units).
bool solveModel(const std::vector<FeatPt>& pts, const std::vector<double>& w, int model, Xf& xf)
{
    double sw = 0, cpx = 0, cpy = 0, cqx = 0, cqy = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        sw += w[i];
        cpx += w[i] * pts[i].x; cpy += w[i] * pts[i].y;
        cqx += w[i] * (pts[i].x + pts[i].vx); cqy += w[i] * (pts[i].y + pts[i].vy);
    }
    if (!(sw > 1e-9)) return false;
    cpx /= sw; cpy /= sw; cqx /= sw; cqy /= sw;
    Xf r;
    if (model <= 0) {
        r.tx = cqx - cpx;
        r.ty = cqy - cpy;
        xf = r;
        return true;
    }
    double spp = 0, dot = 0, cross = 0;
    double mxx = 0, mxy = 0, myy = 0, nxx = 0, nxy = 0, nyx = 0, nyy = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        const double px = pts[i].x - cpx, py = pts[i].y - cpy, qx = pts[i].x + pts[i].vx - cqx, qy = pts[i].y + pts[i].vy - cqy;
        spp += w[i] * (px * px + py * py);
        dot += w[i] * (px * qx + py * qy);
        cross += w[i] * (px * qy - py * qx);
        mxx += w[i] * px * px; mxy += w[i] * px * py; myy += w[i] * py * py;
        nxx += w[i] * qx * px; nxy += w[i] * qx * py; nyx += w[i] * qy * px; nyy += w[i] * qy * py;
    }
    if (!(spp > 1e-12)) {
        r.tx = cqx - cpx; r.ty = cqy - cpy;
        xf = r;
        return true;
    }
    if (model == 1) {
        const double th = std::atan2(cross, dot);
        r.a = std::cos(th); r.b = -std::sin(th); r.c = std::sin(th); r.d = std::cos(th);
    } else if (model == 2) {
        const double a = dot / spp, b = cross / spp;
        r.a = a; r.b = -b; r.c = b; r.d = a;
    } else {
        const double det = mxx * myy - mxy * mxy;
        if (!(det > 1e-12)) {
            const double a = dot / spp, b = cross / spp;
            r.a = a; r.b = -b; r.c = b; r.d = a;
        } else {
            r.a = (nxx * myy - nxy * mxy) / det;
            r.b = (-nxx * mxy + nxy * mxx) / det;
            r.c = (nyx * myy - nyy * mxy) / det;
            r.d = (-nyx * mxy + nyy * mxx) / det;
        }
    }
    r.tx = cqx - (r.a * cpx + r.b * cpy);
    r.ty = cqy - (r.c * cpx + r.d * cpy);
    xf = r;
    return true;
}

// Fits the model robustly: starts from the median shift, then re-weights by the residuals (a Cauchy weight about the median
// residual). Marks the vectors that agree with the result. False when too few do.
bool fitRobust(std::vector<FeatPt>& pts, int model, Xf& xf, double& conf)
{
    const size_t n = pts.size();
    conf = 0;
    if (n < 4) return false;
    std::vector<double> vx(n), vy(n);
    for (size_t i = 0; i < n; ++i) { vx[i] = pts[i].vx; vy[i] = pts[i].vy; }
    Xf cur;
    cur.tx = medianOf(vx);
    cur.ty = medianOf(vy);
    std::vector<double> w(n, 1.0), r(n, 0.0);
    double s = 0.001;
    auto residuals = [&](const Xf& f) {
        for (size_t i = 0; i < n; ++i) {
            double ox, oy;
            f.apply(pts[i].x, pts[i].y, ox, oy);
            r[i] = std::hypot(pts[i].x + pts[i].vx - ox, pts[i].y + pts[i].vy - oy);
        }
        s = std::max(1.4826 * medianOf(r), 0.0006);
    };
    for (int it = 0; it < 7; ++it) {
        residuals(cur);
        for (size_t i = 0; i < n; ++i) {
            const double q = r[i] / (2.2 * s);
            w[i] = 1.0 / (1.0 + q * q);
            if (r[i] > 6 * s) w[i] *= 0.1;
        }
        Xf next;
        if (!solveModel(pts, w, model, next)) return false;
        cur = next;
    }
    residuals(cur);
    const double thr = std::max(3.0 * s, 0.0012);
    size_t inl = 0;
    for (size_t i = 0; i < n; ++i) {
        pts[i].inlier = r[i] < thr;
        if (pts[i].inlier) ++inl;
    }
    if (inl >= 4) {
        std::vector<double> w2(n, 0.0);
        for (size_t i = 0; i < n; ++i) w2[i] = pts[i].inlier ? 1.0 : 0.0;
        Xf next;
        if (solveModel(pts, w2, model, next)) cur = next;
    }
    conf = (double)inl / n;
    xf = cur;
    return inl >= std::max<size_t>(4, n / 5);
}

}  // namespace

bool estimatePair(const Gray& A, const Gray& B, const EstParams& ep, PairResult& out)
{
    out = PairResult();
    if (A.w != B.w || A.h != B.h || A.w < 32 || A.h < 24) return false;
    if (A.alpha < 0.02 || B.alpha < 0.02) return false;
    const int W = A.w, H = A.h;
    const double rw = std::max(0.1, std::min(1.0, ep.roiW)) * W, rh = std::max(0.1, std::min(1.0, ep.roiH)) * H;
    const double rcx = std::max(0.0, std::min(1.0, ep.roiCx)) * W, rcy = std::max(0.0, std::min(1.0, ep.roiCy)) * H;
    const int rx0 = std::max(0, (int)std::lround(rcx - rw / 2)), rx1 = std::min(W, (int)std::lround(rcx + rw / 2));
    const int ry0 = std::max(0, (int)std::lround(rcy - rh / 2)), ry1 = std::min(H, (int)std::lround(rcy + rh / 2));
    if (rx1 - rx0 < 24 || ry1 - ry0 < 24) return false;

    // a first guess for fast movement: the shift of the whole region, found two levels down
    int g0x = 0, g0y = 0;
    {
        const Gray A2 = halfGray(halfGray(A)), B2 = halfGray(halfGray(B));
        double sx = 0, sy = 0;
        if (shiftSearch(A2, B2, rx0 / 4, ry0 / 4, rx1 / 4, ry1 / 4, 6, sx, sy)) {
            g0x = (int)std::lround(sx * 4);
            g0y = (int)std::lround(sy * 4);
        }
    }

    const int ps = std::max(8, std::min(16, (W / 14) & ~1));
    const int gx = std::max(2, std::min(40, ep.gridX)), gy = std::max(2, std::min(40, ep.gridY));
    const int per = std::max(1, std::min(4, ep.perCell));
    const int R = 3;
    const double cellW = (double)(rx1 - rx0) / gx, cellH = (double)(ry1 - ry0) / gy;
    std::vector<Patch> patches;
    patches.reserve((size_t)gx * gy * per);
    const double texMin = 0.004;
    for (int cj = 0; cj < gy; ++cj)
        for (int ci = 0; ci < gx; ++ci) {
            const double cx = rx0 + (ci + 0.5) * cellW, cy = ry0 + (cj + 0.5) * cellH;
            Patch cand[4];
            int nc = 0;
            for (int q = 0; q < 4; ++q) {
                const double ccx = cx + ((q & 1) ? 0.25 : -0.25) * cellW, ccy = cy + ((q & 2) ? 0.25 : -0.25) * cellH;
                const int px = (int)std::lround(ccx - (ps - 1) / 2.0), py = (int)std::lround(ccy - (ps - 1) / 2.0);
                if (px < 1 || py < 1 || px + ps + 1 >= W || py + ps + 1 >= H) continue;
                if (px + g0x - R < 0 || py + g0y - R < 0 || px + g0x + R + ps > W || py + g0y + R + ps > H) continue;
                Patch p;
                p.x = px; p.y = py;                 // (top-left for now)
                p.tex = patchTexture(A, px, py, ps);
                if (p.tex < texMin) continue;
                cand[nc++] = p;
            }
            std::sort(cand, cand + nc, [](const Patch& a, const Patch& b) { return a.tex > b.tex; });
            for (int k = 0; k < std::min(nc, per); ++k) patches.push_back(cand[k]);
        }
    if (patches.size() < 6) return false;

    std::vector<double> costs((size_t)(2 * R + 1) * (2 * R + 1));
    std::vector<FeatPt> pts;
    std::vector<Patch> kept;
    for (Patch& p : patches) {
        const int px = (int)p.x, py = (int)p.y;
        double best = 1e30, sum = 0;
        int bi = 0, bj = 0;
        for (int j = -R; j <= R; ++j)
            for (int i = -R; i <= R; ++i) {
                const double c = patchCost(A, B, px, py, ps, g0x + i, g0y + j, ep.sad);
                costs[(size_t)(j + R) * (2 * R + 1) + (i + R)] = c;
                sum += c;
                if (c < best) { best = c; bi = i; bj = j; }
            }
        const double mean = sum / costs.size();
        if (!(best < 0.6 * mean)) continue;
        double sx = 0, sy = 0;
        auto parab = [](double cm, double c0, double cp) {
            const double den = cm - 2 * c0 + cp;
            if (!(den > 1e-14)) return 0.0;
            return std::max(-0.5, std::min(0.5, 0.5 * (cm - cp) / den));
        };
        if (bi > -R && bi < R) sx = parab(costs[(size_t)(bj + R) * (2 * R + 1) + (bi - 1 + R)], best, costs[(size_t)(bj + R) * (2 * R + 1) + (bi + 1 + R)]);
        if (bj > -R && bj < R) sy = parab(costs[(size_t)(bj - 1 + R) * (2 * R + 1) + (bi + R)], best, costs[(size_t)(bj + 1 + R) * (2 * R + 1) + (bi + R)]);
        p.vx = g0x + bi + sx;
        p.vy = g0y + bj + sy;
        if (ep.lk) lkRefine(A, B, px, py, ps, p.vx, p.vy, 3);
        p.ok = true;
        kept.push_back(p);
    }
    if (kept.size() < 6) return false;

    const double unit = (double)H;
    auto toPts = [&](const std::vector<Patch>& ps_) {
        std::vector<FeatPt> v;
        v.reserve(ps_.size());
        for (const Patch& p : ps_) {
            FeatPt f;
            f.x = (float)((p.x + (ps - 1) / 2.0 + 0.5 - W / 2.0) / unit);
            f.y = (float)((p.y + (ps - 1) / 2.0 + 0.5 - H / 2.0) / unit);
            f.vx = (float)(p.vx / unit);
            f.vy = (float)(p.vy / unit);
            f.inlier = true;
            v.push_back(f);
        }
        return v;
    };
    pts = toPts(kept);
    Xf xf;
    double conf = 0;
    if (!fitRobust(pts, ep.model, xf, conf)) {
        if (ep.model > 0 && fitRobust(pts, 0, xf, conf)) {
        } else {
            return false;
        }
    }
    if (ep.accurate) {
        for (Patch& p : kept) {
            const double cx = (p.x + (ps - 1) / 2.0 + 0.5 - W / 2.0) / unit, cy = (p.y + (ps - 1) / 2.0 + 0.5 - H / 2.0) / unit;
            double ox, oy;
            xf.apply(cx, cy, ox, oy);
            double vx = (ox - cx) * unit, vy = (oy - cy) * unit;
            if (ep.lk) {
                lkRefine(A, B, (int)p.x, (int)p.y, ps, vx, vy, 4);
            } else {
                int bi = 0, bj = 0;
                double best = 1e30;
                const int rx = (int)std::lround(vx), ry = (int)std::lround(vy);
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i) {
                        const int ox2 = rx + i, oy2 = ry + j;
                        if ((int)p.x + ox2 < 0 || (int)p.y + oy2 < 0 || (int)p.x + ox2 + ps > W || (int)p.y + oy2 + ps > H) continue;
                        const double c = patchCost(A, B, (int)p.x, (int)p.y, ps, ox2, oy2, ep.sad);
                        if (c < best) { best = c; bi = ox2; bj = oy2; }
                    }
                vx = bi; vy = bj;
            }
            if (std::fabs(vx) < 4 * W && std::fabs(vy) < 4 * H) { p.vx = vx; p.vy = vy; }
        }
        pts = toPts(kept);
        Xf xf2;
        double conf2 = 0;
        if (fitRobust(pts, ep.model, xf2, conf2) && conf2 >= conf * 0.8) { xf = xf2; conf = conf2; }
        else { pts = toPts(kept); fitRobust(pts, ep.model, xf, conf); }
    }
    const double sc = std::sqrt(std::fabs(xf.a * xf.d - xf.b * xf.c));
    if (!std::isfinite(xf.tx) || !std::isfinite(xf.ty) || sc < 0.85 || sc > 1.18 || std::fabs(xf.c - xf.b) > 0.6) {
        Xf t;
        std::vector<FeatPt> p2 = toPts(kept);
        double c2 = 0;
        if (!fitRobust(p2, 0, t, c2)) return false;
        xf = t; conf = c2; pts = p2;
    }
    out.xf = xf;
    out.conf = conf;
    out.ok = true;
    if (ep.keepPoints) out.pts = std::move(pts);
    return true;
}

}  // namespace analysis
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
