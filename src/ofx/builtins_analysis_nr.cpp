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

void denoisingNLM(Ctx& c)
{
    const double ly = clampd(c.d("Luminance"), 0, 100), co = clampd(c.d("Color"), 0, 100);
    const double k = 0.9 / 255.0 * c.scale;
    denoise(c, ly * k, co * k, 3, 2, clampd(c.d("Amount"), 0, 1));
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
