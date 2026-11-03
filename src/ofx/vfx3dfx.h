// The four 3D transitions of VEGAS (3D Shuffle, 3D Blinds, 3D Cascade, 3D Fly In/Out), as the older (DirectX) effects draw them: the part before the rasteriser (vfx3d.h), a
// re-implementation of what their render functions do (vfx1.dll, read from the user's own VEGAS install and checked against them pixel for pixel on random settings), not a fit.
//
// Every one of them lays the picture (or slices of it) out as flat quads in space, turns and moves them along key-frame splines, cuts them to the view, projects them, and hands
// the polygons to the rasteriser. What they share (the frame is w x h, mx the longer side, wf = w / mx, hf = h / mx, asp the pixel aspect):
//  - a quad is the unit square, moved to its place by: shift (-.5, -.5), scale (wf x asp, hf, 1), the three rotations, shift (position + (0, 0, z + 1)); the matrices multiply
//    new x old, the rotation is by minus the angle (the code's sign), single precision all along;
//  - the view is a pyramid with the four corner rays (+-(0.5 wf asp - 1e-5), +-(0.5 hf - 1e-5), 1) and a near plane at 0.01; the polygon is cut to it (Sutherland-Hodgman);
//    the projection divides by w and puts the middle of the frame at (mx wf / 2, mx hf / 2) pixels (x = y = 0 at the top left of the first row);
//  - the light: the specular light slider times a curve of the progress (shape(), 0 .. 1 .. 0), the point light at fixed fractions of the frame (single precision), the strength
//    of the light the curve value; no light at all (mode 1: only the directional one) when it is 0;
//  - the key-frame spline (tension 0.5, the key's bias weight 0.8 pulling the handles in) is the one of VEGAS's keyframe controls.
#pragma once
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "vfx3d.h"

namespace reno {
namespace ofx {
namespace vfx3d {

namespace fx {

// ---------------------------------------------------------------- the key-frame spline (0x1802565f0)
// A key: d[0] the time, d[1..3] the value (three numbers), d[4] the bias weight.
struct SPt {
    double d[5];
};

inline SPt handle(const SPt& A, const SPt& B, const SPt& C, double T)
{
    double dx[5], ex[5];
    for (int k = 0; k < 5; ++k) {
        dx[k] = B.d[k] - A.d[k];
        ex[k] = C.d[k] - B.d[k];
    }
    const double la = std::sqrt(dx[1] * dx[1] + dx[2] * dx[2] + dx[3] * dx[3]);
    const double lb = std::sqrt(ex[1] * ex[1] + ex[2] * ex[2] + ex[3] * ex[3]);
    if (la == 0 && lb == 0) return B;
    double sA, r;
    if (lb > la) {
        r = (la / lb) * T;
        sA = T + 1.0;
    } else {
        sA = ((lb / la) * T) + 1.0;
        r = T;
    }
    SPt out;
    for (int k = 0; k < 5; ++k) {
        const double Pa = dx[k] * sA + A.d[k];
        const double Pc = ex[k] * r + B.d[k];
        out.d[k] = (Pc - Pa) * 0.5 + Pa;
    }
    return out;
}

inline SPt bezier(const SPt& P0, const SPt& P1, const SPt& P2, const SPt& P3, double u)
{
    SPt a, b, c, d, e, f;
    for (int k = 0; k < 5; ++k) {
        a.d[k] = (P1.d[k] - P0.d[k]) * u + P0.d[k];
        b.d[k] = (P2.d[k] - P1.d[k]) * u + P1.d[k];
        c.d[k] = (P3.d[k] - P2.d[k]) * u + P2.d[k];
    }
    for (int k = 0; k < 5; ++k) {
        d.d[k] = (b.d[k] - a.d[k]) * u + a.d[k];
        e.d[k] = (c.d[k] - b.d[k]) * u + b.d[k];
    }
    for (int k = 0; k < 5; ++k) f.d[k] = (e.d[k] - d.d[k]) * u + d.d[k];
    return f;
}

// n keys in order of time; the value at time t
inline SPt splineAt(const SPt* K, int n, double t)
{
    const double T = 0.5;
    int lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const int mid = (hi + lo) >> 1;
        if (K[mid].d[0] <= t) lo = mid;
        else hi = mid;
    }
    int i = lo;
    while (i < n && K[i].d[0] <= t) ++i;
    auto cl = [&](int j) { return j < 0 ? 0 : (j > n - 1 ? n - 1 : j); };
    const SPt& Pm2 = K[cl(i - 2)];
    const SPt& Pm1 = K[cl(i - 1)];
    const SPt& Pi = K[cl(i)];
    const SPt& Pp1 = K[cl(i + 1)];
    SPt H1 = handle(Pm2, Pm1, Pi, T), H2 = handle(Pp1, Pi, Pm1, T);
    const double Kc = 1.0 / 3.0;
    const double w1 = Pm1.d[4], w2 = Pi.d[4];
    if (1.0 > w1)
        for (int k = 0; k < 5; ++k) {
            const double Q = (Pi.d[k] - Pm1.d[k]) * Kc + Pm1.d[k];
            H1.d[k] = (H1.d[k] - Q) * w1 + Q;
        }
    if (1.0 > w2)
        for (int k = 0; k < 5; ++k) {
            const double R = (Pm1.d[k] - Pi.d[k]) * Kc + Pi.d[k];
            H2.d[k] = (H2.d[k] - R) * w2 + R;
        }
    const double dt = Pi.d[0] - Pm1.d[0];
    double u = 0;
    if (dt > 0) u = (t - Pm1.d[0]) / dt;
    return bezier(Pm1, H1, H2, Pi, u);
}

// A pose key: position, scale, rotation (radians), time
struct Key {
    double v[9];
    double t;
};
// The pose at time t: the three groups (position, scale, rotation) are splined one by one.
inline void poseAt(const Key* K, int n, double t, double out[9])
{
    SPt pts[8];
    if (n > 8) n = 8;
    for (int c = 0; c < 3; ++c) {
        for (int i = 0; i < n; ++i) {
            pts[i].d[0] = K[i].t;
            pts[i].d[1] = K[i].v[c * 3];
            pts[i].d[2] = K[i].v[c * 3 + 1];
            pts[i].d[3] = K[i].v[c * 3 + 2];
            pts[i].d[4] = 0.8;
        }
        const SPt r = splineAt(pts, n, t);
        out[c * 3] = r.d[1];
        out[c * 3 + 1] = r.d[2];
        out[c * 3 + 2] = r.d[3];
    }
}

// The light curve over the progress: dim 0 the strength (0, 0.7, 1, 0.7, 0), dim 1 the fade of 3D Fly In/Out (0, 0.4, 1, 1, 1), keys at 0, 0.2, 0.5, 0.8, 1.
inline double shape(double t, int dim)
{
    static const double SC[5][3] = {{0, 0, 0}, {0.7, 0.4, 0.2}, {1, 1, 0.5}, {0.7, 1, 0.8}, {0, 1, 1}};
    SPt p[5];
    for (int i = 0; i < 5; ++i) {
        p[i].d[0] = SC[i][2];
        p[i].d[1] = SC[i][dim ? 1 : 0];
        p[i].d[2] = p[i].d[3] = 0;
        p[i].d[4] = 0.8;
    }
    return splineAt(p, 5, t).d[1];
}

// ---------------------------------------------------------------- matrices (row major, column vectors, the shift in m[3], m[7], m[11])
typedef float M4[16];
inline void identity(M4 m)
{
    std::memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1;
}
// out = A x out
inline void mulInto(M4 out, const M4 A)
{
    M4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = A[i * 4 + 0] * out[0 * 4 + j] + A[i * 4 + 1] * out[1 * 4 + j] + A[i * 4 + 2] * out[2 * 4 + j] + A[i * 4 + 3] * out[3 * 4 + j];
    std::memcpy(out, r, 64);
}
inline void shift(float x, float y, float z, M4 out)
{
    M4 t;
    identity(t);
    t[3] = x;
    t[7] = y;
    t[11] = z;
    mulInto(out, t);
}
inline void scale(float x, float y, float z, M4 out)
{
    M4 t;
    identity(t);
    t[0] = x;
    t[5] = y;
    t[10] = z;
    mulInto(out, t);
}
// rotation about the axis (x, y, z) by minus 'ang'
inline void rotate(float x, float y, float z, float ang, M4 out)
{
    const double a = (double)ang;
    const double c = std::cos(a), s0 = std::sin(a);
    const float t = (float)(1.0 - c), cf = (float)c, s = (float)s0;
    const float tx = t * x, ty = t * y, tz = t * z;
    M4 m;
    identity(m);
    m[0] = tx * x + cf;
    m[1] = tx * y + s * z;
    m[2] = tx * z - s * y;
    m[4] = tx * y - s * z;
    m[5] = ty * y + cf;
    m[6] = ty * z + s * x;
    m[8] = tx * z + s * y;
    m[9] = ty * z - s * x;
    m[10] = tz * z + cf;
    mulInto(out, m);
}
inline void perspective(M4 m, double a, double b, double c)
{
    identity(m);
    m[0] = (float)(b / a);
    m[5] = (float)(c / a);
    m[14] = (float)(1.0 / a);
    m[15] = 0;
}
inline void transform(float* v, const M4 m)
{
    const float x = v[0], y = v[1], z = v[2], w = v[3];
    float nx = y * m[1] + x * m[0];
    nx += z * m[2];
    nx += w * m[3];
    float ny = x * m[4] + y * m[5];
    ny += z * m[6];
    ny += w * m[7];
    float nz = x * m[8] + y * m[9];
    nz += z * m[10];
    nz += w * m[11];
    float nw = x * m[12] + y * m[13];
    nw += z * m[14];
    nw += w * m[15];
    v[0] = nx;
    v[1] = ny;
    v[2] = nz;
    v[3] = nw;
}

// ---------------------------------------------------------------- polygons
inline float dot3(const float* a, const float* b)
{
    float r = a[1] * b[1];
    const float q = a[0] * b[0];
    r += q;
    r += a[2] * b[2];
    return r;
}
struct Plane {
    float d, n[3];
};
// Sutherland-Hodgman against the planes in turn: a point is inside when n.p >= d
inline void clipPoly(Poly& P, const Plane* pl, int np)
{
    Poly T;
    Poly* src = &P;
    Poly* dst = &T;
    for (int k = 0; k < np; ++k) {
        const float* n = pl[k].n;
        const float d = pl[k].d;
        int cnt = 0;
        const int N = src->n;
        if (N > 0 && N <= 20) {
            float sPrev = dot3(src->v[0], n);
            bool inPrev = sPrev >= d;
            for (int i = 0; i < N; ++i) {
                if (inPrev) std::memcpy(dst->v[cnt++], src->v[i], 28);
                const int j = (i + 1) % N;
                const float sj = dot3(src->v[j], n);
                const bool inj = sj >= d;
                if (inj != inPrev) {
                    const float t = (d - sPrev) / (sj - sPrev);
                    const float* a = src->v[i];
                    const float* b = src->v[j];
                    float* o = dst->v[cnt];
                    for (int c = 0; c < 7; ++c) o[c] = (b[c] - a[c]) * t + a[c];
                    ++cnt;
                }
                inPrev = inj;
                sPrev = sj;
            }
        }
        dst->n = cnt;
        std::swap(src, dst);
        if (cnt <= 2) {
            P.n = cnt;
            if (src != &P) P = *src;
            return;
        }
    }
    if (src != &P) P = *src;
}
inline void cross3(const float* a, const float* b, float* o)
{
    o[0] = b[2] * a[1] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - b[0] * a[1];
}
// The unit normal (from the corner pairs scanned from the last corner back; a cross product shorter than 1e-6 is no normal). False when there is none.
inline bool normalOf(const Poly& P, float* nr)
{
    const float e0[3] = {P.v[1][0] - P.v[0][0], P.v[1][1] - P.v[0][1], P.v[1][2] - P.v[0][2]};
    float len = 0;
    int k = P.n - 1;
    for (; k > 1; --k) {
        const float b[3] = {P.v[k][0] - P.v[0][0], P.v[k][1] - P.v[0][1], P.v[k][2] - P.v[0][2]};
        cross3(e0, b, nr);
        float l = nr[0] * nr[0];
        l = l + nr[1] * nr[1];
        l = l + nr[2] * nr[2];
        len = (float)std::sqrt((double)l);
        if (len > 9.999999975e-07f) break;
    }
    if (k <= 1 && !(len > 9.999999975e-07f)) {
        if (9.999999975e-07f > len) return false;
    }
    nr[0] /= len;
    nr[1] /= len;
    nr[2] /= len;
    return true;
}

// ---------------------------------------------------------------- the view
struct View {
    int w, h;
    double asp, mx, wf, hf;
    float wa, hh;               // the frame's size in the unit of the quads: wf x asp, hf
    float hx, hy;               // the middle of the frame in pixels
    M4 persp;
    Plane pl[5];
};

inline View makeView(int w, int h, double asp)
{
    View V;
    V.w = w;
    V.h = h;
    V.asp = asp;
    V.mx = w > h ? w : h;
    V.wf = w / V.mx;
    V.hf = h / V.mx;
    perspective(V.persp, V.mx, V.mx / asp, V.mx);
    const double hw = V.wf * asp * 0.5, hhh = V.hf * 0.5;
    const float xs0 = (float)(1e-5 - hw), xs1 = (float)(hw - 1e-5), ys0 = (float)(1e-5 - hhh), ys1 = (float)(hhh - 1e-5);
    const float rv[4][3] = {{xs0, ys0, 1}, {xs1, ys0, 1}, {xs1, ys1, 1}, {xs0, ys1, 1}};
    for (int i = 0; i < 4; ++i) {
        const float* a = rv[i];
        const float* b = rv[(i + 1) % 4];
        const float cx = b[2] * a[1] - a[2] * b[1];
        const float cy = a[2] * b[0] - a[0] * b[2];
        const float cz = a[0] * b[1] - b[0] * a[1];
        float l = cx * cx;
        l = l + cy * cy;
        l = l + cz * cz;
        const float len = (float)std::sqrt((double)l);
        V.pl[i].n[0] = cx / len;
        V.pl[i].n[1] = cy / len;
        V.pl[i].n[2] = cz / len;
        V.pl[i].d = dot3(a, V.pl[i].n);
    }
    V.pl[4].d = 0.01f;
    V.pl[4].n[0] = 0;
    V.pl[4].n[1] = 0;
    V.pl[4].n[2] = 1;
    V.wa = (float)(V.wf * asp);
    V.hh = (float)V.hf;
    V.hx = (float)(V.mx * V.wf * 0.5);
    V.hy = (float)(V.mx * V.hf * 0.5);
    return V;
}

// the perspective, the divide and the move to the middle of the frame
inline void project(Poly& P, const View& V)
{
    for (int i = 0; i < P.n; ++i) {
        float* v = P.v[i];
        transform(v, V.persp);
        v[0] /= v[3];
        v[1] /= v[3];
        v[2] /= v[3];
        v[0] += V.hx;
        v[1] += V.hy;
    }
}

// The unit square of a picture (corners (0,0) (1,0) (1,1) (0,1), u v the same, e 1)
inline void unitQuad(Poly& P)
{
    static const float Q[4][7] = {{0, 0, 0, 1, 0, 0, 1}, {1, 0, 0, 1, 1, 0, 1}, {1, 1, 0, 1, 1, 1, 1}, {0, 1, 0, 1, 0, 1, 1}};
    std::memset(&P, 0, sizeof P);
    P.n = 4;
    for (int i = 0; i < 4; ++i) std::memcpy(P.v[i], Q[i], 28);
}

// ---------------------------------------------------------------- what the front ends hand to the painter
struct Item {
    Poly poly;
    float nrm[3];
    int src;                    // 0: the outgoing picture, 1: the incoming one
};
struct Plan {
    std::vector<Item> items;
    float lightX, lightY, lightZ;       // the point light in pixels
    float lightAmt;
    double fade;
};

// the point light at (0.5 + ox, 0.5 + oy) of the frame and oz x the longer side in front of it (single precision, as the code's)
inline void setLight(Plan& P, const View& V, float ox, float oy, float oz, float amt)
{
    const float half = 0.5f;
    P.lightX = (half + ox) * (float)V.w;
    P.lightY = (half + oy) * (float)V.h;
    P.lightZ = oz * (float)V.mx;
    P.lightAmt = amt;
}

// ---------------------------------------------------------------- 3D Shuffle
// The two pictures swap places: each flies a key-frame path through space (the outgoing one up and left, the incoming one down and right and back), the one that is nearer
// on top. Settings: the specular light 0..1.
inline Plan shufflePlan(int w, int h, double asp, double light, double prog)
{
    static const Key T1[3] = {{{0, 0, 0.5, 1, 1, 1, 0, 0, 0}, 0},
                              {{-0.4, 0.4, 0.3, 1, 1, 1, 0.52333333333333332, 0, -1.0466666666666666}, 0.4},
                              {{0, 0, 0, 1, 1, 1, 0, 0, 0}, 1}};
    static const Key T2[3] = {{{0, 0, 0, 1, 1, 1, 0, 0, 0}, 0},
                              {{0.4, -0.4, 0.3, 1, 1, 1, 0.52333333333333332, 0, -1.0466666666666666}, 0.4},
                              {{0, 0, 0.5, 1, 1, 1, 0, 0, 0}, 1}};
    const View V = makeView(w, h, asp);
    Plan P;
    double o1[9], o2[9];
    poseAt(T1, 3, prog, o1);
    poseAt(T2, 3, prog, o2);
    const double* os[2] = {o1, o2};
    P.lightAmt = (float)(shape(prog, 0) * light);
    setLight(P, V, 0.52f, -0.52f, 0.9f, P.lightAmt);
    P.fade = 1.0;
    const bool firstHalf = 0.5 > prog;
    for (int r = 0; r < 2; ++r) {
        const int layer = firstHalf ? r : (r ^ 1);
        M4 m;
        identity(m);
        shift(-0.5f, -0.5f, 0.0f, m);
        scale(V.wa, V.hh, 1.0f, m);
        const double* o = os[layer];
        rotate(1, 0, 0, (float)o[6], m);
        rotate(0, 1, 0, (float)o[7], m);
        rotate(0, 0, 1, (float)o[8], m);
        shift((float)o[0], (float)o[1], (float)(o[2] + 1.0), m);
        Item it;
        std::memset(&it, 0, sizeof it);
        unitQuad(it.poly);
        for (int i = 0; i < 4; ++i) transform(it.poly.v[i], m);
        clipPoly(it.poly, V.pl, 5);
        if (!(it.poly.n > 2)) continue;
        normalOf(it.poly, it.nrm);
        project(it.poly, V);
        it.src = 1 - layer;       // the path of table 1 carries the incoming picture, table 2 the outgoing one
        P.items.push_back(it);
    }
    return P;
}

// ---------------------------------------------------------------- 3D Fly In/Out
// One picture flies in from a far pose (or out to it): position (far x, far y, far z + 1) around the middle, turned by (x, y, z) half turns (the sliders are in whole turns of pi),
// along a spline, fading in over the first part. Settings: far x/y 0..1, far z, the three rotations, the light, and 'in' (1: fly in, 0: fly out).
struct FlySettings {
    double farX, farY, farZ, rotX, rotY, rotZ, light;
    int in;
};
inline Plan flyPlan(int w, int h, double asp, const FlySettings& S, double prog)
{
    const double PI = 3.141592653589793;
    const int flag = S.in == 0 ? 1 : 0;      // 1: fly out (the keys run from rest to far)
    const View V = makeView(w, h, asp);
    Plan P;
    Key K[2];
    const double z1 = S.farZ + 1.0;
    Key far_ = {{(S.farX - 0.5) * z1, (S.farY - 0.5) * z1, z1, 1, 1, 1, S.rotX * PI, S.rotY * PI, S.rotZ * PI}, 0.0};
    Key rest = {{0, 0, 0, 1, 1, 1, 0, 0, 0}, 1.0};
    if (!flag) {
        K[0] = far_;
        K[1] = rest;
    } else {
        K[0] = rest;
        K[0].t = 0.0;
        K[1] = far_;
        K[1].t = 1.0;
    }
    double o[9];
    poseAt(K, 2, prog, o);
    P.lightAmt = (float)(shape(prog, 0) * S.light);
    setLight(P, V, 0.32f, -0.3f, 0.95f, P.lightAmt);
    P.fade = shape(flag ? 1.0 - prog : prog, 1);
    M4 m;
    identity(m);
    shift(-0.5f, -0.5f, 0.0f, m);
    scale(V.wa, V.hh, 1.0f, m);
    rotate(1, 0, 0, (float)o[6], m);
    rotate(0, 1, 0, (float)o[7], m);
    rotate(0, 0, 1, (float)o[8], m);
    shift((float)o[0], (float)o[1], (float)(o[2] + 1.0), m);
    Item it;
    std::memset(&it, 0, sizeof it);
    unitQuad(it.poly);
    for (int i = 0; i < 4; ++i) transform(it.poly.v[i], m);
    clipPoly(it.poly, V.pl, 5);
    if (it.poly.n > 2) {
        normalOf(it.poly, it.nrm);
        project(it.poly, V);
        it.src = flag ? 0 : 1;       // the moving picture: the outgoing one when flying out
        P.items.push_back(it);
    }
    return P;
}


// ---------------------------------------------------------------- 3D Blinds
// The picture is cut into slats that turn over one after the other, the outgoing picture on the front of a slat and the incoming one on its back. Slats that have not
// started are one block (still the outgoing picture), slats that have finished another (the incoming picture); the ones turning are drawn on their own.
// direction: 0 left to right, 1 right to left (vertical slats, turning about their upright axis), 2 top to bottom, 3 bottom to top (horizontal slats).
struct BlindsSettings {
    int divisions, extraSpins;
    double stagger, light;
    int direction;
};
inline void slatQuad(Poly& P, int mode, float a, float b)
{
    std::memset(&P, 0, sizeof P);
    P.n = 4;
    for (int k = 0; k < 4; ++k) {
        P.v[k][3] = 1.0f;
        P.v[k][6] = 1.0f;
    }
    if (mode <= 1) {
        P.v[0][0] = a; P.v[0][4] = a;
        P.v[1][0] = b; P.v[1][4] = b;
        P.v[2][0] = b; P.v[2][4] = b;
        P.v[3][0] = a; P.v[3][4] = a;
        P.v[2][1] = 1; P.v[2][5] = 1;
        P.v[3][1] = 1; P.v[3][5] = 1;
    } else if (mode <= 3) {
        P.v[1][0] = 1; P.v[1][4] = 1;
        P.v[2][0] = 1; P.v[2][4] = 1;
        P.v[0][1] = a; P.v[0][5] = a;
        P.v[1][1] = a; P.v[1][5] = a;
        P.v[2][1] = b; P.v[2][5] = b;
        P.v[3][1] = b; P.v[3][5] = b;
    }
}
inline Plan blindsPlan(int w, int h, double asp, const BlindsSettings& S, double prog)
{
    const View V = makeView(w, h, asp);
    const double PI = 3.1415926535897931;
    Plan P;
    P.fade = 1.0;
    const int dir = S.direction < 0 ? 0 : (S.direction > 3 ? 3 : S.direction);
    const int n = S.divisions < 1 ? 1 : (S.divisions > 128 ? 128 : S.divisions);
    P.lightAmt = (float)(shape(prog, 0) * S.light);
    setLight(P, V, 0.52f, -0.4f, 1.35f, P.lightAmt);
    const double inv = 1.0 / (double)n;
    const double x3 = inv * S.stagger;
    double x2 = inv - x3;
    x2 *= (double)(n - 1);
    const double x1 = 1.0 - x2;
    const double A = 1.0 / x1;
    const double off = A * x3, dur = A * inv;
    std::vector<int> state(n);
    std::vector<float> ang(n);
    int nfin = 0, npend = 0;
    for (int i = 0; i < n; ++i) {
        const bool rev = dir == 1 || dir == 3;
        const double sign = rev ? -1.0 : 1.0;
        const int e = rev ? (n - 1 - i) : i;
        const double start = (double)e * off;
        const double end = start + dur;
        if (prog >= end) {
            ++nfin;
            state[i] = 0;
            ang[i] = (float)(sign * PI);
        } else if (start > prog) {
            state[i] = 0;
            ang[i] = 0.0f;
            ++npend;
        } else {
            state[i] = 1;
            const double u = (prog - start) / dur;
            const double x = 1.0 - u;
            double q = (1.0 - x * x) * ((double)(2 * S.extraSpins + 1) * PI);
            q *= sign;
            ang[i] = (float)q;
        }
    }
    // put the (already moved) slat polygon through the view; back: it shows its other side
    auto process = [&](Poly Q, int srcFixed) {
        float nr[3];
        if (!normalOf(Q, nr)) return;
        const float d = dot3(nr, Q.v[0]);
        const bool back = d <= 0.0f;
        if (back) {
            if (dir < 2 || dir > 3) {
                const float t = Q.v[0][4], t1 = Q.v[1][4];
                Q.v[0][4] = t1; Q.v[3][4] = t1; Q.v[1][4] = t; Q.v[2][4] = t;
            } else {
                const float t0 = Q.v[0][5], t3 = Q.v[3][5];
                Q.v[0][5] = t3; Q.v[1][5] = t3; Q.v[3][5] = t0; Q.v[2][5] = t0;
            }
        }
        clipPoly(Q, V.pl, 5);
        if (Q.n <= 2) return;
        if (!normalOf(Q, nr)) return;
        project(Q, V);
        Item it;
        std::memset(&it, 0, sizeof it);
        it.poly = Q;
        std::memcpy(it.nrm, nr, 12);
        it.src = srcFixed >= 0 ? srcFixed : (back ? 1 : 0);
        P.items.push_back(it);
    };
    M4 Mb;
    identity(Mb);
    shift(-0.5f, -0.5f, 0, Mb);
    scale(V.wa, V.hh, 1.0f, Mb);
    shift(0.5f, 0.5f, 0, Mb);
    shift(-0.5f, -0.5f, 1.0f, Mb);
    if (nfin > 0) {
        float a = 0.0f;
        if (dir == 1 || dir == 3) a = (float)(1.0 - (double)nfin * inv);
        const float b = (float)((double)nfin * inv + (double)a);
        Poly Q;
        slatQuad(Q, dir, a, b);
        for (int k = 0; k < 4; ++k) transform(Q.v[k], Mb);
        process(Q, 1);
    }
    if (npend > 0) {
        float a = 0.0f;
        if ((dir & ~2) == 0) a = (float)(1.0 - (double)npend * inv);
        const float b = (float)((double)npend * inv + (double)a);
        Poly Q;
        slatQuad(Q, dir, a, b);
        for (int k = 0; k < 4; ++k) transform(Q.v[k], Mb);
        process(Q, -1);
    }
    for (int i = 0; i < n; ++i) {
        if (!state[i]) continue;
        const float a = (float)i / (float)n, b = (float)(i + 1) / (float)n;
        Poly Q;
        slatQuad(Q, dir, a, b);
        M4 M0;
        identity(M0);
        shift(-0.5f, -0.5f, 0, M0);
        scale(V.wa, V.hh, 1.0f, M0);
        shift(0.5f, 0.5f, 0, M0);
        for (int k = 0; k < 4; ++k) transform(Q.v[k], M0);
        const float cyf = (Q.v[0][1] + Q.v[3][1]) * 0.5f;
        const float cxf = (Q.v[0][0] + Q.v[1][0]) * 0.5f;
        const double cy = (double)cyf, cx = (double)cxf;
        M4 M2;
        identity(M2);
        shift((float)(-cx), (float)(-cy), 0, M2);
        if (dir >= 2) rotate(1, 0, 0, ang[i], M2);
        else rotate(0, 1, 0, ang[i], M2);
        shift((float)(cx - 0.5), (float)(cy - 0.5), 1.0f, M2);
        for (int k = 0; k < 4; ++k) transform(Q.v[k], M2);
        process(Q, -1);
    }
    return P;
}

// ---------------------------------------------------------------- 3D Cascade
// The incoming picture is cut into strips that fall into place one after the other, each along a key-frame path (turning over from behind, twisted by 'twist'); the strips
// that have landed are copied in flat.
// direction: 0 left to right, 1 right to left (vertical strips), 2 top to bottom, 3 bottom to top (horizontal strips).
struct CascadeSettings {
    int divisions, direction;
    double twist, light;
};
struct CascadePlan {
    Plan plan;
    int finished;               // the first strips (in the order they are numbered) that have landed
    bool horizontalRun;         // the strips lie in a row from left to right (direction 0, 1): the blit rect runs along x
    bool flip;                  // the run is counted from the far side
    int n;
};
inline CascadePlan cascadePlan(int w, int h, double asp, const CascadeSettings& S, double prog)
{
    const double PI3 = 1.0471975511965976;
    const View V = makeView(w, h, asp);
    CascadePlan C;
    Plan& P = C.plan;
    P.fade = 1.0;
    const int dir = S.direction < 0 ? 0 : (S.direction > 3 ? 3 : S.direction);
    const int n = S.divisions < 1 ? 1 : (S.divisions > 64 ? 64 : S.divisions);
    Key K[3] = {{{0, -1.5, -0.4, 1, 1, 1, 3.14, 0.0, 0.0}, 0.0}, {{0, -0.1, -0.3, 1, 1, 1, 1.9, 0.0, 0.0}, 0.7}, {{0, 0, 0, 1, 1, 1, 0, 0, 0}, 1.0}};
    bool vflag = true, flip = false;      // vflag: the strips are cut along y
    float l8 = 0.32f, l10 = 0.52f;
    const double tw = S.twist;
    if (dir == 0 || dir == 1) {
        const double r0 = K[0].v[6], r1 = K[1].v[6];
        K[0].v[7] = -r0;
        K[1].v[7] = -r1;
        K[0].v[6] = tw * PI3;
        K[1].v[6] = K[0].v[6] * 0.375;
        vflag = false;
        flip = dir == 1;
        l8 = dir == 0 ? 0.52f : -0.52f;
        l10 = 0.32f;
    } else if (dir == 2) {
        K[0].v[7] = tw * PI3;
        K[1].v[7] = K[0].v[7] * 0.375;
    } else {
        K[0].v[7] = tw * PI3;
        K[1].v[7] = (tw * PI3) * tw;
        flip = true;
        l10 = -0.52f;
    }
    P.lightAmt = (float)(shape(prog, 0) * S.light);
    setLight(P, V, l8, l10, 0.9f, P.lightAmt);
    const float nf = (float)n;
    bool active[64];
    double os[64][9];
    Poly strips[64];
    int fin = 0;
    for (int i = 0; i < n; ++i) {
        Poly& St = strips[i];
        unitQuad(St);
        const float a = (float)i / nf, b = (float)(i + 1) / nf;
        if (vflag) {
            St.v[0][1] = a; St.v[0][5] = a;
            St.v[1][1] = a; St.v[1][5] = a;
            St.v[2][1] = b; St.v[2][5] = b;
            St.v[3][1] = b; St.v[3][5] = b;
        } else {
            St.v[0][0] = a; St.v[0][4] = a;
            St.v[3][0] = a; St.v[3][4] = a;
            St.v[1][0] = b; St.v[1][4] = b;
            St.v[2][0] = b; St.v[2][4] = b;
        }
        if (flip)
            for (int k = 0; k < 4; ++k) {
                St.v[k][4] = 1.0f - St.v[k][4];
                St.v[k][5] = 1.0f - St.v[k][5];
            }
        const double t = (double)(n - 1 - i) / (double)n + prog;
        if (t >= 1.0) {
            ++fin;
            active[i] = false;
        } else {
            active[i] = true;
            poseAt(K, 3, t, os[i]);
        }
    }
    C.finished = fin;
    C.horizontalRun = !vflag;
    C.flip = flip;
    C.n = n;
    for (int i = 0; i < n; ++i) {
        if (!active[i]) continue;
        M4 m;
        identity(m);
        shift(-0.5f, -0.5f, 0.0f, m);
        scale(V.wa, V.hh, 1.0f, m);
        shift(0.5f, 0.5f, 0.0f, m);
        const double c = 0.5 / (double)n;
        const double c2 = (double)i / (double)n + c;
        if (!vflag) shift((float)(-c2), -0.5f, 0.0f, m);
        else shift(-0.5f, (float)(-c2), 0.0f, m);
        const double* o = os[i];
        rotate(1, 0, 0, (float)o[6], m);
        rotate(0, 1, 0, (float)o[7], m);
        rotate(0, 0, 1, (float)o[8], m);
        shift((float)o[0], (float)o[1], (float)(o[2] + 1.0), m);
        const double c3 = c2 - 0.5;
        if (!vflag) shift((float)c3, 0.0f, 0.0f, m);
        else shift(0.0f, (float)c3, 0.0f, m);
        if (flip) rotate(0, 0, 1, 3.14159274f, m);
        Item it;
        std::memset(&it, 0, sizeof it);
        it.poly = strips[i];
        for (int k = 0; k < 4; ++k) transform(it.poly.v[k], m);
        normalOf(it.poly, it.nrm);
        clipPoly(it.poly, V.pl, 5);
        if (!(it.poly.n > 2)) continue;
        project(it.poly, V);
        it.src = 1;
        P.items.push_back(it);
    }
    return C;
}

// ---------------------------------------------------------------- painting
// The pictures are 8-bit RGBA, w x h, rows w x 4 bytes apart, the first row at the top of the picture.
struct Frame {
    int w, h;
    double asp;                 // pixel aspect (1 = square)
    const uint8_t* a;           // the outgoing picture
    const uint8_t* b;           // the incoming picture
    uint8_t* out;
};

// Draws the plan's polygons over 'out' in order. The summed-area tables of the pictures are made when a polygon first needs them.
inline void paint(const Frame& f, const Plan& P)
{
    Lights L;
    L.pos[0] = P.lightX;
    L.pos[1] = P.lightY;
    L.pos[2] = P.lightZ;
    L.colour[0] = L.colour[1] = L.colour[2] = 1.0f;
    L.intensity = P.lightAmt;
    const int mode = (P.lightAmt > 0.0f) + 1;
    std::vector<float> sat[2];
    Scratch scratch;
    for (size_t i = 0; i < P.items.size(); ++i) {
        const Item& it = P.items[i];
        const int s = it.src ? 1 : 0;
        if (sat[s].empty()) {
            sat[s].resize((size_t)f.w * f.h * 4);
            detail::buildSat(sat[s].data(), s ? f.b : f.a, f.w, f.h, f.w * 4);
        }
        drawPolygon(scratch, f.out, f.w, f.h, f.w * 4, sat[s].data(), f.w, f.h, it.poly, it.nrm, L, mode, f.asp, P.fade);
    }
}

inline bool frameOk(const Frame& f) { return f.w >= 1 && f.h >= 1 && f.w <= 16384 && f.h <= 16384 && f.a && f.b && f.out; }

inline void shuffle(const Frame& f, double light, double prog)
{
    if (!frameOk(f)) return;
    std::memset(f.out, 0, (size_t)f.w * f.h * 4);
    paint(f, shufflePlan(f.w, f.h, f.asp, light, prog));
}
inline void blinds(const Frame& f, const BlindsSettings& S, double prog)
{
    if (!frameOk(f)) return;
    std::memset(f.out, 0, (size_t)f.w * f.h * 4);
    paint(f, blindsPlan(f.w, f.h, f.asp, S, prog));
}
inline void flyInOut(const Frame& f, const FlySettings& S, double prog)
{
    if (!frameOk(f)) return;
    std::memcpy(f.out, S.in == 0 ? f.b : f.a, (size_t)f.w * f.h * 4);
    paint(f, flyPlan(f.w, f.h, f.asp, S, prog));
}

// ---------------------------------------------------------------- the strips that have landed
// VEGAS copies the incoming picture over them with its generic rectangle blit (the rectangle in 14-bit fixed point of the picture's pixels, source and destination the same,
// no filter). The blit stretches the source rectangle onto the destination one with a bilinear read that maps the first destination pixel to the first source position and
// the last to the last (so with equal rectangles every pixel but the last row and column of a rectangle that does not end on a pixel is a plain copy); the pixels the
// rectangle covers in part (its first and last row and column when they do not start or end on a pixel) are mixed over what is there by their coverage.
namespace blit {

inline int fl14(int v) { return v >= 0 ? v >> 14 : -((-v + 16383) >> 14); }       // floor(v / 16384)
inline int ce14(int v) { return -fl14(-v); }                                        // ceil(v / 16384)
inline int dtoi(double v) { return (v > -2147483648.0 && v < 2147483648.0) ? (int)v : INT_MIN; }
struct Rect {
    int x0, y0, x1, y1;
};
inline Rect isect(const Rect& a, const Rect& b)
{
    const Rect r = {std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    if (r.x0 >= r.x1 || r.y0 >= r.y1) return Rect{0, 0, 0, 0};
    return r;
}
// p + (q - p) x t / 255, VEGAS's whole-number way
inline int mix8(int p, int q, int t)
{
    const int m = (q - p) * t + 128;
    const int d = p + ((m + (m >> 8)) >> 8);
    return d < 0 ? 0 : (d > 255 ? 255 : d);
}
inline void mixInto(uint8_t* d, const uint8_t* p, int alpha)
{
    for (int k = 0; k < 4; ++k) d[k] = (uint8_t)mix8(d[k], p[k], alpha & 255);
}
// 14-bit bilinear of the four pixels
inline void bil(const uint8_t* r0, const uint8_t* r1, int i0, int i1, int fx, int fy, uint8_t* out)
{
    const unsigned w00 = (unsigned)((0x4000 - fy) * (0x4000 - fx)) >> 4, w10 = (unsigned)((0x4000 - fx) * fy) >> 4;
    const unsigned w01 = (unsigned)((0x4000 - fy) * fx) >> 4, w11 = (unsigned)(fx * fy) >> 4;
    for (int k = 0; k < 4; ++k)
        out[k] = (uint8_t)((r0[i0 * 4 + k] * w00 + r1[i0 * 4 + k] * w10 + r0[i1 * 4 + k] * w01 + r1[i1 * 4 + k] * w11 + 0x800000u) >> 24);
}
// the same with the SSE2 code's rounding (16-bit weights from 0x3fff, products kept to 16 bits, +4, >> 3)
inline void bilSse(const uint8_t* r0, const uint8_t* r1, int i0, int i1, int fx, int fy, uint8_t* out)
{
    const unsigned A = (unsigned)((0x3fff - fy) << 2) & 0xffff, F = (unsigned)(fy << 2) & 0xffff, a = (unsigned)(0x3fff - fx);
    const int w00 = (int)((a * A) >> 16), w01 = (int)(((unsigned)fx * A) >> 16), w10 = (int)((a * F) >> 16), w11 = (int)(((unsigned)fx * F) >> 16);
    for (int k = 0; k < 4; ++k) {
        auto mh = [](int v, int w) { return ((v << 5) * w) >> 16; };
        int s = std::min(mh(r1[i0 * 4 + k], w10) + 4, 65535);
        s = std::min(s + mh(r1[i1 * 4 + k], w11), 65535);
        const int t = std::min(mh(r0[i1 * 4 + k], w01) + mh(r0[i0 * 4 + k], w00), 65535);
        s = std::min(s + t, 65535);
        out[k] = (uint8_t)std::min(s >> 3, 255);
    }
}

// the rectangle R (left, top, right, bottom) of 'src' onto the same rectangle of 'dst' (both w x h RGBA, rows w x 4 bytes)
inline void copyRect(uint8_t* dst, const uint8_t* src, int w, int h, const Rect& R)
{
    const double k14 = 6.103515625e-05;
    const Rect D = R;
    const Rect S1 = isect(R, Rect{0, 0, w << 14, h << 14});
    if (S1.x1 - S1.x0 < 0x4000 || S1.y1 - S1.y0 < 0x4000) return;
    if (D.x1 - D.x0 < 0x400 || D.y1 - D.y0 < 0x400) return;
    const int scx = dtoi((S1.x1 - S1.x0) * 16384.0 / (D.x1 - D.x0)), scy = dtoi((S1.y1 - S1.y0) * 16384.0 / (D.y1 - D.y0));
    if (scx <= 0 || scy <= 0) return;
    const Rect D1 = isect(D, Rect{0, 0, w << 14, h << 14});
    if (D1.x1 - D1.x0 < 0x400 || D1.y1 - D1.y0 < 0x400) return;
    const Rect M = {D.x0, D.y0, dtoi((S1.x1 - S1.x0) * 16384.0 / scx) + D.x0, dtoi((S1.y1 - S1.y0) * 16384.0 / scy) + D.y0};
    const Rect Fr = isect(M, D1);
    const Rect sr = {dtoi((Fr.x0 - D.x0) * (double)scx * k14) + S1.x0, dtoi((Fr.y0 - D.y0) * (double)scy * k14) + S1.y0,
                     dtoi((Fr.x1 - D.x0) * (double)scx * k14) + S1.x0, dtoi((Fr.y1 - D.y0) * (double)scy * k14) + S1.y0};
    const Rect S = isect(sr, S1);
    const int sx = D1.x1 - D1.x0 < 0x8000 ? 0 : std::max(0, dtoi((S.x1 - S.x0 - 0x4000) * 16384.0 / (D1.x1 - D1.x0 - 0x4000)));
    const int sy = D1.y1 - D1.y0 < 0x8000 ? 0 : std::max(0, dtoi((S.y1 - S.y0 - 0x4000) * 16384.0 / (D1.y1 - D1.y0 - 0x4000)));
    const int px0 = fl14(D1.x0), px1 = ce14(D1.x1), py0 = fl14(D1.y0), py1 = ce14(D1.y1);
    int frc = ((py0 + 1) << 14) - D1.y0;
    const int lrc = D1.y1 - (py1 << 14) + 0x4000;
    int fcc = ((px0 + 1) << 14) - D1.x0;
    const int lcc = D1.x1 - (px1 << 14) + 0x4000;
    const int scol = ce14(S.x1), srow = ce14(S.y1);
    if (px1 - px0 < 2) fcc = D1.x1 - D1.x0;
    const int nrows = py1 - py0;
    if (nrows < 2) frc = D1.y1 - D1.y0;
    const int last = scol - 1;
    if (S.x0 < 0 || S.y0 < 0 || scol > w || srow > h || px0 < 0 || px1 > w || py0 < 0 || py1 > h) return;
    auto dpx = [&](int x, int y) { return dst + ((size_t)y * w + x) * 4; };
    auto spx = [&](int x, int y) { return src + ((size_t)y * w + x) * 4; };
    auto srcRows = [&](int y, const uint8_t*& r0, const uint8_t*& r1, int& fy) {
        if (y < 0) y = 0;
        const int r = y / 16384;
        fy = y % 16384;
        r0 = spx(0, r);
        r1 = spx(0, r + (r != srow - 1 ? 1 : 0));
    };
    auto colOf = [&](int x, int& i0, int& i1, int& fx) {
        i0 = x / 16384;
        fx = x % 16384;
        i1 = i0 + (i0 != last ? 1 : 0);
    };
    // the first and last rows: every pixel mixed in by its coverage
    auto edgeRow = [&](int drow, int y, int rcov) {
        const uint8_t *r0, *r1;
        int fy;
        srcRows(y, r0, r1, fy);
        int x = S.x0;
        int alpha = (dtoi(rcov * (double)fcc * k14) * 255) >> 14;
        uint8_t P[4];
        int i0, i1, fx;
        for (int c = px0; c < std::max(px1 - 1, px0 + 1); ++c) {
            colOf(x, i0, i1, fx);
            bil(r0, r1, i0, i1, fx, fy, P);
            mixInto(dpx(c, drow), P, alpha);
            if (c == px0) {
                x += dtoi(sx * (double)fcc * k14);
                alpha = (rcov * 255) >> 14;
            } else {
                x += sx;
            }
        }
        const int n = px1 - px0;
        if (n <= 1) return;
        if (n > 2) x += -sx - dtoi(lcc * (double)sx * -k14);
        colOf(x, i0, i1, fx);
        bil(r0, r1, i0, i1, fx, fy, P);
        mixInto(dpx(px1 - 1, drow), P, (dtoi(lcc * (double)rcov * k14) * 255) >> 14);
    };
    // the rows between
    auto midRow = [&](int drow, int y) {
        const uint8_t *r0, *r1;
        int fy;
        srcRows(y, r0, r1, fy);
        int x = S.x0;
        uint8_t P[4];
        int i0, i1, fx;
        colOf(x, i0, i1, fx);
        bil(r0, r1, i0, i1, fx, fy, P);
        mixInto(dpx(px0, drow), P, (fcc * 255) >> 14);
        x += dtoi(fcc * (double)sx * k14);
        const int n = px1 - px0 - 2;
        int c = px0 + 1;
        if (n > 0) {
            for (int k = 0; k < (n & ~1); ++k, ++c, x += sx) {
                const int j0 = x >> 14, f = x & 0x3fff;
                bilSse(r0, r1, j0, j0 + (j0 != last ? 1 : 0), f, fy, dpx(c, drow));
            }
            if (n & 1) {
                colOf(x, i0, i1, fx);
                bil(r0, r1, i0, i1, fx, fy, dpx(c, drow));
                x += sx;
                ++c;
            }
        }
        if (px1 - px0 <= 1) return;
        if (px1 - px0 > 2) x += -sx - dtoi(lcc * (double)sx * -k14);
        i0 = x / 16384;
        fx = x % 16384;
        if (i0 == last)
            for (int k = 0; k < 4; ++k) P[k] = (uint8_t)((r0[i0 * 4 + k] * (0x4000 - fy) + r1[i0 * 4 + k] * fy + 0x2000) >> 14);
        else
            bil(r0, r1, i0, i0 + 1, fx, fy, P);
        mixInto(dpx(px1 - 1, drow), P, (lcc * 255) >> 14);
    };
    edgeRow(py0, S.y0, frc);
    const int fs = dtoi(frc * (double)sy * k14);
    for (int k = 0; k < py1 - 1 - (py0 + 1); ++k) midRow(py0 + 1 + k, k * sy + fs + S.y0);
    if (nrows > 1) {
        int y = (nrows - 2) * sy + fs + S.y0;
        if (nrows > 2) y += -sy - dtoi(lrc * (double)sy * -k14);
        edgeRow(py1 - 1, y, lrc);
    }
}

}  // namespace blit

// The rectangle of the strips that have landed, 14-bit fixed point (left, top, right, bottom): the first strips of the run from one side, the rest of the picture untouched.
inline bool landedRect(const Frame& f, const CascadePlan& C, int& left, int& top, int& right, int& bottom)
{
    if (C.finished <= 0) return false;
    left = 0;
    top = 0;
    if (C.horizontalRun) {
        // strips side by side: the landed ones are the first from the left (from the right when flipped)
        const int W = f.w << 14;
        right = (W / C.n) * C.finished;
        bottom = f.h << 14;
        if (C.flip) {
            left = W - right;
            right = W;
        }
    } else {
        const int H = f.h << 14;
        right = f.w << 14;
        bottom = (H / C.n) * C.finished;
        if (C.flip) {
            top = H - bottom;
            bottom = H;
        }
    }
    return true;
}

inline void cascade(const Frame& f, const CascadeSettings& S, double prog)
{
    if (!frameOk(f)) return;
    std::memcpy(f.out, f.a, (size_t)f.w * f.h * 4);
    const CascadePlan C = cascadePlan(f.w, f.h, f.asp, S, prog);
    int l, t, r, b;
    if (landedRect(f, C, l, t, r, b)) blit::copyRect(f.out, f.b, f.w, f.h, blit::Rect{l, t, r, b});
    paint(f, C.plan);
}

}  // namespace fx
}  // namespace vfx3d
}  // namespace ofx
}  // namespace reno
