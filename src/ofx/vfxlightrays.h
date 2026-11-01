// VEGAS Light Rays, as the older (DirectX) effect draws it: a re-implementation of what its render function does (read from the user's own VEGAS install,
// sftrans1.dll, checked bit for bit against that function), not a fit. It works on 8-bit pixels of four bytes (blue, green, red, alpha) in place on a copy of
// the picture and has no other dependencies, so a test program can include it alone.
//
// What it does: the picture is cut into four quarters around the light source's pixel and drawn from the light outwards (upper left, upper right, lower left,
// lower right; each row a step further out, each pixel stepping away from the light). Every pixel takes the mean of a few samples of the RESULT drawn so far, on
// the line from the pixel towards the light (three samples a pixel apart, or as many as the larger of the two distances in pixels when that is under three),
// so the rays run on from pixel to pixel and are long. The result is mixed with the original: where the rays are at least as bright as the pixel the weight
// of the rays is the Strength weight (a fourth root, in 1/65536ths); where the pixel is brighter it is (ray mean - own mean + 256) x ((1 - Sensitivity) x 256
// x Strength weight >> 16), so bright parts keep their own colour. Noise (the 16-bit generator seed x 0x6255 + 0x3619) moves the original's colour a little first. A last pass multiplies the colour by the
// tint and mixes the original back in by Blend and its alpha.
#pragma once
#include <cmath>
#include <cstdint>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace reno {
namespace ofx {
namespace vfxlr {

// The settings as the effect's property block holds them (units of the OpenFX parameters).
struct Params {
    double lightX = 0.5, lightY = 0.5;   // fractions of the frame's width and height; y counts down the buffer
    double strength = 0.25, sensitivity = 0.6;
    bool boundRadius = false;
    double radiusX = 0.5, radiusY = 0.5, feather = 0.5;
    double tintR = 1, tintG = 1, tintB = 1;
    double blend = 0, noise = 0;
};

// cvttsd2si: whole number towards zero, the "indefinite" integer for what does not fit
inline int cvt32(double v) { return (v > -2147483649.0 && v < 2147483648.0) ? (int)v : INT_MIN; }
inline int64_t cvt64(double v) { return (v > -9223372036854775808.0 && v < 9223372036854775808.0) ? (int64_t)v : INT64_MIN; }

// 'src' and 'dst' are W x H pixels of four bytes with 'pitch' pixels a row. 'seed' is the noise generator's state (VEGAS keeps one for all frames).
// 'highQuality': VEGAS's final-quality flag (bilinear samples, strength curve not softened) as opposed to the preview's nearest samples.
inline void render(const Params& pr, int W, int H, int pitch, const uint8_t* src, uint8_t* dst, int32_t& seed, bool highQuality)
{
    if (W <= 0 || H <= 0 || pitch < W) return;
    const double scale = (double)std::min(W, H) / 300.0;
    int lx = cvt32((double)W * pr.lightX), ly = cvt32((double)H * pr.lightY);
    // Strength: the curve is flattened by the frame size, softened above 0.8 in preview, then a fourth root, as 16.16
    double st = std::pow(pr.strength, 1.0 / scale);
    if (!highQuality && st > 0.8) st = st * 0.5 + 0.4;
    const int sensQ = (int)cvt64(std::pow(st, 0.25) * 65536.0);
    const double rW = (double)W * pr.radiusX, rH = (double)H * pr.radiusY;
    const double inner = 1.0 - pr.feather;
    const double invRW = 1.0 / (1.0 > rW ? 1.0 : rW), invRH = 1.0 / (1.0 > rH ? 1.0 : rH);
    const double featherInv = (1.0 == inner) ? 0.0 : 1.0 / (1.0 - inner);
    const int strQ = (int)cvt64((1.0 - pr.sensitivity) * 256.0);
    const int noiseQ = (int)cvt64(pr.noise * 255.0);
    const int blendQ = (int)cvt64(pr.blend * 65536.0);
    const int tintR = (int)cvt64(pr.tintR * 256.0), tintG = (int)cvt64(pr.tintG * 256.0), tintB = (int)cvt64(pr.tintB * 256.0);
    const bool bound = pr.boundRadius;
    const int lxc = std::max(0, std::min(W - 1, lx == W ? lx - 1 : lx)), lyc = std::max(0, std::min(H - 1, ly == H ? ly - 1 : ly));
    const size_t total = (size_t)pitch * (size_t)H;
    auto px4 = [&](const uint8_t* base, long idx) -> const uint8_t* { return base + (size_t)idx * 4; };
    static const uint8_t kEdge[4] = {0xff, 0, 0, 0};   // what a sample beyond the right or bottom edge reads
    // a sample of the result drawn so far; reads past the buffer give zeros
    static const uint8_t kZero[4] = {0, 0, 0, 0};
    auto dstAt = [&](long idx) -> const uint8_t* { return (idx >= 0 && (size_t)idx < total) ? px4(dst, idx) : kZero; };

    for (int zone = 0; zone < 4; ++zone) {
        if ((zone == 1 || zone == 3) && lxc == W - 1) continue;
        if ((zone == 2 || zone == 3) && lyc == H - 1) continue;
        const int xdir = (zone & 1) ? 1 : -1, ydir = (zone & 2) ? 1 : -1;
        const int cols = (zone & 1) ? W - lxc - 1 : lxc + 1, rows = (zone & 2) ? H - lyc - 1 : lyc + 1;
        const int startX = (zone & 1) ? lxc + 1 : lxc, startY = (zone & 2) ? lyc + 1 : lyc;
        std::memcpy(dst + ((size_t)startY * pitch + lxc) * 4, src + ((size_t)startY * pitch + lxc) * 4, 4);
        int y = startY;
        for (int row = 0; row < rows; ++row, y += ydir) {
            const int dy = lyc - y, dy2 = dy * dy;
            const double dyD = (double)dy;
            int x = startX;
            for (int col = 0; col < cols; ++col, x += xdir) {
                const int dx = lxc - x;
                const long selfIdx = (long)y * pitch + x;
                const uint8_t* sp = px4(src, selfIdx);
                uint8_t* dp = dst + (size_t)selfIdx * 4;
                const double dist = std::sqrt((double)(dx * dx + dy2));
                int mask = 0;
                if (bound) {
                    const double a = (double)std::abs(dx) * invRW, b = (double)std::abs(dy) * invRH;
                    const double d = std::sqrt(b * b + a * a);
                    if (d > inner) {
                        if (d >= 1.0) mask = 0;
                        else mask = 0x10000 - (int)cvt64((d - inner) * featherInv * 65536.0);
                    } else mask = 0x10000;
                }
                const double inv = (0.0 == dist) ? 0.0 : 1.0 / dist;
                const int adx = std::abs(dx), ady = std::abs(dy);
                int steps = 3;
                double w = 1.0 / 3.0;
                if (adx < 3 && ady < 3) {
                    steps = std::max(adx, ady);
                    w = steps ? 1.0 / (double)steps : 0.0;
                }
                double vx = (double)dx * inv, vy = dyD * inv;
                const double ux = (-0.99 > vx) ? -0.99 : (0.99 < vx ? 0.99 : vx);
                const double uy = (-0.99 > vy) ? -0.99 : (0.99 < vy ? 0.99 : vy);
                int ray[4];   // byte 0 .. 3
                uint8_t masked[4] = {sp[0], sp[1], sp[2], sp[3]};   // the original with the radius mask on its colour (alpha stays)
                if (bound) for (int k = 0; k < 3; ++k) masked[k] = (uint8_t)(((uint32_t)sp[k] * (uint32_t)mask) >> 16);
                if (steps == 0) {
                    for (int k = 0; k < 4; ++k) ray[k] = sp[k];
                } else {
                    double px = (double)x, py = (double)y;
                    double acc[4] = {0, 0, 0, 0};
                    for (int s = 0; s < steps; ++s) {
                        px += ux;
                        py += uy;
                        int x0 = cvt32(px), y0 = cvt32(py);
                        uint8_t got[4];
                        if (!highQuality) {
                            // nearest: one pixel on towards the light's side unless that is the light's own row or column
                            if ((zone & 1) == 0 && x0 != lxc) ++x0;
                            if (zone <= 1 && y0 != lyc) ++y0;
                            const long idx = (long)pitch * y0 + x0;
                            const uint8_t* q = (x0 == x && y0 == y) ? px4(src, idx) : dstAt(idx);
                            if (idx < 0 || (size_t)idx >= total) q = kZero;
                            std::memcpy(got, q, 4);
                        } else {
                            const double fxd = px - (double)x0, fyd = py - (double)y0;
                            const int fx = (int)cvt64(fxd * 256.0), fy = (int)cvt64(fyd * 256.0);
                            const int wx = 256 - fx, wy = 256 - fy;
                            const long idx = (long)pitch * y0 + x0;
                            const uint8_t *A = dstAt(idx), *B, *C, *D;
                            if (x0 != W - 1 && y0 != H - 1) {
                                B = dstAt(idx + 1); C = dstAt(idx + pitch); D = dstAt(idx + pitch + 1);
                            } else {
                                D = kEdge;
                                B = (x0 == W - 1) ? kEdge : dstAt(idx + 1);
                                C = (y0 == H - 1) ? kEdge : dstAt(idx + pitch);
                            }
                            // a corner that is the pixel being drawn itself reads the original instead
                            if (std::fabs(px - (double)x) < 1.0 && std::fabs(py - (double)y) < 1.0) {
                                const uint8_t* self = bound ? masked : sp;
                                if (fxd == 0.0 && fyd == 0.0) A = self;
                                else if (zone == 0) A = self;
                                else if (zone == 1) B = self;
                                else if (zone == 2) C = self;
                                else D = self;
                            }
                            for (int k = 0; k < 4; ++k) {
                                const uint32_t top = (uint32_t)B[k] * (uint32_t)fx + (uint32_t)A[k] * (uint32_t)wx;
                                const uint32_t bot = (uint32_t)D[k] * (uint32_t)fx + (uint32_t)C[k] * (uint32_t)wx;
                                got[k] = (uint8_t)((top * (uint32_t)wy + bot * (uint32_t)fy) >> 16);
                            }
                        }
                        for (int k = 0; k < 4; ++k) acc[k] = (double)got[k] * w + acc[k];
                    }
                    for (int k = 0; k < 4; ++k) ray[k] = cvt32(acc[k]);
                }
                // mix the ray with the original
                uint8_t sb[4] = {masked[0], masked[1], masked[2], masked[3]};
                const uint32_t rayAvg = (uint32_t)(((uint64_t)((uint32_t)(ray[0] & 255) + (uint32_t)(ray[1] & 255) + (uint32_t)(ray[2] & 255)) * 0xaaaaaaabull) >> 32) >> 1;
                const uint32_t srcAvg = (uint32_t)(((uint64_t)((uint32_t)sb[0] + (uint32_t)sb[1] + (uint32_t)sb[2]) * 0xaaaaaaabull) >> 32) >> 1;
                uint32_t rw;
                if (srcAvg > rayAvg) {
                    const uint32_t f = ((uint32_t)strQ * (uint32_t)sensQ) >> 16;
                    rw = (rayAvg - srcAvg + 256u) * f;
                } else rw = (uint32_t)sensQ;
                const uint32_t sw = 0x10000u - rw;
                if (noiseQ != 0) {
                    uint32_t s = (uint32_t)seed * 0x6255u + 0x3619u;
                    s &= 0x8000ffffu;
                    if ((int32_t)s < 0) s = ((s - 1u) | 0xffff0000u) + 1u;
                    seed = (int32_t)s;
                    if (srcAvg > 1) {
                        int32_t e = (int32_t)(srcAvg * s);
                        e = (int32_t)((uint32_t)e + (uint32_t)e);
                        e = (e + ((e < 0) ? 0xffff : 0)) >> 16;
                        e -= (int32_t)srcAvg;
                        e = (int32_t)((uint32_t)e * (uint32_t)noiseQ);
                        const int32_t delta = (e + ((e < 0) ? 0xff : 0)) >> 8;
                        if (delta > 0) {
                            for (int k = 0; k < 3; ++k) {
                                const int32_t t = (255 - (int32_t)sb[k]) * delta;
                                sb[k] = (uint8_t)(sb[k] + (uint8_t)(t >> 8));
                            }
                        } else if (delta < 0) {
                            for (int k = 0; k < 3; ++k) {
                                const int32_t t = delta * (int32_t)sb[k];
                                sb[k] = (uint8_t)(sb[k] + (uint8_t)((t + 0xff) >> 8));
                            }
                        }
                    }
                }
                for (int k = 0; k < 4; ++k)
                    dp[k] = (uint8_t)((((uint32_t)sb[k] * sw) + ((uint32_t)(ray[k] & 255) * rw)) >> 16);
            }
            // (the row loop's pitch step is in the index arithmetic)
        }
    }
    // tint, and the original mixed back in by Blend x its alpha
    if (blendQ != 0 || tintR != 0 || tintG != 0 || tintB != 0) {
        for (int y = 0; y < H; ++y) {
            const uint8_t* s = src + (size_t)y * pitch * 4;
            uint8_t* d = dst + (size_t)y * pitch * 4;
            for (int x = 0; x < W; ++x, s += 4, d += 4) {
                const uint32_t m = (uint32_t)s[3] * (uint32_t)blendQ;
                const uint32_t t = (uint32_t)(((uint64_t)m * 0x80808081ull) >> 32) >> 7;
                const uint32_t r9 = 0x10000u - t;
                const uint8_t v0 = (uint8_t)(((uint32_t)d[0] * (uint32_t)tintB) >> 8);
                const uint8_t v1 = (uint8_t)(((uint32_t)d[1] * (uint32_t)tintG) >> 8);
                const uint8_t v2 = (uint8_t)(((uint32_t)d[2] * (uint32_t)tintR) >> 8);
                const uint32_t d3 = d[3];
                d[2] = (uint8_t)(((uint32_t)s[2] * t + (uint32_t)v2 * r9) >> 16);
                d[1] = (uint8_t)(((uint32_t)s[1] * t + (uint32_t)v1 * r9) >> 16);
                d[0] = (uint8_t)(((uint32_t)s[0] * t + (uint32_t)v0 * r9) >> 16);
                d[3] = (uint8_t)(((uint32_t)s[3] * t + d3 * r9) >> 16);
            }
        }
    }
}

}  // namespace vfxlr
}  // namespace ofx
}  // namespace reno
