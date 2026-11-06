// VEGAS Add Noise as its OpenCL processor draws it (the path VEGAS takes with GPU acceleration on): a re-implementation of what its kernel and the host code that sets it up
// do (read from the user's own VEGAS install, Vfx1.ofx), checked against the numpy replica of the kernel and against VEGAS's own pictures. No dependencies, so a test program can
// include it alone.
//
// Every column of the picture is one work item with its own "ranlux" generator (luxury level 0: a subtract-with-borrow generator on 24 floats, 24 bits each). The host fills
// the columns' states from a small linear congruential generator (x = x * 40014 mod 2147483563, seeded with column + width * seed + 1) and the kernel throws away 384 numbers,
// then takes four numbers per row, starting at the bottom row. Uniform noise is the first three minus a half; Gaussian noise is the Box-Muller pair (sine and cosine) of two
// pairs of them. Monochromatic repeats the first value in all three channels. The noise is added to the picture's colour with the alpha taken out, and put back in again.
// The seed is rand() of the C library when Animate is on (a number from 0 to 32767 that nobody can reproduce) and 0 when it is off.
#pragma once
#include "../core/rmath.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace reno {
namespace ofx {
namespace vfxnoise {

struct F4 { float x, y, z, w; };

// The host's state set-up for one column: 24 numbers of 24 bits, as floats; six float4 planes and a carry (2^-24 when the last number is 0).
inline void initColumn(int column, int width, int seed, float st[24], float* carry)
{
    uint32_t x = (uint32_t)column + (uint32_t)width * (uint32_t)seed + 1u;      // (32-bit wrapping arithmetic, as the machine's)
    for (int k = 0; k < 24; ++k) {
        // x * 40014 mod 2147483563 by Schrage's way, on signed 32-bit numbers (x may be negative after the wrap: division towards zero)
        const int32_t xs = (int32_t)x;
        const int32_t q = xs / 53668;
        int32_t rem = (int32_t)((uint32_t)xs * 40014u - (uint32_t)q * 2147483563u);
        if (rem < 0) rem = (int32_t)((uint32_t)rem + 2147483563u);
        x = (uint32_t)rem;
        st[k] = (float)(x & 0xffffffu) * 5.9604644775390625e-08f;           // 2^-24
    }
    *carry = st[23] == 0.f ? 5.9604644775390625e-08f : 0.f;
}

// One work item's generator (the kernel's td0: v1..v6 and the carry / counter plane).
struct Ranlux {
    F4 v[6];
    float carry = 0.f;
    int z = 0;          // the group of four numbers that comes next, times four (0 .. 20)

    // f2..f5: one subtract-with-borrow step: new = a - b - carry (plus 1 and a new carry of 2^-24 when it went below 0), stored over b; the number given out is the new value, nudged
    // when it is tiny and never 0.
    static float step(float a, float& b, float& carry)
    {
        float t = a - b - carry;
        if (t < 0.f) { t += 1.f; carry = 5.9604644775390625e-08f; }
        else carry = 0.f;
        b = t;
        float r = t;
        if (t < 0.000244140625f) r += 5.9604644775390625e-08f * a;
        if (r == 0.f) r = 5.9604644775390625e-08f * 5.9604644775390625e-08f;
        return r;
    }
    void load(const float st[24], float c)
    {
        for (int p = 0; p < 6; ++p) v[p] = F4{st[4 * p], st[4 * p + 1], st[4 * p + 2], st[4 * p + 3]};
        carry = c;
        z = 0;
    }
    // the groups (A for the x and y steps, A for the z and w steps, B written), planes counted from 0 (the kernel's v1..v6)
    F4 group(int g)
    {
        static const int ap[6] = {2, 1, 0, 5, 4, 3}, aq[6] = {1, 0, 5, 4, 3, 2}, bb[6] = {5, 4, 3, 2, 1, 0};
        F4& A = v[ap[g]];
        F4& A2 = v[aq[g]];
        F4& B = v[bb[g]];
        F4 o;
        o.x = step(A.y, B.w, carry);
        o.y = step(A.x, B.z, carry);
        o.z = step(A2.w, B.y, carry);
        o.w = step(A2.z, B.x, carry);
        return o;
    }
    void warmUp()
    {
        for (int i = 0; i < 16; ++i)
            for (int g = 0; g < 6; ++g) group(g);
    }
    // f6: four numbers
    F4 next()
    {
        const F4 o = group(z / 4);
        z = (z + 4) % 24;
        return o;
    }
};

struct Params {
    float level = 0.5f;
    bool mono = true, gauss = false;
    int seed = 0;       // 0 .. 32767 (0 with Animate off)
};

// The OpenCL 8-bit store: write_imagef to the card's 8-bit picture. Found on Add Noise and Levels: the float is cut to 12 bits first and the byte is the nearest level to that
// (exact halves down): byte = (floor(v * 4096) * 255 + 2047) >> 12, v kept in 0 .. 1, NaN 0. Returns the level / 255.
inline int store12(float v)
{
    if (!(v > 0.f)) return 0;
    if (v > 1.f) v = 1.f;
    const int q = (int)std::floor(v * 4096.f);
    return (q * 255 + 2047) >> 12;
}

inline float box(float a, float b, float& cosPart)
{
    const float r = std::sqrt(-2.f * rm::logf(a));
    const float ang = 2.f * 3.1415926535897932384f * b;
    cosPart = r * rm::cosf(ang);
    return r * rm::sinf(ang);
}

// The state each column's work item starts from, 25 floats per column (24 numbers, the carry), as the kernel reads it. The host fills W columns' states in planes with stride W; the kernel
// reads plane k of work item g at g + k * G (G: W up to a multiple of 16), which is another column's number when W is not a multiple of 16 (beyond the buffer: zeros here; what the card
// gives there is not defined).
inline void columnStates(int W, int seed, std::vector<float>& out)
{
    const int G = (W + 15) / 16 * 16;
    std::vector<float> init((size_t)W * 25);        // per column: 24 numbers and the carry
    for (int i = 0; i < W; ++i) {
        float* s = &init[(size_t)i * 25];
        initColumn(i, W, seed, s, &s[24]);
    }
    // number comp of plane k as the work item g reads it (plane 6 is the carry, in its x component)
    auto plane = [&](int k, int g, int comp) -> float {
        const size_t idx = (size_t)k * G + g;
        if (idx >= (size_t)7 * W) return 0.f;
        const size_t p = idx / (size_t)W, col = idx % (size_t)W;
        if (p == 6) return comp == 0 ? init[col * 25 + 24] : 0.f;
        return init[col * 25 + p * 4 + comp];
    };
    out.assign((size_t)W * 25, 0.f);
    for (int x = 0; x < W; ++x) {
        for (int p = 0; p < 6; ++p)
            for (int c = 0; c < 4; ++c) out[(size_t)x * 25 + p * 4 + c] = plane(p, x, c);
        out[(size_t)x * 25 + 24] = plane(6, x, 0);
    }
}

// img: W x H RGBA floats, premultiplied, rows TOP first (as Reno keeps them); out: the same size, may be the same array. The kernel's rows run from the bottom. The result is
// the float picture; the caller stores it with store12 when the frame is 8-bit.
inline void render(const float* img, float* out, int W, int H, const Params& P)
{
    if (W <= 0 || H <= 0) return;
    std::vector<float> cols;
    columnStates(W, P.seed, cols);
    for (int x = 0; x < W; ++x) {
        Ranlux R;
        R.load(&cols[(size_t)x * 25], cols[(size_t)x * 25 + 24]);
        R.warmUp();
        for (int j = 0; j < H; ++j) {           // j: the kernel's row, from the bottom
            const int y = H - 1 - j;
            const float* p = img + ((size_t)y * W + x) * 4;
            float* o = out + ((size_t)y * W + x) * 4;
            const float a = p[3];
            const F4 u = R.next();
            float n0, n1, n2;
            if (P.gauss) {
                float c0, c1;
                n0 = box(u.x, u.y, c0);
                n1 = c0;
                n2 = box(u.z, u.w, c1);
            } else {
                n0 = u.x - 0.5f; n1 = u.y - 0.5f; n2 = u.z - 0.5f;
            }
            if (P.mono) { n1 = n0; n2 = n0; }
            if (a > 0.000001f) {
                if (a < 1.f) {
                    const float r = (p[0] / a + P.level * n0) * a, g = (p[1] / a + P.level * n1) * a, b = (p[2] / a + P.level * n2) * a;
                    o[0] = r; o[1] = g; o[2] = b; o[3] = a;
                } else {
                    const float r = p[0] + P.level * n0, g = p[1] + P.level * n1, b = p[2] + P.level * n2;
                    o[0] = r; o[1] = g; o[2] = b; o[3] = 1.f;
                }
            } else {
                o[0] = o[1] = o[2] = o[3] = 0.f;
            }
        }
    }
}

}  // namespace vfxnoise
}  // namespace ofx
}  // namespace reno
