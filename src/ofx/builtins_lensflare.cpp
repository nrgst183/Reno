// VEGAS Lens Flare as VEGAS's own draws it (read off its Vfx1 plug-in; 8-bit, one flare after another onto the frame):
//   - every lens type is a list of flares (the 9 / 23 / 16 / 14 / 8 elements of the names): shape, strength, size, a place C along the line from the light through the middle of the
//     frame (1 - C of the way from the middle to the light: 0 at the light, 1 in the middle, past 1 on the far side) and a colour, kept in the table below;
//   - Light position runs -1 to 1 over the frame from its middle (up is positive); Perspective enlarges flares further along the line (2 ^ ((C - 1) sin(Perspective pi / 2)));
//     size is Size * the flare's size * the shorter side of the frame;
//   - the flare's box is cut from the frame and each pixel gets u, v (-1 to 1 over the box) and its distance from the middle; the shape turns those into a strength (disc cone,
//     ring, soft disc, hexagon, rays for the sunburst) which times the colour (the flare's own, Tint of the way to the light's) and the Intensity is added to the pixel, at most up to
//     its alpha; Blend is how much of that is taken;
//   - the rays' lengths come from the C runtime's rand() as the plug-in finds it after its first 64 calls.
// The frame is taken as 8 bits and the sums done in single precision in VEGAS's order, so the result is bit-exact.
#include "builtins_more_internal.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {
namespace more {
namespace {

struct Flare { int type; float strength, size, place, c0, c1, c2; };

static const Flare kLens0[] = {
    {4, 2.f, 0.2f, 0.f, 1.f, 1.f, 1.f},
    {5, 2.f, 0.3f, 0.f, 0.67f, 0.69f, 0.93f},
    {2, 1.2f, 0.14f, 0.f, 0.16f, 0.18f, 0.55f},
    {0, 1.f, 0.05f, 0.66f, 0.48f, 0.2f, 0.24f},
    {1, 1.f, 0.075f, 0.72f, 0.48f, 0.2f, 0.24f},
    {0, 1.2f, 0.02f, 1.53f, 0.44f, 0.6f, 0.6f},
    {4, 1.f, 0.06f, 1.7f, 0.42f, 0.18f, 0.5f},
    {1, 1.f, 0.3f, 1.9f, 0.28f, 0.36f, 0.12f},
    {2, 0.6f, 0.22f, 1.91f, 0.11f, 0.05f, 0.13f},
};
static const Flare kLens1[] = {
    {1, 1.f, 0.09f, -0.35f, 0.46f, 0.52f, 0.5f},
    {1, 1.f, 0.022f, -0.95f, 0.2f, 0.52f, 0.34f},
    {1, 1.f, 0.012f, -0.97f, 0.2f, 0.52f, 0.34f},
    {4, 0.8f, 0.01f, -1.01f, 0.86f, 0.92f, 0.44f},
    {4, 1.6f, 0.24f, 0.f, 1.f, 1.f, 1.f},
    {5, 2.f, 0.36f, 0.f, 0.87f, 0.74f, 0.7f},
    {2, 1.2f, 0.12f, 0.f, 0.32f, 0.27f, 0.27f},
    {1, 0.6f, 0.07f, 0.29f, 0.43f, 0.48f, 0.54f},
    {4, 1.6f, 0.03f, 0.66f, 0.74f, 0.69f, 0.47f},
    {0, 1.4f, 0.01f, 0.92f, 0.4f, 0.54f, 0.56f},
    {1, 0.8f, 0.06f, 1.46f, 0.12f, 0.16f, 0.2f},
    {0, 1.2f, 0.21f, 1.46f, 0.12f, 0.16f, 0.2f},
    {1, 1.2f, 0.3f, 1.46f, 0.09f, 0.12f, 0.15f},
    {0, 1.4f, 0.08f, 1.59f, 0.18f, 0.24f, 0.3f},
    {1, 1.f, 0.04f, 2.13f, 0.22f, 0.43f, 0.51f},
    {0, 1.f, 0.03f, 2.17f, 0.7f, 0.78f, 0.82f},
    {1, 1.f, 0.09f, 2.19f, 0.06f, 0.1f, 0.12f},
    {1, 1.f, 0.02f, 2.33f, 0.66f, 0.34f, 0.16f},
    {0, 1.f, 0.02f, 2.36f, 0.66f, 0.34f, 0.16f},
    {1, 1.f, 0.02f, 2.4f, 0.66f, 0.34f, 0.16f},
    {1, 1.2f, 0.15f, 2.84f, 0.04f, 0.1f, 0.06f},
    {1, 1.2f, 0.12f, 2.86f, 0.04f, 0.1f, 0.06f},
    {0, 1.2f, 0.1f, 2.99f, 0.04f, 0.1f, 0.06f},
};
static const Flare kLens2[] = {
    {1, 1.f, 0.1f, -0.29f, 0.19f, 0.24f, 0.34f},
    {4, 1.6f, 0.16f, 0.f, 1.f, 1.f, 1.f},
    {5, 2.f, 0.3f, 0.f, 0.67f, 0.69f, 0.93f},
    {2, 1.2f, 0.14f, 0.f, 0.16f, 0.18f, 0.55f},
    {0, 1.2f, 0.06f, 0.48f, 0.39f, 0.3f, 0.3f},
    {1, 1.f, 0.14f, 0.56f, 0.26f, 0.2f, 0.2f},
    {1, 1.f, 0.07f, 0.59f, 0.26f, 0.2f, 0.2f},
    {0, 1.2f, 0.05f, 0.8f, 0.14f, 0.26f, 0.48f},
    {4, 1.2f, 0.03f, 1.26f, 0.46f, 0.6f, 0.37f},
    {1, 1.f, 0.08f, 1.41f, 0.01f, 0.14f, 0.26f},
    {0, 1.2f, 0.04f, 1.48f, 0.01f, 0.14f, 0.26f},
    {1, 1.f, 0.14f, 1.48f, 0.01f, 0.14f, 0.26f},
    {0, 1.f, 0.07f, 1.65f, 0.18f, 0.24f, 0.08f},
    {4, 1.2f, 0.04f, 1.67f, 0.41f, 0.16f, 0.05f},
    {1, 1.f, 0.2f, 2.f, 0.01f, 0.2f, 0.24f},
    {1, 1.2f, 0.39f, 2.34f, 0.08f, 0.12f, 0.1f},
};
static const Flare kLens3[] = {
    {1, 1.f, 0.08f, -0.4f, 0.f, 0.15f, 0.35f},
    {1, 1.f, 0.1f, -0.2f, 0.f, 0.15f, 0.35f},
    {0, 1.f, 0.1f, 0.f, 0.5f, 0.5f, 0.5f},
    {4, 1.f, 0.3f, 0.f, 1.f, 1.f, 1.f},
    {5, 1.f, 0.4f, 0.2f, 1.f, 1.f, 1.f},
    {1, 1.f, 0.2f, 0.3f, 0.f, 0.15f, 0.35f},
    {1, 1.f, 0.09f, 0.7f, 0.f, 0.35f, 0.15f},
    {1, 1.f, 0.05f, 1.05f, 0.f, 0.15f, 0.35f},
    {1, 1.f, 0.07f, 1.24f, 0.f, 0.15f, 0.35f},
    {2, 1.f, 0.13f, 1.26f, 0.f, 0.15f, 0.35f},
    {1, 1.f, 0.07f, 1.34f, 0.29f, 0.21666665f, 0.10666666f},
    {1, 1.f, 0.18f, 1.55f, 0.29f, 0.21666665f, 0.10666666f},
    {3, 1.f, 0.14f, 1.9f, 0.f, 0.17f, 0.07f},
    {1, 1.f, 0.07f, 2.25f, 0.29f, 0.21666665f, 0.10666666f},
};
static const Flare kLens4[] = {
    {0, 1.f, 0.1f, 0.f, 0.5f, 0.5f, 0.5f},
    {4, 1.f, 0.3f, 0.f, 1.f, 1.f, 1.f},
    {5, 1.f, 0.8f, 0.2f, 1.f, 1.f, 1.f},
    {1, 1.f, 0.08f, 0.38f, 0.4f, 0.45f, 0.45f},
    {1, 1.f, 0.02f, 0.8f, 0.4f, 0.45f, 0.45f},
    {1, 1.f, 0.16f, 1.2f, 0.4f, 0.45f, 0.45f},
    {1, 0.8f, 0.1f, 1.6f, 0.4f, 0.45f, 0.45f},
    {1, 1.f, 0.24f, 2.f, 0.4f, 0.45f, 0.45f},
};

// The six sides of the hexagon of the polygon shapes, and the unit normals of its edges.
struct Hexagon {
    float x[6], y[6], nx[6], ny[6];
    Hexagon()
    {
        const float vx[6] = {0.6f, -0.6f, -1.f, -0.6f, 0.6f, 1.f}, vy[6] = {-1.f, -1.f, 0.f, 1.f, 1.f, 0.f};
        for (int i = 0; i < 6; ++i) {
            x[i] = vx[i];
            y[i] = vy[i];
        }
        for (int i = 0; i < 6; ++i) {
            const int j = (i + 1) % 6;
            const float a = x[i] - x[j], b = y[j] - y[i];
            const float len = std::sqrt(b * b + a * a);
            ny[i] = b / len;
            nx[i] = a / len;
        }
    }
    float inside(float u, float v) const
    {
        float m = 0.f;
        for (int i = 0; i < 6; ++i) {
            const float t0 = (v - y[i]) * nx[i], t1 = (u - x[i]) * ny[i];
            const float s = t0 + t1;
            if (i == 0 || s < m) m = s;
        }
        return m;
    }
};

// The rays' lengths round the circle: 64 values (a tenth of the range of rand() each, squared) of the C runtime's rand() from its first start, the first 64 spent by the plug-in on a table it never uses.
struct RayTable {
    double t[64];
    RayTable()
    {
        uint32_t seed = 1;
        for (int i = 0; i < 128; ++i) {
            seed = seed * 214013u + 2531011u;
            if (i < 64) continue;
            const int r = (int)((seed >> 16) & 0x7fff);
            const double q = (double)(r & 0x3ff) * 0.0009765625;
            t[i - 64] = q * q;
        }
    }
};

// The strength of a flare's shape at (u, v), d from its middle; false when it adds nothing. out gets the three colour parts.
bool shape(int type, float u, float v, float d, float I, const float* col, const Hexagon& hex, const RayTable& rays, float* out)
{
    float s;
    switch (type) {
    case 0:
        if (!(d < 1.f)) return false;
        s = 1.f - d;
        break;
    case 1:
        if (!(d < 1.f)) return false;
        if (d > 0.8f) s = (1.f - d) * 5.f;
        else if (d > 0.75f) s = (d - 0.75f) * 5.f + 0.75f;
        else s = d;
        break;
    case 2: {
        if (!(d < 1.f) || !((double)d > 0.85)) return false;
        const float t = 1.f - d;
        s = 0.1f > t ? t * 10.f : (0.15f - t) * 20.f;
        break;
    }
    case 3: {
        const float m = hex.inside(u, v);
        if (m < 0.f) return false;
        if (0.2f > m) {
            s = m / 0.2f;
            s = s < 0.f ? 0.f : (s < 1.f ? s : 1.f);
        } else {
            const float t = m - 0.2f;
            s = (0.8f - t) / 0.8f;
            s = s < 0.f ? 0.f : (s < 1.f ? s : 1.f);
        }
        break;
    }
    case 4: {
        const float m = hex.inside(u, v);
        if (m < 0.f) return false;
        s = m * m;
        break;
    }
    default: {
        if (!(d < 1.f)) return false;
        const float ang = std::atan2(v, u);
        double a = (double)(float)((double)ang + 3.141592653589793);
        a = a / 6.283185307179586 * 3.0;
        a = a * 60.0;
        const double fr = std::fmod(a, 1.0);
        const int i = (int)a;
        const float w = (float)((1.0 - fr) * rays.t[i & 63] + fr * rays.t[(i + 1) & 63]);
        float q;
        if (0.f >= d) q = 0.f;
        else if (d < w) q = d / w;
        else q = 1.f;
        s = (float)std::pow(1.0 - (double)q, 3.0);
        if ((double)d < 0.3) s = (d / 0.3f) * (s - 1.f) + 1.f;
        break;
    }
    }
    out[0] = s * col[0] * I;
    out[1] = s * col[1] * I;
    out[2] = s * col[2] * I;
    return true;
}

}  // namespace

void lensFlare(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    copyThrough(c);
    if (W < 1 || H < 1) return;
    const Color lc = c.color("LightColor");
    double lpx, lpy;
    c.xy("LightPosition", lpx, lpy);
    const int lens = std::min(4, std::max(0, c.i("LensType")));
    const float tint = (float)c.d("Tint"), inten = (float)c.d("Intensity"), blend = (float)c.d("Blend"), size = (float)c.d("Size"), persp = (float)c.d("Perspective");
    const float lx = (float)lpx, ly = (float)-lpy;
    if (!(inten != 0.f) || !std::isfinite(lx) || !std::isfinite(ly)) return;
    static const Hexagon hex;
    static const RayTable rays;
    static const Flare* const lists[5] = {kLens0, kLens1, kLens2, kLens3, kLens4};
    static const int counts[5] = {(int)(sizeof(kLens0) / sizeof(Flare)), (int)(sizeof(kLens1) / sizeof(Flare)), (int)(sizeof(kLens2) / sizeof(Flare)),
                                  (int)(sizeof(kLens3) / sizeof(Flare)), (int)(sizeof(kLens4) / sizeof(Flare))};

    // The frame in 8 bits, B G R A, the bottom row first (VEGAS's order).
    std::vector<uint8_t> px((size_t)W * H * 4);
    for (int y = 0; y < H; ++y) {
        const int row = H - 1 - y;
        for (int x = 0; x < W; ++x) {
            const float* p = c.src.at(x, y);
            uint8_t* q = &px[((size_t)row * W + x) * 4];
            q[0] = (uint8_t)std::lrintf(clamp01(p[2]) * 255.f);
            q[1] = (uint8_t)std::lrintf(clamp01(p[1]) * 255.f);
            q[2] = (uint8_t)std::lrintf(clamp01(p[0]) * 255.f);
            q[3] = (uint8_t)std::lrintf(clamp01(p[3]) * 255.f);
        }
    }

    const float lcb[3] = {(float)lc.b, (float)lc.g, (float)lc.r};
    const float sinP = (float)std::sin((double)(-persp) * 1.5707963267948966);
    const float Wf = (float)W, Hf = (float)H, mn = (float)std::min(W, H);
    const float one = 1.f;
    const float om = one - tint;
    for (int n = 0; n < counts[lens]; ++n) {
        const Flare& fl = lists[lens][n];
        const float yoff = (-ly) * fl.place + ly, xoff = (-lx) * fl.place + lx;
        const float Y = Hf * yoff * 0.5f + Hf * 0.5f;
        const float cx = Wf * xoff * 0.5f + Wf * 0.5f;
        const float e = (fl.place - one) * sinP;
        const float s = (float)std::pow(2.0, (double)e);
        const float r = s * size * fl.size * mn;
        if (!(r > 0.f) || r > 1.0e5f || !std::isfinite(Y) || !std::isfinite(cx) || std::fabs(Y) > 1.0e6f || std::fabs(cx) > 1.0e6f) continue;
        const int a = (int)(Y - r), b = (int)(r + Y);
        const int edx = H - a, ecx = H - b;
        const float fy = (Hf - (Y - r)) - (float)edx;
        const double rx = (double)r, pxd = (double)cx;
        int ebx = (int)(pxd - rx);
        const float fx = (float)(pxd - rx) - (float)ebx;
        int r12 = (int)(rx + pxd) - ebx;
        const float rxf = (float)rx;
        const int rows = edx - ecx;
        int r14 = 0;
        if (((W - r12 - ebx - 1) | ebx) < 0) {
            r14 = ebx < 0 ? -ebx : 0;
            const int e2 = ebx >= 0 ? ebx : 0;
            if (e2 + r12 > W) r12 = r14 - e2 + W;
            ebx = e2;
        }
        const int edi = std::max(ecx, 0), r13 = ecx < 0 ? -ecx : 0;
        int endy = rows;
        if (rows + edi > H) endy = r13 - edi + H;
        if (endy <= r13 || r12 <= r14) continue;
        const float col[3] = {om * fl.c0 + tint * lcb[0], om * fl.c1 + tint * lcb[1], om * fl.c2 + tint * lcb[2]};
        const float I = inten * fl.strength;
        for (int yy = r13; yy < endy; ++yy) {
            const int row = edi + (yy - r13);
            if (row < 0 || row >= H) continue;
            const float v = 1.f - ((float)yy - fy) / r;
            const float vv = v * v;
            for (int xx = r14; xx < r12; ++xx) {
                const int cxi = ebx + (xx - r14);
                if (cxi < 0 || cxi >= W) continue;
                const float u = 1.f - ((float)xx - fx) / rxf;
                const float d = std::sqrt(u * u + vv);
                float o[3];
                if (!shape(fl.type, u, v, d, I, col, hex, rays, o)) continue;
                uint8_t* q = &px[((size_t)row * W + cxi) * 4];
                const float A = (float)q[3];
                for (int k = 0; k < 3; ++k) {
                    const float cin = (float)q[k];
                    const float sm = o[k] * 255.f + cin;
                    const float m = A < sm ? A : sm;
                    const float val = m * blend + cin * (one - blend);
                    q[k] = (uint8_t)(int)val;
                }
            }
        }
    }
    for (int y = 0; y < H; ++y) {
        const int row = H - 1 - y;
        for (int x = 0; x < W; ++x) {
            const uint8_t* q = &px[((size_t)row * W + x) * 4];
            float* o = c.out.at(x, y);
            o[0] = (float)q[2] / 255.f;
            o[1] = (float)q[1] / 255.f;
            o[2] = (float)q[0] / 255.f;
            o[3] = (float)q[3] / 255.f;
        }
    }
}

// Lens Flare for the GPU: what the CPU function above works out per flare (positions and boxes in the exact float / double arithmetic), as a table for the kernel, which does the
// per-pixel sums in the same order. 0 = the CPU does it, 1 = the frame is copied, 2 = the table is 64 ray lengths, the hexagon (x y nx ny, 6 each) at 64 and the flares from 96, 16 floats each
// (type, first row, first box row, end box row, first column, first box column, end box column, fy, fx, r, colour R G B, strength * intensity, 1).
int lensFlareGpu(Ctx& c, int W, int H, std::vector<float>& tab, int& nflares, float& blend)
{
    
    nflares = 0;
    if (W < 1 || H < 1) return 1;
    const Color lc = c.color("LightColor");
    double lpx, lpy;
    c.xy("LightPosition", lpx, lpy);
    const int lens = std::min(4, std::max(0, c.i("LensType")));
    const float tint = (float)c.d("Tint"), inten = (float)c.d("Intensity"), bl = (float)c.d("Blend"), size = (float)c.d("Size"), persp = (float)c.d("Perspective");
    const float lx = (float)lpx, ly = (float)-lpy;
    blend = bl;
    if (!(inten != 0.f) || !std::isfinite(lx) || !std::isfinite(ly)) return 1;
    static const Hexagon hex;
    static const RayTable rays;
    static const Flare* const lists[5] = {kLens0, kLens1, kLens2, kLens3, kLens4};
    static const int counts[5] = {(int)(sizeof(kLens0) / sizeof(Flare)), (int)(sizeof(kLens1) / sizeof(Flare)), (int)(sizeof(kLens2) / sizeof(Flare)),
                                  (int)(sizeof(kLens3) / sizeof(Flare)), (int)(sizeof(kLens4) / sizeof(Flare))};
    tab.assign(96, 0.f);
    for (int i = 0; i < 64; ++i) tab[(size_t)i] = (float)rays.t[i];
    for (int i = 0; i < 6; ++i) {
        tab[64 + (size_t)i] = hex.x[i];
        tab[70 + (size_t)i] = hex.y[i];
        tab[76 + (size_t)i] = hex.nx[i];
        tab[82 + (size_t)i] = hex.ny[i];
    }
    const float lcb[3] = {(float)lc.b, (float)lc.g, (float)lc.r};
    const float sinP = (float)std::sin((double)(-persp) * 1.5707963267948966);
    const float Wf = (float)W, Hf = (float)H, mn = (float)std::min(W, H);
    const float one = 1.f;
    const float om = one - tint;
    for (int n = 0; n < counts[lens]; ++n) {
        const Flare& fl = lists[lens][n];
        const float yoff = (-ly) * fl.place + ly, xoff = (-lx) * fl.place + lx;
        const float Y = Hf * yoff * 0.5f + Hf * 0.5f;
        const float cx = Wf * xoff * 0.5f + Wf * 0.5f;
        const float e = (fl.place - one) * sinP;
        const float sc = (float)std::pow(2.0, (double)e);
        const float r = sc * size * fl.size * mn;
        if (!(r > 0.f) || r > 1.0e5f || !std::isfinite(Y) || !std::isfinite(cx) || std::fabs(Y) > 1.0e6f || std::fabs(cx) > 1.0e6f) continue;
        const int a = (int)(Y - r), b = (int)(r + Y);
        const int edx = H - a, ecx = H - b;
        const float fy = (Hf - (Y - r)) - (float)edx;
        const double rx = (double)r, pxd = (double)cx;
        int ebx = (int)(pxd - rx);
        const float fx = (float)(pxd - rx) - (float)ebx;
        int r12 = (int)(rx + pxd) - ebx;
        const int rows = edx - ecx;
        int r14 = 0;
        if (((W - r12 - ebx - 1) | ebx) < 0) {
            r14 = ebx < 0 ? -ebx : 0;
            const int e2 = ebx >= 0 ? ebx : 0;
            if (e2 + r12 > W) r12 = r14 - e2 + W;
            ebx = e2;
        }
        const int edi = std::max(ecx, 0), r13 = ecx < 0 ? -ecx : 0;
        int endy = rows;
        if (rows + edi > H) endy = r13 - edi + H;
        if (endy <= r13 || r12 <= r14) continue;
        const float col[3] = {om * fl.c0 + tint * lcb[0], om * fl.c1 + tint * lcb[1], om * fl.c2 + tint * lcb[2]};
        const float rec[16] = {(float)fl.type, (float)edi, (float)r13, (float)endy, (float)ebx, (float)r14, (float)r12, fy, fx, r, col[2], col[1], col[0], inten * fl.strength, 1.f, 0.f};
        tab.insert(tab.end(), rec, rec + 16);
        ++nflares;
    }
    return nflares ? 2 : 1;
}

}  // namespace more
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
