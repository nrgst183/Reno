// VEGAS Layer Dimensionality as its OpenCL kernels draw it (Vfx1.ofx, layerDimensionality OpenCL processor; read from the user's own VEGAS install and re-implemented here): the host's
// order of the kernels, the numbers it hands them and the kernels' arithmetic, in floats. Header only (no Reno types) so a test program can check it against the captured pictures.
//
// Pictures are RGBA floats, W x H, rows bottom first (the kernels' y), the frame as the card holds it (premultiplied bytes taken as they are). What the host does, in order:
//   glow     GlowType 1 outer: the frame's alpha blurred (Deriche, below), colour x blurred alpha x (1 - alpha); 2 inner: colour x (1 - blurred alpha) x alpha; 3 edge: the inner one and
//            the outer one both in GlowColor; 4 dual: the inner in GlowColor, the outer in GlowColor2 (the two added, alpha kept to 1). 0 or more than 4: none.
//   emboss   when |Foreground| > 1e-4 and/or |Background x 0.5| > 1e-4: the alpha blurred (size Roundness x 0.2), then the light kernel on it (Sobel of the blurred alpha, a Phong sum), the
//            result weighed by the frame's alpha (foreground) / one minus it (background) and the amount; both added.
//   overlay  glow over emboss (straight "over", see composite()); none: the frame itself.
//   shadow   ShadowType 1 drop: black alpha x Amount, moved along the light by (W x k nx, H x k ny) (k = Height x 0.1 / nz; nearest read, zero outside), blurred (Blur x 0.6);
//            2 inner: black one minus alpha, moved the same way, blurred (Roundness x 0.2, as the host code has it), x alpha x Amount. An opaque frame has no inner shadow.
//   final    the overlay over the frame x Opacity; then the drop shadow under it / the inner shadow over it; or the overlay x Opacity when no shadow.
// The blur is the recursive Gaussian of vfxgauss.h on the alpha plane, in global float buffers with every multiply-add fused (vfxcookie.h's feather()).
#pragma once
#include "vfxcookie.h"
#include "vfxgauss.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace reno {
namespace ofx {
namespace vfxld {

// The settings as the host cuts them to floats and scales them (the processor's own numbers). All of them are the effect's settings as they are; nothing is clamped here.
struct Settings {
    int shadowType = 0, glowType = 0;
    float height = 0.f, blur = 0.f /* x 0.6 */, amount = 0.f;
    float col1[3] = {0, 0, 0}, col2[3] = {0, 0, 0};
    float glowAmount = 0.f, fg = 0.f, bg = 0.f /* x 0.5 */, roundness = 0.f /* x 0.2 */;
    float light[3] = {0, 0, 1};
    float intensity = 0.f, ambience = 0.f, shininess = 0.f, opacity = 1.f;
    float pixelAspect = 1.f;        // the output clip's pixel aspect ratio (1)
};

inline Settings makeSettings(int shadowType, int glowType, double height, double blur, double amount, const double* glowColor, const double* glowColor2, double glowAmount, double fg, double bg,
                             double roundness, const double* light, double intensity, double ambience, double shininess, double opacity, double pixelAspect)
{
    Settings s;
    s.shadowType = shadowType;
    s.glowType = glowType;
    s.height = (float)height;
    s.blur = (float)blur * 0.6f;
    s.amount = (float)amount;
    for (int k = 0; k < 3; ++k) { s.col1[k] = (float)glowColor[k]; s.col2[k] = (float)glowColor2[k]; s.light[k] = (float)light[k]; }
    s.glowAmount = (float)glowAmount;
    s.fg = (float)fg;
    s.bg = (float)bg * 0.5f;
    s.roundness = (float)roundness * 0.2f;
    s.intensity = (float)intensity;
    s.ambience = (float)ambience;
    s.shininess = (float)shininess;
    s.opacity = (float)opacity;
    s.pixelAspect = (float)pixelAspect;
    return s;
}

typedef std::vector<float> Img;     // 4 floats a pixel

// ---- the kernels
// kc4145ad214: the alpha in all four lanes.
inline void kAlpha(const Img& in, Img& out, int n)
{
    out.resize((size_t)n * 4);
    for (int i = 0; i < n; ++i) { const float a = in[(size_t)i * 4 + 3]; out[(size_t)i * 4] = out[(size_t)i * 4 + 1] = out[(size_t)i * 4 + 2] = out[(size_t)i * 4 + 3] = a; }
}
// kc4145ad28: w = (invert ? 1 - alpha : alpha) x amount; colour x w, w.
inline void kFill(const Img& in, Img& out, int n, bool invert, const float* col, float amount)
{
    out.resize((size_t)n * 4);
    for (int i = 0; i < n; ++i) {
        float w = invert ? 1.0f - in[(size_t)i * 4 + 3] : in[(size_t)i * 4 + 3];
        w *= amount;
        out[(size_t)i * 4] = col[0] * w; out[(size_t)i * 4 + 1] = col[1] * w; out[(size_t)i * 4 + 2] = col[2] * w; out[(size_t)i * 4 + 3] = w;
    }
}
// kc4145ad29: the second picture x ((invert ? 1 - a : a) of the first picture's alpha x amount).
inline void kMask(const Img& alphaOf, const Img& colourOf, Img& out, int n, bool invert, float amount)
{
    out.resize((size_t)n * 4);
    for (int i = 0; i < n; ++i) {
        const float a = alphaOf[(size_t)i * 4 + 3];
        const float v49 = invert ? 1.0f - a : a;
        const float f = v49 * amount;
        for (int k = 0; k < 4; ++k) out[(size_t)i * 4 + k] = colourOf[(size_t)i * 4 + k] * f;
    }
}
// kc4145ad211: out(x, y) = in(floor(x + sx), floor(y + sy)), zero where that lies outside.
inline void kShift(const Img& in, Img& out, int W, int H, float sx, float sy)
{
    out.assign((size_t)W * H * 4, 0.f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float px = (float)x + sx, py = (float)y + sy;
            if (0.0f <= px && px < (float)W && 0.0f <= py && py < (float)H) {
                const int ix = (int)std::floor(px), iy = (int)std::floor(py);
                std::memcpy(&out[((size_t)y * W + x) * 4], &in[((size_t)iy * W + ix) * 4], 4 * sizeof(float));
            }
        }
}
// kc4145ad213: x scale.
inline void kScale(const Img& in, Img& out, int n, float s)
{
    out.resize((size_t)n * 4);
    for (size_t i = 0; i < (size_t)n * 4; ++i) out[i] = in[i] * s;
}
// kc4145ad215 (inner): colour x ((1 - a.w) x b.w);  kc4145ad216 (outer): colour x (a.w x (1 - b.w)). colour has alpha 1.
inline void kGlowInner(const Img& blurred, const Img& frame, Img& out, int n, const float* col)
{
    out.resize((size_t)n * 4);
    for (int i = 0; i < n; ++i) {
        const float v49 = (1.0f - blurred[(size_t)i * 4 + 3]) * frame[(size_t)i * 4 + 3];
        out[(size_t)i * 4] = col[0] * v49; out[(size_t)i * 4 + 1] = col[1] * v49; out[(size_t)i * 4 + 2] = col[2] * v49; out[(size_t)i * 4 + 3] = 1.0f * v49;
    }
}
inline void kGlowOuter(const Img& blurred, const Img& frame, Img& out, int n, const float* col)
{
    out.resize((size_t)n * 4);
    for (int i = 0; i < n; ++i) {
        const float v49 = blurred[(size_t)i * 4 + 3] * (1.0f - frame[(size_t)i * 4 + 3]);
        out[(size_t)i * 4] = col[0] * v49; out[(size_t)i * 4 + 1] = col[1] * v49; out[(size_t)i * 4 + 2] = col[2] * v49; out[(size_t)i * 4 + 3] = 1.0f * v49;
    }
}
// kc4145ad217: a + b, alpha cut to 0 .. 1.
inline void kAdd(const Img& a, const Img& b, Img& out, int n)
{
    out.resize((size_t)n * 4);
    for (size_t i = 0; i < (size_t)n * 4; ++i) out[i] = a[i] + b[i];
    for (int i = 0; i < n; ++i) out[(size_t)i * 4 + 3] = std::min(std::max(out[(size_t)i * 4 + 3], 0.0f), 1.0f);
}
// kc4145ad210: the top picture x (mode > 0 ? scale : 1) over the bottom picture x (mode > 0 ? 1 : scale), the colours taken as straight and weighed by alpha, then divided by the alpha again.
inline void kOver(const Img& top, const Img& bottom, Img& out, int n, float scale, int mode)
{
    out.resize((size_t)n * 4);
    const float s54 = mode > 0 ? scale : 1.0f, s55 = mode > 0 ? 1.0f : scale;
    for (int i = 0; i < n; ++i) {
        float t[4], b[4], o[4];
        for (int k = 0; k < 4; ++k) { t[k] = top[(size_t)i * 4 + k] * s54; b[k] = bottom[(size_t)i * 4 + k] * s55; }
        for (int k = 0; k < 3; ++k) o[k] = t[k] * t[3] + b[k] * b[3] * (1.0f - t[3]);
        o[3] = t[3] + b[3] * (1.0f - t[3]);
        if (0.00001f < o[3]) for (int k = 0; k < 3; ++k) o[k] /= o[3];
        std::memcpy(&out[(size_t)i * 4], o, sizeof o);
    }
}

// The HSL helpers of the light kernel (f0 and f1 of the program, with their nudges of one part in a million at pure black and white). v = (h, s, l, w) on the way.
inline void toHsl(const float* in, float* r)
{
    float v60[4] = {in[0], in[1], in[2], in[3]};
    r[0] = in[0]; r[1] = in[1]; r[2] = in[2]; r[3] = in[3];
    float v61 = std::max(std::max(v60[0], v60[1]), v60[2]);
    float v62 = std::min(std::min(v60[0], v60[1]), v60[2]);
    if (v62 + v61 > 2.0f - 0.000001f && v62 + v61 < 2.0f) {
        v61 -= 0.000001f;
        if (v60[0] > v60[1] && v60[0] > v60[2]) v60[0] = v61;
        else if (v60[1] > v60[2]) v60[1] = v61;
        else v60[2] = v61;
    } else if (v62 + v61 >= 2.0f && v62 + v61 < 2.0f + 0.000001f) {
        v61 += 0.000001f;
        if (v60[0] > v60[1] && v60[0] > v60[2]) v60[0] = v61;
        else if (v60[1] > v60[2]) v60[1] = v61;
        else v60[2] = v61;
    }
    if (v62 + v61 > 0.0f - 0.000001f && v62 + v61 < 0.0f) {
        v62 -= 0.000001f;
        if (v60[0] < v60[1] && v60[0] < v60[2]) v60[0] = v62;
        else if (v60[1] < v60[2]) v60[1] = v62;
        else v60[2] = v62;
    } else if (v62 + v61 >= 0.0f && v62 + v61 < 0.0f + 0.000001f) {
        v62 += 0.000001f;
        if (v60[0] < v60[1] && v60[0] < v60[2]) v60[0] = v62;
        else if (v60[1] < v60[2]) v60[1] = v62;
        else v60[2] = v62;
    }
    r[2] = (v62 + v61) * 0.5f;
    r[1] = v61 - v62;
    if (r[1] < -0.000001f || r[1] >= 0.000001f) {
        if (r[2] <= 0.5f) r[1] /= v62 + v61;
        else r[1] /= 2.0f - v61 - v62;
        if (r[2] < 0.0f || r[2] > 1.0f) r[1] *= -1.0f;
    } else {
        r[0] = 0.0f;
        return;
    }
    const float v64 = 1.0f / (v61 - v62);
    const float v65x = (v61 - v60[0]) * v64, v65y = (v61 - v60[1]) * v64, v65z = (v61 - v60[2]) * v64;
    if (v60[0] == v61) r[0] = ((v60[1] == v62) ? (5.0f + v65z) : (1.0f - v65y));
    else if (v60[1] == v61) r[0] = ((v60[2] == v62) ? (1.0f + v65x) : (3.0f - v65z));
    else r[0] = ((v60[0] == v62) ? (3.0f + v65y) : (5.0f - v65x));
    r[0] *= (1.0f / 6.0f);
}
inline void fromHsl(const float* in, float* r)
{
    float v60[4] = {in[0], in[1], in[2], in[3]};
    float v66;
    if (in[2] <= 0.5f) v66 = in[2] * (1.0f + in[1]);
    else v66 = in[2] + in[1] - in[2] * in[1];
    if (v66 <= 0.0f) {
        v60[0] = 0.0f; v60[1] = 0.0f; v60[2] = 0.0f;
    } else {
        const float v73 = v60[0] * 6.0f;
        const float v67 = in[2] + in[2] - v66;
        long long v68 = (long long)v73;
        const float v69 = v73 - (float)v68;
        const float v70 = (v66 - v67) * v69;
        const float v71 = v67 + v70;
        const float v72 = v66 - v70;
        switch (v68) {
        case 0: case 6: v60[0] = v66; v60[1] = v71; v60[2] = v67; break;
        case 1: v60[0] = v72; v60[1] = v66; v60[2] = v67; break;
        case 2: v60[0] = v67; v60[1] = v66; v60[2] = v71; break;
        case 3: v60[0] = v67; v60[1] = v72; v60[2] = v66; break;
        case 4: v60[0] = v71; v60[1] = v67; v60[2] = v66; break;
        case 5: v60[0] = v66; v60[1] = v67; v60[2] = v72; break;
        default: break;
        }
    }
    r[0] = v60[0]; r[1] = v60[1]; r[2] = v60[2]; r[3] = v60[3];
}

// The numbers 18062b730 hands the light kernel (kc4145ad212): amt is the foreground (signed) or the background (x 0.5) amount.
struct LightArgs { float v85, v86, v87, v88, v89, v90; float L[4]; };
inline LightArgs lightArgs(const Settings& s, int W, int H, float amt)
{
    LightArgs a;
    float x = s.light[0], y = -s.light[1], z = s.light[2];
    x -= 0.0f; y -= 0.0f; z -= 0.0f;
    const float len = std::sqrt(y * y + x * x + z * z);
    if (len > 1.0000000116860974e-07f) {
        const float inv = 1.0f / len;
        a.L[0] = x * inv; a.L[1] = y * inv; a.L[2] = z * inv; a.L[3] = 0.f;
    } else { a.L[0] = 0.f; a.L[1] = 0.f; a.L[2] = 1.f; a.L[3] = 1.f; }
    float p0 = 1.0f, p1 = 1.0f;
    const float sc = s.pixelAspect;
    if (1.0f > sc) p0 = sc; else p1 = 1.0f / sc;
    const float fw = (float)W, fh = (float)H;
    const float m4 = std::max(fw * p0, fh * p1);
    float t = amt * amt;
    t *= 0.0625f;
    const double prod = (double)t * ((double)m4 * 0.0015625);
    float f = (float)prod;
    float sign = amt > 0.0f ? 1.0f : -1.0f;
    sign *= f;
    sign *= 255.0f;
    a.v85 = sign;
    a.v86 = s.ambience;
    a.v87 = s.intensity * 4.0f;
    a.v88 = 0.6f - s.shininess * 0.2f;
    a.v89 = s.shininess * 0.5f;
    a.v90 = s.shininess * 8.0f + 4.0f;
    return a;
}
// kc4145ad212: Sobel of the alpha of 'height', Phong lighting, the colour of 'col' in HSL with its lightness x the light, alpha = |light - flat light|.
inline void kLight(const Img& height, const Img& col, Img& out, int W, int H, const LightArgs& a)
{
    out.assign((size_t)W * H * 4, 0.f);
    auto rd = [&](int x, int y) { return &height[((size_t)y * W + x) * 4]; };
    auto phong = [&](const float* n /*normalised*/, float& v) {
        float dot = n[0] * a.L[0] + n[1] * a.L[1] + n[2] * a.L[2] + n[3] * a.L[3];
        const float v101 = dot;
        float r[3];
        const float two = 2.0f * v101;
        r[0] = n[0] * two - a.L[0]; r[1] = n[1] * two - a.L[1]; r[2] = n[2] * two - a.L[2];
        float v103 = r[2];
        v103 = std::fmax(v103, 0.01f);
        v103 = std::pow(v103, a.v90);
        const float x = a.v88 * v101, y = a.v89 * v103;
        v = a.v87 * (x + y);
        v += a.v86;
    };
    float flat;
    {
        const float n[4] = {0.f, 0.f, 1.f, 0.f};
        phong(n, flat);
    }
    for (int y = 0; y < H; ++y) {
        const int y0 = std::max(y - 1, 0), y2 = std::min(y + 1, H - 1);
        for (int x = 0; x < W; ++x) {
            float gx, gy;
            if (x == 0 || x == W - 1) {
                const int xa = x == 0 ? x : x - 1, xb = x == 0 ? x + 1 : x;
                const float p0 = rd(xa, y0)[3], p1 = rd(xa, y)[3], p2 = rd(xa, y2)[3], p3 = rd(xb, y0)[3], p4 = rd(xb, y)[3], p5 = rd(xb, y2)[3];
                gx = p0 - p3 + p1 - p4 + p2 - p5;
                gy = p2 - p0 + p5 - p3 + p5 - p3;
            } else {
                const float p0 = rd(x - 1, y0)[3], p1 = rd(x - 1, y)[3], p2 = rd(x - 1, y2)[3], p3 = rd(x + 1, y0)[3], p4 = rd(x + 1, y)[3], p5 = rd(x + 1, y2)[3];
                const float p6 = rd(x, y0)[3], p7 = rd(x, y2)[3];
                gx = p0 - p3 + p1 - p4 + p2 - p5;
                gy = p2 - p0 + p5 - p3 + p7 - p6;
            }
            gx *= a.v85;
            gy *= a.v85;
            float n[4] = {gx, gy, 1.0f, 0.0f};
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] + n[3] * n[3]);
            const float inv = 1.0f / len;
            n[0] *= inv; n[1] *= inv; n[2] *= inv; n[3] *= inv;
            float v91;
            phong(n, v91);
            const float* c = &col[((size_t)y * W + x) * 4];
            float hsl[4];
            toHsl(c, hsl);
            hsl[2] *= v91;
            if (hsl[2] > 0.99999f) { hsl[0] = 0.0f; hsl[1] = 0.0f; }
            hsl[3] = std::min(std::max(std::fabs(v91 - flat), 0.0f), 1.0f);
            fromHsl(hsl, &out[((size_t)y * W + x) * 4]);
        }
    }
}

// The alpha plane blurred in place and put back as four lanes (kc4145ad25, ad26, ad24, ad218): colour = null: the blurred alpha in all lanes; else (colour, blurred alpha).
// size: the host's amount; the sigma of an axis = sqrt(((pair x amount x larger side / 8) + 1.5)^2 / (2 ln 255)), the first pass down the columns with the sigma of y.
inline void blurAlpha(Img& img, int W, int H, float amount, float pairX, float pairY, const float* colour)
{
    const float maxdim = (float)std::max(W, H);
    float s = maxdim * amount;
    s *= 0.125f;
    const float fx = pairX * s + 1.5f, fy = pairY * s + 1.5f;
    const float l = std::log(0.003921568859368563f);
    const float l2 = l + l;
    const float sigx = std::sqrt(-((fx * fx) / l2)), sigy = std::sqrt(-((fy * fy) / l2));
    std::vector<float> plane((size_t)W * H);
    for (size_t i = 0; i < plane.size(); ++i) plane[i] = img[i * 4 + 3];
    vfxcookie::feather(plane, W, H, sigx, sigy);
    for (size_t i = 0; i < plane.size(); ++i) {
        const float v = plane[i];
        if (colour) { img[i * 4] = colour[0]; img[i * 4 + 1] = colour[1]; img[i * 4 + 2] = colour[2]; }
        else img[i * 4] = img[i * 4 + 1] = img[i * 4 + 2] = v;
        img[i * 4 + 3] = v;
    }
}

// ---- the host: src (the frame, rows bottom first) to out (the card's result, floats; the 8-bit store is the caller's).
inline void render(const float* src, int W, int H, const Settings& s, float* out)
{
    const int n = W * H;
    const size_t total = (size_t)n * 4;
    const Img I(src, src + total);
    float pairX = 1.0f, pairY = 1.0f;
    if (1.0f > s.pixelAspect) pairX = s.pixelAspect; else pairY = 1.0f / s.pixelAspect;
    static const float kZero[4] = {0.f, 0.f, 0.f, 0.f};
    const float one4[4] = {1.f, 1.f, 1.f, 1.f};
    (void)one4;

    // ---- glow
    Img G;                                  // empty: none
    if (s.glowType >= 1 && s.glowType <= 4) {
        Img A, B, C, D;
        const float c1[4] = {s.col1[0], s.col1[1], s.col1[2], 1.0f};
        float c2[4] = {s.col2[0], s.col2[1], s.col2[2], 1.0f};
        if (s.glowType == 3) std::memcpy(c2, c1, sizeof c2);
        if (s.glowType == 1) {
            kAlpha(I, A, n);
            blurAlpha(A, W, H, s.glowAmount, pairX, pairY, nullptr);
            kGlowOuter(A, I, C, n, c1);
        } else if (s.glowType == 2) {
            kAlpha(I, A, n);
            blurAlpha(A, W, H, s.glowAmount, pairX, pairY, nullptr);
            kGlowInner(A, I, C, n, c1);
        } else {
            kAlpha(I, D, n);
            blurAlpha(D, W, H, s.glowAmount, pairX, pairY, nullptr);
            kGlowInner(D, I, A, n, c1);
            kGlowOuter(D, I, B, n, c2);
            kAdd(A, B, C, n);
        }
        G.swap(C);
    }

    // ---- emboss
    Img E;                                  // empty: none
    const bool fgOn = std::fabs(s.fg) > 9.999999747378752e-05f, bgOn = std::fabs(s.bg) > 9.999999747378752e-05f;
    if (fgOn || bgOn) {
        Img D, A, Efg, Ebg;
        kAlpha(I, D, n);
        blurAlpha(D, W, H, s.roundness, pairX, pairY, nullptr);
        if (fgOn) {
            kLight(D, D, A, W, H, lightArgs(s, W, H, s.fg));
            kMask(I, A, Efg, n, false, std::fabs(s.fg));
        }
        if (bgOn) {
            kLight(D, D, A, W, H, lightArgs(s, W, H, s.bg));
            kMask(I, A, Ebg, n, true, std::fabs(s.bg));
        }
        if (fgOn && bgOn) kAdd(Efg, Ebg, E, n);
        else if (fgOn) E.swap(Efg);
        else E.swap(Ebg);
    }

    // ---- glow over emboss
    Img X;                                  // the overlay; empty: the frame itself
    if (!G.empty() && !E.empty()) kOver(G, E, X, n, 1.0f, 0);
    else if (!G.empty()) X.swap(G);
    else if (!E.empty()) X.swap(E);

    // ---- shadow
    Img S;                                  // empty: none
    if (s.shadowType == 1 || s.shadowType == 2) {
        float x = s.light[0], y = s.light[1], z = s.light[2];
        x -= 0.0f; y -= 0.0f; z -= 0.0f;
        const float len = std::sqrt(x * x + y * y + z * z);
        float nx, ny, nz;
        if (len > 1.0000000116860974e-07f) { const float inv = 1.0f / len; nx = x * inv; ny = y * inv; nz = z * inv; }
        else { nx = 0.f; ny = 0.f; nz = 1.0f; }
        float k8 = 0.f, k7 = 0.f;
        if ((float)std::fabs((double)nz) > 9.999999747378752e-05f) {
            k7 = s.height * 0.1f;
            k7 /= nz;
            k8 = k7 * nx;
            k7 = k7 * ny;
        }
        const float shx = (float)W * k8, shy = (float)H * k7;
        Img D, A;
        if (s.shadowType == 1) {
            kFill(I, D, n, false, kZero, s.amount);
            kShift(D, A, W, H, shx, shy);
            blurAlpha(A, W, H, s.blur, pairX, pairY, kZero);
            S.swap(A);
        } else {
            kFill(I, D, n, true, kZero, 1.0f);
            kShift(D, A, W, H, shx, shy);
            blurAlpha(A, W, H, s.roundness, pairX, pairY, kZero);
            kMask(I, A, D, n, false, std::fabs(s.amount));
            S.swap(D);
        }
    }

    // ---- the final composite
    Img R;
    const bool overlay = !X.empty();
    Img Xf;
    if (overlay) {
        // the frame x opacity under the overlay
        kOver(X, I, Xf, n, s.opacity, 0);
    }
    const Img& top = overlay ? Xf : I;
    const float fscale = overlay ? 1.0f : s.opacity;
    if (!S.empty()) {
        if (s.shadowType == 1) kOver(top, S, R, n, fscale, 1);
        else kOver(S, top, R, n, fscale, 0);
    } else {
        kScale(top, R, n, fscale);
    }
    std::memcpy(out, R.data(), total * sizeof(float));
}

}  // namespace vfxld
}  // namespace ofx
}  // namespace reno
