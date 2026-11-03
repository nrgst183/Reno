"""Kernels of the effects that convolve by FFT in double precision on the CPU: Defocus (builtins_defocus.cpp, defocusRenderGpu) and Starburst (builtins_velvet.cpp, starburstVelvet, the
OpenCL path). tools/gpukernels.py loads this file; see its header for the conventions. The C++ side is src/ofx/vkfft.cpp.

The FFT is the CPU's radix-2 decimation in time in software float64 (tools/sf64.py): the same butterflies, the same twiddles (the CPU's own table, uploaded as the bits of its doubles) and the
same operations in the same order for each value, so the result is bit for bit the CPU's. A transform is one pass per stage (a thread per butterfly, working in place on the array).
The bit reversal the CPU does first is not a pass of its own: the forward transform takes its input in natural order and reads and writes the butterflies' two elements at the bit-reversed
slots (so after it the spectrum sits in bit-reversed order, the "scrambled" layout); the inverse transform takes a scrambled array as it is (the elements the CPU's permutation puts
side by side lie side by side there) and leaves natural order. The spectra are only multiplied together, element by element, so the scrambled order never matters.

Complex values are four words (re.hi, re.lo, im.hi, im.lo) in a buffer of words ('u'); twiddles are the same in the table.
"""
from gpukernels import kernel, PIC_IN, PIC_IN1, PIC_OUT, TABLE, WH, dpush, pd, SFVar
from sf64 import SF
from gpukernels_blur import store12
from gpukernels_filters import sf_byte

TABU = (3, 'table', 'u', 'r')
LIN = [('nt', 'u'), ('wd', 'u'), ('ht', 'u')]          # the flat thread count, and the dispatch's width and height


def lin(S):
    """(guard context, t): the flat thread number; the guard keeps the threads past the end out"""
    P = S.p
    return S.in_range(P.wd, P.ht)


def ldc(S, buf, wi):
    return SF(S, buf.load(wi), buf.load(wi + 1)), SF(S, buf.load(wi + 2), buf.load(wi + 3))


def stc(S, buf, wi, re, im):
    buf.store(wi, re.hi)
    buf.store(wi + 1, re.lo)
    buf.store(wi + 2, im.hi)
    buf.store(wi + 3, im.lo)


def bitrev(S, x, nbits):
    """the nbits low bits of x in reverse order"""
    r = x
    r = ((r >> 1) & 0x55555555) | ((r & 0x55555555) << 1)
    r = ((r >> 2) & 0x33333333) | ((r & 0x33333333) << 2)
    r = ((r >> 4) & 0x0f0f0f0f) | ((r & 0x0f0f0f0f) << 4)
    r = ((r >> 8) & 0x00ff00ff) | ((r & 0x00ff00ff) << 8)
    r = (r >> 16) | (r << 16)
    return r >> (S.u(32) - nbits)


# ---------------------------------------------------------------------------------------------------------- the transform
@kernel("ff_stage", LIN + [('n', 'u'), ('ln', 'u'), ('lg', 'u'), ('scr', 'u'), ('vert', 'u'), ('twoff', 'u'), ('twstep', 'u'), ('pad', 'u')],
        [(2, 'data', 'u', 'rw'), TABU])
def ff_stage(S):
    """one stage (len = 2 ^ lg) of fft1() on every row (or column) of every n x n array of the buffer, in place; a thread per butterfly. The twiddle of butterfly k is the complex
    number twoff + k * twstep of the table. scr: the elements are at the bit-reversed slots (the forward transform)."""
    P = S.p
    with lin(S):
        t = S.gy * P.wd + S.gx
        with S.if_(t < P.nt):
            d, tab = S.buf('data'), S.buf('table')
            ln = P.ln
            a = t >> (ln * 2 - 1)
            r = t & ((S.u(1) << (ln * 2 - 1)) - 1)
            line = r >> (ln - 1)
            j = r & ((P.n >> 1) - 1)
            lh = S.u(1) << (P.lg - 1)
            k = j & (lh - 1)
            i0 = ((j >> (P.lg - 1)) << P.lg) + k
            i1 = i0 + lh
            scr = S.ne(P.scr, 0)
            p0 = S.select(scr, bitrev(S, i0, ln), i0)
            p1 = S.select(scr, bitrev(S, i1, ln), i1)
            vert = S.ne(P.vert, 0)
            base = a << (ln * 2)
            e0 = base + S.select(vert, p0 * P.n + line, line * P.n + p0)
            e1 = base + S.select(vert, p1 * P.n + line, line * P.n + p1)
            u = ldc(S, d, e0 * 4)
            v = ldc(S, d, e1 * 4)
            w = ldc(S, tab, (P.twoff + k * P.twstep) * 4)
            vr, vi, wr, wi = v[0], v[1], w[0], w[1]
            tr = vr * wr - vi * wi
            ti = vr * wi + vi * wr
            stc(S, d, e1 * 4, u[0] - tr, u[1] - ti)
            stc(S, d, e0 * 4, u[0] + tr, u[1] + ti)


@kernel("ff_mul", LIN + [('m2', 'u'), ('pad', 'u')], [(1, 'kf', 'u', 'r'), (2, 'data', 'u', 'rw')])
def ff_mul(S):
    """data[i] = data[i] * kf[i] (the product pl[i] *= kf[i] of the CPU: re = ar kr - ai ki, im = ar ki + ai kr); kf one array, data one or more"""
    P = S.p
    with lin(S):
        t = S.gy * P.wd + S.gx
        with S.if_(t < P.nt):
            d, kf = S.buf('data'), S.buf('kf')
            a = ldc(S, d, t * 4)
            k = ldc(S, kf, (t & (P.m2 - 1)) * 4)
            stc(S, d, t * 4, a[0] * k[0] - a[1] * k[1], a[0] * k[1] + a[1] * k[0])


# ---------------------------------------------------------------------------------------------------------- Defocus (defocusRenderGpu)
@kernel("df_prep", WH + [('rf', 'u'), ('bloom', 'u'), ('t', 'f'), ('kb', 'f')], [PIC_IN, PIC_OUT])
def df_prep(S):
    """the picture as the convolution takes it: Reduce flicker's 3 x 3 float blur (the CPU's rows run bottom first: tap row a is the pixel a - 1 rows up the CPU's picture, here down), then the
    bloom v += (v - t) * kb on R, G, B above t"""
    P = S.p
    with S.in_range(P.w, P.h):
        src = S.buf('in0')
        w, h = S.to_i(P.w), S.to_i(P.h)
        x, y = S.to_i(S.gx), S.to_i(S.gy)
        kk = [1.0, 2.0, 1.0]
        def at(xx, yy):
            return src.load(S.to_u(yy) * P.w + S.to_u(xx))
        if True:
            acc = S.var('v4', S.vec4(S.f(0.0), S.f(0.0), S.f(0.0), S.f(0.0)))
            for a in range(3):
                for b in range(3):
                    yy = S.min(S.max(y - (a - 1), S.i(0)), h - 1)
                    xx = S.min(S.max(x + (b - 1), S.i(0)), w - 1)
                    acc.set(acc.get() + at(xx, yy) * (kk[a] * kk[b]))
            blurred = acc.get() / 16.0
        p = S.select(S.ne(P.rf, 0), blurred, src.load(S.gy * P.w + S.gx))
        bl = S.ne(P.bloom, 0)
        def bloom(v):
            return S.select(bl & (v > P.t), v + (v - P.t) * P.kb, v)
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(bloom(p.x), bloom(p.y), bloom(p.z), p.w))


@kernel("df_kinit", LIN + [('t', 'u'), ('half', 'u'), ('pad', 'u')], [(2, 'data', 'u', 'w'), TABLE])
def df_kinit(S):
    """the iris mask (the table: (2 half)^2 byte values as floats, row r column c the offset (c - half + 1, r - half + 1)) placed at the circular positions (u, v) mod T of a T x T array, as
    complex numbers with a zero imaginary part"""
    P = S.p
    with lin(S):
        t = S.gy * P.wd + S.gx
        with S.if_(t < P.nt):
            row, col = t / P.t, t % P.t
            def tap(i):
                # the offset of position i: i for 0 .. half, i - T for T - half + 1 .. T - 1; the tap index (offset + half - 1), valid in 0 .. 2 half - 1
                ok = (i <= P.half) | (i + P.half > P.t)
                off = S.to_i(i) - S.select(i <= P.half, S.i(0), S.to_i(P.t))
                return ok, S.to_u(off + S.to_i(P.half) - 1)
            okr, r = tap(row)
            okc, c = tap(col)
            ok = okr & okc
            m = S.buf('table').load(S.select(ok, r * (P.half * 2) + c, S.u(0)))
            v = SF.sel(S, ok, SF.from_f32(S, m), SF.const(S, 0.0))
            d = S.buf('data')
            stc(S, d, t * 4, v, SF.const(S, 0.0))


@kernel("df_load", LIN + [('t', 'u'), ('nx', 'u'), ('step', 'u'), ('ovn', 'i'), ('tile0', 'u'), ('N', 'i'), ('W', 'u'), ('H', 'u')],
        [PIC_IN, (2, 'data', 'u', 'w')])
def df_load(S):
    """the tiles of a batch, two arrays each (R, G and B, A of the picture), natural order: tile (x0, y0) pixel (x, y) is the picture's pixel (X, Y) mod N (the grid wraps) with the rows beyond
    the picture and the columns beyond it carried on from its last row and column"""
    P = S.p
    with lin(S):
        t = S.gy * P.wd + S.gx
        with S.if_(t < P.nt):
            T2 = P.t * P.t
            tb = t / T2
            r = t % T2
            y, x = r / P.t, r % P.t
            ti = tb + P.tile0
            x0 = P.ovn + S.to_i(ti % P.nx) * S.to_i(P.step)
            y0 = P.ovn + S.to_i(ti / P.nx) * S.to_i(P.step)
            Nn = P.N
            def wrap(v):
                return ((v % Nn) + Nn) % Nn
            Y = wrap(y0 + S.to_i(y))
            X = wrap(x0 + S.to_i(x))
            sy = S.min(Y, S.to_i(P.H) - 1)
            sx = S.min(X, S.to_i(P.W) - 1)
            px = S.buf('in0').load((P.H - 1 - S.to_u(sy)) * P.W + S.to_u(sx))
            d = S.buf('data')
            z = SF.const(S, 0.0)
            e0 = (tb * 2 * T2 + r) * 4
            e1 = e0 + T2 * 4
            stc(S, d, e0, SF.from_f32(S, px.x), SF.from_f32(S, px.y))
            stc(S, d, e1, SF.from_f32(S, px.z), SF.from_f32(S, px.w))


@kernel("df_store", LIN + [('t', 'u'), ('nx', 'u'), ('step', 'u'), ('ovn', 'i'), ('tile0', 'u'), ('ov', 'i'), ('W', 'u'), ('H', 'u')] + dpush('scale'),
        [(0, 'data', 'u', 'r'), (2, 'out', 'v4', 'rw')])
def df_store(S):
    """the middle of every tile of the batch (T - ov pixels square) as (float)(v * scale) into the result picture (all four channels)"""
    P = S.p
    with lin(S):
        t = S.gy * P.wd + S.gx
        with S.if_(t < P.nt):
            T2 = P.t * P.t
            tb = t / T2
            r = t % T2
            y, x = r / P.t, r % P.t
            ti = tb + P.tile0
            x0 = P.ovn + S.to_i(ti % P.nx) * S.to_i(P.step)
            y0 = P.ovn + S.to_i(ti / P.nx) * S.to_i(P.step)
            X, Y = x0 + S.to_i(x), y0 + S.to_i(y)
            half = P.ov / 2
            ti_ = S.to_i(P.t)
            xa, xb = S.max(S.i(0), x0 + half), S.min(S.to_i(P.W), x0 + ti_ - half)
            ya, yb = S.max(S.i(0), y0 + half), S.min(S.to_i(P.H), y0 + ti_ - half)
            with S.if_((X >= xa) & (X < xb) & (Y >= ya) & (Y < yb)):
                d = S.buf('data')
                e0 = (tb * 2 * T2 + r) * 4
                p0 = ldc(S, d, e0)
                p1 = ldc(S, d, e0 + T2 * 4)
                sc = pd(P, 'scale')
                o = S.vec4((p0[0] * sc).to_f32(), (p0[1] * sc).to_f32(), (p1[0] * sc).to_f32(), (p1[1] * sc).to_f32())
                S.buf('out').store((P.H - 1 - S.to_u(Y)) * P.W + S.to_u(X), o)


@kernel("df_finish", WH + [('q8', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def df_finish(S):
    """finish() of defocusRenderGpu: not a number 0, else min(1, max(0, v)) (std::max / std::min: the first operand when they compare equal), the 8-bit store with q8"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        def fin(v):
            m = S.select(S.f(0.0) < v, v, S.f(0.0))
            c = S.select(m < 1.0, m, S.f(1.0))
            c = S.select(S.isnan(v), S.f(0.0), c)
            return S.select(S.ne(P.q8, 0), store12(S, c), c)
        S.buf('out').store(i, S.vec4(fin(p.x), fin(p.y), fin(p.z), fin(p.w)))


# ---------------------------------------------------------------------------------------------------------- Starburst (starburstVelvet, the OpenCL path)
@kernel("sb_prep", WH + [('soft', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT, TABLE])
def sb_prep(S):
    """the picture's bytes as floats (byteOf), times the Soften edges mask (the table, a float per pixel) when soft: (float)floor(v * m + 0.5)"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        m = S.buf('table').load(S.select(S.ne(P.soft, 0), i, S.u(0)))
        def one(v):
            b = sf_byte(S, v)
            s = ((SF.from_f32(S, b) * SF.from_f32(S, m)) + 0.5).floor().to_f32()
            return S.select(S.ne(P.soft, 0), s, b)
        S.buf('out').store(i, S.vec4(one(p.x), one(p.y), one(p.z), one(p.w)))


@kernel("sb_rf", WH, [PIC_IN, PIC_OUT])
def sb_rf(S):
    """Reduce flicker: blur3x3() of the byte planes (1 2 1 each way over whole numbers, edges carried on, (sum + 8) / 16)"""
    P = S.p
    with S.in_range(P.w, P.h):
        src = S.buf('in0')
        w, h = S.to_i(P.w), S.to_i(P.h)
        x, y = S.to_i(S.gx), S.to_i(S.gy)
        wt = [1, 2, 1]
        acc = [S.i(0), S.i(0), S.i(0), S.i(0)]
        for a in range(3):
            yy = S.min(S.max(y + (a - 1), S.i(0)), h - 1)
            for b in range(3):
                xx = S.min(S.max(x + (b - 1), S.i(0)), w - 1)
                q = src.load(S.to_u(yy) * P.w + S.to_u(xx))
                for k, c in enumerate((q.x, q.y, q.z, q.w)):
                    acc[k] = acc[k] + S.to_i(c) * (wt[a] * wt[b])
        o = [S.to_f((acc[k] + 8) / 16) for k in range(4)]
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(o[0], o[1], o[2], o[3]))


@kernel("sb_load", LIN + [('N', 'u'), ('lN', 'u'), ('W', 'u'), ('H', 'u'), ('thr', 'u'), ('boost', 'u'), ('ti', 'f'), ('T', 'f'), ('sb', 'f'), ('pad', 'u')],
        [PIC_IN, (2, 'data', 'u', 'w')])
def sb_load(S):
    """the picture in the two N x N planes of the buffer (A: R and G, then B: B and alpha; natural order): row r of the plane is the picture's row H - 1 - min(H - 1, r) (the picture's rows run top
    first here), column x its column min(W - 1, x); the colours below ti go to 0 (thr), then those above T are boosted v + (v - T) * S (boost); the alpha plane is left alone"""
    P = S.p
    with lin(S):
        t = S.gy * P.wd + S.gx
        with S.if_(t < P.nt):
            r = t >> P.lN
            x = t & (P.N - 1)
            y = (P.H - 1) - S.min(P.H - 1, r)
            q = S.buf('in0').load(y * P.W + S.min(P.W - 1, x))
            def col(v):
                v = S.select(S.ne(P.thr, 0) & (v < P.ti), S.f(0.0), v)
                return S.select(S.ne(P.boost, 0) & (v > P.T), v + (v - P.T) * P.sb, v)
            e = t * 4
            d = S.buf('data')
            stc(S, d, e, SF.from_f32(S, col(q.x)), SF.from_f32(S, col(q.y)))
            stc(S, d, e + (P.N << P.lN) * 4, SF.from_f32(S, col(q.z)), SF.from_f32(S, q.w))


@kernel("sb_kinit", LIN + [('N', 'u'), ('lN', 'u'), ('sn', 'u'), ('scale', 'f')], [(2, 'data', 'u', 'w'), TABU])
def sb_kinit(S):
    """the star in the N x N plane K (natural order): tap (tx, ty) of the star's byte picture (sn x sn, four bytes to a table word) adds (double)(float)(byte * scale) at column (S - tx) mod N,
    row (ty - S + 1) mod N, S = sn / 2, in the CPU's order (rows, then columns of the star); a plane smaller than the star adds up to four taps"""
    P = S.p
    with lin(S):
        t = S.gy * P.wd + S.gx
        with S.if_(t < P.nt):
            a = t & (P.N - 1)
            r = t >> P.lN
            Sh = P.sn / 2
            Ni = S.to_i(P.N)
            tx0 = ((S.to_i(Sh) - S.to_i(a)) % Ni + Ni) % Ni
            ty0 = ((S.to_i(r) + S.to_i(Sh) - 1) % Ni + Ni) % Ni
            acc = SFVar(S, 0.0)
            tab = S.buf('table')
            for my in range(2):
                for mx in range(2):
                    ty = ty0 + Ni * my
                    tx = tx0 + Ni * mx
                    ok = (ty < S.to_i(P.sn)) & (tx < S.to_i(P.sn))
                    idx = S.to_u(S.select(ok, ty * S.to_i(P.sn) + tx, S.i(0)))
                    word = tab.load(idx >> 2)
                    b = (word >> ((idx & 3) * 8)) & 255
                    v = SF.from_f32(S, S.to_f(b) * P.scale)
                    with S.if_(ok & S.ne(b, 0)):
                        acc.set(acc.get() + v)
            stc(S, S.buf('data'), t * 4, acc.get(), SF.const(S, 0.0))


@kernel("sb_final", WH + [('N', 'u'), ('effonly', 'u'), ('q8', 'u'), ('pad', 'u')] + dpush('norm', 'op'),
        [PIC_IN, (1, 'data', 'u', 'r'), (2, 'out', 'v4', 'w')])
def sb_final(S):
    """the added glow: cv = the planes' value * norm, sv = (float)(clamp(cv / 255, 0, 1) * op), the pixel + sv (or sv alone), the 8-bit store with q8"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        pix = S.buf('in0').load(i)
        e = ((P.h - 1 - S.gy) * P.N + S.gx) * 4
        a = ldc(S, S.buf('data'), e)
        b = ldc(S, S.buf('data'), e + (P.N * P.N) * 4)
        norm, op = pd(P, 'norm'), pd(P, 'op')
        c255, z, one = SF.const(S, 255.0), SF.const(S, 0.0), SF.const(S, 1.0)
        def sv(c):
            v = (c * norm) / c255
            v = SF.sel(S, v < z, z, SF.sel(S, v > one, one, v))
            return (v * op).to_f32()
        def comb(cv, o):
            s = sv(cv)
            r = S.select(S.ne(P.effonly, 0), s, o + s)
            return S.select(S.ne(P.q8, 0), store12(S, r), r)
        S.buf('out').store(i, S.vec4(comb(a[0], pix.x), comb(a[1], pix.y), comb(b[0], pix.z), comb(b[1], pix.w)))
