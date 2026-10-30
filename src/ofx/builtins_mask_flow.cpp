// Built-in stand-in for VEGAS Pro 18's Warp Flow transition (de.magix:WarpFlowTransition). VEGAS's plug-in (MagixCVFx) does not run a dense optical flow; it
// morphs the first frame of the outgoing clip into the last frame of the incoming one (whatever the transition is applied to is not looked at; only the progress
// changes between frames) in these steps, which this file follows:
//   1. both pictures to grey; FAST (9 of 16, threshold 5, non-maximum suppression) corners on the outgoing one, the 4 strongest in each 50 x 50 cell;
//   2. each corner followed into the incoming picture by a pyramidal Lucas-Kanade tracker (17 x 17 window, levels from the picture's width) and back again;
//      it stays when the window's error is small and the round trip lands within a squared distance of 2;
//   3. the picture's centre when nothing stayed, and a ring of fixed points along the edges (no movement);
//   4. a Delaunay triangulation of the points; in each triangle the movement is blended from its corners, which gives a movement for every pixel;
//   5. the outgoing picture is looked up at x + progress * movement and the incoming one at x - (1 - progress) * movement (bilinear with weights in 1/1024),
//      and the two are blended by progress (an empty-alpha side gives way to the other).
// The tracker's own details (it tries several starting guesses per corner) are not known, so a movement of a few pixels can differ from VEGAS's by a fraction of a pixel.
#include "builtins_mask_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace reno {
namespace ofx {
namespace builtin {
namespace {

const char* const kGroup = "Reno Built-in";

struct Pt {
    float x = 0, y = 0;
};

// ---------------------------------------------------------------- grey, pyramids, FAST
typedef std::vector<unsigned char> Grey8;

Grey8 toGrey(const Buf& b)
{
    Grey8 g((size_t)b.w * b.h);
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x) {
            const float* p = b.at(x, y);
            float c[3];
            for (int k = 0; k < 3; ++k) {
                float v = p[k] * 255.f;
                c[k] = std::floor(std::min(255.f, std::max(0.f, v)) + 0.5f);
            }
            const float v = c[0] * 0.299f + c[1] * 0.587f + c[2] * 0.114f;
            g[(size_t)y * b.w + x] = (unsigned char)std::floor(v + 0.5f);
        }
    return g;
}

const int kRing[16][2] = {{0, -3}, {1, -3}, {2, -2}, {3, -1}, {3, 0}, {3, 1}, {2, 2}, {1, 3}, {0, 3}, {-1, 3}, {-2, 2}, {-3, 1}, {-3, 0}, {-3, -1}, {-2, -2}, {-1, -3}};

// Whether a run of 9 ring pixels is all brighter than v + t or all darker than v - t.
bool fastIsCorner(const unsigned char* c, int W, int t)
{
    int d[16];
    const int v = *c;
    for (int k = 0; k < 16; ++k) d[k] = (int)c[kRing[k][1] * W + kRing[k][0]] - v;
    for (int sign = 0; sign < 2; ++sign) {
        int run = 0;
        for (int k = 0; k < 16 + 8; ++k) {
            const int e = d[k & 15];
            const bool on = sign == 0 ? e > t : e < -t;
            run = on ? run + 1 : 0;
            if (run >= 9) return true;
        }
    }
    return false;
}

// The corner score: the largest threshold the pixel still passes at (as OpenCV's FAST scores).
int fastScore(const unsigned char* c, int W, int t0)
{
    int lo = t0, hi = 255;
    while (hi > lo) {
        const int mid = (lo + hi + 1) / 2;
        if (fastIsCorner(c, W, mid)) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

void fastCorners(const Grey8& g, int W, int H, int thr, std::vector<Pt>& pts, std::vector<int>& score)
{
    std::vector<int> sc((size_t)W * H, 0);
    for (int y = 3; y < H - 3; ++y)
        for (int x = 3; x < W - 3; ++x) {
            const unsigned char* c = &g[(size_t)y * W + x];
            if (fastIsCorner(c, W, thr)) sc[(size_t)y * W + x] = fastScore(c, W, thr);
        }
    for (int y = 3; y < H - 3; ++y)
        for (int x = 3; x < W - 3; ++x) {
            const int s = sc[(size_t)y * W + x];
            if (!s) continue;
            bool keep = true;
            for (int dy = -1; dy <= 1 && keep; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if ((dx || dy) && sc[(size_t)(y + dy) * W + x + dx] > s) { keep = false; break; }
            if (keep) {
                Pt p;
                p.x = (float)x;
                p.y = (float)y;
                pts.push_back(p);
                score.push_back(s);
            }
        }
}

struct Img {
    int w = 0, h = 0;
    std::vector<float> v;
    float at(int x, int y) const { return v[(size_t)std::min(h - 1, std::max(0, y)) * w + std::min(w - 1, std::max(0, x))]; }
    float bil(float x, float y) const
    {
        const float fx0 = std::floor(x), fy0 = std::floor(y);
        const int x0 = (int)fx0, y0 = (int)fy0;
        const float ax = x - fx0, ay = y - fy0;
        const float a = at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax;
        const float b = at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax;
        return a * (1 - ay) + b * ay;
    }
};

int refl(int i, int n) { return n == 1 ? 0 : (i < 0 ? -i : (i >= n ? 2 * n - 2 - i : i)); }

Img pyrDown(const Img& a)
{
    Img o;
    o.w = (a.w + 1) / 2;
    o.h = (a.h + 1) / 2;
    o.v.assign((size_t)o.w * o.h, 0.f);
    static const float k[5] = {1, 4, 6, 4, 1};
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x) {
            float s = 0;
            for (int j = 0; j < 5; ++j)
                for (int i = 0; i < 5; ++i) s += k[j] * k[i] * a.v[(size_t)refl(2 * y + j - 2, a.h) * a.w + refl(2 * x + i - 2, a.w)];
            o.v[(size_t)y * o.w + x] = s / 256.f;
        }
    return o;
}

struct Pyr {
    std::vector<Img> lv, gx, gy;
};

Pyr buildPyr(const Grey8& g, int W, int H, int levels)
{
    Pyr p;
    Img a;
    a.w = W;
    a.h = H;
    a.v.resize((size_t)W * H);
    for (size_t i = 0; i < a.v.size(); ++i) a.v[i] = g[i];
    p.lv.push_back(a);
    for (int l = 1; l <= levels; ++l) {
        if (p.lv.back().w < 2 || p.lv.back().h < 2) break;
        p.lv.push_back(pyrDown(p.lv.back()));
    }
    for (const Img& im : p.lv) {
        Img dx = im, dy = im;
        for (int y = 0; y < im.h; ++y)
            for (int x = 0; x < im.w; ++x) {
                const float a0 = im.at(x - 1, y - 1), a1 = im.at(x, y - 1), a2 = im.at(x + 1, y - 1);
                const float b0 = im.at(x - 1, y), b2 = im.at(x + 1, y);
                const float c0 = im.at(x - 1, y + 1), c1 = im.at(x, y + 1), c2 = im.at(x + 1, y + 1);
                dx.v[(size_t)y * im.w + x] = (3 * (a2 - a0) + 10 * (b2 - b0) + 3 * (c2 - c0)) / 32.f;
                dy.v[(size_t)y * im.w + x] = (3 * (c0 - a0) + 10 * (c1 - a1) + 3 * (c2 - a2)) / 32.f;
            }
        p.gx.push_back(dx);
        p.gy.push_back(dy);
    }
    return p;
}

// Pyramidal Lucas-Kanade of one point: where it goes in b (false when it is lost). err: the mean grey difference in the window.
bool trackPoint(const Pyr& a, const Pyr& b, Pt p, Pt& out, float& err)
{
    const int half = 8;
    float gx = 0, gy = 0;
    const int top = (int)a.lv.size() - 1;
    for (int l = top; l >= 0; --l) {
        const Img& A = a.lv[l];
        const Img& B = b.lv[l];
        const float s = 1.f / (float)(1 << l);
        const float px = p.x * s, py = p.y * s;
        if (l < top) { gx *= 2; gy *= 2; }
        float g00 = 0, g01 = 0, g11 = 0;
        std::vector<float> tmpl, tx, ty;
        for (int j = -half; j <= half; ++j)
            for (int i = -half; i <= half; ++i) {
                const float X = px + i, Y = py + j;
                const float t = A.bil(X, Y), ix = a.gx[l].bil(X, Y), iy = a.gy[l].bil(X, Y);
                tmpl.push_back(t);
                tx.push_back(ix);
                ty.push_back(iy);
                g00 += ix * ix;
                g01 += ix * iy;
                g11 += iy * iy;
            }
        const float det = g00 * g11 - g01 * g01;
        const float n = (float)(tmpl.size());
        const float minEig = (g11 + g00 - std::sqrt((g00 - g11) * (g00 - g11) + 4 * g01 * g01)) / (2 * n);
        if (minEig < 1e-4f || det < 1e-9f) { if (l == 0) return false; continue; }
        for (int it = 0; it < 30; ++it) {
            float b0 = 0, b1 = 0;
            size_t k = 0;
            for (int j = -half; j <= half; ++j)
                for (int i = -half; i <= half; ++i, ++k) {
                    const float d = tmpl[k] - B.bil(px + gx + i, py + gy + j);
                    b0 += d * tx[k];
                    b1 += d * ty[k];
                }
            const float dx = (g11 * b0 - g01 * b1) / det, dy = (g00 * b1 - g01 * b0) / det;
            gx += dx;
            gy += dy;
            if (dx * dx + dy * dy < 0.01f) break;
        }
        const float ex = px + gx, ey = py + gy;
        if (ex < -half || ey < -half || ex > B.w + half || ey > B.h + half) return false;
    }
    out.x = p.x + gx;
    out.y = p.y + gy;
    if (out.x < 0 || out.y < 0 || out.x > (float)(b.lv[0].w - 1) || out.y > (float)(b.lv[0].h - 1)) return false;
    float e = 0;
    for (int j = -half; j <= half; ++j)
        for (int i = -half; i <= half; ++i) e += std::fabs(a.lv[0].bil(p.x + i, p.y + j) - b.lv[0].bil(out.x + i, out.y + j));
    err = e / (float)((2 * half + 1) * (2 * half + 1));
    return true;
}

// ---------------------------------------------------------------- the point pairs: from A to B
void findPairs(const Buf& A, const Buf& B, std::vector<Pt>& from, std::vector<Pt>& to)
{
    const int W = A.w, H = A.h;
    const Grey8 ga = toGrey(A), gb = toGrey(B);
    std::vector<Pt> corners;
    std::vector<int> score;
    if (W >= 8 && H >= 8) fastCorners(ga, W, H, 5, corners, score);
    // 4 strongest per 50 x 50 cell
    const int cw = (W + 49) / 50, ch = (H + 49) / 50;
    std::vector<std::vector<int>> cells((size_t)cw * ch);
    for (size_t i = 0; i < corners.size(); ++i) cells[(size_t)((int)corners[i].y / 50) * cw + (int)corners[i].x / 50].push_back((int)i);
    std::vector<int> chosen;
    for (auto& c : cells) {
        std::stable_sort(c.begin(), c.end(), [&](int a, int b) { return score[a] > score[b]; });
        for (size_t k = 0; k < c.size() && k < 4; ++k) chosen.push_back(c[k]);
    }
    int levels = 1 + (int)(std::log2((double)W / 240.0) + 0.5);
    if (levels < 2) levels = 2;
    const Pyr pa = buildPyr(ga, W, H, levels), pb = buildPyr(gb, W, H, levels);
    for (int ci : chosen) {
        const Pt p = corners[ci];
        Pt q, r;
        float e1 = 0, e2 = 0;
        if (!trackPoint(pa, pb, p, q, e1) || e1 >= 10.f) continue;
        if (!trackPoint(pb, pa, q, r, e2)) continue;
        const float dx = r.x - p.x, dy = r.y - p.y;
        if (dx * dx + dy * dy >= 2.0f) continue;
        // a shift under a quarter pixel is what the tracker makes of a change in brightness, not movement
        if (std::fabs(q.x - p.x) < 0.25f && std::fabs(q.y - p.y) < 0.25f) q = p;
        from.push_back(p);
        to.push_back(q);
    }
    if (from.empty()) {
        Pt c;
        c.x = (float)(W / 2);
        c.y = (float)(H / 2);
        from.push_back(c);
        to.push_back(c);
    }
    // the fixed ring along the edges
    const float step = (float)H * 0.05f, lim = (float)(H - 1);
    for (float y = 0; lim > y; y += step) {
        Pt l, r;
        l.x = 0; l.y = y;
        r.x = (float)(W - 1); r.y = y;
        from.push_back(l); to.push_back(l);
        from.push_back(r); to.push_back(r);
    }
    const float wf = (float)W, xl = wf * 0.95f - 1.0f, xs = wf * 0.5f;
    for (float x = wf * 0.05f; xl > x; x += xs) {
        Pt t, b;
        t.x = x; t.y = 0;
        b.x = x; b.y = (float)(H - 1);
        from.push_back(t); to.push_back(t);
        from.push_back(b); to.push_back(b);
    }
}

// ---------------------------------------------------------------- Delaunay (Bowyer-Watson)
struct Tri {
    int a, b, c;
};

std::vector<Tri> delaunay(const std::vector<Pt>& pts)
{
    const int n = (int)pts.size();
    std::vector<double> X, Y;
    for (const Pt& p : pts) { X.push_back(p.x); Y.push_back(p.y); }
    X.push_back(-1e5); Y.push_back(-1e5);
    X.push_back(1e5); Y.push_back(-1e5);
    X.push_back(0); Y.push_back(1e5);
    std::vector<Tri> tris;
    tris.push_back({n, n + 1, n + 2});
    auto inCircle = [&](const Tri& t, double px, double py) {
        double ax = X[t.a] - px, ay = Y[t.a] - py, bx = X[t.b] - px, by = Y[t.b] - py, cx = X[t.c] - px, cy = Y[t.c] - py;
        double det = (ax * ax + ay * ay) * (bx * cy - cx * by) - (bx * bx + by * by) * (ax * cy - cx * ay) + (cx * cx + cy * cy) * (ax * by - bx * ay);
        const double orient = (X[t.b] - X[t.a]) * (Y[t.c] - Y[t.a]) - (Y[t.b] - Y[t.a]) * (X[t.c] - X[t.a]);
        return orient > 0 ? det > 1e-9 : det < -1e-9;
    };
    for (int i = 0; i < n; ++i) {
        bool dup = false;
        for (int j = 0; j < i && !dup; ++j) dup = std::fabs(X[i] - X[j]) < 1e-6 && std::fabs(Y[i] - Y[j]) < 1e-6;
        if (dup) continue;
        std::vector<Tri> keep;
        std::vector<std::pair<int, int>> edges;
        for (const Tri& t : tris) {
            if (inCircle(t, X[i], Y[i])) {
                edges.push_back({t.a, t.b});
                edges.push_back({t.b, t.c});
                edges.push_back({t.c, t.a});
            } else {
                keep.push_back(t);
            }
        }
        for (size_t e = 0; e < edges.size(); ++e) {
            bool shared = false;
            for (size_t f = 0; f < edges.size() && !shared; ++f)
                shared = f != e && edges[f].first == edges[e].second && edges[f].second == edges[e].first;
            if (!shared) keep.push_back({edges[e].first, edges[e].second, i});
        }
        tris.swap(keep);
    }
    std::vector<Tri> out;
    for (const Tri& t : tris)
        if (t.a < n && t.b < n && t.c < n) out.push_back(t);
    return out;
}

// ---------------------------------------------------------------- the movement of every pixel
struct Field {
    int w = 0, h = 0;
    std::vector<float> f;           // x, y per pixel: where the incoming picture's point is, as an offset of the outgoing one's
};

void denseField(const std::vector<Pt>& P, const std::vector<Pt>& Q, int W, int H, Field& fld)
{
    fld.w = W;
    fld.h = H;
    fld.f.assign((size_t)W * H * 2, 0.f);
    std::vector<unsigned char> done((size_t)W * H, 0);
    const std::vector<Tri> tris = delaunay(P);
    for (const Tri& t : tris) {
        const Pt a = P[t.a], b = P[t.b], c = P[t.c];
        const int x0 = std::max(0, (int)std::ceil(std::min(a.x, std::min(b.x, c.x))));
        const int x1 = std::min(W - 1, (int)std::floor(std::max(a.x, std::max(b.x, c.x))));
        const int y0 = std::max(0, (int)std::ceil(std::min(a.y, std::min(b.y, c.y))));
        const int y1 = std::min(H - 1, (int)std::floor(std::max(a.y, std::max(b.y, c.y))));
        float det = (a.y - c.y) * (c.x - b.x);
        det = det + (a.x - c.x) * (b.y - c.y);
        if (det == 0.f) continue;
        const Pt qa = Q[t.a], qb = Q[t.b], qc = Q[t.c];
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                if (done[(size_t)y * W + x]) continue;
                const float dx = (float)x - c.x, dy = (float)y - c.y;
                float un = dy * (c.x - b.x);
                un = un + dx * (b.y - c.y);
                float vn = (c.y - a.y) * dx;
                vn = vn + dy * (a.x - c.x);
                const float u = un / det, v = vn / det;
                float w = 1.f - u;
                w = w - v;
                if (!(u >= 0 && v >= 0 && w >= 0 && u <= 1 && v <= 1 && w <= 1)) continue;
                float sx = u * qa.x;
                sx = sx + v * qb.x;
                sx = sx + w * qc.x;
                float sy = v * qb.y;
                sy = sy + u * qa.y;
                sy = sy + w * qc.y;
                done[(size_t)y * W + x] = 1;
                fld.f[((size_t)y * W + x) * 2] = (float)x - sx;
                fld.f[((size_t)y * W + x) * 2 + 1] = (float)y - sy;
            }
    }
}

void hueToRgb(double h, float& r, float& g, float& b)
{
    h = h - std::floor(h);
    const double s = h * 6;
    const int i = (int)s;
    const float f = (float)(s - i);
    switch (i % 6) {
    case 0: r = 1; g = f; b = 0; break;
    case 1: r = 1 - f; g = 1; b = 0; break;
    case 2: r = 0; g = 1; b = f; break;
    case 3: r = 0; g = 1 - f; b = 1; break;
    case 4: r = f; g = 0; b = 1; break;
    default: r = 1; g = 0; b = 1 - f; break;
    }
}

// A cache: the field only depends on the two pictures.
std::mutex gMu;
unsigned long long gKey = 0;
int gW = 0, gH = 0;
Field gField;

unsigned long long hashBufs(const Buf& a, const Buf& b)
{
    unsigned long long h = 1469598103934665603ULL;
    for (const Buf* bp : {&a, &b})
        for (size_t i = 0; i < bp->px.size(); ++i) {
            unsigned u;
            std::memcpy(&u, &bp->px[i], 4);
            h = (h ^ u) * 1099511628211ULL;
        }
    return h;
}

// bilinear lookup with weights in 1/1024 (as VEGAS's remap), 0 outside
void remapPlane(const Buf& s, const Field& f, float p, bool fromSide, Buf& out)
{
    const int W = s.w, H = s.h;
    const float K = 1024.f;
    const size_t size = s.px.size();
    const float* flat = s.px.data();
    const float q = 1.f - p;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float fx0 = f.f[((size_t)y * W + x) * 2], fy0 = f.f[((size_t)y * W + x) * 2 + 1];
            float mx, my;
            if (fromSide) {
                mx = fx0 * p;
                mx = mx + (float)x;
                my = fy0 * p;
                my = my + (float)y;
            } else {
                float tx = fx0 * q, ty = fy0 * q;
                mx = (float)x - tx;
                my = (float)y - ty;
            }
            float* o = out.at(x, y);
            if (!(mx > -0.4f && mx < (float)W - 0.6f && my > -0.4f && my < (float)H - 0.6f)) {
                o[0] = o[1] = o[2] = o[3] = 0.f;
                continue;
            }
            const int ix = (int)mx, iy = (int)my;
            const float fx = std::max(0.f, mx - (float)ix), fy = std::max(0.f, my - (float)iy);
            const float ifx = 1.f - fx, ify = 1.f - fy;
            const float w00 = (float)(int)((ifx * ify) * K), w10 = (float)(int)((ify * fx) * K);
            const float w01 = (float)(int)((ifx * fy) * K), w11 = (float)(int)((fy * fx) * K);
            const int cx = std::min(W - 1, std::max(0, ix)), cy = std::min(H - 1, std::max(0, iy));
            const size_t base = ((size_t)cy * W + cx) * 4;
            for (int k = 0; k < 4; ++k) {
                const size_t i00 = base + k, i10 = i00 + 4, i01 = i00 + (size_t)W * 4, i11 = i01 + 4;
                const float p00 = flat[i00];
                const float p10 = i10 < size ? flat[i10] : p00;
                const float p01 = i01 < size ? flat[i01] : p00;
                const float p11 = i11 < size ? flat[i11] : p01;
                float sum = w10 * p10;
                sum = sum + w00 * p00;
                sum = sum + w01 * p01;
                sum = sum + w11 * p11;
                o[k] = sum * 0.0009765625f;
            }
        }
    });
}

void warpFlowWith(Ctx& c, bool showField)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    if (c.src.w != W || c.src.h != H || c.src2.w != W || c.src2.h != H) {
        c.out.px = c.src.px;
        return;
    }
    const float p = (float)clampd(c.progress, 0, 1);
    Field fld;
    {
        const unsigned long long key = hashBufs(c.src, c.src2);
        std::lock_guard<std::mutex> lk(gMu);
        if (key != gKey || gW != W || gH != H || gField.f.empty()) {
            std::vector<Pt> from, to;
            findPairs(c.src, c.src2, from, to);
            denseField(from, to, W, H, gField);
            gKey = key;
            gW = W;
            gH = H;
        }
        fld = gField;
    }
    if (showField) {
        forRows(H, W, [&](int y) {
            for (int x = 0; x < W; ++x) {
                const double dx = fld.f[((size_t)y * W + x) * 2], dy = fld.f[((size_t)y * W + x) * 2 + 1];
                float* o = c.out.at(x, y);
                float r, g, b;
                hueToRgb(std::atan2(dy, dx) / (2 * kPi) + 0.5, r, g, b);
                const float m = (float)std::min(1.0, std::hypot(dx / W, dy / H) * 8);
                o[0] = r * m; o[1] = g * m; o[2] = b * m; o[3] = 1.f;
            }
        });
        return;
    }
    Buf a, b;
    a.resize(W, H);
    b.resize(W, H);
    remapPlane(c.src, fld, p, true, a);
    remapPlane(c.src2, fld, p, false, b);
    const float q = 1.f - p;
    const bool cut = c.levels;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* pa = a.at(x, y);
            const float* pb = b.at(x, y);
            float* o = c.out.at(x, y);
            for (int k = 0; k < 4; ++k) {
                float v;
                if (pa[3] == 0.f) v = pb[k];
                else if (pb[3] == 0.f) v = pa[k];
                else {
                    v = p * pb[k];
                    v = v + pa[k] * q;
                }
                if (cut) v = std::floor(v * 255.f) / 255.f;
                o[k] = v;
            }
        }
    });
}

void warpFlow(Ctx& c) { warpFlowWith(c, false); }

}  // namespace

void warpFlowForTest(Ctx& c, int, double, double, bool showField) { warpFlowWith(c, showField); }

void addMaskFlow(std::vector<Def>& out)
{
    Def d;
    d.id = "de.magix:WarpFlowTransition";
    d.label = "VEGAS Warp Flow";
    d.group = kGroup;
    d.description = "Morphs the outgoing clip's first picture into the incoming clip's last one along the movement found between them (tracked corners), "
                    "instead of dissolving. Works best with no camera movement.";
    d.transition = true;
    d.temporal = true;      // (the pictures are the clips' first and last frames, not the frame being drawn)
    // (no parameters: VEGAS's has none)
    d.render = warpFlow;
    out.push_back(d);
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
