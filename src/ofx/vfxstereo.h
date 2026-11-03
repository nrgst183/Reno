// VEGAS's Stereoscopic 3D Adjust (Filters.ofx): the matrix builder (0x180189760, read off the code and checked against it with the DLL loaded as an oracle) and the warp that draws with it.
// The capture goes through the warp path: the destination pixel (x, y) of the frame as it lies in memory (bottom row first) is looked up at the inverse matrix applied to it, with a float
// bilinear read (continuous coordinates, no half pixel; taps outside the picture are nothing) and stored as floor(v + 0.46) in 8 bits. No dependencies, so a test program can include it alone.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace reno {
namespace ofx {
namespace vfxstereo {

struct Mat3 {
    double m[3][3];
};
inline Mat3 mIdent()
{
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = i == j ? 1.0 : 0.0;
    return r;
}
inline Mat3 mMul(const Mat3& a, const Mat3& b)
{
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}
inline Mat3 mTrans(double tx, double ty)
{
    Mat3 r = mIdent();
    r.m[0][2] = tx;
    r.m[1][2] = ty;
    return r;
}
inline Mat3 mScale(double s)
{
    Mat3 r = mIdent();
    r.m[0][0] = s;
    r.m[1][1] = s;
    return r;
}
inline void mApply(const Mat3& m, double x, double y, double& ox, double& oy)
{
    const double w = m.m[2][0] * x + m.m[2][1] * y + m.m[2][2];
    ox = (m.m[0][0] * x + m.m[0][1] * y + m.m[0][2]) / w;
    oy = (m.m[1][0] * x + m.m[1][1] * y + m.m[1][2]) / w;
}

// The perspective transform that takes four points (single-precision, as VEGAS gives them) to four others: the eight unknowns of
// OpenCV's getPerspectiveTransform (the last element 1), by Gaussian elimination with pivoting.
inline Mat3 mInverse(const Mat3& a)
{
    const double (*m)[3] = a.m;
    const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    Mat3 r = mIdent();
    if (det == 0) return r;
    const double id = 1.0 / det;
    r.m[0][0] = c00 * id;
    r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
    r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
    r.m[1][0] = c01 * id;
    r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
    r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
    r.m[2][0] = c02 * id;
    r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
    r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
    return r;
}

inline Mat3 mPerspective(const double src[4][2], const double dst[4][2])
{
    double a[8][9];
    for (int i = 0; i < 4; ++i) {
        const double x = (float)src[i][0], y = (float)src[i][1], u = (float)dst[i][0], v = (float)dst[i][1];
        const double r0[9] = {x, y, 1, 0, 0, 0, -x * u, -y * u, u};
        const double r1[9] = {0, 0, 0, x, y, 1, -x * v, -y * v, v};
        for (int k = 0; k < 9; ++k) {
            a[i][k] = r0[k];
            a[i + 4][k] = r1[k];
        }
    }
    for (int c = 0; c < 8; ++c) {
        int p = c;
        for (int r = c + 1; r < 8; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (p != c)
            for (int k = 0; k < 9; ++k) std::swap(a[p][k], a[c][k]);
        const double d = a[c][c];
        if (d == 0) continue;
        for (int r = 0; r < 8; ++r) {
            if (r == c) continue;
            const double f = a[r][c] / d;
            if (f == 0) continue;
            for (int k = c; k < 9; ++k) a[r][k] -= f * a[c][k];
        }
    }
    double s[8];
    for (int i = 0; i < 8; ++i) s[i] = a[i][i] != 0 ? a[i][8] / a[i][i] : 0.0;
    Mat3 r;
    r.m[0][0] = s[0]; r.m[0][1] = s[1]; r.m[0][2] = s[2];
    r.m[1][0] = s[3]; r.m[1][1] = s[4]; r.m[1][2] = s[5];
    r.m[2][0] = s[6]; r.m[2][1] = s[7]; r.m[2][2] = 1.0;
    return r;
}

// Graphics Gems' lines_intersect in doubles: the segment p1 p2 against p3 p4; the point is rounded half a unit away from zero.
inline bool segIntersect(const double* p1, const double* p2, const double* p3, const double* p4, double* out)
{
    const double x1 = p1[0], y1 = p1[1], x2 = p2[0], y2 = p2[1], x3 = p3[0], y3 = p3[1], x4 = p4[0], y4 = p4[1];
    const double Ax = x1 - x2, Ay = y2 - y1;
    const double n8 = x2 * y1 - x1 * y2;
    const double r3 = x3 * Ay + y3 * Ax + n8, r4 = x4 * Ay + y4 * Ax + n8;
    if (r3 != 0 && r4 != 0 && ((r3 > 0 && r4 > 0) || (r3 < 0 && r4 < 0))) return false;
    const double a1 = y4 - y3, b1 = x3 - x4, c1 = x4 * y3 - y4 * x3;
    const double r1 = x1 * a1 + y1 * b1 + c1, r2 = x2 * a1 + y2 * b1 + c1;
    if (r1 != 0 && r2 != 0 && ((r1 > 0 && r2 > 0) || (r1 < 0 && r2 < 0))) return false;
    const double den = b1 * Ay - a1 * Ax;
    if (den == 0) return false;
    const double off = (den < 0 ? -0.5 : 0.5) * den;
    const double num = c1 * Ax - b1 * n8;
    out[0] = (num < 0 ? num - off : num + off) / den;
    const double num2 = a1 * n8 - c1 * Ay;
    out[1] = (num2 < 0 ? num2 - off : num2 + off) / den;
    return true;
}

// The left view's matrix (memory coordinates) with its crop; b the other view's, built the same way.
struct StereoMats {
    Mat3 a;   // without the flips (the picture is flipped first)
    Mat3 af;  // with the flips in it (x -> W - x, y -> H - y), for the unflipped picture
};
inline StereoMats stereoMatrix(double W, double H, double hz, double vt, double zoom, double key, double rot, int flipHc, int flipVc, bool autoCrop, double crop)
{
    Mat3 A = mIdent(), B = mIdent();
    if (hz != 0) {
        A = mMul(A, mTrans(-0.5 * hz * W, 0));
        B = mMul(B, mTrans(0.5 * hz * W, 0));
    }
    if (vt != 0) {
        A = mMul(A, mTrans(0, 0.5 * vt * H));
        B = mMul(B, mTrans(0, -0.5 * vt * H));
    }
    if (zoom != 0) {
        const double z = -0.5 * zoom;
        A = mMul(mMul(mMul(A, mTrans(W / 2, H / 2)), mScale(1 + z)), mTrans(-W / 2, -H / 2));
        B = mMul(mMul(mMul(B, mTrans(W / 2, H / 2)), mScale(1 - z)), mTrans(-W / 2, -H / 2));
    }
    if (key != 0) {
        const double k = -0.5 * key;
        const double sq[4][2] = {{0, 0}, {W, 0}, {0, H}, {W, H}};
        const double dA[4][2] = {{0, -k * H}, {W, k * H}, {0, k * H + H}, {W, H - k * H}};
        const double dB[4][2] = {{0, k * H}, {W, -k * H}, {0, -k * H + H}, {W, H + k * H}};
        A = mMul(A, mPerspective(sq, dA));
        B = mMul(B, mPerspective(sq, dB));
    }
    if (rot != 0) {
        const double th = rot / -365.0 * 3.141592654;
        Mat3 Ra = mIdent(), Rb = mIdent();
        Ra.m[0][0] = std::cos(th); Ra.m[1][0] = std::sin(th); Ra.m[0][1] = -Ra.m[1][0]; Ra.m[1][1] = Ra.m[0][0];
        Rb.m[0][0] = std::cos(-th); Rb.m[1][0] = std::sin(-th); Rb.m[0][1] = -Rb.m[1][0]; Rb.m[1][1] = Rb.m[0][0];
        A = mMul(mMul(mMul(A, mTrans(W / 2, H / 2)), Ra), mTrans(-W / 2, -H / 2));
        B = mMul(mMul(mMul(B, mTrans(W / 2, H / 2)), Rb), mTrans(-W / 2, -H / 2));
    }
    // the real code multiplies the flips in last (x -> W - x, y -> H - y; view A for 1 and 3, view B for 2 and 3) and the crop is worked out with them in
    // the matrices; here the picture is flipped before the matrix, so the crop's point is mirrored back and the flips are left out of the returned matrix
    const bool fAx = flipHc == 1 || flipHc == 3, fBx = flipHc == 2 || flipHc == 3, fAy = flipVc == 1 || flipVc == 3, fBy = flipVc == 2 || flipVc == 3;
    Mat3 Af = A, Bf = B;
    if (fAx) { Mat3 F = mIdent(); F.m[0][0] = -1; F.m[0][2] = W; Af = mMul(Af, F); }
    if (fBx) { Mat3 F = mIdent(); F.m[0][0] = -1; F.m[0][2] = W; Bf = mMul(Bf, F); }
    if (fAy) { Mat3 F = mIdent(); F.m[1][1] = -1; F.m[1][2] = H; Af = mMul(Af, F); }
    if (fBy) { Mat3 F = mIdent(); F.m[1][1] = -1; F.m[1][2] = H; Bf = mMul(Bf, F); }
    if (crop != 0 || autoCrop) {
        double iA[2], iB[2];
        const Mat3 Ainv = mInverse(Af), Binv = mInverse(Bf);
        mApply(Ainv, W / 2, H / 2, iA[0], iA[1]);
        mApply(Binv, W / 2, H / 2, iB[0], iB[1]);
        double zc;
        if (autoCrop) {
            double best = 0.0;
            bool any = false;
            // each view: its own frame's corners through its matrix, cut by segments from the middle of the frame pushed through the inverse of that matrix
            const Mat3* ms[2] = {&Af, &Bf};
            const double* cs[2] = {iA, iB};
            const double corners[4][2] = {{0, 0}, {W, 0}, {W, H}, {0, H}};
            for (int v = 0; v < 2; ++v) {
                double q[4][2];
                for (int i = 0; i < 4; ++i) mApply(*ms[v], corners[i][0], corners[i][1], q[i][0], q[i][1]);
                for (int ci = 0; ci < 4; ++ci) {
                    double I[2] = {0, 0};
                    bool hit = false;
                    for (int i = 0; i < 4 && !hit; ++i) hit = segIntersect(cs[v], corners[ci], q[i], q[(i + 1) % 4], I);
                    double r = 0.0;
                    if (hit) {
                        const double da = std::hypot(corners[ci][0] - I[0], corners[ci][1] - I[1]);
                        const double D = std::hypot(corners[ci][0] - cs[v][0], corners[ci][1] - cs[v][1]);
                        r = da / (D - da);
                    }
                    if (!any || r > best) best = r;
                    any = true;
                }
            }
            zc = best * (1 + std::fabs(key * 1.4));
        } else {
            zc = crop;
        }
        const double s = 1 + zc;
        // the scale is about (x of A's inverse middle, y of B's inverse middle), in the source picture
        double px = iA[0], py = iB[1];
        if (fAx) px = W - px;
        if (fAy) py = H - py;
        A = mMul(mMul(mMul(A, mTrans(px, py)), mScale(s)), mTrans(-px, -py));
        Af = mMul(mMul(mMul(Af, mTrans(iA[0], iB[1])), mScale(s)), mTrans(-iA[0], -iB[1]));
    }
    StereoMats r;
    r.a = A;
    r.af = Af;
    return r;
}

// The warp: mem is the frame as it lies in memory (bottom row first, RGBA, values in levels 0..255 as floats), m the matrix of stereoMatrix (frame -> result, with the flips in it). Every
// result pixel takes the inverse matrix's point, read with a bilinear mix of the four texels around it (continuous coordinates; texels outside the frame count as 0, alpha too). round:
// the 8-bit processor's store floor(v + 0.46) (kept to 0..255); off: the float value as it is. out has the same layout.
inline void warp(const std::vector<float>& mem, int W, int H, const Mat3& m, bool round, std::vector<float>& out)
{
    out.assign((size_t)W * H * 4, 0.f);
    const Mat3 Mi = mInverse(m);
    auto tex = [&](int x, int y, int k) -> double { return (x >= 0 && x < W && y >= 0 && y < H) ? (double)mem[((size_t)y * W + x) * 4 + k] : 0.0; };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double d = Mi.m[2][0] * x + Mi.m[2][1] * y + Mi.m[2][2];
            double sx = (Mi.m[0][0] * x + Mi.m[0][1] * y + Mi.m[0][2]) / d, sy = (Mi.m[1][0] * x + Mi.m[1][1] * y + Mi.m[1][2]) / d;
            float* o = &out[((size_t)y * W + x) * 4];
            if (!(sx > -2.0 && sx < W + 1.0 && sy > -2.0 && sy < H + 1.0)) continue;      // (every texel is outside, or the point is not a number)
            const double fx0 = std::floor(sx), fy0 = std::floor(sy);
            const int x0 = (int)fx0, y0 = (int)fy0;
            const double fx = sx - fx0, fy = sy - fy0;
            for (int k = 0; k < 4; ++k) {
                const double v = (tex(x0, y0, k) * (1 - fx) + tex(x0 + 1, y0, k) * fx) * (1 - fy) + (tex(x0, y0 + 1, k) * (1 - fx) + tex(x0 + 1, y0 + 1, k) * fx) * fy;
                o[k] = round ? (float)std::min(255.0, std::max(0.0, std::floor(v + 0.46))) : (float)v;
            }
        }
}

}  // namespace vfxstereo
}  // namespace ofx
}  // namespace reno
