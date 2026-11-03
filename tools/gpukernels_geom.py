"""Kernels of the geometry effects and the four OpenCL generators, identical to their CPU code (the ports of VEGAS's OpenCL kernels: vfxgen.h,
vfxpipcrop.h, vfxvignette.h, vfxpolar.h, vfxtinyplanet.h, ...). Loaded by gpukernels.py; the planners are in src/ofx/vkfilters.cpp.

All arithmetic is float32 with no fused multiply-add (spvdsl decorates every add / sub / mul NoContraction), the divide is the exact one of spvdsl, and the
library functions (sin, cos, atan2, pow) are the correctly rounded float of the double result (sf64). An 8-bit host frame gets the card's 12-bit store
(more::oclStore12), flagged by the push constant q8.
"""
import struct
from gpukernels import kernel, PIC_IN, PIC_IN1, PIC_OUT, TABLE, WH, SF, ld_sf


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def st12(S, v):
    """more::oclStore12: v kept in 0..1, cut to 12 bits, scaled to a byte with the half down, as a float level / 255 (not a number: 0)"""
    ok = ~S.isnan(v)
    c = S.min(S.max(S.select(ok, v, S.f(0.0)), 0.0), 1.0)
    n = S.to_i(S.floor(c * 4096.0))
    r = S.to_f((n * S.i(255) + S.i(2047)) >> 12) / 255.0
    return S.select(ok, r, S.f(0.0))


def store4(S, P, i, col):
    """the pixel's four floats, through the 12-bit store when P.q8"""
    q = S.ne(P.q8, 0)
    S.buf('out').store(i, S.vec4(*[S.select(q, st12(S, c), c) for c in col]))


def cvt_int(S, v):
    """vfxgen cvtInt: float to int towards zero, the machine's indefinite value (INT_MIN) for what does not fit"""
    ok = (v > -2147483648.0) & (v < 2147483648.0)
    return S.select(ok, S.to_i(S.select(ok, v, S.f(0.0))), S.i(-2147483648))


def cvt_uint(S, v):
    """vfxgen cvtUint: below 0 and not a number 0, above the range all ones"""
    big = v >= 4294967296.0
    pos = v > 0.0
    return S.select(big, S.u(0xffffffff), S.select(pos, S.to_u(S.select(pos & ~big, v, S.f(0.0))), S.u(0)))


def chain(S, pairs, default):
    """nested selects: the value of the first (cond, value) whose cond holds, else default; values are tuples of floats"""
    out = default
    for cond, val in reversed(pairs):
        out = tuple(S.select(cond, a, b) for a, b in zip(val, out))
    return out


# ---------------------------------------------------------------------------------------------------------- Checkerboard (vfxgen::checker)
@kernel("gen_checker", WH + [('q8', 'u'), ('pad', 'u'), ('tw', 'f'), ('th', 'f'), ('hx', 'f'), ('hy', 'f'), ('ix', 'f'), ('iy', 'f'), ('ox', 'f'), ('oy', 'f'),
                              ('a0', 'f'), ('a1', 'f'), ('a2', 'f'), ('a3', 'f'), ('b0', 'f'), ('b1', 'f'), ('b2', 'f'), ('b3', 'f')], [PIC_OUT])
def gen_checker(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        bg = (P.a0, P.a1, P.a2, P.a3)       # Color 2 (the first, "background")
        fg = (P.b0, P.b1, P.b2, P.b3)       # Color 1
        px = (S.to_f(S.gx) + 0.5) - P.ox
        py = (S.to_f(S.gy) + 0.5) - P.oy
        rx = S.f(1.0) / P.tw
        ry = S.f(1.0) / P.th
        ix = cvt_int(S, px * rx)
        iy = cvt_int(S, py * ry)
        ix = ix - S.select(px < S.to_f(ix) * P.tw, S.i(1), S.i(0))
        iy = iy - S.select(py < S.to_f(iy) * P.th, S.i(1), S.i(0))
        fix, fiy = S.to_f(ix), S.to_f(iy)
        x0 = fix * P.tw + P.hx
        y0 = fiy * P.th + P.hy
        x1 = (fix + 1.0) * P.tw + (-P.hx)
        y1 = (fiy + 1.0) * P.th + (-P.hy)

        def mixf(a, b, t):
            return a + (b - a) * t
        inb = (y0 <= py) & (py < y1)
        t = S.select(py < y0, (y0 - py) * P.iy, (py - y1) * P.iy)
        vA = [S.select(inb, bg[k], mixf(bg[k], fg[k], t)) for k in range(4)]
        vB = [S.select(inb, fg[k], mixf(fg[k], bg[k], t)) for k in range(4)]

        def colours(ixx):
            odd = S.ne((ixx + iy) & 1, 0)
            return ([S.select(odd, vA[k], vB[k]) for k in range(4)], [S.select(odd, vB[k], vA[k]) for k in range(4)])
        cA0, cB0 = colours(ix)
        cA1, cB1 = colours(ix + 1)
        x0b = x0 + P.tw
        x1b = x1 + P.tw
        t0 = (x0 - px) * P.ix
        t1 = (x0b - px) * P.ix
        r0a = tuple(mixf(cA0[k], cB0[k], t0) for k in range(4))
        r1a = tuple(mixf(cA1[k], cB1[k], t1) for k in range(4))
        res = chain(S, [(px < x0, r0a), (px < x1, tuple(cA0)), (px < x0b, r1a), (px < x1b, tuple(cA1))], bg)
        store4(S, P, i, res)


# ---------------------------------------------------------------------------------------------------------- Test Pattern (vfxgen::testPattern)
@kernel("gen_testpattern", WH + [('q8', 'u'), ('type', 'u')], [PIC_OUT])
def gen_testpattern(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        yk = hi - 1 - S.to_i(S.gy)
        fx, fy = S.to_f(S.gx), S.to_f(yk)
        one = S.f(1.0)
        white, bar, l125, l70 = [S.f(f32(f32(v) / f32(255.0))) for v in (235.0, 180.0, 125.0, 70.0)]
        blk, sub, plu, iq, iq2 = [S.f(f32(f32(v) / f32(255.0))) for v in (16.0, 7.0, 25.0, 113.0, 40.0)]

        def c3(r, g, b):
            return (r, g, b)
        wf, hf = S.to_f(P.w), S.to_f(P.h)

        def frame(c):
            tx, ty = wf / 10.0, hf / 10.0
            outside = (fx < tx) | (fx > wf - tx) | (fy < ty) | (fy > hf - ty)
            return tuple(S.select(outside, sub, v) for v in c)

        def lt_steps(vals, edges, default):
            return chain(S, [(fx < e, v) for v, e in zip(vals, edges)], default)

        def colour_bars(pal):
            u = wi / 7
            hi_ = white if pal else bar
            cols = [c3(hi_, hi_, hi_), c3(bar, bar, blk), c3(blk, bar, bar), c3(blk, bar, blk), c3(bar, blk, bar), c3(bar, blk, blk)]
            return lt_steps(cols, [S.to_f(u * k) for k in range(1, 7)], c3(blk, blk, bar))

        def reverse():
            u = wi / 7
            cols = [c3(sub, sub, white), c3(blk, blk, blk), c3(bar, sub, bar), c3(blk, blk, blk), c3(sub, bar, bar), c3(blk, blk, blk)]
            return lt_steps(cols, [S.to_f(u * k) for k in range(1, 7)], c3(bar, bar, bar))

        def pluge(ntsc):
            u = wi / 21
            tail = lt_steps([c3(blk, blk, blk), c3(sub, sub, sub), c3(blk, blk, blk), c3(plu, plu, plu)], [S.to_f(u * 15), S.to_f(u * 16), S.to_f(u * 17), S.to_f(u * 18)], c3(blk, blk, blk))
            if not ntsc:
                return tail
            return chain(S, [(fx < wf / 6.0, c3(sub, iq2, iq)), (fx < wf / 3.0, c3(white, white, white)), (fx < wf / 2.0, c3(l70, sub, S.f(f32(0.65))))], tail)

        ix_, iy_ = one / wf, one / hf
        grad2 = (one - fx * ix_, 0.0 + fx * ix_, one - fy * iy_)
        v3 = chain(S, [(fx < wf * 0.2, c3(sub, sub, sub)), (fx < wf * 0.3, c3(blk, blk, blk)), (fx < wf * 0.4, c3(plu, plu, plu)), (fx < wf * 0.5, c3(blk, blk, blk)),
                       (fy > hf * 0.7, c3(white, white, white)), (fy > hf * 0.5, c3(bar, bar, bar)), (fy > hf * 0.3, c3(l125, l125, l125))], c3(l70, l70, l70))
        g = fx * (one / wf)
        v7 = chain(S, [(fx < wf * 0.3, c3(one, one, one)), (fx < wf * 0.5, c3(bar, bar, bar)), (fx < wf * 0.7, c3(l125, l125, l125))], c3(l70, l70, l70))
        v8 = chain(S, [(fx < wf * 0.3, c3(one, one, sub)), (fx < wf * 0.5, c3(bar, bar, sub)), (fx < wf * 0.7, c3(l125, l125, sub))], c3(l70, l70, sub))
        qb0, qb1 = colour_bars(False), colour_bars(True)
        rev = reverse()
        v5 = chain(S, [(yk < hi_div(S, hi, 4), pluge(True)), (yk < hi_div(S, hi, 3), rev)], qb0)
        v6 = chain(S, [(yk < hi_div(S, hi, 4), pluge(False)), (yk < hi_div(S, hi, 3), rev)], qb1)
        t = P.type
        v = chain(S, [(S.eq(t, 0), qb0), (S.eq(t, 1), qb1), (S.eq(t, 2), grad2), (S.eq(t, 3), frame(v3)), (S.eq(t, 4), frame(c3(g, g, g))), (S.eq(t, 5), v5),
                      (S.eq(t, 6), v6), (S.eq(t, 7), frame(v7)), (S.eq(t, 8), frame(v8))], c3(S.f(0.0), S.f(0.0), S.f(0.0)))
        store4(S, P, i, (v[0], v[1], v[2], one))


def hi_div(S, h, d):
    return h / d


# ---------------------------------------------------------------------------------------------------------- Noise Texture (vfxgen::noiseRow, noiseMixOf, noiseColour)
def _mixf(a, b, t):
    return a + (b - a) * t


@kernel("gen_noise_rows", WH + [('fold', 'u'), ('count', 'u'), ('off0', 'f'), ('off1', 'f'), ('freq0', 'f'), ('freq1', 'f'), ('z', 'f')] + [('wt%d' % k, 'f') for k in range(6)],
        [(2, 'out', 'f', 'w'), TABLE], local=(8, 8))
def gen_noise_rows(S):
    """one invocation per (layer = x, picture row = y): the layer's values along the row, walked pixel by pixel as the kernel does (the running sums are float adds in order)"""
    P = S.p
    with S.in_range(P.count, P.h):
        tab = S.buf('table')
        out = S.buf('out')
        layer = S.gx
        step = S.f(1.0) / S.to_f(P.w)
        pos0 = P.off0 * P.freq0
        pos1 = (S.to_f(S.gy) * step + P.off1) * P.freq1
        pos2 = P.z
        pos3 = step * P.freq0
        scale = S.to_f(S.u(1) << layer)
        v0, v1, v2, s = pos0 * scale, pos1 * scale, pos2 * scale, pos3 * scale
        v0 = v0 + 10000.0
        v1 = v1 + 10000.0
        v2 = v2 + S.f(f32(10000.0 + 0.25))
        cells, frs = [], []
        for v in (v0, v1, v2):
            ci = cvt_int(S, S.floor(v))
            frs.append(v - S.to_f(ci))
            cells.append(ci + 990000)
        fade = []
        for fr in frs:
            a = fr * 6.0 + (-15.0)
            b = fr * a + 10.0
            fade.append(fr * fr * fr * b)

        def perm(idx):
            return S.to_i(tab.load(S.to_u(idx) & 255))

        a0 = [c & 255 for c in cells]
        a1 = [(c + 1) & 255 for c in cells]
        v41 = [perm(perm(a0[2]) + a0[1]), perm(perm(a0[2]) + a1[1]), perm(perm(a1[2]) + a0[1]), perm(perm(a1[2]) + a1[1])]

        def cell_values(f0, ax0, ax1):
            g = []
            for k in range(4):
                g.append(perm(v41[k] + ax0))
                g.append(perm(v41[k] + ax1))

            def grad(gi, k):
                return tab.load(S.u(256) + S.to_u(gi) * 3 + k)

            def dot3(gi, dx, dy, dz):
                return grad(gi, 0) * dx + grad(gi, 1) * dy + grad(gi, 2) * dz + S.f(0.0) * S.f(0.0)
            f1, f2 = frs[1], frs[2]
            d = [dot3(g[0], f0, f1, f2), dot3(g[1], f0 - 1.0, f1, f2), dot3(g[2], f0, f1 - 1.0, f2), dot3(g[3], f0 - 1.0, f1 - 1.0, f2),
                 dot3(g[4], f0, f1, f2 - 1.0), dot3(g[5], f0 - 1.0, f1, f2 - 1.0), dot3(g[6], f0, f1 - 1.0, f2 - 1.0), dot3(g[7], f0 - 1.0, f1 - 1.0, f2 - 1.0)]
            m1 = [d[0], d[2], d[1], d[3]]
            m2 = [d[4], d[6], d[5], d[7]]
            m3 = [_mixf(m1[i], m2[i], fade[2]) for i in range(4)]
            gx1 = [grad(g[0], 0), grad(g[2], 0), grad(g[1], 0), grad(g[3], 0)]
            gx2 = [grad(g[4], 0), grad(g[6], 0), grad(g[5], 0), grad(g[7], 0)]
            m4 = [_mixf(gx1[i], gx2[i], fade[2]) for i in range(4)]
            e1 = [m3[0], m3[2], m4[0], m4[2]]
            e2 = [m3[1], m3[3], m4[1], m4[3]]
            m5 = [_mixf(e1[i], e2[i], fade[1]) for i in range(4)]
            return m5[0], m5[1], m5[2] * s, m5[3] * s

        lo0, lo1, hi0, hi1 = cell_values(frs[0], a0[0], a1[0])
        vl0, vl1, vh0, vh1 = S.var('f', lo0), S.var('f', lo1), S.var('f', hi0), S.var('f', hi1)
        vfx = S.var('f', frs[0])
        vcell = S.var('i', cells[0])
        wts = [P.wt0, P.wt1, P.wt2, P.wt3, P.wt4, P.wt5]
        wsel = wts[5]
        for k in range(4, -1, -1):
            wsel = S.select(S.eq(layer, k), wts[k], wsel)
        w = S.f(f32(1.515151515)) * wsel
        fold = S.ne(P.fold, 0)
        base = (S.gy * P.count + layer) * P.w
        with S.for_(0, P.w) as x:
            fx = vfx.get()
            a = fx * 6.0 + (-15.0)
            b = fx * a + 10.0
            fadeX = fx * fx * fx * b
            v = _mixf(vl0.get(), vl1.get(), fadeX) * w
            v = S.select(fold, S.abs(v), v)
            out.store(base + x, v)
            fx2 = fx + s
            with S.if_(fx2 >= 1.0):
                n = S.to_i(cvt_uint(S, S.floor(fx2)))
                cell = vcell.get() + n
                vcell.set(cell)
                fx3 = fx2 - S.to_f(n)
                vfx.set(fx3)
                nl0, nl1, nh0, nh1 = cell_values(fx3, cell & 255, (cell + 1) & 255)
                vl0.set(nl0)
                vl1.set(nl1)
                vh0.set(nh0)
                vh1.set(nh1)
            with S.else_():
                vfx.set(fx2)
                vl0.set(vl0.get() + vh0.get())
                vl1.set(vl1.get() + vh1.get())


@kernel("gen_noise_mix", WH + [('q8', 'u'), ('style', 'u'), ('count', 'u'), ('sum', 'f'), ('freq0', 'f'), ('freq1', 'f'), ('off0', 'f'), ('off1', 'f'),
                               ('lo', 'f'), ('hi', 'f'), ('bias', 'f'), ('amp', 'f')] + [('t%d' % k, 'f') for k in range(5)] +
        [('a%d' % k, 'f') for k in range(4)] + [('b%d' % k, 'f') for k in range(4)], [(0, 'in0', 'f', 'r'), PIC_OUT])
def gen_noise_mix(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        rows = S.buf('in0')
        wf, hf = S.to_f(P.w), S.to_f(P.h)
        yk = S.to_f(S.to_i(P.h) - 1 - S.to_i(S.gy))
        lo, hi, bias, amp = P.lo, P.hi, P.bias, P.amp
        le = hi <= lo
        v44 = S.select(le, S.f(0.0), amp * (S.f(1.0) / (hi - lo)) * 65536.0)
        v45 = S.select(le, ((lo + hi) / 2.0 * amp + bias) * 65536.0 + 0.5, -(lo * v44) + bias * 65536.0 + 0.5)
        step = S.f(1.0) / wf
        v23x = step * P.freq0
        v23y = (-step) * P.freq1
        base = S.gy * P.count * P.w + S.gx
        sm = S.var('f', S.select(P.count > 0, rows.load(base), S.f(0.0)))
        with S.for_(1, P.count) as k:
            sm.set(sm.get() + rows.load(base + k * P.w))
        sm = sm.get()
        st = P.style
        fold = S.eq(st, 1) | S.eq(st, 2) | S.eq(st, 3) | S.eq(st, 4)
        L = P.sum
        # mode 0 (styles 0 and 1)
        v45a = S.select(fold, v45, v44 * 0.5 + v45)
        v47 = S.select(fold, L, L + L)
        v44a = v44 * (S.f(1.0) / v47)
        r0 = cvt_uint(S, sm * v44a + v45a + 0.5)
        # mode 2 (style 6)
        v67 = P.t0 * L
        v44b = v44 / 2.0
        v68 = (sm + 2.0) * v67
        v68 = v68 - S.to_f(cvt_int(S, S.floor(v68)))
        lowhalf = v68 < 0.5
        w1 = v68 * 4.0 + (-1.0)
        w1 = w1 * (-w1) + 2.0
        w2 = v68 * 4.0 + (-3.0)
        w2 = w2 * w2
        r2 = cvt_uint(S, S.select(lowhalf, w1, w2) * v44b + v45)
        # mode 1 (styles 2 to 5)
        v69 = L * P.t2
        v70 = v23x * P.t0
        xx = ((wf * P.off1 + hf) - 1.0) - yk
        v71 = (-xx) * v23y * P.t1
        yy = ((wf * (P.off1 - 1000.0) + hf) - 1.0) - yk
        u = (-yy) * v23y * P.t3
        u = u - S.to_f(cvt_int(S, S.floor(u)))
        g1 = u * 4.0 + (-1.0)
        g1 = g1 * (-g1) + 1.0
        g2 = u * 4.0 + (-3.0)
        g2 = g2 * g2 + (-1.0)
        v71 = S.select(P.t3 > 0.0, v71 + S.select(u < 0.5, g1, g2) * P.t4, v71)
        v72 = (wf * P.off0) * v70 + v71
        q = (sm * v69 + S.to_f(S.gx) * v70) + v72
        q = 2.0 * (q - S.to_f(cvt_int(S, S.floor(q)))) + (-1.0)
        q = q * (-q) + 1.0
        r1 = cvt_uint(S, q * v44 + v45)
        v65 = S.select(st < 2, r0, S.select(st < 6, r1, r2))
        t = S.min(S.max(S.to_f(v65) * S.f(0.0000152587890625), 0.0), 1.0)
        A = (P.a0, P.a1, P.a2, P.a3)
        B = (P.b0, P.b1, P.b2, P.b3)
        store4(S, P, i, [_mixf(A[k], B[k], t) for k in range(4)])


# ---------------------------------------------------------------------------------------------------------------------------------- the card's bilinear read
def lin_read(S, buf, w, h, px, py, repeat):
    """oclLinear4 / vfxpc::linearRead: CL_FILTER_LINEAR at pixel coordinates (no half pixel), the fractions cut to 1/256, blended along x then along y. buf: a picture
    buffer (top row first), read as the kernels' picture with rows from the bottom; texels beyond the picture are the edge's (repeat) or transparent black. A v4."""
    wi, hi = S.to_i(w), S.to_i(h)
    flx, fly = S.floor(px), S.floor(py)
    fx = px - flx
    fy = py - fly
    fx = S.floor(fx * 256.0 + 0.5) * S.f(1.0 / 256.0)
    fy = S.floor(fy * 256.0 + 0.5) * S.f(1.0 / 256.0)
    # (the cell kept within -2 .. size + 1: a position far outside reads the edge or nothing alike)
    cx = S.min(S.max(flx, -2.0), S.to_f(wi) + 1.0)
    cy = S.min(S.max(fly, -2.0), S.to_f(hi) + 1.0)
    x0, y0 = S.to_i(S.select(S.eq(cx, cx), cx, S.f(0.0))), S.to_i(S.select(S.eq(cy, cy), cy, S.f(0.0)))

    def tex(x, y):
        if repeat:
            xx, yy = S.max(S.min(x, wi - 1), S.i(0)), S.max(S.min(y, hi - 1), S.i(0))
            return buf.load(S.to_u(hi - 1 - yy) * w + S.to_u(xx))
        ok = (x >= 0) & (y >= 0) & (x < wi) & (y < hi)
        xx, yy = S.max(S.min(x, wi - 1), S.i(0)), S.max(S.min(y, hi - 1), S.i(0))
        return S.select(ok, buf.load(S.to_u(hi - 1 - yy) * w + S.to_u(xx)), S.splat4(0.0))
    a, b, c, d = tex(x0, y0), tex(x0 + 1, y0), tex(x0, y0 + 1), tex(x0 + 1, y0 + 1)
    t0 = a + (b - a) * fx
    t1 = c + (d - c) * fx
    return t0 + (t1 - t0) * fy


# ---------------------------------------------------------------------------------------------------------------------------------- Crop (vfxpc::cropInside)
@kernel("crop_ocl", WH + [('q8', 'u'), ('pad', 'u')] + [('m%d' % k, 'f') for k in range(6)] + [('b%d' % k, 'f') for k in range(4)] + [('op', 'f')], [PIC_IN, PIC_OUT])
def crop_ocl(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        fx = S.to_f(S.gx)
        fy = S.to_f(S.to_i(P.h) - 1 - S.to_i(S.gy))
        vx = P.m0 * fx + P.m1 * fy + P.m2
        vy = P.m3 * fx + P.m4 * fy + P.m5
        inside = (vx >= P.b0) & (vx <= P.b1) & (vy <= P.b2) & (vy >= P.b3)
        s = S.buf('in0').load(i)
        v = s * P.op
        q = S.ne(P.q8, 0)
        col = [S.select(inside, S.select(q, st12(S, v[k]), v[k]), S.f(0.0)) for k in range(4)]
        S.buf('out').store(i, S.vec4(*col))


# ---------------------------------------------------------------------------------------------------------------------------------- Picture In Picture (pictureInPictureOpenCl)
@kernel("pip_ocl", WH + [('q8', 'u'), ('pad', 'u')] + [('m%d' % k, 'f') for k in range(9)] + [('op', 'f')], [PIC_IN, PIC_OUT])
def pip_ocl(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        fx = S.to_f(S.gx)
        fy = S.to_f(S.to_i(P.h) - 1 - S.to_i(S.gy))
        w = P.m6 * fx + P.m7 * fy + P.m8
        u = (P.m0 * fx + P.m1 * fy + P.m2) / w
        v = (P.m3 * fx + P.m4 * fy + P.m5) / w
        inside = (u >= 0.0) & (u < S.to_f(P.w)) & (v >= 0.0) & (v < S.to_f(P.h))
        res = S.var('v4', S.splat4(0.0))
        with S.if_(inside):
            p = lin_read(S, S.buf('in0'), P.w, P.h, u, v, False) * P.op
            q = S.ne(P.q8, 0)
            res.set(S.vec4(*[S.select(q, st12(S, p[k]), p[k]) for k in range(4)]))
        S.buf('out').store(i, res.get())


def hypot_f(S, x, y):
    """std::hypot of floats: the double sqrt of x * x + y * y (both products exact), rounded to a float (what glibc's hypotf gives)"""
    X, Y = SF.from_f32(S, x), SF.from_f32(S, y)
    return (X * X + Y * Y).sqrt().to_f32()


# ---------------------------------------------------------------------------------------------------------------------------------- Vignette (vfxvig::amount)
@kernel("vignette_ocl", WH + [('invert', 'u'), ('q8', 'u'), ('h1', 'f'), ('cx', 'f'), ('cy', 'f'), ('c', 'f'), ('s', 'f'), ('asp', 'f'), ('inner', 'f'), ('den', 'f'),
                              ('expo', 'f'), ('ca', 'f'), ('cc0', 'f'), ('cc1', 'f'), ('cc2', 'f')], [PIC_IN, PIC_OUT])
def vignette_ocl(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        fy = S.to_f(S.to_i(P.h) - 1 - S.to_i(S.gy))
        dx = S.to_f(S.gx) - P.cx
        dy = (P.h1 - fy) - P.cy
        rx = dx * P.c - dy * P.s
        ry = dx * P.s + dy * P.c
        rx = rx / P.asp
        ry = ry * P.asp
        r = hypot_f(S, rx, ry)
        t = (r - P.inner) / P.den
        t = S.select(t < 0.0, S.f(0.0), S.select(t > 1.0, S.f(1.0), t))
        a = S.pow(t, P.expo)
        a = S.select(a < 0.0, S.f(0.0), S.select(a > 1.0, S.f(1.0), a))
        a = S.select(r > P.inner, a, S.f(0.0))
        a = S.select(S.ne(P.invert, 0), 1.0 - a, a)
        tt = a * P.ca
        s = S.buf('in0').load(i)
        cc = (P.cc0, P.cc1, P.cc2)
        store4(S, P, i, [s[k] + (cc[k] - s[k]) * tt for k in range(3)] + [s[3]])


# ---------------------------------------------------------------------------------------------------------------------------------- Spherize (vfxpolar::spherize)
@kernel("spherize_ocl", WH + [('q8', 'u'), ('cx', 'f'), ('cy', 'f'), ('amount', 'f'), ('sx', 'f'), ('sy', 'f')], [PIC_IN, PIC_OUT])
def spherize_ocl(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        fw, fh = S.to_f(P.w), S.to_f(P.h)
        v28 = ((P.cx * 2.0) - 0.5) * fw
        v29 = ((P.cy * 2.0) - 0.5) * fh
        v30 = S.f(1.0) / (P.sx + S.f(f32(0.0001)))
        v31 = S.f(1.0) / (P.sy + S.f(f32(0.0001)))
        fx = S.to_f(S.gx)
        fy = S.to_f(S.to_i(P.h) - 1 - S.to_i(S.gy))
        v33 = (fx - v28) * 2.0 / fw * v30
        v34 = (fy - v29) * 2.0 / fh * v31
        r2 = v34 * v34 + v33 * v33
        src = S.buf('in0')
        res = S.var('v4', src.load(i))
        with S.if_(r2 < 1.0):
            r = S.sqrt(r2)
            f = 1.0 - (S.cos(r * S.f(f32(1.5707963267949))) * P.amount) * S.f(f32(0.8))
            py = v29 + (fy - v29) * f
            px = v28 + (fx - v28) * f
            res.set(lin_read(S, src, P.w, P.h, px, py, True))
        r = res.get()
        store4(S, P, i, [r[0], r[1], r[2], r[3]])


# ---------------------------------------------------------------------------------------------------------------------------------- Tiny Planet (vftp::sourcePos)
def atan_f(S, x):
    """atanf: the double arctangent rounded to a float"""
    return sf_atan2_f(S, x, S.f(1.0))


def sf_atan2_f(S, y, x):
    from sf64 import sf_atan2
    return sf_atan2(S, SF.from_f32(S, y), SF.from_f32(S, x)).to_f32()


def asin_f(S, x):
    """asinf: the double arcsine (atan2 (x, sqrt (1 - x x))) rounded to a float; not a number beyond 1"""
    from sf64 import sf_atan2
    X = SF.from_f32(S, x)
    d = (SF.const(S, 1.0) - X * X).sqrt()
    r = sf_atan2(S, X, d).to_f32()
    bad = ~(S.abs(x) <= 1.0)
    return S.select(bad, S.f(float('nan')), r)


@kernel("tinyplanet_ocl", WH + [('q8', 'u'), ('lat', 'f'), ('lon', 'f'), ('z', 'f')], [PIC_IN, PIC_OUT])
def tinyplanet_ocl(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        pi = S.f(f32(3.14159265358979323846))
        hp = S.f(f32(3.14159265358979323846) / 2)
        twopi = S.f(f32(3.14159265358979323846) * 2)
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        v28 = S.to_f(wi) / P.z
        v29 = S.fma(P.lat * pi, S.f(f32(1.0 / 180)), -hp)
        v30 = P.lon * pi * S.f(f32(1.0 / 180))
        cos29 = S.select(S.eq(v29, -hp), S.f(0.0), S.cos(v29))
        sin29 = S.sin(v29)
        v31 = S.to_i(S.gx) - wi / 2
        v32 = (hi - 1 - S.to_i(S.gy)) - hi / 2
        f31, f32_ = S.to_f(v31), S.to_f(v32)
        v33 = S.sqrt(S.to_f(v31 * v31 + v32 * v32))
        v34 = 2.0 * atan_f(S, v33 / (2.0 * v28))
        s34, c34 = S.sin(v34), S.cos(v34)
        v35 = hp + asin_f(S, c34 * sin29 + f32_ * s34 * cos29 / v33)
        v36 = v30 + sf_atan2_f(S, f31 * s34, v33 * cos29 * c34 - f32_ * sin29 * s34)
        v36 = v36 + pi
        v36 = S.select(v36 < 0.0, v36 + twopi, S.select(v36 > twopi, v36 - twopi, v36))
        sy = S.to_f(hi - 1) * v35 / pi
        col = S.to_f(wi - 1) * v36 / twopi
        sx = S.trunc(col)
        sx = S.select(S.isfinite(sx), sx, S.f(0.0))
        sy = S.select(S.isfinite(sy), sy, S.f(0.0))
        r = lin_read(S, S.buf('in0'), P.w, P.h, sx, sy, True)
        store4(S, P, i, [r[0], r[1], r[2], r[3]])


# ---------------------------------------------------------------------------------------------------------------------------------- Lens Correction (lensMapPlace, lensMapRead)
@kernel("lenscorr_ocl", WH + [('q8', 'u'), ('pad', 'u'), ('cxp', 'f'), ('cyp', 'f'), ('s', 'f'), ('k1', 'f'), ('k2', 'f'), ('k3', 'f'), ('d', 'f'), ('inv', 'f')], [PIC_IN, PIC_OUT])
def lenscorr_ocl(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        yb = S.to_i(P.h) - 1 - S.to_i(S.gy)
        dx = S.to_f(S.gx) - P.cxp
        dy = S.to_f(yb) - P.cyp
        u = S.sqrt(dx * dx + dy * dy) * P.s
        f = ((P.k3 * u + P.k2) * u + P.k1) * u + P.d
        f = f * P.inv
        mx = dx * f + P.cxp
        my = dy * f + P.cyp
        inside = (mx > -1.0) & (mx < S.to_f(P.w)) & (my > -1.0) & (my < S.to_f(P.h))
        res = S.var('v4', S.splat4(0.0))
        with S.if_(inside):
            p = lin_read(S, S.buf('in0'), P.w, P.h, mx, my, False)
            q = S.ne(P.q8, 0)
            res.set(S.vec4(*[S.select(q, st12(S, p[k]), p[k]) for k in range(4)]))
        S.buf('out').store(i, res.get())


# ---------------------------------------------------------------------------------------------------------------------------------- doubles and bytes
def dpush(*names):
    out = []
    for n in names:
        out += [(n + '_h', 'u'), (n + '_l', 'u')]
    return out


def pd(P, name):
    return SF(P.S, getattr(P, name + '_h'), getattr(P, name + '_l'))


def cvt32_sf(S, v):
    """cvttsd2si: a double to a 32-bit int towards zero, INT_MIN for what is outside the range (and not a number)"""
    ok = (v > SF.const(S, -2147483649.0)) & (v < SF.const(S, 2147483648.0))
    return S.select(ok, v.trunc_i32(), S.i(-2147483648))


def level_of(S, v):
    """v8::toLevel: the picture value kept in 0..1 times 255, halves up; an int 0..255"""
    c = S.min(S.max(v, 0.0), 1.0)
    sc = c * 255.0
    i = S.to_i(sc)
    return i + S.select((sc - S.to_f(i)) >= 0.5, S.i(1), S.i(0))


def level_f(S, i):
    return S.to_f(i) * S.f(1.0 / 255.0)


# ---------------------------------------------------------------------------------------------------------------------------------- Wave (vfwave::render)
@kernel("wave_vf", WH + [('q8', 'u'), ('pad', 'u')] + dpush('ky', 'kx', 'ampx', 'ampy', 'p4', 'p5'), [PIC_IN, PIC_OUT])
def wave_vf(S):
    from sf64 import sf_sin
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        yd, xd = SF.from_u32(S, S.gy), SF.from_u32(S, S.gx)
        Wd, Hd = SF.from_u32(S, P.w), SF.from_u32(S, P.h)
        k = SF.const(S, 16384.0)
        bx0 = cvt32_sf(S, sf_sin(S, yd / Hd * pd(P, 'ky') + pd(P, 'p4')) * pd(P, 'ampx') * k)
        dy = cvt32_sf(S, (sf_sin(S, xd / Wd * pd(P, 'kx') + pd(P, 'p5')) * pd(P, 'ampy') + yd) * k)
        bx = S.to_i(S.to_u(bx0) + S.gx * 16384)
        limx = S.to_i(S.to_u(wi - 2) << 14)
        limy = S.to_i(S.to_u(hi - 2) << 14)
        sx = S.select(bx < 0, S.i(0), S.select(bx > limx, limx, bx))
        sy = S.select(dy < 0, S.i(0), S.select(dy > limy, limy, dy))
        ix, fx, iy, fy = sx >> 14, sx & 0x3fff, sy >> 14, sy & 0x3fff
        src = S.buf('in0')
        t = [src.load(S.to_u(iy + dj) * P.w + S.to_u(ix + di)) for dj in (0, 1) for di in (0, 1)]       # p00, p10, p01, p11
        res = S.var('v4', S.splat4(0.0))
        with S.if_(S.ne(P.q8, 0)):
            ufx, ufy = S.to_u(fx), S.to_u(fy)
            w00 = ((S.u(0x4000) - ufx) * (S.u(0x4000) - ufy)) >> 4
            w10 = (ufx * (S.u(0x4000) - ufy)) >> 4
            w01 = ((S.u(0x4000) - ufx) * ufy) >> 4
            w11 = (ufx * ufy) >> 4
            out = []
            for ch in range(4):
                v = [S.to_u(level_of(S, t[n][ch])) for n in range(4)]
                s = (w00 * v[0] + w10 * v[1] + w01 * v[2] + w11 * v[3] + S.u(0x800000)) >> 24
                out.append(level_f(S, S.to_i(s)))
            res.set(S.vec4(*out))
        with S.else_():
            fxf = S.to_f(fx) * S.f(1.0 / 16384.0)
            fyf = S.to_f(fy) * S.f(1.0 / 16384.0)
            a = 1.0 - fyf
            b = 1.0 - fxf
            w00, w01, w11, w10 = b * a, b * fyf, fyf * fxf, a * fxf
            out = []
            for ch in range(4):
                s = w10 * t[1][ch]
                s = s + w00 * t[0][ch]
                s = s + w01 * t[2][ch]
                s = s + w11 * t[3][ch]
                out.append(s)
            res.set(S.vec4(*out))
        S.buf('out').store(i, res.get())


# ---------------------------------------------------------------------------------------------------------------------------------- Swirl and Pinch/Punch (vfswirl)
def floor_magic(S, v):
    """vfswirl::floorMagic: round-to-nearest-even of (v - 0.499999985) by the 2^52 + 2^51 trick, the low 32 bits as an int"""
    d = (v - 0.499999985) + 6755399441055744.0
    return S.to_i(d.lo)


@kernel("swirlpinch_vf", WH + [('q8', 'u'), ('mode', 'u'), ('cx', 'i'), ('cy', 'i')] + dpush('k13', 'syinv', 'rx', 'ry', 'amt'), [PIC_IN, PIC_OUT])
def swirlpinch_vf(S):
    """mode 0: Swirl (amt = Amount * 15), mode 1: Pinch/Punch (amt = Amount); rows as the CPU's Buf holds them (the row index is the row of the picture)"""
    from sf64 import sf_sincos, sf_cos
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        x, y = S.to_i(S.gx), S.to_i(S.gy)
        src = S.buf('in0')
        k13, syinv, rx, ry, amt = pd(P, 'k13'), pd(P, 'syinv'), pd(P, 'rx'), pd(P, 'ry'), pd(P, 'amt')
        ydiff = SF.from_i32(S, y - P.cy)
        dy = ((ydiff + ydiff) / SF.from_u32(S, P.h)) * syinv
        dy2 = dy * dy
        xrel = SF.from_i32(S, x - P.cx)
        dx = xrel * k13
        d2 = dx * dx + dy2
        inside = d2 < 1.0
        s0 = src.load(i)
        res = S.var('v4', S.select(S.ne(P.q8, 0), S.vec4(*[level_f(S, level_of(S, s0[ch])) for ch in range(4)]), s0))
        with S.if_(inside):
            r = d2.sqrt()
            cxd, cyd = SF.from_i32(S, P.cx), SF.from_i32(S, P.cy)
            xs_v, ys_v = SFVarGeom(S), SFVarGeom(S)
            with S.if_(S.eq(P.mode, 0)):
                one = SF.const(S, 1.0) - r
                t = (one * amt) * one
                sa, ca = sf_sincos(S, t)
                ys_v.set((ca * dy - sa * dx) * ry + cyd)
                xs_v.set((ca * dx + sa * dy) * rx + cxd)
            with S.else_():
                c = sf_cos(S, r * 3.1415926)
                k = SF.const(S, 1.0) - ((c + 1.0) * amt) * 0.4
                ys_v.set(cyd + ydiff * k)
                xs_v.set(cxd + xrel * k)
            ysrc, xsrc = ys_v.get(), xs_v.get()
            iy = floor_magic(S, ysrc)
            ix = floor_magic(S, xsrc)
            y0, y1 = S.max(S.min(iy, hi - 1), S.i(0)), S.max(S.min(iy + 1, hi - 1), S.i(0))
            x0, x1 = S.max(S.min(ix, wi - 1), S.i(0)), S.max(S.min(ix + 1, wi - 1), S.i(0))
            p00, p01 = src.load(S.to_u(y0) * P.w + S.to_u(x0)), src.load(S.to_u(y0) * P.w + S.to_u(x1))
            p10, p11 = src.load(S.to_u(y1) * P.w + S.to_u(x0)), src.load(S.to_u(y1) * P.w + S.to_u(x1))
            swirl = S.eq(P.mode, 0)
            with S.if_(S.ne(P.q8, 0)):
                fy = cvt32_sf(S, (ysrc - SF.from_i32(S, iy)) * 256.0)
                fx = cvt32_sf(S, (xsrc - SF.from_i32(S, ix)) * 256.0)
                ufx, ufy = S.to_u(fx), S.to_u(fy)
                B = [[S.to_u(level_of(S, p[ch])) for ch in range(4)] for p in (p00, p01, p10, p11)]
                with S.if_(swirl):
                    a00 = (B[0][3] * (S.u(256) - ufx) * (S.u(256) - ufy)) >> 8
                    a01 = (B[1][3] * ufx * (S.u(256) - ufy)) >> 8
                    a10 = (B[2][3] * (S.u(256) - ufx) * ufy) >> 8
                    a11 = (B[3][3] * ufx * ufy) >> 8
                    sm = a11 + a10 + a01 + a00
                    out = []
                    for ch in range(3):
                        v = (B[1][ch] * a01 + B[2][ch] * a10 + B[0][ch] * a00 + (B[3][ch] * a11 + S.u(0x8000))) >> 16
                        out.append(S.select(S.eq(sm, 0), S.f(0.0), level_f(S, S.to_i(v & 255))))
                    al = S.select(S.eq(sm, 0), S.f(0.0), level_f(S, S.to_i(((sm + 0x80) >> 8) & 255)))
                    res.set(S.vec4(out[0], out[1], out[2], al))
                with S.else_():
                    out = []
                    for ch in range(4):
                        top = B[0][ch] * (S.u(256) - ufx) + B[1][ch] * ufx
                        bot = B[2][ch] * (S.u(256) - ufx) + B[3][ch] * ufx
                        v = (top * (S.u(256) - ufy) + bot * ufy) >> 16
                        out.append(level_f(S, S.to_i(v & 255)))
                    res.set(S.vec4(*out))
            with S.else_():
                fy = (ysrc - SF.from_i32(S, iy)).to_f32()
                fx = (xsrc - SF.from_i32(S, ix)).to_f32()
                omfy, omfx = 1.0 - fy, 1.0 - fx
                with S.if_(swirl):
                    w00 = (omfx * p00[3]) * omfy
                    w10 = (omfx * p10[3]) * fy
                    w01 = (fx * p01[3]) * omfy
                    w11 = (fx * p11[3]) * fy
                    sm = ((w01 + w00) + w10) + w11
                    good = sm > 0.0
                    out = []
                    for ch in range(3):
                        out.append(S.select(good, ((w00 * p00[ch] + w01 * p01[ch]) + w10 * p10[ch]) + w11 * p11[ch], S.f(0.0)))
                    res.set(S.vec4(out[0], out[1], out[2], S.select(good, sm, S.f(0.0))))
                with S.else_():
                    out = []
                    for ch in range(4):
                        top = omfx * p00[ch] + fx * p01[ch]
                        bot = omfx * p10[ch] + fx * p11[ch]
                        out.append(top * omfy + bot * fy)
                    res.set(S.vec4(*out))
        S.buf('out').store(i, res.get())


class SFVarGeom:
    """a double kept in two uint variables"""
    def __init__(self, S):
        self.hv, self.lv = S.var('u', S.u(0)), S.var('u', S.u(0))
        self.S = S
    def set(self, v):
        self.hv.set(v.hi)
        self.lv.set(v.lo)
    def get(self):
        return SF(self.S, self.hv.get(), self.lv.get())


# ---------------------------------------------------------------------------------------------------------------------------------- Stereoscopic 3D Adjust (vfxstereo::warp and the floating window)
@kernel("stereo_warp", WH + [('q8', 'u'), ('full', 'i')] + dpush('m0', 'm1', 'm2', 'm3', 'm4', 'm5', 'm6', 'm7', 'm8', 'part'), [PIC_IN, PIC_OUT])
def stereo_warp(S):
    """the inverse matrix (m0 .. m8, rows) in doubles; q8: whole levels as the 8-bit processor does"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        x = S.to_i(S.gx)
        ym = hi - 1 - S.to_i(S.gy)                  # the row as it lies in memory (bottom first)
        xd, yd = SF.from_i32(S, x), SF.from_i32(S, ym)
        m = [pd(P, 'm%d' % k) for k in range(9)]
        d = m[6] * xd + m[7] * yd + m[8]
        sx = (m[0] * xd + m[1] * yd + m[2]) / d
        sy = (m[3] * xd + m[4] * yd + m[5]) / d
        wd, hd = SF.from_i32(S, wi), SF.from_i32(S, hi)
        ok = (sx > -2.0) & (sx < wd + 1.0) & (sy > -2.0) & (sy < hd + 1.0)
        src = S.buf('in0')
        q8 = S.ne(P.q8, 0)
        res = S.var('v4', S.splat4(0.0))
        with S.if_(ok):
            fx0, fy0 = sx.floor(), sy.floor()
            x0, y0 = fx0.floor_i32(), fy0.floor_i32()
            fx, fy = sx - fx0, sy - fy0
            wx0, wy0 = SF.const(S, 1.0) - fx, SF.const(S, 1.0) - fy

            def tex(xx, yy):
                inr = (xx >= 0) & (xx < wi) & (yy >= 0) & (yy < hi)
                px = src.load(S.to_u(hi - 1 - S.max(S.min(yy, hi - 1), S.i(0))) * P.w + S.to_u(S.max(S.min(xx, wi - 1), S.i(0))))
                out = []
                for ch in range(4):
                    v = S.min(S.max(px[ch], 0.0), 1.0) * 255.0
                    v = S.select(q8, S.round_even(v), v)
                    out.append(SF.sel(S, inr, SF.from_f32(S, v), SF.const(S, 0.0)))
                return out
            t00, t10, t01, t11 = tex(x0, y0), tex(x0 + 1, y0), tex(x0, y0 + 1), tex(x0 + 1, y0 + 1)
            out = []
            for ch in range(4):
                v = (t00[ch] * wx0 + t10[ch] * fx) * wy0 + (t01[ch] * wx0 + t11[ch] * fx) * fy
                lv = v + 0.46
                fl = lv.floor()
                mx = SF.sel(S, SF.const(S, 0.0) < fl, fl, SF.const(S, 0.0))
                cl = SF.sel(S, mx < 255.0, mx, SF.const(S, 255.0))
                out.append(S.select(q8, cl.to_f32(), v.to_f32()))
            res.set(S.vec4(*out))
        r = res.get()
        # the left floating window
        part = pd(P, 'part')
        inwin = x < P.full
        edge = S.eq(x, P.full) & (part > 0.0) & (P.full < wi)
        omp = SF.const(S, 1.0) - part
        e = []
        for ch in range(3):
            e.append((SF.from_f32(S, r[ch]) * omp).to_f32())
        e.append((SF.from_f32(S, r[3]) * omp + part * 255.0).to_f32())
        e = [S.select(q8, S.round_even(v), v) for v in e]
        fin = [S.select(inwin, S.f(0.0), S.select(edge, e[ch], r[ch])) for ch in range(3)]
        fin.append(S.select(inwin, S.f(255.0), S.select(edge, e[3], r[3])))
        S.buf('out').store(i, S.vec4(*[v / 255.0 for v in fin]))


# ---------------------------------------------------------------------------------------------------------------------------------- Deform (vfdeform::render, all three qualities)
def sfvar(S, init):
    v = SFVarGeom(S)
    v.set(SF.of(S, init))
    return v


@kernel("deform_vf", WH + [('q8', 'u'), ('mode', 'u'), ('flags', 'u'), ('bx0', 'i'), ('by0', 'i'), ('ow', 'i'), ('oh', 'i'), ('bg0', 'f'), ('bg1', 'f'), ('bg2', 'f'), ('bg3', 'f')],
        [PIC_IN, PIC_OUT, TABLE])
def deform_vf(S):
    """The table holds the doubles of vfdeform::Geom (ww hh w2 h2 sheX sheY sqX0 sqXk sqY0 sqYk cuY0 cuY1 cuY0b cuY1b cuX0 cuX1 cuX0b cuX1b offX offY, two words each).
    mode: 0 nearest pixel, 1 bilinear, 2 footprint average (quality < 0.6, < 0.85, else). flags: 1 shear, 2 squeeze x, 4 squeeze y, 8 curve y, 16 curve x, 32 amount != 0.
    q8: the frame is bytes (whole levels). bg: the background pixel (levels as floats when q8). A pixel outside the box (bx0, by0, ow, oh) is the background."""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        x, y = S.to_i(S.gx), S.to_i(S.gy)
        tab = S.buf('table')
        src = S.buf('in0')
        names = ['ww', 'hh', 'w2', 'h2', 'sheX', 'sheY', 'sqX0', 'sqXk', 'sqY0', 'sqYk', 'cuY0', 'cuY1', 'cuY0b', 'cuY1b', 'cuX0', 'cuX1', 'cuX0b', 'cuX1b', 'offX', 'offY']
        G = {n: ld_sf(S, tab, S.u(2 * k)) for k, n in enumerate(names)}
        ww, hh, w2, h2 = G['ww'], G['hh'], G['w2'], G['h2']
        q8 = S.ne(P.q8, 0)
        bg = [P.bg0, P.bg1, P.bg2, P.bg3]
        bgsf = [SF.from_f32(S, b) for b in bg]
        Wd, Hd = SF.from_u32(S, P.w), SF.from_u32(S, P.h)

        def flag(bit):
            return S.ne(P.flags & bit, 0)
        f_shear, f_sqx, f_sqy, f_cuy, f_cux, f_amt = [flag(b) for b in (1, 2, 4, 8, 16, 32)]
        one = SF.const(S, 1.0)

        def unmap(px, py):
            """vfdeform::unmap: where the drawn position comes from (a factor of 0 sets only the one value, as there)"""
            xx, yy = px, py
            xs, ys = xx - yy * G['sheX'], yy - xx * G['sheY']
            xx, yy = SF.sel(S, f_shear, xs, xx), SF.sel(S, f_shear, ys, yy)
            stop = S.eq(S.u(0), S.u(1))
            ovxf, ovyf = stop, stop
            ovx = SF.const(S, -1.0)
            ovy = SF.const(S, -1.0)
            # squeeze x
            s = yy * G['sqXk'] + G['sqX0']
            act = f_sqx & ~stop
            z = s.is_zero()
            new = act & z
            ovxf = ovxf | new
            xx = SF.sel(S, act & ~z, (xx - w2) * w2 / s + w2, xx)
            stop = stop | new
            # squeeze y
            s = xx * G['sqYk'] + G['sqY0']
            act = f_sqy & ~stop
            z = s.is_zero()
            new = act & z
            ovyf = ovyf | new
            yy = SF.sel(S, act & ~z, (yy - h2) * h2 / s + h2, yy)
            stop = stop | new
            # curve x
            t = ((yy - h2) * (yy - h2)) / (h2 * h2)
            b = t * G['cuX1'] - G['cuX1b']
            a = t * G['cuX0'] - G['cuX0b']
            b = b + ww
            act = f_cux & ~stop
            eqab = a.eq(b)
            new = act & eqab
            ovx = SF.sel(S, new, SF.sel(S, a > xx, SF.const(S, -1.0), ww + 1.0), ovx)
            ovxf = ovxf | new
            xx = SF.sel(S, act & ~eqab, ((xx - a) / (b - a)) * ww, xx)
            stop = stop | new
            # curve y
            t = ((xx - w2) * (xx - w2)) / (w2 * w2)
            b = t * G['cuY1'] - G['cuY1b']
            a = t * G['cuY0'] - G['cuY0b']
            b = b + hh
            act = f_cuy & ~stop
            eqab = a.eq(b)
            new = act & eqab
            ovy = SF.sel(S, new, SF.sel(S, a > yy, SF.const(S, -1.0), hh + 1.0), ovy)
            ovyf = ovyf | new
            yy = SF.sel(S, act & ~eqab, ((yy - a) / (b - a)) * hh, yy)
            stop = stop | new
            # (the -1 of squeeze x / y is the same value for both)
            rx = SF.sel(S, ovxf, ovx, SF.sel(S, ovyf, px, xx))
            ry = SF.sel(S, ovyf, ovy, SF.sel(S, ovxf, py, yy))
            return rx, ry

        def texel(xi, yi):
            inr = (xi >= 0) & (xi < wi) & (yi >= 0) & (yi < hi)
            cx = S.max(S.min(xi, wi - 1), S.i(0))
            cy = S.max(S.min(yi, hi - 1), S.i(0))
            t = src.load(S.to_u(cy) * P.w + S.to_u(cx))
            return [SF.from_f32(S, S.select(inr, S.select(q8, S.to_f(level_of(S, t[k])), t[k]), bg[k])) for k in range(4)]

        def chan(v):
            """toChan then back to a double"""
            f = SF.from_f32(S, v.to_f32())
            b = SF.from_i32(S, v.trunc_i32() & 255)
            return SF.sel(S, q8, b, f)

        def bilinear(bxv, byv):
            ix, iy = floor_magic(S, bxv), floor_magic(S, byv)
            valid = (ix >= -1) & (ix < wi) & (iy >= -1) & (iy < hi)
            fx, fy = bxv - SF.from_i32(S, ix), byv - SF.from_i32(S, iy)
            wx0, wy0 = one - fx, one - fy
            t00, t10, t01, t11 = texel(ix, iy), texel(ix + 1, iy), texel(ix, iy + 1), texel(ix + 1, iy + 1)
            out = []
            for k in range(4):
                a = (t00[k] * wx0 + t10[k] * fx) * wy0
                b = (t01[k] * wx0 + t11[k] * fx) * fy
                out.append(SF.sel(S, valid, chan(a + b), bgsf[k]))
            return out

        def colour(vals):
            fl = [v.to_f32() for v in vals]
            lv = [level_f(S, v.trunc_i32()) for v in vals]
            return S.vec4(*[S.select(q8, lv[k], fl[k]) for k in range(4)])

        res = S.var('v4', S.vec4(*[S.select(q8, level_f(S, S.to_i(b)), b) for b in bg]))
        inbox = (x >= P.bx0) & (x < P.bx0 + P.ow) & (y >= P.by0) & (y < P.by0 + P.oh)
        with S.if_(inbox):
            bx, by = x - P.bx0, y - P.by0
            m2 = S.eq(P.mode, 2)
            arr = S.local_array(20, 'u')
            n = S.select(m2, S.u(5), S.u(1))
            with S.for_(0, n) as k:
                dx = S.select(S.eq(k, 1), S.i(-1), S.select(S.eq(k, 3), S.i(1), S.i(0)))
                dy = S.select(S.eq(k, 2), S.i(-1), S.select(S.eq(k, 4), S.i(1), S.i(0)))
                px = SF.from_i32(S, bx + dx) + G['offX']
                py = SF.from_i32(S, by + dy) + G['offY']
                rx, ry = unmap(px, py)
                arr.store(k * 4, rx.hi)
                arr.store(k * 4 + 1, rx.lo)
                arr.store(k * 4 + 2, ry.hi)
                arr.store(k * 4 + 3, ry.lo)

            def pt(k):
                return SF(S, arr.load(k * 4), arr.load(k * 4 + 1)), SF(S, arr.load(k * 4 + 2), arr.load(k * 4 + 3))
            P0x, P0y = pt(0)
            half = SF.const(S, 0.5)
            A0, A1, B0, B1, C0, C1, D0, D1 = P0x, P0y, P0x, P0y, P0x, P0y, P0x, P0y
            if True:
                lx_, ly_ = pt(1)
                ux_, uy_ = pt(2)
                rx_, ry_ = pt(3)
                dx_, dy_ = pt(4)
                hasL, hasU, hasR, hasD = S.ne(bx, 0), S.ne(by, 0), S.ne(bx, P.ow - 1), S.ne(by, P.oh - 1)
                A0 = SF.sel(S, hasL, (P0x + lx_) * half, A0)
                A1 = SF.sel(S, hasL, (P0y + ly_) * half, A1)
                B0 = SF.sel(S, hasU, (P0x + ux_) * half, B0)
                B1 = SF.sel(S, hasU, (P0y + uy_) * half, B1)
                C0 = SF.sel(S, hasR, (P0x + rx_) * half, C0)
                C1 = SF.sel(S, hasR, (P0y + ry_) * half, C1)
                D0 = SF.sel(S, hasD, (P0x + dx_) * half, D0)
                D1 = SF.sel(S, hasD, (P0y + dy_) * half, D1)
                c = A0 > C0
                A0, C0 = SF.sel(S, c, C0, A0), SF.sel(S, c, A0, C0)
                c = B1 > D1
                B1, D1 = SF.sel(S, c, D1, B1), SF.sel(S, c, B1, D1)
                c = A1 > C1
                A1, C1 = SF.sel(S, c, C1, A1), SF.sel(S, c, A1, C1)
                c = B0 > D0
                B0, D0 = SF.sel(S, c, D0, B0), SF.sel(S, c, B0, D0)
            t1 = (C1 - A1) * (C0 - A0)
            t2 = (D1 - B1) * (D0 - B0)
            quad = f_amt & (~t1.eq(0.0) | ~t2.eq(0.0))
            foot = m2 & quad
            with S.if_(foot):
                lx, ly = SF.const(S, 5.0) * Wd, SF.const(S, 5.0) * Hd
                dx1, dy1, dx2, dy2 = C0 - A0, C1 - A1, D0 - B0, D1 - B1
                far = (dx1.abs() > lx) | (dy1.abs() > ly) | (dx2.abs() > lx) | (dy2.abs() > ly)
                with S.if_(~far):
                    sums = [sfvar(S, 0.0) for _ in range(4)]
                    cnt = sfvar(S, 0.0)
                    limN = S.select(P.w > P.h, S.to_i(P.w), S.to_i(P.h)) * 5 + 8
                    slope1, slope2 = dy1 / dx1, dx2 / dy2
                    with S.for_(0, 2) as ps:
                        p1 = S.eq(ps, 1)
                        U0 = SF.sel(S, p1, B1, A0)
                        U1 = SF.sel(S, p1, D1, C0)
                        V0 = SF.sel(S, p1, B0, A1)
                        slope = SF.sel(S, p1, slope2, slope1)
                        Umax = SF.sel(S, p1, Hd, Wd)
                        Vmax = SF.sel(S, p1, Wd, Hd)
                        enabled = S.select(p1, ~B1.eq(D1) & ~(D1 < B1), ~C0.eq(A0) & ~(C0 < A0))
                        est = (U1 - U0).trunc_i32() + 4
                        est = S.select(U0.abs() >= SF.const(S, 1125899906842624.0), limN + 1, S.min(S.max(est, S.i(0)), limN + 1))
                        bound = S.to_u(S.select(enabled, est, S.i(0)))
                        u = sfvar(S, 0.0)
                        u.set(U0)
                        active = S.var('u', S.u(1))
                        with S.for_(0, bound) as it:
                            with S.if_(S.ne(active.get(), 0)):
                                uu = u.get()
                                inu = ~(uu < 0.0) & (Umax > uu)
                                vv = (uu - U0) * slope + V0
                                inn = inu & ~(vv < 0.0) & (Vmax > vv)
                                cur = [sfvar(S, 0.0) for _ in range(4)]
                                for k in range(4):
                                    cur[k].set(bgsf[k])
                                with S.if_(inn):
                                    bl = bilinear(SF.sel(S, p1, vv, uu), SF.sel(S, p1, uu, vv))
                                    for k in range(4):
                                        cur[k].set(bl[k])
                                for k in range(4):
                                    sums[k].set(sums[k].get() + cur[k].get())
                                cnt.set(cnt.get() + 1.0)
                                un = uu + 1.0
                                u.set(un)
                                cont = (U1 >= un) & ((it + 1) <= S.to_u(limN))
                                active.set(S.select(cont, S.u(1), S.u(0)))
                    cn = cnt.get()
                    vals = [SF.sel(S, cn.is_zero(), bgsf[k], chan(sums[k].get() / cn)) for k in range(4)]
                    res.set(colour(vals))
            with S.else_():
                Px, Py = P0x, P0y
                in2 = ~(Px < 0.0) & ~(Wd < Px) & ~(Py < 0.0) & ~(Hd < Py)
                in1 = ~(Px < 0.0) & (Wd > Px) & ~(Py < 0.0) & (Hd > Py)
                inside = S.select(m2, in2, in1)
                with S.if_(inside):
                    with S.if_(S.ne(P.mode, 0)):
                        res.set(colour(bilinear(Px, Py)))
                    with S.else_():
                        t = src.load(S.to_u(Py.trunc_i32()) * P.w + S.to_u(Px.trunc_i32()))
                        res.set(S.select(q8, S.vec4(*[level_f(S, level_of(S, t[k])) for k in range(4)]), t))
        S.buf('out').store(i, res.get())
