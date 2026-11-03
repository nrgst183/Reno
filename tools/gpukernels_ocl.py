"""Kernels of the colour effects that are VEGAS's OpenCL kernels, ported on the CPU in src/ofx/vfxcolour.h, vfxhslwb.h, vfxlevelsconv.h ... : float32 arithmetic on the
premultiplied pixel, one pixel at a time, nothing cut to whole levels between the steps; an 8-bit frame is read as level / 255 (vcol::readLevels) and written through the
12-bit store (more::oclStore12). Every kernel here mirrors the C++ function named in its comment operation for operation (no fused multiply-adds: spvdsl decorates
the float adds and multiplies NoContraction), so the result is bit-identical to the CPU; sin, cos, exp, pow, asin and cbrt are the software double rounded to a float."""
import numpy as np
from gpukernels import kernel, PIC_IN, PIC_OUT, TABLE, WH
from spvdsl import compile_kernel
from sf64 import SF, sf_pow, sf_atan2

def F(x):
    """a float32 constant as the C++ compiler folds it (a Python float holding the float's value)"""
    return float(np.float32(x))

def f32(x):
    return np.float32(x)

def sel(S, c, a, b):
    return S.select(c, a, b)

def br(S, cond, fa, fb, ty='f'):
    """cond ? fa() : fb() with only the taken side run"""
    v = S.var(ty)
    with S.if_(cond):
        v.set(fa())
    with S.else_():
        v.set(fb())
    return v.get()

def clamp01f(S, v):
    """vcol::clamp01: std::min(std::max(v, 0), 1)"""
    return S.min(S.max(v, 0.0), 1.0)

def mixf(S, x, y, a):
    return x + (y - x) * a

def read_level(S, v):
    """vcol::readLevels: floor(v * 255 + 0.5) / 255"""
    return S.floor(v * 255.0 + 0.5) / 255.0

def store12(S, v):
    """more::oclStore12: the value cut to 12 bits and scaled to a byte with the half down, as level / 255 (not a number: 0)"""
    c = S.min(S.max(v, 0.0), 1.0)
    n = S.to_i(S.floor(c * 4096.0))
    lv = (n * 255 + 2047) >> 12
    return S.select(S.isnan(v), S.f(0.0), S.to_f(lv) / 255.0)

# std::fma on floats as the correctly rounded a * b + c, whatever the driver's own Fma does (SwiftShader's is not fused): the product of two floats is exact in a double, so the sum in the
# software double and its rounding to a float give the fused result (but for a double rounding on a tie, one in ~2^29)
def fma_exact(S, a, b, c):
    return (SF.from_f32(S, a) * SF.from_f32(S, b) + SF.from_f32(S, c)).to_f32()

OCL = WH + [('q', 'u'), ('pad', 'u')]

def ocl_pixels(S, body, read=True):
    """oclPixels(): body(S, [r, g, b, a]) -> [r, g, b, a] on the pixel read as levels / 255 when q (an 8-bit host frame; not for the effects that use the picture's floats as they are),
    stored through the 12-bit step when q"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        q = S.ne(P.q, 0)
        px = [S.select(q, read_level(S, p[k]), p[k]) for k in range(4)] if read else [p[k] for k in range(4)]
        o = body(S, px)
        S.buf('out').store(i, S.vec4(*[S.select(q, store12(S, o[k]), o[k]) for k in range(4)]))

def zero4(S):
    z = S.f(0.0)
    return [z, z, z, z]

# ---------------------------------------------------------------------------------------------------------------------------------- Invert (vcol::invertPx)
@kernel("o_invert", OCL + [('t', 'f')], [PIC_IN, PIC_OUT])
def o_invert(S):
    def body(S, p):
        w = p[3]
        out = []
        for k in range(3):
            st = sel(S, w < 0.000001, S.f(0.0), p[k] / w)
            v = (1.0 - st) * w
            out.append(mixf(S, p[k], v, S.p.t))
        return out + [w]
    ocl_pixels(S, body)

# ---------------------------------------------------------------------------------------------------------------------------------- Black and White (vcol::blackWhitePx)
@kernel("o_bw", OCL + [('k', 'f')], [PIC_IN, PIC_OUT])
def o_bw(S):
    def body(S, p):
        k = S.p.k
        on = S.abs(1.0 - k) > 0.000001
        g = (p[0] + p[1] + p[2]) / 3.0
        return [sel(S, on, mixf(S, g, p[i], k), p[i]) for i in range(3)] + [p[3]]
    ocl_pixels(S, body)

# ---------------------------------------------------------------------------------------------------------------------------------- Sepia (vcol::sepiaPx)
# mode: 0, 1, 2 = the three ranges of the strength (worked out by the plan with the CPU's own comparisons)
@kernel("o_sepia", OCL + [('mode', 'u'), ('c0', 'f'), ('c1', 'f'), ('c2', 'f'), ('v10', 'f'), ('v11', 'f'), ('v13', 'f')], [PIC_IN, PIC_OUT])
def o_sepia(S):
    def body(S, p):
        P = S.p
        a = p[3]
        straight = a < 1.0
        c = [sel(S, straight, p[k] / a, p[k]) for k in range(3)]
        col = [P.c0, P.c1, P.c2]
        g = (c[0] + c[1] + c[2]) / 3.0
        # mode 0
        r0 = [mixf(S, c[k], g, P.v11) for k in range(3)]
        # mode 1
        g1 = clamp01f(S, g)
        v14 = clamp01f(S, (g1 * P.v10) + P.v13)
        r1 = [mixf(S, r0[k], col[k], v14) for k in range(3)]
        # mode 2
        v14b = v14
        r2 = [mixf(S, g, col[k], v14b) for k in range(3)]
        r = [sel(S, S.eq(P.mode, 0), r0[k], sel(S, S.eq(P.mode, 1), r1[k], r2[k])) for k in range(3)]
        r = [sel(S, straight, r[k] * a, r[k]) for k in range(3)]
        o3 = sel(S, straight, a, S.f(1.0))
        ok = a > 0.000001
        z = S.f(0.0)
        return [sel(S, ok, r[0], z), sel(S, ok, r[1], z), sel(S, ok, r[2], z), sel(S, ok, o3, z)]
    ocl_pixels(S, body)

# ---------------------------------------------------------------------------------------------------------------------------------- Brightness and Contrast (vcol::brightContrastPx)
@kernel("o_bc", OCL + [('bright', 'f'), ('contrast', 'f'), ('centre', 'f'), ('f', 'f'), ('brightOn', 'u'), ('pad2', 'u')], [PIC_IN, PIC_OUT])
def o_bc(S):
    def body(S, p):
        P = S.p
        z = S.f(0.0)
        c = [p[0], p[1], p[2], p[3]]
        on = S.ne(P.brightOn, 0)
        a = c[3]
        big = a > 0.000001
        part = a < 1.0
        # the brightness step
        b = []
        for k in range(3):
            v_part = (c[k] / a + P.bright) * a
            v_full = c[k] + P.bright
            b.append(sel(S, on, sel(S, big, sel(S, part, v_part, v_full), z), c[k]))
        b3 = sel(S, on, sel(S, big, sel(S, part, a, S.f(1.0)), z), a)
        # the contrast step
        a2 = b3
        big2 = a2 > 0.000001
        part2 = a2 < 1.0
        out = []
        for k in range(3):
            v_part = ((b[k] / a2 - P.centre) * P.f + P.centre) * a2
            v_full = (b[k] - P.centre) * P.f + P.centre
            out.append(sel(S, big2, sel(S, part2, v_part, v_full), z))
        o3 = sel(S, big2, sel(S, part2, a2, S.f(1.0)), z)
        return out + [o3]
    ocl_pixels(S, body)

# ---------------------------------------------------------------------------------------------------------------------------------- Color Balance (vcol::colorBalancePx)
@kernel("o_balance", OCL + [('r', 'f'), ('g', 'f'), ('b', 'f'), ('type', 'i'), ('preserve', 'i')], [PIC_IN, PIC_OUT])
def o_balance(S):
    def body(S, p):
        P = S.p
        m = (p[0] + p[1] + p[2]) / 3.0
        w2 = 1.0 - (m) * (m)
        w1 = 1.0 - (m - 0.5) * 2.0 * (m - 0.5) * 2.0
        w0 = 1.0 - (1.0 - m) * (1.0 - m)
        w = sel(S, S.eq(P.type, 2), w2, sel(S, S.eq(P.type, 1), w1, w0))
        pa = (w * P.r) - (w * P.b / 2.0) - (w * P.g / 2.0)
        pb = (w * P.g) - (w * P.b / 2.0) - (w * P.r / 2.0)
        pc = (w * P.b) - (w * P.g / 2.0) - (w * P.r / 2.0)
        pr = S.eq(P.preserve, 1)
        a = sel(S, pr, pa, w * P.r)
        b = sel(S, pr, pb, w * P.g)
        c = sel(S, pr, pc, w * P.b)
        return [p[0] + a, p[1] + b, p[2] + c, p[3]]
    ocl_pixels(S, body)

# ---------------------------------------------------------------------------------------------------------------------------------- Saturation Adjust (vcol::satCurve, saturationPx)
@kernel("o_sat", OCL + [('amount', 'f'), ('centre', 'f'), ('k1', 'f'), ('k3', 'f'), ('low', 'f'), ('high', 'f')], [PIC_IN, PIC_OUT])
def o_sat(S):
    def body(S, p):
        P = S.p
        y = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]
        q = (y - p[0]) * (y - p[0]) + (y - p[1]) * (y - p[1]) + (y - p[2]) * (y - p[2])
        dist = S.sqrt(q)
        x = dist / 0.92720386107910491
        k0, k1, k2, k3, k4 = S.f(0.0), P.k1, P.centre, P.k3, S.f(1.0)
        c1, c2, c3 = x < k1, x < k2, x < k3
        def tt(xa, xb):
            return sel(S, S.eq(xb, xa), S.f(0.0), (x - xa) / (xb - xa))
        a = sel(S, c1, P.low, sel(S, c2, P.low, sel(S, c3, P.amount, P.high)))
        b = sel(S, c1, P.low, sel(S, c2, P.amount, sel(S, c3, P.high, P.high)))
        t = sel(S, c1, tt(k0, k1), sel(S, c2, tt(k1, k2), sel(S, c3, tt(k2, k3), tt(k3, k4))))
        t2 = t * t
        t3 = t2 * t
        cur = (2.0 * t3 - 3.0 * t2 + 1.0) * a + (-2.0 * t3 + 3.0 * t2) * b
        f = cur + 1.0
        w0, w1, w2 = (1.0 - f) * 0.299, (1.0 - f) * 0.587, (1.0 - f) * 0.114
        return [(w0 + f) * p[0] + w1 * p[1] + w2 * p[2],
                w0 * p[0] + (w1 + f) * p[1] + w2 * p[2],
                w0 * p[0] + w1 * p[1] + (w2 + f) * p[2], p[3]]
    ocl_pixels(S, body)

# ---------------------------------------------------------------------------------------------------------------------------------- LAB Adjust (vcol::labPx)
def fcbrt(S, x):
    """cbrtf of a positive float: the double power 1/3 rounded to a float"""
    return sf_pow(S, SF.from_f32(S, x), SF.const(S, 1.0 / 3.0)).to_f32()

@kernel("o_lab", OCL + [('lightness', 'f'), ('a', 'f'), ('b', 'f'), ('invert', 'i'), ('desat', 'f')], [PIC_IN, PIC_OUT])
def o_lab(S):
    def body(S, p):
        P = S.p
        a = p[3]
        part = a < 1.0
        c = [sel(S, part, p[k] / a, p[k]) for k in range(3)]
        e24, ie24 = F(2.4), float(f32(1.0) / f32(2.4))
        v = []
        for k in range(3):
            hi = c[k] > 0.04045
            pw = br(S, hi, lambda k=k: S.pow((c[k] + 0.055) / 1.055, e24), lambda: S.f(0.0))
            v.append(sel(S, hi, pw, c[k] / 12.92))
        x = v[0] * 0.4124 + v[1] * 0.3576 + v[2] * 0.1805
        y = v[0] * 0.2126 + v[1] * 0.7152 + v[2] * 0.0722
        z = v[0] * 0.0193 + v[1] * 0.1192 + v[2] * 0.9505
        x = x / 0.95047
        z = z / 1.08883
        def fxyz(t):
            hi = t > 0.008856
            cb = br(S, hi, lambda: fcbrt(S, t), lambda: S.f(0.0))
            return sel(S, hi, cb, (7.787 * t) + F(f32(16.0) / f32(116.0)))
        x, y, z = fxyz(x), fxyz(y), fxyz(z)
        L = 116.0 * y - 16.0
        A = 500.0 * (x - y)
        B = 200.0 * (y - z)
        L = sel(S, S.ne(P.invert, 0), 95.580666 - L, L)
        L = L * P.lightness
        A = A + P.a
        B = B + P.b
        A = A * P.desat
        B = B * P.desat
        # rgbFromLab
        y = (L + 16.0) / 116.0
        x = A / 500.0 + y
        z = y - B / 200.0
        cx, cy, cz = x * x * x, y * y * y, z * z * z
        off = F(f32(16.0) / f32(116.0))
        x = sel(S, cx > 0.008856, cx, (x - off) / 7.787)
        y = sel(S, cy > 0.008856, cy, (y - off) / 7.787)
        z = sel(S, cz > 0.008856, cz, (z - off) / 7.787)
        x = x * 0.95047
        z = z * 1.08883
        v0 = x * 3.2406 + y * -1.5372 + z * -0.4986
        v1 = x * -0.9689 + y * 1.8758 + z * 0.0415
        v2 = x * 0.0557 + y * -0.2040 + z * 1.0570
        o = []
        for vk in (v0, v1, v2):
            hi = vk > 0.0031308
            pw = br(S, hi, lambda vk=vk: S.pow(vk, ie24), lambda: S.f(0.0))
            o.append(sel(S, hi, 1.055 * pw - 0.055, 12.92 * vk))
        o0 = sel(S, part, ((o[0] * a) * a) * a, o[0])
        o3 = sel(S, part, a, S.f(1.0))
        ok = a > 0.000001
        z0 = S.f(0.0)
        return [sel(S, ok, o0, z0), sel(S, ok, o[1], z0), sel(S, ok, o[2], z0), sel(S, ok, o3, z0)]
    ocl_pixels(S, body)

# ---------------------------------------------------------------------------------------------------------------------------------- Color Corrector (vcol::ccKernelPx)
def cc_curve(S, x, g, A, B, Pp, C):
    """vcol::ccCurveF; g, A, B, Pp, C are uniform"""
    nA = S.f(-1.0) * A
    def small():
        r1 = br(S, x >= A, lambda: (1.0 + B) * S.pow(x, g) - B,
                lambda: br(S, x > nA, lambda: x * Pp, lambda: ((-1.0) - B) * S.pow((-1.0) * x, g) + B))
        return r1
    def big():
        return br(S, x >= A, lambda: S.pow((x + B) * C, g),
                  lambda: br(S, x > nA, lambda: x * Pp, lambda: (-1.0) * S.pow((x - B) * ((-1.0) * C), g)))
    return br(S, g <= 1.0, small, big)

# table: 0 flags (1 gcurve, 2 wheels, 4 sat), 1 gain, 2 offset, 3 satv, 4 g, 5 A, 6 B, 7 P, 8 C, 9 + 4 w: cs, sn, a2, k (w = 0..2)
@kernel("o_cc", OCL, [PIC_IN, PIC_OUT, TABLE])
def o_cc(S):
    tab = S.buf('table')
    T = lambda i: tab.load(i)
    def body(S, p):
        fl = S.to_u(T(0))
        gcurve, wheels, satf = S.ne(fl & 1, 0), S.ne(fl & 2, 0), S.ne(fl & 4, 0)
        gain, offset, satv = T(1), T(2), T(3)
        a0 = p[3]
        a = sel(S, a0 > 1.0, S.f(1.0), a0)
        ok = a > 0.000001
        r, g, b = p[0] / a, p[1] / a, p[2] / a
        Y = (0.299 * r) + (0.587 * g) + (0.114 * b)
        U = 0.492 * (b - Y)
        V = 0.877 * (r - Y)
        ty = Y
        tu = S.f(0.0)
        tv = S.f(0.0)
        use = gcurve | wheels
        cv = br(S, use, lambda: cc_curve(S, Y, T(4), T(5), T(6), T(7), T(8)), lambda: Y)
        tyc = (cv * gain) + offset
        ty = sel(S, use, tyc, ty)
        cen = [S.f(0.0), S.f(0.5), S.f(1.0)]
        for w in range(3):
            cs, sn, a2, k = T(9 + 4 * w), T(10 + 4 * w), T(11 + 4 * w), T(12 + 4 * w)
            x = ty - cen[w]
            wt = br(S, use & wheels, lambda x=x, a2=a2, k=k: a2 * S.exp(-x * x * k), lambda: S.f(0.0))
            du = cs * wt * 0.5
            dv = sn * wt * 0.5
            if w == 0:
                tu, tv = du, dv
            else:
                tu, tv = tu + du, tv + dv
        tu = sel(S, use & wheels, tu, S.f(0.0))
        tv = sel(S, use & wheels, tv, S.f(0.0))
        Y2 = ty
        U = U + tu
        V = V + tv
        U = sel(S, satf, U * satv, U)
        V = sel(S, satf, V * satv, V)
        R = Y2 + (F(f32(1.0) / f32(0.877)) * V)
        G = Y2 + (F(f32(-0.114) / (f32(0.587) * f32(0.492))) * U) + (F(f32(-0.299) / (f32(0.587) * f32(0.877))) * V)
        Bl = Y2 + (F(f32(1.0) / f32(0.492)) * U)
        z = S.f(0.0)
        return [sel(S, ok, R * a, z), sel(S, ok, G * a, z), sel(S, ok, Bl * a, z), sel(S, ok, a, z)]
    ocl_pixels(S, body)

# ---------------------------------------------------------------------------------------------------------------------------------- Color Corrector (Secondary) (vcol::ccsPx)
# table: 0 mode, 1 inv, 2 satOn, 3 lumOn, 4 tintU1, 5 tintV1 (the mode 1 tint: cos * amt^2 * 0.5), 6 satv, 7 alphaF, 8 gain, 9 offset, 10 g, 11 A, 12 B, 13 P, 14 C, 15 hueShift,
# 16..19 M, 20 pass (the picture as it is), 21.. the three windows (lum, sat, hue) of six: lo, hi, sl, sh, k1, k2
def ccs_win(S, T, base, y):
    """CcsWin::at"""
    lo, hi, sl, sh, k1, k2 = [T(base + k) for k in range(6)]
    z = S.f(0.0)
    d1 = lo - y
    a1 = d1 * d1 * k1
    d2 = hi - y
    a2 = d2 * d2 * k2
    c1 = (y < lo) & S.ne(sl, 0.0)
    c2 = (y > hi) & S.ne(sh, 0.0)
    e1 = br(S, c1, lambda: sel(S, (-15.0) > a1, z, S.exp(a1)), lambda: z)
    e2 = br(S, c2 & ~c1, lambda: sel(S, (-15.0) > a2, z, S.exp(a2)), lambda: z)
    inside = (y >= lo) & (y <= hi)
    return sel(S, c1, e1, sel(S, c2, e2, sel(S, inside, S.f(1.0), z)))

@kernel("o_ccs", OCL, [PIC_IN, PIC_OUT, TABLE])
def o_ccs(S):
    tab = S.buf('table')
    T = lambda i: tab.load(i)
    pi = F(3.1415926535897932384)
    def body(S, p):
        mode = S.to_i(T(0))
        inv = S.ne(T(1), 0.0)
        invf = sel(S, inv, S.f(1.0), S.f(0.0))
        satOn, lumOn = S.ne(T(2), 0.0), S.ne(T(3), 0.0)
        satv, alphaF, gain, offset = T(6), T(7), T(8), T(9)
        g, A, B, Pp, C = T(10), T(11), T(12), T(13), T(14)
        passT = S.ne(T(20), 0.0)
        a0 = p[3]
        a = sel(S, a0 > 1.0, S.f(1.0), a0)
        ok = a > 0.000001
        part = a < 1.0
        r, gg, b = [sel(S, part, p[k] / a, p[k]) for k in range(3)]
        y = (0.299 * r) + (0.587 * gg) + (0.114 * b)
        U = 0.492 * (b - y)
        V = 0.877 * (r - y)
        res = [S.var('f') for _ in range(4)]
        # mode 1: the primary-like kernel
        with S.if_(S.eq(mode, 1)):
            tu, tv, tw = T(4), T(5), satv
            ty = cc_curve(S, y, g, A, B, Pp, C)
            ty = ty * gain
            ty = ty + offset
            m = ccs_win(S, T, 21, y * 255.0)
            m = sel(S, inv, 1.0 - m, m)
            ty2 = sel(S, lumOn, y + (m * (ty - y)), ty)
            tu2 = sel(S, lumOn, m * tu, tu)
            tv2 = sel(S, lumOn, m * tv, tv)
            tw2 = sel(S, lumOn, 1.0 + (m * (tw - 1.0)), tw)
            U1 = U + tu2
            V1 = V + tv2
            U1 = sel(S, satOn, U1 * tw2, U1)
            V1 = sel(S, satOn, V1 * tw2, V1)
            R = ty2 + (F(f32(1.0) / f32(0.877)) * V1)
            G = ty2 + (F(f32(-0.114) / (f32(0.587) * f32(0.492))) * U1) + (F(f32(-0.299) / (f32(0.587) * f32(0.877))) * V1)
            Bl = ty2 + (F(f32(1.0) / f32(0.492)) * U1)
            res[0].set(R * a); res[1].set(G * a); res[2].set(Bl * a); res[3].set(a)
        with S.else_():
            # the masks (the kernels' f2)
            chroma = S.sqrt((U * U) + (V * V))
            aU, aV = S.abs(U), S.abs(V)
            cpos = chroma > 0.0
            lt = aU < aV
            ratio0 = sel(S, lt, aU, aV) / chroma
            ratio = sel(S, cpos, ratio0, S.f(0.0))
            oct_ = sel(S, cpos, sel(S, lt, S.i(4), S.i(0)) + sel(S, V < 0.0, S.i(2), S.i(0)) + sel(S, U < 0.0, S.i(1), S.i(0)), S.i(0))
            lumM = ccs_win(S, T, 21, y * 255.0)
            satM = ccs_win(S, T, 27, chroma * 255.0)
            # asinf in the double: atan2(x, sqrt((1 - x)(1 + x)))
            rd = SF.from_f32(S, ratio)
            asd = sf_atan2(S, rd, ((SF.const(S, 1.0) - rd) * (SF.const(S, 1.0) + rd)).sqrt())
            aa = asd.to_f32() * F(f32(180.0) / f32(pi))
            bb = 90.0 - aa
            angs = [aa, 180.0 - aa, 360.0 - aa, 180.0 + aa, bb, 180.0 - bb, 360.0 - bb, 180.0 + bb]
            ang = angs[7]
            for k in range(6, -1, -1):
                ang = sel(S, S.eq(oct_, k), angs[k], ang)
            h = ang + T(15)
            h = sel(S, 0.0 > h, h + 360.0, sel(S, 360.0 < h, h - 360.0, h))
            hueM = ccs_win(S, T, 33, h)
            mm = lumM * satM * hueM
            pos = 0.0 < mm
            dy = br(S, pos, lambda: ((cc_curve(S, y, g, A, B, Pp, C) * gain) + offset) - y, lambda: S.f(0.0))
            m = lumM * satM * hueM
            invv = invf
            with S.if_(S.eq(mode, 3)):
                v = (invv - m) * sel(S, inv, S.f(1.0), S.f(-1.0))
                v = S.min(S.max(v, 0.0), 1.0)
                for k in range(4): res[k].set(v)
            with S.else_():
                w = S.abs(m - invv)
                with S.if_(0.0 < w):
                    w1 = 1.0 - w
                    y2 = y + dy
                    u2 = U + T(4)
                    v2 = V + T(5)
                    nu = T(16) * u2 + T(17) * v2
                    nv = T(18) * u2 + T(19) * v2
                    R = y2 + (F(f32(1.0) / f32(0.877)) * nv)
                    G = y2 + (F(f32(-0.114) / (f32(0.587) * f32(0.492))) * nu) + (F(f32(-0.299) / (f32(0.587) * f32(0.877))) * nv)
                    Bl = y2 + (F(f32(1.0) / f32(0.492)) * nu)
                    wa = w * alphaF
                    na = a * wa + a * w1
                    nr, ng, nb = R * wa + r * w1, G * wa + gg * w1, Bl * wa + b * w1
                    na = S.min(S.max(na, 0.0), 1.0)
                    res[0].set(nr * a); res[1].set(ng * a); res[2].set(nb * a); res[3].set(na)
                with S.else_():
                    for k in range(4): res[k].set(p[k])
        z = S.f(0.0)
        out = [sel(S, ok, res[k].get(), z) for k in range(4)]
        return [sel(S, passT, p[k], out[k]) for k in range(4)]
    ocl_pixels(S, body)

# ---------------------------------------------------------------------------------------------------------------------------------- Broadcast Colors (vcol::broadcastPx)
# table: the settings and vcol::BroadcastFrame in the order of BCAST_FIELDS (vkeffects.cpp writes it in the same order)
BC = {n: i for i, n in enumerate("compMin compMax v18 v19 v21 v22 v23 v72 lumaLo0 lumaLo1 lumaHi0 lumaHi1 lumaKneeLo lumaKneeHi lumaMid chromaStart chromaEnd chromaLimit chromaCub3 chromaCub2 v79 v47 lumaLoCub3 lumaLoCub2 lumaHiCub3 lumaHiCub2 compLoStart compLoEnd compHiStart compHiEnd compLoCub3 compLoCub2 compHiCub3 compHiCub2 v80 v81".split())}

@kernel("o_bcast", OCL, [PIC_IN, PIC_OUT, TABLE])
def o_bcast(S):
    tab = S.buf('table')
    f = lambda n: tab.load(BC[n])
    def body(S, p):
        z = S.f(0.0)
        w0 = p[3]
        big = w0 > 1.0
        w = sel(S, big, S.f(1.0), w0)
        px = [p[0], p[1], p[2], sel(S, big, S.f(1.0), p[3])]
        ok = w > 0.000001
        part = w < 1.0
        v82, v83, v7 = [sel(S, part, (px[k] * 255.0) / w, px[k] * 255.0) for k in range(3)]
        v49 = (-0.147 * v82 - 0.289 * v83 + 0.436 * v7)
        v50 = (0.615 * v82 - 0.515 * v83 - 0.100 * v7)
        v49 = sel(S, (v49 > F(-1.0 * f32(0.000001))) & (v49 < 0.000001), z, v49)
        v50 = sel(S, (v50 > F(-1.0 * f32(0.000001))) & (v50 < 0.000001), z, v50)
        # the luma (f1)
        v30 = (px[0] * 255.0 - f('v19')) * f('v23')
        v31 = (px[1] * 255.0 - f('v19')) * f('v23')
        v32 = (px[2] * 255.0 - f('v19')) * f('v23')
        l0 = 0.299 * v30 + 0.587 * v31 + 0.114 * v32 + f('v18')
        v24, v25 = f('lumaKneeLo'), f('lumaKneeHi')
        lo0, lo1, hi0, hi1 = f('lumaLo0'), f('lumaLo1'), f('lumaHi0'), f('lumaHi1')
        c_a = l0 < lo0
        c_b = (l0 >= lo0) & (l0 < lo1)
        c_c = (l0 >= lo1) & (l0 < hi0)
        c_d = (l0 >= hi0) & (l0 < hi1)
        c_e = l0 >= hi1
        xa = l0 - lo0
        xb = l0 - hi1
        x2a = xa * xa
        x3a = x2a * xa
        v_b = f('lumaLoCub3') * x3a + f('lumaLoCub2') * x2a + v24
        x2b = xb * xb
        x3b = x2b * xb
        v_d = f('lumaHiCub3') * x3b + f('lumaHiCub2') * x2b + v25
        v15 = sel(S, c_a, v24, sel(S, c_b, v_b, sel(S, c_c, l0, sel(S, c_d, v_d, sel(S, c_e, v25, l0)))))
        v16 = sel(S, c_c, S.i(0), S.i(1))
        yv = (v15 - f('v18')) * f('v22') + f('v19')
        v45 = (S.abs(v49) + S.abs(v50)) * f('v23')
        v84 = f('compMax') - v15
        v85 = v15 - f('compMin')
        keep = S.eq(v16, 0) & (v45 < (v84 + 0.000001)) & (v45 < (v85 + 0.000001)) & (v45 < (f('v47') + 0.000001))
        v86 = sel(S, v85 < v84, v85, v84)
        v87 = sel(S, v85 < v84, f('v80'), f('v81'))
        sideMax = ~(v85 < v84)
        v89 = (v49 * v49 + v50 * v50) * f('v72')
        v90 = v86 * v86 * v87
        # the chroma curve
        cs, ce, cl = f('chromaStart'), f('chromaEnd'), f('chromaLimit')
        c1 = v89 > f('v79')
        e = 2.0 * S.sqrt(v89)
        xc = e - ce
        x2c = xc * xc
        x3c = x2c * xc
        fac = sel(S, (e > 0.0) & (e < cs), S.f(1.0),
                  sel(S, (e >= cs) & (e < ce), (f('chromaCub3') * x3c + f('chromaCub2') * x2c + cl) / e,
                      sel(S, e >= ce, cl / e, z)))
        v46 = sel(S, c1, fac, S.f(1.0))
        v49b = sel(S, c1, v49 * v46, v49)
        v50b = sel(S, c1, v50 * v46, v50)
        v89b = sel(S, c1, v89 * (v46 * v46), v89)
        v16b = sel(S, c1, S.i(1), v16)
        # the composite curves
        c2 = v89b > v90
        e2 = S.sqrt(v89b)
        v27 = v86 / e2
        c3 = v27 > F(f32(100.0) / f32(120.0))
        v54 = 100.0 / v27
        def compf(st, en, c3_, c2_):
            x = v54 - en
            x2 = x * x
            x3 = x2 * x
            return sel(S, v54 < st, S.f(1.0), sel(S, (v54 >= st) & (v54 < en), (c3_ * x3 + c2_ * x2 + 100.0) / v54, 100.0 / v54))
        comp0 = compf(f('compLoStart'), f('compLoEnd'), f('compLoCub3'), f('compLoCub2'))
        comp1 = compf(f('compHiStart'), f('compHiEnd'), f('compHiCub3'), f('compHiCub2'))
        v27b = sel(S, c3, sel(S, sideMax, comp1, comp0), v27)
        v49c = sel(S, c2, v49b * v27b, v49b)
        v50c = sel(S, c2, v50b * v27b, v50b)
        v16c = sel(S, c2, S.i(1), v16b)
        yv2 = yv / 255.0
        a49 = v49c / 255.0
        a50 = v50c / 255.0
        r1 = yv2 + 1.140 * a50
        g1 = yv2 - 0.394 * a49 - 0.581 * a50
        b1 = yv2 + 2.032 * a49
        conv = S.ne(v16c, 0)
        res = [sel(S, conv, r1 * w, px[0]), sel(S, conv, g1 * w, px[1]), sel(S, conv, b1 * w, px[2]), sel(S, conv, w, px[3])]
        res = [sel(S, keep, p[k], res[k]) for k in range(4)]
        return [sel(S, ok, res[k], z) for k in range(4)]
    ocl_pixels(S, body)


# ---------------------------------------------------------------------------------------------------------------------------------- Levels (vfxlc::levelsPixel)
@kernel("o_levels", OCL + [('channel', 'i'), ('linear', 'i'), ('curve0', 'i'), ('range', 'f'), ('inStart', 'f'), ('outStart', 'f'), ('outEnd', 'f'), ('toe', 'f'), ('off', 'f'),
                           ('slope', 'f'), ('v7', 'f'), ('e', 'f')], [PIC_IN, PIC_OUT])
def o_levels(S):
    P = S.p
    def levels_f(x):
        # vfxlc::levelsF: one power at most (its base is chosen first)
        lin = S.ne(P.linear, 0)
        c0 = S.ne(P.curve0, 0)
        d = x - P.inStart
        m = d * (P.outEnd - P.outStart)
        qv = m / P.range
        lres = qv + P.outStart
        t = (x - P.inStart) / P.range
        hi = t >= P.toe
        mid = t > -P.toe
        base = sel(S, c0, sel(S, hi, t, -t), sel(S, hi, (t + P.off) * P.v7, (t - P.off) * (-P.v7)))
        usepow = (~lin) & (hi | ~mid)
        pw = br(S, usepow, lambda: S.pow(base, P.e), lambda: S.f(0.0))
        t0 = sel(S, hi, (1.0 + P.off) * pw - P.off, sel(S, mid, t * P.slope, (-1.0 - P.off) * pw + P.off))
        t1 = sel(S, hi, pw, sel(S, mid, t * P.slope, -pw))
        tt = sel(S, c0, t0, t1)
        r = tt * (P.outEnd - P.outStart)
        return sel(S, lin, lres, r + P.outStart)
    def body(S, p):
        a = p[3]
        ok = a > 0.0
        pa = S.local_array(4)
        ra = S.local_array(4)
        for k in range(4):
            pa.store(k, p[k])
        with S.for_(0, 4) as k:
            x = sel(S, k < 3, pa.load(k) / a, a)
            ra.store(k, levels_f(x))
        r = [ra.load(k) for k in range(4)]
        ch = P.channel
        allc = S.eq(ch, 0) | S.eq(ch, 4)
        out = []
        for k in range(3):
            need = allc | S.eq(ch, k + 1)
            out.append(sel(S, need, r[k] * a, p[k]))
        o3 = sel(S, S.eq(ch, 4), r[3], a)
        z = S.f(0.0)
        return [sel(S, ok, out[0], z), sel(S, ok, out[1], z), sel(S, ok, out[2], z), sel(S, ok, o3, z)]
    ocl_pixels(S, body, read=False)

# ---------------------------------------------------------------------------------------------------------------------------------- Convolution Kernel (vfxlc::convRows)
# table: the nine weights A[i], the offset, alphaPlain
@kernel("o_conv", OCL, [PIC_IN, PIC_OUT, TABLE])
def o_conv(S):
    P = S.p
    tab = S.buf('table')
    src = S.buf('in0')
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        q = S.ne(P.q, 0)
        v = [S.f(0.0), S.f(0.0), S.f(0.0), S.f(0.0)]
        for t in range(9):
            dx, dy = (t % 3) - 1, (t // 3) - 1
            xx = S.min(S.to_i(P.w) - 1, S.max(S.i(0), S.to_i(S.gx) + dx))
            yy = S.min(S.to_i(P.h) - 1, S.max(S.i(0), S.to_i(S.gy) - dy))
            px = src.load(S.to_u(yy) * P.w + S.to_u(xx))
            A = tab.load(t)
            for k in range(4):
                term = A * px[k]
                v[k] = term if t == 0 else v[k] + term
        c3 = src.load(i)[3]
        off = tab.load(9)
        v3 = sel(S, S.ne(tab.load(10), 0.0), c3, v[3])
        v3 = S.min(S.max(v3, 0.0), 1.0)
        o = [(v[k] + off) * v3 for k in range(3)] + [v3]
        S.buf('out').store(i, S.vec4(*[S.select(q, store12(S, o[k]), o[k]) for k in range(4)]))

# ---------------------------------------------------------------------------------------------------------------------------------- HSL Adjust (vhw::hslPixel)
@kernel("o_hsl", OCL + [('hue', 'f'), ('sat', 'f'), ('lum', 'f')], [PIC_IN, PIC_OUT])
def o_hsl(S):
    P = S.p
    EPS = S.f(0.000001)
    def body(S, p):
        c = [S.var('f') for _ in range(3)]
        for k in range(3):
            c[k].set(p[k])
        c0, c1, c2 = c[0].get(), c[1].get(), c[2].get()
        mx0 = sel(S, sel(S, c0 < c1, c1, c0) < c2, c2, sel(S, c0 < c1, c1, c0))
        mn0 = sel(S, c2 < sel(S, c1 < c0, c1, c0), c2, sel(S, c1 < c0, c1, c0))
        mx, mn = S.var('f', mx0), S.var('f', mn0)
        def set_ch(cond, which_max, v):
            a0, a1, a2 = c[0].get(), c[1].get(), c[2].get()
            if which_max:
                i0 = (a0 > a1) & (a0 > a2)
                i1 = ~i0 & (a1 > a2)
            else:
                i0 = (a0 < a1) & (a0 < a2)
                i1 = ~i0 & (a1 < a2)
            i2 = ~i0 & ~i1
            c[0].set(sel(S, cond & i0, v, a0))
            c[1].set(sel(S, cond & i1, v, a1))
            c[2].set(sel(S, cond & i2, v, a2))
        sm = mn.get() + mx.get()
        ca = (sm > 2.0 - EPS) & (sm < 2.0)
        cb = ~ca & (sm >= 2.0) & (sm < 2.0 + EPS)
        mx.set(sel(S, ca, mx.get() - EPS, sel(S, cb, mx.get() + EPS, mx.get())))
        set_ch(ca | cb, True, mx.get())
        sm = mn.get() + mx.get()
        cc = (sm > 0.0 - EPS) & (sm < 0.0)
        cd = ~cc & (sm >= 0.0) & (sm < 0.0 + EPS)
        mn.set(sel(S, cc, mn.get() - EPS, sel(S, cd, mn.get() + EPS, mn.get())))
        set_ch(cc | cd, False, mn.get())
        mxv, mnv = mx.get(), mn.get()
        c0, c1, c2 = c[0].get(), c[1].get(), c[2].get()
        L = (mnv + mxv) * 0.5
        S0 = mxv - mnv
        flat = ~((S0 < -EPS) | (S0 >= EPS))
        S1 = sel(S, L <= 0.5, S0 / (mnv + mxv), S0 / ((2.0 - mxv) - mnv))
        S2 = sel(S, (L < 0.0) | (L > 1.0), S1 * -1.0, S1)
        d = 1.0 / (mxv - mnv)
        cr, cg, cb2 = (mxv - c0) * d, (mxv - c1) * d, (mxv - c2) * d
        Hr = sel(S, S.eq(c0, mxv), sel(S, S.eq(c1, mnv), 5.0 + cb2, 1.0 - cg),
                 sel(S, S.eq(c1, mxv), sel(S, S.eq(c2, mnv), 1.0 + cr, 3.0 - cb2),
                     sel(S, S.eq(c0, mnv), 3.0 + cg, 5.0 - cr)))
        Hh = Hr * F(f32(1.0) / f32(6.0))
        Hh = sel(S, flat, S.f(0.0), Hh)
        Sf = sel(S, flat, S0, S2)
        h = Hh + P.hue
        h = h - S.floor(h)
        s = Sf * P.sat
        l = L * P.lum
        # hslToRgb(h, s, l)
        lo = l <= 0.5
        s1 = sel(S, lo, sel(S, l < 0.0, s * -1.0, s), sel(S, l > 1.0, s * -1.0, s))
        q = sel(S, lo, l * (1.0 + s1), (l + s1) - l * s1)
        t = h * 6.0
        pp = (l + l) - q
        ii = S.to_i(t)
        f = t - S.to_f(ii)
        q1 = (q - pp) * f
        up, dn = pp + q1, q - q1
        def pick(i0, i1, i2, i3, i4, i5, dflt):
            return sel(S, S.eq(ii, 0) | S.eq(ii, 6), i0, sel(S, S.eq(ii, 1), i1, sel(S, S.eq(ii, 2), i2, sel(S, S.eq(ii, 3), i3, sel(S, S.eq(ii, 4), i4, sel(S, S.eq(ii, 5), i5, dflt))))))
        o0 = pick(q, dn, pp, pp, up, q, h)
        o1 = pick(up, q, q, dn, pp, pp, s1)
        o2 = pick(pp, pp, up, q, q, dn, l)
        return [o0, o1, o2, p[3]]
    ocl_pixels(S, body, read=False)

# ---------------------------------------------------------------------------------------------------------------------------------- White Balance (vhw::wbPixel, the HasData histogram and stretch)
# The shift pass: the plan works out du, dv = cos / sin of the angle times amount^2 times 0.5 with the CPU's own library calls.
@kernel("o_wb_shift", OCL + [('bright', 'f'), ('du', 'f'), ('dv', 'f')], [PIC_IN, PIC_OUT])
def o_wb_shift(S):
    P = S.p
    def body(S, p):
        a0 = p[3]
        a = sel(S, a0 > 1.0, S.f(1.0), a0)
        ok = a > 0.000001
        part = a < 1.0
        r, g, b = [sel(S, part, p[k] / a, p[k]) for k in range(3)]
        y = (0.299 * r) + (0.587 * g) + (0.114 * b)
        u = 0.492 * (b - y)
        v = 0.877 * (r - y)
        yb = (y * 1.0) * P.bright
        y = sel(S, S.ne(P.bright, 1.0), yb, y)
        u = u + P.du
        v = v + P.dv
        R = y + (1.14025085518814 * v)
        G = y + (-0.394731374911705 * u) + (-0.580809209031098 * v)
        B = y + (2.032520325203250 * u)
        z = S.f(0.0)
        return [sel(S, ok, R * a, z), sel(S, ok, G * a, z), sel(S, ok, B * a, z), sel(S, ok, a, z)]
    ocl_pixels(S, body, read=False)

@kernel("o_wb_clear", [('n', 'u'), ('one', 'u')], [(2, 'cnt', 'u', 'rw')])
def o_wb_clear(S):
    with S.in_range(S.p.n, S.p.one):
        S.buf('cnt').store(S.gx, S.u(0))

@kernel("o_wb_hist", WH + [('npix', 'u'), ('pad', 'u')], [(0, 'mid', 'v4', 'r'), (2, 'hist', 'u', 'rw')])
def o_wb_hist(S):
    # the first npix pixels counted from the bottom row, each colour value as convert_uchar_sat(255 * v) (vhw::wbPlaneByte)
    P = S.p
    with S.in_range(P.w, P.h):
        i = (P.h - 1 - S.gy) * P.w + S.gx
        with S.if_(i < P.npix):
            m = S.buf('mid').load(S.gy * P.w + S.gx)
            h = S.buf('hist')
            for k in range(3):
                t = 255.0 * m[k]
                by = S.select(t > 0.0, S.select(t >= 255.0, S.u(255), S.to_u(t)), S.u(0))
                h.atomic_add(S.u(256 * k) + by, S.u(1))

@kernel("o_wb_stats", WH, [(0, 'hist', 'u', 'r'), (2, 'stats', 'f', 'w')])
def o_wb_stats(S):
    # vhw::wbStretchChannel, one thread per channel: the running sum in floats, scaled by (float)(1 / total)
    P = S.p
    with S.in_range(P.w, P.h):
        hist = S.buf('hist')
        base = S.gx * 256
        run = S.var('f', S.f(0.0))
        with S.for_(0, 256) as i:
            run.set(run.get() + S.to_f(hist.load(base + i)))
        total = run.get()
        sc = (SF.const(S, 1.0) / SF.from_f32(S, total)).to_f32()
        run2 = S.var('f', S.f(0.0))
        still = S.var('u', S.u(1))
        lo = S.var('u', S.u(0))
        nle = S.var('u', S.u(0))
        thr = S.f(0.9950000047683716)
        with S.for_(0, 256) as i:
            run2.set(run2.get() + S.to_f(hist.load(base + i)))
            cs = run2.get() * sc
            below = S.f(0.005) > cs
            with S.if_(S.ne(still.get(), 0) & below & (i < 255)):
                lo.set(lo.get() + 1)
            still.set(S.select(below, still.get(), S.u(0)))
            with S.if_(~(cs >= thr)):
                nle.set(nle.get() + 1)
        hi = S.select(nle.get() > 0, nle.get() - 1, S.u(0))
        lov = lo.get()
        diff = S.to_f(hi - lov)
        same = S.eq(hi, lov)
        gain = S.select(same, S.f(1.0), S.f(256.0) / S.select(same, S.f(1.0), diff))
        off = S.to_f(lov) * S.f(1.0 / 256.0)
        ok = total > 0.0
        S.buf('stats').store(S.gx * 2, S.select(ok, gain, S.f(1.0)))
        S.buf('stats').store(S.gx * 2 + 1, S.select(ok, off, S.f(0.0)))

@kernel("o_wb_final", OCL, [PIC_IN, (1, 'stats', 'f', 'r'), PIC_OUT])
def o_wb_final(S):
    # vhw::wbStretchPixel, then the 12-bit store, and the stretched blue also goes into the alpha
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        st = S.buf('stats')
        q = S.ne(P.q, 0)
        o = []
        for k in range(3):
            t = st.load(2 * k) * (p[k] - st.load(2 * k + 1))
            m1 = S.select(t < 1.0, t, S.f(1.0))
            o.append(S.select(0.0 < m1, m1, S.f(0.0)))
        w = [S.select(q, store12(S, o[k]), o[k]) for k in range(3)]
        S.buf('out').store(i, S.vec4(w[0], w[1], w[2], w[2]))

# ---------------------------------------------------------------------------------------------------------------------------------- Add Noise (vfxnoise::render)
# One thread per column: its ranlux state (24 numbers and the carry, from vfxnoise::columnStates) is in the table, 25 floats per column.
@kernel("o_noise", OCL + [('level', 'f'), ('mono', 'u'), ('gauss', 'u'), ('pad3', 'u')], [PIC_IN, PIC_OUT, TABLE])
def o_noise(S):
    P = S.p
    tab = S.buf('table')
    src = S.buf('in0')
    with S.in_range(P.w, 1):
        x = S.gx
        v = S.local_array(24)
        for k in range(24):
            v.store(k, tab.load(x * 25 + k))
        carry = S.var('f', tab.load(x * 25 + 24))
        e24 = S.f(5.9604644775390625e-08)
        def step(a, bidx):
            b = v.load(bidx)
            t = (a - b) - carry.get()
            neg = t < 0.0
            t2 = S.select(neg, t + 1.0, t)
            carry.set(S.select(neg, e24, S.f(0.0)))
            v.store(bidx, t2)
            r = S.select(t2 < 0.000244140625, t2 + e24 * a, t2)
            return S.select(S.eq(r, 0.0), S.f(F(f32(5.9604644775390625e-08) * f32(5.9604644775390625e-08))), r)
        def group(g):
            """vfxnoise::Ranlux::group for a group number g (a uint): the four numbers"""
            ap = (S.u(8) - g) % 6
            aq = (S.u(7) - g) % 6
            bb = S.u(5) - g
            ay = v.load(ap * 4 + 1); ax = v.load(ap * 4)
            a2w = v.load(aq * 4 + 3); a2z = v.load(aq * 4 + 2)
            ox = step(ay, bb * 4 + 3)
            oy = step(ax, bb * 4 + 2)
            oz = step(a2w, bb * 4 + 1)
            ow = step(a2z, bb * 4)
            return ox, oy, oz, ow
        with S.for_(0, 96) as it:
            group(it % 6)
        with S.for_(0, P.h) as j:
            y = P.h - 1 - j
            i = y * P.w + x
            p = src.load(i)
            u0, u1, u2, u3 = group(j % 6)
            n = [S.var('f') for _ in range(3)]
            with S.if_(S.ne(P.gauss, 0)):
                def box(a, b):
                    r = S.sqrt(-2.0 * S.log(a))
                    ang = F(2.0 * f32(3.1415926535897932384)) * b
                    return r * S.sin(ang), r * S.cos(ang)
                s0, c0 = box(u0, u1)
                s1, c1 = box(u2, u3)
                n[0].set(s0); n[1].set(c0); n[2].set(s1)
            with S.else_():
                n[0].set(u0 - 0.5); n[1].set(u1 - 0.5); n[2].set(u2 - 0.5)
            n0 = n[0].get()
            mono = S.ne(P.mono, 0)
            nn = [n0, S.select(mono, n0, n[1].get()), S.select(mono, n0, n[2].get())]
            a = p[3]
            q = S.ne(P.q, 0)
            part = a < 1.0
            res = []
            for k in range(3):
                rp = (p[k] / a + P.level * nn[k]) * a
                rf = p[k] + P.level * nn[k]
                res.append(S.select(part, rp, rf))
            o3 = S.select(part, a, S.f(1.0))
            ok = a > 0.000001
            z = S.f(0.0)
            outv = [S.select(ok, res[0], z), S.select(ok, res[1], z), S.select(ok, res[2], z), S.select(ok, o3, z)]
            S.buf('out').store(i, S.vec4(*[S.select(q, store12(S, outv[k]), outv[k]) for k in range(4)]))

# ---------------------------------------------------------------------------------------------------------------------------------- Gradient Map (vfxgm::run8, vfxgm::runFloat)
# 8-bit (c.levels): the 256-entry table of bytes (B, G, R, A per entry) is in the table; the integer steps of vfxgm::run8 on the levels.
@kernel("o_gm8", WH + [('mode', 'u'), ('amtq', 'i')], [PIC_IN, PIC_OUT, TABLE])
def o_gm8(S):
    from gpukernels import to_level, from_level
    P = S.p
    tab = S.buf('table')
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        px = S.buf('in0').load(i)
        pl = [to_level(S, px.z), to_level(S, px.y), to_level(S, px.x), to_level(S, px.w)]      # B, G, R, A
        lum = (pl[0] * 0x74b + pl[1] * 0x2591 + pl[2] * 0x1322 + 0x2000) >> 14
        base = S.to_u(lum) * 4
        L = [S.to_i(tab.load(base + k)) for k in range(4)]
        t = L[3] * P.amtq
        an = S.min(S.i(255), (t >> 14) + pl[3])
        wq = t / 255
        add = S.ne(P.mode, 0)
        out = []
        for k in range(3):
            va = (L[k] * wq) / 16384 + pl[k]
            vb = (L[k] * wq + pl[k] * (0x4000 - wq)) / 16384
            v = S.select(add, va, vb)
            out.append(from_level(S, S.min(v, an)))
        S.buf('out').store(i, S.vec4(out[2], out[1], out[0], from_level(S, an)))

# float: the curves read at the grey (a double) with the spline's Bezier points per segment (vfxgm::segmentPoints), all in the software double.
# table: 0 n, 1.. nine key times (doubles), 19 + 36 i: segment i: m1 time, dt, then for the curves alpha, red-ish colour 1, 2, 3 four control values each (doubles)
@kernel("o_gmf", WH + [('mode', 'u'), ('amt', 'f')], [PIC_IN, PIC_OUT, TABLE])
def o_gmf(S):
    from gpukernels import ld_sf
    P = S.p
    tab = S.buf('table')
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        px = S.buf('in0').load(i)
        Bp, Gp, Rp, Ap = px.z, px.y, px.x, px.w
        lum = SF.from_f32(S, Bp) * 0.114
        lum = lum + SF.from_f32(S, Gp) * 0.587
        lum = lum + SF.from_f32(S, Rp) * 0.299
        n = S.to_u(tab.load(0))
        cnt = S.u(0)
        for k in range(9):
            kt = ld_sf(S, tab, 1 + 2 * k)
            cnt = cnt + S.select((S.u(k) < n) & (kt <= lum), S.u(1), S.u(0))
        base = S.u(19) + cnt * 36
        m1t = ld_sf(S, tab, base)
        dt = ld_sf(S, tab, base + 2)
        u = SF.sel(S, dt > 0.0, (lum - m1t) / dt, SF.const(S, 0.0))
        vals = []
        for cmp_ in range(4):
            cp = [ld_sf(S, tab, base + 4 + (cmp_ * 4 + j) * 2) for j in range(4)]
            a = (cp[1] - cp[0]) * u + cp[0]
            b = (cp[2] - cp[1]) * u + cp[1]
            c = (cp[3] - cp[2]) * u + cp[2]
            d = (b - a) * u + a
            e = (c - b) * u + b
            vals.append((e - d) * u + d)
        av = vals[0]
        zero = SF.const(S, 0.0)
        al = S.select(av < 0.0, S.f(0.0), SF.sel(S, av < 1.0, av, SF.const(S, 1.0)).to_f32())
        ald = SF.from_f32(S, al)
        o = []
        for k in range(3):
            v = vals[1 + k]
            o.append(S.select(v < 0.0, S.f(0.0), SF.sel(S, v < ald, v, ald).to_f32()))
        o3 = al
        ta = o3 * P.amt
        p0, p1, p2 = Bp, Gp, Rp
        om = 1.0 - ta
        add = S.ne(P.mode, 0)
        r0 = S.select(add, o[0] * ta + p0, om * p0 + o[0] * ta)
        r1 = S.select(add, o[1] * ta + p1, om * p1 + o[1] * ta)
        r2 = S.select(add, o[2] * ta + p2, om * p2 + o[2] * ta)
        a = ta + Ap
        S.buf('out').store(i, S.vec4(r2, r1, r0, S.select(a < 1.0, a, S.f(1.0))))

# ---------------------------------------------------------------------------------------------------------------------------------- Chroma Keyer (vcol::keyerAlpha, keyerBlurPlane, keyerFinish)
def keyer_hsl(S, c):
    """vcol::keyerHsl: (h, s, l) of a straight colour"""
    mx = sel(S, c[0] > c[1], c[0], c[1])
    mx = sel(S, mx > c[2], mx, c[2])
    mn = sel(S, c[0] < c[1], c[0], c[1])
    mn = sel(S, mn < c[2], mn, c[2])
    l = (mn + mx) * 0.5
    z = S.f(0.0)
    dark = l <= 0.0
    s0 = mx - mn
    sat = s0 > 0.0
    d = sel(S, l <= 0.5, mn + mx, (2.0 - mx) - mn)
    s1 = s0 / d
    inv = 1.0 / (mx - mn)
    r, g, b = (mx - c[0]) * inv, (mx - c[1]) * inv, (mx - c[2]) * inv
    v8 = [1.0 + r, 3.0 + g, 5.0 + b]
    v9 = [5.0 - r, 1.0 - g, 3.0 - b]
    h = sel(S, S.eq(c[0], mx), sel(S, S.eq(c[1], mn), v8[2], v9[1]),
            sel(S, S.eq(c[1], mx), sel(S, S.eq(c[2], mn), v8[0], v9[2]),
                sel(S, S.eq(c[0], mn), v8[1], v9[0])))
    h = h * 0.16666667
    live = ~dark & sat
    return sel(S, live, h, z), sel(S, dark, z, sel(S, sat, s1, s0)), l

@kernel("o_key_alpha", OCL + [('r', 'f'), ('g', 'f'), ('b', 'f'), ('kh', 'f'), ('ks', 'f'), ('kl', 'f'), ('low', 'f'), ('high', 'f'), ('invr', 'f')], [PIC_IN, (2, 'out', 'f', 'w')])
def o_key_alpha(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        q = S.ne(P.q, 0)
        px = [S.select(q, read_level(S, p[k]), p[k]) for k in range(4)]
        a = px[3]
        div = ~S.eq(a, 1.0) & ~S.eq(a, 0.0)
        c = [sel(S, div, px[k] / a, px[k]) for k in range(3)]
        h, s, l = keyer_hsl(S, c)
        dh = S.abs(h - P.kh)
        ds = S.abs(s - P.ks)
        dl = S.abs(l - P.kl)
        dh = sel(S, dh > 0.5, 1.0 - dh, dh)
        dh = dh * 2.0
        d = (dh + ds + dl) * F(f32(1.0) / f32(3.0))
        dx, dy, dz = c[0] - P.r, c[1] - P.g, c[2] - P.b
        dist = S.sqrt(fma_exact(S, dx, dx, fma_exact(S, dy, dy, dz * dz)))
        d = fma_exact(S, dist, S.f(1.732050807568877), d)
        d = d * 0.5
        k = sel(S, d < P.low, S.f(0.0), sel(S, d + 0.000001 > P.high, S.f(1.0), clamp01f(S, (d - P.low) * P.invr)))
        S.buf('out').store(i, sel(S, a < k, a, k))

@kernel("o_key_line", WH + [('axis', 'u'), ('q', 'u')], [(0, 'src', 'f', 'r'), (1, 'bwd', 'f', 'w'), (2, 'fwd', 'f', 'w'), TABLE])
def o_key_line(S):
    # vcol::keyerBlurLine along the columns (axis 0, the line starts at the bottom row) or along the rows (axis 1): fwd and bwd hold the two passes' values
    P = S.p
    tab = S.buf('table')
    A = lambda k: tab.load(k)
    B = lambda k: tab.load(5 + k)
    C = lambda k: tab.load(10 + k)
    D = lambda k: tab.load(15 + k)
    E = lambda k: tab.load(20 + k)
    q = S.ne(P.q, 0)
    def Q(v):
        return S.select(q, store12(S, v), v)
    cols = S.eq(P.axis, 0)
    n_lines = S.select(cols, P.w, P.h)
    L = S.select(cols, P.h, P.w)
    with S.in_range(n_lines, 1):
        line = S.gx
        def idx(r):
            # element r of the line (the kernels' order)
            return S.select(cols, (P.h - 1 - r) * P.w + line, line * P.w + r)
        src, fwd, bwd = S.buf('src'), S.buf('fwd'), S.buf('bwd')
        # ---- the causal pass
        w = src.load(idx(S.u(0)))
        v42 = -D(4) * w
        v41 = v42 + (A(3) - D(3)) * w
        v40 = v41 + (A(2) - D(2)) * w
        v39 = v40 + (A(1) - D(1)) * w
        v38 = v39 + (A(0) - D(0)) * w
        v49 = v38
        fwd.store(idx(S.u(0)), Q(v49))
        w = src.load(idx(S.u(1)))
        V = {n: S.var('f') for n in (38, 39, 40, 41, 42, 43, 44, 45, 46, 49)}
        V[43].set(S.f(0.0)); V[44].set(v49 * B(0)); V[45].set(S.f(0.0)); V[46].set(S.f(0.0))
        V[38].set(v39 + w * A(0)); V[39].set(v40 + w * A(1)); V[40].set(v41 + w * A(2)); V[41].set(v42 + w * A(3)); V[42].set(S.f(0.0))
        V[49].set(V[38].get() - V[43].get())
        fwd.store(idx(S.u(1)), Q(V[49].get()))
        with S.for_(2, L) as x:
            w = src.load(idx(x))
            v43 = V[44].get() + V[49].get() * B(1)
            v44 = V[45].get() + V[49].get() * B(2)
            v45 = V[46].get() + V[49].get() * B(3)
            v46 = V[49].get() * B(4)
            n38 = V[39].get() + w * A(0)
            n39 = V[40].get() + w * A(1)
            n40 = V[41].get() + w * A(2)
            n41 = V[42].get() + w * A(3)
            V[43].set(v43); V[44].set(v44); V[45].set(v45); V[46].set(v46)
            V[38].set(n38); V[39].set(n39); V[40].set(n40); V[41].set(n41)
            V[49].set(n38 - v43)
            fwd.store(idx(x), Q(V[49].get()))
        # ---- the anti-causal pass
        Lm1 = L - 1
        w = src.load(idx(Lm1))
        u42 = (C(4) - E(4)) * w
        u41 = u42 + (C(3) - E(3)) * w
        u40 = u41 + (C(2) - E(2)) * w
        u39 = u40 + (C(1) - E(1)) * w
        u38 = u39 + (C(0) - E(0)) * w
        u49 = u38
        bwd.store(idx(Lm1), Q(u49))
        w = src.load(idx(L - 2))
        U = {n: S.var('f') for n in (38, 39, 40, 41, 42, 43, 44, 45, 46, 49)}
        U[43].set(S.f(0.0)); U[44].set(u49 * B(0)); U[45].set(S.f(0.0)); U[46].set(S.f(0.0))
        U[38].set(u39 + w * C(0)); U[39].set(u40 + w * C(1)); U[40].set(u41 + w * C(2)); U[41].set(u42 + w * C(3)); U[42].set(w * C(4))
        U[49].set(U[38].get() - U[43].get())
        bwd.store(idx(L - 2), Q(U[49].get()))
        with S.for_(2, L) as x:
            r = Lm1 - x
            w = src.load(idx(r))
            v43 = U[44].get() + U[49].get() * B(1)
            v44 = U[45].get() + U[49].get() * B(2)
            v45 = U[46].get() + U[49].get() * B(3)
            v46 = U[49].get() * B(4)
            n38 = U[39].get() + w * C(0)
            n39 = U[40].get() + w * C(1)
            n40 = U[41].get() + w * C(2)
            n41 = U[42].get() + w * C(3)
            n42 = w * C(4)
            U[43].set(v43); U[44].set(v44); U[45].set(v45); U[46].set(v46)
            U[38].set(n38); U[39].set(n39); U[40].set(n40); U[41].set(n41); U[42].set(n42)
            U[49].set(n38 - v43)
            bwd.store(idx(r), Q(U[49].get()))

@kernel("o_key_sum", WH, [(0, 'a', 'f', 'r'), (1, 'b', 'f', 'r'), (2, 'out', 'f', 'w')])
def o_key_sum(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        S.buf('out').store(i, S.buf('a').load(i) + S.buf('b').load(i))

@kernel("o_key_finish", OCL + [('despill', 'u'), ('clampall', 'u'), ('showmask', 'u'), ('pad4', 'u')], [PIC_IN, (1, 'alpha', 'f', 'r'), PIC_OUT])
def o_key_finish(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        q = S.ne(P.q, 0)
        px = [S.select(q, read_level(S, p[k]), p[k]) for k in range(3)]
        v = px + [S.buf('alpha').load(i)]
        cl = S.ne(P.clampall, 0)
        v = [S.select(cl, clamp01f(S, v[k]), v[k]) for k in range(4)]
        inr = (v[3] >= 0.00392) & (v[3] <= 1.0)
        d2 = S.eq(P.despill, 2) & (v[2] > v[1]) & inr
        d1 = ~(S.eq(P.despill, 2) & (v[2] > v[1])) & S.eq(P.despill, 1) & (v[1] > v[0]) & inr
        v2 = S.select(d2, v[1], v[2])
        v1 = S.select(d1, v[0], v[1])
        v = [v[0], v1, v2, v[3]]
        sm = S.ne(P.showmask, 0)
        o = [S.select(sm, v[3], v[0] * v[3]), S.select(sm, v[3], v[1] * v[3]), S.select(sm, v[3], v[2] * v[3]), v[3]]
        S.buf('out').store(i, S.vec4(*[S.select(q, store12(S, o[k]), o[k]) for k in range(4)]))

# ---------------------------------------------------------------------------------------------------------------------------------- Pixelate (pixelateOpenCl in builtins_filters.cpp)
# table: x cell start (W), x cell length (W), y cell start (H), y cell length (H); the plan works the cells out with the CPU's own pixelateCellsOcl. Rows of cells first, then columns
# (the buffers are top row first: the CPU's vertical cells that start at its last row start at the first row here).
def _pix_pass(S, horizontal):
    P = S.p
    tab = S.buf('table')
    inb = S.buf('in0')
    with S.in_range(P.w, P.h):
        if horizontal:
            st = tab.load(S.gx)
            ln = tab.load(P.w + S.gx)
        else:
            st = tab.load(P.w + P.w + S.gy)
            ln = tab.load(P.w + P.w + P.h + S.gy)
        n = S.to_u(ln)
        s0 = S.to_u(st)
        acc = [S.var('f', 0.0) for _ in range(4)]
        with S.for_(0, n) as i:
            if horizontal:
                v = inb.load(S.gy * P.w + (s0 + i))
            else:
                v = inb.load((s0 + i) * P.w + S.gx)
            for k in range(4):
                acc[k].set(acc[k].get() + v[k])
        q = S.ne(P.q, 0)
        o = []
        for k in range(4):
            m = acc[k].get() / ln
            o.append(S.select(q, store12(S, m), m))
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(*o))

@kernel("o_pix_h", OCL, [PIC_IN, PIC_OUT, TABLE])
def o_pix_h(S):
    _pix_pass(S, True)

@kernel("o_pix_v", OCL, [PIC_IN, PIC_OUT, TABLE])
def o_pix_v(S):
    _pix_pass(S, False)

# ---------------------------------------------------------------------------------------------------------------------------------- Mirror (mirrorOpenCl in builtins_filters.cpp)
# kind 0..3: the axis kernels (the picture bottom row first: the row asked for is H - 1 - the buffer's row), 4: the general one (top row first). The plan works out what is the same for
# every pixel with the CPU's own float code (v5, v6, the tangent terms, the integer limits).
@kernel("o_mirror", OCL + [('kind', 'u'), ('bg0', 'f'), ('bg1', 'f'), ('bg2', 'f'), ('bg3', 'f'), ('v5', 'f'), ('v6', 'f'), ('pos', 'u'), ('v22', 'f'), ('v23', 'f'), ('k0', 'f'),
                           ('i10', 'i'), ('iA1', 'i'), ('iA2', 'i'), ('pad1', 'u')], [PIC_IN, PIC_OUT])
def o_mirror(S):
    P = S.p
    inb = S.buf('in0')
    W = S.to_i(P.w)
    H = S.to_i(P.h)
    q = S.ne(P.q, 0)
    bg = [P.bg0, P.bg1, P.bg2, P.bg3]
    with S.in_range(P.w, P.h):
        x = S.to_i(S.gx)
        gy = S.to_i(S.gy)
        general = S.eq(P.kind, 4)
        y = S.select(general, gy, H - 1 - gy)
        r = [S.var('f', 0.0) for _ in range(4)]
        def px(xx, yy):
            row = S.select(general, yy, H - 1 - yy)
            return inb.load(S.to_u(row * W + xx))
        def put(vals):
            for k in range(4):
                r[k].set(vals[k])
        def putpx(xx, yy):
            v = px(xx, yy)
            put([v[k] for k in range(4)])
        def mix2(p0, p1, t):
            return [mixf(S, p0[k], p1[k], t) for k in range(4)]
        def reflect(v13, size, at):
            v14 = S.to_i(v13)
            v15 = v13 - S.to_f(v14)
            with S.if_((v14 < 0) | (v14 >= size)):
                put(bg)
            with S.else_():
                with S.if_(S.eq(v14, size - 1)):
                    p0 = at(v14)
                    put(mix2([p0[k] for k in range(4)], bg, v15))
                with S.else_():
                    p0, p1 = at(v14), at(v14 + 1)
                    put(mix2([p0[k] for k in range(4)], [p1[k] for k in range(4)], v15))
        k_ = P.kind
        with S.if_(k_ < 2):
            lineRow = S.select(S.eq(k_, 0), ~(y < P.i10) & (y < P.iA1), ~(y < P.iA2) & (y <= P.i10))
            with S.if_(S.eq(k_, 1) & (y < P.iA2)):
                put(bg)
            with S.else_():
                with S.if_(~lineRow):
                    putpx(x, y)
                with S.else_():
                    v13 = (2.0 * P.v6 - S.to_f(y))
                    reflect(v13, H, lambda yy: px(x, yy))
        with S.else_():
            with S.if_(k_ < 4):
                refl = S.select(S.eq(k_, 2), ~(x < P.i10), x <= P.i10)
                with S.if_(~refl):
                    putpx(x, y)
                with S.else_():
                    v13 = (2.0 * P.v5 - S.to_f(x))
                    reflect(v13, W, lambda xx: px(xx, y))
            with S.else_():
                yf = S.to_f(y)
                v25 = fma_exact(S, P.v6 - yf, P.v23, P.v5)
                v26 = S.to_i(v25) + 1
                pos = S.ne(P.pos, 0)
                v27 = S.select(pos, S.min(v26, W), S.i(0))
                v28 = S.select(pos, W, S.min(v26, W))
                with S.if_((x < v27) | (x >= v28)):
                    putpx(x, y)
                with S.else_():
                    v29 = S.to_f(x)
                    v30 = (fma_exact(S, v29, -P.v23, yf) + P.k0) / (P.v22 - P.v23)
                    v31 = fma_exact(S, P.v22, v30 - P.v5, P.v6)
                    rx = (v30 * 2.0 - v29)
                    ry = (v31 * 2.0 - yf)
                    v36 = S.to_i(S.floor(rx))
                    v37 = S.to_i(S.floor(ry))
                    with S.if_((v36 < -1) | (v36 >= W) | (v37 < -1) | (v37 >= H)):
                        put(bg)
                    with S.else_():
                        t = [[S.var('f', 0.0) for _ in range(4)] for _ in range(4)]
                        def setb(j):
                            for k in range(4): t[j][k].set(bg[k])
                        def setp(j, xx, yy):
                            v = px(xx, yy)
                            for k in range(4): t[j][k].set(v[k])
                        with S.if_(v36 < 0):
                            setb(0); setb(2)
                            with S.if_(v37 < 0):
                                setb(1); setp(3, S.i(0), S.i(0))
                            with S.else_():
                                setp(1, S.i(0), v37)
                                with S.if_(v37 >= H - 1):
                                    setp(3, S.i(0), v37)
                                with S.else_():
                                    setp(3, S.i(0), v37 + 1)
                        with S.else_():
                            with S.if_(v36 >= W - 1):
                                setb(1); setb(3)
                                with S.if_(v37 < 0):
                                    setb(0); setp(2, v36, S.i(0))
                                with S.else_():
                                    setp(0, v36, v37)
                                    with S.if_(v37 >= H - 1):
                                        setp(2, v36, v37)
                                    with S.else_():
                                        setp(2, v36, v37 + 1)
                            with S.else_():
                                with S.if_(v37 < 0):
                                    setb(0); setb(1); setp(2, v36, S.i(0)); setp(3, v36 + 1, S.i(0))
                                with S.else_():
                                    with S.if_(v37 >= H - 1):
                                        setp(0, v36, v37); setp(1, v36 + 1, v37); setp(2, v36, v37); setp(3, v36 + 1, v37)
                                    with S.else_():
                                        setp(0, v36, v37); setp(1, v36 + 1, v37); setp(2, v36, v37 + 1); setp(3, v36 + 1, v37 + 1)
                        fx = rx - S.to_f(v36)
                        fy = ry - S.to_f(v37)
                        T = [[t[j][k].get() for k in range(4)] for j in range(4)]
                        m0 = mix2(T[0], T[1], fx)
                        m1 = mix2(T[2], T[3], fx)
                        put(mix2(m0, m1, fy))
        o = [S.select(q, store12(S, r[k].get()), r[k].get()) for k in range(4)]
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(*o))

# ---------------------------------------------------------------------------------------------------------------------------------- Median, Min and Max (medianOpenCl in builtins_more_blur.cpp)
# Three passes: the half-size picture as levels (every second pixel counted from the bottom row; the half-size buffers hold their rows bottom first, as the CPU's), the rank per row and channel
# (the weighted histogram slid along the row with the same integer steps as the CPU), the bicubic read back to full size through the 12-bit store.
@kernel("o_med_levels", WH + [('fw', 'u'), ('fh', 'u')], [PIC_IN, PIC_OUT])
def o_med_levels(S):
    P = S.p
    with S.in_range(P.w, P.h):
        row = S.to_u(S.to_i(P.fh) - 1 - S.to_i(S.gy) * 2)
        v = S.buf('in0').load(row * P.fw + S.gx * 2)
        o = [S.round_even(S.min(S.max(v[k] * 255.0, 0.0), 255.0)) for k in range(4)]
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(*o))

# thread (a row of the half-size picture, a channel); table: nfull taps then nedge taps (dx, dy, weight)
@kernel("o_med_rank", WH + [('wh', 'u'), ('nfull', 'u'), ('nedge', 'u'), ('interval', 'u'), ('rank', 'f'), ('pad', 'u')], [PIC_IN, (2, 'out', 'f', 'w'), TABLE])
def o_med_rank(S):
    P = S.p
    tab = S.buf('table')
    inb = S.buf('in0')
    with S.in_range(P.w, P.h):
        y = S.to_i(S.gx)
        ch = S.gy
        Wh = S.to_i(P.wh)
        Hh = S.to_i(P.w)
        hist = S.local_array(256, 'i')
        cum = S.local_array(256, 'i')
        def chv(v):
            return S.select(S.eq(ch, 0), v[0], S.select(S.eq(ch, 1), v[1], S.select(S.eq(ch, 2), v[2], v[3])))
        def tap(xx, yy, z, plus):
            with S.if_((xx >= 0) & (xx < Wh) & (yy >= 0) & (yy < Hh)):
                lv = S.to_u(S.to_i(chv(inb.load(S.to_u(yy * Wh + xx)))))
                old = hist.load(lv)
                hist.store(lv, old + z if plus else old - z)
        def entry(base, e):
            i = e * 3 + base
            return S.to_i(tab.load(i)), S.to_i(tab.load(i + 1)), S.to_i(tab.load(i + 2))
        with S.for_(0, P.nfull) as e:
            dx, dy, z = entry(S.u(0), e)
            tap(dx, y + dy, z, True)
        base = P.nfull * 3
        with S.for_(0, P.wh) as xu:
            x = S.to_i(xu)
            with S.if_(xu > 0):
                with S.for_(0, P.nedge) as e:
                    dx, dy, z = entry(base, e)
                    tap(x + dx, y + dy, z, True)
                    tap(x - dx - 1, y + dy, z, False)
                    with S.if_(S.ne(dy, 0)):
                        tap(x - dx - 1, y - dy, z, False)
                        tap(x + dx, y - dy, z, True)
            run = S.var('i', 0)
            with S.for_(0, 256) as b:
                run.set(run.get() + hist.load(b))
                cum.store(b, run.get())
            c255 = cum.load(S.u(255))
            pick = S.var('i', 0)
            with S.if_(S.eq(P.interval, 0)):
                t = S.to_i(S.to_f(c255) * P.rank)
                found = S.var('u', 0)
                with S.for_(0, 256) as b:
                    cb = cum.load(b)
                    hb = hist.load(b)
                    hit = S.select(P.rank < 0.5, (t < cb) & ((cb - hb) <= t), (t <= cb) & ((cb - hb) < t))
                    with S.if_(hit & S.eq(found.get(), 0)):
                        pick.set(S.to_i(b))
                        found.set(S.u(1))
            with S.else_():
                sc = 20000.0
                tot = S.to_f(c255) / sc
                mid = P.rank * (tot - 1.0) + 0.5
                hi = mid + 0.5
                lo = mid - 0.5
                lane = S.local_array(64, 'f')
                with S.for_(0, 64) as l:
                    acc = S.var('f', 0.0)
                    for j in range(4):
                        b = l * 4 + j
                        bm = S.select(b > 0, b - 1, S.u(0))
                        prev = S.select(b > 0, S.to_f(cum.load(bm)) / sc, S.f(0.0))
                        cur = S.to_f(cum.load(b)) / sc
                        h = S.to_f(hist.load(b)) / sc
                        bf = S.to_f(b)
                        inner = S.select(prev >= lo, S.select(cur > hi, (hi - prev) * bf, h * bf), S.select(cur > hi, bf, (cur - lo) * bf))
                        term = S.select((prev < hi) & (cur >= lo), inner, S.f(0.0))
                        if j == 0:
                            acc.set(term)
                        else:
                            acc.set(acc.get() + term)
                    lane.store(l, acc.get())
                grp = S.local_array(16, 'f')
                with S.for_(0, 16) as g:
                    a0 = lane.load(g * 4)
                    a1 = lane.load(g * 4 + 1)
                    a2 = lane.load(g * 4 + 2)
                    a3 = lane.load(g * 4 + 3)
                    grp.store(g, (a2 + a3) + (a0 + a1))
                quad = S.local_array(4, 'f')
                with S.for_(0, 4) as g:
                    a0 = grp.load(g * 4)
                    a1 = grp.load(g * 4 + 1)
                    a2 = grp.load(g * 4 + 2)
                    a3 = grp.load(g * 4 + 3)
                    quad.store(g, (a2 + a3) + (a0 + a1))
                tt = (quad.load(S.u(2)) + quad.load(S.u(3))) + (quad.load(S.u(0)) + quad.load(S.u(1)))
                pick.set(S.max(S.i(0), S.min(S.i(255), S.to_i(tt))))
            S.buf('out').store(S.to_u((y * Wh + x) * 4) + ch, S.to_f(pick.get()) / 255.0)
            # the histogram is carried to the next pixel as it is (zeroed only at the start of the row: the array is fresh for each invocation)

def _cubic_w(S, t):
    a = F(-0.5)
    u = t + 1.0
    v = 2.0 - t
    s = 1.0 - t
    w0 = ((a * u - F(5.0 * a)) * u + F(8.0 * a)) * u - F(4.0 * a)
    w1 = ((F(a + 2.0) * t - F(a + 3.0)) * t) * t + 1.0
    w2 = ((F(a + 2.0) * s - F(a + 3.0)) * s) * s + 1.0
    w3 = ((a * v - F(5.0 * a)) * v + F(8.0 * a)) * v - F(4.0 * a)
    return [w0, w1, w2, w3]

@kernel("o_med_up", WH + [('wh', 'u'), ('hh', 'u')], [PIC_IN, PIC_OUT])
def o_med_up(S):
    P = S.p
    inb = S.buf('in0')
    with S.in_range(P.w, P.h):
        Wh = S.to_i(P.wh)
        Hh = S.to_i(P.hh)
        y = S.to_i(P.h) - 1 - S.to_i(S.gy)
        px = S.to_f(S.to_i(S.gx)) / 2.0
        py = S.to_f(y) / 2.0
        flx = S.floor(px)
        fly = S.floor(py)
        tx = px - flx
        ty = py - fly
        ix = S.to_i(S.max(-4.0, S.min(S.to_f(Wh) + 4.0, flx)))
        iy = S.to_i(S.max(-4.0, S.min(S.to_f(Hh) + 4.0, fly)))
        wx = _cubic_w(S, tx)
        wy = _cubic_w(S, ty)
        acc = [S.var('f', 0.0) for _ in range(4)]
        for j in range(4):
            yy = S.max(0, S.min(Hh - 1, iy - 1 + j))
            for i in range(4):
                xx = S.max(0, S.min(Wh - 1, ix - 1 + i))
                p = inb.load(S.to_u(yy * Wh + xx))
                w = wy[j] * wx[i]
                for k in range(4):
                    acc[k].set(acc[k].get() + w * p[k])
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(*[store12(S, acc[k].get()) for k in range(4)]))
