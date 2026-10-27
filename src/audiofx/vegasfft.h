// The FFTs of VEGAS's sfppack2 effects (Graphic EQ, Paragraphic EQ, ...), worked out from the DLL's own code so that
// Reno's versions give the same bits: a decimation-in-time complex transform (bit reversal, a first stage of blocks of
// four, radix-4 stages, an optional radix-2 stage, and a last radix-2 stage that applies the scale) whose twiddle
// factors come from a recurrence with a table of (cos(pi/2^k)-1, sin(pi/2^k)) steps, and a real transform made from
// the complex one by a pass over the pairs (k, n-k). There is a single precision version (the sound's path) and a
// double precision one (the filters' design). The recurrences and the order of the operations are the plug-in's, so
// the code is not tidied: a rounding that is not where the plug-in has it would change the output bits.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace reno {
namespace vegfft {

template <class T>
struct C {
    T re, im;
};

// (cos(pi/2^k)-1, sin(pi/2^k)) as float: -2 sin^2(pi/2^(k+1)) and sin(pi/2^k), k = 0..23
inline const float* ftab()
{
    struct Table {
        float t[2 * 24];
        Table()
        {
            for (int k = 0; k < 24; ++k) {
                const double th = 3.14159265358979323846 / std::ldexp(1.0, k);
                const double h = std::sin(th / 2);
                t[2 * k] = (float)(-2.0 * h * h);
                t[2 * k + 1] = k == 0 ? 0.0f : (float)std::sin(th);
            }
        }
    };
    static const Table table;
    return table.t;
}

// The double precision table is typed in with 14 digits in the plug-in (and slightly off the exact values: they are
// what its transform uses).
inline const double (*dtab())[2]
{
    static const double t[24][2] = {{-2.0, 0.0},
                                    {-1.0000000000001, 1.0},
                                    {-0.29289321881349, 0.70710678118658},
                                    {-0.076120467488723, 0.38268343236511},
                                    {-0.019214719596772, 0.19509032201614},
                                    {-0.0048152733278037, 0.098017140329567},
                                    {-0.0012045437948278, 0.049067674327421},
                                    {-0.00030118130379582, 0.024541228522914},
                                    {-7.5298160855469e-05, 0.012271538285721},
                                    {-1.882471739886e-05, 0.0061358846491549},
                                    {-4.7061904238291e-06, 0.0030679567629662},
                                    {-1.1765482980902e-06, 0.0015339801862849},
                                    {-2.9413711778088e-07, 0.00076699031874276},
                                    {-7.3534282148865e-08, 0.00038349518757142},
                                    {-1.8383570706194e-08, 0.00019174759731072},
                                    {-4.5958926871096e-09, 9.5873799095984e-05},
                                    {-1.1489731724375e-09, 4.793689960307e-05},
                                    {-2.8724329315062e-10, 2.396844980842e-05},
                                    {-7.1810823290234e-11, 1.198422490507e-05},
                                    {-1.795270582272e-11, 5.9921124526428e-06},
                                    {-4.48817645569e-12, 2.9960562263349e-06},
                                    {-1.1220441139231e-12, 1.4980281131691e-06},
                                    {-2.8051102848082e-13, 7.4901405658477e-07},
                                    {-7.0127757120208e-14, 3.7450702829241e-07}};
    return t;
}

template <class T>
inline void bitrev(C<T>* d, int n)
{
    int j = 0;
    for (int i = 0; i < n; ++i) {
        if (j > i) std::swap(d[i], d[j]);
        int k = n / 2;
        while (k >= 1 && j >= k) {
            j -= k;
            k /= 2;
        }
        j += k;
    }
}

// one step of the twiddle recurrence of the radix-4 and radix-2 stages: w = w * (1 + (c, s))
template <class T>
inline void rstep(T& re, T& im, T c, T s)
{
    const T nr = (re * c + re) - im * s;
    const T ni = (im * c + im) + re * s;
    re = nr;
    im = ni;
}

// the first two levels: butterflies on blocks of four
template <class T>
inline void stageFirst(C<T>* d, int n, T a2, T a3)
{
    const T x7 = a2 + T(1), x8 = a3;
    for (int g = 0; g < n; g += 4) {
        C<T>* p = d + g;
        const T e2r = p[2].re, e0r = p[0].re, e3r = p[3].re, e2i = p[2].im;
        const T d1 = e2r - e3r;
        const T s23 = e3r + e2r;
        T xr = d1 * x7;
        const T d2 = e2i - p[3].im;
        T xi = d2 * x7;
        xi = xi + d1 * x8;
        const T e1r = p[1].re;
        const T dd = e0r - e1r;
        const T ss = e0r + e1r;
        xr = xr - d2 * x8;
        p[0].re = s23 + ss;
        p[2].re = ss - s23;
        p[3].re = dd - xr;
        p[1].re = dd + xr;
        const T e0i = p[0].im, e1i = p[1].im, e3i = p[3].im, e2i2 = p[2].im;
        const T sa = e0i + e1i;
        const T da = e0i - e1i;
        const T sb = e3i + e2i2;
        p[0].im = sb + sa;
        p[2].im = sa - sb;
        p[3].im = da - xi;
        p[1].im = da + xi;
    }
}

// radix-4 stage: blocks of m become blocks of 4m. (ca, sa) and (cb, sb) are the recurrence steps of the two twiddle
// sets (angles pi/(2m) and pi/(4m)); fwd selects the forward transform (the inverse differs in the signs of the
// combinations)
template <class T>
inline void stageR4(C<T>* d, int n, int m, T ca, T sa, T cb, T sb, bool fwd)
{
    T Ar = T(1), Ai = T(0), Br = T(1), Bi = T(0);
    for (int j = 0; j < m; j += 2) {
        T A1r = Ar, A1i = Ai, B1r = Br, B1i = Bi;
        rstep(A1r, A1i, ca, sa);
        rstep(B1r, B1i, cb, sb);
        for (int b = j; b < n; b += 4 * m) {
            C<T>* e0 = d + b;
            C<T>* e1 = d + b + m;
            C<T>* e2 = d + b + 2 * m;
            C<T>* e3 = d + b + 3 * m;
            {   // column j
                const T x1 = e1[0].re, y1 = e1[0].im;
                const T t3 = e0[0].im, t8 = e0[0].re;
                const T t9 = e2[0].re, t10 = e2[0].im;
                const T t11 = x1 * Ar - y1 * Ai;
                const T t12 = y1 * Ar + x1 * Ai;
                const T t14 = t8 + t11, t15 = t8 - t11, t16 = t3 + t12, t19 = t3 - t12;
                const T t13 = e3[0].re, t20 = e3[0].im;
                const T t23 = t20 * Ar + t13 * Ai;
                const T t24 = t13 * Ar - t20 * Ai;
                const T t25 = t10 + t23, t26 = t10 - t23, t29 = t9 + t24, t30 = t9 - t24;
                const T t33 = t29 * Bi + t25 * Br;
                const T t34 = t29 * Br - t25 * Bi;
                e0[0].im = t16 + t33;
                e0[0].re = t14 + t34;
                e2[0].im = t16 - t33;
                e2[0].re = t14 - t34;
                if (fwd) {
                    const T t44 = t30 * Br - t26 * Bi;
                    const T t48 = (-(t30 * Bi)) - t26 * Br;
                    e3[0].im = t19 - t44;
                    e1[0].im = t19 + t44;
                    e3[0].re = t15 - t48;
                    e1[0].re = t15 + t48;
                } else {
                    const T u1 = t30 * Bi + t26 * Br;
                    const T u2 = t26 * Bi - t30 * Br;
                    e3[0].re = t15 - u1;
                    e1[0].re = t15 + u1;
                    e3[0].im = t19 - u2;
                    e1[0].im = t19 + u2;
                }
            }
            {   // column j+1
                const T x1 = e1[1].re, y1 = e1[1].im;
                const T t57 = e0[1].re, t56 = e0[1].im;
                const T t58 = e2[1].re, t61 = e2[1].im;
                const T t63 = e3[1].re, t70 = e3[1].im;
                const T t55 = x1 * A1r - y1 * A1i;
                const T t62 = fwd ? y1 * A1r + x1 * A1i : x1 * A1i + y1 * A1r;
                const T t73 = fwd ? t70 * A1r + t63 * A1i : t63 * A1i + t70 * A1r;
                const T t74 = t63 * A1r - t70 * A1i;
                const T t64 = t57 + t55, t65 = t57 - t55, t66 = t56 + t62, t69 = t56 - t62;
                const T t75 = t61 + t73, t76 = t61 - t73, t79 = t58 + t74, t80 = t58 - t74;
                const T t84 = t79 * B1r - t75 * B1i;
                const T t85 = t79 * B1i + t75 * B1r;
                e0[1].re = t64 + t84;
                e2[1].re = t64 - t84;
                e0[1].im = t66 + t85;
                e2[1].im = t66 - t85;
                if (fwd) {
                    const T t95 = t80 * B1r - t76 * B1i;
                    const T t97 = (-(t80 * B1i)) - t76 * B1r;
                    e3[1].re = t65 - t97;
                    e1[1].re = t65 + t97;
                    e3[1].im = t69 - t95;
                    e1[1].im = t69 + t95;
                } else {
                    const T t94 = t76 * B1i - t80 * B1r;
                    const T t95 = t80 * B1i + t76 * B1r;
                    e3[1].re = t65 - t95;
                    e1[1].re = t65 + t95;
                    e3[1].im = t69 - t94;
                    e1[1].im = t69 + t94;
                }
            }
        }
        Ar = A1r;
        Ai = A1i;
        Br = B1r;
        Bi = B1i;
        rstep(Ar, Ai, ca, sa);
        rstep(Br, Bi, cb, sb);
    }
}

// radix-2 stage with the twiddle recurrence: blocks of m become blocks of 2m
template <class T>
inline void stageR2(C<T>* d, int n, int m, T c, T s)
{
    T Wr = T(1), Wi = T(0);
    for (int j = 0; j < m; j += 2) {
        const T W1r = (Wr * c + Wr) - Wi * s;
        const T W1i = (Wi * c + Wi) + Wr * s;
        for (int b = j; b < n; b += 2 * m) {
            C<T>* a = d + b;
            C<T>* q = d + b + m;
            {
                const T x = q[0].re, y = q[0].im;
                const T tr = x * Wr - y * Wi;
                const T ti = x * Wi + y * Wr;
                const T ar = a[0].re, ai = a[0].im;
                q[0].re = ar - tr;
                q[0].im = ai - ti;
                a[0].im = ti + ai;
                a[0].re = tr + ar;
            }
            {
                const T x = q[1].re, y = q[1].im;
                const T tr = x * W1r - y * W1i;
                const T ti = x * W1i + y * W1r;
                const T ar = a[1].re, ai = a[1].im;
                q[1].re = ar - tr;
                q[1].im = ai - ti;
                a[1].re = tr + ar;
                a[1].im = ti + ai;
            }
        }
        const T nr = (W1r * c + W1r) - W1i * s;
        const T ni = (W1i * c + W1i) + W1r * s;
        Wr = nr;
        Wi = ni;
    }
}

// the last radix-2 stage; it multiplies all outputs by scale
template <class T>
inline void stageLast(C<T>* d, int n, T c, T s, T scale)
{
    const int h = n / 2;
    T Wr = T(1), Wi = T(0);
    for (int k = 0; k < h; ++k) {
        C<T>* a = d + k;
        C<T>* b = d + k + h;
        const T x = b->re, y = b->im;
        const T tr = x * Wr - y * Wi;
        const T ti = x * Wi + y * Wr;
        b->re = (a->re - tr) * scale;
        b->im = (a->im - ti) * scale;
        a->re = (tr + a->re) * scale;
        a->im = (ti + a->im) * scale;
        const T dr = Wr * c - Wi * s;
        const T di = Wi * c + Wr * s;
        Wr = Wr + dr;
        Wi = Wi + di;
    }
}

// Single precision complex transform of n points (n >= 16, a power of two). Forward: all outputs are multiplied by
// scale / (2 n); inverse: by scale * 2.
inline void fftcF(C<float>* d, int n, bool fwd, float scale)
{
    const float* T = ftab();
    bitrev(d, n);
    stageFirst<float>(d, n, -1.0f, fwd ? 1.0f : -1.0f);
    int m = 4, e = 2;
    if (n > 16) {
        for (;;) {
            const float ca = T[2 * e], sa = T[2 * e + 1], cb = T[2 * (e + 1)], sb = T[2 * (e + 1) + 1];
            stageR4<float>(d, n, m, ca, fwd ? sa : -sa, cb, fwd ? sb : -sb, fwd);
            m *= 4;
            e += 2;
            if (!(4 * m < n)) break;
        }
    }
    if (4 * m == n) {
        stageR2<float>(d, n, m, T[2 * e], fwd ? T[2 * e + 1] : -T[2 * e + 1]);
        ++e;
    }
    stageLast<float>(d, n, T[2 * e], fwd ? T[2 * e + 1] : -T[2 * e + 1], fwd ? scale / (float)(2 * n) : scale * 2.0f);
}

// Single precision real transform: n complex points are 2n reals in data. Forward: data[0] is the DC value and data[1]
// the Nyquist one, then the (re, im) of bins 1 .. n-1; inverse: the other way. scale is as for fftcF.
inline void rfftF(float* data, int n, bool fwd, float scale)
{
    C<float>* c = reinterpret_cast<C<float>*>(data);
    float theta = 3.1415927410125732f / (float)n;
    float r0, i0;
    float c11;
    if (fwd) {
        fftcF(c, n, true, scale);
        r0 = data[0];
        i0 = data[1];
        c11 = -0.5f;
    } else {
        r0 = data[1];
        i0 = 0.0f;
        data[1] = 0.0f;
        theta = -theta;
        c11 = 0.5f;
    }
    const float h = (float)std::sin((double)theta * 0.5);
    const float wpr = (h * -2.0f) * h;
    const float wpi = (float)std::sin((double)theta);
    const float t9 = data[1], t10 = data[0];
    const float a12 = t9 + i0, a13 = t10 + r0, a14 = t10 - r0;
    float Wr = wpr + 1.0f, Wi = wpi;
    const float a15 = a12 * c11, a16 = a13 * 0.5f, a17 = a14 * c11;
    const float a20 = t9 - i0;
    const float a21 = -a15;
    const float a22 = a20 * 0.5f;
    const float a23 = a22 + a17;
    const float a24 = a21 + a16;
    const float nyq = a16 - a21;
    data[1] = a23;
    data[0] = a24;
    const int half = n / 2;
    for (int k = 1; k <= half; ++k) {
        float* p = data + 2 * k;
        float* q = data + 2 * (n - k);
        const float t1 = p[0], t2 = p[1], t3 = q[1], t4 = q[0];
        const float t5 = t2 - t3, t6 = t3 + t2;
        const float t7 = t6 * c11, t8 = t5 * 0.5f;
        const float t9b = t4 + t1, t10b = t1 - t4;
        const float t11 = -t7;
        const float t12 = t10b * c11, t13 = t9b * 0.5f;
        const float t14 = t11 * Wr, t15 = t12 * Wi, t16 = t12 * Wr;
        const float t17 = t14 + t13, t18 = t13 - t14;
        const float t19 = t11 * Wi;
        const float t21 = t17 - t15, t22 = t18 + t15;
        const float t24 = t16 + t8, t25 = t16 - t8;
        const float t26 = t24 + t19, t27 = t25 + t19;
        p[0] = t21;
        p[1] = t26;
        q[0] = t22;
        q[1] = t27;
        const float dr = wpr * Wr - Wi * wpi;
        const float di = Wr * wpi + Wi * wpr;
        Wr = Wr + dr;
        Wi = Wi + di;
    }
    if (fwd) data[1] = nyq;
    else fftcF(c, n, false, scale);
}

// Double precision forward complex transform of nc points (scaled by scale / (2 nc)).
inline void fftcD(C<double>* d, int nc, double scale)
{
    const double (*T)[2] = dtab();
    bitrev(d, nc);
    stageFirst<double>(d, nc, T[1][0], T[1][1]);
    int mc = 4, e = 2;
    while (2 * mc < nc) {
        if (4 * mc == nc) {
            stageR2<double>(d, nc, mc, T[e][0], T[e][1]);
            ++e;
            break;
        }
        stageR4<double>(d, nc, mc, T[e][0], T[e][1], T[e + 1][0], T[e + 1][1], true);
        mc *= 4;
        e += 2;
    }
    stageLast<double>(d, nc, T[e][0], T[e][1], scale / (double)(2 * nc));
}

// Double precision forward real transform of N doubles (N >= 32, a power of two), packed as rfftF's forward output.
inline void rfftDfwd(double* data, int N, double scale)
{
    const int nc = N / 2;
    C<double>* c = reinterpret_cast<C<double>*>(data);
    fftcD(c, nc, scale);
    const double re0 = data[0], im0 = data[1];
    const double theta = 3.141592653589793 / (double)nc;
    const double s1 = std::sin(theta * 0.5);
    const double wpr = (s1 * -2.0) * s1;
    const double wpi = std::sin(theta);
    double Wr = 1.0, Wi = 0.0;
    double nyq;
    {   // bin 0 and the Nyquist bin, with the twiddle written out as the plug-in does for its first pass
        const double t3 = im0 + im0, t4 = re0 + re0;
        const double t5 = t3 * -0.5, t6 = t4 * 0.5;
        const double t7 = im0 - im0;
        const double t9 = -t5;
        const double t10 = t7 * 0.5;
        const double t11 = re0 - re0;
        const double t12 = t9 * Wi;
        const double t13 = t11 * -0.5;
        const double t14 = t9 * Wr;
        const double t15 = t6 - t14;
        const double t16 = t13 * Wr;
        const double t17 = t13 * Wi;
        const double t18 = t14 + t6;
        nyq = t15 + t17;
        data[0] = t18 - t17;
        const double t22 = t16 + t10;
        data[1] = t22 + t12;
    }
    {
        const double dr = wpr * Wr - wpi * Wi;
        const double di = wpi * Wr + wpr * Wi;
        Wr = Wr + dr;
        Wi = Wi + di;
    }
    for (int k = 1; k <= N / 4; ++k) {
        double* p = data + 2 * k;
        double* q = data + N - 2 * k;
        const double t1 = p[1], t2 = p[0], t3 = q[0];
        const double t4 = t3 + t2, t6 = t2 - q[0];
        const double t7 = t4 * 0.5, t8 = t6 * -0.5;
        const double t10 = t1 + q[1], t12 = t1 - q[1];
        const double t13 = t8 * Wr, t14 = t8 * Wi;
        const double t15 = t10 * -0.5, t16 = t12 * 0.5;
        const double t17 = -t15;
        const double t18 = t17 * Wi, t19 = t17 * Wr;
        const double t20 = t19 + t7, t21 = t7 - t19;
        const double t22 = t20 - t14, t23 = t21 + t14;
        const double t24 = t13 - t16, t25 = t13 + t16;
        const double t26 = t24 + t18, t27 = t25 + t18;
        p[0] = t22;
        p[1] = t27;
        q[0] = t23;
        q[1] = t26;
        const double dr = wpr * Wr - wpi * Wi;
        const double di = wpi * Wr + wpr * Wi;
        Wr = Wr + dr;
        Wi = Wi + di;
    }
    data[1] = nyq;
}

}  // namespace vegfft
}  // namespace reno
