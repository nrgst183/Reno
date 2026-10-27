"""The kernels of the transitions and the generators that are made exact against their CPU code (src/ofx/builtins_trans.cpp, builtins_gen.cpp).
Imported by gpukernels.py (which defines the kernel language helpers used here). The planners are in src/ofx/vktrans.cpp.

A transition's outgoing frame is buffer 0 (in0), the incoming one the table (w * h * 4 floats, premultiplied, top row first) - or, for the
kernels that need the table for something else, a buffer made by table_to_buf. Numbers the CPU works out in doubles come in the push constants
or a table as the halves of the bits of the double.
"""
from gpukernels import kernel, PIC_IN, PIC_IN1, PIC_OUT, TABLE, WH, SF, to_px, ld_sf, fmin, fmax, fsqrt, luma

QW = lambda n: [('q%d' % k, 'u') for k in range(n)]      # generic 32-bit words (an int, a float's bits, or half a double)


def qsf(P, k):
    """the double held in push words q<k> (high half) and q<k+1> (low half)"""
    return SF(P.S, getattr(P, 'q%d' % k), getattr(P, 'q%d' % (k + 1)))


def qf(S, P, k):
    """the float held in push word q<k>"""
    return S.bits_to_f(getattr(P, 'q%d' % k))


def qi(S, P, k):
    return S.to_i(getattr(P, 'q%d' % k))


def to8(S, v):
    """the lambda to8 of builtins_trans.cpp: x = v * 255 + 0.5, cut to 0..255 and truncated; an int"""
    x = v * 255.0 + 0.5
    return S.to_i(S.select(x < 0.0, S.f(0.0), S.select(x > 255.0, S.f(255.0), x)))


def lerp8(S, a, b, w):
    """dis::lerp8: ((diff * w + 128) + its high part) / 256 + a, cut to 0..255 (ints)"""
    x = (b - a) * w + 128
    r = (((x >> 8) + x) >> 8) + a
    return S.max(S.min(r, S.i(255)), S.i(0))


def f_from_level(S, i):
    return S.to_f(i) * S.f(1.0 / 255.0)


# ---------------------------------------------------------------------------------------------------------- Dissolve
@kernel("dissolve8", WH + [('type', 'i'), ('w1024', 'i'), ('t255', 'i')] + QW(12), [PIC_IN, PIC_OUT, TABLE])
def dissolve8(S):
    # dis::run: q0..3 the thresholds of the outgoing picture (colour bleed / morph), q4..7 of the incoming, q8..11 the fade colour
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        pa = S.buf('in0').load(i)
        pb = to_px(S, i)
        A = [to8(S, pa[k]) for k in range(4)]
        B = [to8(S, pb[k]) for k in range(4)]
        ta = [qi(S, P, k) for k in range(4)]
        tb = [qi(S, P, 4 + k) for k in range(4)]
        fc = [qi(S, P, 8 + k) for k in range(4)]
        w1024, t255, ty = P.w1024, P.t255, P.type
        O = [S.var('i', 0) for _ in range(4)]
        def satadd(a, b): return S.min(a + b, S.i(255))
        with S.if_(S.eq(ty, 0)):
            first = w1024 < 512
            wt = S.select(first, w1024 * 2, (S.i(1024) - w1024) * 2)
            for k in range(4):
                base = S.select(first, A[k], B[k])
                oth = S.select(first, B[k], A[k])
                O[k].set(satadd(base, (oth * wt) >> 10))
        with S.else_():
            with S.if_(S.eq(ty, 1) | S.eq(ty, 2)):
                o3 = lerp8(S, A[3], B[3], t255)
                O[3].set(o3)
                for k in range(3):
                    v2 = ((A[k] + B[k]) * w1024) / 1024 - A[k]
                    v1 = S.select(w1024 < 512, A[k] - (B[k] * (w1024 * 2)) / 1024, B[k] - (A[k] * ((S.i(1024) - w1024) * 2)) / 1024)
                    v = S.select(S.eq(ty, 2), v2, v1)
                    O[k].set(S.min(S.abs(v), o3))
            with S.else_():
                with S.if_(S.eq(ty, 3)):
                    for k in (3, 2, 1, 0):
                        d1 = S.max(S.i(0), A[k] - ta[k])
                        d2 = S.max(S.i(0), B[k] - tb[k])
                        lim = S.i(255) if k == 3 else O[3].get()
                        sm = d1 + d2
                        O[k].set(S.select(sm < lim, sm & 255, lim))
                with S.else_():
                    with S.if_(S.eq(ty, 4)):
                        for k in (3, 2, 1, 0):
                            v = S.select(A[k] < B[k], S.min(B[k], A[k] + ta[k]), S.max(B[k], A[k] - ta[k]))
                            if k < 3:
                                v = S.select(v >= O[3].get(), O[3].get(), v)
                            O[k].set(v)
                    with S.else_():
                        with S.if_(S.eq(ty, 5) | S.eq(ty, 6)):
                            T = [S.select(S.eq(ty, 5), A[k], B[k]) for k in range(3)]
                            avg4 = ((T[0] + T[1] + T[2]) * 4) / 3
                            use_a = S.select(S.eq(ty, 5), avg4 > w1024, avg4 < (S.i(1024) - w1024))
                            for k in range(4):
                                O[k].set(S.select(use_a, A[k], B[k]))
                        with S.else_():
                            with S.if_(S.eq(ty, 7) | S.eq(ty, 8)):
                                gv = (A[0] + A[1] + A[2] + B[0] + B[1] + B[2]) / 6
                                for k in range(4):
                                    g = S.select(S.eq(ty, 7), gv, fc[k]) if k < 3 else fc[3]
                                    lo = lerp8(S, A[k], g, t255 * 2)
                                    w = S.max(S.min(t255 * 2 - 254, S.i(255)), S.i(0))
                                    hi = lerp8(S, g, B[k], w)
                                    v = S.select(t255 < 127, lo, hi)
                                    if k == 3:
                                        v = S.select(S.eq(ty, 7), lerp8(S, A[3], B[3], t255), v)
                                    O[k].set(v)
                            with S.else_():
                                o3 = lerp8(S, A[3], B[3], t255)
                                O[3].set(o3)
                                ph = S.select(t255 < 85, S.i(0), S.select(t255 < 170, S.i(1), S.i(2)))
                                w = S.max(S.min((t255 - ph * 85) * 3, S.i(255)), S.i(0))
                                for k in range(3):
                                    v = S.select(S.i(k) < ph, B[k], S.select(S.eq(ph, k), lerp8(S, A[k], B[k], w), A[k]))
                                    O[k].set(S.min(v, o3))
        S.buf('out').store(i, S.vec4(f_from_level(S, O[0].get()), f_from_level(S, O[1].get()), f_from_level(S, O[2].get()), f_from_level(S, O[3].get())))


@kernel("dissolve_f", WH + [('type', 'i')] + QW(24), [PIC_IN, PIC_OUT, TABLE])
def dissolve_f(S):
    # dissolve() on float pictures. Words: types 0..2: q0 / q1 the floats for the outgoing / incoming picture; 3 and 4: the doubles ta[k] (q0 + 2k) and tb[k] (q8 + 2k)
    # (morph: the limit in ta); 5 and 6: the threshold double in q0, q1; 7 and 8: the doubles p * 2 and p * 2 - 1 in q0, q2, the middle colour in q4..q7 (type 8),
    # q8 / q9 the floats 1 - p and p, q10 (p < 0.5); 9: the floats t[k] in q0..q3
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        a = S.buf('in0').load(i)
        b = to_px(S, i)
        ty = P.type
        R = [S.var('f', 0.0) for _ in range(4)]
        with S.if_(S.eq(ty, 0)):
            ka, kb = qf(S, P, 0), qf(S, P, 1)
            for k in range(4):
                v = a[k] * ka + b[k] * kb
                R[k].set(S.select(v < 1.0, v, S.f(1.0)))
        with S.else_():
            with S.if_(S.eq(ty, 1)):
                ka, kb = qf(S, P, 0), qf(S, P, 1)
                for k in range(4):
                    va, vb = a[k] * ka, b[k] * kb
                    if k == 3:
                        s = va + vb
                        R[k].set(S.select(s < 1.0, s, S.f(1.0)))
                    else:
                        R[k].set(S.abs(va - vb))
            with S.else_():
                with S.if_(S.eq(ty, 2)):
                    ka, kb = qf(S, P, 0), qf(S, P, 1)
                    for k in range(4):
                        va, vb = a[k] * ka, b[k] * kb
                        R[k].set(va + vb if k == 3 else S.abs(va - vb))
                with S.else_():
                    with S.if_(S.eq(ty, 3) | S.eq(ty, 4)):
                        a3, b3 = a[3], b[3]
                        res = {}
                        for k in (3, 2, 1, 0):
                            sa = a[k]
                            sb = b[k]
                            if k < 3:
                                sa = S.select(a3 > 1e-6, a[k] / a3, a[k])
                                sb = S.select(b3 > 1e-6, b[k] / b3, b[k])
                            fa, fb = SF.from_f32(S, sa), SF.from_f32(S, sb)
                            ta, tb = qsf(P, 2 * k), qsf(P, 8 + 2 * k)
                            zero = SF.const(S, 0.0)
                            d3 = (zero.max(fa - ta) + zero.max(fb - tb)).to_f32()
                            lim = ta
                            fd = SF.from_f32(S, sb - sa)                           # (float - float)
                            cl = SF.sel(S, fd < lim.neg(), lim.neg(), SF.sel(S, fd > lim, lim, fd))
                            d4 = sa + cl.to_f32()
                            r = S.select(S.eq(ty, 3), d3, d4)
                            r = S.select(r < 0.0, S.f(0.0), r)               # std::max(res, 0.f)
                            r = S.select(1.0 < r, S.f(1.0), r)               # std::min(.., 1.f)
                            res[k] = r
                        for k in range(3):
                            R[k].set(res[k] * res[3])
                        R[3].set(res[3])
                    with S.else_():
                        with S.if_(S.eq(ty, 5) | S.eq(ty, 6)):
                            t = [S.select(S.eq(ty, 5), a[k], b[k]) for k in range(3)]
                            avg = SF.from_f32(S, (t[0] + t[1]) + t[2]) * 255.0 / 3.0
                            tob = avg <= qsf(P, 0)
                            for k in range(4):
                                R[k].set(S.select(tob, b[k], a[k]))
                        with S.else_():
                            with S.if_(S.eq(ty, 7) | S.eq(ty, 8)):
                                p1, p2 = qf(S, P, 8), qf(S, P, 9)           # (float)(1 - p), (float)p
                                lm = luma(S, a[0], a[1], a[2]) * p1 + luma(S, b[0], b[1], b[2]) * p2
                                mid = [S.select(S.eq(ty, 7), lm, qf(S, P, 4 + k)) for k in range(3)] + [S.select(S.eq(ty, 7), S.f(1.0), qf(S, P, 7))]
                                first = S.ne(P.q10, 0)
                                tt = SF.sel(S, first, qsf(P, 0), qsf(P, 2))
                                for k in range(4):
                                    x0 = S.select(first, a[k], mid[k])
                                    x1 = S.select(first, mid[k], b[k])
                                    R[k].set((SF.from_f32(S, x0) + SF.from_f32(S, x1 - x0) * tt).to_f32())
                            with S.else_():
                                for k in range(4):
                                    R[k].set(a[k] + (b[k] - a[k]) * qf(S, P, k))
        o = [R[k].get() for k in range(4)]
        o = [S.select(v < 0.0, S.f(0.0), v) for v in o]
        o = [S.select(1.0 < v, S.f(1.0), v) for v in o]
        for k in range(3):
            o[k] = S.select(o[k] > o[3], o[3], o[k])
        S.buf('out').store(i, S.vec4(o[0], o[1], o[2], o[3]))


# ---------------------------------------------------------------------------------------------------------- Linear Wipe and Clock Wipe on 8-bit pictures
def smooth_weight(S, u):
    """g = (3 - 2u) u^2 * 255 + 0.5, cut to a whole number and masked to a byte (the CPU's guarded (int)g & 255); an int"""
    g = 3.0 - (u + u)
    g = g * (u * u)
    g = g * 255.0
    g = g + 0.5
    bad = S.ne(g, g) | (g >= 2147483648.0) | (g < -2147483648.0)
    return S.select(bad, S.i(0), S.to_i(g) & 255)


def mix_wipe8(S, fa, fb, wgt, a_first):
    """per channel: the bytes of the two pictures mixed by the weight: r = (x + ((d + (d < 0 ? 255 : 0)) >> 8)) & 255 with d = (a - b) * wgt (linear: x = b)
    or d = (b - a) * wgt (clock: x = a); the two pictures' floats in, the mixed floats out"""
    out = []
    for k in range(4):
        av, bv = to8(S, fa[k]), to8(S, fb[k])
        if a_first:        # clock wipe: a + (d >> 8), d = (b - a) * wgt
            d = (bv - av) * wgt
            r = (av + ((d + S.select(d < 0, S.i(255), S.i(0))) >> 8)) & 255
        else:
            d = (av - bv) * wgt
            r = (bv + ((d + S.select(d < 0, S.i(255), S.i(0))) >> 8)) & 255
        out.append(r)
    return out


@kernel("wipe_linear8", WH + QW(9), [PIC_IN, PIC_OUT, TABLE])
def wipe_linear8(S):
    # linearWipe8(): q0 x7, q1 y12, q2 o6, q3 inv, q4 edge, q5 lo, q6 hi, q7 q, q8 k2 (floats the planner worked out as the CPU does)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        x7, y12, o6, inv, edge, lo, hi, q, k2 = [qf(S, P, k) for k in range(9)]
        ys = S.to_f(S.gy) * y12
        v = S.to_f(S.gx) * x7
        v = v + ys
        v = v + o6
        sv = S.abs(v) * inv
        fa = S.buf('in0').load(i)
        fb = to_px(S, i)
        u = sv - edge
        u = u + q
        u = u * k2
        wgt = smooth_weight(S, u)
        mixed = mix_wipe8(S, fa, fb, wgt, False)
        res = []
        for k in range(4):
            r = S.select(lo > sv, to8(S, fb[k]), S.select(sv > hi, to8(S, fa[k]), mixed[k]))
            res.append(f_from_level(S, r))
        S.buf('out').store(i, S.vec4(res[0], res[1], res[2], res[3]))


def sf_asin(S, x):
    from sf64 import sf_atan2
    one = SF.const(S, 1.0)
    return sf_atan2(S, x, (one - x * x).sqrt())


@kernel("wipe_clock8", WH + [('ccw', 'u'), ('done', 'u')] + QW(7), [PIC_IN, PIC_OUT, TABLE])
def wipe_clock8(S):
    # clockWipe8(): q0 f, q1 f2 (= f + 2 pi), q2 edge, q3 cx, q4 cy + 0.5, q5 2 pi, q6 sqrt 2 (floats the planner worked out as the CPU does)
    from sf64 import sf_atan2
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        f, f2, edge, cx, cy5, twopi, root2 = [qf(S, P, k) for k in range(7)]
        ccw = S.ne(P.ccw, 0)
        Y = cy5 - S.to_f(S.gy)
        X = S.select(ccw, cx - S.to_f(S.gx), S.to_f(S.gx) - cx)
        a0 = sf_atan2(S, SF.from_f32(S, X), SF.from_f32(S, Y)).to_f32()
        a = S.select(a0 < 0.0, a0 + twopi, a0)
        use_a = ~(edge > a)
        R = fsqrt(S, X * X + Y * Y)
        rr = S.select(R < 2.0, S.f(2.0), R)                      # std::max(R, 2.0f): R < 2 ? 2 : R
        ww0 = sf_asin(S, SF.from_f32(S, root2 / rr)).to_f32()
        ww = S.select(ww0 > f, ww0, f)
        ew = edge - ww
        low = S.select(ew < f2, ew, f2)                           # std::min(f2, edge - ww)
        use_b = S.ne(P.done, 0) | (low >= a)
        t = (a - low) / ww
        u = 1.0 - t
        wgt = smooth_weight(S, u)
        fa = S.buf('in0').load(i)
        fb = to_px(S, i)
        mixed = mix_wipe8(S, fa, fb, wgt, True)
        res = []
        for k in range(4):
            r = S.select(use_a, to8(S, fa[k]), S.select(use_b, to8(S, fb[k]), mixed[k]))
            res.append(f_from_level(S, r))
        S.buf('out').store(i, S.vec4(res[0], res[1], res[2], res[3]))


# ---------------------------------------------------------------------------------------------------------- Linear Wipe and Clock Wipe on float pictures (doubles, as the CPU)
def sf_clamp(S, v, lo, hi):
    """clampd: v < lo ? lo : (v > hi ? hi : v)"""
    lo, hi = SF.of(S, lo), SF.of(S, hi)
    return SF.sel(S, v < lo, lo, SF.sel(S, v > hi, hi, v))


def sf_smooth01(S, t):
    t = sf_clamp(S, t, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def sf_mixpx(S, a, b, t):
    """mixPx: per channel (float)(a + (float)(b - a) * t) with t a double"""
    out = []
    for k in range(4):
        out.append((SF.from_f32(S, a[k]) + SF.from_f32(S, b[k] - a[k]) * t).to_f32())
    return S.vec4(*out)


@kernel("wipe_linear_f", WH + QW(10), [PIC_IN, PIC_OUT, TABLE])
def wipe_linear_f(S):
    # linearWipe() on float pictures: q0 dx, q2 dy, q4 span, q6 w (the doubles the planner worked out as the CPU does); edge in the table's... no: q8 edge
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        dx, dy, span, w = qsf(P, 0), qsf(P, 2), qsf(P, 4), qsf(P, 6)
        edge = SF(S, P.q8, P.q9)
        Wd, Hd = SF.from_u32(S, P.w), SF.from_u32(S, P.h)
        xc = SF.from_u32(S, S.gx) + 0.5
        yd = SF.from_u32(S, S.gy) + 0.5
        s = ((xc - Wd * 0.5) * dx + (yd - Hd * 0.5) * dy) / span + 0.5
        hard = SF.sel(S, s < edge, SF.const(S, 1.0), SF.const(S, 0.0))
        t = SF.sel(S, w <= 1e-9, hard, sf_smooth01(S, (edge - s) / w))
        S.buf('out').store(i, sf_mixpx(S, S.buf('in0').load(i), to_px(S, i), t))


@kernel("wipe_clock_f", WH + [('ccw', 'u'), ('mode', 'u')] + QW(12), [PIC_IN, PIC_OUT, TABLE])
def wipe_clock_f(S):
    # clockWipe() on float pictures; doubles q0 F, q2 edge, q4 nex, q6 ney, q8 uex, q10 uey; mode 0 soft (F > 1e-9), 1 edge <= 0, 2 edge >= 360, 3 hard
    from sf64 import sf_atan2
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        F, edge, nex, ney, uex, uey = [qsf(P, 2 * k) for k in range(6)]
        Wd, Hd = SF.from_u32(S, P.w), SF.from_u32(S, P.h)
        cx = Wd * 0.5 + 0.5
        cy = Hd * 0.5 + 1.0
        xc = SF.from_u32(S, S.gx) + 0.5
        yc = SF.from_u32(S, S.gy) + 0.5
        dx = SF.sel(S, S.ne(P.ccw, 0), cx - xc, xc - cx)
        dy = yc - cy
        k180 = 180.0 / 3.141592653589793
        a0 = sf_atan2(S, dx, dy.neg()) * k180
        a = SF.sel(S, a0 < 0.0, a0 + 360.0, a0)
        soft = sf_smooth01(S, (edge - a) / F + 0.5)
        hard = SF.sel(S, a < edge, SF.const(S, 1.0), SF.const(S, 0.0))
        d0 = dx + 0.5
        d1 = dx * nex + dy * ney - 0.71
        w0 = SF.sel(S, d0.abs() < 1.0, sf_clamp(S, dy.neg() - 0.5, 0.0, 1.0), SF.const(S, 0.0))
        w1 = SF.sel(S, d1.abs() < 1.0, sf_clamp(S, dx * uex + dy * uey - 0.5, 0.0, 1.0), SF.const(S, 0.0))
        t0 = hard + (sf_clamp(S, d0 + 0.5, 0.0, 1.0) - hard) * w0
        t1 = hard + (sf_clamp(S, d1 + 0.5, 0.0, 1.0) - hard) * w1
        both = SF.sel(S, edge <= 180.0, t0.min(t1), t0.max(t1))
        h = SF.sel(S, (w0 > 0.0) & (w1 > 0.0), both, SF.sel(S, w0 > 0.0, t0, SF.sel(S, w1 > 0.0, t1, hard)))
        zero, one = SF.const(S, 0.0), SF.const(S, 1.0)
        t = SF.sel(S, S.eq(P.mode, 0), soft, SF.sel(S, S.eq(P.mode, 1), zero, SF.sel(S, S.eq(P.mode, 2), one, h)))
        S.buf('out').store(i, sf_mixpx(S, S.buf('in0').load(i), to_px(S, i), t))


# ---------------------------------------------------------------------------------------------------------- VEGAS's blit and border library on 8-bit pictures (sf:: in builtins_trans.cpp)
# Push, Swap, Squeeze, Zoom, Slide, Spiral, Split and Barn Door draw a few rectangles (blit) and borders on a cleared 8-bit picture. The planner records those
# calls (transrec.h) and each becomes one pass over the whole picture: it reads the drawing so far (in1, raw levels as floats) and writes it back with the
# call's pixels changed; the last pass writes level / 255 (final). The source of a blit is in0 (a picture: levels as byte / 255). The numbers of a call come
# in the table as the bits of ints, in the order the planner (vktrans.cpp) and transrec.h's blit record put them.
BLIT_FIELDS = ['px0', 'px1', 'py0', 'py1', 'frc', 'lrc', 'fcc', 'lcc', 'sx0', 'sy0', 'sx', 'sy', 'fs', 'last', 'srow', 'xfirst', 'xlast', 'ylast', 'box', 'scx', 'scy',
               'wsF', 'wsL', 'hsF', 'hsL'] + ['a2_%d' % k for k in range(9)] + ['boxarea', 'invh', 'invl', 'srcw', 'srch']


class TI:
    """the ints of a call's table by name"""
    def __init__(self, S, names, base=0):
        self.S, self.names, self.base = S, names, base
        self.cache = {}
        for k, n in enumerate(names):          # (all read at once, before any branch: a value first read inside one would not be defined in the others)
            self.cache[n] = S.to_i(S.f_to_bits(S.buf('table').load(base + k)))
    def __getattr__(self, n):
        if n in ('S', 'names', 'base', 'cache'): raise AttributeError(n)
        return self.cache[n]


def lvl4(S, p):
    """toBytesTop: a picture value (premultiplied float) as its level: finite, cut to 0..1, v * 255 rounded (roundNonNeg); four ints"""
    out = []
    for k in range(4):
        v = p[k]
        v = S.select(S.isfinite(v), S.min(S.max(v, 0.0), 1.0), S.f(0.0))
        x = v * 255.0
        i = S.to_i(x)
        out.append(i + S.select((x - S.to_f(i)) >= 0.5, S.i(1), S.i(0)))
    return out


def mix8(S, p, q, t):
    """page::mix8: p + ((m + (m >> 8)) >> 8) with m = (q - p) * t + 128, cut to a byte"""
    m = (q - p) * t + 128
    d = p + ((m + (m >> 8)) >> 8)
    return S.max(S.min(d, S.i(255)), S.i(0))


def bil4(S, rows, i0, i1, fx, fy, W):
    """sf::bil: rows = (r0, r1) row numbers; 14-bit weights, four ints"""
    u = lambda v: S.to_u(v)
    a = S.i(0x4000)
    w00 = u((a - fy) * (a - fx)) >> 4
    w10 = u((a - fx) * fy) >> 4
    w01 = u((a - fy) * fx) >> 4
    w11 = u(fx * fy) >> 4
    r0, r1 = rows
    out = []
    ps = {}
    for key, (r, i) in dict(a=(r0, i0), b=(r1, i0), c=(r0, i1), d=(r1, i1)).items():
        ps[key] = S.buf('in0').load(r * W + i)
    L = {k: [u(x) for x in lvl4(S, v)] for k, v in ps.items()}
    for k in range(4):
        s = L['a'][k] * w00 + L['b'][k] * w10 + L['c'][k] * w01 + L['d'][k] * w11 + S.u(0x800000)
        out.append(S.to_i(s >> 24))
    return out


def bil_sse(S, rows, i0, i1, fx, fy, W):
    """sf::bilSse"""
    A = S.to_u((S.i(0x3fff) - fy) << 2) & 0xffff
    F = S.to_u(fy << 2) & 0xffff
    a = S.to_u(S.i(0x3fff) - fx)
    fxu = S.to_u(fx)
    w00 = S.to_i((a * A) >> 16); w01 = S.to_i((fxu * A) >> 16); w10 = S.to_i((a * F) >> 16); w11 = S.to_i((fxu * F) >> 16)
    r0, r1 = rows
    P = lambda r, i: lvl4(S, S.buf('in0').load(r * W + i))
    p10, p11, p01, p00 = P(r1, i0), P(r1, i1), P(r0, i1), P(r0, i0)
    mh = lambda v, w: ((v << 5) * w) >> 16
    out = []
    for k in range(4):
        s = S.min(mh(p10[k], w10) + 4, S.i(65535))
        s = S.min(s + mh(p11[k], w11), S.i(65535))
        t = S.min(mh(p01[k], w01) + mh(p00[k], w00), S.i(65535))
        s = S.min(s + t, S.i(65535))
        out.append(S.min(s >> 3, S.i(255)))
    return out


@kernel("table_to_buf", WH, [PIC_OUT, TABLE])
def table_to_buf(S):
    with S.in_range(S.p.w, S.p.h):
        i = S.gy * S.p.w + S.gx
        S.buf('out').store(i, to_px(S, i))


@kernel("blit8", WH + [('first', 'u'), ('final', 'u')], [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def blit8(S):
    P = S.p
    with S.in_range(P.w, P.h):
        t = TI(S, BLIT_FIELDS)
        W = P.w
        c = S.to_i(S.gx)
        row = S.to_i(S.gy)
        i = S.gy * W + S.gx
        st = S.select(S.ne(P.first, 0), S.splat4(0.0), S.buf('in1').load(i))
        state = [S.to_i(S.floor(st[k] + 0.5)) for k in range(4)]            # (raw levels held as floats)
        res = [S.var('i', 0) for _ in range(4)]
        for k in range(4):
            res[k].set(state[k])
        ncols = t.px1 - t.px0
        nrows = t.py1 - t.py0
        cj = c - t.px0
        ri = row - t.py0
        inside = (c >= t.px0) & (c < t.px1) & (row >= t.py0) & (row < t.py1)
        W_ = S.u(0)
        def src_rows(y):
            r = y / 16384
            fy = y % 16384
            r1 = r + S.select(S.ne(r, t.srow - 1), S.i(1), S.i(0))
            return (S.to_u(r), S.to_u(r1)), fy
        def col_of(x):
            i0 = x / 16384
            fx = x % 16384
            i1 = i0 + S.select(S.ne(i0, t.last), S.i(1), S.i(0))
            return i0, i1, fx
        def mix_into(p, alpha):
            a = alpha & 255
            for k in range(4):
                res[k].set(mix8(S, state[k], p[k], a))
        with S.if_(inside):
            with S.if_(S.ne(t.box, 0)):
                # sf::boxSmall: the box of source pixels this destination pixel covers
                last_c = ncols - 1
                last_r = nrows - 1
                covx = S.select(S.eq(cj, 0), t.fcc, S.select((S.eq(cj, last_c)), t.lcc, S.i(0x4000)))
                covy = S.select(S.eq(ri, 0), t.frc, S.select((S.eq(ri, last_r)), t.lrc, S.i(0x4000)))
                ws = S.select(S.eq(cj, 0), t.wsF, S.select(S.eq(cj, last_c), t.wsL, t.scx))
                hs = S.select(S.eq(ri, 0), t.hsF, S.select(S.eq(ri, last_r), t.hsL, t.scy))
                xsrc = S.select(S.eq(cj, 0), t.sx0, t.sx0 + t.wsF + (cj - 1) * t.scx)
                ysrc = S.select(S.eq(ri, 0), t.sy0, t.sy0 + t.hsF + (ri - 1) * t.scy)
                row0 = ysrc >> 14
                rowEnd = (((ysrc + hs - 1) | 0x3fff) + 1) >> 14
                nRows = rowEnd - row0
                wyL = hs + ysrc - ((rowEnd - 1) << 14)
                wy0 = S.select(nRows >= 2, ((row0 + 1) << 14) - ysrc, hs)
                col0 = xsrc >> 14
                colEnd = (((xsrc + ws - 1) | 0x3fff) + 1) >> 14
                nCols = colEnd - col0
                wxL = ws + xsrc - ((colEnd - 1) << 14)
                wx0 = S.select(nCols >= 2, ((col0 + 1) << 14) - xsrc, ws)
                sums = [S.var('i', 0) for _ in range(4)]
                def px(x, y):
                    xx = S.max(S.min(x, t.srcw - 1), S.i(0))
                    yy = S.max(S.min(y, t.srch - 1), S.i(0))
                    return lvl4(S, S.buf('in0').load(S.to_u(yy) * W + S.to_u(xx)))
                def add(x, y, w):
                    q = px(x, y)
                    for k in range(4):
                        sums[k].set(sums[k].get() + q[k] * w)
                # (dtoi(a * b * k14) of two ints below 2^15 / 2^17: the product cut by 14 bits; the weights are worked out in 64 bits where they could be large)
                def prod14(a, b):
                    return S.to_i(mulshr14(S, a, b))
                with S.for_(row0 + 1, rowEnd - 1, 1, True) as r:
                    with S.for_(col0 + 1, colEnd - 1, 1, True) as cc:
                        q = px(cc, r)
                        for k in range(4):
                            sums[k].set(sums[k].get() + (q[k] << 14))
                add(col0, row0, prod14(wx0, wy0))
                with S.if_(nCols > 1):
                    add(colEnd - 1, row0, prod14(wxL, wy0))
                with S.if_(nRows > 1):
                    add(col0, rowEnd - 1, prod14(wyL, wx0))
                    with S.if_(nCols > 1):
                        add(colEnd - 1, rowEnd - 1, prod14(wxL, wyL))
                with S.for_(col0 + 1, colEnd - 1, 1, True) as cc:
                    add(cc, row0, wy0)
                    with S.if_(nRows > 1):
                        add(cc, rowEnd - 1, wyL)
                with S.for_(row0 + 1, rowEnd - 1, 1, True) as r:
                    add(col0, r, wx0)
                    with S.if_(nCols > 1):
                        add(colEnd - 1, r, wxL)
                full = (S.eq(covx, 0x4000)) & (S.eq(covy, 0x4000))
                with S.if_(full):
                    inv = SF(S, S.to_u(t.invh), S.to_u(t.invl))
                    for k in range(4):
                        v = SF.from_i32(S, sums[k].get()) * inv + 1.5e-8 + 6755399441055744.0
                        res[k].set(S.to_i(v.lo & 255))
                with S.else_():
                    cxi = S.select(S.eq(cj, 0), S.i(0), S.select(S.eq(cj, last_c), S.i(2), S.i(1)))
                    cyi = S.select(S.eq(ri, 0), S.i(0), S.select(S.eq(ri, last_r), S.i(2), S.i(1)))
                    a2 = S.var('i', 0)
                    for yy in range(3):
                        for xx in range(3):
                            with S.if_(S.eq(cxi, xx) & S.eq(cyi, yy)):
                                a2.set(getattr(t, 'a2_%d' % (yy * 3 + xx)))
                    with S.if_(a2.get() > 0):
                        alpha14 = S.to_i(S.to_u(covx * covy) >> 14)
                        alpha8 = ((alpha14 * 255) >> 14) & 255
                        for k in range(4):
                            mixed = mix8(S, state[k], sums[k].get() / S.max(a2.get(), S.i(1)) & 255, alpha8)
                            res[k].set(mixed)
            with S.else_():
                is_edge = S.eq(ri, 0) | S.eq(ri, nrows - 1)
                y_first = t.sy0
                y_last = (nrows - 2) * t.sy + t.fs + t.sy0 + t.ylast
                y_mid = (ri - 1) * t.sy + t.fs + t.sy0
                y = S.select(S.eq(ri, 0), y_first, S.select(S.eq(ri, nrows - 1), y_last, y_mid))
                rcov = S.select(S.eq(ri, 0), t.frc, t.lrc)
                rows, fy = src_rows(y)
                with S.if_(is_edge):
                    # edgeRow: every pixel is mixed in by its coverage
                    with S.if_(ncols > 1):
                        x_first = t.sx0
                        x_mid = t.sx0 + t.xfirst + (cj - 1) * t.sx
                        x_last = t.sx0 + t.xfirst + (ncols - 2) * t.sx + t.xlast
                        x = S.select(S.eq(cj, 0), x_first, S.select(S.eq(cj, ncols - 1), x_last, x_mid))
                        i0, i1, fx = col_of(x)
                        p = bil4(S, rows, S.to_u(i0), S.to_u(i1), fx, fy, W)
                        a_first = ((rcov * t.fcc) >> 14) * 255 >> 14
                        a_mid = (rcov * 255) >> 14
                        a_last = (((t.lcc * rcov) >> 14) * 255) >> 14
                        alpha = S.select(S.eq(cj, 0), a_first, S.select(S.eq(cj, ncols - 1), a_last, a_mid))
                        mix_into(p, alpha)
                with S.else_():
                    # midRow
                    nm = ncols - 2
                    x_first = t.sx0
                    k = cj - 1
                    x_mid = t.sx0 + t.xfirst + k * t.sx
                    x_last = t.sx0 + t.xfirst + nm * t.sx + t.xlast
                    with S.if_(S.eq(cj, 0)):
                        i0, i1, fx = col_of(x_first)
                        p = bil4(S, rows, S.to_u(i0), S.to_u(i1), fx, fy, W)
                        mix_into(p, (t.fcc * 255) >> 14)
                    with S.else_():
                        with S.if_(cj < ncols - 1):
                            odd_tail = S.ne(nm & 1, 0) & S.eq(k, nm - 1)
                            j0 = x_mid >> 14
                            f = x_mid & 0x3fff
                            j1 = j0 + S.select(S.ne(j0, t.last), S.i(1), S.i(0))
                            ps = bil_sse(S, rows, S.to_u(j0), S.to_u(j1), f, fy, W)
                            i0, i1, fx = col_of(x_mid)
                            pb = bil4(S, rows, S.to_u(i0), S.to_u(i1), fx, fy, W)
                            for q in range(4):
                                res[q].set(S.select(odd_tail, pb[q], ps[q]))
                        with S.else_():
                            xx = x_last
                            i0 = xx / 16384
                            fx = xx % 16384
                            r0, r1 = rows
                            same = S.eq(i0, t.last)
                            a0 = lvl4(S, S.buf('in0').load(r0 * W + S.to_u(i0)))
                            a1 = lvl4(S, S.buf('in0').load(r1 * W + S.to_u(i0)))
                            p_same = [S.to_i((S.to_u(a0[q]) * S.to_u(0x4000 - fy) + S.to_u(a1[q]) * S.to_u(fy) + S.u(0x2000)) >> 14) for q in range(4)]
                            i1 = i0 + 1
                            p_bil = bil4(S, rows, S.to_u(i0), S.to_u(i1), fx, fy, W)
                            p = [S.select(same, p_same[q], p_bil[q]) for q in range(4)]
                            mix_into(p, (t.lcc * 255) >> 14)
        fin = S.ne(P.final, 0)
        sc = S.select(fin, S.f(1.0 / 255.0), S.f(1.0))
        S.buf('out').store(i, S.vec4(S.to_f(res[0].get()) * sc, S.to_f(res[1].get()) * sc, S.to_f(res[2].get()) * sc, S.to_f(res[3].get()) * sc))


def mulshr14(S, a, b):
    """(a * b) >> 14 for ints with a product that may not fit 32 bits (an unsigned 64-bit product by 16-bit halves), a, b >= 0"""
    from sf64 import mul32
    hi, lo = mul32(S, S.to_u(a), S.to_u(b))
    return (hi << 18) | (lo >> 14)


BORDER_FIELDS = ['x0', 'y0', 'x1', 'y1', 'iys', 'iye', 'ixs', 'ixe', 'total', 'bsize', 'feather', 'ext', 'c0', 'c1', 'c2', 'c3', 'nedges'] + \
    ['e%d_%s' % (e, f) for e in range(4) for f in ('a0', 'a1', 'pos', 'flags')]


@kernel("border8", WH + [('first', 'u'), ('final', 'u')], [PIC_IN1, PIC_OUT, TABLE])
def border8(S):
    # sf::border: the table holds the call's ints, then the 1024 bytes of the smoothstep table (as floats) from index 33
    P = S.p
    with S.in_range(P.w, P.h):
        t = TI(S, BORDER_FIELDS)
        W = P.w
        c = S.to_i(S.gx)
        row = S.to_i(S.gy)
        i = S.gy * W + S.gx
        st = S.select(S.ne(P.first, 0), S.splat4(0.0), S.buf('in1').load(i))
        state = [S.to_i(S.floor(st[k] + 0.5)) for k in range(4)]
        res = [S.var('i', 0) for _ in range(4)]
        for k in range(4):
            res[k].set(state[k])
        inside = (c >= t.x0) & (c < t.x1) & (row >= t.y0) & (row < t.y1)
        inner = (t.iys < row) & (row < t.iye) & (t.ixs < c) & (c < t.ixe)
        with S.if_(inside & ~inner):
            Y = (row << 14) + 0x2000
            X = (c << 14) + 0x2000
            dout = S.var('i', t.total)
            din = S.var('i', t.feather)
            found = S.var('i', 0)
            for e in range(4):
                a0, a1, pos, fl = [getattr(t, 'e%d_%s' % (e, f)) for f in ('a0', 'a1', 'pos', 'flags')]
                vert = S.ne(fl & 1, 0)
                cond = S.select(vert, (a0 <= Y) & (Y <= a1), (a0 <= X) & (X <= a1)) & (t.nedges > e)
                far = S.ne(fl & 2, 0)
                d = S.select(vert, S.select(far, X - pos, pos - X), S.select(far, Y - pos, pos - Y))
                with S.if_(cond):
                    with S.if_(d >= 0):
                        with S.if_(d < dout.get()):
                            dout.set(d)
                            found.set(S.i(1))
                    with S.else_():
                        with S.if_((S.i(0) - d) < din.get()):
                            din.set(S.i(0) - d)
                            found.set(S.i(1))
            with S.if_(S.eq(found.get(), 0)):
                for e in range(4):
                    a0, a1, pos, fl = [getattr(t, 'e%d_%s' % (e, f)) for f in ('a0', 'a1', 'pos', 'flags')]
                    do = dout.get()
                    ea = a0 - X
                    ea = S.select(ea < 0, do, ea)
                    ec = X - a1
                    ec = S.select(ec < 0, do, ec)
                    dx = S.min(ea, ec)
                    dy = S.select(S.ne(fl & 2, 0), Y - pos, pos - Y)
                    ok = (t.nedges > e) & S.eq(fl & 1, 0) & ~((dx < 0) | (dx >= do)) & ~((dy <= 0) | (dy >= do))
                    with S.if_(ok):
                        dxs, dys = SF.from_i32(S, dx), SF.from_i32(S, dy)
                        dd = ((dxs * dxs + dys * dys).sqrt() + 0.5).trunc_i32()
                        with S.if_(dd < do):
                            dout.set(dd)
                            found.set(S.i(1))
            with S.if_(S.ne(found.get(), 0)):
                do = S.min(dout.get() + t.ext, t.total)
                di = S.min(din.get() + t.ext, t.feather)
                col = [t.c0, t.c1, t.c2, t.c3]
                with S.if_(do < t.bsize):
                    for k in range(4):
                        res[k].set(col[k])
                with S.else_():
                    al = S.var('i', 0)
                    solid = S.var('i', 0)
                    with S.if_(S.ne(t.feather, 0)):
                        dd = S.min(di, do - t.bsize)
                        q = (SF.from_i32(S, dd) * 1023.0 / SF.from_i32(S, t.feather)).trunc_i32()
                        idx = S.max(S.min(q, S.i(1023)), S.i(0))
                        tabv = S.to_i(S.buf('table').load(S.to_u(idx) + 33))
                        a = (~tabv) & 255
                        al.set(a)
                        with S.if_(S.eq(a, 255)):
                            solid.set(S.i(1))
                    with S.if_(S.ne(solid.get(), 0)):
                        for k in range(4):
                            res[k].set(col[k])
                    with S.else_():
                        for k in range(4):
                            v = (col[k] - state[k]) * al.get()
                            v = v + S.select(v < 0, S.i(255), S.i(0))
                            res[k].set(state[k] + (v >> 8))
        fin = S.ne(P.final, 0)
        sc = S.select(fin, S.f(1.0 / 255.0), S.f(1.0))
        S.buf('out').store(i, S.vec4(S.to_f(res[0].get()) * sc, S.to_f(res[1].get()) * sc, S.to_f(res[2].get()) * sc, S.to_f(res[3].get()) * sc))


# ---------------------------------------------------------------------------------------------------------- float pictures: Push, Swap, Squeeze, Zoom, Barn Door (positions in doubles, Buf::sample)
def sf_sample(S, load, w, h, x, y):
    """Buf::sample(x, y) (all four taps inside or the edge version: the same sums in the same order): position doubles (SF), bilinear weights in floats,
    taps outside the picture left out, nothing at all more than a pixel out"""
    wf, hf = SF.from_u32(S, w), SF.from_u32(S, h)
    xs, ys = x - 0.5, y - 0.5
    near = (xs >= -1.0) & (ys >= -1.0) & (xs <= wf) & (ys <= hf)
    x0, y0 = xs.floor_i32(), ys.floor_i32()
    fx = (xs - SF.from_i32(S, x0)).to_f32()
    fy = (ys - SF.from_i32(S, y0)).to_f32()
    wi, hi = S.to_i(w), S.to_i(h)
    acc = S.splat4(0.0)
    for j in (0, 1):
        for i in (0, 1):
            xx, yy = x0 + i, y0 + j
            inside = near & (xx >= 0) & (yy >= 0) & (xx < wi) & (yy < hi)
            wgt = (fx if i else 1.0 - fx) * (fy if j else 1.0 - fy)
            idx = S.to_u(S.max(S.min(yy, hi - 1), S.i(0))) * w + S.to_u(S.max(S.min(xx, wi - 1), S.i(0)))
            acc = S.select(inside, acc + load(idx) * wgt, acc)
    return acc


def sf_sample_clamped(S, load, w, h, x, y):
    wf, hf = SF.from_u32(S, w), SF.from_u32(S, h)
    cx = SF.sel(S, x < 0.5, SF.const(S, 0.5), SF.sel(S, x > wf - 0.5, wf - 0.5, x))
    cy = SF.sel(S, y < 0.5, SF.const(S, 0.5), SF.sel(S, y > hf - 0.5, hf - 0.5, y))
    return sf_sample(S, load, w, h, cx, cy)


def sf_ring(S, o, d, R, Wd, bc):
    """mixPx(o, bc, smooth01((R - |d|) / Wd + 0.5))"""
    return sf_mixpx(S, o, bc, sf_smooth01(S, (R - d.abs()) / Wd + 0.5))


def bcvec(S, P, k):
    return S.vec4(qf(S, P, k), qf(S, P, k + 1), qf(S, P, k + 2), qf(S, P, k + 3))


def load_a(S):
    return lambda k: S.buf('in0').load(k)


def load_b(S):
    return lambda k: to_px(S, k)


@kernel("push_f", WH + [('horiz', 'u'), ('neg', 'u'), ('pushoff', 'u'), ('border', 'u')] + QW(18), [PIC_IN, PIC_OUT, TABLE])
def push_f(S):
    # pushFx() on float pictures: doubles q0 seam, q2 centre, q4 shift, q6 L, q8 R, q10 Wd, q12 early; floats q14..q17 the border colour
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        seam, centre, shift, L, R, Wd, early = [qsf(P, 2 * k) for k in range(7)]
        bc = bcvec(S, P, 14)
        horiz, neg, border = S.ne(P.horiz, 0), S.ne(P.neg, 0), S.ne(P.border, 0)
        xc = SF.from_u32(S, S.gx) + 0.5
        yc = SF.from_u32(S, S.gy) + 0.5
        a = SF.sel(S, horiz, xc, yc)
        inb = S.select(neg, a >= seam - early, a < seam)
        ib = SF.sel(S, neg, a - seam + early, a - seam + L)
        fa = SF.sel(S, S.ne(P.pushoff, 0), SF.sel(S, neg, a + shift, a - shift), a)
        res = S.var('v4')
        with S.if_(inb):
            res.set(sf_sample(S, load_b(S), P.w, P.h, SF.sel(S, horiz, ib, xc), SF.sel(S, horiz, yc, ib)))
        with S.else_():
            res.set(sf_sample(S, load_a(S), P.w, P.h, SF.sel(S, horiz, fa, xc), SF.sel(S, horiz, yc, fa)))
        o = res.get()
        o = S.select(border, sf_ring(S, o, a - centre, R, Wd, bc), o)
        S.buf('out').store(i, o)


@kernel("swap_f", WH + [('horiz', 'u'), ('flip', 'u'), ('atop', 'u'), ('border', 'u')] + QW(18), [PIC_IN, PIC_OUT, TABLE])
def swap_f(S):
    # swapFx() on float pictures: doubles q0 L, q2 Oi, q4 cen, q6 R, q8 Wd; floats q14..q17 the border colour
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        L, Oi, cen, R, Wd = [qsf(P, 2 * k) for k in range(5)]
        bc = bcvec(S, P, 14)
        horiz, flip, atop, border = S.ne(P.horiz, 0), S.ne(P.flip, 0), S.ne(P.atop, 0), S.ne(P.border, 0)
        xc = SF.from_u32(S, S.gx) + 0.5
        yc = SF.from_u32(S, S.gy) + 0.5
        pos0 = SF.sel(S, horiz, xc, yc)
        pos = SF.sel(S, flip, L - pos0, pos0)
        sa, sb = pos + Oi, pos - Oi
        zero = SF.const(S, 0.0)
        ina = (sa >= zero) & (sa < L)
        inb = (sb >= zero) & (sb < L)
        qa = SF.sel(S, flip, L - sa, sa)
        qb = SF.sel(S, flip, L - sb, sb)
        pa = sf_sample(S, load_a(S), P.w, P.h, SF.sel(S, horiz, qa, xc), SF.sel(S, horiz, yc, qa))
        pb = sf_sample(S, load_b(S), P.w, P.h, SF.sel(S, horiz, qb, xc), SF.sel(S, horiz, yc, qb))
        first_a = S.select(atop, ina, inb)             # the top picture first
        o = S.select(atop, S.select(ina, pa, S.select(inb, pb, bc)), S.select(inb, pb, S.select(ina, pa, bc)))
        o = S.select(border, sf_ring(S, o, pos - cen, R, Wd, bc), o)
        S.buf('out').store(i, o)


@kernel("squeeze_f", WH + [('alongy', 'u'), ('mid', 'u'), ('far', 'u'), ('prev', 'u'), ('border', 'u')] + QW(20), [PIC_IN, PIC_OUT, TABLE])
def squeeze_f(S):
    # squeezeFx() on float pictures: doubles q0 L, q2 w, q4 sB, q6 aw, q8 cen, q10 R, q12 Wd; floats q14..q17 the border colour
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        L, w, sB, aw, cen, R, Wd = [qsf(P, 2 * k) for k in range(7)]
        bc = bcvec(S, P, 14)
        alongy, mid, far, prev, border = [S.ne(getattr(P, n), 0) for n in ('alongy', 'mid', 'far', 'prev', 'border')]
        xc = SF.from_u32(S, S.gx) + 0.5
        yc = SF.from_u32(S, S.gy) + 0.5
        pos = SF.sel(S, alongy, yc, xc)
        half = L * 0.5
        right = pos > half
        s = SF.sel(S, mid, (pos - half).abs(), SF.sel(S, far, L - pos, pos))
        inb = s < sB
        one = SF.const(S, 1.0)
        sbm = SF.sel(S, sB < 1e-6, SF.const(S, 1e-6), sB)
        f1 = s / sbm
        t_in = SF.sel(S, mid, half + SF.sel(S, right, f1, f1.neg()) * half, SF.sel(S, far, L - f1 * L, f1 * L))
        awm = SF.sel(S, aw < 1e-6, SF.const(S, 1e-6), aw)
        f2 = (s - sB - w) / awm
        t_out = SF.sel(S, mid, SF.sel(S, right, half + f2 * half, half - f2 * half), SF.sel(S, far, (one - f2) * L, f2 * L))
        t = SF.sel(S, inb, t_in, SF.sel(S, prev, t_out, pos))
        pb = sf_sample(S, load_b(S), P.w, P.h, SF.sel(S, alongy, xc, t), SF.sel(S, alongy, t, yc))
        pa = sf_sample(S, load_a(S), P.w, P.h, SF.sel(S, alongy, xc, t), SF.sel(S, alongy, t, yc))
        o = S.select(inb, pb, S.select(s < sB + w, bc, pa))
        o = S.select(border, sf_ring(S, o, s - cen, R, Wd, bc), o)
        S.buf('out').store(i, o)


@kernel("barndoor_f", WH + [('along', 'u'), ('inward', 'u'), ('border', 'u'), ('onec', 'u'), ('onef', 'u')] + QW(18), [PIC_IN, PIC_OUT, TABLE])
def barndoor_f(S):
    # barndoor() on float pictures: doubles q0 L, q2 T, q4 e, q6 cen, q8 R, q10 Wd; floats q14..q17 the border colour
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        L, T, e, cen, R, Wd = [qsf(P, 2 * k) for k in range(6)]
        bc = bcvec(S, P, 14)
        alongx, border = S.ne(P.along, 0), S.ne(P.border, 0)
        xc = SF.from_u32(S, S.gx) + 0.5
        yc = SF.from_u32(S, S.gy) + 0.5
        a = SF.sel(S, alongx, xc, yc)
        far = a > T
        am = SF.sel(S, far, L - a, a)
        moving = am < e
        onef = SF.sel(S, S.ne(P.onef, 0), SF.const(S, 1.0), SF.const(S, 0.0))
        onec = SF.sel(S, S.ne(P.onec, 0), SF.const(S, 1.0), SF.const(S, 0.0))
        one = SF.sel(S, border, SF.const(S, 1.0), SF.const(S, 0.0))
        sa0 = SF.sel(S, far, a - (T - e) + onef, a + (T - e) - onec)
        sa0 = sf_clamp(S, sa0, 0.5, L - 0.5)
        sa = SF.sel(S, moving, sa0, a)
        frompic = S.select(S.ne(P.inward, 0), ~moving, moving)
        sx, sy = SF.sel(S, alongx, sa, xc), SF.sel(S, alongx, yc, sa)
        res = S.var('v4')
        with S.if_(frompic):
            res.set(sf_sample(S, load_a(S), P.w, P.h, sx, sy))
        with S.else_():
            res.set(sf_sample(S, load_b(S), P.w, P.h, sx, sy))
        o = res.get()
        o = S.select(border, sf_ring(S, o, am - (cen - one), R, Wd, bc), o)
        S.buf('out').store(i, o)



class SFVar:
    """a double kept in two uint variables (for sums in a loop)"""
    def __init__(self, S, init=0.0):
        self.S = S
        self.hv, self.lv = S.var('u'), S.var('u')
        self.set(SF.of(S, init))

    def set(self, v):
        self.hv.set(v.hi)
        self.lv.set(v.lo)

    def get(self):
        return SF(self.S, self.hv.get(), self.lv.get())


def sf_boxat(S, load, w, h, x0, x1, y0, y1):
    """boxAt() of builtins_trans.cpp: the average of the box [x0, x1) x [y0, y1) in pixels (partial pixels by coverage, edges extended), sums in doubles"""
    res = S.var('v4')
    res.set(S.splat4(0.0))
    with S.if_((x1 > x0) & (y1 > y0)):
        ix0, iy0 = x0.floor_i32(), y0.floor_i32()
        ix1 = S.i(0) - x1.neg().floor_i32()                     # ceil
        iy1 = S.i(0) - y1.neg().floor_i32()
        wi, hi = S.to_i(w), S.to_i(h)
        n = SF.from_i32(S, ix1 - ix0) * SF.from_i32(S, iy1 - iy0)
        with S.if_(~(n > 1e6)):
            acc = [SFVar(S) for _ in range(4)]
            tot = SFVar(S)
            with S.for_(iy0, iy1, 1, True) as y:
                yf = SF.from_i32(S, y)
                top = yf + 1.0
                wy = SF.sel(S, y1 < top, y1, top) - SF.sel(S, yf < y0, y0, yf)
                with S.if_(wy > 0.0):
                    yy = S.min(hi - 1, S.max(S.i(0), y))
                    with S.for_(ix0, ix1, 1, True) as x:
                        xf = SF.from_i32(S, x)
                        rgt = xf + 1.0
                        wx = SF.sel(S, x1 < rgt, x1, rgt) - SF.sel(S, xf < x0, x0, xf)
                        with S.if_(wx > 0.0):
                            xx = S.min(wi - 1, S.max(S.i(0), x))
                            q = load(S.to_u(yy) * w + S.to_u(xx))
                            wgt = wx * wy
                            for k in range(4):
                                acc[k].set(acc[k].get() + SF.from_f32(S, q[k]) * wgt)
                            tot.set(tot.get() + wgt)
            with S.if_(tot.get() > 0.0):
                t = tot.get()
                res.set(S.vec4(*[(acc[k].get() / t).to_f32() for k in range(4)]))
    return res.get()


@kernel("zoom_f", WH + [('out', 'u'), ('border', 'u')] + QW(24), [PIC_IN, PIC_OUT, TABLE])
def zoom_f(S):
    # zoomFx() on float pictures: doubles q0 px0, q2 py0, q4 x0, q6 x1, q8 y0, q10 y1, q12 sp, q14 Wd, q16 R, q18 cen; floats q20..q23 the border colour
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        px0, py0, x0, x1, y0, y1, sp, Wd, R, cen = [qsf(P, 2 * k) for k in range(10)]
        bc = bcvec(S, P, 20)
        out, border = S.ne(P.out, 0), S.ne(P.border, 0)
        fx = SF.from_u32(S, S.gx) + 0.5
        fy = SF.from_u32(S, S.gy) + 0.5
        zero = SF.const(S, 0.0)
        def dmax(a, b): return SF.sel(S, a < b, b, a)                           # std::max
        def dmin(a, b): return SF.sel(S, b < a, b, a)                           # std::min
        cov = dmax(zero, dmin(fx + 0.5, x1) - dmax(fx - 0.5, x0)) * dmax(zero, dmin(fy + 0.5, y1) - dmax(fy - 0.5, y0))
        pic = S.var('v4')
        pic.set(S.splat4(0.0))
        base = S.var('v4')
        base.set(S.splat4(0.0))
        with S.if_(cov > 0.0):
            qx = (fx - px0) / sp + px0
            qy = (fy - py0) / sp + py0
            bx0, bx1 = (fx - 0.5 - px0) / sp + px0, (fx + 0.5 - px0) / sp + px0
            by0, by1 = (fy - 0.5 - py0) / sp + py0, (fy + 0.5 - py0) / sp + py0
            for ld, sel in ((load_a(S), out), (load_b(S), ~out)):
                with S.if_(sel):
                    with S.if_(sp >= 1.0):
                        pic.set(sf_sample_clamped(S, ld, P.w, P.h, qx, qy))
                    with S.else_():
                        pic.set(sf_boxat(S, ld, P.w, P.h, bx0, bx1, by0, by1))
        with S.if_(cov < 1.0):
            with S.if_(out):
                base.set(sf_sample(S, load_b(S), P.w, P.h, fx, fy))
            with S.else_():
                base.set(sf_sample(S, load_a(S), P.w, P.h, fx, fy))
        ex = dmax(x0 - fx, fx - x1)
        ey = dmax(y0 - fy, fy - y1)
        d = SF.sel(S, (ex > 0.0) & (ey > 0.0), (ex * ex + ey * ey).sqrt(), dmax(ex, ey))
        ring = sf_smooth01(S, (R - (d - cen).abs()) / Wd + 0.5)
        hasring = border & (ring > 0.0)
        o = S.var('v4')
        full = cov >= 1.0
        o.set(S.select(full, pic.get(), base.get()))
        with S.if_(hasring):
            o.set(sf_mixpx(S, o.get(), bc, ring))
        with S.if_(~full & (cov > 0.0)):
            o.set(sf_mixpx(S, o.get(), pic.get(), cov))
        S.buf('out').store(i, o.get())


@kernel("wipe_venetian_f", WH + QW(12), [PIC_IN, PIC_OUT, TABLE])
def wipe_venetian_f(S):
    # venetian() on float pictures (wipe() with the blinds' sawtooth): doubles q0 dx, q2 dy, q4 span, q6 n, q8 f, q10 edge
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        dx, dy, span, n, f, edge = [qsf(P, 2 * k) for k in range(6)]
        Wd, Hd = SF.from_u32(S, P.w), SF.from_u32(S, P.h)
        u = (SF.from_u32(S, S.gx) + 0.5) / Wd
        v = (SF.from_u32(S, S.gy) + 0.5) / Hd
        pos = ((u - 0.5) * dx + (v - 0.5) * dy) / span * n + 0.5
        sv = pos - SF.from_i32(S, pos.floor_i32())
        t = sf_smooth01(S, (edge - 0.0 - sv) / f)
        S.buf('out').store(i, sf_mixpx(S, S.buf('in0').load(i), to_px(S, i), t))


def portal_rnd(S, a, b, k, seed):
    """rnd() of portalsFx(): an integer hash of the cell, as a double in 0 .. 1"""
    h = a * S.u(374761393) + b * S.u(668265263) + S.u(k) * S.u(1013904223) + seed * S.u(1664525)
    h = (h ^ (h >> 13)) * S.u(1274126177)
    return SF.from_u32(S, (h ^ (h >> 16)) & 0xffff) / 65535.0


@kernel("portals_f", WH + [('n', 'u'), ('seed', 'u')] + QW(12), [PIC_IN, PIC_OUT, TABLE])
def portals_f(S):
    # portalsFx() on float pictures: doubles q0 p * 1.6, q2 maxT, q4 maxO, q6 bs (the border size, 0 for none); floats q8..q11 the border colour
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p16, maxT, maxO, bs = [qsf(P, 2 * k) for k in range(4)]
        bc = bcvec(S, P, 8)
        Wd, Hd = SF.from_u32(S, P.w), SF.from_u32(S, P.h)
        nd = SF.from_u32(S, P.n)
        xc = SF.from_u32(S, S.gx) + 0.5
        yc = SF.from_u32(S, S.gy) + 0.5
        u = xc / Wd * nd
        v = yc / Hd * nd
        gxi, gyi = u.floor_i32(), v.floor_i32()
        gxu, gyu = S.to_u(gxi), S.to_u(gyi)
        when = portal_rnd(S, gxu, gyu, 1, P.seed) * 0.6
        t = sf_clamp(S, (p16 - when) / 0.4, 0.0, 1.0)
        o = S.var('v4')
        o.set(S.buf('in0').load(i))
        with S.if_(t > 0.0):
            fx = u - SF.from_i32(S, gxi)
            fy = v - SF.from_i32(S, gyi)
            one_t = SF.const(S, 1.0) - t
            ox = (portal_rnd(S, gxu, gyu, 3, P.seed) - 0.5) * maxO * one_t * Wd * 0.1
            oy = (portal_rnd(S, gxu, gyu, 4, P.seed) - 0.5) * maxO * one_t * Hd * 0.1
            b = sf_sample(S, load_b(S), P.w, P.h, xc + ox, yc + oy)
            m = (SF.const(S, 1.0) - maxT * one_t) * sf_smooth01(S, t * 2.0)
            o.set(sf_mixpx(S, o.get(), b, m))
            e = bs * 0.2
            edge = (fx < e) | (fx > SF.const(S, 1.0) - e) | (fy < e) | (fy > SF.const(S, 1.0) - e)
            with S.if_((bs > 0.0) & (t < 1.0) & edge):
                o.set(sf_mixpx(S, o.get(), bc, SF.const(S, 0.8) * one_t))
        S.buf('out').store(i, o.get())


@kernel("blinds3d_f", WH + [('horiz', 'u'), ('n', 'u')], [PIC_IN, PIC_OUT, TABLE])
def blinds3d_f(S):
    # blindsLike() on float pictures (3D Blinds, 3D Cascade): the incoming picture first in the table, then for each strip three doubles (turn t,
    # |cos| of the turn, light * 0.3 * sin of the turn) worked out by the planner as the CPU does
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        horiz = S.ne(P.horiz, 0)
        tab = S.buf('table')
        Wd, Hd = SF.from_u32(S, P.w), SF.from_u32(S, P.h)
        u = (SF.from_u32(S, S.gx) + 0.5) / Wd
        v = (SF.from_u32(S, S.gy) + 0.5) / Hd
        nd = SF.from_u32(S, P.n)
        along = SF.sel(S, horiz, v, u)
        idx = S.min(S.to_i(P.n) - 1, (along * nd).trunc_i32())
        cell = along * nd - SF.from_i32(S, idx)
        base = P.w * P.h * 4 + S.to_u(idx) * 6
        t = ld_sf(S, tab, base)
        width = ld_sf(S, tab, base + 2)
        ks = ld_sf(S, tab, base + 4)
        mid = (SF.from_i32(S, idx) + 0.5) / nd
        Ld = SF.sel(S, horiz, Hd, Wd)
        a = (along - mid) * Ld
        wm = SF.sel(S, SF.const(S, 0.02) < width, width, SF.const(S, 0.02))     # std::max(0.02, width)
        a2 = a / wm
        limit = Ld * 0.5 / nd
        first = t < 0.5
        o = S.var('v4')
        o.set(S.vec4(S.f(0.0), S.f(0.0), S.f(0.0), S.f(1.0)))          # (colorPx gives alpha 1 whatever the colour's)
        with S.if_(a2.abs() <= limit):
            q = mid * Ld + a2
            qx = SF.sel(S, horiz, xc_(S), q)
            qy = SF.sel(S, horiz, q, yc_(S))
            with S.if_(first):
                o.set(sf_sample(S, load_a(S), P.w, P.h, qx, qy))
            with S.else_():
                o.set(sf_sample(S, load_b(S), P.w, P.h, qx, qy))
            shade = (S.f(1.0) - (SF.const(S, 0.55) * (SF.const(S, 1.0) - width)).to_f32()) + (ks * (cell - 0.5)).to_f32()
            p = o.get()
            def cl(k): return S.select(p.w < p[k] * shade, p.w, p[k] * shade)         # std::min(v * shade, a)
            o.set(S.vec4(cl(0), cl(1), cl(2), p.w))
        p = o.get()
        with S.if_(p.w < 1.0):
            und = S.var('v4')
            with S.if_(first):
                und.set(to_px(S, i))
            with S.else_():
                und.set(S.buf('in0').load(i))
            u4 = sf_mixpx(S, S.vec4(S.f(0.0), S.f(0.0), S.f(0.0), S.f(1.0)), und.get(), SF.const(S, 0.15))
            k = S.f(1.0) - p.w
            o.set(S.vec4(p.x + u4.x * k, p.y + u4.y * k, p.z + u4.z * k, p.w + u4.w * k))
        S.buf('out').store(i, o.get())


def xc_(S):
    return SF.from_u32(S, S.gx) + 0.5


def yc_(S):
    return SF.from_u32(S, S.gy) + 0.5


@kernel("fly3d_f", WH + [('out', 'u')] + QW(12), [PIC_IN, PIC_OUT, TABLE])
def fly3d_f(S):
    # fly3d() on float pictures: doubles q0 s, q2 cos, q4 sin, q6 ox, q8 oy (worked out by the planner as the CPU does)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s, ca, sa, ox, oy = [qsf(P, 2 * k) for k in range(5)]
        out = S.ne(P.out, 0)
        Wd, Hd = SF.from_u32(S, P.w), SF.from_u32(S, P.h)
        xc, yc = xc_(S), yc_(S)
        dx = (xc - Wd * 0.5 - ox) / s
        dy = (yc - Hd * 0.5 - oy) / s
        qx = dx * ca + dy * sa + Wd * 0.5
        qy = dx.neg() * sa + dy * ca + Hd * 0.5
        o = S.var('v4')
        f = S.var('v4')
        with S.if_(out):
            o.set(to_px(S, i))
            f.set(sf_sample(S, load_a(S), P.w, P.h, qx, qy))
        with S.else_():
            o.set(S.buf('in0').load(i))
            f.set(sf_sample(S, load_b(S), P.w, P.h, qx, qy))
        fv, ov = f.get(), o.get()
        k = S.f(1.0) - fv.w
        S.buf('out').store(i, S.vec4(fv.x * 1.0 + ov.x * k, fv.y * 1.0 + ov.y * k, fv.z * 1.0 + ov.z * k, fv.w * 1.0 + ov.w * k))


# ---------------------------------------------------------------------------------------------------------- Iris (irisFx in builtins_trans.cpp)
@kernel("iris_map", WH + [('nx', 'u'), ('ny', 'u'), ('cw', 'u'), ('ch', 'u'), ('shape', 'u'), ('window', 'u'), ('cxf', 'f'), ('cyf', 'f'), ('a', 'f')],
        [PIC_IN, PIC_OUT, TABLE])
def iris_map(S):
    # the distance of every pixel to the nearest iris centre in the shape's metric, row j counted from the bottom (kept in the red of map[j * w + x])
    import iris_dist_gen
    P = S.p
    with S.in_range(P.w, P.h):
        H = S.to_i(P.h)
        x = S.to_i(S.gx)
        j = H - 1 - S.to_i(S.gy)
        nx, ny, cw, ch = S.to_i(P.nx), S.to_i(P.ny), S.to_i(P.cw), S.to_i(P.ch)
        jf = S.to_f(j)
        iy = S.select(P.cyf > jf, S.i(-1), S.min(S.to_i(jf - P.cyf) / ch, ny - 1))
        ix = S.select(P.cxf > S.to_f(x), S.i(-1), S.min(S.to_i(S.to_f(x) - P.cxf) / cw, nx - 1))
        shape = P.shape
        s11 = S.eq(shape, 11)
        special = s11 & S.eq(ix, -1) & (iy >= 0) & (iy + 2 < ny)
        best = S.var('f')
        have = S.var('u')
        best.set(S.f(0.0))
        have.set(S.u(0))
        with S.for_(0, 18) as k:
            ki = S.to_i(k)
            dx = ki / 4 - 1
            dy = ki - (ki / 4) * 4 - 1
            kx = S.select(k < 16, ix + dx, S.select(S.eq(k, 16), S.i(1), S.max(ix, 0)))
            ky = S.select(k < 16, iy + dy, S.select(S.eq(k, 16), iy + 2, S.max(iy, 0)))
            inwin = S.ne((P.window >> S.select(k < 16, k, S.u(0))) & 1, 0)
            inside = (kx >= 0) & (kx < nx) & (ky >= 0) & (ky < ny)
            skip = special & S.eq(kx, 0) & S.eq(ky, iy + 2)
            win_ok = inwin & inside & ~skip
            extra16 = special & S.eq(nx, 1)
            doit = S.select(k < 16, win_ok, S.select(S.eq(k, 16), extra16, S.eq(have.get(), 0)))
            with S.if_(doit):
                d = iris_dist_gen.iris_distance(S, S.to_f(x), jf, S.to_f(kx * cw) + P.cxf, S.to_f(ky * ch) + P.cyf, shape, P.a, S.f(1.0), S.f(1.0))
                best.set(S.select(S.ne(have.get(), 0), S.select(best.get() < d, best.get(), d), d))
                have.set(S.u(1))
        S.buf('out').store(S.to_u(j) * P.w + S.gx, S.vec4(best.get(), S.f(0.0), S.f(0.0), S.f(0.0)))


@kernel("iris_max", WH, [PIC_IN, PIC_OUT, TABLE])
def iris_max(S):
    # the largest distance on the frame's four edges (at least 1), written after the map (in0 and out are the map); one invocation
    P = S.p
    with S.in_range(S.u(1), S.u(1)):
        mx = S.var('f')
        mx.set(S.f(1.0))
        src = S.buf('in0')
        with S.for_(0, P.h) as j:
            edge_row = S.eq(j, 0) | S.eq(j, P.h - 1)
            with S.if_(edge_row):
                with S.for_(0, P.w) as x:
                    v = src.load(j * P.w + x).x
                    mx.set(S.select(mx.get() < v, v, mx.get()))
            with S.else_():
                v0 = src.load(j * P.w).x
                mx.set(S.select(mx.get() < v0, v0, mx.get()))
                v1 = src.load(j * P.w + P.w - 1).x
                mx.set(S.select(mx.get() < v1, v1, mx.get()))
        S.buf('out').store(P.w * P.h, S.vec4(mx.get(), S.f(0.0), S.f(0.0), S.f(0.0)))


@kernel("iris_final", WH + [('mode', 'u'), ('swap', 'u'), ('bc0', 'u'), ('bc1', 'u'), ('bc2', 'u'), ('p', 'f'), ('f0', 'f'), ('f1', 'f'), ('f2', 'f'), ('f3', 'f'),
                              ('k1', 'f'), ('k2', 'f')], [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def iris_final(S):
    # the picture: in0 the outgoing frame, in1 the map (and the largest distance after it), the table the incoming frame. Modes: 0 hard edge, no border (p);
    # 1 soft edge (f0 lo, f1 hi, k1 = 1 / (hi - lo)); 2 border, hard edge (f0 lo, f1 hi as floats); 3 border, soft (f0 af, f1 lof, f2 hif, f3 bf, k1 = 1 / (bf - hif), k2 = 1 / (lof - af))
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        j = P.h - 1 - S.gy
        mp = S.buf('in1')
        mx = mp.load(P.w * P.h).x
        inv = S.f(1.0) / mx
        idx = j * P.w + S.gx
        idx = S.select(S.eq(idx, P.w), (P.h - 1) * P.w, S.select(S.eq(idx, P.w + P.w - 1), (P.h - 1) * P.w + P.w - 1, idx))
        d = inv * mp.load(idx).x
        a8 = lvl4(S, S.buf('in0').load(i))
        b8 = lvl4(S, to_px(S, i))
        sw = S.ne(P.swap, 0)
        S0 = [S.select(sw, b8[k], a8[k]) for k in range(4)]
        S1 = [S.select(sw, a8[k], b8[k]) for k in range(4)]
        bc = [S.to_i(P.bc0), S.to_i(P.bc1), S.to_i(P.bc2), S.i(255)]
        o = [S.var('i') for _ in range(4)]
        def copy(src):
            for k in range(4): o[k].set(src[k])
        def blend(sa, sb, t, u, alpha):
            for k in range(3): o[k].set(S.to_i(S.to_f(sa[k]) * u + S.to_f(sb[k]) * t))
            o[3].set(alpha)
        def tq(q):
            return S.select(S.f(0.0) > q, S.f(0.0), S.select(q < 1.0, q, S.f(1.0)))
        mode = P.mode
        with S.if_(S.eq(mode, 0)):
            copy([S.select(P.p > d, S1[k], S0[k]) for k in range(4)])
        with S.if_(S.eq(mode, 1)):
            lo, hi = P.f0, P.f1
            with S.if_(lo > d):
                copy(S1)
            with S.else_():
                with S.if_(d > hi):
                    copy(S0)
                with S.else_():
                    t = tq(P.k1 * (d - lo))
                    u = S.f(1.0) - t
                    blend(S1, S0, t, u, S.select(S.f(0.5) <= t, S0[3], S1[3]))
        with S.if_(S.eq(mode, 2)):
            with S.if_(P.f0 > d):
                copy(S1)
            with S.else_():
                with S.if_(P.f1 <= d):
                    copy(S0)
                with S.else_():
                    copy(bc)
        with S.if_(S.eq(mode, 3)):
            af, lof, hif, bf = P.f0, P.f1, P.f2, P.f3
            with S.if_(af > d):
                copy(S1)
            with S.else_():
                with S.if_(d > bf):
                    copy(S0)
                with S.else_():
                    with S.if_((af <= d) & (lof >= d)):
                        t = tq(P.k2 * (d - af))
                        blend(S1, bc, t, S.f(1.0) - t, S1[3])
                    with S.else_():
                        with S.if_(hif > d):
                            copy(bc)
                        with S.else_():
                            t = tq((d - hif) * P.k1)
                            u = S.f(1.0) - t
                            for k in range(3): o[k].set(S.to_i(S.to_f(S0[k]) * t + S.to_f(bc[k]) * u))
                            o[3].set(S0[3])
        k255 = S.f(1.0 / 255.0)
        S.buf('out').store(i, S.vec4(*[S.to_f(o[k].get()) * k255 for k in range(4)]))


# ---------------------------------------------------------------------------------------------------------- Star Wipe (starWipe, starWipeFx8)
@kernel("star8", WH + [('mode', 'u'), ('bc0', 'u'), ('bc1', 'u'), ('bc2', 'u'), ('p', 'f'), ('lo', 'f'), ('hi', 'f'), ('inv', 'f'), ('x9', 'f'), ('x10', 'f'),
                         ('x11', 'f'), ('inv12', 'f'), ('inv13', 'f')], [PIC_IN, PIC_OUT, TABLE])
def star8(S):
    # starWipeFx8() on the 8-bit levels: in0 the outgoing picture, the table the incoming one and then the mask (bottom row first, one float per pixel).
    # mode 0: no border, hard edge (p); 1: no border, soft (lo, hi, inv); 2: border, hard (lo, hi); 3: border and soft (lo = x8, x9, x10, x11, inv12, inv13)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        m = S.buf('table').load(P.w * P.h * 4 + (P.h - 1 - S.gy) * P.w + S.gx)
        pa = lvl4(S, S.buf('in0').load(i))
        pb = lvl4(S, to_px(S, i))
        bc = [S.to_i(P.bc0), S.to_i(P.bc1), S.to_i(P.bc2), S.i(255)]                 # (as ints: the blends use them so)
        bcb = [bc[0] & 255, bc[1] & 255, bc[2] & 255, S.i(255)]                       # (as bytes: the plain border colour)
        o = [S.var('i') for _ in range(4)]
        def copy(src):
            for k in range(4): o[k].set(src[k])
        def clamp01(v):
            return S.select(S.f(0.0) > v, S.f(0.0), S.select(S.f(1.0) < v, S.f(1.0), v))
        def blend(sa, wa, sb, wb, alpha):
            for k in range(3): o[k].set(S.to_i(S.to_f(sa[k]) * wa + S.to_f(sb[k]) * wb))
            o[3].set(alpha)
        mode = P.mode
        with S.if_(S.eq(mode, 0)):
            copy([S.select(P.p > m, pb[k], pa[k]) for k in range(4)])
        with S.if_(S.eq(mode, 1)):
            with S.if_(P.lo > m):
                copy(pb)
            with S.else_():
                with S.if_(m > P.hi):
                    copy(pa)
                with S.else_():
                    t = clamp01(S.f(1.0) - (m - P.lo) * P.inv)
                    u = S.f(1.0) - t
                    blend(pa, u, pb, t, S.select(S.f(0.5) <= t, pb[3], pa[3]))
        with S.if_(S.eq(mode, 2)):
            with S.if_(P.lo > m):
                copy(pb)
            with S.else_():
                with S.if_(m > P.hi):
                    copy(pa)
                with S.else_():
                    copy(bcb)
        with S.if_(S.eq(mode, 3)):
            with S.if_(P.lo > m):
                copy(pb)
            with S.else_():
                with S.if_(m > P.x11):
                    copy(pa)
                with S.else_():
                    with S.if_((m >= P.lo) & (P.x9 >= m)):
                        t = clamp01((m - P.lo) * P.inv12)
                        blend(pb, S.f(1.0) - t, bc, t, pb[3])
                    with S.else_():
                        with S.if_((m > P.x9) & (P.x10 > m)):
                            copy(bcb)
                        with S.else_():
                            t = clamp01((m - P.x10) * P.inv13)
                            blend(pa, t, bc, S.f(1.0) - t, pa[3])
        k255 = S.f(1.0 / 255.0)
        S.buf('out').store(i, S.vec4(*[S.to_f(o[k].get()) * k255 for k in range(4)]))


@kernel("star_f", WH + [('rev', 'u'), ('hasf', 'u'), ('hasw', 'u')] + QW(18), [PIC_IN, PIC_OUT, TABLE])
def star_f(S):
    # starWipe() on float pictures: in0 the outgoing picture, the table the incoming one and then the wave position of every pixel (a double, two words);
    # doubles q0 pp (the progress, 1 - progress when reversed), q2 wf, q4 hw; floats q6..q9 the border colour
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        pp, wf, hw = [qsf(P, 2 * k) for k in range(3)]
        bc = bcvec(S, P, 6)
        f = ld_sf(S, S.buf('table'), P.w * P.h * 4 + i * 2)
        rev, hasf, hasw = S.ne(P.rev, 0), S.ne(P.hasf, 0), S.ne(P.hasw, 0)
        dd = pp - f
        zero, one = SF.const(S, 0.0), SF.const(S, 1.0)
        t = SF.sel(S, hasf, sf_clamp(S, dd / wf + 0.5, 0.0, 1.0), SF.sel(S, dd >= 0.0, one, zero))
        wh = wf * 0.5
        bet = SF.sel(S, hasf, sf_clamp(S, (hw + wh - dd.abs()) / wh, 0.0, 1.0), SF.sel(S, dd.abs() <= hw, one, zero))
        bet = SF.sel(S, hasw, bet, zero)
        t = SF.sel(S, rev, one - t, t)
        o = sf_mixpx(S, S.buf('in0').load(i), to_px(S, i), t)
        o = S.select(bet > 0.0, sf_mixpx(S, o, bc, bet), o)
        S.buf('out').store(i, o)


# ---------------------------------------------------------------------------------------------------------- Flash without diffusion (flashFx8, flashFx)
@kernel("flash8", WH + [('fromb', 'u'), ('base', 'u'), ('dscale', 'u'), ('t0', 'u'), ('t1', 'u'), ('t2', 'u'), ('diff', 'u')], [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def flash8(S):
    # flashFx8(): in0 the outgoing picture, the table the incoming one; fromb 1 takes the incoming. With diff the luma comes from in1, the blurred source (0..1, whole levels)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        q = [S.to_u(v) for v in lvl4(S, S.select(S.ne(P.fromb, 0), to_px(S, i), S.buf('in0').load(i)))]
        bl = S.buf('in1').load(i)
        def lr(v):
            x = v * 255.0
            return S.to_u(S.max(S.min(S.to_i(S.floor(x + 0.5)), S.i(255)), S.i(0)))
        l = [S.select(S.ne(P.diff, 0), lr(bl[k]), q[k]) for k in range(3)]
        d = ((l[0] * 0x13 + l[2] * 7 + l[1] * 0x25) * P.dscale) >> 14
        al = S.min(q[3] + (((d * 255) >> 14) + P.base), S.u(255))
        tint = [P.t0, P.t1, P.t2]
        o = []
        for k in range(3):
            v = q[k] + (((d * tint[k]) >> 14) + P.base)
            o.append(S.select(al < v, al, v))
        k255 = S.f(1.0 / 255.0)
        S.buf('out').store(i, S.vec4(S.to_f(S.to_i(o[0])) * k255, S.to_f(S.to_i(o[1])) * k255, S.to_f(S.to_i(o[2])) * k255, S.to_f(S.to_i(al)) * k255))


@kernel("flash_f", WH + [('fromb', 'u'), ('add', 'f')], [PIC_IN, PIC_OUT, TABLE])
def flash_f(S):
    # flashFx() on float pictures when the diffusions are 0: the same light (add, a float) on every pixel, kept under the alpha
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        o = S.select(S.ne(P.fromb, 0), to_px(S, i), S.buf('in0').load(i))
        def add(k):
            v = o[k] + P.add
            return S.select(v < o.w, v, o.w)               # std::min(o.a, v): v < a ? v : a  (b < a ? b : a)
        S.buf('out').store(i, S.vec4(add(0), add(1), add(2), o.w))


# ---------------------------------------------------------------------------------------------------------- Cross Effect (crossEffect in builtins_trans.cpp: always 8-bit)
def cross_byte(S, v):
    """toBytes of crossEffect(): v * 255 + 0.5 cut to 0 .. 255 and truncated"""
    x = v * 255.0 + 0.5
    return S.select(x < 0.0, S.i(0), S.select(x > 255.0, S.i(255), S.to_i(x)))


def cross_store(S, i, o):
    """the byte values o (ints) as (float)(o / 255.0)"""
    c255 = SF.const(S, 255.0)
    S.buf('out').store(i, S.vec4(*[(SF.from_i32(S, o[k]) / c255).to_f32() for k in range(4)]))


@kernel("cross_prep", WH + [('fromt', 'u')], [PIC_IN, PIC_OUT, TABLE])
def cross_prep(S):
    # a picture (in0, or the incoming one in the table with fromt) as the bytes crossEffect() takes, kept as byte / 255
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        px = S.select(S.ne(P.fromt, 0), to_px(S, i), S.buf('in0').load(i))
        k255 = S.f(1.0 / 255.0)
        S.buf('out').store(i, S.vec4(*[S.to_f(cross_byte(S, px[k])) * k255 for k in range(4)]))


@kernel("cross_mix", WH + [('t8', 'i')], [PIC_IN, PIC_IN1, PIC_OUT])
def cross_mix(S):
    # the cross-fade a + ((b - a) * t8) / 256 of two pictures of bytes (in0, in1), as (float)(o / 255.0)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        a = lvl4(S, S.buf('in0').load(i))
        b = lvl4(S, S.buf('in1').load(i))
        t8 = S.to_i(P.t8)
        cross_store(S, i, [a[k] + ((b[k] - a[k]) * t8) / 256 for k in range(4)])


@kernel("cross_out", WH, [PIC_IN, PIC_OUT])
def cross_out(S):
    # a blurred picture of whole levels (0..1) as (float)(level / 255.0)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        v = S.buf('in0').load(i)
        o = [S.max(S.min(S.to_i(S.floor(v[k] * 255.0 + 0.5)), S.i(255)), S.i(0)) for k in range(4)]
        cross_store(S, i, o)


@kernel("cross_cells", WH + [('nx', 'u'), ('ny', 'u'), ('fxa', 'u'), ('fxb', 'u')], [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def cross_cells(S):
    # the pixelate cells' averages, (sum / width) / height per channel: in0 the outgoing bytes, in1 the incoming, out nx x 2 ny (the outgoing's cells, then the incoming's);
    # the table holds each cell column's first pixel and width (nx each), then each row's (ny each)
    P = S.p
    with S.in_range(P.nx, P.ny):
        tab = S.buf('table')
        def ti(k): return S.to_u(S.to_i(S.f_to_bits(tab.load(k))))
        x0, ww = ti(S.gx), ti(P.nx + S.gx)
        y0, hh = ti(2 * P.nx + S.gy), ti(2 * P.nx + P.ny + S.gy)
        for which, (name, flag) in enumerate((('in0', P.fxa), ('in1', P.fxb))):
            res = S.var('v4')
            res.set(S.splat4(0.0))
            with S.if_(S.ne(flag, 0) & (hh > 0) & (ww > 0)):
                acc = [S.var('u') for _ in range(4)]
                for a in acc: a.set(S.u(0))
                src = S.buf(name)
                with S.for_(0, hh) as y:
                    with S.for_(0, ww) as x:
                        q = lvl4(S, src.load((P.h - 1 - (y0 + y)) * P.w + x0 + x))          # (the CPU counts rows from the bottom)
                        for k in range(4): acc[k].set(acc[k].get() + S.to_u(q[k]))
                res.set(S.vec4(*[S.to_f(S.to_i((acc[k].get() / ww) / hh)) for k in range(4)]))
            S.buf('out').store((S.gy + which * P.ny) * P.nx + S.gx, res.get() * (1.0 / 255.0))


@kernel("cross_pix", WH + [('nx', 'u'), ('ny', 'u'), ('fxa', 'u'), ('fxb', 'u'), ('t8', 'i')], [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def cross_pix(S):
    # the pixelated cross-fade: in0 the cells (cross_cells), in1 the picture that has no effect on it, the table the last cell row and column that wrote each row / column
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        tab = S.buf('table')
        j = S.to_i(S.f_to_bits(tab.load(P.h - 1 - S.gy)))
        ci = S.to_i(S.f_to_bits(tab.load(P.h + S.gx)))
        fxa, fxb = S.ne(P.fxa, 0), S.ne(P.fxb, 0)
        jj, cc = S.to_u(S.max(j, S.i(0))), S.to_u(S.max(ci, S.i(0)))
        cells = S.buf('in0')
        pas = lvl4(S, S.buf('in1').load(i))
        ca = lvl4(S, cells.load(jj * P.nx + cc))
        cb = lvl4(S, cells.load((jj + P.ny) * P.nx + cc))
        t8 = S.to_i(P.t8)
        ok = (j >= 0) & (ci >= 0)
        o = []
        for k in range(4):
            a = S.select(fxa, ca[k], pas[k])
            b = S.select(fxb, cb[k], pas[k])
            o.append(S.select(ok, a + ((b - a) * t8) / 256, S.i(0)))
        cross_store(S, i, o)


# ---------------------------------------------------------------------------------------------------------- Checkerboard (checkerboard / checkerboardFx8 in builtins_gen.cpp)
@kernel("checker8", WH, [PIC_OUT, TABLE])
def checker8(S):
    # the 8-bit picture: the table holds for each column (parity of the tile, blend weight or -1) and then for each row (parity, the colours A and B as bytes)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        tab = S.buf('table')
        def ti(k): return S.to_i(S.f_to_bits(tab.load(k)))
        px = ti(2 * S.gx)
        wv = ti(2 * S.gx + 1)
        rb = 2 * P.w + 9 * S.gy
        par = (px + ti(rb)) & 1
        out = []
        for k in range(4):
            a, b = ti(rb + 1 + k), ti(rb + 5 + k)
            c0 = S.select(S.eq(par, 0), a, b)
            c1 = S.select(S.eq(par, 0), b, a)
            v = S.select(wv >= 0, (c0 * (0x10000 - wv) + c1 * wv) >> 16, c0)
            out.append(S.to_f(v) / 255.0)
        S.buf('out').store(i, S.vec4(*out))


@kernel("checker_f", WH + QW(16), [PIC_OUT, TABLE])
def checker_f(S):
    # the float picture: the table holds the horizontal wave of each column and then the vertical wave of each row (doubles); doubles q0..q6 Color 1, q8..q14 Color 2
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        tab = S.buf('table')
        sx = ld_sf(S, tab, 2 * S.gx)
        sy = ld_sf(S, tab, 2 * P.w + 2 * S.gy)
        out = []
        for k in range(4):
            a, b = qsf(P, 2 * k), qsf(P, 8 + 2 * k)
            m1 = (( a + (b - a) * sx) * 255.0 + 1e-4).floor()
            m2 = (( b + (a - b) * sx) * 255.0 + 1e-4).floor()
            out.append(((m1 + (m2 - m1) * sy + 1e-4).floor() / 255.0).to_f32())
        S.buf('out').store(i, S.vec4(*out))
