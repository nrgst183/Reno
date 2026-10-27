// Fast algorithms behind the slow built-in effects, part 1: Min and Max (sliding windows), Median (histograms), the disc blur of Defocus (row sums) and the
// blur on a reduced copy (builtins_fast.h).
#include "builtins_fast.h"
#include "builtins_vec.h"
#include "../core/base.h"
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>

namespace reno {
namespace ofx {
namespace builtin {

using namespace v4;

namespace {

// ------------------------------------------------------------------ Min and Max
template <bool MX>
inline V4 vop(V4 a, V4 b) { return MX ? vmax(a, b) : vmin(a, b); }

// The minimum / maximum of every window of n elements of an array of M elements of E floats each (E a multiple of 4), into 'out' (outCount elements): the window is
// doubled in place (f[i] = op(f[i], f[i + s]) for s = 1, 2, 4 ...), then two overlapping windows of the largest power of two that fits make the n. 'f' is used up.
template <bool MX>
void slideFlat(float* f, size_t M, size_t E, int n, size_t outCount, float* out)
{
    int k = 0;
    while ((2 << k) <= n) ++k;                      // 2^k <= n < 2^(k+1)
    for (int j = 0, s = 1; j < k; ++j, s <<= 1) {
        const size_t cnt = (M - (size_t)s) * E;
        const float* b = f + (size_t)s * E;
        for (size_t i = 0; i < cnt; i += 4) st(f + i, vop<MX>(ld(f + i), ld(b + i)));
    }
    const size_t sh = (size_t)(n - (1 << k)) * E, cnt = outCount * E;
    for (size_t i = 0; i < cnt; i += 4) st(out + i, vop<MX>(ld(f + i), ld(f + sh + i)));
}

// Every row's window of 2r + 1 pixels (edges clamped).
template <bool MX>
void horizontalExtreme(const Buf& in, Buf& out, int r)
{
    const int W = in.w, H = in.h;
    out.ensure(W, H);
    parallelRows(H, (size_t)W * 16, [&](int y0, int y1) {
        std::vector<float> tmp((size_t)(W + 2 * r) * 4);
        for (int y = y0; y < y1; ++y) {
            const float* row = &in.px[(size_t)y * W * 4];
            for (int i = 0; i < r; ++i) {
                std::memcpy(&tmp[(size_t)i * 4], row, 16);
                std::memcpy(&tmp[((size_t)(W + r + i)) * 4], row + (size_t)(W - 1) * 4, 16);
            }
            std::memcpy(&tmp[(size_t)r * 4], row, (size_t)W * 16);
            slideFlat<MX>(tmp.data(), (size_t)(W + 2 * r), 4, 2 * r + 1, (size_t)W, &out.px[(size_t)y * W * 4]);
        }
    });
}

// Every column's window of 2r + 1 pixels, 16 columns at a time (so that the strip stays in the cache).
template <bool MX>
void verticalExtreme(const Buf& in, Buf& out, int r)
{
    const int W = in.w, H = in.h, SW = 16;
    out.ensure(W, H);
    const int strips = (W + SW - 1) / SW;
    parallelRows(strips, (size_t)H * 64, [&](int s0, int s1) {
        std::vector<float> buf, res;
        for (int s = s0; s < s1; ++s) {
            const int x0 = s * SW, sw = std::min(SW, W - x0);
            const size_t E = (size_t)sw * 4;
            const int M = H + 2 * r;
            buf.resize((size_t)M * E);
            res.resize((size_t)H * E);
            for (int j = 0; j < M; ++j) {
                const int yy = std::min(H - 1, std::max(0, j - r));
                std::memcpy(&buf[(size_t)j * E], &in.px[((size_t)yy * W + (size_t)x0) * 4], E * 4);
            }
            slideFlat<MX>(buf.data(), (size_t)M, E, 2 * r + 1, (size_t)H, res.data());
            for (int y = 0; y < H; ++y) std::memcpy(&out.px[((size_t)y * W + (size_t)x0) * 4], &res[(size_t)y * E], E * 4);
        }
    });
}

// The half-width of the window in each row of the (round) neighbourhood; the very test the plain loop makes.
// (ax, ay > 0: the ellipse's own radii, the test is then u^2 + v^2 <= 1 with limit 1)
std::vector<int> halfWidths(int rx, int ry, bool round, double limit = 1.0625, double ax = 0, double ay = 0)
{
    std::vector<int> hw((size_t)(2 * ry + 1), rx);
    if (!round) return hw;
    const double nx = ax > 0 ? ax : (double)rx, ny = ay > 0 ? ay : (double)ry;
    for (int j = -ry; j <= ry; ++j) {
        const double v = ny > 0 ? (double)j / ny : 0;
        int h = 0;
        for (int i = rx; i >= 0; --i) {
            const double u = nx > 0 ? (double)i / nx : 0;
            if (!(u * u + v * v > limit + 1e-9)) { h = i; break; }
        }
        hw[(size_t)(j + ry)] = h;
    }
    return hw;
}

template <bool MX>
void extremeImpl(const Buf& src, Buf& out, int rx, int ry, bool round, double ax, double ay)
{
    const int W = src.w, H = src.h;
    const std::vector<int> hw = halfWidths(rx, ry, round, ax > 0 || ay > 0 ? 1.0 : 1.0625, ax, ay);
    bool square = true;
    for (int h : hw) square = square && h == rx;
    if (square) {
        Buf t;
        if (rx > 0) horizontalExtreme<MX>(src, t, rx); else t = src;
        if (ry > 0) verticalExtreme<MX>(t, out, ry); else out = t;
    } else {
        std::map<int, std::vector<int>> groups;
        for (int j = -ry; j <= ry; ++j) groups[hw[(size_t)(j + ry)]].push_back(j);
        Buf acc;
        acc.ensure(W, H);
        std::fill(acc.px.begin(), acc.px.end(), MX ? -FLT_MAX : FLT_MAX);
        Buf hbuf;
        for (const auto& g : groups) {
            const Buf* hp = &src;
            if (g.first > 0) { horizontalExtreme<MX>(src, hbuf, g.first); hp = &hbuf; }
            parallelRows(H, (size_t)W * 16 * g.second.size(), [&](int y0, int y1) {
                for (int y = y0; y < y1; ++y) {
                    float* a = &acc.px[(size_t)y * W * 4];
                    for (int dy : g.second) {
                        const int yy = std::min(H - 1, std::max(0, y + dy));
                        const float* b = &hp->px[(size_t)yy * W * 4];
                        for (size_t i = 0; i < (size_t)W * 4; i += 4) st(a + i, vop<MX>(ld(a + i), ld(b + i)));
                    }
                }
            });
        }
        out.w = W;
        out.h = H;
        out.px.swap(acc.px);
    }
    parallelRows(H, (size_t)W * 8, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* o = &out.px[(size_t)y * W * 4];
            for (int x = 0; x < W; ++x, o += 4)
                for (int k = 0; k < 3; ++k) o[k] = std::min(std::max(o[k], 0.f), o[3]);
        }
    });
}

// ------------------------------------------------------------------ Median
// A Huang histogram median of one channel's keys (16-bit, or 8-bit when WIDE is false): the window slides along each row taking one column out and one in, and the
// median is found from the last one by stepping along the histogram (jumping empty runs of 256 with a coarse histogram when WIDE). 'plane' holds the keys with edge
// pixels repeated rx columns / ry rows outside (Ws wide), so no index needs clamping.
template <bool WIDE>
void medianRows(const uint16_t* plane, int Ws, int rx, int W, int y0, int y1, const std::vector<int>& hw, const float* tbl, float rankFrac,
                const Buf& src, Buf& out, int ch)
{
    const int levels = WIDE ? 65536 : 256;
    std::vector<uint16_t> hist((size_t)levels);
    std::vector<uint16_t> coarse(WIDE ? 256 : 1);
    int count = 0;
    for (int h : hw) count += 2 * h + 1;
    const int half = (int)std::floor(rankFrac * (float)(count - 1) + 0.5f);
    const int nr = (int)hw.size();
    std::vector<const uint16_t*> rows((size_t)nr);
    for (int y = y0; y < y1; ++y) {
        std::fill(hist.begin(), hist.end(), (uint16_t)0);
        if (WIDE) std::fill(coarse.begin(), coarse.end(), (uint16_t)0);
        for (int j = 0; j < nr; ++j) rows[(size_t)j] = plane + (size_t)(y + j) * Ws + rx;
        int m = 0, lt = 0;
        // nothing is below key 0: the first adds leave lt alone
        for (int j = 0; j < nr; ++j)
            for (int i = -hw[(size_t)j]; i <= hw[(size_t)j]; ++i) {
                const unsigned v = rows[(size_t)j][i];
                ++hist[v];
                if (WIDE) ++coarse[v >> 8];
            }
        const float* alphaRow = &src.px[(size_t)y * W * 4 + 3];
        float* outRow = &out.px[(size_t)y * W * 4 + (size_t)ch];
        for (int x = 0; x < W; ++x) {
            if (x > 0)
                for (int j = 0; j < nr; ++j) {
                    const int h = hw[(size_t)j];
                    const int a = rows[(size_t)j][x - 1 - h], b = rows[(size_t)j][x + h];
                    --hist[(size_t)a];
                    ++hist[(size_t)b];
                    if (WIDE) { --coarse[(size_t)a >> 8]; ++coarse[(size_t)b >> 8]; }
                    lt += (b < m) - (a < m);
                }
            for (;;) {
                if (lt > half) {
                    if (WIDE && (m & 255) == 0) {
                        int b = (m >> 8) - 1;
                        while (b > 0 && coarse[(size_t)b] == 0) --b;
                        m = (b << 8) | 255;
                    } else {
                        --m;
                    }
                    lt -= hist[(size_t)m];
                } else if (lt + hist[(size_t)m] <= half) {
                    lt += hist[(size_t)m];
                    ++m;
                    if (WIDE && (m & 255) == 0) {
                        int b = m >> 8;
                        while (b < 255 && coarse[(size_t)b] == 0) ++b;
                        m = b << 8;
                    }
                } else {
                    break;
                }
            }
            outRow[(size_t)x * 4] = std::min(std::max(tbl[m], 0.f), alphaRow[(size_t)x * 4]);
        }
    }
}

}  // namespace

void extremeFilter(const Buf& src, Buf& out, int rx, int ry, bool round, bool maximum, double ax, double ay)
{
    if (src.w <= 0 || src.h <= 0 || src.px.size() < (size_t)src.w * src.h * 4) { out = src; return; }
    if (maximum) extremeImpl<true>(src, out, rx, ry, round, ax, ay);
    else extremeImpl<false>(src, out, rx, ry, round, ax, ay);
}

bool medianFilter(const Buf& src, Buf& out, int rx, int ry, bool round, float rankFrac)
{
    const int W = src.w, H = src.h;
    if (W <= 0 || H <= 0 || src.px.size() < (size_t)W * H * 4) return false;
    const size_t n = (size_t)W * H;
    // all values must be in 0..1 to be keys
    for (size_t i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) {
            const float v = src.px[i * 4 + (size_t)k];
            if (!(v >= -1e-4f && v <= 1.0001f)) return false;
        }
    const std::vector<int> hw = halfWidths(rx, ry, round, 1.2);
    out.ensure(W, H);
    int maxHw = 0;
    for (int h : hw) maxHw = std::max(maxHw, h);
    const int Ws = W + 2 * maxHw, Hs = H + 2 * ry;
    std::vector<uint16_t> plane((size_t)Ws * Hs);
    std::vector<float> tbl8(256), tbl16(65536);
    for (int k = 0; k < 256; ++k) tbl8[(size_t)k] = (float)k / 255.f;
    for (int k = 0; k < 65536; ++k) tbl16[(size_t)k] = (float)k / 65535.f;
    for (int ch = 0; ch < 3; ++ch) {
        bool grid = true;
        for (size_t i = 0; i < n && grid; ++i) {
            const float t = clampf(src.px[i * 4 + (size_t)ch], 0.f, 1.f) * 255.f;
            if (std::fabs(t - std::floor(t + 0.5f)) > 5e-4f) grid = false;
        }
        const float scale = grid ? 255.f : 65535.f;
        parallelRows(Hs, (size_t)Ws * 4, [&](int ya, int yb) {
            for (int yp = ya; yp < yb; ++yp) {
                const int ys = std::min(H - 1, std::max(0, yp - ry));
                uint16_t* d = &plane[(size_t)yp * Ws];
                for (int xp = 0; xp < Ws; ++xp) {
                    const int xs = std::min(W - 1, std::max(0, xp - maxHw));
                    d[xp] = (uint16_t)(int)(clampf(src.px[((size_t)ys * W + (size_t)xs) * 4 + (size_t)ch], 0.f, 1.f) * scale + 0.5f);
                }
            }
        });
        parallelRows(H, (size_t)W * 60, [&](int y0, int y1) {
            if (grid) medianRows<false>(plane.data(), Ws, maxHw, W, y0, y1, hw, tbl8.data(), rankFrac, src, out, ch);
            else medianRows<true>(plane.data(), Ws, maxHw, W, y0, y1, hw, tbl16.data(), rankFrac, src, out, ch);
        });
    }
    for (size_t i = 0; i < n; ++i) out.px[i * 4 + 3] = src.px[i * 4 + 3];
    return true;
}

// ------------------------------------------------------------------ a blur on a reduced copy
int softBlurFactor(double sigma)
{
    return std::max(1, std::min(8, (int)std::floor(sigma / 2.0)));
}

void upsampleBilinear(const std::vector<float>& low, int lowW, int lowH, int c, int f, std::vector<float>& out, int w, int h)
{
    out.resize((size_t)w * h * (size_t)c);
    std::vector<int> x0((size_t)w);
    std::vector<float> fx((size_t)w);
    for (int x = 0; x < w; ++x) {
        const float u = clampf(((float)x + 0.5f) / (float)f - 0.5f, 0.f, (float)(lowW - 1));
        int i = (int)u;
        if (i >= lowW - 1 && lowW > 1) i = lowW - 2;
        x0[(size_t)x] = i;
        fx[(size_t)x] = lowW > 1 ? u - (float)i : 0.f;
    }
    parallelRows(h, (size_t)w * 8, [&](int ya, int yb) {
        for (int y = ya; y < yb; ++y) {
            const float v = clampf(((float)y + 0.5f) / (float)f - 0.5f, 0.f, (float)(lowH - 1));
            int j = (int)v;
            if (j >= lowH - 1 && lowH > 1) j = lowH - 2;
            const float fy = lowH > 1 ? v - (float)j : 0.f;
            const int j1 = std::min(lowH - 1, j + 1);
            const float* r0 = &low[(size_t)j * lowW * (size_t)c];
            const float* r1 = &low[(size_t)j1 * lowW * (size_t)c];
            float* o = &out[(size_t)y * w * (size_t)c];
            if (c == 4) {
                for (int x = 0; x < w; ++x) {
                    const int i = x0[(size_t)x], i1 = std::min(lowW - 1, i + 1);
                    const float t = fx[(size_t)x];
                    const V4 top = lerp(ld(r0 + (size_t)i * 4), ld(r0 + (size_t)i1 * 4), t);
                    const V4 bot = lerp(ld(r1 + (size_t)i * 4), ld(r1 + (size_t)i1 * 4), t);
                    st(o + (size_t)x * 4, lerp(top, bot, fy));
                }
            } else {
                for (int x = 0; x < w; ++x) {
                    const int i = x0[(size_t)x], i1 = std::min(lowW - 1, i + 1);
                    const float t = fx[(size_t)x];
                    const float top = r0[i] + (r0[i1] - r0[i]) * t, bot = r1[i] + (r1[i1] - r1[i]) * t;
                    o[x] = top + (bot - top) * fy;
                }
            }
        }
    });
}

void reduceBox(const Buf& in, Buf& low, int f)
{
    const int W = in.w, H = in.h, lw = (W + f - 1) / f, lh = (H + f - 1) / f;
    low.ensure(lw, lh);
    parallelRows(lh, (size_t)W * f * 8, [&](int j0, int j1) {
        std::vector<float> acc((size_t)lw * 4);
        for (int j = j0; j < j1; ++j) {
            std::fill(acc.begin(), acc.end(), 0.f);
            const int ya = j * f, yb = std::min(H, ya + f);
            for (int y = ya; y < yb; ++y) {
                const float* row = &in.px[(size_t)y * W * 4];
                for (int x = 0; x < W; ++x) {
                    float* a = &acc[(size_t)(x / f) * 4];
                    st(a, add(ld(a), ld(row + (size_t)x * 4)));
                }
            }
            for (int i = 0; i < lw; ++i) {
                const int xw = std::min(W, (i + 1) * f) - i * f;
                st(&low.px[((size_t)j * lw + (size_t)i) * 4], mul(ld(&acc[(size_t)i * 4]), set(1.f / (float)(xw * (yb - ya)))));
            }
        }
    });
}

void softBlur(Buf& b, double sigma)
{
    if (b.w <= 0 || b.h <= 0) return;
    const int f = softBlurFactor(sigma);
    if (f <= 1) { gaussianBlur(b, sigma, sigma); return; }
    const int W = b.w, H = b.h;
    Buf low;
    reduceBox(b, low, f);
    gaussianBlur(low, sigma / f, sigma / f);
    std::vector<float> up;
    upsampleBilinear(low.px, low.w, low.h, 4, f, up, W, H);
    b.px.swap(up);
}

// ------------------------------------------------------------------ Defocus
// Defocus, worked out from VEGAS's pictures (tools/effectref_compare.py): every colour channel above the bloom threshold is brightened by a gain of 1 + gainA x (its
// distance from the threshold to white), then the frame is averaged over the iris, a FLAT disc (or polygon) of pixels whose centre lies within the radius of the point half a
// pixel up and left of the output pixel (what an even-sized kernel does) - then clipped. Outside the frame the source continues as if wrapped on its left and bottom edges
// and clamped on the others (VEGAS's pictures show that, not a symmetric edge).
void defocusFast(const Buf& src, Buf& out, double radius, int sides, double turn, double curvature, double thresh, double gainA)
{
    const int W = src.w, H = src.h;
    out.ensure(W, H);
    radius = std::min(radius, 96.0);
    if (sides > 64) sides = 0;                       // as good as a circle
    struct Span { int dy, lo, hi; float w; };
    std::vector<Span> spans;
    const int levels = curvature == 0 ? 1 : 4;
    const int Rc = (int)std::ceil(radius) + 2;
    std::vector<double> wl((size_t)levels + 1, 0.0);
    // (a ring of the iris, "Disc curvature" below 0: VEGAS's picture of -100 shows hardly anything of it, so it is made mild)
    for (int l = 1; l <= levels; ++l) {
        const double rmid = std::sqrt((l - 0.5) / levels);
        wl[(size_t)l] = levels == 1 ? 1.0 : std::max(0.05, 1.0 + 0.1 * curvature * (1.0 - 2 * rmid));
    }
    for (int l = 1; l <= levels; ++l) {
        const double b = wl[(size_t)l] - (l < levels ? wl[(size_t)l + 1] : 0.0);
        const double rho = std::sqrt((double)l / levels) * radius;
        for (int dy = -Rc; dy <= Rc; ++dy) {
            int lo = 1 << 30, hi = -(1 << 30);
            for (int dx = -Rc; dx <= Rc; ++dx) {
                const double ux = dx + 0.5, uy = dy - 0.5;        // from the iris centre
                const double d = std::sqrt(ux * ux + uy * uy);
                double lim = rho;
                if (sides && d > 0) {
                    const double seg = 2 * kPi / sides, ang = std::atan2(uy, ux) + kPi * 0.5 - turn;
                    lim *= std::cos(kPi / sides) / std::cos(std::fmod(std::fabs(ang), seg) - kPi / sides);
                }
                if (d <= lim + 1e-9) { lo = std::min(lo, dx); hi = std::max(hi, dx); }
            }
            if (lo <= hi && b != 0.0) spans.push_back({dy, lo, hi, (float)b});
        }
    }
    float total = 0;
    for (const Span& s : spans) total += s.w * (float)(s.hi - s.lo + 1);
    if (!(total > 0.f)) { out.px = src.px; return; }
    const int m = Rc + 1, Wp = W + 2 * m, Hp = H + 2 * m;
    std::vector<float> ps((size_t)Hp * (Wp + 1) * 4);
    const float th = (float)thresh, inv1 = th < 0.999f ? 1.f / (1.f - th) : 0.f, gA = (float)gainA;
    parallelRows(Hp, (size_t)Wp * 12, [&](int y0, int y1) {
        for (int yp = y0; yp < y1; ++yp) {
            int ys = yp - m;
            if (ys < 0) ys = 0;
            else if (ys >= H) ys = std::min(H - 1, ys - H);
            float* a = &ps[(size_t)yp * (Wp + 1) * 4];
            V4 run = zero();
            st(a, run);
            for (int xp = 0; xp < Wp; ++xp) {
                int xs = xp - m;
                if (xs < 0) xs = std::max(0, W + xs);
                else if (xs >= W) xs = W - 1;
                const float* p = &src.px[((size_t)ys * W + (size_t)xs) * 4];
                float q[4] = {p[0], p[1], p[2], p[3]};
                if (gA > 0.f && p[3] > 1e-4f) {
                    const float ia = 1.f / p[3];
                    for (int k = 0; k < 3; ++k) {
                        const float w = (p[k] * ia - th) * inv1;
                        if (w > 0.f) q[k] *= 1.f + gA * (w > 1.f ? 1.f : w);
                    }
                }
                run = add(run, ld(q));
                st(a + (size_t)(xp + 1) * 4, run);
            }
        }
    });
    const float invTotal = 1.f / total;
    parallelRows(H, (size_t)W * (8 + spans.size() * 6), [&](int y0, int y1) {
        std::vector<float> num((size_t)W * 4);
        for (int y = y0; y < y1; ++y) {
            std::fill(num.begin(), num.end(), 0.f);
            // eight pixels at a time with their sums kept in registers (each pixel adds the spans in the same order)
            const int W8 = W & ~7;
            for (int x0 = 0; x0 < W8; x0 += 8) {
                V4 acc[8];
                for (int i = 0; i < 8; ++i) acc[i] = zero();
                for (const Span& s : spans) {
                    const float* a = &ps[(size_t)(y + m + s.dy) * (Wp + 1) * 4];
                    const float* pa = a + (size_t)(x0 + m + s.lo) * 4;
                    const float* pb = a + (size_t)(x0 + m + s.hi + 1) * 4;
                    const V4 bw = set(s.w);
                    for (int i = 0; i < 8; ++i) acc[i] = add(acc[i], mul(sub(ld(pb + (size_t)i * 4), ld(pa + (size_t)i * 4)), bw));
                }
                for (int i = 0; i < 8; ++i) st(&num[(size_t)(x0 + i) * 4], acc[i]);
            }
            for (const Span& s : spans) {
                const int row = y + m + s.dy;
                const float* a = &ps[(size_t)row * (Wp + 1) * 4];
                const V4 bw = set(s.w);
                for (int x = W8; x < W; ++x) {
                    const int ia = x + m + s.lo, ib = x + m + s.hi + 1;
                    st(&num[(size_t)x * 4], add(ld(&num[(size_t)x * 4]), mul(sub(ld(a + (size_t)ib * 4), ld(a + (size_t)ia * 4)), bw)));
                }
            }
            for (int x = 0; x < W; ++x) {
                float* o = &out.px[((size_t)y * W + (size_t)x) * 4];
                for (int k = 0; k < 4; ++k) o[k] = std::min(1.f, num[(size_t)x * 4 + (size_t)k] * invTotal);
                if (o[0] > o[3]) o[0] = o[3];
                if (o[1] > o[3]) o[1] = o[3];
                if (o[2] > o[3]) o[2] = o[3];
            }
        }
    });
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
