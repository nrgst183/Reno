"""Kernels of the blur and light effects that follow VEGAS's OpenCL kernels (the CPU code is src/ofx/vfx*.h and builtins_*.cpp; each kernel says which function it mirrors).
tools/gpukernels.py loads this file; see its header for the conventions. Pictures are premultiplied RGBA floats, TOP row first (the CPU's OpenCL ports hold them bottom row first,
which the kernels take into account where it matters)."""
from gpukernels import kernel, PIC_IN, PIC_IN1, PIC_OUT, TABLE, WH, fmin, fmax, fsqrt, dpush, pd, ld_sf
from spvdsl import compile_kernel
from sf64 import SF


def efma(S, a, b, c):
    """std::fmaf(a, b, c): the product of two floats is exact in a double, so the double sum rounded to a float is the fused result (a double rounding at the very rare float ties only);
    the hardware Fma of the GPU is not reliably fused (SwiftShader), so it is done in tools/sf64.py"""
    return (SF.from_f32(S, S.conv(a, 'f')) * SF.from_f32(S, S.conv(b, 'f')) + SF.from_f32(S, S.conv(c, 'f'))).to_f32()


def store12(S, v):
    """more::oclStore12 (builtins_more_internal.h): the kernel's 8-bit store, v cut to 0..1 and to 12 bits, scaled to a byte with the half down, as a float level / 255; not a number: 0"""
    t = S.min(S.max(v, S.f(0.0)), S.f(1.0)) * 4096.0
    n = S.to_i(S.floor(t))
    q = (n * 255 + 2047) >> 12
    return S.select(S.isnan(v), S.f(0.0), S.to_f(q) / 255.0)


def unorm_level(S, v):
    """more::oclUnormLevel(v, 0.46f): v kept in 0..1, floor(v 255 + 0.46) / 255"""
    c = fmin(S, fmax(S, v, S.f(0.0)), S.f(1.0))
    return S.floor(c * 255.0 + 0.46) / 255.0


def maybe_store(S, q8, v):
    return S.select(q8, store12(S, v), v)


def _coef_fields():
    f = []
    for nm in ('n', 'd', 'm', 'bn', 'bm'):
        f += [('%s%d' % (nm, k), 'f') for k in range(5)]
    return f


# ---------------------------------------------------------------------------------------------------------- the recursive Gaussian (vfxgauss.h passColumns)
def _vg_line(fused):
    def vg_line(S):
        _vg_line_body(S, fused)
    return vg_line

def _vg_line_body(S, fused):
    """vfxgauss::passColumns(), one thread per line: down every column (vert: from the BOTTOM row up, as the OpenCL kernels' frames lie bottom row first; the causal half, then the
    anti-causal half from the top row down) or along every row; the two halves are stored (the 12-bit store when flags & 1) and added, as the effects read their sum:
    out = store(causal) + store(anti-causal), stored again with flags & 2. fused: the card's fused multiply-add (Gaussian Blur and its three kin); else a multiply and an add (Radial Blur)."""
    P = S.p
    vert = S.ne(P.vert, 0)
    L = S.select(vert, P.h, P.w)
    lines = S.select(vert, P.w, P.h)
    q8 = S.ne(P.q8 & 1, 0)
    qs = S.ne(P.q8 & 2, 0)
    with S.if_(S.gx < lines):
        ln = S.gx
        src = S.buf('in0')
        out = S.buf('out')
        def idx(p): return S.select(vert, (P.h - 1 - p) * P.w + ln, ln * P.w + p)
        def co(nm, k): return getattr(P, '%s%d' % (nm, k))
        d = [co('d', k) for k in range(5)]
        fma = (lambda a, b, c: efma(S, a, b, c)) if fused else (lambda a, b, c: a * b + c)
        for anti in (False, True):
            nn = [co('m' if anti else 'n', k) for k in range(5)]
            ee = [co('bm' if anti else 'bn', k) for k in range(5)]
            def pos(i): return (L - 1 - i) if anti else i
            v = [[S.var('f') for _ in range(9)] for _ in range(4)]      # v11..v15, v16..v19
            y = [S.var('f') for _ in range(4)]                          # v22
            def put(i, vals):
                a = idx(pos(i))
                st = [maybe_store(S, q8, t) for t in vals]
                if anti:
                    o = out.load(a)
                    sm = [o.x + st[0], o.y + st[1], o.z + st[2], o.w + st[3]]
                    out.store(a, S.vec4(*[maybe_store(S, qs, t) for t in sm]))
                else:
                    out.store(a, S.vec4(st[0], st[1], st[2], st[3]))
            x0v = src.load(idx(pos(S.u(0))))
            x0 = [x0v.x, x0v.y, x0v.z, x0v.w]
            for ch in range(4):
                V = v[ch]
                if not anti:
                    v15 = -ee[4] * x0[ch]
                else:
                    v15 = (nn[4] - ee[4]) * x0[ch]
                v14 = fma(nn[3] - ee[3], x0[ch], v15)
                v13 = fma(nn[2] - ee[2], x0[ch], v14)
                v12 = fma(nn[1] - ee[1], x0[ch], v13)
                v11 = fma(nn[0] - ee[0], x0[ch], v12)
                V[0].set(v11); V[1].set(v12); V[2].set(v13); V[3].set(v14); V[4].set(v15)
                y[ch].set(v11)
            put(S.u(0), [y[ch].get() for ch in range(4)])
            with S.if_(L >= 2):
                x1v = src.load(idx(pos(S.u(1))))
                x1 = [x1v.x, x1v.y, x1v.z, x1v.w]
                res = []
                for ch in range(4):
                    V = v[ch]
                    v22 = y[ch].get()
                    V[5].set(S.f(0.0))
                    V[6].set(v22 * d[0])
                    V[7].set(S.f(0.0))
                    V[8].set(S.f(0.0))
                    V[0].set(fma(x1[ch], nn[0], V[1].get()))
                    V[1].set(fma(x1[ch], nn[1], V[2].get()))
                    V[2].set(fma(x1[ch], nn[2], V[3].get()))
                    V[3].set(fma(x1[ch], nn[3], V[4].get()))
                    V[4].set(x1[ch] * nn[4] if anti else S.f(0.0))
                    n22 = V[0].get() - V[5].get()
                    y[ch].set(n22)
                    res.append(n22)
                put(S.u(1), res)
            with S.for_(2, L) as i:
                xiv = src.load(idx(pos(i)))
                xi = [xiv.x, xiv.y, xiv.z, xiv.w]
                res = []
                for ch in range(4):
                    V = v[ch]
                    v22 = y[ch].get()
                    V[5].set(fma(v22, d[1], V[6].get()))
                    V[6].set(fma(v22, d[2], V[7].get()))
                    V[7].set(fma(v22, d[3], V[8].get()))
                    V[8].set(v22 * d[4])
                    V[0].set(fma(xi[ch], nn[0], V[1].get()))
                    V[1].set(fma(xi[ch], nn[1], V[2].get()))
                    V[2].set(fma(xi[ch], nn[2], V[3].get()))
                    V[3].set(fma(xi[ch], nn[3], V[4].get()))
                    V[4].set(xi[ch] * nn[4] if anti else S.f(0.0))
                    n22 = V[0].get() - V[5].get()
                    y[ch].set(n22)
                    res.append(n22)
                put(i, res)


kernel("vg_line", [('w', 'u'), ('h', 'u'), ('vert', 'u'), ('q8', 'u')] + _coef_fields(), [PIC_IN, PIC_OUT], local=(64, 1))(_vg_line(True))
kernel("vg_line_mad", [('w', 'u'), ('h', 'u'), ('vert', 'u'), ('q8', 'u')] + _coef_fields(), [PIC_IN, PIC_OUT], local=(64, 1))(_vg_line(False))


# ---------------------------------------------------------------------------------------------------------- the little kernels after the recursions (vfxblurs.h)
@kernel("vg_gauss", WH + [('mask', 'u'), ('q8', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def vg_gauss(S):
    """vfxblurs::gaussianBlur's last step: the blur (in1, the sum of the two halves) where the channel is on, else the source (in0); then the store"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        b = S.buf('in1').load(i)
        q8 = S.ne(P.q8, 0)
        sc = [s.x, s.y, s.z, s.w]
        bc = [b.x, b.y, b.z, b.w]
        o = [maybe_store(S, q8, S.select(S.ne(P.mask & (1 << k), 0), bc[k], sc[k])) for k in range(4)]
        S.buf('out').store(i, S.vec4(o[0], o[1], o[2], o[3]))


@kernel("vg_unsharp", WH + [('amount', 'f'), ('thr', 'f'), ('q8', 'u'), ('pad', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def vg_unsharp(S):
    """vfxblurs::unsharpMask's last step: v = s + amount (s - b); taken where |v - b| > threshold, else the source; every channel; then the store"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        b = S.buf('in1').load(i)
        q8 = S.ne(P.q8, 0)
        sc = [s.x, s.y, s.z, s.w]
        bc = [b.x, b.y, b.z, b.w]
        o = []
        for k in range(4):
            v = sc[k] + P.amount * (sc[k] - bc[k])
            r = S.select(S.abs(v - bc[k]) > P.thr, v, sc[k])
            o.append(maybe_store(S, q8, r))
        S.buf('out').store(i, S.vec4(o[0], o[1], o[2], o[3]))


@kernel("vg_glow", WH + [('supp', 'f'), ('inten', 'f'), ('c0', 'f'), ('c1', 'f'), ('c2', 'f'), ('q8', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def vg_glow(S):
    """vfxblurs::glow's last step: m = clamp(((b.r + b.g + b.b) / 3 - suppression) x intensity, 0, 1); out = s + (colour - s) m (colour alpha 1); then the store"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        b = S.buf('in1').load(i)
        q8 = S.ne(P.q8, 0)
        avg = (b.x + b.y + b.z) / 3.0
        m = (avg - P.supp) * P.inten
        m = fmin(S, fmax(S, m, S.f(0.0)), S.f(1.0))
        col = [P.c0, P.c1, P.c2, S.f(1.0)]
        sc = [s.x, s.y, s.z, s.w]
        o = [maybe_store(S, q8, sc[k] + (col[k] - sc[k]) * m) for k in range(4)]
        S.buf('out').store(i, S.vec4(o[0], o[1], o[2], o[3]))


@kernel("vg_yuv", WH + [('q8', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def vg_yuv(S):
    """vfxblurs::chromaBlur's first step: Y U V (U, V with 0.5 added), alpha kept; stored"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        q8 = S.ne(P.q8, 0)
        r, g, b = s.x, s.y, s.z
        y = S.f(0.299) * r + S.f(0.587) * g + S.f(0.114) * b
        u = S.f(-0.14713) * r + S.f(-0.28886) * g + S.f(0.436) * b + S.f(0.5)
        v = S.f(0.615) * r + S.f(-0.51499) * g + S.f(-0.10001) * b + S.f(0.5)
        S.buf('out').store(i, S.vec4(maybe_store(S, q8, y), maybe_store(S, q8, u), maybe_store(S, q8, v), maybe_store(S, q8, s.w)))


@kernel("vg_chroma_out", WH + [('q8', 'u'), ('pad', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def vg_chroma_out(S):
    """vfxblurs::chromaBlur's last steps: Y and alpha of the converted picture (in0), U and V from the blur (in1), stored; then back to RGB, stored"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        yv = S.buf('in0').load(i)
        bl = S.buf('in1').load(i)
        q8 = S.ne(P.q8, 0)
        my = maybe_store(S, q8, yv.x)
        mu = maybe_store(S, q8, bl.y)
        mv = maybe_store(S, q8, bl.z)
        ma = maybe_store(S, q8, yv.w)
        u = mu - S.f(0.5)
        v = mv - S.f(0.5)
        z = S.f(0.0)
        r = S.f(1.0) * my + z * u + S.f(1.13983) * v
        g = S.f(1.0) * my + S.f(-0.39465) * u + S.f(-0.58060) * v
        b = S.f(1.0) * my + S.f(2.03211) * u + z * v
        S.buf('out').store(i, S.vec4(maybe_store(S, q8, r), maybe_store(S, q8, g), maybe_store(S, q8, b), maybe_store(S, q8, ma)))


# ---------------------------------------------------------------------------------------------------------- shared pieces of the OpenCL ports
def ocl_linear4(S, buf, W, H, px, py):
    """more::oclLinear4 (builtins_more_internal.h): the card's CLK_FILTER_LINEAR read at pixel coordinates (no half pixel), weights cut to 1/256, edges repeated. The picture is held TOP row
    first in 'buf' (the CPU's rows run bottom first: its row y is row H-1-y here); W, H are uints; returns a vec4."""
    flx, fly = S.floor(px), S.floor(py)
    fx = S.floor((px - flx) * 256.0 + 0.5) * 0.00390625
    fy = S.floor((py - fly) * 256.0 + 0.5) * 0.00390625
    Wi, Hi = S.to_i(W), S.to_i(H)
    def start(fl, n):
        t = S.select(fl < S.to_f(n + 1), fl, S.to_f(n + 1))          # std::min((double)n + 1, fl)
        t = S.select(S.f(-2.0) < t, t, S.f(-2.0))                     # std::max(-2.0, .)
        return S.to_i(t)
    x0, y0 = start(flx, Wi), start(fly, Hi)
    def cl(v, n): return S.max(S.min(v, n - 1), S.i(0))
    xa, xb = cl(x0, Wi), cl(x0 + 1, Wi)
    ya, yb = cl(y0, Hi), cl(y0 + 1, Hi)
    def at(x, y): return buf.load(S.to_u((Hi - 1 - y) * Wi + x))
    a, b, c, d = at(xa, ya), at(xb, ya), at(xa, yb), at(xb, yb)
    t0 = a + (b - a) * fx
    t1 = c + (d - c) * fx
    return t0 + (t1 - t0) * fy


def f_ratio(S, a_u, b_u):
    """(float)a / (float)b"""
    return S.to_f(a_u) / S.to_f(b_u)


# ---------------------------------------------------------------------------------------------------------- Radial Blur (radialBlurOpenCl in builtins_more_blur.cpp)
@kernel("rbo_polar", WH + [('q8', 'u'), ('pad', 'u'), ('v56x', 'f'), ('v56y', 'f')], [PIC_IN, PIC_OUT, TABLE])
def rbo_polar(S):
    """the picture unwrapped to polar form (columns = angle, rows = radius); q8 1: the 12-bit store (Radial Blur), 2: oclUnormLevel (Radial Pixelate); thread (gx, gy) is the CPU's pixel (x = gx, y = H-1-gy)"""
    P = S.p
    with S.in_range(P.w, P.h):
        y = P.h - 1 - S.gy
        pi, two = S.f(3.14159274), S.f(2.828427125)
        yn = f_ratio(S, y, P.h)
        tb = S.buf('table')
        sn, cs = tb.load(S.gx * 2), tb.load(S.gx * 2 + 1)     # std::sin / std::cos of the column's angle, from the host (the CPU's libm: sinf is not always the rounded value)
        r = yn * two
        px = r * sn + P.v56x
        py = r * cs + P.v56y
        px = (px / 2.0 + 0.5) * S.to_f(P.w)
        py = (py / 2.0 + 0.5) * S.to_f(P.h)
        o = ocl_linear4(S, S.buf('in0'), P.w, P.h, px, py)
        res = [S.select(S.eq(P.q8, 1), store12(S, o[k]), S.select(S.eq(P.q8, 2), unorm_level(S, o[k]), o[k])) for k in range(4)]
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(*res))


@kernel("rbo_rec", WH + [('type', 'u'), ('q8', 'u'), ('s0', 'f'), ('s1', 'f'), ('wt', 'f'), ('pad', 'u')], [PIC_IN, PIC_OUT], local=(64, 1))
def rbo_rec(S):
    """the one-pole recursion outwards along every column of the polar picture (Proportional: type 0, Fixed: type 1): row y = row(y) + (row(y - back) - row(y)) w, the rows already
    blurred where back > 0; then the 12-bit store of the whole picture (q8)"""
    P = S.p
    with S.if_(S.gx < P.w):
        src, out = S.buf('in0'), S.buf('out')
        Hi = S.to_i(P.h)
        def idx(y): return S.to_u((Hi - 1 - y) * S.to_i(P.w) + S.to_i(S.gx))
        with S.for_(0, Hi, 1, True) as y:
            p = src.load(idx(y))
            yf = S.to_f(y)
            b1 = S.select(yf <= P.s0, S.i(0), y - S.to_i(S.round_even(yf * (P.s0 / yf))))
            b0 = y - S.to_i(S.round_even(yf * P.s1))
            back = S.select(S.eq(P.type, 1), b1, b0)
            back = S.max(S.min(back, y), S.i(0))
            bk = S.select(S.eq(back, y), p, out.load(idx(back)))
            out.store(idx(y), p + (bk - p) * P.wt)
        with S.if_(S.ne(P.q8, 0)):
            with S.for_(0, Hi, 1, True) as y:
                v = out.load(idx(y))
                out.store(idx(y), S.vec4(store12(S, v.x), store12(S, v.y), store12(S, v.z), store12(S, v.w)))


@kernel("rbo_wrap", WH + [('q8', 'u'), ('pad', 'u'), ('v56x', 'f'), ('v56y', 'f')], [PIC_IN, PIC_OUT, TABLE])
def rbo_wrap(S):
    """the blurred polar picture wrapped back to the frame"""
    P = S.p
    with S.in_range(P.w, P.h):
        y = P.h - 1 - S.gy
        pi, two = S.f(3.14159274), S.f(2.828427125)
        xn = f_ratio(S, S.gx, P.w)
        yn = f_ratio(S, y, P.h)
        vx = (xn / 0.5 - 1.0) - P.v56x
        vy = (yn / 0.5 - 1.0) - P.v56y
        a2 = S.buf('table').load(S.gy * P.w + S.gx)     # std::atan2(vx, vy) from the host (the CPU's libm)
        r2 = S.sqrt(vx * vx + vy * vy)
        u = ((a2 / pi) / 2.0 + 0.5) * S.to_f(P.w)
        vv = (r2 / two) * S.to_f(P.h)
        o = ocl_linear4(S, S.buf('in0'), P.w, P.h, u, vv)
        q = S.ne(P.q8, 0)
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(*[maybe_store(S, q, o[k]) for k in range(4)]))


# ---------------------------------------------------------------------------------------------------------- Linear Blur (vfxlinblur.h run())
@kernel("lbo_turn", [('ow', 'u'), ('oh', 'u'), ('sw', 'u'), ('sh', 'u'), ('mode', 'u'), ('pad', 'u'), ('s', 'f'), ('c', 'f'), ('cx', 'f'), ('cy', 'f'), ('ox', 'f'), ('oy', 'f')], [PIC_IN, PIC_OUT])
def lbo_turn(S):
    """the picture turned: thread (gx, gy) of the ow x oh output is the CPU's pixel (x = gx, y = oh-1-gy); u = (x-cx) c + (y-cy) s + cx, v = -((x-cx) s) + (y-cy) c + cy, read from
    the sw x sh source at (u + ox, v + oy) (the offsets signed by the host); mode 0: as read, 1: the 12-bit store, 2: kept in 0..1"""
    P = S.p
    with S.in_range(P.ow, P.oh):
        y = P.oh - 1 - S.gy
        xf, yf = S.to_f(S.gx), S.to_f(y)
        u = (xf - P.cx) * P.c + (yf - P.cy) * P.s + P.cx
        v = (-((xf - P.cx) * P.s)) + (yf - P.cy) * P.c + P.cy
        o = ocl_linear4(S, S.buf('in0'), P.sw, P.sh, u + P.ox, v + P.oy)
        res = []
        for k in range(4):
            res.append(S.select(S.eq(P.mode, 1), store12(S, o[k]), S.select(S.eq(P.mode, 2), fmin(S, fmax(S, o[k], S.f(0.0)), S.f(1.0)), o[k])))
        S.buf('out').store(S.gy * P.ow + S.gx, S.vec4(*res))


@kernel("ocl_final", WH + [('mode', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def ocl_final(S):
    """a kernel's last write of a picture that passes through: mode 1: the 12-bit store; 2: kept in 0..1"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        o = S.buf('in0').load(i)
        res = []
        for k in range(4):
            res.append(S.select(S.eq(P.mode, 1), store12(S, o[k]), S.select(S.eq(P.mode, 2), fmin(S, fmax(S, o[k], S.f(0.0)), S.f(1.0)), o[k])))
        S.buf('out').store(i, S.vec4(*res))


# ---------------------------------------------------------------------------------------------------------- Radial Pixelate (vfxpolar.h radialPixelate)
@kernel("rpo_groups", WH + [('type', 'u'), ('q8', 'u'), ('g', 'f'), ('pad', 'u')], [PIC_IN, PIC_OUT], local=(64, 1))
def rpo_groups(S):
    """step 2: the means of the groups of the polar picture along every column (type 1) or row; a group ends where fmod(i + 1, g) < 1 or at the line's end; the means are 8-bit (q8)"""
    P = S.p
    vert = S.eq(P.type, 1)
    L = S.select(vert, P.h, P.w)
    lines = S.select(vert, P.w, P.h)
    q = S.ne(P.q8, 0)
    with S.if_(S.gx < lines):
        ln = S.gx
        src, out = S.buf('in0'), S.buf('out')
        def idx(pos): return S.select(vert, (P.h - 1 - pos) * P.w + ln, ln * P.w + pos)
        sm = [S.var('f', 0.0) for _ in range(4)]
        n = S.var('u', 0)
        with S.for_(0, L) as i:
            n.set(n.get() + 1)
            p = src.load(idx(i))
            for k in range(4):
                sm[k].set(sm[k].get() + p[k])
            end = (S.fmod(S.to_f(i) + 1.0, P.g) < 1.0) | S.eq(i, L - 1)
            with S.if_(end):
                nf = S.to_f(n.get())
                avg = [sm[k].get() / nf for k in range(4)]
                v = S.vec4(*[S.select(q, unorm_level(S, a), a) for a in avg])
                with S.for_(0, n.get()) as m:
                    out.store(idx(i - m), v)
                for k in range(4):
                    sm[k].set(S.f(0.0))
                n.set(S.u(0))


# ---------------------------------------------------------------------------------------------------------- Fill Light (vfxfilllight.h render)
@kernel("flo_map", WH, [PIC_IN, PIC_OUT, TABLE])
def flo_map(S):
    """kernels 588 / 589: the luma of the pixel picks its colour from the 256 entry table (the table holds the bytes as floats, 4 a level); out = byte / 255"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        l = p.x * 0.299
        l = l + p.y * 0.587
        l = l + p.z * 0.114
        l = l + S.f(0.0) * S.f(0.0)
        idx = S.to_i(l * 255.0 + 0.5)
        idx = S.min(S.max(idx, S.i(0)), S.i(255))
        t = S.buf('table')
        b = S.to_u(idx) * 4
        S.buf('out').store(i, S.vec4(t.load(b) / 255.0, t.load(b + 1) / 255.0, t.load(b + 2) / 255.0, t.load(b + 3) / 255.0))


@kernel("flo_final", WH + [('k', 'f'), ('q8', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def flo_final(S):
    """kernel 5810: the picture (in0) plus the fill (in1) times k, kept in 0..1, the alpha untouched; the 12-bit store (q8)"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        x = S.buf('in1').load(i)
        q = S.ne(P.q8, 0)
        sc, xc = [s.x, s.y, s.z], [x.x, x.y, x.z]
        o = []
        for c in range(3):
            v = xc[c] * P.k + sc[c]
            v = S.min(S.max(v, S.f(0.0)), S.f(1.0))
            o.append(maybe_store(S, q, v))
        S.buf('out').store(i, S.vec4(o[0], o[1], o[2], maybe_store(S, q, s.w)))


# ---------------------------------------------------------------------------------------------------------- Newsprint (vfxnews.h drawRow)
@kernel("nwo_draw", WH + [('type', 'u'), ('q8', 'u'), ('par', 'u'), ('fade', 'u'), ('v41x', 'f'), ('v41y', 'f'), ('v42x', 'f'), ('v42y', 'f'), ('v45x', 'f'), ('v45y', 'f'), ('fa', 'f'), ('pad', 'u')],
        [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def nwo_draw(S):
    """vfxnews::drawRow: in0 the picture, in1 its blurred copy, the table the 256 entries of the edge coverage; thread (gx, gy) is the CPU's pixel (x = gx, y = H-1-gy). type 0 Color, 1 Mono, 2 Offset."""
    P = S.p
    with S.in_range(P.w, P.h):
        Wi, Hi = S.to_i(P.w), S.to_i(P.h)
        X = S.to_f(S.gx)
        Y = S.to_f(P.h - 1 - S.gy)
        blur, tab = S.buf('in1'), S.buf('table')
        half = S.f(0.5)
        def rte(a): return S.round_even(a)
        def cell_x(off): return ((rte(X / P.v41x - off - P.v42x) + off) + P.v42x) * P.v41x
        cell_y = ((rte(Y / P.v41y - P.v42y) + P.v42y)) * P.v41y
        def tap_at(sx, sy):
            xi = S.min(S.max(S.to_i(sx), S.i(0)), Wi - 1)
            yi = S.min(S.max(S.to_i(sy), S.i(0)), Hi - 1)
            q = blur.load(S.to_u((Hi - 1 - yi) * Wi + xi))
            lum = 0.114 * q.x + 0.587 * q.y + 0.299 * q.z
            v60 = 1.0 - lum
            rx = P.v41x * P.v45x * 0.5 * v60 * 1.25
            ry = P.v41y * P.v45y * 0.5 * v60 * 1.25
            ok = (rx > 0.00392156862745098) & (ry > 0.00392156862745098)
            return q, rx, ry, ok
        def cover(cx, cy, rx, ry):
            dx = (X - cx) * P.v45x
            dy = (Y - cy) * P.v45y
            d = S.sqrt(dx * dx + dy * dy)
            idx = S.to_i((d - rx) * 255.0 + 128.0)
            idx = S.min(S.max(idx, S.i(0)), S.i(255))
            tv = tab.load(S.to_u(idx))
            zero = (d > rx + 0.5) & (d > ry + 0.5)
            one = (d < rx - 0.5) & (d < ry - 0.5)
            return S.select(zero, S.f(0.0), S.select(one, S.f(1.0), tv))
        acc = [S.var('f', 1.0) for _ in range(3)]
        row = rte(Y / P.v41y - P.v42y)
        base = S.ne((S.to_i(row) + S.to_i(P.par)) & 1, 0)
        p58 = S.select(base, half, S.f(0.0))
        def flip(v): return S.select(v > 0.0, S.f(0.0), half)
        with S.if_(S.ne(P.type, 2)):
            mono = S.eq(P.type, 1)
            def tap(sx, sy):
                q, rx, ry, ok = tap_at(sx, sy)
                c = cover(sx, sy, rx, ry)
                pk = [q.x, q.y, q.z]
                for k in range(3):
                    d = S.select(mono, c, (1.0 - pk[k]) * c)
                    acc[k].set(S.select(ok, acc[k].get() - d, acc[k].get()))
            vx = cell_x(p58)
            vy = cell_y
            tap(vx, vy)
            vx = vx - P.v41x; tap(vx, vy)
            vx = vx + 2.0 * P.v41x; tap(vx, vy)
            p2 = flip(p58)
            vx = cell_x(p2); vy = cell_y - P.v41y; tap(vx, vy)
            vy = vy + 2.0 * P.v41y; tap(vx, vy)
        with S.else_():
            zs = [(0, 0, 0), (0, 1, 0), (1, 0, -1), (0, 1, -1)]
            ys = [(0, 0, 0), (1, 0, -1), (1, -1, 0), (1, -1, -1)]
            xs = [(0, 0, 0), (1, 0, 1), (1, 1, 0), (0, -1, 0)]
            for chn in range(3):
                ch = 2 if chn == 0 else (1 if chn == 1 else 0)
                seq = zs if chn == 0 else (ys if chn == 1 else xs)
                pp = p58
                for (fl, dx, dy) in seq:
                    if fl: pp = flip(pp)
                    sx, sy = cell_x(pp), cell_y
                    if dx: sx = sx + float(dx) * P.v41x
                    if dy: sy = sy + float(dy) * P.v41y
                    q, rx, ry, ok = tap_at(sx, sy)
                    if ch == 2: cx, cy = sx - rx / 4.0, sy + ry / 4.0
                    elif ch == 1: cx, cy = sx + rx / 4.0, sy + ry / 4.0
                    else: cx, cy = sx, sy - ry / 4.0
                    c = cover(cx, cy, rx, ry)
                    pk = [q.x, q.y, q.z][ch]
                    acc[ch].set(S.select(ok, acc[ch].get() - (1.0 - pk) * c, acc[ch].get()))
        s = S.buf('in0').load(S.gy * P.w + S.gx)
        a = [acc[k].get() for k in range(3)]
        with S.if_(S.ne(P.fade, 0)):
            l = 0.114 * s.z + 0.587 * s.y + 0.299 * s.x
            sc = [s.x, s.y, s.z]
            for k in range(3):
                tgt = S.select(S.eq(P.type, 1), l, sc[k])
                acc[k].set(a[k] + (tgt - a[k]) * P.fa)
        q8 = S.ne(P.q8, 0)
        r = [maybe_store(S, q8, acc[k].get()) for k in range(3)]
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(r[0], r[1], r[2], s.w))


# ---------------------------------------------------------------------------------------------------------- Cookie Cutter (vfxcookie.h composite)
@kernel("cko_apply", WH + [('nob', 'u'), ('cut', 'u'), ('q8', 'u'), ('pad', 'u'), ('br', 'f'), ('bg', 'f'), ('bb', 'f'), ('pad2', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def cko_apply(S):
    """vfxcookie::composite: in0 the picture, in1 the (feathered) mask in .x; fused mixes (fma(b - a, t, a)); the 12-bit store (q8)"""
    import struct
    def kf(bits): return S.f(struct.unpack("<f", struct.pack("<I", bits))[0])
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        m = S.buf('in1').load(i).x
        nob, cut = S.ne(P.nob, 0), S.ne(P.cut, 0)
        q8 = S.ne(P.q8, 0)
        sc = [s.x, s.y, s.z, s.w]
        col = [P.br, P.bg, P.bb, S.f(1.0)]
        lo = m < kf(0x3727c5ac)
        hi = m > kf(0x3fffffac)
        le1 = S.f(1.0) >= m
        res = []
        for k in range(4):
            A = S.select(cut, sc[k], S.f(0.0))
            B = S.select(cut, S.f(0.0), sc[k])
            a = S.select(nob | le1, B, col[k])
            b = S.select(nob, A, S.select(le1, col[k], A))
            t = S.select(nob, m * 0.5, S.select(le1, m, m - 1.0))
            mixed = efma(S, b - a, t, a)
            r = S.select(lo, B, S.select(hi, A, mixed))
            res.append(maybe_store(S, q8, r))
        S.buf('out').store(i, S.vec4(*res))


# ---------------------------------------------------------------------------------------------------------- Lens Flare (vfxflare.h render / shape)
@kernel("lfo_draw", WH + [('n', 'u'), ('q8', 'u'), ('blend', 'f'), ('iblend', 'f')], [PIC_IN, PIC_OUT, TABLE])
def lfo_draw(S):
    """vfxflare::render: every element in turn on the pixel (a pixel only depends on its own earlier value). Table: 64 ray lengths, the hexagon (vx, vy, n0, n1: 6 each), then 16 floats an
    element (type, strength, ox, oy, R, Rx, xa, xb, ya, yb, colour 3). q8: the 8-bit store of the writes (and of the picture before the first element)."""
    P = S.p
    with S.in_range(P.w, P.h):
        t = S.buf('table')
        i = S.gy * P.w + S.gx
        p0 = S.buf('in0').load(i)
        q = S.ne(P.q8, 0)
        c = [S.var('f', 0.0) for _ in range(4)]
        for k in range(4):
            c[k].set(maybe_store(S, q, p0[k]))
        xf = S.to_f(S.gx)
        yi = S.to_i(S.gy)
        xi = S.to_i(S.gx)
        pi = S.f(3.14159265358979323846)
        with S.for_(0, P.n) as e:
            base = 88 + e * 16
            def T(k): return t.load(base + k)
            xa, xb, ya, yb = S.to_i(T(6)), S.to_i(T(7)), S.to_i(T(8)), S.to_i(T(9))
            with S.if_((xi >= xa) & (xi < xb) & (yi >= ya) & (yi < yb)):
                ty = S.to_i(T(0))
                strength, ox, oy, R, Rx = T(1), T(2), T(3), T(4), T(5)
                col = [T(10), T(11), T(12)]
                v = S.f(1.0) - ((S.to_f(yi + 1) - oy) / R)
                u = S.f(1.0) - ((xf - ox) / Rx)
                ud, vd = SF.from_f32(S, u), SF.from_f32(S, v)
                d = (ud * ud + vd * vd).sqrt().to_f32()
                ok = S.var('u', 0)
                sh = S.var('f', 0.0)
                pre = S.var('u', 0)            # 1: colour * s * strength, 2: colour * strength * s
                # hexagon: the smallest signed distance to the six edges
                m = S.var('f', 0.0)
                is_hex = S.eq(ty, 3) | S.eq(ty, 4)
                with S.if_(is_hex):
                    def edge(j):
                        return (u - t.load(64 + j)) * t.load(64 + 12 + j) + (v - t.load(64 + 6 + j)) * t.load(64 + 18 + j)
                    m.set(edge(0))
                    for j in range(1, 6):
                        s_ = edge(j)
                        m.set(S.select(s_ < m.get(), s_, m.get()))
                one = S.f(1.0)
                with S.if_(S.eq(ty, 0)):
                    with S.if_(d < 1.0):
                        ok.set(S.u(1)); pre.set(S.u(1)); sh.set(one - d)
                with S.if_(S.eq(ty, 1)):
                    with S.if_(d < 1.0):
                        ok.set(S.u(1)); pre.set(S.u(1))
                        sh.set(S.select(d > 0.8, (one - d) * 5.0, S.select(d > 0.75, 0.75 + ((d - 0.75) * 5.0), d)))
                with S.if_(S.eq(ty, 2)):
                    with S.if_((d < 1.0) & (d > 0.85)):
                        ok.set(S.u(1)); pre.set(S.u(1))
                        tt = one - d
                        sh.set(S.select(tt < 0.1, tt * 10.0, (S.f(0.15) - tt) * 20.0))
                with S.if_(S.eq(ty, 3)):
                    with S.if_(m.get() >= 0.0):
                        ok.set(S.u(1)); pre.set(S.u(1))
                        a_ = m.get() / 0.2
                        b_ = (S.f(0.8) - (m.get() - 0.2)) / 0.8
                        sh.set(S.select(m.get() < 0.2, fmin(S, fmax(S, a_, S.f(0.0)), one), fmin(S, fmax(S, b_, S.f(0.0)), one)))
                with S.if_(S.eq(ty, 4)):
                    with S.if_(m.get() >= 0.0):
                        ok.set(S.u(1)); pre.set(S.u(1)); sh.set(m.get() * m.get())
                with S.if_(ty >= 5):
                    with S.if_(d < 1.0):
                        from sf64 import sf_atan2
                        ang = sf_atan2(S, vd, ud).to_f32()
                        a2 = ang + pi
                        pp = a2 / (2.0 * pi) * 3.0
                        pv = pp * 60.0
                        fr = pv - S.trunc(pv)
                        ii = S.max(S.to_i(pv), S.i(0))
                        w = t.load(S.to_u(ii % 64)) * (one - fr) + t.load(S.to_u((ii + 1) % 64)) * fr
                        lim = one * w
                        qq = S.select(d <= 0.0, S.f(0.0), S.select(d >= lim, one, (d - 0.0) / (lim - 0.0)))
                        s3 = one - qq
                        s3 = s3 * s3 * s3
                        s3 = S.select(d < 0.3, one + ((s3 - one) * (d / 0.3)), s3)
                        ok.set(S.u(1)); pre.set(S.u(2)); sh.set(s3)
                with S.if_(S.ne(ok.get(), 0)):
                    eff = [S.select(S.eq(pre.get(), 1), col[k] * sh.get() * strength, col[k] * strength * sh.get()) for k in range(3)]
                    a = c[3].get()
                    r3 = [c[k].get() for k in range(3)]
                    n3 = [S.var('f', 0.0) for _ in range(4)]
                    for k in range(3):
                        t1 = r3[k] / a
                        t1 = t1 * P.iblend + (eff[k] + t1) * P.blend
                        full = r3[k] * P.iblend + (eff[k] + r3[k]) * P.blend
                        v3 = S.select(a < 1.0, t1 * a, full)
                        n3[k].set(S.select(a > 0.000001, v3, S.f(0.0)))
                    n3[3].set(S.select(a > 0.000001, S.select(a < 1.0, a, one), S.f(0.0)))
                    for k in range(4):
                        c[k].set(maybe_store(S, q, n3[k].get()))
        S.buf('out').store(i, S.vec4(c[0].get(), c[1].get(), c[2].get(), c[3].get()))


# ---------------------------------------------------------------------------------------------------------- Denoise (vfxdenoise.h; all whole numbers but the colour matrices)
def store12i(S, v):
    """vfxdn::store12: the card's write to an 8-bit picture as a byte (an int)"""
    c = S.select(v > 0.0, fmin(S, v, S.f(1.0)), S.f(0.0))
    m = S.to_i(S.floor(c * 4096.0))
    return S.select(v > 0.0, (m * 255 + 2047) >> 12, S.i(0))


def dn_byte_of(S, v):
    t = S.f(255.0) * v
    return S.select(t <= 0.0, S.f(0.0), S.select(t >= 255.0, S.f(255.0), S.to_f(S.to_i(t))))


def dn_level_in(S, v, host8):
    c = S.select(v > 0.0, fmin(S, v, S.f(1.0)), S.f(0.0))
    return S.select(host8, S.to_f(S.to_i(c * 255.0 + 0.5)) / 255.0, c)


@kernel("dn_yuv", WH + [('host8', 'u'), ('mode', 'u')], [PIC_IN, PIC_OUT])
def dn_yuv(S):
    """steps 1 + 2: the picture as the 8-bit picture the card holds (host8), its YUV (BT.601) cut to bytes (mode 0: the bytes of Y, U, V, alpha as floats; mode 1: only the picture itself, no YUV)"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        h8 = S.ne(P.host8, 0)
        r, g, b, a = [dn_level_in(S, p[k], h8) for k in range(4)]
        y = S.f(0.299) * r
        y = y + S.f(0.587) * g
        y = y + S.f(0.114) * b
        u = S.f(-0.14713) * r
        u = u + S.f(-0.28886) * g
        u = u + S.f(0.436) * b
        u = u + S.f(0.5)
        v = S.f(0.615) * r
        v = v + S.f(-0.51499) * g
        v = v + S.f(-0.10001) * b
        v = v + S.f(0.5)
        def st(c): return S.select(h8, S.to_f(store12i(S, c)) / 255.0, c)
        bytes_ = S.vec4(dn_byte_of(S, st(y)), dn_byte_of(S, st(u)), dn_byte_of(S, st(v)), dn_byte_of(S, st(a)))
        S.buf('out').store(i, S.select(S.eq(P.mode, 1), S.vec4(r, g, b, a), bytes_))


@kernel("dn_pad", WH + [('ow', 'u'), ('oh', 'u')], [PIC_IN, PIC_OUT, TABLE])
def dn_pad(S):
    """step 2's planes with their 13 pixel border: thread (gx, gy) = (column, row) of the padded plane (rows as the card holds them, bottom first); the table holds the source column
    and row (texel()) of every padded column then row; w, h here are the padded size, ow, oh the picture's"""
    P = S.p
    with S.in_range(P.w, P.h):
        t = S.buf('table')
        cx = S.to_u(t.load(S.gx))
        cy = S.to_u(t.load(P.w + S.gy))
        v = S.buf('in0').load((P.oh - 1 - cy) * P.ow + cx)
        S.buf('out').store(S.gy * P.w + S.gx, v)


@kernel("dn_nlm", WH + [('plane', 'u'), ('en', 'u'), ('ew', 'u'), ('dy0', 'u'), ('dy1', 'u'), ('pad', 'u')], [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def dn_nlm(S):
    """step 3 for one plane, the rows dy0 - 10 .. dy1 - 11 of the 21 x 21 search (dispatches of a few rows: a long dispatch is cut short by a card's watchdog): in0 the padded planes (EW wide,
    bottom row first), in1 the sums so far (the whole numbers num, den as float bits in x, y; none before dy0 = 0). The weights are the table (196); 7 x 7 patch of absolute differences.
    A plane that is not denoised (en 0) has num = its byte, den = 1. Thread (gx, gy) is the picture's pixel (x = gx, row y = H - 1 - gy)."""
    P = S.p
    with S.in_range(P.w, P.h):
        t = S.buf('table')
        pad = S.buf('in0')
        B = 13
        ew = P.ew
        y = P.h - 1 - S.gy
        x = S.gx
        def pv(row, col):
            v = pad.load(row * ew + col)
            return S.to_i(S.select(S.eq(P.plane, 0), v.x, S.select(S.eq(P.plane, 1), v.y, v.z)))
        yb = S.to_i(y + B)
        xb = S.to_i(x + B)
        i = S.gy * P.w + S.gx
        num = S.var('u', 0)
        den = S.var('u', 0)
        with S.if_(S.eq(P.en, 0)):
            num.set(S.to_u(pv(S.to_u(yb), S.to_u(xb))))
            den.set(S.u(1))
        with S.if_(S.ne(P.en, 0)):
            prev = S.buf('in1').load(i)
            with S.if_(P.dy0 > 0):
                num.set(S.f_to_bits(prev.x))
                den.set(S.f_to_bits(prev.y))
            with S.for_(S.to_i(P.dy0) - 10, S.to_i(P.dy1) - 10, 1, True) as dy:
                with S.for_(S.i(-10), S.i(11), 1, True) as dx:
                    sm = S.var('u', 0)
                    with S.for_(S.i(-3), S.i(4), 1, True) as py:
                        with S.for_(S.i(-3), S.i(4), 1, True) as px:
                            a = pv(S.to_u(yb + py), S.to_u(xb + px))
                            b = pv(S.to_u(yb + py + dy), S.to_u(xb + px + dx))
                            sm.set(sm.get() + S.to_u(S.select(b > a, b - a, a - b)))
                    wi = S.min(sm.get() >> 6, S.u(195))
                    w = S.to_u(S.to_i(t.load(wi)))
                    cand = S.to_u(pv(S.to_u(yb + dy), S.to_u(xb + dx)))
                    den.set(den.get() + w)
                    num.set(num.get() + w * cand)
        S.buf('out').store(i, S.vec4(S.bits_to_f(num.get()), S.bits_to_f(den.get()), S.f(0.0), S.f(0.0)))


@kernel("dn_nlmfin", WH + [('plane', 'u'), ('pad', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def dn_nlmfin(S):
    """the mean of the sums (in0: num, den as float bits): the byte (whole number quotient, den 0: 0, & 255) into the channel 'plane' of in1's pixel (copied; the alpha byte stays)"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        a = S.buf('in0').load(i)
        num, den = S.f_to_bits(a.x), S.f_to_bits(a.y)
        q = S.select(den > 0, num / S.select(den > 0, den, S.u(1)), S.u(0))
        rv = S.to_f(q & 255)
        prev = S.buf('in1').load(i)
        S.buf('out').store(i, S.vec4(S.select(S.eq(P.plane, 0), rv, prev.x), S.select(S.eq(P.plane, 1), rv, prev.y),
                                     S.select(S.eq(P.plane, 2), rv, prev.z), prev.w))


@kernel("dn_merge", WH + [('host8', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def dn_merge(S):
    """step 4: the bytes of Y, U, V (byte / 255 in a double, to a float) and alpha back to RGB, into an 8-bit picture (host8)"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        h8 = S.ne(P.host8, 0)
        def lv(b): return (SF.from_f32(S, b) / 255.0).to_f32()
        Y, U, V, A = lv(p.x), lv(p.y), lv(p.z), lv(p.w)
        U = U - 0.5
        V = V - 0.5
        R = S.f(1.0) * Y
        R = R + S.f(0.0) * U
        R = R + S.f(1.13983) * V
        G = S.f(1.0) * Y
        G = G + S.f(-0.39465) * U
        G = G + S.f(-0.58060) * V
        Bv = S.f(1.0) * Y
        Bv = Bv + S.f(2.03211) * U
        Bv = Bv + S.f(0.0) * V
        def st(c): return S.select(h8, S.to_f(store12i(S, c)) / 255.0, c)
        S.buf('out').store(i, S.vec4(st(R), st(G), st(Bv), st(A)))


@kernel("dn_sharp", WH + [('host8', 'u'), ('pad', 'u'), ('c0', 'f'), ('dv', 'f')], [PIC_IN, PIC_OUT])
def dn_sharp(S):
    """step 5: the 3 x 3 sharpening (corners -1, edges -2, centre c0, divided by c0 - 12) over the four channels, edges repeated; mem rows as the card holds them"""
    P = S.p
    with S.in_range(P.w, P.h):
        src = S.buf('in0')
        h8 = S.ne(P.host8, 0)
        yi = S.to_i(P.h) - 1 - S.to_i(S.gy)
        xi = S.to_i(S.gx)
        ya = S.max(S.i(0), yi - 1)
        yb = S.min(S.to_i(P.h) - 1, yi + 1)
        xa = S.max(S.i(0), xi - 1)
        xb = S.min(S.to_i(P.w) - 1, xi + 1)
        def at(ym, xm): return src.load(S.to_u((S.to_i(P.h) - 1 - ym) * S.to_i(P.w) + xm))
        p00, p01, p02 = at(ya, xa), at(yi, xa), at(yb, xa)
        p10, p11, p12 = at(ya, xi), at(yi, xi), at(yb, xi)
        p20, p21, p22 = at(ya, xb), at(yi, xb), at(yb, xb)
        m1, m2 = S.f(-1.0), S.f(-2.0)
        out = []
        for k in range(4):
            s = m1 * p00[k]
            s = s + m2 * p01[k]
            s = s + m1 * p02[k]
            s = s + m2 * p10[k]
            s = s + P.c0 * p11[k]
            s = s + m2 * p12[k]
            s = s + m1 * p20[k]
            s = s + m2 * p21[k]
            s = s + m1 * p22[k]
            s = s / P.dv
            out.append(S.select(h8, S.to_f(store12i(S, s)) / 255.0, s))
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(*out))


# ---------------------------------------------------------------------------------------------------------- Layer Dimensionality (vfxlayerdim.h; pictures here are the GPU's, top row first)
def f32(x):
    import struct
    return struct.unpack('<f', struct.pack('<f', x))[0]


@kernel("ldo_alpha", WH, [PIC_IN, PIC_OUT])
def ldo_alpha(S):
    """kAlpha: the alpha in all four lanes"""
    with S.in_range(S.p.w, S.p.h):
        i = S.gy * S.p.w + S.gx
        a = S.buf('in0').load(i).w
        S.buf('out').store(i, S.vec4(a, a, a, a))


@kernel("ldo_fix", WH + [('col', 'u'), ('r', 'f'), ('g', 'f'), ('b', 'f')], [PIC_IN, PIC_OUT])
def ldo_fix(S):
    """blurAlpha's write back: the blurred alpha (lane w) in all lanes, or (col 1) the colour and the alpha"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        v = S.buf('in0').load(i).w
        c = S.ne(P.col, 0)
        S.buf('out').store(i, S.vec4(S.select(c, P.r, v), S.select(c, P.g, v), S.select(c, P.b, v), v))


@kernel("ldo_glow", WH + [('outer', 'u'), ('pad', 'u'), ('r', 'f'), ('g', 'f'), ('b', 'f'), ('pad2', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def ldo_glow(S):
    """kGlowInner / kGlowOuter: in0 the blurred alpha picture, in1 the frame; colour x v49, alpha 1 x v49"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        bl = S.buf('in0').load(i)
        fr = S.buf('in1').load(i)
        one = S.f(1.0)
        v = S.select(S.ne(P.outer, 0), bl.w * (one - fr.w), (one - bl.w) * fr.w)
        S.buf('out').store(i, S.vec4(P.r * v, P.g * v, P.b * v, one * v))


@kernel("ldo_add", WH, [PIC_IN, PIC_IN1, PIC_OUT])
def ldo_add(S):
    """kAdd: a + b, the alpha cut to 0 .. 1"""
    with S.in_range(S.p.w, S.p.h):
        i = S.gy * S.p.w + S.gx
        a = S.buf('in0').load(i)
        b = S.buf('in1').load(i)
        w = fmin(S, fmax(S, a.w + b.w, S.f(0.0)), S.f(1.0))
        S.buf('out').store(i, S.vec4(a.x + b.x, a.y + b.y, a.z + b.z, w))


@kernel("ldo_fill", WH + [('inv', 'u'), ('pad', 'u'), ('r', 'f'), ('g', 'f'), ('b', 'f'), ('amount', 'f')], [PIC_IN, PIC_OUT])
def ldo_fill(S):
    """kFill: w = (invert ? 1 - alpha : alpha) x amount; colour x w, w"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        a = S.buf('in0').load(i).w
        w = S.select(S.ne(P.inv, 0), S.f(1.0) - a, a)
        w = w * P.amount
        S.buf('out').store(i, S.vec4(P.r * w, P.g * w, P.b * w, w))


@kernel("ldo_shift", WH + [('sx', 'f'), ('sy', 'f')], [PIC_IN, PIC_OUT])
def ldo_shift(S):
    """kShift: out(x, y) = in(floor(x + sx), floor(y + sy)), zero outside (y counts rows from the bottom, as the card's)"""
    P = S.p
    with S.in_range(P.w, P.h):
        ym = S.to_f(P.h - 1 - S.gy)
        px = S.to_f(S.gx) + P.sx
        py = ym + P.sy
        ok = (px >= 0.0) & (px < S.to_f(P.w)) & (py >= 0.0) & (py < S.to_f(P.h))
        ix = S.to_i(S.floor(S.select(ok, px, S.f(0.0))))
        iy = S.to_i(S.floor(S.select(ok, py, S.f(0.0))))
        v = S.buf('in0').load(S.to_u(S.to_i(P.h) - 1 - iy) * P.w + S.to_u(ix))
        z = S.f(0.0)
        S.buf('out').store(S.gy * P.w + S.gx, S.select(ok, v, S.vec4(z, z, z, z)))


@kernel("ldo_mask", WH + [('inv', 'u'), ('amount', 'f')], [PIC_IN, PIC_IN1, PIC_OUT])
def ldo_mask(S):
    """kMask: in1 (the colour) x ((invert ? 1 - a : a) x amount), a the alpha of in0"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        a = S.buf('in0').load(i).w
        c = S.buf('in1').load(i)
        v = S.select(S.ne(P.inv, 0), S.f(1.0) - a, a)
        f = v * P.amount
        S.buf('out').store(i, S.vec4(c.x * f, c.y * f, c.z * f, c.w * f))


@kernel("ldo_over", WH + [('q8', 'u'), ('pad', 'u'), ('s54', 'f'), ('s55', 'f')], [PIC_IN, PIC_IN1, PIC_OUT])
def ldo_over(S):
    """kOver: the top picture x s54 over the bottom picture x s55, the colours taken as straight and weighed by alpha, then divided by the alpha again; q8: the 12-bit store of the result"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        tp = S.buf('in0').load(i)
        bt = S.buf('in1').load(i)
        t = [tp[k] * P.s54 for k in range(4)]
        b = [bt[k] * P.s55 for k in range(4)]
        one = S.f(1.0)
        o = [t[k] * t[3] + b[k] * b[3] * (one - t[3]) for k in range(3)]
        o3 = t[3] + b[3] * (one - t[3])
        div = o3 > 0.00001
        o = [S.select(div, o[k] / S.select(div, o3, one), o[k]) for k in range(3)]
        q = S.ne(P.q8, 0)
        S.buf('out').store(i, S.vec4(*[maybe_store(S, q, v) for v in o + [o3]]))


@kernel("ldo_scale", WH + [('q8', 'u'), ('pad', 'u'), ('s', 'f'), ('pad2', 'u')], [PIC_IN, PIC_OUT])
def ldo_scale(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        v = S.buf('in0').load(i)
        q = S.ne(P.q8, 0)
        S.buf('out').store(i, S.vec4(*[maybe_store(S, q, v[k] * P.s) for k in range(4)]))


def _ld_to_hsl(S, rgb, w):
    """toHsl (vfxlayerdim.h): the HSL helper with its nudges of one part in a million at pure black and white"""
    one, two, half = S.f(1.0), S.f(2.0), S.f(0.5)
    eps = S.f(0.000001)
    v60 = [S.var('f', 0.0) for _ in range(3)]
    for k in range(3): v60[k].set(rgb[k])
    v61 = S.var('f', 0.0)
    v62 = S.var('f', 0.0)
    v61.set(fmax(S, fmax(S, v60[0].get(), v60[1].get()), v60[2].get()))
    v62.set(fmin(S, fmin(S, v60[0].get(), v60[1].get()), v60[2].get()))
    def set_max(val):
        a, b, c = v60[0].get(), v60[1].get(), v60[2].get()
        m0 = (a > b) & (a > c)
        m1 = (~m0) & (b > c)
        m2 = (~m0) & (~m1)
        v60[0].set(S.select(m0, val, a)); v60[1].set(S.select(m1, val, b)); v60[2].set(S.select(m2, val, c))
    def set_min(val):
        a, b, c = v60[0].get(), v60[1].get(), v60[2].get()
        m0 = (a < b) & (a < c)
        m1 = (~m0) & (b < c)
        m2 = (~m0) & (~m1)
        v60[0].set(S.select(m0, val, a)); v60[1].set(S.select(m1, val, b)); v60[2].set(S.select(m2, val, c))
    s = v62.get() + v61.get()
    hi_lo = (s > S.f(f32(f32(2.0) - f32(0.000001)))) & (s < two)
    hi_hi = (~hi_lo) & (s >= two) & (s < S.f(f32(f32(2.0) + f32(0.000001))))
    with S.if_(hi_lo):
        v61.set(v61.get() - eps)
        set_max(v61.get())
    with S.if_(hi_hi):
        v61.set(v61.get() + eps)
        set_max(v61.get())
    s = v62.get() + v61.get()
    lo_lo = (s > S.f(f32(f32(0.0) - f32(0.000001)))) & (s < S.f(0.0))
    lo_hi = (~lo_lo) & (s >= S.f(0.0)) & (s < S.f(f32(f32(0.0) + f32(0.000001))))
    with S.if_(lo_lo):
        v62.set(v62.get() - eps)
        set_min(v62.get())
    with S.if_(lo_hi):
        v62.set(v62.get() + eps)
        set_min(v62.get())
    a61, a62 = v61.get(), v62.get()
    l = (a62 + a61) * half
    sat = a61 - a62
    nz = (sat < S.f(-0.000001)) | (sat >= eps)
    sat2 = S.select(l <= half, sat / (a62 + a61), sat / ((two - a61) - a62))
    sat2 = S.select((l < S.f(0.0)) | (l > one), sat2 * S.f(-1.0), sat2)
    v64 = one / (a61 - a62)
    c0, c1, c2 = v60[0].get(), v60[1].get(), v60[2].get()
    v65x, v65y, v65z = (a61 - c0) * v64, (a61 - c1) * v64, (a61 - c2) * v64
    h0 = S.select(S.eq(c0, a61), S.select(S.eq(c1, a62), S.f(5.0) + v65z, one - v65y),
                  S.select(S.eq(c1, a61), S.select(S.eq(c2, a62), one + v65x, S.f(3.0) - v65z),
                           S.select(S.eq(c0, a62), S.f(3.0) + v65y, S.f(5.0) - v65x)))
    h0 = h0 * S.f(f32(1.0 / 6.0))
    return S.select(nz, h0, S.f(0.0)), S.select(nz, sat2, sat), l


def _ld_from_hsl(S, h, sat, l, w):
    """fromHsl (vfxlayerdim.h)"""
    one, half = S.f(1.0), S.f(0.5)
    v66 = S.select(l <= half, l * (one + sat), (l + sat) - l * sat)
    v73 = h * 6.0
    v67 = (l + l) - v66
    v68 = S.to_i(v73)
    v69 = v73 - S.to_f(v68)
    v70 = (v66 - v67) * v69
    v71 = v67 + v70
    v72 = v66 - v70
    def pick(c0, c1, c2, c3, c4, c5, dflt):
        return S.select(S.eq(v68, 0) | S.eq(v68, 6), c0, S.select(S.eq(v68, 1), c1, S.select(S.eq(v68, 2), c2, S.select(S.eq(v68, 3), c3, S.select(S.eq(v68, 4), c4, S.select(S.eq(v68, 5), c5, dflt))))))
    r = pick(v66, v72, v67, v67, v71, v66, h)
    g = pick(v71, v66, v66, v72, v67, v67, sat)
    b = pick(v67, v67, v71, v66, v66, v72, l)
    pos = v66 > 0.0
    z = S.f(0.0)
    return S.select(pos, r, z), S.select(pos, g, z), S.select(pos, b, z), w


@kernel("ldo_light", WH + [('v85', 'f'), ('v86', 'f'), ('v87', 'f'), ('v88', 'f'), ('v89', 'f'), ('v90', 'f'), ('l0', 'f'), ('l1', 'f'), ('l2', 'f'), ('l3', 'f'), ('flat', 'f'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def ldo_light(S):
    """kLight: Sobel of the alpha of the picture, Phong lighting (the power as the double power rounded to a float), the picture's colour in HSL with its lightness x the light, the alpha |light - flat light|"""
    from sf64 import sf_pow
    P = S.p
    with S.in_range(P.w, P.h):
        src = S.buf('in0')
        Hi, Wi = S.to_i(P.h), S.to_i(P.w)
        yi = Hi - 1 - S.to_i(S.gy)
        xi = S.to_i(S.gx)
        y0, y2 = S.max(yi - 1, S.i(0)), S.min(yi + 1, Hi - 1)
        def at(xm, ym): return src.load(S.to_u((Hi - 1 - ym) * Wi + xm))
        edge = S.eq(xi, 0) | S.eq(xi, Wi - 1)
        xa = S.select(S.eq(xi, 0), xi, xi - 1)
        xb = S.min(S.select(S.eq(xi, 0), xi + 1, xi), Wi - 1)   # (a one pixel wide picture: column 0 on both sides, as the CPU)
        # the edge columns read xa and xb; the others x - 1 and x + 1
        xl = S.select(edge, xa, xi - 1)
        xr = S.select(edge, xb, xi + 1)
        p0, p1, p2 = at(xl, y0).w, at(xl, yi).w, at(xl, y2).w
        p3, p4, p5 = at(xr, y0).w, at(xr, yi).w, at(xr, y2).w
        p6, p7 = at(xi, y0).w, at(xi, y2).w
        gx = p0 - p3 + p1 - p4 + p2 - p5
        gy_edge = p2 - p0 + p5 - p3 + p5 - p3
        gy_mid = p2 - p0 + p5 - p3 + p7 - p6
        gy = S.select(edge, gy_edge, gy_mid)
        gx = gx * P.v85
        gy = gy * P.v85
        n0, n1, n2, n3 = gx, gy, S.f(1.0), S.f(0.0)
        ln = fsqrt(S, n0 * n0 + n1 * n1 + n2 * n2 + n3 * n3)
        inv = S.f(1.0) / ln
        n0, n1, n2, n3 = n0 * inv, n1 * inv, n2 * inv, n3 * inv
        dot = n0 * P.l0 + n1 * P.l1 + n2 * P.l2 + n3 * P.l3
        two = S.f(2.0) * dot
        r2 = n2 * two - P.l2
        v103 = fmax(S, r2, S.f(0.01))
        v103 = sf_pow(S, SF.from_f32(S, v103), SF.from_f32(S, P.v90)).to_f32()
        x = P.v88 * dot
        y = P.v89 * v103
        v91 = P.v87 * (x + y)
        v91 = v91 + P.v86
        c = src.load(S.gy * P.w + S.gx)
        h, sat, l = _ld_to_hsl(S, [c.x, c.y, c.z], c.w)
        l = l * v91
        z = l > 0.99999
        h = S.select(z, S.f(0.0), h)
        sat = S.select(z, S.f(0.0), sat)
        a = fmin(S, fmax(S, S.abs(v91 - P.flat), S.f(0.0)), S.f(1.0))
        r, g, b, w = _ld_from_hsl(S, h, sat, l, a)
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(r, g, b, w))


# ---------------------------------------------------------------------------------------------------------- Black Bar Fill (blackBarFill in builtins_more_creative.cpp, the float path)
@kernel("bbo_straight", WH, [PIC_IN, PIC_OUT])
def bbo_straight(S):
    """the straight (not premultiplied) picture: colour x (1 / alpha), 0 where the alpha is not above 0"""
    with S.in_range(S.p.w, S.p.h):
        i = S.gy * S.p.w + S.gx
        p = S.buf('in0').load(i)
        inv = S.select(p.w > 0.0, S.f(1.0) / S.select(p.w > 0.0, p.w, S.f(1.0)), S.f(0.0))
        S.buf('out').store(i, S.vec4(p.x * inv, p.y * inv, p.z * inv, p.w))


@kernel("bbo_read", WH + [('mode', 'u'), ('pad', 'u')] + [('m%d' % k, 'f') for k in range(9)], [PIC_IN, PIC_OUT])
def bbo_read(S):
    """bbfReadF: the straight picture sampled through the matrix (bilinear, the neighbour past the edge transparent black). mode 0: the background (colour x alpha: premultiplied),
    1: the foreground as it is, 2: the shadow's alpha plane (0, 0, 0, the foreground's alpha). The frame's rows are counted from the bottom (j = H - 1 - gy)."""
    P = S.p
    with S.in_range(P.w, P.h):
        m = [getattr(P, 'm%d' % k) for k in range(9)]
        xf = S.to_f(S.gx)
        yf = S.to_f(P.h - 1 - S.gy)
        x9 = yf * m[1] + m[2]
        x7 = yf * m[4] + m[5]
        x5 = yf * m[7] + m[8]
        inv = S.f(1.0) / (xf * m[6] + x5)
        xp = (xf * m[0] + x9) * inv
        yp = (xf * m[3] + x7) * inv
        ok = (xp > -0.4) & (xp < S.to_f(P.w)) & (yp > -0.4) & (yp < S.to_f(P.h))
        flx, fly = S.floor(xp), S.floor(yp)
        x0 = S.to_i(S.select(ok, flx, S.f(0.0)))
        y0 = S.to_i(S.select(ok, fly, S.f(0.0)))
        fx, fy = xp - flx, yp - fly
        Wi, Hi = S.to_i(P.w), S.to_i(P.h)
        z = S.f(0.0)
        taps = []
        for q in range(4):
            xx = x0 + (q & 1)
            yy = y0 + (q >> 1)
            inb = (xx >= 0) & (xx < Wi) & (yy >= 0) & (yy < Hi)
            xs = S.select(inb, xx, S.i(0))
            ys = S.select(inb, yy, S.i(0))
            v = S.buf('in0').load(S.to_u(Hi - 1 - ys) * P.w + S.to_u(xs))
            taps.append(S.select(inb, v, S.vec4(z, z, z, z)))
        o = []
        for k in range(4):
            t0 = taps[0][k] + (taps[1][k] - taps[0][k]) * fx
            t1 = taps[2][k] + (taps[3][k] - taps[2][k]) * fx
            o.append(S.select(ok, t0 + (t1 - t0) * fy, z))
        prem = S.eq(P.mode, 0)
        r = [S.select(prem, o[k] * o[3], o[k]) for k in range(3)]
        shadow = S.eq(P.mode, 2)
        res = S.vec4(S.select(shadow, z, r[0]), S.select(shadow, z, r[1]), S.select(shadow, z, r[2]), o[3])
        S.buf('out').store(S.gy * P.w + S.gx, res)


@kernel("bbo_la", WH, [PIC_IN, PIC_IN1, PIC_OUT])
def bbo_la(S):
    """in0 the foreground, in1 the blurred shadow plane: (colour x alpha, la), la = alpha + (1 - alpha) x the shadow's alpha cut to 0 .. 1"""
    with S.in_range(S.p.w, S.p.h):
        i = S.gy * S.p.w + S.gx
        f = S.buf('in0').load(i)
        sa = fmin(S, S.f(1.0), fmax(S, S.f(0.0), S.buf('in1').load(i).w))
        la = f.w + (S.f(1.0) - f.w) * sa
        S.buf('out').store(i, S.vec4(f.x * f.w, f.y * f.w, f.z * f.w, la))


@kernel("bbo_final", WH + [('q8', 'u'), ('pad', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def bbo_final(S):
    """in0 the layer (bbo_la), in1 the blurred background: colour = layer + (1 - la) x background, alpha la + (1 - la) x background alpha; the 12-bit store (q8)"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        d = S.buf('in0').load(i)
        b = S.buf('in1').load(i)
        one = S.f(1.0)
        q = S.ne(P.q8, 0)
        o = [d[k] + (one - d.w) * b[k] for k in range(3)] + [d.w + (one - d.w) * b.w]
        S.buf('out').store(i, S.vec4(*[maybe_store(S, q, v) for v in o]))
