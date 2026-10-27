// VEGAS Stereoscopic 3D Adjust (the Filters plug-in's maths, read off VEGAS Pro 18's code and pictures), for the left view (VEGAS's Left / Both flips and the Left floating
// window; Reno has the one picture). 8-bit throughout, as VEGAS's 8-bit processor:
//   1. the matrix, in the frame as it lies in memory (bottom row first, x across, y up): offsets (Horizontal / Vertical: half the setting in frame widths / heights, the other view
//      the other way), Zoom (left view scale 1 - Zoom / 2 about the middle), Keystone (frame corners moved up and down, a perspective transform through four point pairs),
//      Rotation (Rotation / -365 * 3.141592654 about the middle), each multiplied in on the right;
//   2. the crop: a scale about a point (x from the left view's inverse matrix applied to the middle, y from the other view's; the flips are in those matrices) by 1 + the crop. Automatic: for both views the segments from the
//      view's own middle (its inverse matrix applied to the middle) to the frame's corners are cut with the edges of the transformed frame (Graphics Gems' segment intersection, rounding half a unit away from zero); the part
//      from the corner to the cut over the part from the cut to the middle is the crop wanted at that corner; the largest of the eight is taken (times 1 + 1.4 |Keystone|);
//   3. no Keystone and no Rotation: the frame is a rectangle drawn by VEGAS's 14-bit fixed point bilinear stretch (edge pixels by coverage), the middle pixels two at a time in
//      16-bit steps; otherwise OpenCV 2.1's warpPerspective (INTER_LINEAR, constant border: 5 bit places, 15 bit weights; transparent outside);
//   4. the Left floating window: black over the columns of that part of the width.
// The flips are done to the source picture first.
#include "builtins_mask_internal.h"
#include "vkfilters.h"
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

// ------------------------------------------------------------------ the matrix
struct Mat3 {
    double m[3][3];
};
Mat3 mIdent()
{
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = i == j ? 1.0 : 0.0;
    return r;
}
Mat3 mMul(const Mat3& a, const Mat3& b)
{
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}
Mat3 mTrans(double tx, double ty)
{
    Mat3 r = mIdent();
    r.m[0][2] = tx;
    r.m[1][2] = ty;
    return r;
}
Mat3 mScale(double s)
{
    Mat3 r = mIdent();
    r.m[0][0] = s;
    r.m[1][1] = s;
    return r;
}
void mApply(const Mat3& m, double x, double y, double& ox, double& oy)
{
    const double w = m.m[2][0] * x + m.m[2][1] * y + m.m[2][2];
    ox = (m.m[0][0] * x + m.m[0][1] * y + m.m[0][2]) / w;
    oy = (m.m[1][0] * x + m.m[1][1] * y + m.m[1][2]) / w;
}

// The perspective transform that takes four points (single-precision, as VEGAS gives them) to four others: the eight unknowns of
// OpenCV's getPerspectiveTransform (the last element 1), by Gaussian elimination with pivoting.
Mat3 mInverse(const Mat3& a)
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

Mat3 mPerspective(const double src[4][2], const double dst[4][2])
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
bool segIntersect(const double* p1, const double* p2, const double* p3, const double* p4, double* out)
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
StereoMats stereoMatrix(double W, double H, double hz, double vt, double zoom, double key, double rot, int flipHc, int flipVc, bool autoCrop, double crop)
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

// ------------------------------------------------------------------ the pictures: 8-bit, four channels
struct Pic8 {
    int w = 0, h = 0;
    std::vector<uint8_t> p;
    void make(int W, int H)
    {
        w = W;
        h = H;
        p.assign((size_t)W * H * 4, 0);
    }
    uint8_t* at(int x, int y) { return &p[((size_t)y * w + x) * 4]; }
    const uint8_t* at(int x, int y) const { return &p[((size_t)y * w + x) * 4]; }
};

i64 interMax(i64 a, i64 b) { return a > b ? a : b; }
i64 interMin(i64 a, i64 b) { return a < b ? a : b; }
void inter(const i64* a, const i64* b, i64* r)
{
    r[0] = interMax(a[0], b[0]);
    r[1] = interMax(a[1], b[1]);
    r[2] = interMin(a[2], b[2]);
    r[3] = interMin(a[3], b[3]);
}
i64 ceil14(i64 v) { return (((v - 1) | 0x3fff) + 1) >> 14; }
i64 floor14(i64 v) { return (v & ~(i64)0x3fff) >> 14; }
// a * b / 16384 the way VEGAS has it: the doubles' product times 2^-14, cut to a whole number
i64 mulStep(i64 v, i64 st) { return (i64)((double)v * (double)st * 6.103515625e-05); }

// Where each pixel of the result takes its colour from. kind 0: nothing is drawn; 1 and 2: the stretch's bilinear sample (1: the plain one, 2: the two-at-a-time one) of the texels
// (x0, y0), (x1, y0), (x0, y1), (x1, y1) with the 14-bit fractions fx, fy, mixed in by the coverage cov (0..255); 3: OpenCV's warp, texel (x0, y0) and the 5-bit fractions fx, fy
// (the texels outside the picture are nothing).
struct Tap {
    int kind = 0;
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0, fx = 0, fy = 0, cov = 0;
};

// VEGAS's stretch of a whole picture (W x H) into the rectangle dR (x0, y0, x1, y1 in 1/16384 pixels, rows from the top): bilinear in
// 14-bit fixed point, the edge pixels mixed in by the part of them the rectangle covers; the taps of the pixels it draws. False when either is too small to draw.
bool stretchTaps(int W, int H, const i64* dR, std::vector<Tap>& taps)
{
    const int dh = H, dw = W, sh = H, sw = W;
    const i64 sFull[4] = {0, 0, (i64)sw << 14, (i64)sh << 14}, dFull[4] = {0, 0, (i64)dw << 14, (i64)dh << 14};
    i64 cs[4];
    inter(sFull, sFull, cs);
    if (cs[2] - cs[0] < 0x4000 || cs[3] - cs[1] < 0x4000) return false;
    if (dR[2] - dR[0] < 0x400 || dR[3] - dR[1] < 0x400) return false;
    const i64 stepX = (i64)((double)(cs[2] - cs[0]) * 16384.0 / (double)(dR[2] - dR[0]));
    const i64 stepY = (i64)((double)(cs[3] - cs[1]) * 16384.0 / (double)(dR[3] - dR[1]));
    if (stepX <= 0 || stepY <= 0) return false;
    i64 cd[4];
    inter(dR, dFull, cd);
    if (cd[2] - cd[0] < 0x400 || cd[3] - cd[1] < 0x400) return false;
    const i64 cw = cs[2] - cs[0], ch = cs[3] - cs[1];
    const i64 Rp[4] = {dR[0], dR[1], dR[0] + (i64)((double)cw * 16384.0 / (double)stepX), dR[1] + (i64)((double)ch * 16384.0 / (double)stepY)};
    i64 o[4];
    inter(Rp, cd, o);
    const i64 Sp[4] = {cs[0] + mulStep(o[0] - dR[0], stepX), cs[1] + mulStep(o[1] - dR[1], stepY), cs[0] + mulStep(o[2] - dR[0], stepX),
                       cs[1] + mulStep(o[3] - dR[1], stepY)};
    i64 fs[4];
    inter(Sp, cs, fs);
    auto st2 = [](i64 fsw, i64 cdw) -> i64 {
        if (cdw < 0x8000) return 0;
        const i64 v = (i64)((double)(fsw - 0x4000) * 16384.0 / (double)(cdw - 0x4000));
        return v > 0 ? v : 0;
    };
    const i64 sx = st2(fs[2] - fs[0], cd[2] - cd[0]), sy = st2(fs[3] - fs[1], cd[3] - cd[1]);
    const i64 X0 = floor14(cd[0]), X1 = ceil14(cd[2]), Y0 = floor14(cd[1]), Y1 = ceil14(cd[3]);
    i64 covX0 = ((X0 + 1) << 14) - cd[0], covY0 = ((Y0 + 1) << 14) - cd[1];
    const i64 covX1 = cd[2] - ((X1 - 1) << 14), covY1 = cd[3] - ((Y1 - 1) << 14);
    if (X1 - X0 < 2) covX0 = cd[2] - cd[0];
    if (Y1 - Y0 < 2) covY0 = cd[3] - cd[1];
    const i64 lastC = ceil14(fs[2]), lastR = ceil14(fs[3]);
    const i64 nX = X1 - X0, nY = Y1 - Y0;
    if (nX <= 0 || nY <= 0) return false;
    std::vector<i64> xpos((size_t)nX);
    {
        i64 p = fs[0];
        for (i64 k = 0; k < nX; ++k) {
            if (k == 0) {
            } else if (k == 1) {
                p += mulStep(covX0, sx);
            } else {
                p += sx;
            }
            xpos[(size_t)k] = p;
        }
        if (nX > 2) xpos[(size_t)nX - 1] = xpos[(size_t)nX - 2] + mulStep(covX1, sx);
    }
    auto clampc = [](i64 v, int n) -> int { return (int)(v < 0 ? 0 : (v >= n ? n - 1 : v)); };
    auto tapOf = [&](i64 y14, i64 x14, int kind) {
        Tap t;
        const i64 yi = y14 >> 14, xi = x14 >> 14;
        const i64 yb = yi + (yi != lastR - 1 ? 1 : 0), xb = xi + (xi != lastC - 1 ? 1 : 0);
        t.kind = kind;
        t.fx = (int)(x14 & 0x3fff);
        t.fy = (int)(y14 & 0x3fff);
        t.x0 = clampc(xi, sw);
        t.x1 = clampc(xb, sw);
        t.y0 = clampc(yi, sh);
        t.y1 = clampc(yb, sh);
        return t;
    };
    const i64 simdCols = (nX - 2) / 2 * 2;
    for (i64 r = 0; r < nY; ++r) {
        i64 y;
        if (r == 0) {
            y = fs[1];
        } else {
            y = fs[1] + (r - 1) * sy + mulStep(covY0, sy);
        }
        if (r == nY - 1 && nY > 2) y = fs[1] + (nY - 2) * sy + mulStep(covY0, sy) + mulStep(covY1, sy) - sy;
        const i64 rc = r == 0 ? covY0 : (r == nY - 1 && nY > 1 ? covY1 : 0x4000);
        const bool simdRow = r > 0 && r < nY - 1;
        for (i64 k = 0; k < nX; ++k) {
            const i64 x = xpos[(size_t)k];
            const i64 cc = k == 0 ? covX0 : (k == nX - 1 && nX > 1 ? covX1 : 0x4000);
            int a = (int)((double)rc * (double)cc * 6.103515625e-05) * 255;
            a = a >= 0 ? (a + ((a >> 31) & 0x3fff)) >> 14 : -((-a) >> 14);
            Tap t = tapOf(y, x, simdRow && k >= 1 && k <= nX - 2 && k - 1 < simdCols ? 2 : 1);
            t.cov = a;
            taps[(size_t)(Y0 + r) * W + (size_t)(X0 + k)] = t;
        }
    }
    return true;
}


// OpenCV 2.1's warpPerspective (linear, constant border 0) by the forward matrix m: the taps of every pixel (the picture's rows are in memory order, from the bottom;
// the taps are given for the rows from the top).
void warpTaps(int W, int H, const Mat3& m, std::vector<Tap>& taps)
{
    const double (*S)[3] = m.m;
    const double det = S[0][0] * (S[1][1] * S[2][2] - S[1][2] * S[2][1]) - S[0][1] * (S[1][0] * S[2][2] - S[1][2] * S[2][0]) +
                       S[0][2] * (S[1][0] * S[2][1] - S[1][1] * S[2][0]);
    if (det == 0) return;
    const double d = 1.0 / det;
    double M[9];
    M[0] = (S[1][1] * S[2][2] - S[1][2] * S[2][1]) * d;
    M[1] = (S[0][2] * S[2][1] - S[0][1] * S[2][2]) * d;
    M[2] = (S[0][1] * S[1][2] - S[0][2] * S[1][1]) * d;
    M[3] = (S[1][2] * S[2][0] - S[1][0] * S[2][2]) * d;
    M[4] = (S[0][0] * S[2][2] - S[0][2] * S[2][0]) * d;
    M[5] = (S[0][2] * S[1][0] - S[0][0] * S[1][2]) * d;
    M[6] = (S[1][0] * S[2][1] - S[1][1] * S[2][0]) * d;
    M[7] = (S[0][1] * S[2][0] - S[0][0] * S[2][1]) * d;
    M[8] = (S[0][0] * S[1][1] - S[0][1] * S[1][0]) * d;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const double X0 = M[0] * x + M[1] * y + M[2], Y0 = M[3] * x + M[4] * y + M[5], W0 = M[6] * x + M[7] * y + M[8];
            const double Wd = W0 != 0 ? 32.0 / W0 : 0.0;
            double fX = X0 * Wd, fY = Y0 * Wd;
            fX = std::max(-2147483648.0, std::min(2147483647.0, fX));
            fY = std::max(-2147483648.0, std::min(2147483647.0, fY));
            const int X = (int)std::nearbyint(fX), Y = (int)std::nearbyint(fY);
            const int xi = X >> 5, yi = Y >> 5, ax = X & 31, ay = Y & 31;
            Tap t;
            t.kind = 3;
            // (the texels far outside are all nothing; the numbers stay small)
            auto lim = [](int v, int n) { return v < -2 ? -2 : (v > n + 1 ? n + 1 : v); };
            t.x0 = lim(xi, W);
            t.x1 = lim(xi + 1, W);
            t.y0 = H - 1 - lim(yi, H);
            t.y1 = H - 1 - lim(yi + 1, H);
            t.fx = ax;
            t.fy = ay;
            taps[(size_t)(H - 1 - y) * W + x] = t;
        }
    }
}


// The colour of channel c a tap gives (before the coverage), from the 8-bit picture (rows from the top)
int tapValue(const Pic8& src, const Tap& t, int c)
{
    if (t.kind == 1) {
        const i64 fy = t.fy, fx = t.fx;
        const i64 w00 = ((16384 - fy) * (16384 - fx)) >> 4, w01 = ((16384 - fy) * fx) >> 4, w10 = ((16384 - fx) * fy) >> 4, w11 = (fx * fy) >> 4;
        const i64 p00 = src.at(t.x0, t.y0)[c], p01 = src.at(t.x1, t.y0)[c], p10 = src.at(t.x0, t.y1)[c], p11 = src.at(t.x1, t.y1)[c];
        return (int)((p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 0x800000) >> 24);
    }
    if (t.kind == 2) {
        const i64 fy = t.fy, fx = t.fx;
        const i64 A = ((0x3fff - fy) << 2) & 0xffff, B = (fy << 2) & 0xffff;
        const i64 w0 = ((0x3fff - fx) * A) >> 16, w1 = (fx * A) >> 16, w2 = ((0x3fff - fx) * B) >> 16, w3 = (fx * B) >> 16;
        const i64 p00 = src.at(t.x0, t.y0)[c], p01 = src.at(t.x1, t.y0)[c], p10 = src.at(t.x0, t.y1)[c], p11 = src.at(t.x1, t.y1)[c];
        auto tt = [](i64 p, i64 w) { return (p * 32 * w) >> 16; };
        i64 r = tt(p10, w2) + 4;
        if (r > 65535) r = 65535;
        r += tt(p11, w3);
        if (r > 65535) r = 65535;
        i64 q = tt(p01, w1) + tt(p00, w0);
        if (q > 65535) q = 65535;
        r += q;
        if (r > 65535) r = 65535;
        r >>= 3;
        return (int)(r > 255 ? 255 : r);
    }
    // kind 3: OpenCV's weights; the texels outside the picture are nothing
    const int ax = t.fx, ay = t.fy;
    const int w00 = (32 - ay) * (32 - ax) * 32, w01 = (32 - ay) * ax * 32, w10 = ay * (32 - ax) * 32, w11 = ay * ax * 32;
    auto px = [&](int yy, int xx) -> int { return (xx >= 0 && xx < src.w && yy >= 0 && yy < src.h) ? src.at(xx, yy)[c] : 0; };
    const int acc = px(t.y0, t.x0) * w00 + px(t.y0, t.x1) * w01 + px(t.y1, t.x0) * w10 + px(t.y1, t.x1) * w11;
    return (acc + 16384) >> 15;
}

// Everything about the picture that does not depend on its colours: the taps of every pixel and the floating window's columns
struct StereoTaps {
    std::vector<Tap> taps;
    bool stretchPath = false, flipH = false, flipV = false;
    int full = 0, part = 0;
};

bool stereoTaps(Ctx& c, int W, int H, StereoTaps& out)
{
    if (W <= 1 || H <= 1) return false;
    const double hz = clampd(c.d("Horizontal"), -1, 1), vt = clampd(c.d("Vertical"), -1, 1), zoom = clampd(c.d("Zoom"), -0.95, 10);
    const double key = clampd(c.d("Keystone"), -1, 1), rot = clampd(c.d("Rotation"), -180, 180);
    const int fhc = c.i("FlipHorizontal"), fvc = c.i("FlipVertical");
    out.flipH = fhc == 1 || fhc == 3;
    out.flipV = fvc == 1 || fvc == 3;
    const bool autoCrop = c.b("AutoCrop");
    const double crop = clampd(c.d("Crop"), 0, 0.5);
    const double fw = clampd(c.d("FloatingWindowLeft"), 0, 0.5);
    const StereoMats sm = stereoMatrix(W, H, hz, vt, zoom, key, rot, fhc, fvc, autoCrop, crop);
    out.taps.assign((size_t)W * H, Tap());
    // the rectangle (no Keystone, no Rotation) is drawn from the flipped picture; the warp has the flips in its matrix
    out.stretchPath = key == 0 && rot == 0;
    if (out.stretchPath) {
        double x0, y0, x1, y1;
        mApply(sm.a, 0, 0, x0, y0);
        mApply(sm.a, W, H, x1, y1);
        // (the matrix is for the frame with the bottom row first: from the top, y runs the other way)
        i64 dR[4] = {(i64)(x0 * 16384), (i64)(((double)H - y1) * 16384), (i64)(x1 * 16384), (i64)(((double)H - y0) * 16384)};
        if (dR[0] > dR[2]) std::swap(dR[0], dR[2]);
        if (dR[1] > dR[3]) std::swap(dR[1], dR[3]);
        stretchTaps(W, H, dR, out.taps);
    } else {
        warpTaps(W, H, sm.af, out.taps);
    }
    const double fcols = fw * W;
    out.full = (int)fcols;
    out.part = (int)((fcols - out.full) * 255.0 + 0.5);
    return true;
}

}  // namespace

void vegasStereo3dAdjust(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    StereoTaps st;
    if (!stereoTaps(c, W, H, st)) {
        c.out.px = c.src.px;
        return;
    }
    Pic8 src, dst;
    src.make(W, H);
    dst.make(W, H);
    for (int y = 0; y < H; ++y) {
        const int sy = st.flipV && st.stretchPath ? H - 1 - y : y;
        for (int x = 0; x < W; ++x) {
            const int sx = st.flipH && st.stretchPath ? W - 1 - x : x;
            const float* p = c.src.at(sx, sy);
            uint8_t* q = src.at(x, y);
            for (int k = 0; k < 4; ++k) q[k] = (uint8_t)std::nearbyint(std::min(1.f, std::max(0.f, p[k])) * 255.f);
        }
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const Tap& t = st.taps[(size_t)y * W + x];
            if (t.kind == 0) continue;
            uint8_t* d = dst.at(x, y);
            for (int k = 0; k < 4; ++k) {
                int v = tapValue(src, t, k);
                if (t.kind != 3) {                              // mixed in by the coverage (over nothing)
                    const int xx = v * t.cov + 128;
                    v = (xx + (xx >> 8)) >> 8;
                    v = v < 0 ? 0 : (v > 255 ? 255 : v);
                }
                d[k] = (uint8_t)v;
            }
        }
    const int full = st.full, part = st.part;
    for (int y = 0; y < H; ++y) {
        uint8_t* row = dst.at(0, y);
        for (int x = 0; x < full && x < W; ++x) {
            row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = 0;
            row[x * 4 + 3] = 255;
        }
        if (part > 0 && full < W) {
            uint8_t* q = row + (size_t)full * 4;
            for (int k = 0; k < 3; ++k) q[k] = (uint8_t)((q[k] * (255 - part) + 127) / 255);
            q[3] = (uint8_t)((q[3] * (255 - part) + 255 * part + 127) / 255);
        }
    }
    for (int y = 0; y < H; ++y) {
        const uint8_t* q = dst.at(0, y);
        float* o = c.out.at(0, y);
        for (int i = 0; i < W * 4; ++i) o[i] = (float)q[i] / 255.f;
    }
}

// For the GPU (vkfilters.cpp): 8 floats a pixel (kind, x0, x1, y0, y1, fx, fy, coverage) with the picture's flips in the texel positions, and the floating window's columns.
// False where the effect copies the picture.
bool stereoTapsGpu(Ctx& c, int W, int H, std::vector<float>& rec, unsigned& full, unsigned& part)
{
    StereoTaps st;
    if (!stereoTaps(c, W, H, st)) return false;
    rec.assign((size_t)W * H * 8, 0.f);
    for (size_t i = 0; i < st.taps.size(); ++i) {
        const Tap& t = st.taps[i];
        float* r = &rec[i * 8];
        r[0] = (float)t.kind;
        const bool fx = st.flipH && st.stretchPath, fy = st.flipV && st.stretchPath;
        r[1] = (float)(fx ? W - 1 - t.x0 : t.x0);
        r[2] = (float)(fx ? W - 1 - t.x1 : t.x1);
        r[3] = (float)(fy ? H - 1 - t.y0 : t.y0);
        r[4] = (float)(fy ? H - 1 - t.y1 : t.y1);
        r[5] = (float)t.fx;
        r[6] = (float)t.fy;
        r[7] = (float)t.cov;
    }
    full = (unsigned)std::max(0, st.full);
    part = (unsigned)std::max(0, st.part);
    return true;
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
