"""Kernels of the text effects (Titles & Text, Credit Roll, Timecode): the per-pixel passes that follow the glyph rasterisation, which stays on the CPU (GDI/GDI+ on Windows, Reno's own
rasteriser and the application's font source elsewhere). The CPU code is src/ofx/builtins_gen.cpp (titlesAndText, creditRoll), vfxtitles.h / vfxtitlerich.h (blurLayer, glowLayer, dropShadow),
vfxcredit.h and builtins_more_creative.cpp (timecodeFx); each kernel says which function it mirrors. The planners are in src/ofx/vktext.cpp.
tools/gpukernels.py loads this file; see its header for the conventions. A text layer goes up as a table of w * h * 4 floats holding the layer's BYTES (b g r a, rows top first)."""
from gpukernels import kernel, PIC_IN, PIC_IN1, PIC_OUT, TABLE, WH, dpush, pd
from sf64 import SF


# ---------------------------------------------------------------------------------------------------------- Timecode (timecodeFx)
@kernel("tc_box", WH + [('ox', 'i'), ('oy', 'i'), ('wb', 'u'), ('hb', 'u')], [PIC_IN, PIC_OUT, TABLE])
def tc_box(S):
    """timecodeFx: inside the bitmap's rectangle (one row and column in from its origin) the pixel is (g, g, g, 1) with g the bitmap's byte / 255, elsewhere the picture.
    The table holds the bitmap's bytes (wb x hb, row 0 at the bottom of the picture)."""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        src = S.buf('in0').load(i)
        y = S.to_i(P.h) - 1 - S.to_i(S.gy)                  # the picture's row counted from the bottom
        ry = y - P.oy
        rx = S.to_i(S.gx) - P.ox
        inside = (ry > 0) & (ry < S.to_i(P.hb)) & (rx > 0) & (rx < S.to_i(P.wb))
        idx = S.select(inside, S.to_u(ry) * P.wb + S.to_u(rx), S.u(0))
        g = S.buf('table').load(idx) / S.f(255.0)
        S.buf('out').store(i, S.select(inside, S.vec4(g, g, g, S.f(1.0)), src))


# ---------------------------------------------------------------------------------------------------------- Titles & Text (the layer's own passes)
@kernel("tt_load", WH, [PIC_OUT, TABLE])
def tt_load(S):
    """the layer's bytes (table: b g r a floats, rows top first) into a picture buffer, lanes as they are"""
    with S.in_range(S.p.w, S.p.h):
        i = S.gy * S.p.w + S.gx
        t = S.buf('table')
        b = i * 4
        S.buf('out').store(i, S.vec4(t.load(b), t.load(b + 1), t.load(b + 2), t.load(b + 3)))


@kernel("tt_round", WH, [PIC_IN, PIC_OUT])
def tt_round(S):
    """blurLayer's write back: (int)floor(v + 0.5f) held to 0..255, per lane"""
    with S.in_range(S.p.w, S.p.h):
        i = S.gy * S.p.w + S.gx
        p = S.buf('in0').load(i)
        def r(v): return S.max(S.min(S.floor(v + S.f(0.5)), S.f(255.0)), S.f(0.0))
        S.buf('out').store(i, S.vec4(r(p.x), r(p.y), r(p.z), r(p.w)))


@kernel("tt_glow", WH + [('inner', 'u'), ('pad', 'u')] + dpush('ca', 'c0', 'c1', 'c2'), [PIC_IN, PIC_IN1, PIC_OUT])
def tt_glow(S):
    """glowLayer (vfxtitlerich.h), the part after the blur: in0 the layer's bytes, in1 the blurred alpha (all lanes). t = the blurred alpha rounded to a byte / 255, the glow colour
    (c0 c1 c2 = b g r, already cut to 0..1 and kept as the CPU's doubles) x t x ca put under the layer (over it for an inner glow); doubles in tools/sf64.py"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        d = S.buf('in0').load(i)
        bl = S.buf('in1').load(i).w
        ca = pd(P, 'ca')
        t = (SF.from_f32(S, bl) + 0.5).floor()
        t = t.max(0.0).min(255.0) / 255.0
        ga = t * ca
        dd = [SF.from_f32(S, d.x), SF.from_f32(S, d.y), SF.from_f32(S, d.z), SF.from_f32(S, d.w)]
        da = dd[3] / 255.0
        ida = SF.of(S, 1.0) - da
        ig = SF.of(S, 1.0) - ga
        inner = S.ne(P.inner, 0)
        outs = []
        for c in range(4):
            g = ga if c == 3 else t * pd(P, 'c%d' % c) * ca
            o_out = (dd[c] + g * ida * 255.0 + 0.5).floor().min(255.0)
            o_in = (g * 255.0 + dd[c] * ig + 0.5).floor().min(255.0)
            v = SF.sel(S, inner, o_in, o_out).to_f32()
            outs.append(v)
        keep = t <= 0.0
        res = S.vec4(S.select(keep, d.x, outs[0]), S.select(keep, d.y, outs[1]), S.select(keep, d.z, outs[2]), S.select(keep, d.w, outs[3]))
        S.buf('out').store(i, res)


@kernel("tt_sbyte", WH, [PIC_IN, PIC_OUT])
def tt_sbyte(S):
    """dropShadow: cgBlurByte of the blurred alpha (lane w): the nearest whole number (ties to even) of v + 1.5e-8 in doubles, held to 0..255; in all lanes"""
    with S.in_range(S.p.w, S.p.h):
        i = S.gy * S.p.w + S.gx
        v = S.buf('in0').load(i).w
        x = SF.from_f32(S, v) + 1.5e-8
        big = S.f(1.0e6)
        vc = S.max(S.min(v, big), S.f(-1.0e6))
        xs = SF.from_f32(S, vc) + 1.5e-8
        fl = xs.floor_i32()
        fr = xs - SF.from_i32(S, fl)
        odd = S.ne(fl & 1, 0)
        up = (fr > 0.5) | (fr.eq(0.5) & odd)
        r = fl + S.select(up, S.i(1), S.i(0))
        r = S.max(S.min(r, S.i(255)), S.i(0))
        r = S.select(S.isnan(v), S.i(0), r)
        f = S.to_f(r)
        S.buf('out').store(i, S.vec4(f, f, f, f))


@kernel("tt_shadow", WH + [('pad', 'u'), ('pad2', 'u'), ('s0', 'f'), ('s1', 'f'), ('s2', 'f'), ('s3', 'f')] + dpush('ox', 'oy'), [PIC_IN, PIC_IN1, PIC_OUT])
def tt_shadow(S):
    """dropShadow (vfxtitles.h), the part after the blur: in0 the layer's bytes (b g r a), in1 the blurred alpha as bytes. The shadow picture sampled bilinearly at (x - ox, y - oy) in doubles, times
    the premultiplied shadow colour (s0 s1 s2 s3 = the colour per byte lane b g r a), rounded to a byte and added under the layer (mul255 of the layer's missing alpha)."""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        d = S.buf('in0').load(i)
        ab = S.buf('in1')
        W, H = S.to_i(P.w), S.to_i(P.h)
        fx = SF.from_i32(S, S.to_i(S.gx)) - pd(P, 'ox')
        fy = SF.from_i32(S, S.to_i(S.gy)) - pd(P, 'oy')
        ix, iy = fx.floor_i32(), fy.floor_i32()
        tx = fx - SF.from_i32(S, ix)
        ty = fy - SF.from_i32(S, iy)
        zero = SF.of(S, 0.0)
        def at(xx, yy):
            ok = (xx >= 0) & (yy >= 0) & (xx < W) & (yy < H)
            idx = S.select(ok, S.to_u(yy * W + xx), S.u(0))
            return SF.sel(S, ok, SF.from_f32(S, ab.load(idx).x), zero)
        one = SF.of(S, 1.0)
        a00, a10, a01, a11 = at(ix, iy), at(ix + 1, iy), at(ix, iy + 1), at(ix + 1, iy + 1)
        v = (a00 * (one - tx) + a10 * tx) * (one - ty) + (a01 * (one - tx) + a11 * tx) * ty
        v = v / 255.0
        ia = 255 - S.to_i(d.w)
        def mul255(a, b):
            t = a * b + 128
            return (t + (t >> 8)) >> 8
        outs = []
        for k, (cv, dv) in enumerate(((P.s0, d.x), (P.s1, d.y), (P.s2, d.z), (P.s3, d.w))):
            sv = v * SF.from_f32(S, cv)
            s = (sv * 255.0 + 0.5).floor_i32()
            r = S.min(S.to_i(dv) + mul255(s, ia), S.i(255))
            outs.append(S.to_f(r))
        S.buf('out').store(i, S.vec4(outs[0], outs[1], outs[2], outs[3]))


@kernel("tt_comp", WH + [('op', 'f'), ('b0', 'f'), ('b1', 'f'), ('b2', 'f'), ('b3', 'f'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def tt_comp(S):
    """the layer (bytes b g r a) over the background (premultiplied floats) at opacity op, as titlesAndText does it: a = alpha/255 x op; colour/255 x op + background x (1 - a)"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        a = s.w / S.f(255.0) * P.op
        ia = S.f(1.0) - a
        r = s.z / S.f(255.0) * P.op + P.b0 * ia
        g = s.y / S.f(255.0) * P.op + P.b1 * ia
        b = s.x / S.f(255.0) * P.op + P.b2 * ia
        al = a + P.b3 * ia
        skip = s.w <= S.f(0.0)
        S.buf('out').store(i, S.select(skip, S.vec4(P.b0, P.b1, P.b2, P.b3), S.vec4(r, g, b, al)))


# ---------------------------------------------------------------------------------------------------------- Credit Roll (creditPlay in builtins_gen.cpp)
@kernel("cr_blit", WH + [('np', 'u'), ('pad', 'u'), ('b0', 'f'), ('b1', 'f'), ('b2', 'f'), ('b3', 'f')], [PIC_OUT, TABLE])
def cr_blit(S):
    """creditBlit for every recorded blit in turn, per pixel: the table holds np records of 16 words (x0 y0 w h clipL clipT clipR clipB as ints, the colour r g b a, the mask's offset, 3 spare) and then
    the masks' coverage bytes; coverage x (1/255) x the colour's alpha is the weight v, colour x v + picture x (1 - v), v + alpha x (1 - v). The picture starts as the background."""
    P = S.p
    with S.in_range(P.w, P.h):
        t = S.buf('table')
        x, y = S.to_i(S.gx), S.to_i(S.gy)
        d = [S.var('f', P.b0), S.var('f', P.b1), S.var('f', P.b2), S.var('f', P.b3)]
        with S.for_(0, P.np) as n:
            base = n * 16
            def ri(k): return S.to_i(S.f_to_bits(t.load(base + k)))
            x0, y0, w, h = ri(0), ri(1), ri(2), ri(3)
            inside = (x >= x0) & (x < x0 + w) & (y >= y0) & (y < y0 + h) & (x >= ri(4)) & (x < ri(6)) & (y >= ri(5)) & (y < ri(7))
            off = S.f_to_bits(t.load(base + 12))
            idx = S.select(inside, off + S.to_u((y - y0) * w + (x - x0)), S.u(0))
            cov = t.load(idx)
            v = cov * S.f(1.0 / 255.0) * t.load(base + 11)
            hit = inside & (v > 0.0)
            k = S.f(1.0) - v
            n0 = t.load(base + 8) * v + d[0].get() * k
            n1 = t.load(base + 9) * v + d[1].get() * k
            n2 = t.load(base + 10) * v + d[2].get() * k
            n3 = v + d[3].get() * k
            d[0].set(S.select(hit, n0, d[0].get()))
            d[1].set(S.select(hit, n1, d[1].get()))
            d[2].set(S.select(hit, n2, d[2].get()))
            d[3].set(S.select(hit, n3, d[3].get()))
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(d[0].get(), d[1].get(), d[2].get(), d[3].get()))


@kernel("cr_wipe", WH + [('x0', 'i'), ('y0', 'i'), ('x1', 'i'), ('y1', 'i'), ('vert', 'u'), ('clear', 'u'), ('a0', 'i'), ('a1', 'i'), ('hw', 'i'), ('cx', 'i'), ('b0', 'f'), ('b1', 'f'), ('b2', 'f'), ('b3', 'f')],
        [PIC_IN, PIC_OUT])
def cr_wipe(S):
    """vfxcredit::wipe: inside the box, along the wipe's axis u: the side the edge has passed (clear 1: before it) becomes the background, the other stays, in the edge's band
    (a0 .. a1) the picture and the background are mixed by the integer weight a = (u - (cx - hw)) x 255 / (2 hw) held to 0..255"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        x, y = S.to_i(S.gx), S.to_i(S.gy)
        inside = (x >= P.x0) & (x < P.x1) & (y >= P.y0) & (y < P.y1)
        vert = S.ne(P.vert, 0)
        clear = S.ne(P.clear, 0)
        u = S.select(vert, y, x)
        bg = S.vec4(P.b0, P.b1, P.b2, P.b3)
        before = u < P.a0
        after = u >= P.a1
        to_bg = S.select(clear, before, after)
        keep = S.select(clear, after, before)
        hw2 = S.max(2 * P.hw, S.i(1))
        a = S.select(P.hw > 0, ((u - (P.cx - P.hw)) * 255) / hw2, S.i(255))
        a = S.max(S.min(a, S.i(255)), S.i(0))
        w = S.to_f(S.select(clear, a, 255 - a)) / S.f(255.0)
        mixed = S.vec4(P.b0 + (p.x - P.b0) * w, P.b1 + (p.y - P.b1) * w, P.b2 + (p.z - P.b2) * w, P.b3 + (p.w - P.b3) * w)
        res = S.select(to_bg, bg, S.select(keep, p, mixed))
        S.buf('out').store(i, S.select(inside, res, p))


# ---------------------------------------------------------------------------------------------------------- Credit Roll's zoom (vfxcredit::zoomBox, VEGAS's 8-bit resampler)
@kernel("cr_zq", WH, [PIC_IN, PIC_OUT])
def cr_zq(S):
    """zoomBox's first step: the picture's bytes, (int)(v x 255 + 0.5f) held to 0..255, as whole numbers in the lanes"""
    with S.in_range(S.p.w, S.p.h):
        i = S.gy * S.p.w + S.gx
        p = S.buf('in0').load(i)
        def q(v): return S.to_f(S.max(S.min(S.to_i(v * S.f(255.0) + S.f(0.5)), S.i(255)), S.i(0)))
        S.buf('out').store(i, S.vec4(q(p.x), q(p.y), q(p.z), q(p.w)))


@kernel("cr_zshrink", WH + [('bx0', 'i'), ('by0', 'i'), ('bw', 'i'), ('bh', 'i'), ('ix0', 'i'), ('iy0', 'i'), ('iw', 'i'), ('ih', 'i'), ('stepx', 'i'), ('stepy', 'i')], [PIC_IN, PIC_OUT, TABLE])
def cr_zshrink(S):
    """vfxcredit::cubicShrink: the box's bytes (in0, at the frame's coordinates) brought down to iw x ih with VEGAS's cubic (table: 16384 phases x 4 weights): first along the columns (s / 16384,
    not clamped), then along the rows (clamped to 0..255). The result is written at the inner rectangle's place in the frame."""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        x, y = S.to_i(S.gx) - P.ix0, S.to_i(S.gy) - P.iy0
        inside = (x >= 0) & (y >= 0) & (x < P.iw) & (y < P.ih)
        src = S.buf('in0')
        t = S.buf('table')
        posy = 0x2000 + y * P.stepy
        posx = 0x2000 + x * P.stepx
        wy = [S.to_i(t.load(S.to_u(S.select(inside, posy & 0x3fff, S.i(0))) * 4 + k)) for k in range(4)]
        wx = [S.to_i(t.load(S.to_u(S.select(inside, posx & 0x3fff, S.i(0))) * 4 + k)) for k in range(4)]
        iy = posy >> 14
        ix = posx >> 14
        def clampi(v, n): return S.select(v < 0, S.i(0), S.select(v >= n, n - 1, v))
        rows = [clampi(iy - 2 + k, P.bh) for k in range(4)]
        outc = []
        cols = []
        for k in range(4):
            cx = clampi(ix - 2 + k, P.bw)
            lane = []
            for c in range(4):
                s = S.i(0x2000)
                for j in range(4):
                    px = src.load(S.to_u(S.select(inside, (P.by0 + rows[j]) * S.to_i(P.w) + P.bx0 + cx, S.i(0))))
                    s = s + S.to_i((px.x, px.y, px.z, px.w)[c]) * wy[j]
                lane.append(s / 16384)
            cols.append(lane)
        for c in range(4):
            s = cols[0][c] * wx[0] + cols[1][c] * wx[1] + cols[2][c] * wx[2] + cols[3][c] * wx[3] + 0x2000
            o = s / 16384
            outc.append(S.to_f(S.max(S.min(o, S.i(255)), S.i(0))))
        S.buf('out').store(i, S.vec4(outc[0], outc[1], outc[2], outc[3]))


@kernel("cr_zsmooth", WH + [('ix0', 'i'), ('iy0', 'i'), ('iw', 'i'), ('ih', 'i'), ('ka', 'i'), ('kb', 'i'), ('kc', 'i'), ('tot', 'i')], [PIC_IN, PIC_OUT])
def cr_zsmooth(S):
    """vfxcredit::smooth3 on the shrunk picture (in0, at the inner rectangle): corners x A, edges x B, the middle x C, divided by their sum; the edge pixels use their own neighbours twice"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        x, y = S.to_i(S.gx) - P.ix0, S.to_i(S.gy) - P.iy0
        inside = (x >= 0) & (y >= 0) & (x < P.iw) & (y < P.ih)
        src = S.buf('in0')
        y0 = S.select(y > 0, y - 1, y)
        y1 = S.select(y + 1 < P.ih, y + 1, y)
        x0 = S.select(x > 0, x - 1, x)
        x1 = S.select(x + 1 < P.iw, x + 1, x)
        W = S.to_i(P.w)
        def g(yy, xx):
            return src.load(S.to_u(S.select(inside, (P.iy0 + yy) * W + P.ix0 + xx, S.i(0))))
        n = [g(y0, x0), g(y0, x1), g(y1, x0), g(y1, x1), g(y0, x), g(y1, x), g(y, x0), g(y, x1), g(y, x)]
        outc = []
        for c in range(4):
            v = [S.to_i((q.x, q.y, q.z, q.w)[c]) for q in n]
            s = P.ka * (v[0] + v[1] + v[2] + v[3]) + P.kb * (v[4] + v[5] + v[6] + v[7]) + P.kc * v[8]
            o = s / S.max(P.tot, S.i(1))
            outc.append(S.to_f(S.max(S.min(o, S.i(255)), S.i(0))))
        S.buf('out').store(i, S.vec4(outc[0], outc[1], outc[2], outc[3]))


@kernel("cr_zfinal", WH + [('x0', 'i'), ('y0', 'i'), ('x1', 'i'), ('y1', 'i'), ('ix0', 'i'), ('iy0', 'i'), ('ix1', 'i'), ('iy1', 'i'), ('inner', 'u'), ('b0', 'f'), ('b1', 'f'), ('b2', 'f'), ('b3', 'f')],
        [PIC_IN, PIC_IN1, PIC_OUT])
def cr_zfinal(S):
    """zoomBox's last step: in the box, the shrunk picture (in1) / 255 inside the inner rectangle and the background round it; the picture (in0) outside the box"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        x, y = S.to_i(S.gx), S.to_i(S.gy)
        inbox = (x >= P.x0) & (x < P.x1) & (y >= P.y0) & (y < P.y1)
        ini = S.ne(P.inner, 0) & (x >= P.ix0) & (x < P.ix1) & (y >= P.iy0) & (y < P.iy1)
        o = S.buf('in1').load(i)
        zoomed = S.vec4(o.x / S.f(255.0), o.y / S.f(255.0), o.z / S.f(255.0), o.w / S.f(255.0))
        bg = S.vec4(P.b0, P.b1, P.b2, P.b3)
        S.buf('out').store(i, S.select(inbox, S.select(ini, zoomed, bg), p))
