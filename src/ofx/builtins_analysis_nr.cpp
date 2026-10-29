// Built-in versions of VEGAS Pro 18's noise reducers:
//
//   de.magix:DenoisingNLM                           "VEGAS Denoise" (MagixCVFx.ofx): a non-local-means denoiser in YUV (BT.601, the matrix in its OpenCL kernels) with
//                                                   Luminance and Color strengths (0 to 100, 3 each) and a Sharpening Amount (0 to 1). Names, defaults and ranges are the plug-in's.
//   com.sonycreativesoftware:smart_noise_reduction  Sony's SPICA noise reduction. Only its identifier is in the VEGAS Pro 18 files here, so the one parameter it surely
//                                                   has (the noise sensitivity of its sibling Smart Upscale) and an enhancement switch are guesses.
//
// The method is Reno's own: every pixel becomes the average of the pixels round it (7 x 7 search window), weighted by how much the 5 x 5 patch round each looks like its
// own patch (patch distances from box filters of the squared difference per offset, so cost does not depend on patch size). Luminance is done at full size, the two colour
// planes at half size (colour noise is soft). The strengths are the noise levels (in 1/255ths of the range) the denoiser expects.
#include "builtins_analysis_internal.h"
#include "../core/base.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
namespace analysis {
namespace {

inline int mirrorIndex(int i, int n)
{
    if (n <= 1) return 0;
    const int period = 2 * n - 2;
    int m = i % period;
    if (m < 0) m += period;
    return m < n ? m : period - m;
}

// A set of planes padded by 'm' pixels (mirrored) all round.
struct Padded {
    int w = 0, h = 0, m = 0, pw = 0, ph = 0, k = 0;
    std::vector<float> data;                // k planes of pw x ph
    float* plane(int i) { return data.data() + (size_t)i * pw * ph; }
    const float* plane(int i) const { return data.data() + (size_t)i * pw * ph; }
};

void padPlanes(const std::vector<const float*>& planes, int w, int h, int m, Padded& p)
{
    p.w = w; p.h = h; p.m = m; p.pw = w + 2 * m; p.ph = h + 2 * m; p.k = (int)planes.size();
    p.data.resize((size_t)p.k * p.pw * p.ph);
    for (int i = 0; i < p.k; ++i) {
        float* dst = p.plane(i);
        const float* src = planes[(size_t)i];
        for (int y = 0; y < p.ph; ++y) {
            const float* srow = src + (size_t)mirrorIndex(y - m, h) * w;
            float* drow = dst + (size_t)y * p.pw;
            for (int x = 0; x < p.pw; ++x) drow[x] = srow[mirrorIndex(x - m, w)];
        }
    }
}

struct WeightTable {
    float v[1025];
    WeightTable() { for (int i = 0; i <= 1024; ++i) v[i] = std::exp(-(float)i / 102.4f); }       // exp(-t) for t in 0..10
    float operator()(float t) const
    {
        if (t >= 10.f) return 0.f;
        const float f = t * 102.4f;
        const int i = (int)f;
        return v[i] + (v[i + 1] - v[i]) * (f - i);
    }
};

// Non-local means over the padded planes. Weights come from the patch distance summed over all planes; every plane is averaged with them.
// sigma: the noise level (the distance of two patches of the same thing is 2 sigma^2 on average), out: k planes of w x h.
void nlm(const Padded& P, int S, int R, float sigma, float hFactor, std::vector<float>& out)
{
    static const WeightTable table;
    const int w = P.w, h = P.h, k = P.k, m = P.m, pw = P.pw;
    out.assign((size_t)k * w * h, 0.f);
    const float s2 = sigma * sigma;
    const float base = 2.f * s2;
    const float tinv = 1.f / std::max(1e-12f, hFactor * hFactor * s2);
    const float norm = 1.f / ((float)k * (2 * R + 1) * (2 * R + 1));
    parallelRows(h, (size_t)w * (2 * S + 1) * (2 * S + 1) * 4, [&](int y0, int y1) {
        const int rows = (y1 - y0) + 2 * R;
        std::vector<float> D((size_t)rows * (w + 2 * R)), Hs((size_t)rows * w), den((size_t)(y1 - y0) * w, 1.f), vs((size_t)w);
        std::vector<float> num((size_t)k * (y1 - y0) * w);
        for (int i = 0; i < k; ++i)
            for (int y = y0; y < y1; ++y)
                std::memcpy(&num[((size_t)i * (y1 - y0) + (y - y0)) * w], P.plane(i) + (size_t)(y + m) * pw + m, (size_t)w * sizeof(float));
        for (int dy = -S; dy <= S; ++dy)
            for (int dx = -S; dx <= S; ++dx) {
                if (dx == 0 && dy == 0) continue;
                for (int r = 0; r < rows; ++r) {
                    const int y = y0 - R + r;
                    float* d = &D[(size_t)r * (w + 2 * R)];
                    for (int x = 0; x < w + 2 * R; ++x) d[x] = 0.f;
                    for (int i = 0; i < k; ++i) {
                        const float* a = P.plane(i) + (size_t)(y + m) * pw + m - R;
                        const float* b = P.plane(i) + (size_t)(y + m + dy) * pw + m - R + dx;
                        for (int x = 0; x < w + 2 * R; ++x) {
                            const float t = b[x] - a[x];
                            d[x] += t * t;
                        }
                    }
                }
                for (int r = 0; r < rows; ++r) {
                    const float* d = &D[(size_t)r * (w + 2 * R)];
                    float* hs = &Hs[(size_t)r * w];
                    float acc = 0.f;
                    for (int x = 0; x < 2 * R + 1; ++x) acc += d[x];
                    hs[0] = acc;
                    for (int x = 1; x < w; ++x) {
                        acc += d[x + 2 * R] - d[x - 1];
                        hs[x] = acc;
                    }
                }
                for (int x = 0; x < w; ++x) {
                    float s = 0.f;
                    for (int r = 0; r < 2 * R + 1; ++r) s += Hs[(size_t)r * w + x];
                    vs[(size_t)x] = s;
                }
                for (int y = y0; y < y1; ++y) {
                    const int r0 = y - y0;
                    if (r0 > 0) {
                        const float* add = &Hs[(size_t)(r0 + 2 * R) * w];
                        const float* sub = &Hs[(size_t)(r0 - 1) * w];
                        for (int x = 0; x < w; ++x) vs[(size_t)x] += add[x] - sub[x];
                    }
                    float* dn = &den[(size_t)(y - y0) * w];
                    for (int x = 0; x < w; ++x) {
                        const float d = vs[(size_t)x] * norm - base;
                        if (d >= 10.f / tinv) continue;
                        const float wt = table(d > 0.f ? d * tinv : 0.f);
                        if (wt <= 0.f) continue;
                        dn[x] += wt;
                        for (int i = 0; i < k; ++i)
                            num[((size_t)i * (y1 - y0) + (y - y0)) * w + x] += wt * P.plane(i)[(size_t)(y + m + dy) * pw + m + x + dx];
                    }
                }
            }
        for (int i = 0; i < k; ++i)
            for (int y = y0; y < y1; ++y) {
                float* o = &out[((size_t)i * h + y) * w];
                const float* nu = &num[((size_t)i * (y1 - y0) + (y - y0)) * w];
                const float* dn = &den[(size_t)(y - y0) * w];
                for (int x = 0; x < w; ++x) o[x] = nu[x] / dn[x];
            }
    });
}

// The denoiser: sigmaY / sigmaC are the noise levels (0..1 of the range), S the search radius, R the patch radius, sharpen the amount (0..1).
void denoise(Ctx& c, double sigmaY, double sigmaC, int S, int R, double sharpen)
{
    const int W = c.out.w, H = c.out.h;
    if (c.src.px.size() != c.out.px.size() || W < 4 || H < 4) { c.out.px = c.src.px; return; }
    if (sigmaY <= 0 && sigmaC <= 0 && sharpen <= 0) { c.out.px = c.src.px; return; }
    const size_t n = (size_t)W * H;
    std::vector<float> Y(n), U(n), V(n);
    std::vector<float> A(n);
    forRows(H, W, [&](int y) {
        const float* s = c.src.at(0, y);
        for (int x = 0; x < W; ++x, s += 4) {
            float r, g, b, a;
            unpremul(s, r, g, b, a);
            float yy, u, v;
            rgbToYuv(r, g, b, yy, u, v);
            const size_t i = (size_t)y * W + x;
            Y[i] = yy; U[i] = u; V[i] = v; A[i] = a;
        }
    });
    const int m = S + R;
    std::vector<float> Yd;
    if (sigmaY > 0) {
        Padded p;
        padPlanes({Y.data()}, W, H, m, p);
        std::vector<float> o;
        nlm(p, S, R, (float)sigmaY, 0.85f, o);
        Yd = std::move(o);
    }
    std::vector<float> Ud, Vd;
    if (sigmaC > 0) {
        const int cw = (W + 1) / 2, ch = (H + 1) / 2;
        std::vector<float> cu((size_t)cw * ch), cv((size_t)cw * ch);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                const int x0 = std::min(2 * x, W - 1), x1 = std::min(2 * x + 1, W - 1), y0 = std::min(2 * y, H - 1), y1 = std::min(2 * y + 1, H - 1);
                const size_t a = (size_t)y0 * W + x0, b = (size_t)y0 * W + x1, c2 = (size_t)y1 * W + x0, d = (size_t)y1 * W + x1;
                cu[(size_t)y * cw + x] = 0.25f * (U[a] + U[b] + U[c2] + U[d]);
                cv[(size_t)y * cw + x] = 0.25f * (V[a] + V[b] + V[c2] + V[d]);
            }
        Padded p;
        const int cm = S + std::max(1, R - 1);
        padPlanes({cu.data(), cv.data()}, cw, ch, cm, p);
        std::vector<float> o;
        nlm(p, S, std::max(1, R - 1), (float)(sigmaC * 0.6), 0.85f, o);
        Ud.resize(n); Vd.resize(n);
        forRows(H, W, [&](int y) {
            const double fy = (y + 0.5) / 2.0 - 0.5;
            const int y0 = std::max(0, std::min(ch - 1, (int)std::floor(fy))), y1 = std::min(ch - 1, y0 + 1);
            const float ty = (float)std::max(0.0, std::min(1.0, fy - y0));
            for (int x = 0; x < W; ++x) {
                const double fx = (x + 0.5) / 2.0 - 0.5;
                const int x0 = std::max(0, std::min(cw - 1, (int)std::floor(fx))), x1 = std::min(cw - 1, x0 + 1);
                const float tx = (float)std::max(0.0, std::min(1.0, fx - x0));
                auto bil = [&](const float* pl) {
                    return (pl[(size_t)y0 * cw + x0] * (1 - tx) + pl[(size_t)y0 * cw + x1] * tx) * (1 - ty) + (pl[(size_t)y1 * cw + x0] * (1 - tx) + pl[(size_t)y1 * cw + x1] * tx) * ty;
                };
                Ud[(size_t)y * W + x] = bil(o.data());
                Vd[(size_t)y * W + x] = bil(o.data() + (size_t)cw * ch);
            }
        });
    }
    const std::vector<float>& Yo = Yd.empty() ? Y : Yd;
    const std::vector<float>& Uo = Ud.empty() ? U : Ud;
    const std::vector<float>& Vo = Vd.empty() ? V : Vd;
    std::vector<float> Ys;
    if (sharpen > 0) {
        Ys = Yo;
        gaussianBlurPlane(Ys, W, H, std::max(0.5, c.px(1.1)), std::max(0.5, c.px(1.1)));
    }
    forRows(H, W, [&](int y) {
        float* o = c.out.at(0, y);
        for (int x = 0; x < W; ++x, o += 4) {
            const size_t i = (size_t)y * W + x;
            const float a = A[i];
            if (a <= 0.f) { o[0] = o[1] = o[2] = o[3] = 0.f; continue; }
            float yy = Yo[i];
            if (sharpen > 0) yy += (float)(sharpen * 1.5) * (yy - Ys[i]);
            float r, g, b;
            yuvToRgb(yy, Uo[i], Vo[i], r, g, b);
            o[0] = r * a; o[1] = g * a; o[2] = b * a; o[3] = a;
        }
    });
}

// ---- VEGAS Denoise, as MagixCVFx.ofx does it (settled by running the plug-in's own routines: the 8-bit picture is read as bytes b0 b1 b2 a (the plug-in treats the first
// byte as red but is given BGRA, so blue sits where it expects red - kept), turned into float planes Y U V (BT.601, rounded in the order the plug-in's code does it), each plane denoised
// with a non-local-means over a 21 x 21 search window of 7 x 7 patches (L1 patch distance, weight table exp(-max(0, i - 2 h^2) / h^2) for the distance rounded to a whole number,
// cut to 0 where it falls under 0.001), turned back and cut to whole levels; Amount then runs a 3 x 3 sharpening kernel over the bytes. Luminance and Color are the h of the planes
// as they are. The plug-in's picture has its first row at the bottom and its border is mirrored one way at the top and left and the other way at the bottom and right; done the same.
struct VegasNlm {
    static constexpr int T = 7, S = 21, TH = 3, SH = 10, B = TH + SH;
    std::vector<double> tab;
    explicit VegasNlm(double hh)
    {
        tab.assign(65536, 0.0);
        const double twoSigma2 = (hh + hh) * hh;
        const double k = -1.0 / (hh * hh);
        for (int i = 0; i < 65536; ++i) {
            const double v = (double)i - twoSigma2;
            const double m = 0.0 > v ? 0.0 : v;
            const double e = std::exp(m * k);
            if (0.001 > e) break;                                           // from here on the table is 0
            tab[(size_t)i] = e;
        }
    }
    static int mapLow(int i, int n) { if (i < 0) i = -i; return i < 0 ? 0 : (i > n - 1 ? n - 1 : i); }                 // top / left: the edge pixel not repeated
    static int mapHigh(int i, int n) { if (i >= n) i = 2 * n - 1 - i; return i < 0 ? 0 : (i > n - 1 ? n - 1 : i); }     // bottom / right: the edge pixel repeated
    // src, dst: w x h float planes (first row at the bottom of the picture).
    void run(const float* src, float* dst, int w, int h) const
    {
        const int EW = w + 2 * B, EH = h + 2 * B;
        std::vector<float> E((size_t)EW * EH);
        for (int j = 0; j < EH; ++j) {
            int i = j - B;
            const int sy = i < 0 ? mapLow(i, h) : mapHigh(i, h);
            const float* sr = src + (size_t)sy * w;
            float* er = &E[(size_t)j * EW];
            for (int x = 0; x < EW; ++x) {
                const int ix = x - B;
                er[x] = sr[ix < 0 ? mapLow(ix, w) : mapHigh(ix, w)];
            }
        }
        const double invT = 1.0 / (double)(T * T);
        const double* tb = tab.data();
        parallelRows(h, (size_t)w * S * S * 40, [&](int y0, int y1) {
            const int bh = y1 - y0, qn = bh + T - 1;
            std::vector<double> num((size_t)bh * w, 0.0), den((size_t)bh * w, 0.0), d((size_t)qn * (w + T - 1)), hs((size_t)qn * w), sum(w);
            for (int sy = S - 1; sy >= 0; --sy)
                for (int sx = S - 1; sx >= 0; --sx) {
                    for (int q = 0; q < qn; ++q) {
                        const float* a = &E[(size_t)(SH + y0 + q) * EW + SH];
                        const float* b = &E[(size_t)(y0 + q + sy) * EW + sx];
                        double* dr = &d[(size_t)q * (w + T - 1)];
                        for (int x = 0; x < w + T - 1; ++x) {
                            const float df = b[x] - a[x];
                            dr[x] = (double)std::fabs(df);
                        }
                        double* hr = &hs[(size_t)q * w];
                        for (int x = 0; x < w; ++x) {
                            double acc = dr[x];
                            for (int t = 1; t < T; ++t) acc += dr[x + t];
                            hr[x] = acc;
                        }
                    }
                    for (int y = 0; y < bh; ++y) {
                        for (int x = 0; x < w; ++x) sum[(size_t)x] = hs[(size_t)y * w + x];
                        for (int t = 1; t < T; ++t) {
                            const double* hr = &hs[(size_t)(y + t) * w];
                            for (int x = 0; x < w; ++x) sum[(size_t)x] += hr[x];
                        }
                        const float* qrow = &E[(size_t)(y0 + y + sy + TH) * EW + sx + TH];
                        double* nr = &num[(size_t)y * w];
                        double* dn = &den[(size_t)y * w];
                        for (int x = 0; x < w; ++x) {
                            int idx = (int)std::round(sum[(size_t)x] * invT);
                            if (idx < 0) idx = 0;
                            if (idx > 65535) idx = 65535;
                            const float wt = (float)tb[idx];
                            const float prod = wt * qrow[x];
                            dn[x] += (double)wt;
                            nr[x] += (double)prod;
                        }
                    }
                }
            for (int y = 0; y < bh; ++y)
                for (int x = 0; x < w; ++x) dst[(size_t)(y0 + y) * w + x] = (float)(num[(size_t)y * w + x] / den[(size_t)y * w + x]);
        });
    }
};

inline float cutLevel(float v) { if (0.0f > v) return 0.0f; return v < 255.0f ? v : 255.0f; }

void denoisingNLM(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (c.src.px.size() != c.out.px.size() || W < 1 || H < 1) { c.out.px = c.src.px; return; }
    const double lum = clampd(c.d("Luminance"), 0, 100), col = clampd(c.d("Color"), 0, 100), amt = clampd(c.d("Amount"), 0, 1);
    const double eps = 1.1920928955078125e-07;
    const size_t n = (size_t)W * H;
    // the picture as the plug-in is given it: 8-bit premultiplied, first row at the bottom
    std::vector<uint8_t> px(n * 4);
    for (int y = 0; y < H; ++y) {
        const float* s = c.src.at(0, y);
        uint8_t* o = &px[(size_t)(H - 1 - y) * W * 4];
        for (int i = 0; i < W * 4; ++i) {
            const float v = s[(i & ~3) + (i & 3 ? (i & 3) == 2 ? 0 : (i & 3) : 2)] * 255.f + 0.5f;       // bytes in B G R A order
            o[i] = (uint8_t)(v <= 0.f ? 0 : (v >= 255.f ? 255 : (int)v));
        }
    }
    std::vector<uint8_t> res = px;
    if (lum > eps || col > eps) {
        std::vector<float> Y(n), U(n), V(n), Y2, U2, V2;
        for (size_t i = 0; i < n; ++i) {
            const float r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
            float y = r * 0.299f;
            y = y + g * 0.587f;
            y = y + b * 0.114f;
            float t = g * 0.28886f;
            t = t + r * 0.14713f;
            const float u = b * 0.436f - t;
            float v = r * 0.615f;
            v = v - g * 0.51499f;
            v = v - b * 0.10001f;
            Y[i] = y; U[i] = u; V[i] = v;
        }
        Y2.resize(n); U2.resize(n); V2.resize(n);
        if (lum > eps) VegasNlm(lum).run(Y.data(), Y2.data(), W, H); else Y2 = Y;
        if (col > eps) {
            VegasNlm nc(col);
            nc.run(U.data(), U2.data(), W, H);
            nc.run(V.data(), V2.data(), W, H);
        } else { U2 = U; V2 = V; }
        for (size_t i = 0; i < n; ++i) {
            const float y = Y2[i], u = U2[i], v = V2[i];
            float R = u * 0.0f;
            R = R + y;
            R = R + v * 1.13983f;
            float G = y - u * 0.39465f;
            G = G - v * 0.5806f;
            float Bv = u * 2.03211f;
            Bv = Bv + y;
            Bv = Bv + v * 0.0f;
            res[i * 4] = (uint8_t)(int)cutLevel(R);
            res[i * 4 + 1] = (uint8_t)(int)cutLevel(G);
            res[i * 4 + 2] = (uint8_t)(int)cutLevel(Bv);
            res[i * 4 + 3] = px[i * 4 + 3];
        }
    }
    if (amt > eps) {
        int center;
        if (amt >= 0.25) center = 29 - (int)(amt * 16.0);
        else if (amt >= 0.125) center = (int)(63.0 - amt * 152.0);
        else if (amt >= 0.06300000101327896) center = (int)(88.961538 - amt * 269.230769);
        else center = (int)(100.451613 - amt * 451.612903);
        const int div = center - 12;
        const std::vector<uint8_t> in = res;
        for (int y = 0; y < H; ++y) {
            const int ya = y > 0 ? y - 1 : 0, yb = y < H - 1 ? y + 1 : H - 1;
            for (int x = 0; x < W; ++x) {
                const int xa = x > 0 ? x - 1 : 0, xb = x < W - 1 ? x + 1 : W - 1;
                int outv[4];
                for (int k = 0; k < 4; ++k) {
                    auto P = [&](int yy, int xx) { return (int)in[((size_t)yy * W + xx) * 4 + k]; };
                    const int sum = center * P(y, x) - 2 * (P(ya, x) + P(yb, x) + P(y, xa) + P(y, xb)) - (P(ya, xa) + P(ya, xb) + P(yb, xa) + P(yb, xb));
                    int v = sum / div;
                    outv[k] = v < 0 ? 0 : (v > 255 ? 255 : v);
                }
                uint8_t* o = &res[((size_t)y * W + x) * 4];
                for (int k = 0; k < 3; ++k) o[k] = (uint8_t)std::min(outv[k], outv[3]);
                o[3] = (uint8_t)outv[3];
            }
        }
    }
    for (int y = 0; y < H; ++y) {
        const uint8_t* s = &res[(size_t)(H - 1 - y) * W * 4];
        float* o = c.out.at(0, y);
        for (int i = 0; i < W * 4; ++i) o[(i & ~3) + (i & 3 ? (i & 3) == 2 ? 0 : (i & 3) : 2)] = (float)s[i] / 255.f;
    }
}

void smartNoiseReduction(Ctx& c)
{
    if (!c.b("EnableEnhancement")) { c.out.px = c.src.px; return; }
    const double sens = clampd(c.d("Noise"), 0, 20000) / 2000.0;
    const double s = sens * 3.0 * 0.9 / 255.0 * c.scale;
    denoise(c, s, s, 2, 1, 0.0);
}

}  // namespace

void addDenoiseEffects(std::vector<Def>& out)
{
    {
        std::vector<PDef> ps;
        ps.push_back(group("Noise Reduction", "Noise Reduction"));
        ps.push_back(dbl("Luminance", "Luminance", 3.0, 0, 100, "Noise Reduction"));
        ps.back().dmax = 20;
        ps.push_back(dbl("Color", "Chrominance", 3.0, 0, 100, "Noise Reduction"));
        ps.back().dmax = 20;
        ps.push_back(group("Sharpening", "Sharpening"));
        ps.push_back(dbl("Amount", "Amount", 0.0, 0, 1, "Sharpening"));
        out.push_back(make("de.magix:DenoisingNLM", "Denoise",
                           "Reduces noise: every pixel becomes the average of the pixels round it that have the same surroundings (non-local means), brightness and colour separately, with an optional sharpening",
                           ps, denoisingNLM));
    }
    {
        std::vector<PDef> ps;
        ps.push_back(dbl("Noise", "Noise sensitivity", 2000, 0, 20000));
        ps.back().dmax = 5000;
        ps.back().hint = "The noise level of the image.";
        ps.push_back(boolean("EnableEnhancement", "Enable enhancement", true));
        ps.back().hint = "Enable or disable the noise reduction.";
        out.push_back(make("com.sonycreativesoftware:smart_noise_reduction", "Smart Noise Reduction",
                           "Reduces noise while keeping edges (a small non-local-means denoiser with the noise sensitivity of Smart Upscale); the settings are a guess since only the identifier is known", ps, smartNoiseReduction));
    }
}

}  // namespace analysis
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
