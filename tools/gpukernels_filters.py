"""Kernels of the built-in FILTERS that are bit-identical to their CPU code (tools/gpukernels.py loads this file; see its header for the conventions)."""
from gpukernels import kernel, PIC_IN, PIC_IN1, PIC_OUT, TABLE, WH, clamp01, unpremul, to_level, from_level, sdiv_pos, dpush, pd, ld_sf, fpow, fsqrt, fmin, fmax, sample_clamped, luma
from spvdsl import compile_kernel
from sf64 import SF, sf_pow, sf_atan2

@kernel("sepia", WH + [('grey_mix', 'u'), ('use_lut', 'u'), ('to_grey', 'i'), ('c0', 'i'), ('c1', 'i'), ('c2', 'i')], [PIC_IN, PIC_OUT, TABLE])
def sepia(S):
    # sepiaRows() in builtins_filters.cpp: whole-level integer steps on the premultiplied levels; the table holds the 256 lut levels
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        def lv(v):
            t = v * 255.0 + 0.5
            return S.select(t < 0.0, S.i(0), S.select(t > 255.0, S.i(255), S.to_i(t)))
        v = [lv(p.x), lv(p.y), lv(p.z)]
        grey = (v[0] + v[1] + v[2]) / 3
        lutv = S.to_i(S.buf('table').load(S.to_u(grey)))
        out = []
        for k in range(3):
            r = S.select(S.ne(P.grey_mix, 0), v[k] + ((grey - v[k]) * P.to_grey) / 256, grey)
            col = (P.c0, P.c1, P.c2)[k]
            r = S.select(S.ne(P.use_lut, 0), r + ((col - r) * lutv) / 256, r)
            out.append(S.to_f(r & 255) / 255.0)
        S.buf('out').store(i, S.vec4(out[0], out[1], out[2], p.w))

def wb_round(S, v):
    """wbRound() in builtins_more_colour.cpp: (int)floor((double)v + 1.5e-8 + 0.5); an int"""
    return ((SF.from_f32(S, v) + 1.5e-8) + 0.5).floor_i32()

def wb_clamp255(S, v):
    return S.select(v < 0.0, S.f(0.0), S.select(v > 255.0, S.f(255.0), v))

@kernel("wb_bytes", WH + [('shift', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT, TABLE])
def wb_bytes(S):
    # whiteBalance() step 1: the 8-bit R, G, B of each pixel after the Y / U / V shift (the table holds the 1024 x (V shift, U shift, Y)); out = (R, G, B, alpha)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        a = p.w
        has = a > 1e-6
        z = S.f(0.0)
        r = S.select(has, p.x / a, z)
        g = S.select(has, p.y / a, z)
        b = S.select(has, p.z / a, z)
        R = S.to_f(wb_round(S, clamp01(S, r) * 255.0))
        G = S.to_f(wb_round(S, clamp01(S, g) * 255.0))
        B = S.to_f(wb_round(S, clamp01(S, b) * 255.0))
        Y = (G * 0.587 + R * 0.299) + B * 0.114
        base = S.to_u(wb_round(S, Y * 4.0)) * 3
        tab = S.buf('table')
        t0, t1, t2 = tab.load(base), tab.load(base + 1), tab.load(base + 2)
        U = (B - Y) * 0.492 + t1
        V = (R - Y) * 0.877 + t0
        R2 = V * 1.1402509 + t2
        B2 = U * 2.0325203 + t2
        G2 = (t2 - U * 0.39473134) - V * 0.5808092
        sh = S.ne(P.shift, 0)
        oR = S.select(sh, S.to_f(wb_round(S, wb_clamp255(S, R2))), R)
        oG = S.select(sh, S.to_f(wb_round(S, wb_clamp255(S, G2))), G)
        oB = S.select(sh, S.to_f(wb_round(S, wb_clamp255(S, B2))), B)
        live = a > 0.0
        S.buf('out').store(i, S.vec4(S.select(live, oR, z), S.select(live, oG, z), S.select(live, oB, z), a))

@kernel("wb_clear", [('n', 'u'), ('one', 'u')], [(2, 'cnt', 'u', 'rw')])
def wb_clear(S):
    with S.in_range(S.p.n, S.p.one):
        S.buf('cnt').store(S.gx, S.u(0))

@kernel("wb_hist", WH, [(0, 'bytes', 'v4', 'r'), (2, 'hist', 'u', 'rw')])
def wb_hist(S):
    # the histogram of each channel's byte over the pixels with alpha above 0 (whole numbers: the order of the additions does not matter)
    P = S.p
    with S.in_range(P.w, P.h):
        p = S.buf('bytes').load(S.gy * P.w + S.gx)
        with S.if_(p.w > 0.0):
            h = S.buf('hist')
            h.atomic_add(S.to_u(p.x), S.u(1))
            h.atomic_add(S.u(256) + S.to_u(p.y), S.u(1))
            h.atomic_add(S.u(512) + S.to_u(p.z), S.u(1))

@kernel("wb_stats", WH, [(0, 'hist', 'u', 'r'), (2, 'stats', 'f', 'w')])
def wb_stats(S):
    # whiteBalance() step 2: per channel (thread x) the stretch (offset, gain) from the cumulative histogram, in the CPU's float steps
    P = S.p
    with S.in_range(P.w, P.h):
        hist = S.buf('hist')
        base = S.gx * 256
        tot = S.var('f', S.f(0.0))
        with S.for_(0, 256) as i:
            tot.set(tot.get() + S.to_f(S.min(hist.load(base + i), S.u(16777216))))
        total = tot.get()
        scd = SF.const(S, 1.0) / SF.from_f32(S, total)
        sc = scd.to_f32()
        tail = S.f(0.009999999776482582) * 0.5
        up = S.f(1.0) - tail
        run = S.var('f', S.f(0.0))
        still = S.var('u', S.u(1))
        lo = S.var('u', S.u(0))
        nle = S.var('u', S.u(0))
        with S.for_(0, 256) as i:
            run.set(run.get() + S.to_f(S.min(hist.load(base + i), S.u(16777216))))
            cs = run.get() * sc
            below = tail > cs
            with S.if_(S.ne(still.get(), 0) & below & (i < 255)):
                lo.set(lo.get() + 1)
            still.set(S.select(below, still.get(), S.u(0)))
            with S.if_(cs <= up):
                nle.set(nle.get() + 1)
        hi = S.select(nle.get() > 0, nle.get() - 1, S.u(0))
        lov = lo.get()
        off = S.to_f(S.i(0) - S.to_i(lov)) * 0.00390625
        diff = S.to_f(S.to_i(hi) - S.to_i(lov))
        gain = S.select(S.eq(hi, lov), S.f(1.0), S.f(256.0) / S.select(S.eq(hi, lov), S.f(1.0), diff))
        ok = total > 0.0
        S.buf('stats').store(S.gx * 2, S.select(ok, off, S.f(0.0)))
        S.buf('stats').store(S.gx * 2 + 1, S.select(ok, gain, S.f(1.0)))

@kernel("wb_final", WH + [('has_data', 'u'), ('pad', 'u')], [(0, 'bytes', 'v4', 'r'), (1, 'stats', 'f', 'r'), PIC_OUT])
def wb_final(S):
    # whiteBalance() step 3: the bytes (stretched by the histogram when HasData) as premultiplied floats
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        q = S.buf('bytes').load(i)
        a = q.w
        st = S.buf('stats')
        hd = S.ne(P.has_data, 0)
        def one(f, k):
            off, gain = st.load(S.u(2 * k)), st.load(S.u(2 * k + 1))
            s2 = wb_clamp255(S, (f + off * 255.0) * gain)
            v = S.to_f(S.to_i(s2 + 0.5))
            return S.select(hd, v, f)
        v = [one(q.x, 0), one(q.y, 1), one(q.z, 2)]
        z = S.f(0.0)
        live = a > 0.0
        S.buf('out').store(i, S.vec4(S.select(live, (v[0] / 255.0) * a, z), S.select(live, (v[1] / 255.0) * a, z), S.select(live, (v[2] / 255.0) * a, z), S.select(live, a, z)))


# ---------------------------------------------------------------------------------------------------------- helpers for doubles
class SFVar:
    """a double kept in a variable (for loops): get() / set()"""
    def __init__(self, S, init):
        self.S = S
        self.h = S.var('u', init.hi)
        self.l = S.var('u', init.lo)
    def get(self): return SF(self.S, self.h.get(), self.l.get())
    def set(self, v):
        self.h.set(v.hi)
        self.l.set(v.lo)

def sf_zero(S): return SF.const(S, 0.0)

# ---------------------------------------------------------------------------------------------------------- Chroma Blur
@kernel("chroma_conv", WH + [('vert', 'u'), ('n', 'u')], [PIC_IN, PIC_OUT, TABLE])
def chroma_conv(S):
    # convolvePlane() in builtins_more_blur.cpp for the two difference planes (y, z of the pixel; x is the luma): a double sum of float taps in order, edge pixels repeated
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        tab = S.buf('table')
        inb = S.buf('in0')
        a1 = SFVar(S, sf_zero(S))
        a2 = SFVar(S, sf_zero(S))
        n = S.to_i(P.n)
        vert = S.ne(P.vert, 0)
        with S.for_(-n, n + 1, 1, True) as j:
            k = SF.from_f32(S, tab.load(S.to_u(j + n)))
            xx = S.select(vert, S.gx, S.to_u(S.clamp(S.to_i(S.gx) + j, 0, S.to_i(P.w) - 1)))
            yy = S.select(vert, S.to_u(S.clamp(S.to_i(S.gy) + j, 0, S.to_i(P.h) - 1)), S.gy)
            q = inb.load(yy * P.w + xx)
            a1.set(a1.get() + k * SF.from_f32(S, q.y))
            a2.set(a2.get() + k * SF.from_f32(S, q.z))
        q0 = inb.load(i)
        S.buf('out').store(i, S.vec4(q0.x, a1.get().to_f32(), a2.get().to_f32(), q0.w))

@kernel("chroma8_split", WH, [PIC_IN, PIC_OUT])
def chroma8_split(S):
    # chromaBlur8(): (Y, U, V, alpha level) of the premultiplied levels (b0 = blue, b1 = green, b2 = red)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        b0, b1, b2 = S.to_f(to_level(S, p.z)), S.to_f(to_level(S, p.y)), S.to_f(to_level(S, p.x))
        Y = (b0 * 0.299 + b1 * 0.587) + b2 * 0.114
        U = b2 * 0.436 - (b1 * 0.28886 + b0 * 0.14713)
        V = (b0 * 0.615 - b1 * 0.51499) - b2 * 0.10001
        S.buf('out').store(i, S.vec4(Y, U, V, S.to_f(to_level(S, p.w))))

def box_scan(S, inb, outb, index, count, r, bottom_first):
    """boxPassH / boxPassV for U and V (components y, z) along one line of 'count' pixels: index(k) is the pixel number of the k-th pixel
    of the line (in the order the CPU walks it); radius r (uint), 2r + 1 <= count."""
    inv = S.f(1.0) / S.to_f(2 * r + 1)
    first = inb.load(index(S.u(0)))
    last = inb.load(index(count - 1))
    val_u = S.var('f', S.to_f(r + 1) * first.y)
    val_v = S.var('f', S.to_f(r + 1) * first.z)
    with S.for_(0, r) as j:
        q = inb.load(index(j))
        val_u.set(val_u.get() + q.y)
        val_v.set(val_v.get() + q.z)
    with S.for_(0, count) as ti:
        ph1 = ti <= r
        ph2 = (~ph1) & (ti < count - r)
        ri = S.min(r + ti, count - 1)
        li = S.select(ti >= r + 1, ti - r - 1, S.u(0))
        qa = inb.load(index(ri))
        ql = inb.load(index(li))
        du = S.select(ph1, qa.y - first.y, S.select(ph2, qa.y - ql.y, last.y - ql.y))
        dv = S.select(ph1, qa.z - first.z, S.select(ph2, qa.z - ql.z, last.z - ql.z))
        val_u.set(val_u.get() + du)
        val_v.set(val_v.get() + dv)
        q0 = inb.load(index(ti))
        outb.store(index(ti), S.vec4(q0.x, val_u.get() * inv, val_v.get() * inv, q0.w))

@kernel("chroma8_box_h", WH + [('r', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def chroma8_box_h(S):
    # boxPassH(): one row per invocation (the running sums go along the row)
    P = S.p
    with S.in_range(P.h, 1):
        row = S.gx * P.w
        box_scan(S, S.buf('in0'), S.buf('out'), lambda k: row + k, P.w, P.r, False)

@kernel("chroma8_box_v", WH + [('r', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def chroma8_box_v(S):
    # boxPassV(): one column per invocation, walking up from the bottom row
    P = S.p
    with S.in_range(P.w, 1):
        box_scan(S, S.buf('in0'), S.buf('out'), lambda k: (P.h - 1 - k) * P.w + S.gx, P.h, P.r, True)

@kernel("chroma8_join", WH, [PIC_IN, PIC_OUT])
def chroma8_join(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        q = S.buf('in0').load(i)
        y, u, v = q.x, q.y, q.z
        v0 = (u * 0.0 + y) + v * 1.13983
        v1 = (y - u * 0.39465) - v * 0.5806
        v2 = (u * 2.03211 + y) + v * 0.0
        def cut(t):
            m = S.select(t < 255.0, t, S.f(255.0))
            return S.to_f(S.to_i(S.select(t < 0.0, S.f(0.0), m))) * S.f(1.0 / 255.0)
        S.buf('out').store(i, S.vec4(cut(v2), cut(v1), cut(v0), q.w * S.f(1.0 / 255.0)))

# ---------------------------------------------------------------------------------------------------------- Buf::sample in doubles
def buf_sample(S, buf, w, h, x, y):
    """Buf::sample(x, y): bilinear at a position in pixels (centres at +0.5; x, y doubles); four taps inside, else sampleEdge() (taps outside are 0,
    nothing beyond a pixel out). w, h: uint values; returns a vec4 (float arithmetic in the CPU's order)."""
    wf, hf = SF.from_u32(S, w), SF.from_u32(S, h)
    xs, ys = x - 0.5, y - 0.5
    inside = (xs >= 0.0) & (ys >= 0.0) & (xs < wf - 1.0) & (ys < hf - 1.0)
    res = S.var('v4', S.splat4(0.0))
    wi, hi = S.to_i(w), S.to_i(h)
    with S.if_(inside):
        x0 = S.min(S.to_u(S.max(xs.trunc_i32(), 0)), w - 1)
        y0 = S.min(S.to_u(S.max(ys.trunc_i32(), 0)), h - 1)
        fx = (xs - SF.from_i32(S, S.to_i(x0))).to_f32()
        fy = (ys - SF.from_i32(S, S.to_i(y0))).to_f32()
        ix, iy = 1.0 - fx, 1.0 - fy
        w00, w10, w01, w11 = ix * iy, fx * iy, ix * fy, fx * fy
        x1, y1 = S.min(x0 + 1, w - 1), S.min(y0 + 1, h - 1)
        p00, p10 = buf.load(y0 * w + x0), buf.load(y0 * w + x1)
        p01, p11 = buf.load(y1 * w + x0), buf.load(y1 * w + x1)
        acc = S.splat4(0.0) + p00 * w00
        acc = acc + p10 * w10
        acc = acc + p01 * w01
        acc = acc + p11 * w11
        res.set(acc)
    with S.else_():
        near = (xs >= -1.0) & (ys >= -1.0) & (xs <= wf) & (ys <= hf)
        with S.if_(near):
            x0 = xs.floor_i32()
            y0 = ys.floor_i32()
            fx = (xs - SF.from_i32(S, x0)).to_f32()
            fy = (ys - SF.from_i32(S, y0)).to_f32()
            acc = S.splat4(0.0)
            for j in (0, 1):
                for i in (0, 1):
                    xx, yy = x0 + i, y0 + j
                    ok = (xx >= 0) & (yy >= 0) & (xx < wi) & (yy < hi)
                    wgt = (fx if i else 1.0 - fx) * (fy if j else 1.0 - fy)
                    idx = S.to_u(S.clamp(yy, 0, hi - 1)) * w + S.to_u(S.clamp(xx, 0, wi - 1))
                    acc = acc + S.select(ok, buf.load(idx) * wgt, S.splat4(0.0))
            res.set(acc)
    return res.get()

# ---------------------------------------------------------------------------------------------------------- Deform
@kernel("deform", WH + dpush('st', 'sb', 'sl', 'sr', 'sh', 'sv', 'ct', 'cb', 'cl', 'cr') + [('br', 'f'), ('bg', 'f'), ('bb', 'f'), ('ba', 'f')], [PIC_IN, PIC_OUT, TABLE])
def deform(S):
    # deform() in builtins_more_creative.cpp, in doubles; the table holds sin(pi v) of every row, then sin(pi u) of every column, as the bits of doubles
    P = S.p
    with S.in_range(P.w, P.h):
        wd, hd = SF.from_u32(S, P.w), SF.from_u32(S, P.h)
        u = (SF.from_u32(S, S.gx) + 0.5) / wd
        v = (SF.from_u32(S, S.gy) + 0.5) / hd
        tab = S.buf('table')
        sinv = ld_sf(S, tab, S.gy * 2)
        sinu = ld_sf(S, tab, (P.h + S.gx) * 2)
        st, sb, sl, sr, sh, sv = [pd(P, n) for n in ('st', 'sb', 'sl', 'sr', 'sh', 'sv')]
        ct, cb, cl, cr = [pd(P, n) for n in ('ct', 'cb', 'cl', 'cr')]
        sq = (1.0 - v) * st + v * sb
        sqv = (1.0 - u) * sl + u * sr
        t1 = 1.0 - sq
        t2 = 1.0 - sqv
        m1 = SF.sel(S, SF.const(S, 0.05) < t1, t1, SF.const(S, 0.05))
        m2 = SF.sel(S, SF.const(S, 0.05) < t2, t2, SF.const(S, 0.05))
        su = (u - 0.5) / m1 + 0.5
        sv2 = (v - 0.5) / m2 + 0.5
        su = su + (sh * (v - 0.5)) * 1.0
        sv2 = sv2 + sv * (u - 0.5)
        su = su + (((ct * (1.0 - v) + cb * v) * 0.5) * sinv) * 0.5
        sv2 = sv2 + (((cl * (1.0 - u) + cr * u) * 0.5) * sinu) * 0.5
        outside = (su < 0.0) | (su > 1.0) | (sv2 < 0.0) | (sv2 > 1.0)
        i = S.gy * P.w + S.gx
        res = S.var('v4', S.vec4(P.br, P.bg, P.bb, P.ba))
        with S.if_(~outside):
            res.set(buf_sample(S, S.buf('in0'), P.w, P.h, su * wd, sv2 * hd))
        S.buf('out').store(i, res.get())

# ---------------------------------------------------------------------------------------------------------- Displacement Map, Height Map
@kernel("displacementmap", WH + dpush('sh', 'sv') + [('wrap', 'u'), ('cx', 'u'), ('cy', 'u'), ('ix', 'u'), ('iy', 'u'), ('alpha', 'u'), ('flip', 'u')], [PIC_IN, PIC_OUT])
def displacementmap(S):
    # displacementMap() in builtins_vegasrest.cpp: positions in doubles
    from gpukernels import channel_of
    P = S.p
    with S.in_range(P.w, P.h):
        ry = S.select(S.ne(P.flip, 0), P.h - 1 - S.gy, S.gy)
        m = S.buf('in0').load(ry * P.w + S.gx)
        vx = channel_of(S, m, P.cx)
        vy = channel_of(S, m, P.cy)
        vx = S.select(S.ne(P.ix, 0), 1.0 - vx, vx)
        vy = S.select(S.ne(P.iy, 0), 1.0 - vy, vy)
        k = SF.from_f32(S, S.select(S.ne(P.alpha, 0), m.w, S.f(1.0)))
        sh, sv = pd(P, 'sh'), pd(P, 'sv')
        sx = (SF.from_u32(S, S.gx) + 0.5) + (((SF.from_f32(S, vx) - 0.5) * 2.0) * sh) * k
        sy = (SF.from_u32(S, S.gy) + 0.5) + (((SF.from_f32(S, vy) - 0.5) * 2.0) * sv) * k
        wd, hd = SF.from_u32(S, P.w), SF.from_u32(S, P.h)
        wrap = S.ne(P.wrap, 0)
        wx = sx - (sx / wd).floor() * wd
        wy = sy - (sy / hd).floor() * hd
        sx = SF.sel(S, wrap, wx, sx)
        sy = SF.sel(S, wrap, wy, sy)
        S.buf('out').store(S.gy * P.w + S.gx, buf_sample(S, S.buf('in0'), P.w, P.h, sx, sy))

PLANE_IN1 = (1, 'in1', 'f', 'r')

@kernel("heightmap", WH + dpush('lift', 'base') + [('wrap', 'u'), ('pad', 'u')], [PIC_IN, PLANE_IN1, PIC_OUT])
def heightmap(S):
    # heightMap() in builtins_vegasrest.cpp, the second half: the source row moves by base + lift * height (doubles)
    P = S.p
    with S.in_range(P.w, P.h):
        hp = SF.from_f32(S, S.buf('in1').load(S.gy * P.w + S.gx))
        sy = (SF.from_u32(S, S.gy) + 0.5) + (pd(P, 'base') + pd(P, 'lift') * hp)
        hd = SF.from_u32(S, P.h)
        sy = SF.sel(S, S.ne(P.wrap, 0), sy - (sy / hd).floor() * hd, sy)
        S.buf('out').store(S.gy * P.w + S.gx, buf_sample(S, S.buf('in0'), P.w, P.h, SF.from_u32(S, S.gx) + 0.5, sy))

# ---------------------------------------------------------------------------------------------------------- Mesh Warp (pictures drawn through the points)
def sf_clamp(S, v, lo, hi):
    """clampd(): v < lo ? lo : (v > hi ? hi : v)"""
    lo, hi = SF.of(S, lo), SF.of(S, hi)
    return SF.sel(S, v < lo, lo, SF.sel(S, v > hi, hi, v))

@kernel("meshwarp", WH + [('cols', 'u'), ('rows', 'u')], [PIC_IN, PIC_OUT, TABLE])
def meshwarp(S):
    # meshWarp() in builtins_magix.cpp, WarpDirection 0, in doubles; the table holds the points (floats), x y per point
    P = S.p
    with S.in_range(P.w, P.h):
        tab = S.buf('table')
        wd, hd = SF.from_u32(S, P.w), SF.from_u32(S, P.h)
        u = sf_clamp(S, (SF.from_u32(S, S.gx) + 0.5) / wd, 0.0, 1.0) * SF.from_u32(S, P.cols - 1)
        v = sf_clamp(S, (SF.from_u32(S, S.gy) + 0.5) / hd, 0.0, 1.0) * SF.from_u32(S, P.rows - 1)
        i = S.to_u(S.min(S.to_i(P.cols) - 2, u.trunc_i32()))
        j = S.to_u(S.min(S.to_i(P.rows) - 2, v.trunc_i32()))
        fx = u - SF.from_u32(S, i)
        omfx = 1.0 - fx
        fy = v - SF.from_u32(S, j)
        omfy = 1.0 - fy
        base = (j * P.cols + i) * 2
        nxt = P.cols * 2
        def pt(off): return SF.from_f32(S, tab.load(base + off))
        a0, a1, b0, b1 = pt(0), pt(1), pt(2), pt(3)
        c0, c1, d0, d1 = pt(nxt), pt(nxt + 1), pt(nxt + 2), pt(nxt + 3)
        t0 = a0 * omfx + b0 * fx
        t1 = c0 * omfx + d0 * fx
        t2 = a1 * omfx + b1 * fx
        t3 = c1 * omfx + d1 * fx
        sx = (t0 * omfy + t1 * fy) * wd
        sy = (t2 * omfy + t3 * fy) * hd
        cx = SF.sel(S, sx < 0.5, SF.const(S, 0.5), SF.sel(S, sx > wd - 0.5, wd - 0.5, sx))
        cy = SF.sel(S, sy < 0.5, SF.const(S, 0.5), SF.sel(S, sy > hd - 0.5, hd - 0.5, sy))
        S.buf('out').store(S.gy * P.w + S.gx, buf_sample(S, S.buf('in0'), P.w, P.h, cx, cy))

# ---------------------------------------------------------------------------------------------------------- LAB Adjust
def sf_cbrt(S, x):
    """cbrt of a positive double: the power x^(1/3) and one Newton step (the nearest double but for a rare last bit)"""
    y = sf_pow(S, x, SF.const(S, 1.0 / 3.0))
    return y - (y * y * y - x) / (y * y * 3.0)

@kernel("labadjust", WH + dpush('light', 'da', 'db', 'des') + [('invert', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT, TABLE])
def labadjust(S):
    # labAdjust() in builtins_more_colour.cpp, in doubles. Table: srgbToLinear of the 256 levels (doubles), then the 255 steps of the encoding (doubles).
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        tab = S.buf('table')
        a = p.w
        has = a > 1e-6
        z = S.f(0.0)
        straight = [S.select(has, p.x / a, z), S.select(has, p.y / a, z), S.select(has, p.z / a, z)]
        res = S.var('v4', S.splat4(0.0))
        with S.if_(a > 0.0):
            def lin(f):
                k = S.to_u(S.max(S.to_i(f * 255.0 + 0.5), 0))
                key = S.to_f(S.min(k, 255)) * S.f(1.0 / 255.0)
                hit = (f >= 0.0) & (f <= 1.0) & (k <= 255) & S.eq(key, f)
                out = SFVar(S, sf_zero(S))
                with S.if_(hit):
                    out.set(ld_sf(S, tab, S.min(k, 255) * 2))
                with S.else_():
                    v = sf_clamp(S, SF.from_f32(S, f), 0.0, 1.0)
                    pw = sf_pow(S, (v + 0.055) / 1.055, SF.const(S, 2.4))
                    out.set(SF.sel(S, v <= 0.04045, v / 12.92, pw))
                return out.get()
            lr, lg, lb = lin(straight[0]), lin(straight[1]), lin(straight[2])
            wx, wz = 0.95047, 1.08883
            X = (lr * 0.4124564 + lg * 0.3575761 + lb * 0.1804375) / wx
            Y = lr * 0.2126729 + lg * 0.7151522 + lb * 0.0721750
            Z = (lr * 0.0193339 + lg * 0.1191920 + lb * 0.9503041) / wz
            e, kk = 216.0 / 24389.0, 24389.0 / 27.0
            def f(v):
                return SF.sel(S, v > e, sf_cbrt(S, SF.sel(S, v > e, v, SF.const(S, 1.0))), (v * kk + 16.0) / 116.0)
            fx, fy, fz = f(X), f(Y), f(Z)
            L = fy * 116.0 - 16.0
            A = (fx - fy) * 500.0
            B = (fy - fz) * 200.0
            L = SF.sel(S, S.ne(P.invert, 0), 95.58 - L, L)
            L = L * pd(P, 'light')
            omd = 1.0 - pd(P, 'des')
            A = (A + pd(P, 'da') * 100.0) * omd
            B = (B + pd(P, 'db') * 100.0) * omd
            gy = (L + 16.0) / 116.0
            gx = gy + A / 500.0
            gz = gy - B / 200.0
            def fi(v):
                v3 = v * v * v
                return SF.sel(S, v3 > e, v3, (v * 116.0 - 16.0) / kk)
            x2, y2, z2 = fi(gx) * wx, fi(gy), fi(gz) * wz
            cr = x2 * 3.24045484 - y2 * 1.53713885 - z2 * 0.49853155
            cg = x2 * -0.96926639 + y2 * 1.87601093 + z2 * 0.04155608
            cb = x2 * 0.05564342 - y2 * 0.20402585 + z2 * 1.05722516
            def enc(v):
                # std::upper_bound over the 255 steps: how many are <= v
                lo = S.var('u', S.u(0))
                hi = S.var('u', S.u(255))
                with S.for_(0, 9) as it:
                    mid = (lo.get() + hi.get()) >> 1
                    go = lo.get() < hi.get()
                    le = ld_sf(S, tab, S.u(512) + S.min(mid, 254) * 2) <= v
                    lo.set(S.select(go & le, mid + 1, lo.get()))
                    hi.set(S.select(go & ~le, mid, hi.get()))
                return (SF.from_u32(S, lo.get()) / 255.0).to_f32()
            r, g, b = enc(cr), enc(cg), enc(cb)
            res.set(S.vec4(r * a, g * a, b * a, a))
        S.buf('out').store(i, res.get())

# ---------------------------------------------------------------------------------------------------------- Convolution Kernel
@kernel("conv_prep", WH + [('lv', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def conv_prep(S):
    # convolutionKernelRaw(): the straight colour times 255; in 8 bits a value within float noise of a whole level becomes that level
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        a = p.w
        has = a > 1e-6
        out = []
        for c in (p.x, p.y, p.z):
            t = S.select(has, c / a, S.f(0.0)) * 255.0
            r = S.floor(t + 0.5)
            out.append(S.select(S.ne(P.lv, 0) & (S.abs(t - r) < 2e-3), r, t))
        S.buf('out').store(i, S.vec4(out[0], out[1], out[2], a))

@kernel("conv_main", WH + [('k%d' % q, 'f') for q in range(9)] + [('mask', 'u'), ('mode', 'u'), ('mul', 'f'), ('off', 'f'), ('lv', 'u')] + dpush('cr', 'sr', 'scale'), [PIC_IN, PIC_OUT])
def conv_main(S):
    # convolutionKernelRaw() (the plain per-pixel sum over the taps in order) and the cut to whole levels of convolutionKernel()
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        inb = S.buf('in0')
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        wd, hd = SF.from_u32(S, P.w), SF.from_u32(S, P.h)
        acc = [S.var('f', S.f(0.0)) for _ in range(3)]
        cr, sr, scale = pd(P, 'cr'), pd(P, 'sr'), pd(P, 'scale')
        xd, yd = SF.from_u32(S, S.gx) + 0.5, SF.from_u32(S, S.gy) + 0.5
        for q in range(9):
            r_, col = q // 3, q % 3
            dx, dy = col - 1, r_ - 1
            with S.if_(S.ne(P.mask & (1 << q), 0)):
                kv = getattr(P, 'k%d' % q)
                fdx, fdy = SF.const(S, float(dx)), SF.const(S, float(dy))
                px = S.var('v4', S.splat4(0.0))
                with S.if_(S.eq(P.mode, 0)):
                    xx = S.to_u(S.clamp(S.to_i(S.gx) + dx, 0, wi - 1))
                    yy = S.to_u(S.clamp(S.to_i(S.gy) + dy, 0, hi - 1))
                    px.set(inb.load(yy * P.w + xx))
                with S.else_():
                    sx_t = (xd + fdx * cr) - fdy * sr
                    sy_t = (yd + fdx * sr) + fdy * cr
                    sx_b = xd + (fdx * cr - fdy * sr) * scale
                    sy_b = yd + (fdx * sr + fdy * cr) * scale
                    between = S.eq(P.mode, 2)
                    sx = SF.sel(S, between, sx_b, sx_t)
                    sy = SF.sel(S, between, sy_b, sy_t)
                    cx = SF.sel(S, sx < 0.5, SF.const(S, 0.5), SF.sel(S, sx > wd - 0.5, wd - 0.5, sx))
                    cy = SF.sel(S, sy < 0.5, SF.const(S, 0.5), SF.sel(S, sy > hd - 0.5, hd - 0.5, sy))
                    px.set(buf_sample(S, inb, P.w, P.h, cx, cy))
                t = px.get()
                acc[0].set(acc[0].get() + kv * t.x)
                acc[1].set(acc[1].get() + kv * t.y)
                acc[2].set(acc[2].get() + kv * t.z)
        a = inb.load(i).w
        v = [(acc[k].get() * P.mul + P.off) * a for k in range(3)]
        has = a > 1e-6
        def cut(o):
            w = o / a
            lvl = S.select(w >= 0.0, S.floor(S.select(S.f(255.0) < w, S.f(255.0), w)), S.f(0.0))
            return (S.select(S.ne(P.lv, 0), lvl, w) / 255.0) * a
        out = [S.select(has, cut(o), o) for o in v]
        S.buf('out').store(i, S.vec4(out[0], out[1], out[2], a))

# ---------------------------------------------------------------------------------------------------------- Mirror, slanted lines in 8 bits
@kernel("mirror8", WH + [('sx', 'f'), ('isx', 'f'), ('isy', 'f'), ('cxs', 'f'), ('cys', 'f'), ('t', 'f'), ('m1t', 'f'), ('tcx', 'f'), ('den', 'f'),
                         ('up', 'u'), ('bg', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def mirror8(S):
    # mirror8General() in builtins_filters.cpp: VEGAS's float steps on the bytes
    P = S.p
    with S.in_range(P.w, P.h):
        inb = S.buf('in0')
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        x, j = S.gx, S.gy
        def packed(q):
            return (S.to_u(to_level(S, q.x))) | (S.to_u(to_level(S, q.y)) << 8) | (S.to_u(to_level(S, q.z)) << 16) | (S.to_u(to_level(S, q.w)) << 24)
        def tap(xx, yy):
            inside = (xx >= 0) & (yy >= 0) & (xx < wi) & (yy < hi)
            q = inb.load(S.to_u(S.clamp(yy, 0, hi - 1)) * P.w + S.to_u(S.clamp(xx, 0, wi - 1)))
            return S.select(inside, packed(q), P.bg)
        own = tap(S.to_i(x), S.to_i(j))
        Yj = S.to_f(j) * P.isy
        xs = (((Yj - P.cys) / P.t) + P.cxs) * P.sx
        e = S.to_i(xs) + 1
        upv = S.ne(P.up, 0)
        mir_lo = S.select(upv, S.max(S.min(e, wi), 0), S.i(0))
        mir_hi = S.select(upv, wi, S.min(e, wi))
        mirrored = (S.to_i(x) >= mir_lo) & (S.to_i(x) < mir_hi)
        xu = S.to_f(x) * P.isx
        Pp = (((Yj - xu * P.m1t) - P.cys) + P.tcx) / P.den
        srcX = ((Pp + Pp) - xu) * P.sx
        srcY = ((((Pp - P.cxs) * P.t + P.cys) + ((Pp - P.cxs) * P.t + P.cys)) - Yj) * P.sx
        x0 = S.to_i(S.floor(srcX))
        y0 = S.to_i(S.floor(srcY))
        outside = (x0 < -1) | (x0 >= wi) | (y0 < -1) | (y0 >= hi)
        fx = srcX - S.to_f(x0)
        fy = srcY - S.to_f(y0)
        ix = 1.0 - fx
        iy = 1.0 - fy
        tl, tr, bl, br = tap(x0, y0), tap(x0 + 1, y0), tap(x0, y0 + 1), tap(x0 + 1, y0 + 1)
        bil = S.u(0)
        for k in range(4):
            sh = 8 * k
            a_ = S.to_f((tl >> sh) & 255)
            b_ = S.to_f((tr >> sh) & 255)
            c_ = S.to_f((bl >> sh) & 255)
            d_ = S.to_f((br >> sh) & 255)
            r = (a_ * ix + b_ * fx) * iy + (c_ * ix + d_ * fx) * fy
            bil = bil | ((S.to_u(S.to_i(r)) & 255) << sh)
        px = S.select(mirrored, S.select(outside, P.bg, bil), own)
        k255 = S.f(1.0 / 255.0)
        S.buf('out').store(j * P.w + x, S.vec4(S.to_f(S.to_i(px & 255)) * k255, S.to_f(S.to_i((px >> 8) & 255)) * k255, S.to_f(S.to_i((px >> 16) & 255)) * k255,
                                               S.to_f(S.to_i((px >> 24) & 255)) * k255))

# ---------------------------------------------------------------------------------------------------------- Pixelate
@kernel("pixelate_blocks", WH + [('nbx', 'u'), ('nby', 'u'), ('kx', 'u'), ('ky', 'u'), ('lv', 'u'), ('copy_only', 'u')], [PIC_IN, PIC_OUT, TABLE])
def pixelate_blocks(S):
    # one invocation per cell: pixelate() in builtins_filters.cpp - the average of the cell's window, summed in order (bytes as whole numbers in 8 bits,
    # doubles otherwise). The table holds the cells' first pixels across (nbx + 1 of them, the last one W) and then those down counted from the bottom row (nby + 1)
    P = S.p
    with S.in_range(P.nbx, P.nby):
        W, H = S.to_i(P.w), S.to_i(P.h)
        x0 = S.to_i(S.to_u(S.buf('table').load(S.gx)))
        y0 = S.to_i(S.to_u(S.buf('table').load(P.nbx + 1 + S.gy)))
        x1 = S.min(x0 + S.to_i(P.kx), W)
        y1 = S.min(y0 + S.to_i(P.ky), H)
        inb = S.buf('in0')
        n = SF.from_i32(S, x1 - x0) * SF.from_i32(S, y1 - y0)
        res = S.var('v4', S.splat4(0.0))
        with S.if_(S.ne(P.lv, 0)):
            tot = [S.var('u', S.u(0)) for _ in range(4)]
            with S.for_(y0, y1, 1, True) as yb:
                with S.for_(x0, x1, 1, True) as x:
                    q = inb.load(S.to_u(H - 1 - yb) * P.w + S.to_u(x))
                    for k in range(4):
                        tot[k].set(tot[k].get() + S.to_u(to_level(S, q[k])))
            inv = SF.const(S, 1.0) / n
            out = []
            for k in range(4):
                b = (SF.from_u32(S, tot[k].get()) * inv).trunc_i32()
                x2 = (SF.from_i32(S, b) * 0.00392156862745098) * 255.0
                m = SF.sel(S, SF.const(S, 255.0) < x2, SF.const(S, 255.0), x2)
                b2 = S.select(S.ne(P.copy_only, 0), b, m.trunc_i32())
                out.append(from_level(S, b2))
            res.set(S.vec4(out[0], out[1], out[2], out[3]))
        with S.else_():
            tot = [SFVar(S, sf_zero(S)) for _ in range(4)]
            with S.for_(y0, y1, 1, True) as yb:
                with S.for_(x0, x1, 1, True) as x:
                    q = inb.load(S.to_u(H - 1 - yb) * P.w + S.to_u(x))
                    for k in range(4):
                        tot[k].set(tot[k].get() + SF.from_f32(S, q[k]))
            res.set(S.vec4(*[(tot[k].get() / n).to_f32() for k in range(4)]))
        S.buf('out').store(S.gy * P.nbx + S.gx, res.get())

# ---------------------------------------------------------------------------------------------------------- Spherize
def sf_nearbyint(S, x):
    """nearbyint(): to the nearest whole number, halves to even; an int (|x| < 2^31)"""
    f = x.floor()
    diff = x - f
    fi = f.trunc_i32()
    up = (diff > 0.5) | (diff.eq(0.5) & S.ne(fi & 1, 0))
    return fi + S.select(up, S.i(1), S.i(0))

@kernel("spherize", WH + dpush('kx', 'ky', 'amount') + [('cxp', 'i'), ('cyp', 'i'), ('q8', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def spherize(S):
    # spherize() in builtins_filters2.cpp (VEGAS's CPU processor), in doubles
    from sf64 import sf_cos
    P = S.p
    with S.in_range(P.w, P.h):
        inb = S.buf('in0')
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        i = S.to_i(S.gx)
        yt = S.gy
        j = hi - 1 - S.to_i(yt)
        dy = j - P.cyp
        dx = i - P.cxp
        kx, ky, amount = pd(P, 'kx'), pd(P, 'ky'), pd(P, 'amount')
        yy0 = ((SF.from_i32(S, dy) * 2.0) / SF.from_i32(S, hi)) * ky
        yy = yy0 * yy0
        xx = SF.from_i32(S, dx) * kx
        r2 = xx * xx + yy
        res = S.var('v4', inb.load(yt * P.w + S.gx))
        with S.if_(r2 < 1.0):
            f = 1.0 - ((sf_cos(S, (r2.sqrt() * 3.1415926) * 0.5) * amount) * 0.8)
            sy = f * SF.from_i32(S, dy) + SF.from_i32(S, P.cyp)
            sx = f * SF.from_i32(S, dx) + SF.from_i32(S, P.cxp)
            iy = sf_nearbyint(S, sy - 0.499999985)
            ix = sf_nearbyint(S, sx - 0.499999985)
            y0, y1 = S.clamp(iy, 0, hi - 1), S.clamp(iy + 1, 0, hi - 1)
            x0, x1 = S.clamp(ix, 0, wi - 1), S.clamp(ix + 1, 0, wi - 1)
            def at(xx_, yy_): return inb.load(S.to_u(hi - 1 - yy_) * P.w + S.to_u(xx_))
            p00, p10, p01, p11 = at(x0, y0), at(x1, y0), at(x0, y1), at(x1, y1)
            fyd = sy - SF.from_i32(S, iy)
            fxd = sx - SF.from_i32(S, ix)
            out = []
            for k in range(4):
                # 8-bit: whole-level taps and weights in 1/256
                fy8 = (fyd * 256.0).trunc_i32()
                fx8 = (fxd * 256.0).trunc_i32()
                def v8(q):
                    t = S.min(S.max(q[k], 0.0), 1.0) * 255.0
                    ti = S.to_i(t)
                    return ti + S.select((t - S.to_f(ti)) >= 0.5, S.i(1), S.i(0))
                a = (v8(p00) * (256 - fx8) + v8(p10) * fx8) * (256 - fy8) + (v8(p01) * (256 - fx8) + v8(p11) * fx8) * fy8
                o8 = S.to_f(a >> 16) * S.f(1.0 / 255.0)
                # float: doubles throughout
                omx, omy = 1.0 - fxd, 1.0 - fyd
                a0 = SF.from_f32(S, p00[k]) * omx + SF.from_f32(S, p10[k]) * fxd
                a1 = SF.from_f32(S, p01[k]) * omx + SF.from_f32(S, p11[k]) * fxd
                of = (a0 * omy + a1 * fyd).to_f32()
                out.append(S.select(S.ne(P.q8, 0), o8, of))
            res.set(S.vec4(out[0], out[1], out[2], out[3]))
        S.buf('out').store(yt * P.w + S.gx, res.get())

# ---------------------------------------------------------------------------------------------------------- Deriche Gaussian in doubles
# Coefficient table (pairs of words, ld_sf): 0-3 n0..n3, 4-7 m1..m4, 8-11 d1..d4, 12-16 eN0..eN4 (n - bn), 17-21 eM0..eM4 (m - bm)
DER_N, DER_M, DER_D, DER_EN, DER_EM = 0, 4, 8, 12, 17

@kernel("deriche", WH + [('vert', 'u'), ('inlv', 'u'), ('outlv', 'u'), ('pad', 'u')], [PIC_IN, (1, 'st', 'u', 'rw'), PIC_OUT, TABLE], local=(64, 1))
def deriche(S):
    """One line (a row, or a column with vert) of VEGAS's Deriche Gaussian, vegasDericheBlurSigma() / dericheLanes() in builtins_deriche.cpp, in doubles
    in the CPU's order of operations, one invocation per line, each channel in turn: the causal half (the first five values from the edge terms) into the
    state buffer (the double's two words, 8 words a pixel), then the anti-causal half added to it. inlv: the input is a picture (0..1) taken to levels
    (1; 2: whole ones); 0: it is levels already (the middle plane). outlv: the result back to 0..1 (1; 2: rounded to whole levels first); 0: as it is."""
    P = S.p
    vert = S.ne(P.vert, 0)
    n = S.select(vert, P.h, P.w)
    lines = S.select(vert, P.w, P.h)
    with S.if_(S.gx < lines):
        line = S.gx
        tab = S.buf('table')
        st = S.buf('st')
        src = S.buf('in0')
        out = S.buf('out')
        def idx(q): return S.select(vert, q * P.w + line, line * P.w + q)
        def pos(t): return S.select(vert, n - 1 - t, t)
        def co(i): return ld_sf(S, tab, S.u(2 * i))
        c255 = SF.const(S, 255.0)
        zero = SF.const(S, 0.0)
        lim = SF.const(S, 1e-17)
        def tiny(a):
            return SF.sel(S, (a < lim) & (a > lim.neg()), zero, a)
        def pick(q, ch):
            return S.select(S.eq(ch, 0), q.x, S.select(S.eq(ch, 1), q.y, S.select(S.eq(ch, 2), q.z, q.w)))
        def get(q, ch):
            v = pick(src.load(idx(q)), ch)
            f = SF.from_f32(S, v)
            sc = f * c255
            r = SF.from_i32(S, sf_nearbyint(S, SF.sel(S, sc < zero, zero, SF.sel(S, sc > c255, c255, sc))))
            return SF.sel(S, S.eq(P.inlv, 0), f, SF.sel(S, S.eq(P.inlv, 1), sc, r))
        def ld(pix, ch, w):
            return st.load(pix * 8 + ch * 2 + w)
        with S.for_(0, 4) as ch:
            # ---- causal
            xL = get(pos(S.u(0)), ch)
            x1, x2, x3 = SFVar(S, zero), SFVar(S, zero), SFVar(S, zero)
            y1, y2, y3, y4 = SFVar(S, zero), SFVar(S, zero), SFVar(S, zero), SFVar(S, zero)
            with S.for_(0, n) as t:
                x0 = get(pos(t), ch)
                acc = SFVar(S, zero)
                with S.if_(t < 5):
                    xs = [x0, x1.get(), x2.get(), x3.get()]
                    ys = [zero, y1.get(), y2.get(), y3.get()]
                    for k in range(5):
                        b = co(DER_EN + k) * xL
                        if k < 4:
                            a = x0 * co(DER_N + k) if k == 0 else xs[k] * co(DER_N + k) - co(DER_D + k - 1) * ys[k]
                            acc.set(acc.get() + SF.sel(S, t > k, a, b))
                        else:
                            acc.set(acc.get() + b)
                with S.else_():
                    a = x0 * co(DER_N)
                    a = a + (x1.get() * co(DER_N + 1) - co(DER_D) * y1.get())
                    a = a + (x2.get() * co(DER_N + 2) - co(DER_D + 1) * y2.get())
                    a = a + (x3.get() * co(DER_N + 3) - co(DER_D + 2) * y3.get())
                    a = a - co(DER_D + 3) * y4.get()
                    acc.set(a)
                y = tiny(acc.get())
                pix = idx(pos(t))
                st.store(pix * 8 + ch * 2, y.hi)
                st.store(pix * 8 + ch * 2 + 1, y.lo)
                x3.set(x2.get()); x2.set(x1.get()); x1.set(x0)
                y4.set(y3.get()); y3.set(y2.get()); y2.set(y1.get()); y1.set(y)
            # ---- anti-causal, and the sum
            xR = get(pos(n - 1), ch)
            a1, a2, a3, a4 = SFVar(S, zero), SFVar(S, zero), SFVar(S, zero), SFVar(S, zero)
            z1, z2, z3, z4 = SFVar(S, zero), SFVar(S, zero), SFVar(S, zero), SFVar(S, zero)
            with S.for_(0, n) as t:
                q = pos(n - 1 - t)
                x0 = get(q, ch)
                acc = SFVar(S, zero)
                with S.if_(t < 5):
                    xs = [x0, a1.get(), a2.get(), a3.get(), a4.get()]
                    zs = [zero, z1.get(), z2.get(), z3.get(), z4.get()]
                    for k in range(1, 5):
                        b = co(DER_EM + k) * xR
                        a = xs[k] * co(DER_M + k - 1) - co(DER_D + k - 1) * zs[k]
                        acc.set(acc.get() + SF.sel(S, t > k, a, b))
                with S.else_():
                    a = a1.get() * co(DER_M) - co(DER_D) * z1.get()
                    a = a + (a2.get() * co(DER_M + 1) - co(DER_D + 1) * z2.get())
                    a = a + (a3.get() * co(DER_M + 2) - co(DER_D + 2) * z3.get())
                    a = a + (a4.get() * co(DER_M + 3) - co(DER_D + 3) * z4.get())
                    acc.set(a)
                z = tiny(acc.get())
                pix = idx(q)
                yc = SF(S, ld(pix, ch, 0), ld(pix, ch, 1))
                tot = yc + z
                r = SF.from_i32(S, sf_nearbyint(S, SF.sel(S, tot < zero, zero, SF.sel(S, tot > c255, c255, tot))))
                v = SF.sel(S, S.eq(P.outlv, 2), r, tot)
                v = SF.sel(S, S.eq(P.outlv, 0), tot, v / c255)
                st.store(pix * 8 + ch * 2, S.f_to_bits(v.to_f32()))
                a4.set(a3.get()); a3.set(a2.get()); a2.set(a1.get()); a1.set(x0)
                z4.set(z3.get()); z3.set(z2.get()); z2.set(z1.get()); z1.set(z)
        with S.for_(0, n) as t:
            pix = idx(t)
            def fv(ch): return S.bits_to_f(st.load(pix * 8 + ch * 2))
            out.store(pix, S.vec4(fv(0), fv(1), fv(2), fv(3)))

@kernel("unsharp_mix", WH + dpush('amount') + [('thr', 'f'), ('lv', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def unsharp_mix(S):
    # unsharpMask() in builtins_more_blur.cpp in doubles: in0 the frame, in1 its blur; every channel, in levels (whole ones for 8-bit pictures)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        bl = S.buf('in1').load(i)
        q = S.ne(P.lv, 0)
        c255, zero = SF.const(S, 255.0), SF.const(S, 0.0)
        amount = pd(P, 'amount')
        thr = SF.from_f32(S, P.thr)
        res = []
        for ch in (0, 1, 2, 3):
            def lev(v):
                d = SF.from_f32(S, S.select(q, S.min(S.max(v, 0.0), 1.0), v)) * c255
                return SF.sel(S, q, SF.from_i32(S, sf_nearbyint(S, d)), d)
            sv, bv = lev(s[ch]), lev(bl[ch])
            v = sv + (sv - bv) * amount
            v = SF.sel(S, v < zero, zero, v)
            v = SF.sel(S, v > c255, c255, v)
            v = SF.sel(S, q, SF.from_i32(S, v.trunc_i32()), v)
            d = v - bv
            d = SF.sel(S, d < zero, d.neg(), d)
            res.append((SF.sel(S, d > thr, v, sv) / c255).to_f32())
        S.buf('out').store(i, S.vec4(res[0], res[1], res[2], res[3]))

# ---------------------------------------------------------------------------------------------------------- Quick Blur, channel mix
@kernel("quickblur", WH + [('wb', 'i'), ('wf', 'f'), ('lv', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def quickblur(S):
    # quickBlur() in builtins_more_blur.cpp: a 3 x 3 binomial blur (edges repeated), then a mix towards the source; whole levels in 8-bit
    P = S.p
    with S.in_range(P.w, P.h):
        inb = S.buf('in0')
        x0, y0 = S.to_i(S.gx), S.to_i(S.gy)
        xs = [S.to_u(S.max(x0 - 1, 0)), S.gx, S.to_u(S.min(x0 + 1, S.to_i(P.w) - 1))]
        ys = [S.to_u(S.max(y0 - 1, 0)), S.gy, S.to_u(S.min(y0 + 1, S.to_i(P.h) - 1))]
        wt = [1, 2, 1, 2, 4, 2, 1, 2, 1]
        taps = [inb.load(ys[j // 3] * P.w + xs[j % 3]) for j in range(9)]
        res = S.var('v4', S.splat4(0.0))
        with S.if_(S.ne(P.lv, 0)):
            out = []
            for ch in range(4):
                tot = S.i(0)
                for j in range(9):
                    tot = tot + to_level(S, taps[j][ch]) * wt[j]
                bl = tot >> 4
                sv = to_level(S, taps[4][ch])
                t = (sv - bl) * P.wb
                t = t + S.select(t < 0, S.i(255), S.i(0))
                out.append(from_level(S, bl + (t >> 8)))
            res.set(S.vec4(out[0], out[1], out[2], out[3]))
        with S.else_():
            out = []
            for ch in range(4):
                tot = S.f(0.0)
                for j in range(9):
                    tot = tot + taps[j][ch] * float(wt[j])
                bl = tot / 16.0
                out.append(bl + (taps[4][ch] - bl) * P.wf)
            res.set(S.vec4(out[0], out[1], out[2], out[3]))
        S.buf('out').store(S.gy * P.w + S.gx, res.get())

@kernel("mask_mix", WH + [('mask', 'u'), ('pad', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def mask_mix(S):
    # the channels of the mask (bit 0 red .. 3 alpha) from in1, the others from in0
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        a, b = S.buf('in0').load(i), S.buf('in1').load(i)
        def pick(bit, new, old): return S.select(S.ne(P.mask & (1 << bit), 0), new, old)
        S.buf('out').store(i, S.vec4(pick(0, b.x, a.x), pick(1, b.y, a.y), pick(2, b.z, a.z), pick(3, b.w, a.w)))

# ---------------------------------------------------------------------------------------------------------- Linear Blur
# vegasLinearBlur() in builtins_deriche.cpp: the frame as levels (lb_pic), sheared (lb_in; a pixel of the sheared frame worked out directly from the
# CPU's column loop, see shearIn()), blurred by the Deriche passes, sheared back (lb_out, shearOut()).
@kernel("lb_pic", WH + [('lv', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def lb_pic(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        q = S.buf('in0').load(i)
        c255, zero = SF.const(S, 255.0), SF.const(S, 0.0)
        out = []
        for ch in range(4):
            d = SF.from_f32(S, q[ch]) * c255
            r = SF.from_i32(S, sf_nearbyint(S, SF.sel(S, d < zero, zero, SF.sel(S, d > c255, c255, d))))
            out.append(SF.sel(S, S.ne(P.lv, 0), r, d).to_f32())
        S.buf('out').store(i, S.vec4(out[0], out[1], out[2], out[3]))

def lb_mix8(S, a, b, w8, lv):
    """mix8() in builtins_deriche.cpp: a, b vec4 of levels, w8 an int 0..254"""
    res = S.var('v4', S.splat4(0.0))
    c255 = SF.const(S, 255.0)
    with S.if_(lv):
        out = []
        for ch in range(4):
            A = S.to_i(a[ch])
            t = (S.to_i(b[ch]) - A) * w8 + 128
            v = A + ((t + (t >> 8)) >> 8)
            out.append(S.to_f(S.min(S.max(v, 0), 255)))
        res.set(S.vec4(out[0], out[1], out[2], out[3]))
    with S.else_():
        out = []
        w = S.to_f(w8)
        for ch in range(4):
            m = (b[ch] - a[ch]) * w
            q = (SF.from_f32(S, m) / c255).to_f32()
            out.append(S.min(S.f(255.0), S.max(S.f(0.0), a[ch] + q)))
        res.set(S.vec4(out[0], out[1], out[2], out[3]))
    return res.get()

@kernel("lb_in", [('w', 'u'), ('h', 'u'), ('pw', 'u'), ('ph', 'u'), ('memory', 'u'), ('across', 'u'), ('shear', 'u'), ('bufh', 'u'), ('lv', 'u'), ('s0', 'u'), ('pad', 'u'), ('pad2', 'u')],
        [PIC_IN, PIC_OUT, TABLE])
def lb_in(S):
    """shearIn() for the pixel (x, y) of the sheared frame (w x h = the frame as it is blurred), as VEGAS's column loop leaves it: x's own rows from
    the picture, the rows above from the right-hand edge column, those below from the left-hand one (both worked out from the table of X per row)."""
    P = S.p
    with S.in_range(P.w, P.h):
        memory, across, lv = S.ne(P.memory, 0), S.ne(P.across, 0), S.ne(P.lv, 0)
        gy = S.select(memory, P.h - 1 - S.gy, S.gy)
        sx = S.select(across, gy, S.gx)             # column and row of the sheared plane
        k = S.select(across, S.gx, gy)
        FW = S.select(across, P.ph, P.pw)
        FH = S.select(across, P.pw, P.ph)
        pic = S.buf('in0')
        def srcF(x, y):
            gx_ = S.select(across, y, x)
            gy_ = S.select(across, x, y)
            yy = S.select(memory, P.ph - 1 - gy_, gy_)
            return pic.load(yy * P.pw + gx_)
        inc = S.select(FW > 1, P.shear / S.max(FW - 1, S.u(1)), S.u(0))
        tab = S.buf('table')
        res = S.var('v4', S.splat4(0.0))
        with S.if_(S.eq(inc, 0)):
            res.set(srcF(sx, S.min(k, FH - 1)))
        with S.else_():
            acc = sx * inc
            r12 = acc >> 14
            w8 = S.to_i((((acc & 0x3fff) * 255) >> 14))
            right = k <= r12
            # the preset of the edge column at row k: right: dst(W-1, k) from the first row; left: dst(0, k) from the last row
            rr = S.select(right, k, P.bufh - 1 - S.min(k, P.bufh - 1))
            valid = S.select(right, (k <= P.s0) & (k < P.bufh), (k < P.bufh) & (rr <= P.s0))
            X = tab.load(S.min(rr, P.s0))
            Xu = S.f_to_bits(X)
            Xi = Xu >> 14
            pw8 = S.to_i(((Xu & 0x3fff) * 255) >> 14)
            c0 = S.min(Xi, FW - 1)
            c1 = S.min(c0 + 1, FW - 1)
            R = S.select(FW - 1 > Xi, FW - 1 - Xi, S.u(0))
            R2 = S.select(R > 0, R - 1, R)
            pa = S.select(right, srcF(c0, S.u(0)), srcF(R, FH - 1))
            pb = S.select(right, srcF(c1, S.u(0)), srcF(R2, FH - 1))
            pm = lb_mix8(S, pa, pb, pw8, lv)
            preset = S.select(valid, pm, S.splat4(0.0))
            kk = k - S.min(k, r12)
            inside = (k > r12) & (k < r12 + FH)
            ka = S.min(kk, FH - 1)
            kb = S.min(S.max(kk, S.u(1)) - 1, FH - 1)
            is2 = S.eq(k, r12)
            is4 = S.eq(k, r12 + FH)
            a = S.select(is2, srcF(sx, S.u(0)), S.select(is4, preset, srcF(sx, ka)))
            b = S.select(is2, preset, S.select(is4, srcF(sx, FH - 1), srcF(sx, kb)))
            mixed = lb_mix8(S, a, b, w8, lv)
            res.set(S.select(is2 | is4 | inside, mixed, preset))
        c255 = SF.const(S, 255.0)
        v = res.get()
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(*[(SF.from_f32(S, v[ch]) / c255).to_f32() for ch in range(4)]))

@kernel("lb_out", [('w', 'u'), ('h', 'u'), ('fw', 'u'), ('fh', 'u'), ('memory', 'u'), ('across', 'u'), ('shear', 'u'), ('bufh', 'u'), ('lv', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def lb_out(S):
    """shearOut() for the pixel (x, y) of the picture: in0 is the blurred frame (fw x fh, 0..1)"""
    P = S.p
    with S.in_range(P.w, P.h):
        memory, across, lv = S.ne(P.memory, 0), S.ne(P.across, 0), S.ne(P.lv, 0)
        gy = S.select(memory, P.h - 1 - S.gy, S.gy)
        fx = S.select(across, gy, S.gx)
        fy = S.select(across, S.gx, gy)
        Wb = S.select(across, P.h, P.w)               # the sheared plane's width
        frame = S.buf('in0')
        c255, zero = SF.const(S, 255.0), SF.const(S, 0.0)
        def readS(x, y):
            gx_ = S.select(across, y, x)
            gy_ = S.select(across, x, y)
            by = S.select(memory, P.fh - 1 - gy_, gy_)
            q = frame.load(by * P.fw + gx_)
            out = []
            for ch in range(4):
                d = SF.from_f32(S, q[ch]) * c255
                out.append(SF.sel(S, S.ne(P.lv, 0), SF.from_i32(S, sf_nearbyint(S, d)), d).to_f32())
            return S.vec4(out[0], out[1], out[2], out[3])
        inc = P.shear / S.max(Wb, S.u(1))
        res = S.var('v4', S.splat4(0.0))
        with S.if_(S.eq(inc, 0)):
            res.set(readS(fx, S.min(fy, P.bufh - 1)))
        with S.else_():
            acc = fx * inc
            s = acc >> 14
            w8 = S.to_i(((acc & 0x3fff) * 255) >> 14)
            a = S.min(s + fy, P.bufh - 1)
            j = s + fy + 1
            wrapx = across & (fx > 0)
            bx = S.select(j < P.bufh, fx, S.select(wrapx, fx - 1, fx))
            by = S.select(j < P.bufh, j, S.select(wrapx, S.u(0), P.bufh - 1))
            res.set(lb_mix8(S, readS(fx, a), readS(bx, by), w8, lv))
        v = res.get()
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(*[(SF.from_f32(S, v[ch]) / c255).to_f32() for ch in range(4)]))

# ---------------------------------------------------------------------------------------------------------- Cookie Cutter
# builtins_cookie.cpp / builtins_cookie_vegas.h: single-precision float arithmetic in VEGAS's order. One mask kernel per shape (the shape is fixed in the kernel,
# the cell structure is dynamic); the feather is the Deriche pass over the mask; cookie_apply mixes the picture.
def fdiv(S, a, b):
    """the float quotient, correctly rounded (computed in double)"""
    return (SF.from_f32(S, a) / SF.from_f32(S, b)).to_f32()

def _kf(bits):
    import struct
    return struct.unpack("<f", struct.pack("<I", bits))[0]

_CK = dict(c333=0x3eaaaaab, c559=0x3f0f1bbd, c577=0x3f13cd3a, c667=0x3f2aaaab, c1118=0x3f8f1bbd, c1154=0x3f93cd3a, c16=0x3fcccccd, c04=0x3ecccccd, c08=0x3f4ccccd,
           c1732=0x3fddb3d7, m333=0xbeaaaaab, m04=0xbecccccd, m577=0xbf13cd3a, m667=0xbf2aaaab, m08=0xbf4ccccd, m1154=0xbf93cd3a, m16=0xbfcccccd)

def cookie_shape_dist(S, shape, X, Y, Kr):
    """cookieShapeDist() in builtins_cookie_vegas.h for the offset (X, Y) (floats)"""
    c = {k: S.f(_kf(v)) for k, v in _CK.items()}
    z = S.f(0.0)
    m1f = S.f(-1.0)
    ab = S.abs
    def sel(cond, a, b): return S.select(cond, a, b)
    def gt(a, b): return a > b
    if shape == 0: return fsqrt(S, Y * Y + X * X)
    if shape == 1:
        X = X * Kr
        return fsqrt(S, Y * Y + X * X)
    if shape == 2:
        Y = Y * Kr
        return fsqrt(S, Y * Y + X * X)
    if shape == 3:
        a, b = ab(Y), ab(X) * Kr
        return sel(a > b, a, b)
    if shape == 4: return ab(Y) + ab(X)
    if shape == 5:
        a, b = ab(X), ab(Y)
        return sel(a > b, a, b)
    if shape == 6:
        x0 = X * c['c577']
        cond1 = ~(z < X) & (Y > x0)
        A = ab((c['c667'] - Y) * c['m577'] - (z - X) * m1f)
        cond2 = ~(x0 < Y) & ((-x0) > Y)
        B = ab((c['m333'] - Y) * c['c1154'] - (c['m577'] - X) * z)
        C = ab((c['m333'] - Y) * c['m577'] - (c['c577'] - X))
        return sel(cond1, A, sel(cond2, B, C))
    if shape == 7:
        x0 = X * c['c1732']
        x1 = -x0
        cond1 = (Y > x1) & ~(Y < z)
        A = ab((z - Y) * m1f - (c['c667'] - X) * c['c577'])
        cond2 = ~(x1 < Y) & (Y > x0)
        B = ab((c['c577'] - Y) * z - (c['m333'] - X) * c['m1154'])
        C = ab((c['m577'] - Y) - (c['m333'] - X) * c['c577'])
        return sel(cond1, A, sel(cond2, B, C))
    if shape == 8:
        x0 = X * c['c577']
        cond1 = ~(X < z) & (x0 > Y)
        A = ab((c['m667'] - Y) * c['c577'] - (z - X))
        cond2 = (z > X) & ~((-x0) < Y)
        B = ab((c['c333'] - Y) * c['c577'] - (c['m577'] - X) * m1f)
        C = ab((c['c333'] - Y) * c['m1154'] - (c['c577'] - X) * z)
        return sel(cond1, A, sel(cond2, B, C))
    if shape == 9:
        x0 = X * c['c1732']
        cond1 = (Y > z) & ~(Y < x0)
        A = ab((c['c577'] - Y) * m1f - (c['c333'] - X) * c['m577'])
        cond2 = ~(z < Y) & ((-x0) > Y)
        B = ab((c['m577'] - Y) * m1f - (c['c333'] - X) * c['c577'])
        C = ab((c['m577'] - Y) * z - (c['c333'] - X) * c['c1154'])
        return sel(cond1, A, sel(cond2, B, C))
    if shape == 10:
        nx = -X
        o1 = ~(X < z)
        a1 = o1 & (nx > Y)
        A1 = ab((c['m04'] - Y) * c['c08'] - (z - X) * c['m04']) * c['c1118']
        a2 = o1 & ~(nx > Y) & (X > z) & ~(Y < nx)
        A2 = ab((c['m08'] - Y) * c['m08'] - (c['c08'] - X) * c['c16']) * c['c559']
        b = ~(z < X) & (Y > X)
        B = ab((c['c08'] - Y) * c['m08'] - (z - X) * c['m16']) * c['c559']
        C = ab((c['m08'] - Y) * c['c08'] - (c['m08'] - X) * c['c04']) * c['c1118']
        return sel(a1, A1, sel(a2, A2, sel(b, B, C)))
    if shape == 11:
        nx = -X
        o1 = ~(Y < z)
        a1 = o1 & (Y > nx)
        A1 = ab((z - Y) * c['m16'] - (c['c08'] - X) * c['c08']) * c['c559']
        a2 = o1 & ~(Y > nx) & (Y > z) & ~(nx < Y)
        A2 = ab((c['c08'] - Y) * c['c04'] - (c['m08'] - X) * c['m08']) * c['c1118']
        b = ~(z < Y) & (Y > X)
        B = ab((z - Y) * c['m04'] - (c['m04'] - X) * c['m08']) * c['c1118']
        C = ab((c['m08'] - Y) * c['c16'] - (c['m08'] - X) * c['c08']) * c['c559']
        return sel(a1, A1, sel(a2, A2, sel(b, B, C)))
    if shape == 12:
        a = (X > z) & ~(Y < X)
        A = ab((c['c08'] - Y) * c['m08'] - (c['c08'] - X) * c['m04']) * c['c1118']
        o1 = ~(z < X)
        nx = -X
        b1 = o1 & ~(Y <= nx)
        B1 = ab((c['c04'] - Y) * c['m08'] - (z - X) * c['c04']) * c['c1118']
        b2 = o1 & (Y <= nx) & ~(z <= X) & ~(nx < Y)
        B2 = ab((c['c08'] - Y) * c['c08'] - (c['m08'] - X) * c['m16']) * c['c559']
        C = ab((c['m08'] - Y) * c['c08'] - (z - X) * c['c16']) * c['c559']
        return sel(a, A, sel(b1, B1, sel(b2, B2, C)))
    # 13
    o1 = ~(Y < z)
    a1 = o1 & (X > Y)
    A1 = ab((z - Y) * c['c04'] - (c['c04'] - X) * c['c08']) * c['c1118']
    a2 = o1 & ~(X > Y) & (Y > z) & ~(Y < X)
    A2 = ab((c['c08'] - Y) * c['m16'] - (c['c08'] - X) * c['m08']) * c['c559']
    nx = -X
    b = ~(z < Y) & (nx > Y)
    B = ab((z - Y) * c['c16'] - (c['m08'] - X) * c['m08']) * c['c559']
    C = ab((c['m08'] - Y) * c['m04'] - (c['c08'] - X) * c['c08']) * c['c1118']
    return sel(a1, A1, sel(a2, A2, sel(b, B, C)))

def cookie_cands(shape, i, j, rx, ry, which):
    """the cells (di, dj, condition) whose shape the least distance is taken over, tileDist() in builtins_cookie_vegas.h; which: 0 inside, 1 ix == -1,
    2 iy == -1, 3 both; i, j ints (rx, ry the counts)"""
    T = None
    L = [(0, 0, T)]
    def add(di, dj, cond=None): L.append((di, dj, cond))
    def AND(*cs):
        out = None
        for q in cs:
            if q is None: continue
            out = q if out is None else (out & q)
        return out
    s = shape
    if which == 0:
        add(1, 1, AND(i + 1 < rx, j + 1 < ry)); add(1, 0, i + 1 < rx); add(0, 1, j + 1 < ry)
        if s == 11:
            add(1, 2, AND(i + 1 < rx, j + 2 < ry)); add(1, -1, AND(i + 1 < rx, j > 0))
            add(2, 0, i + 2 < rx); add(2, -1, AND(i + 2 < rx, j > 0)); add(2, 1, AND(i + 2 < rx, j + 1 < ry)); add(2, 2, AND(i + 2 < rx, j + 2 < ry))
        elif s == 13:
            add(0, 2, j + 2 < ry); add(0, -1, j > 0)
            add(-1, 0, i > 0); add(-1, 1, AND(i > 0, j + 1 < ry)); add(-1, 2, AND(i > 0, j + 2 < ry)); add(-1, -1, AND(i > 0, j > 0))
        elif s == 10:
            add(2, 1, AND(j + 1 < ry, i + 2 < rx)); add(-1, 1, AND(j + 1 < ry, i > 0))
            add(0, 2, j + 2 < ry); add(2, 2, AND(j + 2 < ry, i + 2 < rx)); add(1, 2, AND(j + 2 < ry, i + 1 < rx)); add(-1, 2, AND(j + 2 < ry, i > 0))
        elif s == 12:
            add(2, 0, i + 2 < rx); add(-1, 0, i > 0)
            add(0, -1, j > 0); add(1, -1, AND(j > 0, i + 1 < rx)); add(2, -1, AND(j > 0, i + 2 < rx)); add(-1, -1, AND(j > 0, i > 0))
    elif which == 1:        # ix == -1, iy = j >= 0 (the cell column 0's centre X0)
        add(0, 1, j + 1 < ry)
        if s == 11:
            add(1, 0, rx > 1); add(1, -1, AND(rx > 1, j > 0)); add(1, 1, AND(rx > 1, j + 1 < ry)); add(1, 2, AND(rx > 1, j + 2 < ry))
            add(0, -1, j > 0); add(1, 2, j + 2 < ry)
        elif s == 10:
            add(1, 1, AND(rx > 1, j + 1 < ry)); add(1, 2, AND(rx > 1, j + 2 < ry)); add(0, 2, j + 2 < ry)
        elif s == 12:
            add(1, -1, AND(rx > 1, j > 0)); add(0, -1, j > 0); add(1, 0, rx > 1)
    elif which == 2:        # iy == -1, ix = i >= 0
        add(1, 0, i + 1 < rx)
        if s == 10:
            add(0, 1, ry > 1); add(1, 1, AND(ry > 1, i + 1 < rx)); add(2, 1, AND(ry > 1, i + 2 < rx)); add(-1, 1, AND(ry > 1, i > 0))
            add(-1, 0, i > 0); add(2, 0, i + 2 < rx)
        elif s == 13:
            add(-1, 1, AND(ry > 1, i > 0)); add(0, 1, ry > 1); add(-1, 0, i > 0)
        elif s == 11:
            add(2, 1, AND(ry > 1, i + 2 < rx)); add(1, 1, AND(ry > 1, i + 1 < rx)); add(2, 0, i + 2 < rx)
    else:                   # both
        if s == 10 or s == 11:
            add(1, 1, AND(rx > 1, ry > 1)); add(1, 0, rx > 1); add(0, 1, ry > 1)
    return L

def _cookie_mask_kernel(shape):
    @kernel("cookie_mask%d" % shape, [('w', 'u'), ('h', 'u'), ('repx', 'u'), ('repy', 'u'), ('cw', 'u'), ('ch', 'u'), ('nob', 'u'), ('pad', 'u'),
                                       ('cx0', 'f'), ('cy0', 'f'), ('ratio', 'f'), ('tin', 'f'), ('tout', 'f'), ('rin', 'f'), ('hx', 'f'), ('hy', 'f'), ('ix0', 'f'), ('iy0', 'f'),
                                       ('ovw', 'f'), ('ovh', 'f'), ('boxhi', 'f'), ('boxlo', 'f'), ('ntout', 'f')], [PIC_IN, PIC_OUT])
    def cookie_mask(S):
        P = S.p
        with S.in_range(P.w, P.h):
            x = S.to_i(S.gx)
            y = S.to_i(P.h - 1 - S.gy)            # (the CPU's rows run bottom row first)
            cwi, chi = S.to_i(P.cw), S.to_i(P.ch)
            rx, ry = S.to_i(P.repx), S.to_i(P.repy)
            xf, yf = S.to_f(x), S.to_f(y)
            inx, iny = P.cx0 <= xf, P.cy0 <= yf
            ix = S.select(inx, S.min((S.to_i(xf - P.cx0)) / cwi, rx - 1), S.i(-1))
            iy = S.select(iny, S.min((S.to_i(yf - P.cy0)) / chi, ry - 1), S.i(-1))
            ax = S.abs(S.to_f(x % cwi) - P.cx0)
            ay = S.abs(S.to_f(y % chi) - P.cy0)
            res = S.var('f', S.f(2.0))
            def tile_dist():
                best = S.var('f', S.f(0.0))
                for which in range(4):
                    cond = {0: (ix >= 0) & (iy >= 0), 1: (ix < 0) & (iy >= 0), 2: (ix >= 0) & (iy < 0), 3: (ix < 0) & (iy < 0)}[which]
                    with S.if_(cond):
                        i = S.max(ix, 0)
                        j = S.max(iy, 0)
                        xb = S.select(ix >= 0, S.to_f(cwi * i) + P.cx0, P.cx0)
                        yb = S.select(iy >= 0, S.to_f(chi * j) + P.cy0, P.cy0)
                        cwf, chf = S.to_f(cwi), S.to_f(chi)
                        def coord(b, c, k):
                            if k == 0: return b
                            if k == 1: return c + b
                            if k == 2: return (b + c) + c
                            return b - c
                        first = True
                        for di, dj, cnd in cookie_cands(shape, i, j, rx, ry, which):
                            def put():
                                d = cookie_shape_dist(S, shape, xf - coord(xb, cwf, di), yf - coord(yb, chf, dj), P.ratio)
                                if first_flag[0]:
                                    best.set(d)
                                else:
                                    with S.if_(d < best.get()):
                                        best.set(d)
                            first_flag = [first]
                            if cnd is None: put()
                            else:
                                with S.if_(cnd): put()
                            first = False
                return best.get()
            if shape == 3:
                with S.if_((P.ix0 > ax) & (P.iy0 > ay)): res.set(S.f(0.0))
                with S.else_():
                    with S.if_((ax > P.hx) | (ay > P.hy)): res.set(S.f(2.0))
                    with S.else_(): res.set(S.f(1.0))
            elif shape in (1, 2):
                dx = fdiv(S, ax, P.ovw)
                dx = dx * dx
                dy = fdiv(S, ay, P.ovh)
                dy = dy * dy
                s2 = dy + dx
                res.set(S.select(P.tin > s2, S.f(0.0), S.select(s2 > P.tout, S.f(2.0), S.f(1.0))))
            elif shape == 0:
                inbox = (ax >= P.boxlo) & (P.boxhi >= ax) & (ay >= P.boxlo) & (P.boxhi > ay)
                outer = (P.ntout > ax) | (ax > P.tout) | (P.ntout > ay) | (ay > P.tout)
                with S.if_(inbox): res.set(S.f(0.0))
                with S.else_():
                    with S.if_(outer): res.set(S.f(2.0))
                    with S.else_():
                        d = tile_dist()
                        nob = S.ne(P.nob, 0)
                        res.set(S.select(nob, S.select(P.rin > d, S.f(0.0), S.f(2.0)), S.select(P.tin > d, S.f(0.0), S.select(d > P.tout, S.f(2.0), S.f(1.0)))))
            else:
                d = tile_dist()
                res.set(S.select(P.tin > d, S.f(0.0), S.select(d > P.tout, S.f(2.0), S.f(1.0))))
            S.buf('out').store(S.gy * P.w + S.gx, S.vec4(res.get(), S.f(0.0), S.f(0.0), S.f(0.0)))
    return cookie_mask

for _s in range(14):
    _cookie_mask_kernel(_s)

@kernel("cookie_apply", WH + [('nob', 'u'), ('cut', 'u'), ('lv', 'u'), ('c0', 'i'), ('c1', 'i'), ('c2', 'i'), ('c3', 'i'), ('br', 'f'), ('bg', 'f'), ('bb', 'f'), ('pad', 'u')],
        [PIC_IN, PIC_IN1, PIC_OUT])
def cookie_apply(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        sp = S.buf('in0').load(i)
        m = S.buf('in1').load(i).x
        nob, cut = S.ne(P.nob, 0), S.ne(P.cut, 0)
        lo = S.f(_kf(0x3727c5ac)) > m
        hi = m > S.f(_kf(0x3fffffac))
        res = S.var('v4', S.splat4(0.0))
        with S.if_(S.ne(P.lv, 0)):
            s = [to_level(S, sp[k]) for k in range(4)]
            z = S.i(0)
            Av = [S.select(cut, s[k], z) for k in range(4)]
            Bv = [S.select(cut, z, s[k]) for k in range(4)]
            col = [P.c0, P.c1, P.c2, P.c3]
            # the pair to mix and its weight
            t_nob = m * 0.5
            t_a = S.select(1.0 >= m, m, m - 1.0)
            first = [S.select(nob | (1.0 >= m), Bv[k], col[k]) for k in range(4)]
            second = [S.select(nob, Av[k], S.select(1.0 >= m, col[k], Av[k])) for k in range(4)]
            t = S.select(nob, t_nob, t_a)
            u = 1.0 - t
            mixed = []
            for k in range(4):
                v = S.to_f(first[k]) * u + S.to_f(second[k]) * t
                n = S.to_i(v + 0.5)
                mixed.append(S.min(S.max(n, 0), 255))
            out = [S.select(lo, Bv[k], S.select(hi, Av[k], mixed[k])) for k in range(4)]
            res.set(S.vec4(from_level(S, out[0]), from_level(S, out[1]), from_level(S, out[2]), from_level(S, out[3])))
        with S.else_():
            cf = [P.br, P.bg, P.bb, S.f(1.0)]
            Av = [S.select(cut, sp[k], S.f(0.0)) for k in range(4)]
            Bv = [S.select(cut, S.f(0.0), sp[k]) for k in range(4)]
            t_nob = m * 0.5
            t = S.select(nob, t_nob, S.select(1.0 >= m, m, m - 1.0))
            first = [S.select(nob | (1.0 >= m), Bv[k], cf[k]) for k in range(4)]
            second = [S.select(nob, Av[k], S.select(1.0 >= m, cf[k], Av[k])) for k in range(4)]
            u = 1.0 - t
            out = []
            for k in range(4):
                mixv = u * first[k] + t * second[k]
                out.append(S.select(lo, Bv[k], S.select(hi, Av[k], mixv)))
            res.set(S.vec4(out[0], out[1], out[2], out[3]))
        S.buf("out").store(i, res.get())

# ---------------------------------------------------------------------------------------------------------- Channel Blend
@kernel("channelblend", WH + [('flags', 'u'), ('pad', 'u')] + [('w%d' % i, 'f') for i in range(20)], [PIC_IN, PIC_OUT])
def channelblend(S):
    # channelBlend() in builtins_more_colour.cpp, in the CPU's order of operations; flags: 1 Normalize, 2 8-bit (the byte sum cut to whole levels)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        ins = [p.x, p.y, p.z, p.w]
        norm = S.ne(P.flags & 1, 0)
        lv = S.ne(P.flags & 2, 0)
        outs = []
        for o in range(4):
            ws = [getattr(P, 'w%d' % (o * 5 + k)) for k in range(5)]
            sm = S.f(0.0)
            wsum = S.abs(ws[4])
            for k in range(4):
                sm = sm + ws[k] * ins[k]
                wsum = wsum + S.abs(ws[k])
            sm = sm + ws[4]
            safe = S.select(wsum > 1e-6, wsum, S.f(1.0))
            smf = S.select(norm, S.select(wsum > 1e-6, fdiv(S, sm, safe), S.f(0.0)), sm)
            fl = clamp01(S, smf)
            bs = ws[4] * 255.0
            for k in range(4):
                bs = bs + ws[k] * S.floor(ins[k] * 255.0 + 0.5)
            bs = S.select(norm, S.select(wsum > 1e-6, fdiv(S, bs, safe), S.f(0.0)), bs)
            t = S.min(S.f(255.0), S.max(S.f(0.0), S.floor(bs)))
            outs.append(S.select(lv, fdiv(S, t, S.f(255.0)), fl))
        S.buf('out').store(i, S.vec4(outs[0], outs[1], outs[2], outs[3]))

# ---------------------------------------------------------------------------------------------------------- Chroma Keyer
# chromaKeyer() in builtins_filters.cpp: the mask bytes (doubles for the HSL, floats for the distance), VEGAS's float recursive blur (rows, then columns
# bottom row first), the bytes of the keyed picture.
def ck_hsl(S, r, g, b):
    """keyerHsl(): (h, sat, l) as SFs"""
    mx = SF.sel(S, r < g, g, r)
    mx = SF.sel(S, ~(mx > b), b, mx)
    mn = SF.sel(S, g < r, g, r)
    mn = SF.sel(S, ~(b > mn), b, mn)
    sm = mn + mx
    l = sm * 0.5
    d = mx - mn
    den = SF.sel(S, ~(l <= 0.5), (SF.const(S, 2.0) - mx) - mn, sm)
    sat = d / den
    inv = SF.const(S, 1.0) / d
    rc, gc, bc = (mx - r) * inv, (mx - g) * inv, (mx - b) * inv
    hr = SF.sel(S, g.eq(mn), bc + 5.0, SF.const(S, 1.0) - gc)
    hg = SF.sel(S, b.eq(mn), rc + 1.0, SF.const(S, 3.0) - bc)
    hb = SF.sel(S, r.eq(mn), gc + 3.0, SF.const(S, 5.0) - rc)
    hh = SF.sel(S, r.eq(mx), hr, SF.sel(S, g.eq(mx), hg, hb))
    h = hh * 0.16666666666666666
    zero = SF.const(S, 0.0)
    flat = ~(zero < l)
    nod = ~(d > zero)
    sat = SF.sel(S, flat, zero, SF.sel(S, nod, d, sat))
    h = SF.sel(S, flat | nod, zero, h)
    return h, sat, l

@kernel("ck_mask", WH + [('krf', 'f'), ('kgf', 'f'), ('kbf', 'f'), ('lowf', 'f'), ('hif', 'f'), ('invr', 'f'), ('pad', 'u'), ('pad2', 'u')] + dpush('kh', 'ks', 'kl'),
        [PIC_IN, (2, 'out', 'f', 'w'), TABLE])
def ck_mask(S):
    # the mask byte of every pixel (a float holding a whole number); the table: val[256] then inv[256] as doubles
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        px = S.buf('in0').load(i)
        tab = S.buf('table')
        a = to_level(S, px.w)
        br, bg, bb = to_level(S, px.x), to_level(S, px.y), to_level(S, px.z)
        def val(v): return ld_sf(S, tab, S.to_u(v) * 2)
        invA = ld_sf(S, tab, S.u(512) + S.to_u(a) * 2)
        mid = (a >= 1) & (a <= 254)
        def chan(v):
            return SF.sel(S, mid, invA * val(v), val(v))
        r, g, b = chan(br), chan(bg), chan(bb)
        h, sat, l = ck_hsl(S, r, g, b)
        dh = (h - pd(P, 'kh')).abs().to_f32()
        ds = (sat - pd(P, 'ks')).abs().to_f32()
        dl = (l - pd(P, 'kl')).abs().to_f32()
        dh = S.select(dh > 0.5, 1.0 - dh, dh)
        dg = P.kgf - g.to_f32()
        dr = P.krf - r.to_f32()
        db = P.kbf - b.to_f32()
        dist = fsqrt(S, (dr * dr + dg * dg) + db * db)
        hs = dh + dh
        hs = hs + ds
        hs = hs + dl
        hs = fdiv(S, hs, S.f(3.0))
        d = dist * S.f(1.7320508) + hs
        d = d * 0.5
        v = (d - P.lowf) * P.invr
        v = v * 255.0
        v = S.select(S.f(0.0) > v, S.f(0.0), S.min(S.f(255.0), v))
        m = S.select(P.lowf > d, S.i(0), S.select(d > P.hif, S.i(255), S.to_i(v)))
        S.buf('out').store(i, S.to_f(S.min(m, a)))

@kernel("ck_line", WH + [('vert', 'u'), ('cut', 'u')], [(0, 'in0', 'f', 'r'), (1, 'st', 'f', 'rw'), (2, 'out', 'f', 'w'), TABLE], local=(64, 1))
def ck_line(S):
    """keyerLine() for one row (or column, bottom row first, with vert) per invocation; table: A[5] B[5] C[5] D[5] E[5] (floats). cut: the result is cut to a whole number"""
    P = S.p
    vert = S.ne(P.vert, 0)
    n = S.select(vert, P.h, P.w)
    lines = S.select(vert, P.w, P.h)
    with S.if_(S.gx < lines):
        line = S.gx
        tab = S.buf('table')
        st = S.buf('st')
        src = S.buf('in0')
        out = S.buf('out')
        def idx(q): return S.select(vert, q * P.w + line, line * P.w + q)
        def pos(t): return S.select(vert, n - 1 - t, t)
        def co(k, j): return tab.load(S.u(k * 5 + j))      # k: 0 A, 1 B, 2 C, 3 D, 4 E
        xL = src.load(idx(pos(S.u(0))))
        xR = src.load(idx(pos(n - 1)))
        zero = S.f(0.0)
        lim = SF.const(S, 1e-17)
        def tiny(a):
            f = SF.from_f32(S, a)
            return S.select((f > lim) | ((-f) > lim), a, zero)
        xh = [S.var('f', zero) for _ in range(5)]     # in[i - j], j >= 1 (index j)
        uh = [S.var('f', zero) for _ in range(5)]     # in[L - 1 - (i - j)]
        yh = [S.var('f', zero) for _ in range(5)]
        bh = [S.var('f', zero) for _ in range(5)]
        with S.for_(0, n) as i:
            x0 = src.load(idx(pos(i)))
            u0 = src.load(idx(pos(n - 1 - i)))
            xs = [x0] + [xh[j].get() for j in range(1, 5)]
            us = [u0] + [uh[j].get() for j in range(1, 5)]
            ys = [zero] + [yh[j].get() for j in range(1, 5)]
            bs = [zero] + [bh[j].get() for j in range(1, 5)]
            acc, accb = zero, zero
            for j in range(5):
                inside = S.u(j) < i
                acc = acc + S.select(inside, xs[j] * co(0, j) - co(1, j) * ys[j], (co(0, j) - co(3, j)) * xL)
                accb = accb + S.select(inside, us[j] * co(2, j) - co(1, j) * bs[j], (co(2, j) - co(4, j)) * xR)
            acc = tiny(acc)
            accb = tiny(accb)
            pix = idx(pos(i))
            st.store(pix * 2, acc)
            st.store(pix * 2 + 1, accb)
            for j in range(4, 1, -1):
                xh[j].set(xh[j - 1].get()); uh[j].set(uh[j - 1].get()); yh[j].set(yh[j - 1].get()); bh[j].set(bh[j - 1].get())
            xh[2].set(xh[1].get()); uh[2].set(uh[1].get()); yh[2].set(yh[1].get()); bh[2].set(bh[1].get())
            xh[1].set(x0); uh[1].set(u0); yh[1].set(acc); bh[1].set(accb)
        with S.for_(0, n) as i:
            v = st.load(idx(pos(n - 1 - i)) * 2 + 1) + st.load(idx(pos(i)) * 2)
            o = S.select(zero > v, zero, S.min(S.f(255.0), v))
            o = S.select(S.ne(P.cut, 0), S.to_f(S.to_i(o)), o)
            out.store(idx(pos(i)), o)

@kernel("ck_mix", WH + [('maskonly', 'u'), ('despill', 'u')], [PIC_IN, (1, 'in1', 'f', 'r'), PIC_OUT])
def ck_mix(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        a8 = S.to_i(S.buf('in1').load(i))
        p = [to_level(S, s.x), to_level(S, s.y), to_level(S, s.z)]
        p2 = S.select((a8 >= 1) & S.eq(P.despill, 1), S.min(p[2], p[1]), p[2])
        p1 = S.select((a8 >= 1) & S.eq(P.despill, 2), S.min(p[1], p[0]), p[1])
        p = [p[0], p1, p2]
        f = SF.from_i32(S, a8) * 0.00392156862745098
        o = [from_level(S, (SF.from_i32(S, p[k]) * f).trunc_i32()) for k in range(3)]
        av = from_level(S, a8)
        keyed = S.vec4(o[0], o[1], o[2], av)
        S.buf('out').store(i, S.select(S.ne(P.maskonly, 0), S.vec4(av, av, av, av), keyed))

# ---------------------------------------------------------------------------------------------------------- Glow
@kernel("glow_apply", WH + [('supp', 'f'), ('inten', 'f'), ('lv', 'u'), ('c0', 'i'), ('c1', 'i'), ('c2', 'i'), ('c3', 'i'), ('f0', 'f'), ('f1', 'f'), ('f2', 'f'), ('f3', 'f')],
        [PIC_IN, PIC_IN1, PIC_OUT])
def glow_apply(S):
    # glow() in builtins_filters.cpp: in0 the picture, in1 its blur (8-bit: floats of 0..255)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        m = S.buf('in1').load(i)
        f = m.x + m.y + m.z
        f = fdiv(S, f, S.f(3.0))
        res = S.var('v4', S.splat4(0.0))
        with S.if_(S.ne(P.lv, 0)):
            f8 = fdiv(S, f, S.f(255.0))
            f8 = (f8 - P.supp) * P.inten
            f8 = S.select(f8 < 0.0, S.f(0.0), S.min(S.f(1.0), f8))
            f8 = f8 * 255.0
            f8 = S.select(f8 < 0.0, S.f(0.0), S.min(S.f(255.0), f8))
            w = S.to_i(f8)
            cb = [P.c0, P.c1, P.c2, P.c3]
            out = []
            for k in range(4):
                d = to_level(S, s[k])
                t = (cb[k] - d) * w
                t = t + S.select(t < 0, S.i(255), S.i(0))
                out.append(from_level(S, (d + (t >> 8)) & 255))
            res.set(S.vec4(out[0], out[1], out[2], out[3]))
        with S.else_():
            g = (f - P.supp) * P.inten
            w = S.select(g < 0.0, S.f(0.0), S.min(S.f(1.0), g))
            cf = [P.f0, P.f1, P.f2, P.f3]
            res.set(S.vec4(*[(cf[k] - s[k]) * w + s[k] for k in range(4)]))
        S.buf('out').store(i, res.get())

# ---------------------------------------------------------------------------------------------------------- Vignette
@kernel("vignette", WH + [('invert', 'u'), ('lv', 'u'), ('cxp', 'f'), ('cyp', 'f'), ('cs', 'f'), ('sn', 'f'), ('g', 'f'), ('expo', 'f'), ('rin', 'f'), ('ramp', 'f'), ('ca', 'f'),
                          ('cb0', 'i'), ('cb1', 'i'), ('cb2', 'i'), ('cf0', 'f'), ('cf1', 'f'), ('cf2', 'f')], [PIC_IN, PIC_OUT])
def vignette(S):
    # vignette() in builtins_more_creative.cpp (single precision, VEGAS's order; the power is the double one rounded to a float)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        v = S.to_f(P.h - 1 - S.gy) - P.cyp
        v14 = v * P.cs
        v15 = -(v * P.sn)
        u = S.to_f(S.gx) - P.cxp
        xr = u * P.sn + v14
        yr = u * P.cs + v15
        xr = xr * P.g
        yr = fdiv(S, yr, P.g)
        d = fsqrt(S, xr * xr + yr * yr)
        t = fdiv(S, d - P.rin, P.ramp)
        t = S.select(t < 0.0, S.f(0.0), S.min(S.f(1.0), t))
        pw = fpow(S, t, P.expo)
        m = S.select(d > P.rin, S.select(pw < 0.0, S.f(0.0), S.min(S.f(1.0), pw)), S.f(0.0))
        m = S.select(S.ne(P.invert, 0), 1.0 - m, m)
        w = P.ca * m
        iw = 1.0 - w
        s = S.buf('in0').load(i)
        res = S.var('v4', S.splat4(0.0))
        with S.if_(S.ne(P.lv, 0)):
            cb = [P.cb0, P.cb1, P.cb2, S.i(0)]
            out = []
            for k in range(4):
                sv = to_level(S, s[k])
                cv = sv if k == 3 else cb[k]
                r = S.to_i(S.to_f(sv) * iw + S.to_f(cv) * w + 0.5)
                out.append(from_level(S, S.min(S.max(r, 0), 255)))
            res.set(S.vec4(out[0], out[1], out[2], out[3]))
        with S.else_():
            cf = [P.cf0, P.cf1, P.cf2]
            res.set(S.vec4(*[s[k] * iw + cf[k] * w for k in range(3)], s.w))
        S.buf('out').store(i, res.get())

# ---------------------------------------------------------------------------------------------------------- HSL Adjust, 8-bit
@kernel("hsladjust8", WH + dpush('hue', 'sat', 'lum'), [PIC_IN, PIC_OUT])
def hsladjust8(S):
    # hslAdjust() with c.levels in builtins_filters.cpp: VEGAS's byte HSL routines in doubles (the odd last pixel of a row takes the scalar conversion)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        k = 0.00392156862745098
        zero, one = SF.const(S, 0.0), SF.const(S, 1.0)
        bb, gb, rb, ab = to_level(S, s.z), to_level(S, s.y), to_level(S, s.x), to_level(S, s.w)
        b, g, r, a = [SF.from_i32(S, v) * k for v in (bb, gb, rb, ab)]
        h, sat, L = ck_hsl(S, r, g, b)
        h = h + pd(P, 'hue')
        h = SF.sel(S, h > one, h - 1.0, h)
        ns = sat * pd(P, 'sat')
        sat = SF.sel(S, zero > ns, zero, SF.sel(S, ns > one, one, ns))
        nl = L * pd(P, 'lum')
        L = SF.sel(S, zero > nl, zero, SF.sel(S, nl > one, one, nl))
        c255 = SF.const(S, 255.0)
        def itrunc(x): return x.trunc_i32()
        # ---- the pair conversion
        LS = L * sat
        m2p = SF.sel(S, L <= 0.5, LS + L, (sat - LS) + L)
        m1p = (L + L) - m2p
        h6 = h * 6.0
        h6 = SF.sel(S, h6.eq(6.0), zero, h6)
        sector = itrunc(h6)
        f = h6 - SF.from_i32(S, sector)
        v = f * (m2p - m1p)
        q = m2p - v
        t = v + m1p
        M, Pp, T, Q = itrunc(m2p * c255), itrunc(m1p * c255), itrunc(t * c255), itrunc(q * c255)
        even = S.eq(sector & 1, 0)
        X9 = S.select(even, M, Q)
        X7 = S.select(even, T, M)
        s2 = sector >> 1
        Gp = S.select(S.eq(s2, 0), X7, S.select(S.eq(s2, 1), X9, Pp))
        Rp = S.select(S.eq(s2, 0), X9, S.select(S.eq(s2, 1), Pp, X7))
        Bp = S.select(S.eq(s2, 0), Pp, S.select(S.eq(s2, 1), X7, X9))
        pos = ~(zero < m2p)
        pair = [S.select(pos, S.i(0), Bp) & 255, S.select(pos, S.i(0), Gp) & 255, S.select(pos, S.i(0), Rp) & 255]
        alp = itrunc(a * c255)
        # ---- the scalar one
        m2s = SF.sel(S, L <= 0.5, (sat + 1.0) * L, (sat + L) - sat * L)
        als = S.min(S.max(alp, 0), 255)
        m1s = (L + L) - m2s
        h6s = h * 6.0
        secs = itrunc(h6s)
        fs = h6s - SF.from_i32(S, secs)
        vs = (((m2s - m1s) / m2s) * m2s) * fs
        qs = m2s - vs
        ts = vs + m1s
        def pick(cases):
            # cases: values for sector 1..5, default for 0 and 6
            out = cases[5]
            for sct in range(1, 6):
                out = SF.sel(S, S.eq(secs, sct), cases[sct - 1], out)
            return out
        rs = pick([qs, m1s, m1s, ts, m2s, m2s])
        gs = pick([m2s, m2s, qs, m1s, m1s, ts])
        bs = pick([m1s, ts, m2s, m2s, qs, m1s])
        poss = ~(zero < m2s)
        scal = [S.select(poss, S.i(0), itrunc(bs * c255)) & 255, S.select(poss, S.i(0), itrunc(gs * c255)) & 255, S.select(poss, S.i(0), itrunc(rs * c255)) & 255]
        tail = S.ne(P.w & 1, 0) & S.eq(S.gx, P.w - 1)
        o = [S.select(tail, scal[kk], pair[kk]) for kk in range(3)]
        alb = S.select(tail, als, alp & 255)
        o = [S.select(o[kk] >= alb, alb, o[kk]) for kk in range(3)]
        S.buf('out').store(i, S.vec4(from_level(S, o[2]), from_level(S, o[1]), from_level(S, o[0]), from_level(S, alb)))

# ---------------------------------------------------------------------------------------------------------- Picture in Picture
@kernel("pip", WH + [('mode', 'u'), ('lv', 'u'), ('opf', 'f'), ('pad', 'u')], [PIC_IN, PIC_OUT, TABLE])
def pip(S):
    """pictureInPicture() in builtins_more_creative.cpp: the source position in doubles, a bilinear fetch in floats, the 8-bit rounding and the opacity.
    The table holds the doubles: hm[9], sx, sy, cs, sn, shear, lxW, lyH, axW, ayH (pipSetup())."""
    P = S.p
    with S.if_(S.gx < P.w):
      with S.if_(S.gy < P.h):
        tab = S.buf('table')
        def T(i): return ld_sf(S, tab, i * 2)
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        x = SF.from_i32(S, S.to_i(S.gx))
        j = SF.from_i32(S, hi - 1 - S.to_i(S.gy))
        Wd, Hd = SF.from_i32(S, wi), SF.from_i32(S, hi)
        px = SF.const(S, 0.0)
        pj = SF.const(S, 0.0)
        if True:
            X, Y = x / Wd, j / Hd
            w = T(6) * X + T(7) * Y + T(8)
            den = SF.sel(S, w.abs() < 1e-12, SF.const(S, 1e-12), w)
            u = (T(0) * X + T(1) * Y + T(2)) / den
            v = (T(3) * X + T(4) * Y + T(5)) / den
            pxa, pja = u * Wd, v * Hd
            uu = x - T(14)
            vv = j - T(15)
            ru = uu * T(11) - vv * T(12)
            rv = uu * T(12) + vv * T(11)
            ru = (ru - (T(13) * rv) * 0.5) / T(9)
            rv = rv / T(10)
            pxb, pjb = ru + T(16), rv + T(17)
            m2 = S.eq(P.mode, 2)
            px = SF.sel(S, m2, pxa, pxb)
            pj = SF.sel(S, m2, pja, pjb)
        py = (Hd - 1.0) - pj
        eps = 0.397
        ok = (px >= -eps) & (px <= (Wd - 1.0) + eps) & (py >= -eps) & (py <= (Hd - 1.0) + eps)
        res = S.var('v4', S.splat4(0.0))
        with S.if_(ok):
            cx = sf_clamp(S, px, SF.const(S, 0.0), Wd - 1.0)
            cy = sf_clamp(S, py, SF.const(S, 0.0), Hd - 1.0)
            x0 = S.min(S.max(cx.trunc_i32(), 0), wi - 1)
            y0 = S.min(S.max(cy.trunc_i32(), 0), hi - 1)
            x1, y1 = S.min(x0 + 1, wi - 1), S.min(y0 + 1, hi - 1)
            fu = (cx - SF.from_i32(S, x0)).to_f32()
            fv = (cy - SF.from_i32(S, y0)).to_f32()
            inb = S.buf('in0')
            a = inb.load(S.to_u(y0) * P.w + S.to_u(x0))
            b = inb.load(S.to_u(y0) * P.w + S.to_u(x1))
            cc = inb.load(S.to_u(y1) * P.w + S.to_u(x0))
            d = inb.load(S.to_u(y1) * P.w + S.to_u(x1))
            ex = (SF.abs(px - (px + 0.5).floor()) < 1e-7) & (SF.abs(py - (py + 0.5).floor()) < 1e-7)
            ex = SF.abs(px - (px + 0.5).floor()) < 1e-7
            ex = ex & (SF.abs(py - (py + 0.5).floor()) < 1e-7)
            lv = S.ne(P.lv, 0)
            out = []
            for k in range(4):
                v = ((a[k] * (1.0 - fu) + b[k] * fu) * (1.0 - fv) + (cc[k] * (1.0 - fu) + d[k] * fu) * fv)
                t = v * 255.0 - 0.001
                r8 = S.max(0.0, (0.0 - S.floor(0.0 - t)) - 1.0)
                r8 = fdiv(S, r8, S.f(255.0))
                v8 = S.select(ex, v, r8)
                m = v8 * P.opf
                vc = S.select(m >= 0.0, S.min(m, 2.0), S.f(0.0))
                q = fdiv(S, S.floor(vc * 255.0 + 0.001), S.f(255.0))
                out.append(S.select(lv, q, v * P.opf))
            res.set(S.vec4(out[0], out[1], out[2], out[3]))
        S.buf('out').store(S.gy * P.w + S.gx, res.get())

@kernel("pip8", WH + [('m%d' % i, 'f') for i in range(9)] + [('opf', 'f')], [PIC_IN, PIC_OUT])
def pip8(S):
    """the 8-bit branch of pictureInPicture() (VEGAS's fixed-point bilinear render): the position in floats, the weights in 1/1024, the sum shifted down 10"""
    P = S.p
    with S.if_(S.gx < P.w):
      with S.if_(S.gy < P.h):
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        jj = hi - 1 - S.to_i(S.gy)
        yf = S.to_f(jj)
        xf = S.to_f(S.to_i(S.gx))
        a9 = yf * P.m1 + P.m2
        a7 = yf * P.m4 + P.m5
        a5 = yf * P.m7 + P.m8
        inv = fdiv(S, S.f(1.0), xf * P.m6 + a5)
        u = (xf * P.m0 + a9) * inv
        v = (xf * P.m3 + a7) * inv
        wf, hf = S.to_f(wi), S.to_f(hi)
        ok = (u > -0.4) & ((wf - 0.6) > u) & (v > -0.4) & ((hf - 0.6) > v)
        res = S.var('v4', S.splat4(0.0))
        with S.if_(ok):
            ix, iy = S.to_i(u), S.to_i(v)
            fx = S.max(S.f(0.0), u - S.to_f(ix))
            fy = S.max(S.f(0.0), v - S.to_f(iy))
            omx = 1.0 - fx
            w1 = S.to_i(((1.0 - fy) * fx) * 1024.0)
            w2 = S.to_i((omx * fy) * 1024.0)
            w3 = S.to_i((fy * fx) * 1024.0)
            w0 = S.to_i((omx * (1.0 - fy)) * 1024.0)
            edgeX, edgeY = S.eq(ix, wi - 1), S.eq(iy, hi - 1)
            ix1 = S.select(edgeX, ix, ix + 1)
            iy1 = S.select(edgeY, iy, iy + 1)
            inb = S.buf('in0')
            def px(xx, yy): return inb.load(S.to_u(hi - 1 - yy) * P.w + S.to_u(xx))
            pa, pb, pc, pd_ = px(ix, iy), px(ix1, iy), px(ix, iy1), px(ix1, iy1)
            out = []
            for k in range(4):
                a = to_level(S, pa[k])
                b = S.select(edgeX, a, to_level(S, pb[k]))
                cc = S.select(edgeY, a, to_level(S, pc[k]))
                dd = S.select(edgeX | edgeY, cc, to_level(S, pd_[k]))
                sm = dd * w3 + cc * w2 + b * w1 + a * w0
                byte = (sm >> 10) & 255
                out.append(from_level(S, S.to_i(S.to_f(byte) * P.opf)))
            res.set(S.vec4(out[0], out[1], out[2], out[3]))
        S.buf('out').store(S.gy * P.w + S.gx, res.get())

# ---------------------------------------------------------------------------------------------------------- 8-bit Convolution Kernel, LAB Adjust (VEGAS's byte routines)
@kernel("conv8", WH + [('k%d' % q, 'f') for q in range(9)] + [('off', 'f'), ('alphaPlain', 'u')], [PIC_IN, PIC_OUT])
def conv8(S):
    """convolutionKernel8() in builtins_filters2.cpp: the 3 x 3 sums over the stored bytes in float, in the CPU's order"""
    P = S.p
    with S.in_range(P.w, P.h):
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        x, y = S.to_i(S.gx), S.to_i(S.gy)
        inb = S.buf('in0')
        wts = [P.k0, P.k1, P.k2, P.k3, P.k4, P.k5, P.k6, P.k7, P.k8]
        acc = [S.var('f', S.f(0.0)) for _ in range(4)]
        plain = S.ne(P.alphaPlain, 0)
        for j in range(3):
            ry = S.min(S.max(y + (1 - j), 0), hi - 1)
            for t in range(3):
                cx = S.min(S.max(x + (t - 1), 0), wi - 1)
                q = inb.load(S.to_u(ry) * P.w + S.to_u(cx))
                wt = wts[j * 3 + t]
                for k in range(3):
                    acc[k].set(acc[k].get() + S.to_f(to_level(S, q[k])) * wt)
                acc[3].set(S.select(plain, acc[3].get(), acc[3].get() + S.to_f(to_level(S, q[3])) * wt))
        qa = inb.load(S.to_u(y) * P.w + S.to_u(S.min(x + 1, wi - 1)))
        a3 = S.select(plain, S.to_f(to_level(S, qa[3])), acc[3].get())
        def clampv(v): return S.select(0.0 > v, S.f(0.0), S.min(S.f(255.0), v))
        a = clampv(a3)
        out = []
        for k in range(3):
            v = S.min(clampv(acc[k].get() + P.off), a)
            out.append(from_level(S, S.to_i(v)))
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(out[0], out[1], out[2], from_level(S, S.to_i(a))))

@kernel("labadjust8", WH + dpush('light', 'cha', 'chb', 'keep') + [('invert', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT, TABLE])
def labadjust8(S):
    """labAdjust8() in builtins_more_colour.cpp (VEGAS's byte routine) in doubles; the table: the linear value of each of the 256 levels"""
    from sf64 import sf_pow
    P = S.p
    with S.in_range(P.w, P.h):
        q = S.buf('in0').load(S.gy * P.w + S.gx)
        tab = S.buf('table')
        r8, g8, b8, a8 = to_level(S, q.x), to_level(S, q.y), to_level(S, q.z), to_level(S, q.w)
        def lin(l): return ld_sf(S, tab, S.to_u(l) * 2)
        rl, gl, bl = lin(r8), lin(g8), lin(b8)
        X = ((rl * 0.4124 + gl * 0.3576) + bl * 0.1805) / 0.95047
        Y = (gl * 0.7152 + rl * 0.2126) + bl * 0.0722
        Z = ((rl * 0.0193 + gl * 0.1192) + bl * 0.9505) / 1.08883
        third = SF.const(S, 0.3333333333333333)
        def fwd(t):
            big = t > 0.008856
            pw = sf_pow(S, SF.sel(S, big, t, SF.const(S, 1.0)), third)
            return SF.sel(S, big, pw, t * 7.7870001792907715 + 0.13793103448275862)
        fx, fy, fz = fwd(X), fwd(Y), fwd(Z)
        L = fy * 116.0 - 16.0
        A = (fx - fy) * 500.0
        B = (fy - fz) * 200.0
        L = SF.sel(S, S.ne(P.invert, 0), 95.5806655883789 - L, L) * pd(P, 'light')
        keep = pd(P, 'keep')
        A = (A + pd(P, 'cha')) * keep
        B = (B + pd(P, 'chb')) * keep
        gy = (L + 16.0) / 116.0
        gx = A / 500.0 + gy
        gz = gy - B / 200.0
        def inv(f):
            cube = (f * f) * f
            return SF.sel(S, cube > 0.008856, cube, (f - 0.13793103448275862) / 7.787)
        xr = inv(gx) * 0.950469970703125
        yr = inv(gy)
        zr = inv(gz) * 1.0888299942016602
        cr = (xr * 3.2406 - yr * 1.5372) - zr * 0.4986
        cg = (yr * 1.8758 - xr * 0.9689) + zr * 0.0415
        cb = (xr * 0.0557 - yr * 0.204) + zr * 1.057
        expo = SF.const(S, 0.4166666666666667)
        def back(v):
            big = v > 0.0031308
            pw = sf_pow(S, SF.sel(S, big, v, SF.const(S, 1.0)), expo)
            e = SF.sel(S, big, pw * 1.055 - 0.055, v * 12.92)
            e = SF.sel(S, e < 0.0, SF.const(S, 0.0), e)
            return SF.sel(S, e > 1.0, SF.const(S, 1.0), e)
        out = [from_level(S, S.min((back(v) * 255.0).trunc_i32(), a8)) for v in (cr, cg, cb)]
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(out[0], out[1], out[2], from_level(S, a8)))

@kernel("colorbalance8", WH + dpush('pr', 'pg', 'pb') + [('type', 'u'), ('keep', 'u')], [PIC_IN, PIC_OUT])
def colorbalance8(S):
    """colorBalance8() in builtins_filters.cpp: VEGAS's byte routine; groups of four pixels along a row take the SIMD code, the last (width mod 4) the scalar code"""
    import struct
    P = S.p
    with S.in_range(P.w, P.h):
        q = S.buf('in0').load(S.gy * P.w + S.gx)
        r, g, b, a = to_level(S, q.x), to_level(S, q.y), to_level(S, q.z), to_level(S, q.w)
        pr, pg, pb = pd(P, 'pr'), pd(P, 'pg'), pd(P, 'pb')
        inv765 = struct.unpack("<d", struct.pack("<Q", 0x3f556ac015c06a15))[0]
        keep = S.ne(P.keep, 0)
        def weight(L):
            y = 1.0 - L
            w0 = 1.0 - y * y
            x = L - 0.5
            w1 = 1.0 - (x * x) * 4.0
            w2 = 1.0 - L * L
            return SF.sel(S, S.eq(P.type, 2), w2, SF.sel(S, S.eq(P.type, 1), w1, w0))
        tot = r + g + b
        simd = S.gx < ((P.w >> 2) << 2)
        # SIMD
        ws = weight(SF.from_i32(S, tot) * inv765)
        tr, tg, tb = (ws * (pr * 255.0)).trunc_i32(), (ws * (pg * 255.0)).trunc_i32(), (ws * (pb * 255.0)).trunc_i32()
        hr, hg, hb = (ws * (pr * 127.5)).trunc_i32(), (ws * (pg * 127.5)).trunc_i32(), (ws * (pb * 127.5)).trunc_i32()
        sdr = S.select(keep, tr - hb - hg, tr)
        sdg = S.select(keep, tg - hb - hr, tg)
        sdb = S.select(keep, tb - hg - hr, tb)
        # scalar
        wc = weight(SF.from_u32(S, S.to_u(tot) / 3) / 255.0)
        ur, ug, ub = pr * wc * 255.0, pg * wc * 255.0, pb * wc * 255.0
        rr, rg, rb = sf_nearbyint(S, ur + 1.5e-8), sf_nearbyint(S, ug + 1.5e-8), sf_nearbyint(S, ub + 1.5e-8)
        qr, qg, qb = sf_nearbyint(S, ur * 0.5 + 1.5e-8), sf_nearbyint(S, ug * 0.5 + 1.5e-8), sf_nearbyint(S, ub * 0.5 + 1.5e-8)
        cdr = S.select(keep, rr - qb - qg, rr)
        cdg = S.select(keep, rg - qr - qb, rg)
        cdb = S.select(keep, rb - qr - qg, rb)
        def fin(v, d1, d2):
            sv = S.min(S.min(255, S.max(0, v + d1)), a)
            cv = S.min(255, S.max(0, v + d2))
            return S.select(simd, sv, cv)
        o = [fin(r, sdr, cdr), fin(g, sdg, cdg), fin(b, sdb, cdb)]
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(from_level(S, o[0]), from_level(S, o[1]), from_level(S, o[2]), from_level(S, a)))

# ---------------------------------------------------------------------------------------------------------- Border
# builtins_filters.cpp border(): Solid, Blurred (a 255 mask through three box blurs, whole-number sums) and Beveled (per-row spans, a table of 25 numbers a row)
def _chb(k): return 2 - k if k < 3 else 3

@kernel("border_solid", WH + [('xi', 'i'), ('xr', 'i'), ('yb', 'i'), ('yt', 'i'), ('c0', 'f'), ('c1', 'f'), ('c2', 'f'), ('c3', 'f')], [PIC_IN, PIC_OUT])
def border_solid(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        x = S.to_i(S.gx)
        y = S.to_i(P.h) - 1 - S.to_i(S.gy)
        inside = (x < P.xi) | (x > P.xr) | (y < P.yb) | (y > P.yt)
        s = S.buf('in0').load(i)
        d255 = S.f(255.0)
        cv = S.vec4(fdiv(S, P.c2, d255), fdiv(S, P.c1, d255), fdiv(S, P.c0, d255), fdiv(S, P.c3, d255))
        S.buf('out').store(i, S.select(inside, cv, s))

@kernel("border_mask", WH + [('xi', 'i'), ('xr', 'i'), ('yb', 'i'), ('yt', 'i')], [PIC_OUT])
def border_mask(S):
    P = S.p
    with S.in_range(P.w, P.h):
        x = S.to_i(S.gx)
        y = S.to_i(P.h) - 1 - S.to_i(S.gy)
        inside = (x < P.xi) | (x > P.xr) | (y < P.yb) | (y > P.yt)
        S.buf('out').store(S.gy * P.w + S.gx, S.splat4(0.0) + S.select(inside, S.f(255.0), S.f(0.0)))

@kernel("border_box", WH + [('vert', 'u'), ('r', 'u'), ('iarr', 'f'), ('pad', 'u')], [PIC_IN, PIC_OUT], local=(64, 1))
def border_box(S):
    """one pass of Kutskir's box blur (borderBoxH): a line of whole numbers, the window of 2r + 1 with the edge values repeated, rounded floor(sum * iarr + 0.5)"""
    P = S.p
    vert = S.ne(P.vert, 0)
    n = S.select(vert, P.h, P.w)
    lines = S.select(vert, P.w, P.h)
    with S.if_(S.gx < lines):
        inb, outb = S.buf('in0'), S.buf('out')
        def idx(k): return S.select(vert, k * P.w + S.gx, S.gx * P.w + k)
        def at(k): return inb.load(idx(k)).x
        r = P.r
        val = S.var('f', S.to_f(r + 1) * at(S.u(0)))
        with S.for_(0, r) as j:
            val.set(val.get() + at(S.min(j, n - 1)))
        with S.for_(0, n) as ti:
            ph1 = ti <= r
            ph2 = (~ph1) & (ti < n - r)
            ri = S.min(r + ti, n - 1)
            li = S.select(ti >= r + 1, ti - r - 1, S.u(0))
            qa = at(ri)
            d = S.select(ph1, qa - at(S.u(0)), S.select(ph2, qa - at(li), at(n - 1) - at(li)))
            val.set(val.get() + d)
            outb.store(idx(ti), S.splat4(0.0) + S.floor(val.get() * P.iarr + 0.5))

@kernel("border_apply", WH + [('q8', 'u'), ('pad', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def border_apply(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        m = S.buf('in1').load(i).x
        k = 1.0 - fdiv(S, m, S.f(255.0))
        q8 = S.ne(P.q8, 0)
        def one(v):
            t = S.min(S.max(v, 0.0), 1.0) * 255.0
            ti = S.to_i(t)
            r = S.to_f(ti + S.select((t - S.to_f(ti)) >= 0.5, S.i(1), S.i(0)))
            return S.select(q8, fdiv(S, S.to_f(S.to_i(r * k)), S.f(255.0)), v * k)
        S.buf('out').store(i, S.vec4(one(s.x), one(s.y), one(s.z), one(s.w)))

@kernel("border_bevel", WH + [('q8', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT, TABLE])
def border_bevel(S):
    """the Beveled type: per row (from the bottom) the table holds end[5], cov[5], face[5][3]; the first span whose end is past x is the pixel's"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        y = P.h - 1 - S.gy
        tab = S.buf('table')
        base = y * 25
        x = S.to_i(S.gx)
        kk = S.var('u', S.u(4))
        done = S.var('u', S.u(0))
        for k in range(5):
            e = S.to_i(tab.load(base + k))
            with S.if_(S.eq(done.get(), 0) & (x < e)):
                kk.set(S.u(k))
                done.set(S.u(1))
        k_ = kk.get()
        cov = S.to_i(tab.load(base + 5 + k_))
        s = S.buf('in0').load(i)
        q8 = S.ne(P.q8, 0)
        def v8(v):
            t = S.min(S.max(v, 0.0), 1.0) * 255.0
            ti = S.to_i(t)
            r = S.to_f(ti + S.select((t - S.to_f(ti)) >= 0.5, S.i(1), S.i(0)))
            return S.select(q8, r, v * 255.0)
        al = v8(s.w)
        outc = []
        for ch in range(3):
            kb = 2 - ch
            face = S.to_i(tab.load(base + 10 + k_ * 3 + kb))
            vv = v8([s.x, s.y, s.z][ch])
            vq = S.to_f((S.to_i(vv) * face) / 256)
            vf = fdiv(S, vv * S.to_f(face), S.f(256.0))
            v = S.select(q8, vq, vf)
            outc.append(fdiv(S, S.min(v, al), S.f(255.0)))
        res = S.vec4(outc[0], outc[1], outc[2], S.f(0.0))
        S.buf('out').store(i, S.select(S.eq(cov, 0), s, res))

# ---------------------------------------------------------------------------------------------------------- Broadcast Colors, 8-bit
BC_R, BC_G, BC_B, BC_Q0, BC_Q1, BC_TC, BC_TD, BC_TE = 8, 264, 520, 776, 1012, 1248, 1544, 1785

@kernel("broadcastcolors8", WH, [PIC_IN, PIC_OUT, TABLE])
def broadcastcolors8(S):
    """broadcastColors8() in builtins_more_colour.cpp (VEGAS's table pass), in doubles; the table holds the constants and tables as doubles (BC_* offsets)"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        sp = S.buf('in0').load(i)
        tab = S.buf('table')
        def T(off, k): return ld_sf(S, tab, S.to_u(off + k) * 2)
        Q, Q2, cmin, cmax, PA, PB, P2, P3 = [ld_sf(S, tab, S.u(2 * n)) for n in range(8)]
        R, G, B, A = to_level(S, sp.x), to_level(S, sp.y), to_level(S, sp.z), to_level(S, sp.w)
        Y = (T(BC_G, G) + T(BC_R, R)) + T(BC_B, B)
        x9, x3, x2 = SF.from_i32(S, R), SF.from_i32(S, B), SF.from_i32(S, G)
        x1 = x2 * 0.289
        x2 = x2 * 0.515
        x0 = x9 * 0.147
        x9 = x9 * 0.615
        x7 = SF.from_i32(S, B)
        x3 = x3 * 0.1
        x7 = x7 * 0.436
        x9 = x9 - x2
        x9 = x9 - x3
        x1 = x1 + x0
        x7 = x7 - x1
        x1 = (x9.abs() + x7.abs()) * Q
        idx = S.clamp(sf_nearbyint(S, Y), -16, 219)
        x6 = cmax - Y
        x4 = Y - cmin
        v0 = T(BC_Q0, S.to_u(idx + 16))
        unch = v0.eq(SF.const(S, -3454.0))
        skip1 = unch & (x6 > x1) & (x4 > x1) & (PA > x1)
        x4 = SF.sel(S, unch, x4, v0 - cmin)
        x6 = SF.sel(S, unch, x6, cmax - v0)
        changed = ~unch
        usetd = x6 > x4
        K = SF.sel(S, usetd, P2, P3)
        x6 = SF.sel(S, usetd, x4, x6)
        x8 = (x9 * x9 + x7 * x7) * Q2
        Ql = T(BC_Q1, S.to_u(idx + 16))
        x10 = (x6 * x6) * K
        # chroma limit
        big = x8 > PB
        sq = x8.sqrt()
        i2 = S.clamp(sf_nearbyint(S, (sq + sq) + 1.5e-8), 0, 295)
        f = T(BC_TC, S.to_u(i2))
        x7 = SF.sel(S, big, x7 * f, x7)
        x9 = SF.sel(S, big, x9 * f, x9)
        x8 = SF.sel(S, big, x8 * (f * f), x8)
        changed = changed | big
        # composite limit
        scl = x8 > x10
        s2 = x8.sqrt()
        x6b = x6 / s2
        gt = x6b > 0.8333333333333334
        tt = SF.const(S, 200.0) / x6b
        i3 = S.clamp(sf_nearbyint(S, tt - 0.499999985), 0, 240)
        tv = SF.sel(S, usetd, T(BC_TD, S.to_u(i3)), T(BC_TE, S.to_u(i3)))
        x6c = SF.sel(S, gt, tv, x6b)
        x7 = SF.sel(S, scl, x7 * x6c, x7)
        x9 = SF.sel(S, scl, x9 * x6c, x9)
        keep = skip1 | (~scl & ~changed)
        rr = (x9 * 1.14 + Ql) + 1.5e-8
        u394 = x7 * 0.394
        x9b = x9 * 0.581
        bb = (x7 * 2.032 + Ql) + 1.5e-8
        gg = ((Ql - u394) - x9b) + 1.5e-8
        def fin(v): return S.min(S.max(sf_nearbyint(S, v), 0), A)
        res = S.vec4(from_level(S, fin(rr)), from_level(S, fin(gg)), from_level(S, fin(bb)), sp.w)
        S.buf('out').store(i, S.select(keep, sp, res))

# ---------------------------------------------------------------------------------------------------------- Black Bar Fill
def bbf_pos(S, P, tab, off):
    """srcPos() of blackBarFill(): the source position (doubles) of the pixel (gx, gy); the table holds cx * W, cy * H, z at 'off'"""
    wi, hi = S.to_i(P.w), S.to_i(P.h)
    cxW, cyH, z = ld_sf(S, tab, S.u(off * 2)), ld_sf(S, tab, S.u(off * 2 + 2)), ld_sf(S, tab, S.u(off * 2 + 4))
    sx = (SF.from_i32(S, S.to_i(S.gx)) - cxW) / z + SF.from_i32(S, wi) * 0.5
    sj = (SF.from_i32(S, hi - 1 - S.to_i(S.gy)) - cyH) / z + SF.from_i32(S, hi) * 0.5
    sy = SF.from_i32(S, hi - 1) - sj
    return sx, sy

def bbf_inside(S, P, sx, sy):
    wi, hi = S.to_i(P.w), S.to_i(P.h)
    return (sx >= -0.397) & (sx <= SF.from_i32(S, wi - 1) + 0.397) & (sy >= -0.397) & (sy <= SF.from_i32(S, hi - 1) + 0.397)

def bbf_fetch(S, inb, P, sx, sy):
    """fetch() of blackBarFill(): bilinear on the index grid, edges clamped (floats in the CPU's order)"""
    wi, hi = S.to_i(P.w), S.to_i(P.h)
    cx = sf_clamp(S, sx, SF.const(S, 0.0), SF.from_i32(S, wi - 1))
    cy = sf_clamp(S, sy, SF.const(S, 0.0), SF.from_i32(S, hi - 1))
    x0 = S.min(cx.trunc_i32(), wi - 1)
    y0 = S.min(cy.trunc_i32(), hi - 1)
    x1, y1 = S.min(x0 + 1, wi - 1), S.min(y0 + 1, hi - 1)
    u = (cx - SF.from_i32(S, x0)).to_f32()
    v = (cy - SF.from_i32(S, y0)).to_f32()
    def at(xx, yy): return inb.load(S.to_u(yy) * P.w + S.to_u(xx))
    a, b, c, d = at(x0, y0), at(x1, y0), at(x0, y1), at(x1, y1)
    return (a * (1.0 - u) + b * u) * (1.0 - v) + (c * (1.0 - u) + d * u) * v

@kernel("bbf_bg", WH, [PIC_IN, PIC_OUT, TABLE])
def bbf_bg(S):
    P = S.p
    with S.in_range(P.w, P.h):
        sx, sy = bbf_pos(S, P, S.buf('table'), 0)
        res = S.var('v4', S.splat4(0.0))
        with S.if_(bbf_inside(S, P, sx, sy)):
            res.set(bbf_fetch(S, S.buf('in0'), P, sx, sy))
        S.buf('out').store(S.gy * P.w + S.gx, res.get())

@kernel("bbf_decim", WH + [('sw', 'u'), ('sh', 'u'), ('f', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def bbf_decim(S):
    P = S.p
    with S.in_range(P.w, P.h):
        x = S.min(P.sw - 1, S.gx * P.f + P.f / 2)
        y = S.min(P.sh - 1, S.gy * P.f + P.f / 2)
        S.buf('out').store(S.gy * P.w + S.gx, S.buf('in0').load(y * P.sw + x))

@kernel("fir_gauss", WH + [('vert', 'u'), ('r', 'u')], [PIC_IN, PIC_OUT, TABLE])
def fir_gauss(S):
    """one pass of the generic Gaussian (blurLine's sums): s = in[i] * k0, then s += (in[a] + in[b]) * k[k] for k = 1 .. r, edges clamped; the table is the taps"""
    P = S.p
    with S.in_range(P.w, P.h):
        inb, tab = S.buf('in0'), S.buf('table')
        vert = S.ne(P.vert, 0)
        n = S.to_i(S.select(vert, P.h, P.w))
        pos = S.to_i(S.select(vert, S.gy, S.gx))
        def at(k): 
            kk = S.to_u(k)
            return inb.load(S.select(vert, kk * P.w + S.gx, S.gy * P.w + kk))
        s = S.var('v4', at(pos) * tab.load(S.u(0)))
        with S.for_(1, P.r + 1) as k:
            ki = S.to_i(k)
            a = S.max(pos - ki, 0)
            b = S.min(pos + ki, n - 1)
            s.set(s.get() + (at(a) + at(b)) * tab.load(k))
        S.buf('out').store(S.gy * P.w + S.gx, s.get())

@kernel("bbf_up", WH + [('lw', 'u'), ('lh', 'u'), ('f', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def bbf_up(S):
    """upsampleBilinear() in builtins_fast.cpp"""
    P = S.p
    with S.in_range(P.w, P.h):
        inb = S.buf('in0')
        ff = S.to_f(P.f)
        lw, lh = S.to_i(P.lw), S.to_i(P.lh)
        def axis(g, ln):
            u = fdiv(S, S.to_f(S.to_i(g)) + 0.5, ff) - 0.5
            u = S.min(S.max(u, S.f(0.0)), S.to_f(ln - 1))
            i = S.to_i(u)
            i = S.select((i >= ln - 1) & (ln > 1), ln - 2, i)
            fr = S.select(ln > 1, u - S.to_f(i), S.f(0.0))
            return i, fr, S.min(ln - 1, i + 1)
        i, t, i1 = axis(S.gx, lw)
        j, fy, j1 = axis(S.gy, lh)
        def at(xx, yy): return inb.load(S.to_u(yy) * P.lw + S.to_u(xx))
        def lerp(a, b, tt): return a + (b - a) * tt
        top = lerp(at(i, j), at(i1, j), t)
        bot = lerp(at(i, j1), at(i1, j1), t)
        S.buf('out').store(S.gy * P.w + S.gx, lerp(top, bot, fy))

@kernel("bbf_fix", WH + [('blurred', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def bbf_fix(S):
    """the 8-bit darkening of the background (what VEGAS's resampling/blur does to levels)"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        o = S.buf('in0').load(i)
        blurred = S.ne(P.blurred, 0)
        def ceil_(x): return x.neg().floor().neg()
        c255 = S.f(255.0)
        res = []
        for ch in range(3):
            v = SF.from_f32(S, [o.x, o.y, o.z][ch]) * 255.0
            a = SF.sel(S, SF.const(S, 0.0) < (v * (254.0 / 255.0) - 0.5 + 1e-4).floor(), (v * (254.0 / 255.0) - 0.5 + 1e-4).floor(), SF.const(S, 0.0))
            b = (ceil_(v - 1e-3) * (127.0 / 128.0) + 1e-6).floor()
            r = SF.sel(S, blurred, a, b).to_f32()
            res.append(fdiv(S, r, c255))
        a3 = SF.from_f32(S, o.w)
        t = ceil_(a3 * 255.0 - 1e-3) - SF.sel(S, a3 > 0.0, SF.const(S, 1.0), SF.const(S, 0.0))
        t = SF.sel(S, SF.const(S, 0.0) < t, t, SF.const(S, 0.0))
        res.append(fdiv(S, t.to_f32(), c255))
        S.buf('out').store(i, S.vec4(res[0], res[1], res[2], res[3]))

@kernel("bbf_mask", WH + [('sw', 'u'), ('sh', 'u'), ('f', 'u'), ('pad', 'u')], [PIC_OUT, TABLE])
def bbf_mask(S):
    """the foreground mask on the (reduced) grid: 1 where the position of the sampled pixel lies inside the frame"""
    P = S.p
    with S.in_range(P.w, P.h):
        # the position of the pixel the reduced grid samples
        tab = S.buf('table')
        px = S.min(P.sw - 1, S.gx * P.f + P.f / 2)
        py = S.min(P.sh - 1, S.gy * P.f + P.f / 2)
        wi, hi = S.to_i(P.sw), S.to_i(P.sh)
        cxW, cyH, z = ld_sf(S, tab, S.u(0)), ld_sf(S, tab, S.u(2)), ld_sf(S, tab, S.u(4))
        sx = (SF.from_i32(S, S.to_i(px)) - cxW) / z + SF.from_i32(S, wi) * 0.5
        sj = (SF.from_i32(S, hi - 1 - S.to_i(py)) - cyH) / z + SF.from_i32(S, hi) * 0.5
        sy = SF.from_i32(S, hi - 1) - sj
        ins = (sx >= -0.397) & (sx <= SF.from_i32(S, wi - 1) + 0.397) & (sy >= -0.397) & (sy <= SF.from_i32(S, hi - 1) + 0.397)
        S.buf('out').store(S.gy * P.w + S.gx, S.splat4(0.0) + S.select(ins, S.f(1.0), S.f(0.0)))

@kernel("bbf_shade", WH + [('lv', 'u'), ('pad', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def bbf_shade(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        o = S.buf('in0').load(i)
        k = S.min(S.max(S.buf('in1').load(i).x, 0.0), 1.0)
        lv = S.ne(P.lv, 0)
        out = []
        for v in (o.x, o.y, o.z):
            t = v * (1.0 - k)
            out.append(S.select(lv, fdiv(S, S.floor(t * 255.0 + 0.001), S.f(255.0)), t))
        S.buf('out').store(i, S.vec4(out[0], out[1], out[2], o.w))

@kernel("bbf_final", WH + [('lv', 'u'), ('fgWhole', 'u')], [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def bbf_final(S):
    """in0: the picture; in1: the background (shaded); the foreground over it"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        o = S.buf('in1').load(i)
        sx, sy = bbf_pos(S, P, S.buf('table'), 0)
        ins = bbf_inside(S, P, sx, sy)
        res = S.var('v4', o)
        with S.if_(ins):
            p = bbf_fetch(S, S.buf('in0'), P, sx, sy)
            fix = S.ne(P.lv, 0) & S.eq(P.fgWhole, 0)
            def ceil_(x): return x.neg().floor().neg()
            q = []
            for v in (p.x, p.y, p.z):
                r = (ceil_(SF.from_f32(S, v) * 255.0 - 1e-3) * (127.0 / 128.0) + 1e-6).floor().to_f32()
                q.append(S.select(fix, fdiv(S, r, S.f(255.0)), v))
            pa = p.w
            om = 1.0 - pa
            res.set(S.vec4(q[0] + o.x * om, q[1] + o.y * om, q[2] + o.z * om, pa + o.w * om))
        S.buf('out').store(i, res.get())

# ---------------------------------------------------------------------------------------------------------- Newsprint
# builtins_newsprint.cpp (VEGAS's own halftone, 8 bits): the picture to bytes, a box shrink (14-bit weights), then every pixel adds up the dots that reach it
def nint_f(S, x):
    """the float x to the nearest whole number, halves to even (std::nearbyint); an int"""
    fl = S.floor(x)
    d = x - fl
    fi = S.to_i(fl)
    up = (d > 0.5) | (S.eq(d, S.f(0.5)) & S.ne(fi & 1, 0))
    return fi + S.select(up, S.i(1), S.i(0))

@kernel("news_bytes", WH, [PIC_IN, PIC_OUT])
def news_bytes(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        def q(v): return S.to_f(nint_f(S, S.min(S.max(v, 0.0), 1.0) * 255.0))
        S.buf('out').store(i, S.vec4(q(p.z), q(p.y), q(p.x), q(p.w)))      # B G R A

@kernel("news_shrink", WH + [('pw', 'u'), ('ph', 'u'), ('nx', 'u'), ('ny', 'u')], [PIC_IN, PIC_OUT, TABLE])
def news_shrink(S):
    """boxShrink() (VEGAS's large-scale bilinear shrink): this is cell (gx, gy) of the small picture; the table holds the norm (a double), then the columns' and rows' (S, E, F, L)"""
    P = S.p
    with S.in_range(P.w, P.h):
        inb, tab = S.buf('in0'), S.buf('table')
        pw, ph = S.to_i(P.pw), S.to_i(P.ph)
        outs = [S.var('f', S.f(0.0)) for _ in range(4)]
        with S.if_((S.gx < P.nx) & (S.gy < P.ny)):
            def cell(base, n):
                return [S.to_i(tab.load(base + n * 4 + k)) for k in range(4)]
            ixS, ixE, wxF, wxL = cell(S.u(2), S.gx)
            iyS, iyE, wyF, wyL = cell(S.u(2) + P.nx * 4, S.gy)
            ncols, nrows = ixE - ixS, iyE - iyS
            acc = [SFVar(S, sf_zero(S)) for _ in range(4)]
            def add(x, y, w):
                ins = (x >= 0) & (x < pw) & (y >= 0) & (y < ph)
                q = inb.load(S.to_u(S.clamp(y, 0, ph - 1)) * P.pw + S.to_u(S.clamp(x, 0, pw - 1)))
                for k in range(4):
                    acc[k].set(acc[k].get() + SF.from_i32(S, S.select(ins, S.to_i(q[k]), S.i(0)) * w))
            def wt(a, b): return (SF.from_i32(S, a) * SF.from_i32(S, b) * 6.103515625e-05).trunc_i32()
            with S.for_(iyS + 1, iyE - 1, 1, True) as yy:
                with S.for_(ixS + 1, ixE - 1, 1, True) as xx:
                    add(xx, yy, S.i(16384))
            add(ixS, iyS, wt(wyF, wxF))
            with S.if_(ncols > 1):
                add(ixE - 1, iyS, wt(wxL, wyF))
            with S.if_(nrows > 1):
                add(ixS, iyE - 1, wt(wyL, wxF))
                with S.if_(ncols > 1):
                    add(ixE - 1, iyE - 1, wt(wxL, wyL))
            with S.for_(ixS + 1, ixE - 1, 1, True) as xx:
                add(xx, iyS, wyF)
                with S.if_(nrows > 1):
                    add(xx, iyE - 1, wyL)
            with S.for_(iyS + 1, iyE - 1, 1, True) as yy:
                add(ixS, yy, wxF)
                with S.if_(ncols > 1):
                    add(ixE - 1, yy, wxL)
            norm = ld_sf(S, tab, S.u(0))
            for k in range(4):
                outs[k].set(S.to_f(sf_nearbyint(S, acc[k].get() * norm + 1.5e-8) & 255))
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(outs[0].get(), outs[1].get(), outs[2].get(), outs[3].get()))

def _news_dots(mode):
    @kernel("news_dots%d" % mode, WH + [('shw', 'u'), ('shh', 'u'), ('nbx', 'i'), ('nby', 'i'), ('par', 'i'), ('maxx', 'i'), ('maxy', 'i'), ('fade', 'i')], [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
    def news_dots(S):
        P = S.p
        with S.in_range(P.w, P.h):
            tab = S.buf('table')
            sb = S.buf('in1')
            Pd, fxo, fyo = ld_sf(S, tab, S.u(0)), ld_sf(S, tab, S.u(2)), ld_sf(S, tab, S.u(4))
            wi, hi = S.to_i(P.w), S.to_i(P.h)
            swi, shi = S.to_i(P.shw), S.to_i(P.shh)
            xd, yd = SF.from_i32(S, S.to_i(S.gx)), SF.from_i32(S, S.to_i(S.gy))
            sub = [S.var('i', S.i(0)) for _ in range(3)]
            def tabcov(k): return S.to_i(tab.load(S.to_u(k) + 8))
            def sampleS(sx, sy):
                x, fx = sx >> 14, sx & 0x3fff
                y, fy = sy >> 14, sy & 0x3fff
                def at(xx, yy): return sb.load(S.to_u(yy) * P.shw + S.to_u(xx))
                inner = (x + 1 < swi) & (y + 1 < shi)
                xx_, yy_ = S.min(x, swi - 1), S.min(y, shi - 1)
                p = at(xx_, yy_)
                q10 = at(S.min(xx_ + 1, swi - 1), yy_)       # right
                q01 = at(xx_, S.min(yy_ + 1, shi - 1))       # below
                p11 = at(S.min(xx_ + 1, swi - 1), S.min(yy_ + 1, shi - 1))
                w00 = S.to_u(((16384 - fy) * (16384 - fx)) >> 4)
                w01 = S.to_u(((16384 - fy) * fx) >> 4)
                w10 = S.to_u(((16384 - fx) * fy) >> 4)
                w11 = S.to_u((fx * fy) >> 4)
                bothEdge = (y + 1 >= shi) & (x + 1 >= swi)
                rowEdge = (y + 1 >= shi)
                out = []
                for k in range(4):
                    pk, rk, bk, dk = S.to_u(p[k]), S.to_u(q10[k]), S.to_u(q01[k]), S.to_u(p11[k])
                    vi = (pk * w00 + rk * w01 + bk * w10 + dk * w11 + S.u(0x800000)) >> 24
                    vh = (pk * S.to_u(16384 - fx) + rk * S.to_u(fx) + S.u(0x2000)) >> 14
                    vv = (pk * S.to_u(16384 - fy) + bk * S.to_u(fy) + S.u(0x2000)) >> 14
                    out.append(S.to_i(S.select(inner, vi, S.select(bothEdge, pk, S.select(rowEdge, vh, vv)))))
                return out
            def ellipse(X, Y, rx, ry, cols):
                y0 = S.min(S.max(sf_nearbyint(S, Y - ry - 0.499999985), 0), hi - 1)
                x0 = S.min(S.max(sf_nearbyint(S, X - rx - 0.499999985), 0), wi - 1)
                y1 = S.min(S.max(sf_nearbyint(S, Y + ry + 0.499999985), 0), hi)
                x1 = S.min(S.max(sf_nearbyint(S, X + rx + 0.499999985), 0), wi)
                gx_, gy_ = S.to_i(S.gx), S.to_i(S.gy)
                ok = (y1 - y0 >= 1) & (x1 - x0 >= 1) & (gy_ >= y0) & (gy_ < y1) & (gx_ >= x0) & (gx_ < x1)
                with S.if_(ok):
                    k = ry / rx
                    in0 = SF.sel(S, SF.const(S, 0.0) < ry - 0.7, ry - 0.7, SF.const(S, 0.0))
                    inn = in0 * in0
                    ddy = ((yd + 0.0) - Y) + 0.5
                    dy = ddy * ddy
                    dx = ((xd + 0.5) - X) * k
                    d2 = dx * dx + dy
                    insd = inn > d2
                    dd = d2.sqrt()
                    far = dd > ry + 0.5
                    near = dd < ry - 0.5
                    idx = (sf_nearbyint(S, (dd - ry) * 255.0) + 128) & 255
                    cov = S.select(far, S.i(0), S.select(near, S.i(255), tabcov(idx)))
                    for ch in range(3):
                        col = cols[ch]
                        if col is None: continue
                        amt_in = S.i(0) - (((col - 255) * 255) / 256)
                        amt_ed = ((255 - col) * cov) / 256
                        sub[ch].set(sub[ch].get() + S.select(insd, amt_in, amt_ed))
            ic_y = (yd / Pd - fyo).floor_i32()
            ic_x = (xd / Pd - fxo).floor_i32()
            with S.for_(ic_y - 2, ic_y + 3, 1, True) as j:
              with S.if_((j >= -1) & (j < P.nby + 3)):
                yy = SF.from_i32(S, j) + 0.0 + fyo
                sy = (yy * 16384.0).trunc_i32()
                sy = S.select(sy < 0, S.i(0), S.min(sy, P.maxy))
                odd = S.ne((j + P.par) & 1, 0)
                with S.for_(ic_x - 3, ic_x + 4, 1, True) as i:
                  with S.if_((i >= -1) & (i < P.nbx + 3)):
                    xx = SF.sel(S, odd, SF.from_i32(S, i) + 0.0 + 0.5, SF.from_i32(S, i) + 0.0 + 0.0) + fxo
                    sx = (xx * 16384.0).trunc_i32()
                    sx = S.select(sx < 0, S.i(0), S.min(sx, P.maxx))
                    cc = sampleS(sx, sy)
                    lum = SF.const(S, 1.0) - (SF.from_i32(S, cc[0]) * 0.0004470588235294118 + SF.from_i32(S, cc[1]) * 0.0023019607843137255 + SF.from_i32(S, cc[2]) * 0.001172549019607843)
                    rx = ((Pd * 0.5) * lum) * 1.25
                    ry = rx
                    with S.if_((rx > 0.00392156862745098) & (ry > 0.00392156862745098)):
                        X, Y = xx * Pd, yy * Pd
                        if mode == 0:
                            ellipse(X, Y, rx, ry, [S.i(0), S.i(0), S.i(0)])
                        elif mode == 1:
                            ellipse(X, Y, rx, ry, [cc[0], cc[1], cc[2]])
                        else:
                            ellipse(X - rx * 0.25, Y - ry * 0.25, rx, ry, [cc[0], None, None])
                            ellipse(X + rx * 0.25, Y - ry * 0.25, rx, ry, [None, cc[1], None])
                            ellipse(X, Y + ry * 0.25, rx, ry, [None, None, cc[2]])
            d = [S.max(S.i(255) - sub[ch].get(), 0) for ch in range(3)] + [S.i(255)]
            if True:
                src = S.buf('in0').load(S.gy * P.w + S.gx)
                def qb(v): return nint_f(S, S.min(S.max(v, 0.0), 1.0) * 255.0)
                s = [qb(src.z), qb(src.y), qb(src.x), qb(src.w)]       # B G R A
                if mode == 0:
                    g = SF.from_i32(S, s[0]) * 0.114 + SF.from_i32(S, s[1]) * 0.587 + SF.from_i32(S, s[2]) * 0.299
                    a3 = SF.from_i32(S, s[3])
                    g = SF.sel(S, g < a3, g, a3)
                    gi = g.trunc_i32()
                    s = [gi, gi, gi, s[3]]
                t = P.fade
                def mix(A, B):
                    q = (B - A) * t + 128
                    v = A + ((q + (q >> 8)) >> 8)
                    return S.clamp(v, 0, 255)
                a = mix(d[3], s[3])
                fd = [S.min(mix(d[ch], s[ch]), a) for ch in range(3)] + [a]
                d = [S.select(P.fade >= 0, fd[ch], d[ch]) for ch in range(4)]
            c255 = S.f(255.0)
            f = [fdiv(S, S.to_f(v), c255) for v in d]
            S.buf('out').store(S.gy * P.w + S.gx, S.vec4(f[2], f[1], f[0], f[3]))
    return news_dots
for _m in range(3): _news_dots(_m)

# ---------------------------------------------------------------------------------------------------------- Min and Max / Median in 8 bits (rankFilter8), Median (vegasMedian)
@kernel("rank8", WH + [('nx', 'u'), ('ny', 'u'), ('rank', 'f'), ('pad', 'u')], [PIC_IN, PIC_OUT, TABLE])
def rank8(S):
    """rankFilter8() in builtins_more_blur.cpp: per pixel and channel (B, G, R, A bytes) a histogram of floats in the window's walk order; the table holds the quadrant's weights"""
    P = S.p
    with S.in_range(P.w, P.h):
        inb, tab = S.buf('in0'), S.buf('table')
        hist = S.local_array(256, 'f')
        wi, hi_ = S.to_i(P.w), S.to_i(P.h)
        nx, ny = S.to_i(P.nx), S.to_i(P.ny)
        x, y = S.to_i(S.gx), S.to_i(S.gy)
        total = S.var('f', S.f(0.0))
        vals = [None] * 4
        for b in range(4):
            ch = [2, 1, 0, 3][b]
            with S.for_(0, 256) as z:
                hist.store(z, S.f(0.0))
            with S.for_(0, 2 * P.ny - 1) as step:
                dy = (ny - 1) - S.to_i(step)
                yy = y + dy
                with S.if_((yy >= 0) & (yy < hi_)):
                    iy = S.select(dy < 0, S.i(0) - dy, dy)
                    with S.for_(0, 2 * P.nx - 1) as dxs:
                        dx = S.to_i(dxs) - (nx - 1)
                        xx = x + dx
                        with S.if_((xx >= 0) & (xx < wi)):
                            w = tab.load(S.to_u(iy * nx + S.select(dx < 0, S.i(0) - dx, dx)))
                            with S.if_(~S.eq(w, S.f(0.0))):
                                v = to_level(S, inb.load(S.to_u(yy) * P.w + S.to_u(xx))[ch])
                                hist.store(S.to_u(v), hist.load(S.to_u(v)) + w)
            if b == 0:
                with S.for_(0, 256) as z:
                    total.set(total.get() + hist.load(z))
            t = ((total.get() - 1.0) * P.rank) + 0.5
            lo = t - 0.5
            hi = 0.5 + t
            prev = S.var('f', S.f(0.0))
            acc = S.var('f', S.f(0.0))
            with S.for_(0, 256) as v:
                w = hist.load(v)
                with S.if_(w > 0.0):
                    cur = w + prev.get()
                    with S.if_(~(hi <= prev.get()) & (lo <= cur)):
                        ov = S.select(hi < cur, S.select(lo <= prev.get(), hi - prev.get(), S.f(1.0)), S.select(lo <= prev.get(), w, cur - lo))
                        acc.set(acc.get() + ov * S.to_f(S.to_i(v)))
                    prev.set(cur)
            r = S.clamp(S.to_i(acc.get()), 0, 255)
            vals[ch] = from_level(S, r)
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(vals[0], vals[1], vals[2], vals[3]))

@kernel("med_levels", WH, [PIC_IN, PIC_OUT])
def med_levels(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        def q(v): return S.to_f(nint_f(S, S.min(S.max(v, 0.0), 1.0) * 255.0))
        S.buf('out').store(i, S.vec4(q(p.x), q(p.y), q(p.z), q(p.w)))

@kernel("med_int", WH + [('ncell', 'u'), ('nstep', 'u'), ('r12', 'i'), ('whole', 'i')], [PIC_IN, PIC_OUT, TABLE], local=(64, 1))
def med_int(S):
    """vegasMedian() (builtins_vegasmedian.cpp): one invocation per row, the four channels side by side; a sliding integer histogram (weights of 14 bits)"""
    P = S.p
    with S.if_(S.gx < P.h):
        inb, tab = S.buf('in0'), S.buf('table')
        hist = S.local_array(4 * 257, 'i')
        block = S.local_array(4 * 17, 'i')
        res = S.local_array(4, 'f')
        y = S.to_i(S.gx)
        wi, hi_ = S.to_i(P.w), S.to_i(P.h)
        def level(ch, xx, yy):
            ins = (xx >= 0) & (xx < wi) & (yy >= 0) & (yy < hi_)
            q = inb.load(S.to_u(S.clamp(yy, 0, hi_ - 1)) * P.w + S.to_u(S.clamp(xx, 0, wi - 1)))
            sel = S.select(S.eq(ch, 0), q.x, S.select(S.eq(ch, 1), q.y, S.select(S.eq(ch, 2), q.z, q.w)))
            return S.select(ins, S.to_i(sel), S.i(256))
        with S.for_(0, 4 * 257) as z:
            hist.store(z, S.i(0))
        with S.for_(0, 4 * 17) as z:
            block.store(z, S.i(0))
        with S.for_(0, P.ncell) as ci:
            dyc, dxc, wc = [S.to_i(tab.load(ci * 3 + k)) for k in range(3)]
            with S.for_(0, 4) as ch:
                v = level(ch, dxc, y + dyc)
                hi_idx = ch * 257 + S.to_u(v)
                hist.store(hi_idx, hist.load(hi_idx) + wc)
                bi = ch * 17 + S.to_u(v >> 4)
                block.store(bi, block.load(bi) + wc)
        with S.for_(0, P.w) as xu:
            x = S.to_i(xu)
            with S.for_(0, 4) as ch:
                hb, bb = ch * 257, ch * 17
                total = P.whole - hist.load(hb + 256)
                T = 8192 - (SF.from_i32(S, total - 16384) * SF.from_i32(S, P.r12) * -6.103515625e-05).trunc_i32()
                lo = T - 8192
                hi = T + 8192
                start = S.var('i', S.i(0))
                v0 = S.var('i', S.i(256))
                found = S.var('u', S.u(0))
                with S.for_(0, 16) as bk:
                    with S.if_(S.eq(found.get(), 0)):
                        bsum = start.get() + block.load(bb + bk)
                        with S.if_(bsum <= lo):
                            start.set(bsum)
                        with S.else_():
                            v0.set(S.to_i(bk) * 16)
                            found.set(S.u(1))
                acc = S.var('i', S.i(0))
                done = S.var('u', S.u(0))
                with S.if_(start.get() <= hi):
                    with S.for_(S.to_u(v0.get()), 256) as vu:
                        with S.if_(S.eq(done.get(), 0)):
                            cn = hist.load(hb + vu)
                            with S.if_(cn > 0):
                                end = start.get() + cn
                                a_ = S.max(start.get(), lo)
                                b_ = S.min(end, hi)
                                with S.if_(b_ > a_):
                                    acc.set(acc.get() + S.to_i(vu) * (b_ - a_))
                                start.set(end)
                                with S.if_(end >= hi):
                                    done.set(S.u(1))
                o = S.clamp((acc.get() + 0x2000) >> 14, 0, 255)
                res.store(ch, fdiv(S, S.to_f(o), S.f(255.0)))
            S.buf('out').store(S.to_u(y) * P.w + xu, S.vec4(res.load(S.u(0)), res.load(S.u(1)), res.load(S.u(2)), res.load(S.u(3))))
            with S.if_(x + 1 < wi):
                with S.for_(0, P.nstep) as si:
                    dyc = S.to_i(tab.load((P.ncell + si) * 3))
                    dxc = S.to_i(tab.load((P.ncell + si) * 3 + 1))
                    dw = S.to_i(tab.load((P.ncell + si) * 3 + 2))
                    with S.for_(0, 4) as ch:
                        v2 = level(ch, x + dxc, y + dyc)
                        hidx = ch * 257 + S.to_u(v2)
                        hist.store(hidx, hist.load(hidx) + dw)
                        bidx = ch * 17 + S.to_u(v2 >> 4)
                        block.store(bidx, block.load(bidx) + dw)

# Black Bar Fill, 8-bit (blackBarFill8() in builtins_more_creative.cpp): every buffer here is bottom row first, values as byte / 255
@kernel("bbf8_sample", WH + [('m%d' % i, 'f') for i in range(9)], [PIC_IN, PIC_OUT])
def bbf8_sample(S):
    """bbfSample(): the frame through the 3 x 3 matrix in floats, bilinear with weights of 1/1024 cut to whole numbers"""
    P = S.p
    with S.in_range(P.w, P.h):
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        yf = S.to_f(S.to_i(S.gy))
        xf = S.to_f(S.to_i(S.gx))
        x9 = yf * P.m1 + P.m2
        x7 = yf * P.m4 + P.m5
        x5 = yf * P.m7 + P.m8
        den = xf * P.m6 + x5
        inv = fdiv(S, S.f(1.0), den)
        xp = (xf * P.m0 + x9) * inv
        yp = (xf * P.m3 + x7) * inv
        ok = (xp > -0.4) & (xp < (S.to_f(wi) - 0.6)) & (yp > -0.4) & (yp < (S.to_f(hi) - 0.6))
        res = S.var('v4', S.splat4(0.0))
        with S.if_(ok):
            ix, iy = S.to_i(xp), S.to_i(yp)
            fx = S.select(xp - S.to_f(ix) > 0.0, xp - S.to_f(ix), S.f(0.0))
            fy = S.select(yp - S.to_f(iy) > 0.0, yp - S.to_f(iy), S.f(0.0))
            omx, omy = 1.0 - fx, 1.0 - fy
            w00 = S.to_i((omx * omy) * 1024.0)
            w01 = S.to_i((omx * fy) * 1024.0)
            w10 = S.to_i((omy * fx) * 1024.0)
            w11 = S.to_i((fy * fx) * 1024.0)
            lastX, lastY = S.eq(ix, wi - 1), S.eq(iy, hi - 1)
            ix1 = S.select(lastX, ix, ix + 1)
            iy1 = S.select(lastY, iy, iy + 1)
            inb = S.buf('in0')
            def px(xx, yy): return inb.load(S.to_u(hi - 1 - yy) * P.w + S.to_u(xx))
            q00, q10, q01, q11 = px(ix, iy), px(ix1, iy), px(ix, iy1), px(ix1, iy1)
            out = []
            for k in range(4):
                p00, p10, p01, p11 = [to_level(S, q[k]) for q in (q00, q10, q01, q11)]
                sm = p11 * w11 + p01 * w01 + p10 * w10 + p00 * w00
                out.append(from_level(S, sm >> 10))
            res.set(S.vec4(out[0], out[1], out[2], out[3]))
        S.buf('out').store(S.gy * P.w + S.gx, res.get())

@kernel("bbf8_pl", WH, [PIC_IN, PIC_OUT, TABLE])
def bbf8_pl(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        f = S.buf('in0').load(i)
        a = S.buf('table').load(S.to_u(to_level(S, f.w)))
        S.buf('out').store(i, S.vec4(S.f(0.0), S.f(0.0), S.f(0.0), a * 1.0))

@kernel("bbf8_lay", WH, [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def bbf8_lay(S):
    """the foreground over its shadow (in floats, straight colours), rounded to bytes; in0: the foreground, in1: the blurred shadow plane"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        f = S.buf('in0').load(i)
        pl = S.buf('in1').load(i)
        tab = S.buf('table')
        def T(v): return tab.load(S.to_u(to_level(S, v)))
        Fa = pl.w
        Fc = [pl.x, pl.y, pl.z]
        fb = [f.x, f.y, f.z]
        x0 = 1.0 * Fa
        x1 = 1.0 * T(f.w)
        x4 = 1.0 - x1
        x2 = x0 * 1.0
        x3 = x1 * 1.0
        x2 = x2 * x4
        x4 = x4 * x0
        x4 = x4 + x1
        big = x4 > 9.999999747378752e-05
        col = []
        for k in range(3):
            c = (x3 * T(fb[k])) + (x2 * Fc[k])
            col.append(S.select(big, fdiv(S, c, x4), c))
        col.append(x4)
        out = []
        for k in range(4):
            v = S.to_i(col[k] * 255.0 + 0.5)
            out.append(from_level(S, S.clamp(v, 0, 255)))
        S.buf('out').store(i, S.vec4(out[0], out[1], out[2], out[3]))

@kernel("bbf8_over", WH, [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def bbf8_over(S):
    """the layer (in0) over the background (in1); the result goes out top row first"""
    P = S.p
    with S.in_range(P.w, P.h):
        j = P.h - 1 - S.gy
        i = j * P.w + S.gx
        lay = S.buf('in0').load(i)
        bg = S.buf('in1').load(i)
        tab = S.buf('table')
        ld = [to_level(S, lay.x), to_level(S, lay.y), to_level(S, lay.z), to_level(S, lay.w)]
        dd = [to_level(S, bg.x), to_level(S, bg.y), to_level(S, bg.z), to_level(S, bg.w)]
        ta = tab.load(S.to_u(ld[3]))
        kk = (1.0 - ta) * tab.load(S.to_u(dd[3]))
        out = []
        for k in range(3):
            v = S.to_i((S.to_f(dd[k]) * kk) + (S.to_f(ld[k]) * ta))
            out.append(from_level(S, S.min(v, 255)))
        out.append(from_level(S, S.max(ld[3], dd[3])))
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(out[0], out[1], out[2], out[3]))

# ---------------------------------------------------------------------------------------------------------- Layer Dimensionality (layerDimensionality() in builtins_more_creative.cpp)
@kernel("ld_plane", WH + dpush('sx', 'sy') + [('mode', 'u'), ('ox', 'i'), ('oy', 'i')], [PIC_IN, PIC_OUT])
def ld_plane(S):
    """the alpha plane: mode 0 as it is, 1 shifted by a sub-pixel offset (doubles, zero outside), 2 shifted by whole pixels (the edge repeated); all four lanes carry it"""
    P = S.p
    with S.in_range(P.w, P.h):
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        x, y = S.to_i(S.gx), S.to_i(S.gy)
        inb = S.buf('in0')
        def alpha(xx, yy): return inb.load(S.to_u(yy) * P.w + S.to_u(xx)).w
        a0 = alpha(x, y)
        # mode 2
        sxc, syc = S.clamp(x - P.ox, 0, wi - 1), S.clamp(y - P.oy, 0, hi - 1)
        a2 = alpha(sxc, syc)
        # mode 1
        px = SF.from_i32(S, x) - pd(P, 'sx')
        py = SF.from_i32(S, y) - pd(P, 'sy')
        x0, y0 = px.floor_i32(), py.floor_i32()
        fx, fy = px - SF.from_i32(S, x0), py - SF.from_i32(S, y0)
        def g(xx, yy):
            ins = (xx >= 0) & (yy >= 0) & (xx < wi) & (yy < hi)
            v = alpha(S.clamp(xx, 0, wi - 1), S.clamp(yy, 0, hi - 1))
            return SF.sel(S, ins, SF.from_f32(S, v), SF.const(S, 0.0))
        a1 = ((g(x0, y0) * (1.0 - fx) + g(x0 + 1, y0) * fx) * (1.0 - fy) + (g(x0, y0 + 1) * (1.0 - fx) + g(x0 + 1, y0 + 1) * fx) * fy).to_f32()
        v = S.select(S.eq(P.mode, 1), a1, S.select(S.eq(P.mode, 2), a2, a0))
        S.buf('out').store(S.gy * P.w + S.gx, S.splat4(0.0) + v)

@kernel("ld_base", WH + [('fgop', 'f')], [PIC_IN, PIC_OUT])
def ld_base(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        S.buf('out').store(i, S.select(P.fgop < 1.0, p * P.fgop, p))

@kernel("ld_inner", WH + [('fgop', 'f'), ('amount', 'f')], [PIC_IN, PIC_IN1, PIC_OUT])
def ld_inner(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        src = S.buf('in0').load(i)
        pl = S.buf('in1').load(i).w
        o = S.select(P.fgop < 1.0, src * P.fgop, src)
        v = S.min(S.f(1.0), S.max(S.f(0.0), ((1.0 - pl) * P.amount) * src.w))
        om = 1.0 - v
        S.buf('out').store(i, S.vec4(o.x * om, o.y * om, o.z * om, v + o.w * om))

@kernel("ld_drop", WH + [('fgop', 'f'), ('amount', 'f')], [PIC_IN, PIC_IN1, PIC_OUT])
def ld_drop(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        src = S.buf('in0').load(i)
        sh = S.buf('in1').load(i).x
        o = S.select(P.fgop < 1.0, src * P.fgop, src)
        k = (sh * P.amount) * (1.0 - o.w)
        af = o.w
        aout = af + k
        adj = (P.fgop < 1.0) & (o.w + k > 0.0)
        r = [S.select(adj, fdiv(S, c * af, aout), c) for c in (o.x, o.y, o.z)]
        S.buf('out').store(i, S.vec4(r[0], r[1], r[2], o.w + k))

@kernel("ld_gdiff", WH + [('inner', 'u'), ('pad', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def ld_gdiff(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        alpha = S.buf('in0').load(i).w
        g = S.buf('in1').load(i).x
        a = S.select(S.ne(P.inner, 0), S.max(S.f(0.0), g - alpha), S.max(S.f(0.0), alpha - g) * 2.0)
        S.buf('out').store(i, S.splat4(0.0) + a)

@kernel("ld_glow", WH + [('cr', 'f'), ('cg', 'f'), ('cb', 'f'), ('pad', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def ld_glow(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        o = S.buf('in0').load(i)
        a = S.buf('in1').load(i).x
        om = 1.0 - a
        res = S.vec4(P.cr * a + o.x * om, P.cg * a + o.y * om, P.cb * a + o.z * om, a + o.w * om)
        S.buf('out').store(i, S.select(a <= 0.0, o, res))

@kernel("ld_emboss", WH + dpush('ox', 'oy') + [('gain', 'f'), ('pad', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def ld_emboss(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        o = S.buf('in0').load(i)
        sm = S.buf('in1')
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        x, y = S.to_i(S.gx), S.to_i(S.gy)
        interior = (x >= 1) & (x < wi - 1) & (y >= 1) & (y < hi - 1) & (hi > 2)
        ox, oy = pd(P, 'ox'), pd(P, 'oy')
        def axis(pos, n):
            pos = sf_clamp(S, pos, SF.const(S, 0.0), SF.from_i32(S, n - 1))
            i0 = pos.trunc_i32()
            return i0, S.min(n - 1, i0 + 1), (pos - SF.from_i32(S, i0)).to_f32()
        def at(ax, ay):
            def px(xx, yy): return sm.load(S.to_u(yy) * P.w + S.to_u(xx)).x
            a = px(ax[0], ay[0]) * (1.0 - ax[2]) + px(ax[1], ay[0]) * ax[2]
            b = px(ax[0], ay[1]) * (1.0 - ax[2]) + px(ax[1], ay[1]) * ax[2]
            return a * (1.0 - ay[2]) + b * ay[2]
        xd, yd = SF.from_i32(S, x), SF.from_i32(S, y)
        cA, cB = axis(xd + ox, wi), axis(xd - ox, wi)
        rA, rB = axis(yd + oy, hi), axis(yd - oy, hi)
        slope = (at(cA, rA) - at(cB, rB)) * P.gain
        c = [S.min(S.max(v + slope * o.w, 0.0), o.w) for v in (o.x, o.y, o.z)]
        S.buf('out').store(i, S.select(interior, S.vec4(c[0], c[1], c[2], o.w), o))

# ---------------------------------------------------------------------------------------------------------- Radial Pixelate (radialPixelate() in builtins_more_creative.cpp)
@kernel("rpix_polar", WH + [('c0', 'f'), ('c1', 'f'), ('q8', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT, TABLE])
def rpix_polar(S):
    """the picture unrolled about the centre: column = angle, row = radius, read at the nearest pixel below; values in levels. The table holds the sine and cosine of each column's angle"""
    P = S.p
    with S.in_range(P.w, P.h):
        tab = S.buf('table')
        Wf, Hf = S.to_f(P.w), S.to_f(P.h)
        rr = fdiv(S, S.to_f(S.gy), Hf) * 2.8284270763397217
        px = tab.load(S.gx * 2) * rr
        py = tab.load(S.gx * 2 + 1) * rr
        px = ((px + P.c0) * 0.5 + 0.5) * Wf
        py = ((py + P.c1) * 0.5 + 0.5) * Hf
        px = S.select(px < 0.0, S.f(0.0), px)
        px = S.select(px < Wf, px, S.to_f(P.w - 1))
        py = S.select(py < 0.0, S.f(0.0), py)
        py = S.select(py < Hf, py, S.to_f(P.h - 1))
        sx, sy = S.to_u(S.to_i(S.floor(px))), S.to_u(S.to_i(S.floor(py)))
        p = S.buf('in0').load((P.h - 1 - sy) * P.w + sx)
        q8 = S.ne(P.q8, 0)
        def lv(v): return S.select(q8, S.to_f(to_level(S, v)), v * 255.0)
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(lv(p.x), lv(p.y), lv(p.z), lv(p.w)))

@kernel("rpix_small", WH + [('gw', 'u'), ('gh', 'u'), ('sx', 'i'), ('sy', 'i'), ('bw', 'i'), ('bh', 'i'), ('q8', 'u'), ('pad', 'u')] + dpush('inv'), [PIC_IN, PIC_OUT])
def rpix_small(S):
    """the block means of the polar picture (a double sum in the CPU's order); one thread per block"""
    P = S.p
    with S.in_range(P.gw, P.gh):
        inb = S.buf('in0')
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        x0 = (S.to_i(S.gx) * P.sx) / 16384
        y0 = (S.to_i(S.gy) * P.sy) / 16384
        tot = [SFVar(S, sf_zero(S)) for _ in range(4)]
        with S.for_(y0, S.min(hi, y0 + P.bh), 1, True) as yy:
            with S.for_(x0, S.min(wi, x0 + P.bw), 1, True) as xx:
                q = inb.load(S.to_u(yy) * P.w + S.to_u(xx))
                for k in range(4):
                    tot[k].set(tot[k].get() + SF.from_f32(S, q[k]))
        q8 = S.ne(P.q8, 0)
        out = []
        for k in range(4):
            v = tot[k].get() * pd(P, 'inv')
            t = (SF.from_i32(S, v.trunc_i32()) * 0.00392156862745098) * 255.0
            lvl = S.min(S.i(255), t.trunc_i32())
            out.append(S.select(q8, S.to_f(lvl), v.to_f32()))
        S.buf('out').store(S.gy * P.gw + S.gx, S.vec4(*out))

@kernel("rpix_out", WH + [('gw', 'u'), ('gh', 'u'), ('tx', 'i'), ('ty', 'i'), ('c0', 'f'), ('c1', 'f')], [PIC_IN, PIC_OUT])
def rpix_out(S):
    """the blocks spread back over the polar picture, rolled up again: each output pixel reads the block holding its polar position"""
    P = S.p
    with S.in_range(P.w, P.h):
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        Wf, Hf = S.to_f(P.w), S.to_f(P.h)
        j = P.h - 1 - S.gy
        Y = (fdiv(S, S.to_f(j), Hf) * 2.0 - 1.0) - P.c1
        X = (fdiv(S, S.to_f(S.gx), Wf) * 2.0 - 1.0) - P.c0
        r = fsqrt(S, X * X + Y * Y)
        ri = fdiv(S, r, S.f(2.8284270763397217)) * Hf
        ang = SF.from_f32(S, sf_atan2(S, SF.from_f32(S, X), SF.from_f32(S, Y)).to_f32())      # (the CPU keeps the angle as a float)
        u = ((ang / 3.141592653589793) * 0.5 + 0.5).to_f32() * Wf
        u = S.select(u > 0.0, u, S.f(0.0))
        u = S.select(u < Wf, u, S.to_f(P.w - 1))
        ri = S.select(ri > 0.0, ri, S.f(0.0))
        ri = S.select(ri < Hf, ri, S.to_f(P.h - 1))
        xx, yy = S.to_i(S.floor(u)), S.to_i(S.floor(ri))
        gx = S.var('i', S.min(S.to_i(P.gw) - 1, (xx * 16384) / P.tx))
        gy = S.var('i', S.min(S.to_i(P.gh) - 1, (yy * 16384) / P.ty))
        for _ in range(2):      # (the estimate is at most a block or two short)
            with S.if_((gx.get() < S.to_i(P.gw) - 1) & ((((gx.get() + 1) * P.tx) / 16384) <= xx)):
                gx.set(gx.get() + 1)
            with S.if_((gy.get() < S.to_i(P.gh) - 1) & ((((gy.get() + 1) * P.ty) / 16384) <= yy)):
                gy.set(gy.get() + 1)
        p = S.buf('in0').load(S.to_u(gy.get()) * P.gw + S.to_u(gx.get()))
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(fdiv(S, p.x, S.f(255.0)), fdiv(S, p.y, S.f(255.0)), fdiv(S, p.z, S.f(255.0)), fdiv(S, p.w, S.f(255.0))))

# ---------------------------------------------------------------------------------------------------------- AutoLooks (autoLooks() in builtins_color.cpp)
from gpukernels import lut_apply, pixel_row

@kernel("autolooks", WH + [('n', 'u'), ('g8', 'f'), ('d0', 'f'), ('d1', 'f'), ('d2', 'f'), ('e0', 'f'), ('e1', 'f'), ('e2', 'f')], [PIC_IN, PIC_OUT, TABLE])
def autolooks(S):
    # the look's table (trilinear), cut to 0..1, then mixed with the picture in bytes: (picture x (255 - s) + table x s) / 255 rounded
    P = S.p
    def body(S, r, g, b):
        v = lut_apply(S, S.buf('table'), S.u(0), P.n, [P.d0, P.d1, P.d2], [P.e0, P.e1, P.e2], S.u(0), [r, g, b])
        res = []
        for cin, t in zip((r, g, b), v):
            t = S.select(t < 0.0, S.f(0.0), S.select(t > 1.0, S.f(1.0), t))
            s8 = S.floor(S.min(S.max(cin, 0.0), 1.0) * 255.0 + 0.5)
            l8 = S.floor(t * 255.0 + 0.5)
            m = fdiv(S, s8 * (255.0 - P.g8) + l8 * P.g8, S.f(255.0))
            res.append(fdiv(S, S.floor(m + 0.5), S.f(255.0)))
        return res[0], res[1], res[2]
    pixel_row(S, body)

# ---------------------------------------------------------------------------------------------------------- Tiny Planet (both versions)
def sf_asin_ok(S, x):
    """(asin(x), x within -1..1): the angle by atan2 of x and the root of 1 - x^2"""
    den = ((1.0 - x) * (1.0 + x))
    ok = (x <= 1.0) & (x >= -1.0)
    den = SF.sel(S, ok, den, SF.const(S, 0.0))
    return sf_atan2(S, x, den.sqrt()), ok

@kernel("tpr", WH + dpush('ct', 'st', 'r', 'lon'), [PIC_IN, PIC_OUT])
def tpr(S):
    # tinyPlanet() in builtins_more.cpp (Reno's own): a bilinear read, in doubles
    P = S.p
    with S.in_range(P.w, P.h):
        pi = SF.const(S, 3.1415926535897932384626433832795)
        wd, hd = SF.from_u32(S, P.w), SF.from_u32(S, P.h)
        R = pd(P, 'r')
        px = ((SF.from_u32(S, S.gx) + 0.5) - wd * 0.5) / R
        py = ((SF.from_u32(S, S.gy) + 0.5) - hd * 0.5) / R
        r2 = px * px + py * py
        k = 1.0 / (r2 + 1.0)
        X = (2.0 * px) * k
        Z = (2.0 * py) * k
        Y = (r2 - 1.0) * k
        ct, st = pd(P, 'ct'), pd(P, 'st')
        Y2 = Y * ct - Z * st
        Z2 = Y * st + Z * ct
        lonS = sf_atan2(S, X, Z2) + pd(P, 'lon')
        yc = SF.sel(S, Y2 < -1.0, SF.const(S, -1.0), SF.sel(S, Y2 > 1.0, SF.const(S, 1.0), Y2))
        latS, _ok = sf_asin_ok(S, yc)
        u = lonS / (pi * 2.0) + 0.5
        u = u - u.floor()
        v = 0.5 - latS / pi
        S.buf('out').store(S.gy * P.w + S.gx, buf_sample(S, S.buf('in0'), P.w, P.h, u * wd, v * hd))

@kernel("gather_tab", WH, [PIC_IN, PIC_OUT, TABLE])
def gather_tab(S):
    """each output pixel copies the input pixel whose index (the bits of a table word) the host worked out"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        src = S.f_to_bits(S.buf('table').load(i))
        S.buf('out').store(i, S.buf('in0').load(src))

# ---------------------------------------------------------------------------------------------------------- Color Corrector (Secondary), 8-bit general case (ccsRowGeneral() in builtins_filters2.cpp)
@kernel("ccs8", WH + [('fl', 'u'), ('show', 'u'), ('inv', 'u'), ('tintu', 'f'), ('tintv', 'f'), ('satf', 'f'), ('alphaf', 'f'), ('r0', 'f'), ('r1', 'f'), ('r2', 'f'), ('r3', 'f')], [PIC_IN, PIC_OUT, TABLE])
def ccs8(S):
    from gpukernels import round_magic
    P = S.p
    with S.in_range(P.w, P.h):
        tab = S.buf('table')
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        rr, gg, bb, aa = to_level(S, p.x), to_level(S, p.y), to_level(S, p.z), to_level(S, p.w)
        fb, fg, fr = S.to_f(bb), S.to_f(gg), S.to_f(rr)
        Y = fr * 0.299
        Y = Y + fg * 0.587
        Y = Y + fb * 0.114
        U = (fb - Y) * 0.492
        V = (fr - Y) * 0.877
        rad = fsqrt(S, V * V + U * U)
        aU, aV = S.abs(U), S.abs(V)
        swap = aV > aU
        mn = S.select(swap, aU, aV)
        oct_ = S.select(swap, S.u(4), S.u(0)) | (((S.f_to_bits(V) & S.u(0xBFFFFFFF)) | (S.f_to_bits(U) >> 1)) >> 30)
        rowv = S.select(rad > 0.0, round_magic(S, fdiv(S, mn, rad) * 240.0), S.i(0))
        oct_ = S.select(rad > 0.0, oct_, S.u(0))
        iy = S.max(S.min(round_magic(S, Y * 4.0), S.i(1023)), S.i(0))
        ir = S.max(S.min(round_magic(S, rad * 4.0), S.i(647)), S.i(0))
        rowv = S.max(S.min(rowv, S.i(174)), S.i(0))
        mk = tab.load(S.to_u(iy)) * tab.load(S.u(2048) + S.to_u(ir))
        mk = mk * tab.load(S.u(2696) + S.to_u(rowv) * 8 + oct_)
        winv = S.select(S.ne(P.inv, 0), S.f(1.0), S.f(0.0))
        # show mask
        v = (winv - mk) * S.select(S.ne(P.inv, 0), S.f(255.0), S.f(-255.0))
        v = S.select(S.f(0.0) > v, S.f(0.0), S.select(S.f(255.0) < v, S.f(255.0), v))
        qm = round_magic(S, v) & 255
        # apply
        w = S.abs(mk - winv)
        Ut = U + P.tintu
        Vt = V + P.tintv
        Yp = Y + tab.load(S.u(1024) + S.to_u(iy))
        omw = 1.0 - w
        rot = S.ne(P.fl & 0x18, 0)
        fa = S.to_f(aa)
        # without rotation / alpha
        Us = Ut * P.satf
        Vs = Vt * P.satf
        nb = Us * 2.032520294189453
        nb = nb + Yp
        ng = Yp - Us * 0.394731342792511
        ng = ng - Vs * 0.5808091759681702
        nr = Vs * 1.1402509212493896
        nr = nr + Yp
        nb = nb * w; nr = nr * w; ng = ng * w
        nr = nr + omw * fr
        ng = ng + omw * fg
        nb = nb + omw * fb
        def cut(x, hi): return S.select(S.f(0.0) > x, S.f(0.0), S.select(hi < x, hi, x))
        a_r, a_g, a_b = cut(nr, fa), cut(ng, fa), cut(nb, fa)
        # with rotation / alpha
        M0, M1, M2, M3 = P.satf * P.r0, P.satf * P.r1, P.satf * P.r2, P.satf * P.r3
        wa = w * P.alphaf
        nu = M0 * Ut
        nu = nu + M1 * Vt
        nv = M3 * Vt
        nv = nv + M2 * Ut
        na = omw * fa
        na = na + wa * fa
        na = S.select(S.f(0.0) > na, S.f(0.0), S.select(S.f(255.0) < na, S.f(255.0), na))
        qb = nu * 2.032520294189453
        qb = qb + Yp
        qg = Yp - nu * 0.394731342792511
        qg = qg - nv * 0.5808091759681702
        qr = nv * 1.1402509212493896
        qr = qr + Yp
        qb = qb * wa; qr = qr * wa; qg = qg * wa
        qr = qr + omw * fr
        qg = qg + omw * fg
        qb = qb + omw * fb
        b_r, b_g, b_b = cut(qr, na), cut(qg, na), cut(qb, na)
        sel_r = S.select(rot, b_r, a_r)
        sel_g = S.select(rot, b_g, a_g)
        sel_b = S.select(rot, b_b, a_b)
        applied = w > 0.0
        o_r = S.select(applied, round_magic(S, sel_r) & 255, rr)
        o_g = S.select(applied, round_magic(S, sel_g) & 255, gg)
        o_b = S.select(applied, round_magic(S, sel_b) & 255, bb)
        o_a = S.select(applied & rot, round_magic(S, na) & 255, aa)
        shown = S.ne(P.show, 0)
        S.buf('out').store(i, S.vec4(from_level(S, S.select(shown, qm, o_r)), from_level(S, S.select(shown, qm, o_g)), from_level(S, S.select(shown, qm, o_b)), from_level(S, S.select(shown, qm, o_a))))

# ---------------------------------------------------------------------------------------------------------- Radial Blur (radialBlur() in builtins_more_blur.cpp)
@kernel("rb_prep", WH + [('q8', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def rb_prep(S):
    """the picture bottom row first, in levels (8-bit: whole levels) or floats of 0..255"""
    P = S.p
    with S.in_range(P.w, P.h):
        p = S.buf('in0').load((P.h - 1 - S.gy) * P.w + S.gx)
        q8 = S.ne(P.q8, 0)
        def lv(v): return S.select(q8, S.to_f(to_level(S, v)), v * 255.0)
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(lv(p.x), lv(p.y), lv(p.z), lv(p.w)))

@kernel("rb_fin", WH, [PIC_IN, PIC_OUT])
def rb_fin(S):
    P = S.p
    with S.in_range(P.w, P.h):
        p = S.buf('in0').load((P.h - 1 - S.gy) * P.w + S.gx)
        k = S.f(1.0 / 255.0)
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(p.x * k, p.y * k, p.z * k, p.w * k))

@kernel("rb_step", WH + [('type', 'u'), ('q8', 'u'), ('cx', 'i'), ('cy', 'i'), ('off', 'u'), ('cnt', 'u'), ('wi', 'i'), ('iwi', 'i')] + dpush('s0', 's1', 'wf', 'iwf'), [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def rb_step(S):
    """one step of the in-place pass: in0 / out the blurred picture, in1 the source (both bottom row first); the table lists the pixels (x | y << 14 | quadrant << 28 | restore << 30)"""
    P = S.p
    with S.in_range(P.cnt, S.u(1)):
        D, SRC = S.buf('in0'), S.buf('in1')
        wi_, hi_ = S.to_i(P.w), S.to_i(P.h)
        rec = S.f_to_bits(S.buf('table').load(P.off + S.gx))
        x = S.to_i(rec & 0x3fff)
        y = S.to_i((rec >> 14) & 0x3fff)
        q = (rec >> 28) & 3
        restore = S.ne((rec >> 30) & 1, 0)
        idx = S.to_u(y) * P.w + S.to_u(x)
        o0 = SRC.load(idx)
        with S.if_(restore):
            D.store(idx, o0)
        with S.else_():
            dX, dY = P.cx - x, P.cy - y
            ddx, ddy = SF.from_i32(S, dX), SF.from_i32(S, dY)
            xd, yd = SF.from_i32(S, x), SF.from_i32(S, y)
            s0, s1 = pd(P, 's0'), pd(P, 's1')
            d = SF.from_i32(S, dX * dX + dY * dY).sqrt()
            near = ~(s0 < d)
            dd = SF.sel(S, near, SF.const(S, 1.0), d)
            f = s0 / dd
            sxr = SF.sel(S, near, SF.from_i32(S, P.cx), ddx * f + xd)
            syr = SF.sel(S, near, SF.from_i32(S, P.cy), ddy * f + yd)
            sx = SF.sel(S, S.ne(P.type, 0), sxr, ddx * s1 + xd)
            sy = SF.sel(S, S.ne(P.type, 0), syr, ddy * s1 + yd)
            ix = S.max(S.i(0), S.min(wi_ - 1, sx.trunc_i32()))
            iy = S.max(S.i(0), S.min(hi_ - 1, sy.trunc_i32()))
            frx = sx - SF.from_i32(S, ix)
            fry = sy - SF.from_i32(S, iy)
            edge = S.eq(ix, wi_ - 1) | S.eq(iy, hi_ - 1)
            ixr, iyd = S.min(ix + 1, wi_ - 1), S.min(iy + 1, hi_ - 1)
            def at(xx, yy): return D.load(S.to_u(yy) * P.w + S.to_u(xx))
            black = S.vec4(0.0, 0.0, 0.0, 255.0)
            t0 = at(ix, iy)
            t1 = S.select(S.eq(ix, wi_ - 1), black, at(ixr, iy))
            t2 = S.select(S.eq(iy, hi_ - 1), black, at(ix, iyd))
            t3 = S.select(edge, black, at(ixr, iyd))
            # the source in place of the pixel's own corner
            close = (sx - xd).abs() < 1.0
            close = close & ((sy - yd).abs() < 1.0)
            zero = frx.eq(0.0) & fry.eq(0.0)
            t0 = S.select(close & zero, o0, t0)
            rep = close & ~zero
            t0 = S.select(rep & S.eq(q, 0), o0, t0)
            t1 = S.select(rep & S.eq(q, 1), o0, t1)
            t2 = S.select(rep & S.eq(q, 2), o0, t2)
            t3 = S.select(rep & S.eq(q, 3), o0, t3)
            res = []
            with S.if_(S.ne(P.q8, 0)):
                fx = (frx * 256.0).trunc_i32()
                fy = (fry * 256.0).trunc_i32()
                out = []
                for ch in range(4):
                    a = S.to_i(t0[ch]) * (256 - fx) + S.to_i(t1[ch]) * fx
                    a = a * (256 - fy)
                    b = S.to_i(t2[ch]) * (256 - fx) + S.to_i(t3[ch]) * fx
                    b = b * fy
                    smp = ((a + b) >> 16) & 255
                    out.append(S.to_f((smp * P.wi + S.to_i(o0[ch]) * P.iwi) >> 16))
                D.store(idx, S.vec4(*out))
            with S.else_():
                out = []
                for ch in range(4):
                    p0, p1, p2, p3 = (SF.from_f32(S, t[ch]) for t in (t0, t1, t2, t3))
                    smp = (p0 * (1.0 - frx) + p1 * frx) * (1.0 - fry) + (p2 * (1.0 - frx) + p3 * frx) * fry
                    out.append((smp * pd(P, 'wf') + SF.from_f32(S, o0[ch]) * pd(P, 'iwf')).to_f32())
                D.store(idx, S.vec4(*out))

# ---------------------------------------------------------------------------------------------------------- Histogram (histogramFx() in builtins_vegasrest.cpp; linear scale, types 0 to 4)
@kernel("hist_count", WH + [('type', 'u'), ('pad', 'u')] + dpush('hdr'), [(0, 'src', 'v4', 'r'), (2, 'hist', 'u', 'rw')])
def hist_count(S):
    P = S.p
    with S.in_range(P.w, P.h):
        p = S.buf('src').load(S.gy * P.w + S.gx)
        a = p.w
        t = P.type
        use = S.eq(t, 4) | (a > 1e-5)
        r = fdiv(S, p.x, a)
        g = fdiv(S, p.y, a)
        b = fdiv(S, p.z, a)
        lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
        v = S.select(S.eq(t, 4), a, S.select(S.eq(t, 0), lum, S.select(S.eq(t, 1), r, S.select(S.eq(t, 2), g, b))))
        q = SF.from_f32(S, v) / pd(P, 'hdr')
        q = SF.sel(S, q < 0.0, SF.const(S, 0.0), SF.sel(S, q > 0.99999, SF.const(S, 0.99999), q))
        bin_ = S.min((q * 128.0).trunc_i32(), S.i(127))
        with S.if_(use):
            S.buf('hist').atomic_add(S.to_u(bin_), S.u(1))

@kernel("hist_draw", WH + [('x0', 'i'), ('y0', 'i'), ('bw', 'i'), ('bh', 'i'), ('back', 'f'), ('cr', 'f'), ('cg', 'f'), ('cb', 'f')], [(0, 'src', 'v4', 'r'), (1, 'hist', 'u', 'r'), (2, 'out', 'v4', 'w')])
def hist_draw(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        o = S.buf('src').load(i)
        hist = S.buf('hist')
        x, y = S.to_i(S.gx), S.to_i(S.gy)
        inside = (x >= P.x0) & (x < P.x0 + P.bw) & (y >= P.y0) & (y < P.y0 + P.bh)
        res = S.var('v4', o)
        with S.if_(inside):
            mxv = S.var('u', S.u(0))
            with S.for_(0, 128) as k:
                c = hist.load(k)
                mxv.set(S.max(mxv.get(), c))
            b0 = S.min(127, ((x - P.x0) * 128) / P.bw)
            b1 = S.max(b0, S.min(127, ((x - P.x0 + 1) * 128) / P.bw - 1))
            bv = S.var('u', S.u(0))
            with S.for_(S.to_u(b0), S.to_u(b1) + 1) as k:
                bv.set(S.max(bv.get(), hist.load(k)))
            hgt = SF.from_u32(S, bv.get()) / SF.from_u32(S, mxv.get())
            fb = 1.0 - ((SF.from_i32(S, y - P.y0) + 0.5) / SF.from_i32(S, P.bh))
            k = P.back * 0.6
            om = 1.0 - k
            bgc = S.vec4(o.x * om, o.y * om, o.z * om, o.w + (1.0 - o.w) * k)
            bar = S.vec4(P.cr * 0.9, P.cg * 0.9, P.cb * 0.9, S.f(1.0))
            drawn = S.select(~(fb > hgt), bar, bgc)
            res.set(S.select(S.ne(mxv.get(), 0), drawn, o))
        S.buf('out').store(i, res.get())

# ---------------------------------------------------------------------------------------------------------- Bezier Masking (bezierMasking() in builtins_mask_bezier.cpp): the masks' total comes from the host
@kernel("bz_apply", WH + [('invert', 'u'), ('blend', 'f')], [PIC_IN, PIC_OUT, TABLE])
def bz_apply(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        m = S.buf('table').load(i)
        m = S.select(S.ne(P.invert, 0), 1.0 - m, m)
        m = m + (1.0 - m) * P.blend
        s = S.buf('in0').load(i)
        S.buf('out').store(i, S.vec4(s.x * m, s.y * m, s.z * m, s.w * m))

# ---------------------------------------------------------------------------------------------------------- Lens Correction (lensCorrection() in builtins_mask.cpp)
@kernel("lens", WH + [('lv', 'u'), ('pad', 'u'), ('cxp', 'f'), ('cyp', 'f'), ('d', 'f'), ('x14', 'f'), ('x3', 'f'), ('x13', 'f'), ('s', 'f'), ('iw', 'f'), ('ih', 'f')], [PIC_IN, PIC_OUT])
def lens(S):
    P = S.p
    with S.in_range(P.w, P.h):
        inb = S.buf('in0')
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        m = hi - 1 - S.to_i(S.gy)                      # (the row counted from the bottom)
        dy = S.to_f(m) - P.cyp
        dx = S.to_f(S.gx) - P.cxp
        r = fsqrt(S, dx * dx + dy * dy)
        f = ((r * P.x13 + P.x3) * r + P.x14) * r + P.d
        f = f * P.s
        mx = (f * dx + P.cxp) * P.iw
        my = (f * dy + P.cyp) * P.ih
        sx = S.to_f(wi - 1) * mx
        sy = S.to_f(hi - 1) * my
        ok = (sx > -0.4) & (sx < S.to_f(wi) - 0.6) & (sy > -0.4) & (sy < S.to_f(hi) - 0.6)
        res = S.var('v4', S.splat4(0.0))
        with S.if_(ok):
            x0, y0 = S.to_i(sx), S.to_i(sy)
            fx = S.max(sx - S.to_f(x0), S.f(0.0))
            fy = S.max(sy - S.to_f(y0), S.f(0.0))
            N = wi * hi
            i = y0 * wi + x0
            lv = S.ne(P.lv, 0)
            def px(idx):
                ii = S.max(S.min(idx, N - 1), S.i(0))
                row = ii / wi
                col = ii - row * wi
                p = inb.load(S.to_u(hi - 1 - row) * P.w + S.to_u(col))
                out = []
                for k in range(4):
                    v = S.min(S.max(p[k], 0.0), 1.0) * 255.0
                    out.append(S.select(lv, S.to_f(nint_f(S, v)), p[k] * 255.0))
                return out
            b = px(i)
            i10 = S.select(i + 1 < N, i + 1, i)
            i01 = S.select(i + wi < N, i + wi, i)
            s10 = px(i10)
            s01 = px(i01)
            s11 = [S.select(i + wi + 1 < N, a, c) for a, c in zip(px(S.select(i + wi + 1 < N, i + wi + 1, i)), s01)]
            w00 = (1.0 - fx) * (1.0 - fy)
            w01 = (1.0 - fx) * fy
            w11 = fy * fx
            w10 = (1.0 - fy) * fx
            i00 = S.to_i(w00 * 1024.0); i01w = S.to_i(w01 * 1024.0); i11 = S.to_i(w11 * 1024.0); i10w = S.to_i(w10 * 1024.0)
            out = []
            for k in range(4):
                acc = (S.to_i(s11[k]) * i11 + S.to_i(s01[k]) * i01w + S.to_i(s10[k]) * i10w + S.to_i(b[k]) * i00) >> 10
                e8 = fdiv(S, S.to_f(acc), S.f(255.0))
                ef = s11[k] * w11
                ef = ef + s01[k] * w01
                ef = ef + s10[k] * w10
                ef = ef + b[k] * w00
                out.append(S.select(lv, e8, fdiv(S, ef, S.f(255.0))))
            res.set(S.vec4(*out))
        S.buf('out').store(S.gy * P.w + S.gx, res.get())


# ---------------------------------------------------------------------------------------------------------- CoreGraphics.Native Gaussian blur (single precision)
@kernel("cg_blur", WH + [('vert', 'u'), ('gain', 'f'), ('c1', 'f'), ('c2', 'f'), ('c3', 'f')], [PIC_IN, PIC_OUT], local=(64, 1))
def cg_blur(S):
    """filterLine() in builtins_cgblur.cpp: one line (a row, or a column with vert), the four channels together, in floats in the CPU's order of operations;
    the forward half goes into the output, the backward half runs over it. In place (in0 == out) works."""
    P = S.p
    vert = S.ne(P.vert, 0)
    n = S.select(vert, P.h, P.w)
    lines = S.select(vert, P.w, P.h)
    with S.if_(S.gx < lines):
        line = S.gx
        src = S.buf('in0')
        out = S.buf('out')
        def idx(q): return S.select(vert, q * P.w + line, line * P.w + q)
        tiny = S.f(1e-5)
        zero = S.f(0.0)
        def step(x, h):
            ys = []
            for k in range(4):
                acc = x[k] * P.gain
                acc = acc + P.c1 * h[0][k].get()
                acc = acc + P.c2 * h[1][k].get()
                acc = acc + P.c3 * h[2][k].get()
                ys.append(S.select(tiny > acc, zero, acc))
            for k in range(4):
                h[2][k].set(h[1][k].get())
                h[1][k].set(h[0][k].get())
                h[0][k].set(ys[k])
            return ys
        x0 = src.load(idx(S.u(0)))
        h = [[S.var('f', v) for v in (x0.x, x0.y, x0.z, x0.w)] for _ in range(3)]
        with S.for_(0, n) as i:
            xi = src.load(idx(i))
            ys = step((xi.x, xi.y, xi.z, xi.w), h)
            out.store(idx(i), S.vec4(ys[0], ys[1], ys[2], ys[3]))
        la = out.load(idx(n - 1))
        for k, v in enumerate((la.x, la.y, la.z, la.w)):
            for r in range(3):
                h[r][k].set(v)
        with S.for_(0, n) as t:
            q = idx(n - 1 - t)
            yi = out.load(q)
            ys = step((yi.x, yi.y, yi.z, yi.w), h)
            out.store(q, S.vec4(ys[0], ys[1], ys[2], ys[3]))


def sf_byte(S, v):
    """byteOf() in builtins_velvet.cpp: (int)clamp(floor((double)(v * 255.f) + 0.5), 0, 255); a float"""
    t = SF.from_f32(S, v * 255.0) + 0.5
    r = t.floor_i32()
    return S.to_f(S.min(S.max(r, S.i(0)), S.i(255)))


@kernel("fl_mask", WH, [PIC_IN, PIC_OUT, TABLE])
def fl_mask(S):
    # fillLightVelvet(): the byte luma of each pixel picks the colour (table: 256 x (alpha, r, g, b) bytes); out = (r, g, b, alpha) as bytes
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        r, g, b = S.to_i(sf_byte(S, p.x)), S.to_i(sf_byte(S, p.y)), S.to_i(sf_byte(S, p.z))
        l = (S.i(19595) * r + S.i(38469) * g + S.i(7472) * b + S.i(32768)) >> 16
        base = S.to_u(l) * 4
        t = S.buf('table')
        S.buf('out').store(i, S.vec4(t.load(base + 1), t.load(base + 2), t.load(base + 3), t.load(base)))


@kernel("fl_final", WH + [('fop', 'f'), ('rnd', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def fl_final(S):
    # fillLightVelvet(): the (blurred) mask back to bytes (cgBlurByte, when it was blurred), then the Add at the opacity on the float picture
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s0 = S.buf('in0').load(i)
        m = S.buf('in1').load(i)
        def mb(v):
            c255, zero = SF.const(S, 255.0), SF.const(S, 0.0)
            t = SF.from_f32(S, v) + 1.5e-8
            t = SF.sel(S, t < zero, zero, SF.sel(S, t > c255, c255, t))
            return S.select(S.ne(P.rnd, 0), S.to_f(sf_nearbyint(S, t)), v)
        k = S.f(1.0 / 255.0)
        mm = [mb(m.x), mb(m.y), mb(m.z), mb(m.w)]
        o = [s0.x + mm[0] * k * P.fop, s0.y + mm[1] * k * P.fop, s0.z + mm[2] * k * P.fop]
        a = s0.w + mm[3] * k * P.fop * (S.f(1.0) - s0.w)
        S.buf('out').store(i, S.vec4(o[0], o[1], o[2], a))


# ---------------------------------------------------------------------------------------------------------- Soft Contrast (softContrast() in builtins_velvet.cpp)
@kernel("sc_stretch", WH, [PIC_IN, PIC_OUT, TABLE])
def sc_stretch(S):
    # the straight colour through the stretch table (256 levels): out = (R, G, B, alpha) with R, G, B whole levels
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        a = p.w
        has = a > 1e-6
        z = S.f(0.0)
        t = S.buf('table')
        def ch(v):
            q = S.select(has, fdiv(S, v, a), z)
            return t.load(S.to_u(S.to_i(sf_byte(S, clamp01(S, q)))))
        S.buf('out').store(i, S.vec4(ch(p.x), ch(p.y), ch(p.z), a))


@kernel("sc_gray", WH, [PIC_IN, PIC_OUT])
def sc_gray(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        l = S.to_f((S.i(19595) * S.to_i(p.x) + S.i(38469) * S.to_i(p.y) + S.i(7472) * S.to_i(p.z) + S.i(32768)) >> 16)
        S.buf('out').store(i, S.vec4(l, l, l, S.f(255.0)))


@kernel("sc_final", WH + [('oq', 'i'), ('ext', 'i'), ('vq', 'i'), ('flags', 'u')], [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def sc_final(S):
    # in0 the stretched picture, in1 the grey (blurred, not yet rounded when flags & 1); table: 0 trim table, 256 tint tables (3 x 256), 1024 the vignette mask (w x h)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        st = S.buf('in0').load(i)
        gr = S.buf('in1').load(i)
        t = S.buf('table')
        a = st.w
        c255, zero = SF.const(S, 255.0), SF.const(S, 0.0)
        def gbyte(v):
            q = SF.from_f32(S, v) + 1.5e-8
            q = SF.sel(S, q < zero, zero, SF.sel(S, q > c255, c255, q))
            return S.select(S.ne(P.flags & 1, 0), sf_nearbyint(S, q), S.to_i(v))
        v = []
        for k, (d_f, g_f) in enumerate(((st.x, gr.x), (st.y, gr.y), (st.z, gr.z))):
            d = S.to_i(d_f)
            gv = gbyte(g_f)
            gv = S.select(S.ne(P.flags & 2, 0), S.to_i(t.load(S.to_u(gv))), gv)
            s = S.select(S.ne(P.flags & 4, 0), S.to_i(t.load(S.u(256 + 256 * k) + S.to_u(gv))), gv)
            ov = S.select(d < 128, (S.i(2) * d * s) / 255, S.i(255) - (S.i(2) * (S.i(255) - d) * (S.i(255) - s)) / 255)
            v.append((d * (S.i(255) - P.oq) + ov * P.oq) / 255)
        ext = P.ext
        hasx = S.ne(ext, 0)
        vm = t.load(S.u(1024) + S.select(hasx, S.to_u(i), S.u(0)))
        fl = (SF.from_f32(S, vm) * 255.0).floor_i32()
        M = S.min(S.max(S.i(255) - fl, S.i(0)), S.i(255))
        M = S.select(S.ne(P.flags & 8, 0), (S.i(2) * M * P.vq + S.i(255)) / 510, M)
        e = S.select(S.eq(ext, 3), S.i(255), S.i(0))          # exterior colour: 2 black, 3 white, 4 transparent (colour 0)
        ba = S.to_i(sf_byte(S, a))
        a4 = S.to_f((ba * (S.i(255) - M)) / 255) * S.f(1.0 / 255.0)
        oa = S.select(S.eq(ext, 4), a4, a)
        k255 = S.f(1.0 / 255.0)
        o = []
        for k in range(3):
            vk = S.select(hasx, (e * M + v[k] * (S.i(255) - M)) / 255, v[k])
            o.append(S.select(a <= 0.0, S.f(0.0), (S.to_f(vk) * k255) * oa))
        S.buf('out').store(i, S.vec4(o[0], o[1], o[2], S.select(a <= 0.0, S.f(0.0), oa)))


# ---------------------------------------------------------------------------------------------------------- Glint (glintVelvet() in builtins_velvet.cpp)
def _gl_sp(S, p, mode, mtab, i):
    """the source pixel, multiplied by the mask in mode 2; (px, mask value)"""
    m = mtab.load(S.select(S.ne(mode, 0), S.to_u(i), S.u(0)))
    f = S.select(S.eq(mode, 2), m, S.f(1.0))
    q = S.vec4(S.select(S.eq(mode, 2), p.x * f, p.x), S.select(S.eq(mode, 2), p.y * f, p.y), S.select(S.eq(mode, 2), p.z * f, p.z), S.select(S.eq(mode, 2), p.w * f, p.w))
    return q, m


@kernel("gl_g", WH + [('mode', 'u'), ('alpha', 'u')], [PIC_IN, PIC_OUT, TABLE])
def gl_g(S):
    # the byte luma (alpha: the byte alpha) of the (masked) picture, times the mask in mode 1; out.x
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        q, m = _gl_sp(S, S.buf('in0').load(i), P.mode, S.buf('table'), i)
        r, g, b = S.to_i(sf_byte(S, q.x)), S.to_i(sf_byte(S, q.y)), S.to_i(sf_byte(S, q.z))
        l = (S.i(19595) * r + S.i(38469) * g + S.i(7472) * b + S.i(32768)) >> 16
        ab = (SF.from_f32(S, q.w) * 255.0 + 0.5).floor_i32()
        l = S.select(S.ne(P.alpha, 0), S.min(S.max(ab, S.i(0)), S.i(255)), l)
        v1 = (SF.from_f32(S, S.to_f(l) * m) + 0.5).floor_i32()
        v = S.select(S.eq(P.mode, 1), v1, l)
        z = S.f(0.0)
        S.buf('out').store(i, S.vec4(S.to_f(v), z, z, z))


@kernel("gl_b3h", WH, [PIC_IN, PIC_OUT])
def gl_b3h(S):
    P = S.p
    with S.in_range(P.w, P.h):
        src = S.buf('in0')
        row = S.gy * P.w
        xl = S.select(S.gx > 0, S.gx - 1, S.u(0))
        xr = S.min(P.w - 1, S.gx + 1)
        v = src.load(row + xl).x + S.f(2.0) * src.load(row + S.gx).x + src.load(row + xr).x
        z = S.f(0.0)
        S.buf('out').store(row + S.gx, S.vec4(v, z, z, z))


@kernel("gl_b3v", WH, [PIC_IN, PIC_OUT])
def gl_b3v(S):
    P = S.p
    with S.in_range(P.w, P.h):
        src = S.buf('in0')
        yu = S.select(S.gy > 0, S.gy - 1, S.u(0))
        yd = S.min(P.h - 1, S.gy + 1)
        s = S.to_i(src.load(yu * P.w + S.gx).x) + S.i(2) * S.to_i(src.load(S.gy * P.w + S.gx).x) + S.to_i(src.load(yd * P.w + S.gx).x) + S.i(8)
        z = S.f(0.0)
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(S.to_f(s / 16), z, z, z))


@kernel("gl_plane", WH, [PIC_IN, PIC_OUT, TABLE])
def gl_plane(S):
    # a streak's plane: the threshold table of the byte
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        g = S.to_i(S.buf('in0').load(i).x)
        z = S.f(0.0)
        S.buf('out').store(i, S.vec4(S.buf('table').load(S.to_u(S.min(S.max(g, S.i(0)), S.i(255)))), z, z, z))


# ---------------------------------------------------------------------------------------------------------- Rays (raysVelvet() in builtins_velvet.cpp)
def _bil(S, tab, base, ix, fx, iy, fy, x0, x1, y0, y1, rowlen):
    """a = r0[x0] * (1 - fx) + r0[x1] * fx ...; the bilinear mix of four floats of a plane (tab or buffer loader `tab(index)`)"""
    one = S.f(1.0)
    a = tab(y0 * rowlen + x0) * (one - fx) + tab(y0 * rowlen + x1) * fx
    b = tab(y1 * rowlen + x0) * (one - fx) + tab(y1 * rowlen + x1) * fx
    return a * (one - fy) + b * fy


@kernel("ry_noise", WH, [PIC_IN, PIC_OUT, TABLE])
def ry_noise(S):
    # the 512 x 512 noise stretched over the frame (table: xi (w), xf (w), yi (h), yf (h), then the texture), multiplied into the plane
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        t = S.buf('table')
        xi = S.to_u(t.load(S.gx))
        xf = t.load(P.w + S.gx)
        yi = S.to_u(t.load(P.w * 2 + S.gy))
        yf = t.load(P.w * 2 + P.h + S.gy)
        base = P.w * 2 + P.h * 2
        x0, x1 = S.min(S.u(511), xi), S.min(S.u(511), xi + 1)
        y0, y1 = S.min(S.u(511), yi), S.min(S.u(511), yi + 1)
        z = _bil(S, lambda q: t.load(base + q), base, xi, xf, yi, yf, x0, x1, y0, y1, S.u(512))
        m = S.buf('in0').load(i).x
        zz = S.f(0.0)
        S.buf('out').store(i, S.vec4(m * z, zz, zz, zz))


@kernel("ry_pass", WH + [('w1', 'f'), ('w2', 'f')], [PIC_IN, PIC_OUT, TABLE])
def ry_pass(S):
    # one blur pass: the plane blended with a zoomed copy of itself (table: xi (w), xf (w), yi (h), yf (h))
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        t = S.buf('table')
        src = S.buf('in0')
        xi = S.to_u(t.load(S.gx))
        xf = t.load(P.w + S.gx)
        yi = S.to_u(t.load(P.w * 2 + S.gy))
        yf = t.load(P.w * 2 + P.h + S.gy)
        x0, x1 = S.min(P.w - 1, xi), S.min(P.w - 1, xi + 1)
        y0, y1 = S.min(P.h - 1, yi), S.min(P.h - 1, yi + 1)
        z = _bil(S, lambda q: src.load(q).x, 0, xi, xf, yi, yf, x0, x1, y0, y1, P.w)
        own = src.load(i).x
        zz = S.f(0.0)
        S.buf('out').store(i, S.vec4(own * P.w1 + z * P.w2, zz, zz, zz))


@kernel("ry_final", WH + [('mode', 'u'), ('eo', 'u')], [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def ry_final(S):
    # the picture (times the mask in mode 2, or nothing with Effect only) with the plane (in1.x) through the colour table (1024: 256 x A R G B), Screen; the mask follows the table
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        t = S.buf('table')
        m = S.buf('in1').load(i).x
        mk = t.load(S.u(1024) + S.select(S.ne(P.mode, 0), S.to_u(i), S.u(0)))
        f = S.select(S.eq(P.mode, 2), mk, S.f(1.0))
        p = S.buf('in0').load(i)
        e = S.ne(P.eo, 0)
        z = S.f(0.0)
        def base(v): return S.select(e, z, S.select(S.eq(P.mode, 2), v * f, v))
        o = [base(p.x), base(p.y), base(p.z), base(p.w)]
        v = S.floor(m + 0.5)
        v = S.min(S.max(v, 0.0), 255.0)
        mb = S.to_u(S.to_i(v))
        s = [S.to_i(t.load(mb * 4 + 1)), S.to_i(t.load(mb * 4 + 2)), S.to_i(t.load(mb * 4 + 3)), S.to_i(t.load(mb * 4))]
        d = [S.to_i(sf_byte(S, o[q])) for q in range(4)]
        k = S.f(1.0 / 255.0)
        r = [S.to_f(d[q] + s[q] - (d[q] * s[q]) / 255) * k for q in range(3)]
        a = S.to_f(d[3] + s[3] - (d[3] * s[3] + S.i(127)) / 255) * k
        S.buf('out').store(i, S.vec4(r[0], r[1], r[2], a))


# ---------------------------------------------------------------------------------------------------------- Scene Rotation / 360 Stabilization (rotatePanorama() in builtins_vegasrest.cpp)
@kernel("pano_pole", WH + [('pw', 'u'), ('ph', 'u'), ('px', 'u'), ('py', 'u'), ('part', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def pano_pole(S):
    # a pole row's average colour (gx 0 top, 1 bottom) of the part, summed in order as the CPU does: out[part * 2 + gx] = (r, g, b, a)
    P = S.p
    with S.if_(S.gx < 2):
        bot = S.ne(S.gx, 0)
        row = P.py + S.select(bot, P.ph - 1, S.u(0))
        src = S.buf('in0')
        pwf = S.to_f(P.pw)
        acc = [S.var('f', S.f(0.0)) for _ in range(4)]
        with S.for_(0, P.pw) as k:
            p = src.load(row * P.w + P.px + k)
            for q, v in enumerate((p.x, p.y, p.z, p.w)):
                acc[q].set(acc[q].get() + fdiv(S, v, pwf))
        S.buf('out').store(P.part * 2 + S.gx, S.vec4(acc[0].get(), acc[1].get(), acc[2].get(), acc[3].get()))


@kernel("pano_gather", WH + [('pw', 'u'), ('ph', 'u'), ('px', 'u'), ('py', 'u'), ('part', 'u'), ('pad', 'u')], [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def pano_gather(S):
    # in0 the frame, in1 the pole colours; table: x0, y0, tx, ty per output pixel of the part
    P = S.p
    with S.in_range(P.pw, P.ph):
        t = S.buf('table')
        src = S.buf('in0')
        pol = S.buf('in1')
        e = (S.gy * P.pw + S.gx) * 4
        x0 = S.to_i(t.load(e))
        y0 = S.to_i(t.load(e + 1))
        tx = t.load(e + 2)
        ty = t.load(e + 3)
        pwi, phi = S.to_i(P.pw), S.to_i(P.ph)
        def tap(xx, yy):
            # a row inside the picture is read from the frame (columns wrap), one beyond a pole from the pole colours
            inside = (yy >= 0) & (yy <= phi - 1)
            yc = S.select(inside, yy, S.i(0))
            wrapped = ((xx % pwi) + pwi) % pwi
            a = src.load((S.to_u(yc) + P.py) * P.w + P.px + S.to_u(wrapped))
            b = S.select(yy < 0, pol.load(P.part * 2), pol.load(P.part * 2 + 1))
            return S.vec4(S.select(inside, a.x, b.x), S.select(inside, a.y, b.y), S.select(inside, a.z, b.z), S.select(inside, a.w, b.w))
        p00, p10, p01, p11 = tap(x0, y0), tap(x0 + 1, y0), tap(x0, y0 + 1), tap(x0 + 1, y0 + 1)
        o = []
        for a, b, c, d in ((p00.x, p10.x, p01.x, p11.x), (p00.y, p10.y, p01.y, p11.y), (p00.z, p10.z, p01.z, p11.z), (p00.w, p10.w, p01.w, p11.w)):
            top = a + (b - a) * tx
            bot = c + (d - c) * tx
            o.append(top + (bot - top) * ty)
        S.buf('out').store((P.py + S.gy) * P.w + P.px + S.gx, S.vec4(o[0], o[1], o[2], o[3]))


@kernel("scene_gather", WH + [('mirror', 'u'), ('levels', 'u')], [PIC_IN, PIC_OUT, TABLE])
def scene_gather(S):
    # sceneRotateKernel() in builtins_vegasrest.cpp: the sampler's four texels (rows from the bottom, repeating or mirroring) and the weights; table: x0, y0, fx, fy per pixel
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        t = S.buf('table')
        src = S.buf('in0')
        x0, y0 = S.to_i(t.load(i * 4)), S.to_i(t.load(i * 4 + 1))
        fx, fy = t.load(i * 4 + 2), t.load(i * 4 + 3)
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        def wrap(v, n):
            plain = ((v % n) + n) % n
            n2 = n * 2
            m = ((v % n2) + n2) % n2
            mir = S.select(m < n, m, n2 - 1 - m)
            return S.select(S.ne(P.mirror, 0), mir, plain)
        def texel(xx, yy):
            return src.load(S.to_u(hi - 1 - wrap(yy, hi)) * P.w + S.to_u(wrap(xx, wi)))
        t00, t10, t01, t11 = texel(x0, y0), texel(x0 + 1, y0), texel(x0, y0 + 1), texel(x0 + 1, y0 + 1)
        one = S.f(1.0)
        o = []
        for a, b, c, d in ((t00.x, t10.x, t01.x, t11.x), (t00.y, t10.y, t01.y, t11.y), (t00.z, t10.z, t01.z, t11.z)):
            top = a * (one - fx) + b * fx
            bot = c * (one - fx) + d * fx
            r = top * (one - fy) + bot * fy
            r12 = S.to_i(S.floor(S.min(S.max(r, 0.0), 1.0) * 4096.0))
            rl = fdiv(S, S.to_f((r12 * S.i(255) + S.i(2047)) / 4096), S.f(255.0))
            o.append(S.select(S.ne(P.levels, 0), rl, r))
        S.buf('out').store(i, S.vec4(o[0], o[1], o[2], one))


# ---------------------------------------------------------------------------------------------------------- Light Rays (marchTowardsCentre() in builtins_fast2.cpp)
def _lerp4(S, a, b, t):
    """lerp(V4 a, V4 b, t) = a + (b - a) * t, per lane; a, b tuples"""
    return tuple(a[k] + (b[k] - a[k]) * t for k in range(4))


@kernel("lr_ray", WH + [('nT', 'u'), ('rN', 'u'), ('t0', 'u'), ('chunk', 'u'), ('cx', 'f'), ('cy', 'f'), ('sens', 'f'), ('pad', 'u')] + dpush('span', 'fmin'),
        [PIC_IN, (1, 'st', 'u', 'rw'), PIC_OUT, TABLE], local=(64, 1))
def lr_ray(S):
    """One ray a thread (marchTowardsCentre() with Light Rays' pixel function): the frame sampled along the ray (floats, stepping by the cosine and sine), the running
    sums of the samples in doubles, then every radius' value; out[ray * rN + r]. st: per ray of the chunk the samples (4 words each), then the two sums (6 words a position)."""
    P = S.p
    with S.if_(S.gx < P.chunk):
        t = P.t0 + S.gx
        with S.if_(t < P.nT):
            tab = S.buf('table')
            src = S.buf('in0')
            st = S.buf('st')
            out = S.buf('out')
            c = tab.load(t)
            sn = tab.load(P.nT + t)
            count = S.to_u(tab.load(P.nT * 2 + t))
            n = count + 8
            qw = (P.rN + 8) * 4
            sw = (P.rN + 9) * 6
            base = S.gx * (qw + sw * 2)
            s0b = base + qw
            s1b = s0b + sw
            wi, hi = S.to_i(P.w), S.to_i(P.h)
            mx, my = S.to_f(P.w - 1), S.to_f(P.h - 1)
            zero_f = S.f(0.0)
            # ---- the samples
            x = S.var('f', P.cx + S.f(-4.0) * c)
            y = S.var('f', P.cy + S.f(-4.0) * sn)
            with S.for_(0, n) as i:
                xc = S.select(x.get() < 0.0, zero_f, S.select(x.get() > mx, mx, x.get()))
                yc = S.select(y.get() < 0.0, zero_f, S.select(y.get() > my, my, y.get()))
                x0, y0 = S.to_i(xc), S.to_i(yc)
                fx, fy = xc - S.to_f(x0), yc - S.to_f(y0)
                x1 = S.select(x0 + 1 < wi, x0 + 1, x0)
                y1 = S.select(y0 + 1 < hi, y0 + 1, y0)
                def px(xx, yy):
                    v = src.load(S.to_u(yy) * P.w + S.to_u(xx))
                    return (v.x, v.y, v.z, v.w)
                top = _lerp4(S, px(x0, y0), px(x1, y0), fx)
                bot = _lerp4(S, px(x0, y1), px(x1, y1), fx)
                p = _lerp4(S, top, bot, fy)
                a = S.select(p[3] > 1e-4, fdiv(S, S.f(1.0), p[3]), zero_f)
                l = (S.f(0.2126) * (p[0] * a) + S.f(0.7152) * (p[1] * a)) + S.f(0.0722) * (p[2] * a)
                w = S.select(zero_f < l - P.sens, l - P.sens, zero_f)
                for k in range(3):
                    st.store(base + i * 4 + k, S.f_to_bits(p[k] * a * w))
                st.store(base + i * 4 + 3, S.f_to_bits(zero_f))
                x.set(x.get() + c)
                y.set(y.get() + sn)
            # ---- the running sums
            zero = SF.const(S, 0.0)
            for k in range(3):
                for b in (s0b, s1b):
                    st.store(b + k * 2, zero.hi)
                    st.store(b + k * 2 + 1, zero.lo)
            run0 = [SFVar(S, zero) for _ in range(3)]
            run1 = [SFVar(S, zero) for _ in range(3)]
            with S.for_(0, n) as j:
                r = SF.from_i32(S, S.to_i(j) - 4)
                for k in range(3):
                    qv = SF.from_f32(S, S.bits_to_f(st.load(base + j * 4 + k)))
                    n0 = run0[k].get() + qv
                    n1 = run1[k].get() + r * qv
                    run0[k].set(n0)
                    run1[k].set(n1)
                    pos = (j + 1) * 6 + k * 2
                    st.store(s0b + pos, n0.hi); st.store(s0b + pos + 1, n0.lo)
                    st.store(s1b + pos, n1.hi); st.store(s1b + pos + 1, n1.lo)
            # ---- the values
            span, fmin = pd(P, 'span'), pd(P, 'fmin')
            one, half = SF.const(S, 1.0), SF.const(S, 0.5)
            taps = SF.const(S, 40.0)
            def qf(j, k): return SF.from_f32(S, S.bits_to_f(st.load(base + S.to_u(j) * 4 + k)))
            def sget(b, j, k):
                pos = S.to_u(j) * 6 + k * 2
                return SF(S, st.load(b + pos), st.load(b + pos + 1))
            with S.for_(0, count) as r:
                dst = (t * P.rN + r)
                ri = S.to_i(r)
                vals = [S.var('f', zero_f) for _ in range(3)]
                with S.if_(ri <= 4):
                    rd = SF.from_i32(S, ri)
                    for k in range(3):
                        acc = SFVar(S, zero)
                        with S.for_(1, 41) as tt:
                            tq = SF.from_u32(S, tt) / taps
                            pos = SF.from_i32(S, S.i(4)) + rd * (one - tq * span)
                            j = S.max(S.i(0), S.min(S.to_i(n) - 2, pos.trunc_i32()))
                            f = pos - SF.from_i32(S, j)
                            v = qf(j, k) * (one - f) + qf(j + 1, k) * f
                            acc.set(acc.get() + v * (one - tq))
                        vals[k].set((acc.get() / taps).to_f32())
                with S.else_():
                    rd = SF.from_i32(S, ri)
                    a = (SF.from_i32(S, S.i(4)) + rd * fmin) + half
                    ja = a.trunc_i32()
                    jb = S.i(4) + ri
                    fa = a - SF.from_i32(S, ja)
                    fb = half
                    den = ((span * span) * rd) * rd
                    for k in range(3):
                        qa, qb = qf(ja, k), qf(jb, k)
                        i0 = (sget(s0b, jb, k) + fb * qb) - (sget(s0b, ja, k) + fa * qa)
                        i1 = (sget(s1b, jb, k) + (fb * SF.from_i32(S, jb - 4)) * qb) - (sget(s1b, ja, k) + (fa * SF.from_i32(S, ja - 4)) * qa)
                        vals[k].set((((i1 - (fmin * rd) * i0) / den) - qb / (SF.const(S, 2.0) * taps)).to_f32())
                out.store(dst, S.vec4(vals[0].get(), vals[1].get(), vals[2].get(), zero_f))


@kernel("lr_read", WH + [('nT', 'u'), ('rN', 'u'), ('tscale', 'f'), ('k0', 'f'), ('k1', 'f'), ('k2', 'f'), ('fb', 'f'), ('pad', 'u')] + dpush('px0', 'py0'), [PIC_IN, PIC_IN1, PIC_OUT])
def lr_read(S):
    # polarRun()'s read-back for Light Rays, then the final mix of lightRays(): in0 the frame, in1 the ray values
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        res = S.buf('in1')
        dx = ((SF.from_u32(S, S.gx) + 0.5) - pd(P, 'px0')).to_f32()
        dy = ((SF.from_u32(S, S.gy) + 0.5) - pd(P, 'py0')).to_f32()
        rr = fsqrt(S, dx * dx + dy * dy)
        rcap = S.to_f(P.rN - 2)
        r = S.select(rr < rcap, rr, rcap)
        # fastAtan2()
        ax, ay = S.abs(dx), S.abs(dy)
        mxv = S.select(ax < ay, ay, ax)
        mnv = S.select(ay < ax, ay, ax)
        q = fdiv(S, mnv, S.select(mxv <= 0.0, S.f(1.0), mxv))
        sq = q * q
        rv = q * (S.f(0.99986600) + sq * (S.f(-0.33029950) + sq * (S.f(0.18014100) + sq * (S.f(-0.08513300) + sq * S.f(0.02083510)))))
        rv = S.select(ay > ax, S.f(1.57079633) - rv, rv)
        rv = S.select(dx < 0.0, S.f(3.14159265) - rv, rv)
        ang = S.select(mxv <= 0.0, S.f(0.0), S.select(dy < 0.0, S.f(0.0) - rv, rv))
        ang = S.select(ang < 0.0, ang + S.f(6.28318531), ang)
        tf = ang * P.tscale
        t0i = S.min(S.to_u(S.to_i(tf)), P.nT - 1)
        ft = tf - S.to_f(t0i)
        t1i = S.select(t0i + 1 >= P.nT, S.u(0), t0i + 1)
        ri = S.to_u(S.to_i(r))
        fr = r - S.to_f(S.to_i(r))
        a0 = res.load(t0i * P.rN + ri)
        a0n = res.load(t0i * P.rN + ri + 1)
        a1 = res.load(t1i * P.rN + ri)
        a1n = res.load(t1i * P.rN + ri + 1)
        v0 = _lerp4(S, (a0.x, a0.y, a0.z, a0.w), (a0n.x, a0n.y, a0n.z, a0n.w), fr)
        v1 = _lerp4(S, (a1.x, a1.y, a1.z, a1.w), (a1n.x, a1n.y, a1n.z, a1n.w), fr)
        v = _lerp4(S, v0, v1, ft)
        one = S.f(1.0)
        def mix(sv, acc, kk):
            t = sv + acc * kk * P.fb
            return S.select(t < one, t, one)
        S.buf('out').store(i, S.vec4(mix(s.x, v[0], P.k0), mix(s.y, v[1], P.k1), mix(s.z, v[2], P.k2), s.w))


# ---------------------------------------------------------------------------------------------------------- Glint (glintVelvet() in builtins_velvet.cpp, byte planes)
# The streak planes live in one 'u' buffer, a word a byte, plane after plane.
@kernel("gl2_init", WH + [('n', 'u'), ('pad', 'u')], [PIC_IN, (2, 'out', 'u', 'rw'), TABLE])
def gl2_init(S):
    # every plane starts as the threshold table of the byte plane (in0.x); the grid is w x (h * n)
    P = S.p
    with S.in_range(P.w, P.h * P.n):
        plane = S.gy / P.h
        y = S.gy - plane * P.h
        g = S.to_i(S.buf('in0').load(y * P.w + S.gx).x)
        v = S.buf('table').load(S.to_u(S.min(S.max(g, S.i(0)), S.i(255))))
        S.buf('out').store((plane * P.h + y) * P.w + S.gx, S.to_u(v))


@kernel("gl2_shear", WH + [('lines', 'u'), ('len', 'u'), ('lineStep', 'u'), ('posStep', 'u'), ('dLineStep', 'u'), ('dPosStep', 'u'), ('rev', 'u'), ('side1', 'u'),
                           ('srcPlane', 'u'), ('dstPlane', 'u')], [(0, 'in0', 'u', 'rw'), (2, 'out', 'u', 'rw'), TABLE])
def gl2_shear(S):
    # shearLines() in builtins_velvet.cpp on every plane: w is the destination length, the grid w x (lines * planes); table: whole pixels and 12 bit weight a line
    P = S.p
    with S.in_range(P.w, P.h):
        plane = S.gy / P.lines
        k = S.gy - plane * P.lines
        t = S.buf('table')
        ip = S.to_i(t.load(k * 2))
        fr = S.to_i(t.load(k * 2 + 1))
        src = S.buf('in0')
        rev = S.ne(P.rev, 0)
        leni, dleni = S.to_i(P.len), S.to_i(P.w)
        x = S.to_i(S.gx)
        xr = S.select(rev, dleni - 1 - x, x)
        side1 = S.ne(P.side1, 0)
        a = S.select(side1, xr + ip, xr - ip)
        b = S.select(side1, a + 1, a - 1)
        def at(i):
            ic = S.min(S.max(i, S.i(0)), leni - 1)
            pos = S.select(rev, leni - 1 - ic, ic)
            return S.to_i(src.load(plane * P.srcPlane + k * P.lineStep + S.to_u(pos) * P.posStep))
        v = (at(a) * (S.i(4096) - fr) + at(b) * fr + S.i(2048)) >> 12
        S.buf('out').store(plane * P.dstPlane + k * P.dLineStep + S.gx * P.dPosStep, S.to_u(v))


@kernel("gl2_tof", WH + [('off', 'u'), ('pad', 'u')], [(0, 'in0', 'u', 'rw'), PIC_OUT])
def gl2_tof(S):
    # one plane (at word off, w x h) as floats in the x channel
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        z = S.f(0.0)
        S.buf('out').store(i, S.vec4(S.to_f(S.buf('in0').load(P.off + i)), z, z, z))


@kernel("gl2_fromf", WH + [('off', 'u'), ('scale', 'f')], [PIC_IN, (2, 'out', 'u', 'rw')])
def gl2_fromf(S):
    # cgBlurByte(f * scale) back into the plane
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        v = S.buf('in0').load(i).x * P.scale
        c255, zero = SF.const(S, 255.0), SF.const(S, 0.0)
        q = SF.from_f32(S, v) + 1.5e-8
        q = SF.sel(S, q < zero, zero, SF.sel(S, q > c255, c255, q))
        S.buf('out').store(P.off + i, S.to_u(sf_nearbyint(S, q)))


@kernel("gl2_final", WH + [('n', 'u'), ('mode', 'u'), ('eo', 'u'), ('pad', 'u')], [PIC_IN, (1, 'stk', 'u', 'rw'), PIC_OUT, TABLE])
def gl2_final(S):
    # the picture (times the mask in mode 2, nothing with Effect only) with every streak plane Screen-ed over it in order; table: 1024 colour table (256 x A R G B), then the mask
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        t = S.buf('table')
        stk = S.buf('stk')
        mk = t.load(S.u(1024) + S.select(S.ne(P.mode, 0), S.to_u(i), S.u(0)))
        f = S.select(S.eq(P.mode, 2), mk, S.f(1.0))
        p = S.buf('in0').load(i)
        e = S.ne(P.eo, 0)
        z = S.f(0.0)
        def base(v): return S.select(e, z, S.select(S.eq(P.mode, 2), v * f, v))
        o = [S.var('f', base(v)) for v in (p.x, p.y, p.z, p.w)]
        k255 = S.f(1.0 / 255.0)
        with S.for_(0, P.n) as pl:
            mb = stk.load(pl * P.w * P.h + i)
            s = [S.to_i(t.load(mb * 4 + 1)), S.to_i(t.load(mb * 4 + 2)), S.to_i(t.load(mb * 4 + 3)), S.to_i(t.load(mb * 4))]
            d = [S.to_i(sf_byte(S, o[q].get())) for q in range(4)]
            for q in range(3):
                o[q].set(S.to_f(d[q] + s[q] - (d[q] * s[q]) / 255) * k255)
            o[3].set(S.to_f(d[3] + s[3] - (d[3] * s[3] + S.i(127)) / 255) * k255)
        S.buf('out').store(i, S.vec4(o[0].get(), o[1].get(), o[2].get(), o[3].get()))


# ---------------------------------------------------------------------------------------------------------- Dual Fish Eye Stitching (dualFishEye() in builtins_vr.cpp)
def _f32(v):
    import struct
    return struct.unpack('f', struct.pack('f', v))[0]


@kernel("vr_stitch", WH + [('levels', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT, TABLE])
def vr_stitch(S):
    """Per pixel the CPU's 12 numbers (mode, front weight, then per lens x0 y0 fx fy gamma): the sampler's four texels, the vignetting removed with pow, the two lenses
    blended in YUV, the result cut to 12 bits for 8-bit pictures (KSample / shadeSample / stitchShade)."""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        t = S.buf('table')
        src = S.buf('in0')
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        one = S.f(1.0)
        def texel(xx, yy):
            cx = S.min(S.max(xx, 0), wi - 1)
            cy = S.min(S.max(yy, 0), hi - 1)
            return src.load(S.to_u(hi - 1 - cy) * P.w + S.to_u(cx))
        def shade(base):
            x0, y0 = S.to_i(t.load(base)), S.to_i(t.load(base + 1))
            fx, fy, gamma = t.load(base + 2), t.load(base + 3), t.load(base + 4)
            t00, t10, t01, t11 = texel(x0, y0), texel(x0 + 1, y0), texel(x0, y0 + 1), texel(x0 + 1, y0 + 1)
            o = []
            for a, b, c, d in ((t00.x, t10.x, t01.x, t11.x), (t00.y, t10.y, t01.y, t11.y), (t00.z, t10.z, t01.z, t11.z)):
                top = a * (one - fx) + b * fx
                bot = c * (one - fx) + d * fx
                r = top * (one - fy) + bot * fy
                q = S.select(r > 0.0, S.pow(r, gamma), S.f(0.0))
                q = S.select(q < 0.0, S.f(0.0), S.select(q > 1.0, one, q))
                o.append(S.select(S.ne(gamma, 1e30), q, r))
            return o
        def yuv(c):
            y0 = S.f(0.299) * c[0] + S.f(0.587) * c[1] + S.f(0.114) * c[2]
            y1 = S.f(0.436) * ((c[2] - y0) / S.f(_f32(1.0 - _f32(0.114))))
            y2 = S.f(0.615) * ((c[0] - y0) / S.f(_f32(1.0 - _f32(0.299))))
            return y0, y1, y2
        base = i * 12
        a = shade(base + 2)
        mode = t.load(base)
        col = S.var('v4', S.vec4(a[0], a[1], a[2], 0.0))
        with S.if_(mode > 0.5):
            b = shade(base + 7)
            yf, yb = yuv(a), yuv(b)
            dk = t.load(base + 1)
            bd = one - dk
            Y = yf[0] * dk + yb[0] * bd
            U = yf[1] * dk + yb[1] * bd
            V = yf[2] * dk + yb[2] * bd
            col.set(S.vec4(fmin(S, one, fmax(S, S.f(0.0), Y + S.f(1.14) * V)),
                           fmin(S, one, fmax(S, S.f(0.0), Y - S.f(0.395) * U - S.f(0.581) * V)),
                           fmin(S, one, fmax(S, S.f(0.0), Y + S.f(2.033) * U)), 0.0))
        c = col.get()
        o = []
        for k in range(3):
            r = S.extract(c, k)
            r12 = S.to_i(S.floor(S.min(S.max(r, 0.0), 1.0) * 4096.0))
            rl = fdiv(S, S.to_f((r12 * S.i(255) + S.i(2047)) / 4096), S.f(255.0))
            o.append(S.select(S.ne(P.levels, 0), rl, r))
        S.buf('out').store(i, S.vec4(o[0], o[1], o[2], one))


# ---------------------------------------------------------------------------------------------------------- Stereoscopic 3D Adjust (vegasStereo3dAdjust() in builtins_stereo.cpp)
@kernel("stereo3d", WH + [('full', 'u'), ('part', 'u')], [PIC_IN, PIC_OUT, TABLE])
def stereo3d(S):
    """The 8-bit pipeline: the picture cut to bytes, then per pixel the CPU's tap (kind, x0, x1, y0, y1, fx, fy, coverage): 1 / 2 VEGAS's stretch (plain and two-at-a-time
    bilinear, mixed in by the coverage), 3 OpenCV's warp; the floating window's columns; the bytes read back as /255 (tapValue() / vegasStereo3dAdjust())."""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        t = S.buf('table')
        src = S.buf('in0')
        wi, hi = S.to_i(P.w), S.to_i(P.h)
        base = i * 8
        kind = S.to_u(t.load(base))
        x0, x1 = S.to_i(t.load(base + 1)), S.to_i(t.load(base + 2))
        y0, y1 = S.to_i(t.load(base + 3)), S.to_i(t.load(base + 4))
        fx, fy = S.to_u(t.load(base + 5)), S.to_u(t.load(base + 6))
        cov = S.to_u(t.load(base + 7))
        def tex(xx, yy):
            ok = (xx >= 0) & (xx < wi) & (yy >= 0) & (yy < hi)
            cx = S.min(S.max(xx, 0), wi - 1)
            cy = S.min(S.max(yy, 0), hi - 1)
            return src.load(S.to_u(cy) * P.w + S.to_u(cx)), ok
        taps = [tex(x0, y0), tex(x1, y0), tex(x0, y1), tex(x1, y1)]
        def byte(v, ok, k):
            comp = (v.x, v.y, v.z, v.w)[k]
            b = S.to_u(S.round_even(S.min(S.max(comp, 0.0), 1.0) * 255.0))
            return S.select(ok, b, S.u(0))
        mn = lambda a, b: S.select(a < b, a, b)
        res = []
        for k in range(4):
            p00, p01, p10, p11 = [byte(v, ok, k) for v, ok in taps]
            # kind 1: 14-bit bilinear
            a16 = S.u(16384)
            w00, w01 = ((a16 - fy) * (a16 - fx)) >> 4, ((a16 - fy) * fx) >> 4
            w10, w11 = ((a16 - fx) * fy) >> 4, (fx * fy) >> 4
            v1 = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + S.u(0x800000)) >> 24
            # kind 2: the two-at-a-time sample
            A = ((S.u(0x3fff) - fy) << 2) & 0xffff
            B = (fy << 2) & 0xffff
            u0 = S.u(0x3fff) - fx
            m0, m1, m2, m3 = (u0 * A) >> 16, (fx * A) >> 16, (u0 * B) >> 16, (fx * B) >> 16
            tt = lambda p, w: (p * 32 * w) >> 16
            r = mn(tt(p10, m2) + 4, S.u(65535))
            r = mn(r + tt(p11, m3), S.u(65535))
            q = mn(tt(p01, m1) + tt(p00, m0), S.u(65535))
            r = mn(r + q, S.u(65535))
            v2 = mn(r >> 3, S.u(255))
            # kind 3: OpenCV's 5-bit weights (32768 a unit)
            s32 = S.u(32)
            c00, c01 = (s32 - fy) * (s32 - fx) * 32, (s32 - fy) * fx * 32
            c10, c11 = fy * (s32 - fx) * 32, fy * fx * 32
            v3 = (p00 * c00 + p01 * c01 + p10 * c10 + p11 * c11 + S.u(16384)) >> 15
            xx = S.select(S.eq(kind, 1), v1, v2) * cov + 128
            vm = mn((xx + (xx >> 8)) >> 8, S.u(255))
            val = S.select(S.eq(kind, 0), S.u(0), S.select(S.eq(kind, 3), v3, vm))
            res.append(val)
        # the floating window
        x = S.gx
        inwin = x < P.full
        onpart = S.eq(x, P.full) & (P.part > 0)
        o = []
        for k in range(4):
            v = res[k]
            if k < 3:
                pv = (v * (S.u(255) - P.part) + 127) / 255
            else:
                pv = (v * (S.u(255) - P.part) + 255 * P.part + 127) / 255
            v = S.select(onpart, pv, v)
            v = S.select(inwin, S.u(255) if k == 3 else S.u(0), v)
            o.append(fdiv(S, S.to_f(v), S.f(255.0)))
        S.buf('out').store(i, S.vec4(o[0], o[1], o[2], o[3]))
