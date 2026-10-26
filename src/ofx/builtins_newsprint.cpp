// VEGAS Newsprint: a halftone as VEGAS's own draws it (read off its Vfx1 plug-in). 8-bit, four channels, the frame in the library's order (top row first):
//   - P = max(W, H) * Dot size / 20 + 3 pixels is the dot pitch; the picture is shrunk by averaging boxes of P pixels (14-bit weights, whole-level results) to about W / P by H / P cells;
//   - dots sit on a grid of rows P apart, every second row half a pitch over, centred on the frame (fractional part of half the leftover width / height taken off); each dot takes the
//     colour at its cell (bilinear in the small picture, 14 bits) and is an ellipse of radius 0.625 P (1 - brightness) in the colour (Color), black (Monochrome) or the three inks of an
//     offset print (Offset color: yellow up and left, magenta up and right, cyan down, a quarter of a radius out);
//   - the ellipse's edge is a pixel wide, its coverage from a table (area of a circle's segment), laid on the paper by removing (255 - colour) * coverage / 256 from each channel;
//   - Dot sizes under 0.15 are faded in from the picture (Monochrome: from its grey); at 0 it is the picture itself.
#include "builtins_impl.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {
namespace {

typedef int64_t i64;

// The coverage of an edge pixel (0..255) by how far the pixel is inside (k) the ellipse's edge: a circular segment's area.
struct CoverTable {
    int t[256];
    CoverTable()
    {
        for (int k = 0; k < 256; ++k) {
            const double x = k * 0.00390625 - 0.5;
            if (k == 128) {
                t[k] = 128;
                continue;
            }
            const double a = std::fabs(x);
            const double ac = std::acos(a + a), s = std::sin(ac);
            const double x6 = ac / 3.14159265358979 * 0.7853981633974475;
            const double h = 0.5 * s, x1 = h * a * h;
            double v = k < 128 ? (0.7853981633974475 - (x6 - x1)) / 0.7853981633974475 * 255 : (x6 - x1) / 0.7853981633974475 * 255;
            v = 0.0 > v ? 0.0 : std::min(255.0, v);
            t[k] = (int)v;
        }
    }
};

inline int rnd(double v) { return (int)std::nearbyint(v); }

// The weights of the source columns for each of n cells of a box shrink: the cell i takes [i * step, i * step + step) (14-bit
// units), each source pixel by the part of it inside.
void boxWeights(int nOut, int nIn, i64 step, std::vector<std::vector<std::pair<int, int> > >& res)
{
    res.assign((size_t)nOut, std::vector<std::pair<int, int> >());
    for (int i = 0; i < nOut; ++i) {
        const i64 a = (i64)i * step, b = a + step;
        i64 j = a >> 14;
        while ((j << 14) < b && j < nIn) {
            const i64 lo = std::max(a, j << 14), hi = std::min(b, (j + 1) << 14);
            res[(size_t)i].push_back(std::make_pair((int)j, (int)(hi - lo)));
            ++j;
        }
    }
}

struct Pic {
    int w = 0, h = 0;
    std::vector<int> p;     // 4 ints per pixel, B G R A (VEGAS's order)
    int* at(int x, int y) { return &p[((size_t)y * w + x) * 4]; }
    const int* at(int x, int y) const { return &p[((size_t)y * w + x) * 4]; }
};

// The picture shrunk into a rect of x2 by y2 cells (a part of a cell at the end), cells P pixels wide: sw x sh of them.
void boxShrink(const Pic& M, Pic& S, int sw, int sh, double x2, double y2)
{
    const int W = M.w, H = M.h;
    const i64 dRx = (i64)(x2 * 16384), dRy = (i64)(y2 * 16384);
    S.w = sw;
    S.h = sh;
    S.p.assign((size_t)sw * sh * 4, 0);
    if (dRx <= 0 || dRy <= 0) return;
    const i64 sx = (i64)((double)((i64)W << 14) * 16384.0 / (double)dRx), sy = (i64)((double)((i64)H << 14) * 16384.0 / (double)dRy);
    const i64 nrm = (i64)((double)(sx * sy) * 6.103515625e-05);
    if (nrm <= 0) return;
    const double norm = 1.0 / (double)nrm;
    std::vector<std::vector<std::pair<int, int> > > ax, ay;
    boxWeights(sw, W, sx, ax);
    boxWeights(sh, H, sy, ay);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            i64 acc[4] = {0, 0, 0, 0};
            for (size_t a = 0; a < ay[(size_t)y].size(); ++a)
                for (size_t b = 0; b < ax[(size_t)x].size(); ++b) {
                    const int wy = ay[(size_t)y][a].second, wx = ax[(size_t)x][b].second;
                    const i64 w = (wx == 16384 && wy == 16384) ? 16384 : (i64)((double)((i64)wx * wy) * 6.103515625e-05);
                    const int* q = M.at(ax[(size_t)x][b].first, ay[(size_t)y][a].first);
                    for (int k = 0; k < 4; ++k) acc[k] += (i64)q[k] * w;
                }
            int* o = S.at(x, y);
            for (int k = 0; k < 4; ++k) o[k] = (int)std::floor((double)acc[k] * norm + 1.5e-8 + 0.5);
        }
}

// The small picture at the place (sx, sy) in 14-bit cells: bilinear (14-bit weights); at the last row / column the nearer ones.
void sampleS(const Pic& S, i64 sx, i64 sy, int* out)
{
    const int sw = S.w, sh = S.h;
    const i64 x = sx >> 14, fx = sx & 0x3fff, y = sy >> 14, fy = sy & 0x3fff;
    if (x + 1 < sw && y + 1 < sh) {
        const i64 w00 = ((16384 - fy) * (16384 - fx)) >> 4, w01 = ((16384 - fy) * fx) >> 4, w10 = ((16384 - fx) * fy) >> 4, w11 = (fx * fy) >> 4;
        const int *p00 = S.at((int)x, (int)y), *p01 = S.at((int)x + 1, (int)y), *p10 = S.at((int)x, (int)y + 1), *p11 = S.at((int)x + 1, (int)y + 1);
        for (int k = 0; k < 4; ++k) out[k] = (int)((p00[k] * w00 + p01[k] * w01 + p10[k] * w10 + p11[k] * w11 + 0x800000) >> 24);
        return;
    }
    const int xx = (int)std::min<i64>(x, sw - 1), yy = (int)std::min<i64>(y, sh - 1);
    const int* p = S.at(xx, yy);
    if (y + 1 >= sh && x + 1 >= sw) {
        for (int k = 0; k < 4; ++k) out[k] = p[k];
    } else if (y + 1 >= sh) {
        const int* q = S.at(xx + 1, yy);
        for (int k = 0; k < 4; ++k) out[k] = (int)((p[k] * (16384 - fx) + q[k] * fx + 0x2000) >> 14);
    } else {
        const int* q = S.at(xx, yy + 1);
        for (int k = 0; k < 4; ++k) out[k] = (int)((p[k] * (16384 - fy) + q[k] * fy + 0x2000) >> 14);
    }
}

// One dot: the ellipse at (X, Y) with the radii rx, ry takes (255 - col) * cover / 256 off each of the three colour channels.
void ellipse(Pic& D, const CoverTable& tab, double X, double Y, double rx, double ry, const int* col)
{
    const int W = D.w, H = D.h;
    int y0 = rnd(Y - ry - 0.499999985), x0 = rnd(X - rx - 0.499999985), y1 = rnd(Y + ry + 0.499999985), x1 = rnd(X + rx + 0.499999985);
    y0 = std::min(std::max(y0, 0), H - 1);
    y1 = std::min(std::max(y1, 0), H);
    x0 = std::min(std::max(x0, 0), W - 1);
    x1 = std::min(std::max(x1, 0), W);
    if (y1 - y0 < 1 || x1 - x0 < 1) return;
    const double k = ry / rx;
    const double in0 = std::max(ry - 0.7, 0.0), inn = in0 * in0;
    for (int y = y0; y < y1; ++y) {
        const double ddy = (double)y + 0.0 - Y + 0.5, dy = ddy * ddy;
        for (int x = x0; x < x1; ++x) {
            const double dx = ((double)x + 0.5 - X) * k, d2 = dx * dx + dy;
            int* d = D.at(x, y);
            if (inn > d2) {
                for (int ch = 0; ch < 3; ++ch) {
                    const int t = (col[ch] - 255) * 255, q = t / 256;
                    const int v = d[ch] + q;
                    d[ch] = v > 0 ? v : 0;
                }
            } else {
                const double dd = std::sqrt(d2);
                int cov;
                if (dd > ry + 0.5) cov = 0;
                else if (dd < ry - 0.5) cov = 255;
                else cov = tab.t[(rnd((dd - ry) * 255) + 128) & 255];
                for (int ch = 0; ch < 3; ++ch) {
                    const int t = (255 - col[ch]) * cov, q = t / 256;
                    const int v = d[ch] - q;
                    d[ch] = v > 0 ? v : 0;
                }
            }
        }
    }
}

}  // namespace

void vegasNewsprint(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double dot = clampd(c.d("DotSize"), 0, 1);
    const int type = c.i("Type");
    if (dot <= 0.0 || W < 2 || H < 2) {
        c.out.px = c.src.px;
        return;
    }
    // the picture in 8 bits, B G R A (premultiplied, as it lies)
    Pic M;
    M.w = W;
    M.h = H;
    M.p.resize((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float* p = c.src.at(x, y);
            int* q = M.at(x, y);
            q[0] = rnd(clamp01(p[2]) * 255.f);
            q[1] = rnd(clamp01(p[1]) * 255.f);
            q[2] = rnd(clamp01(p[0]) * 255.f);
            q[3] = rnd(clamp01(p[3]) * 255.f);
        }
    const double P = std::max(W, H) * dot / 20.0 + 3.0;
    const int sw = rnd((double)W / P + 0.499999985), sh = rnd((double)H / P + 0.499999985);
    const double x2 = (double)W / P, y2 = (double)H / P;
    if (sw < 2 || sh < 2 || x2 < 1.5 || y2 < 1.5) {
        c.out.px = c.src.px;
        return;
    }
    static const CoverTable tab;
    Pic S;
    boxShrink(M, S, sw, sh, x2, y2);
    Pic D;
    D.w = W;
    D.h = H;
    D.p.assign((size_t)W * H * 4, 255);
    const int mode = type == 2 ? 2 : (type == 0 ? 1 : 0);
    const int nbx = (int)x2, nby = (int)y2;
    const i64 maxX = (i64)((x2 - 1.5) * 16384), maxY = (i64)((y2 - 1.5) * 16384);
    const double x3 = (W - x2) * 0.5, x7 = (H - y2) * 0.5;
    const double fxo = -std::fmod(x3, 1.0), fyo = -std::fmod(x7, 1.0);
    const int par = ~(int)x7;
    for (int j = -1; j < nby + 3; ++j) {
        const double yy = (double)j + 0.0 + fyo;
        const double shift = ((j + par) & 1) ? 0.5 : 0.0;
        i64 sy = (i64)(yy * 16384);
        sy = sy < 0 ? 0 : std::min(sy, maxY);
        for (int i = -1; i < nbx + 3; ++i) {
            const double xx = (double)i + 0.0 + shift + fxo;
            i64 sx = (i64)(xx * 16384);
            sx = sx < 0 ? 0 : std::min(sx, maxX);
            int cc[4];
            sampleS(S, sx, sy, cc);
            const double lum = 1.0 - (cc[0] * 0.0004470588235294118 + cc[1] * 0.0023019607843137255 + cc[2] * 0.001172549019607843);
            const double rx = P * 0.5 * lum * 1.25, ry = P * 0.5 * lum * 1.25;
            if (!(rx > 0.00392156862745098 && ry > 0.00392156862745098)) continue;
            const double X = xx * P, Y = yy * P;
            if (mode == 0) {
                const int col[3] = {0, 0, 0};
                ellipse(D, tab, X, Y, rx, ry, col);
            } else if (mode == 1) {
                ellipse(D, tab, X, Y, rx, ry, cc);
            } else {
                const int c0[3] = {cc[0], 255, 255}, c1[3] = {255, cc[1], 255}, c2[3] = {255, 255, cc[2]};
                ellipse(D, tab, X - rx * 0.25, Y - ry * 0.25, rx, ry, c0);
                ellipse(D, tab, X + rx * 0.25, Y - ry * 0.25, rx, ry, c1);
                ellipse(D, tab, X, Y + ry * 0.25, rx, ry, c2);
            }
        }
    }
    if (dot < 0.15) {
        const int t = rnd((1.0 - dot / 0.15) * 255 + 0.5 - 0.499999985) & 255;
        auto mix = [&](int A, int B) {
            const int q = (B - A) * t + 128;
            const int v = A + ((q + (q >> 8)) >> 8);
            return v < 0 ? 0 : (v > 255 ? 255 : v);
        };
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int* d = D.at(x, y);
                const int* s = M.at(x, y);
                int src[4] = {s[0], s[1], s[2], s[3]};
                if (mode == 0) {
                    const double g = std::min((double)s[0] * 0.114 + (double)s[1] * 0.587 + (double)s[2] * 0.299, (double)s[3]);
                    src[0] = src[1] = src[2] = (int)g;
                }
                const int a = mix(d[3], src[3]);
                for (int ch = 0; ch < 3; ++ch) d[ch] = std::min(mix(d[ch], src[ch]), a);
                d[3] = a;
            }
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int* q = D.at(x, y);
            float* o = c.out.at(x, y);
            o[0] = (float)q[2] / 255.f;
            o[1] = (float)q[1] / 255.f;
            o[2] = (float)q[0] / 255.f;
            o[3] = (float)q[3] / 255.f;
        }
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
