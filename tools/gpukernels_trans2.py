"""The older (DirectX-era) VEGAS transitions that the CPU draws as VEGAS's own code does (src/ofx/vfxvenetian.h, vfxportals.h, vfxgradwipe.h, vfx3d.h): Venetian Blinds,
Portals, Gradient Wipe, the 3D transitions. The planners are in src/ofx/vktrans2.cpp. Imported by gpukernels.py (see its header).

Conventions as gpukernels_trans.py: the outgoing frame is buffer 0 (in0), the incoming one the first w * h * 4 floats of the table (to_px), top row first; what the
planner works out on the CPU (a profile, a layout, a list of rectangles - numbers that do not depend on the pictures) follows the frame in the table, integers as the
bits of a float (ti()).
"""
from gpukernels import kernel, PIC_IN, PIC_IN1, PIC_OUT, TABLE, WH, SF, SFVar, to_px, PLANE_IN, PLANE_IN1, PLANE_OUT
from gpukernels_trans import QW, qf, qi


def ti(S, tab, k):
    """the int stored as the bits of a float at table word k (k a uint)"""
    return S.to_i(S.f_to_bits(tab.load(k)))


def tf(S, tab, k):
    return tab.load(k)


# ---------------------------------------------------------------------------------------------------------- Venetian Blinds
@kernel("venetian", WH + [('levels', 'u'), ('n', 'i'), ('rowOff', 'u'), ('colOff', 'u'), ('tblOff', 'u')], [PIC_IN, PIC_OUT, TABLE])
def venetian(S):
    # venetian() in builtins_trans.cpp: the profile (alpha bytes, as floats, after the blur) and the position of each row / column (vfxvb::rowBase, the column's part of
    # positionAt; ints) are worked out by the planner; here the pixel's position is the row's less the column's, its alpha the profile read between two entries
    # (vfxvb::alphaAt), the picture the outgoing one plus (incoming - outgoing) x alpha, in whole levels (mix8) or in floats (mixF)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        tab = S.buf('table')
        pos = ti(S, tab, P.rowOff + S.gy) - ti(S, tab, P.colOff + S.gx)
        idx = (pos + ((pos >> 31) & 0x3fff)) >> 14
        fr = (((pos & 0x3fff) * 255) >> 14) & 0xff
        ok0 = (idx >= 0) & (idx < P.n)
        ok1 = (idx + 1 >= 0) & (idx + 1 < P.n)
        t0 = S.to_i(tab.load(P.tblOff + S.to_u(S.select(ok0, idx, S.i(0)))))
        t1 = S.to_i(tab.load(P.tblOff + S.to_u(S.select(ok1, idx + 1, S.i(0)))))
        v0 = S.select(ok0, t0, S.i(0))
        v1 = S.select(ok1, t1, S.i(0))
        d = (v1 - v0) * fr + 128
        r = (((d >> 8) + d) >> 8) + v0
        alpha = S.max(S.min(r, S.i(255)), S.i(0))
        pa = S.buf('in0').load(i)
        pb = to_px(S, i)
        wf = S.to_f(alpha) * S.f(0.003921568859368563)
        res = []
        for k in range(4):
            a, b = pa[k], pb[k]
            fa = S.select(S.isfinite(a), S.min(S.max(a, 0.0), 1.0), S.f(0.0))
            fb = S.select(S.isfinite(b), S.min(S.max(b, 0.0), 1.0), S.f(0.0))
            def lvl(v):
                x = v * 255.0
                n = S.to_i(x)
                return n + S.select((x - S.to_f(n)) >= 0.5, S.i(1), S.i(0))
            la, lb = lvl(fa), lvl(fb)
            dd = (lb - la) * alpha + 128
            m = S.max(S.min((((dd >> 8) + dd) >> 8) + la, S.i(255)), S.i(0))
            lv = S.to_f(m) * S.f(1.0 / 255.0)
            fl = (b - a) * wf + a
            res.append(S.select(S.ne(P.levels, 0), lv, fl))
        S.buf('out').store(i, S.vec4(res[0], res[1], res[2], res[3]))


# ---------------------------------------------------------------------------------------------------------- Portals
OPW = 20       # ints in an op of the list (vfxportals::Op as the planner writes it)


@kernel("portals", WH + [('nops', 'u'), ('opOff', 'u')], [PIC_IN, PIC_OUT, TABLE])
def portals(S):
    # vfxportals::render(): the picture is cut to bytes, every stretch of the list (the layout's rectangles and their borders, in drawing order) is applied to the
    # pixel when it lies in the stretch's rectangle: the stretch() of the header, one pixel at a time (the source row / column from the counters in closed form),
    # mixPixel() on whole levels, and the result comes back as levels / 255
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        tab = S.buf('table')
        x = S.to_i(S.gx)
        y = S.to_i(S.gy)
        W = S.to_i(P.w)
        d = [S.var('i', 0) for _ in range(4)]

        def cut(v):
            r = S.to_i(v * 255.0 + 0.5)
            return S.select(v <= 0.0, S.i(0), S.select(v >= 1.0, S.i(255), r))

        def src_px(src, sx, sy):
            """the four bytes of source 'src' at column sx, row sy (the single pixel's from the op)"""
            idx = sy * W + sx
            ui = S.to_u(S.max(S.min(idx, W * S.to_i(P.h) - 1), S.i(0)))
            pa = S.buf('in0').load(ui)
            pb = to_px(S, ui)
            return [S.select(S.eq(src, 0), cut(pa[k]), cut(pb[k])) for k in range(4)]

        with S.for_(S.u(0), P.nops) as k:
            o = P.opOff + k * OPW

            def g(n):
                return ti(S, tab, o + n)
            px0, px1, py0, py1 = g(2), g(3), g(4), g(5)
            with S.if_((x >= px0) & (x < px1) & (y >= py0) & (y < py1)):
                mix, src = g(0), g(1)
                firstRowCov, lastRowCov, firstColCov, lastColCov = g(6), g(7), g(8), g(9)
                sx0, sy0, xstep, ystep, step1, syStart = g(10), g(11), g(12), g(13), g(14), g(15)
                sw1, sh1, alpha14, cp = g(16), g(17), g(18), g(19)
                rows = py1 - py0
                # the row
                first_row = S.eq(y, py0)
                last_row = (rows >= 2) & S.eq(y, py1 - 1)
                syf = S.select(first_row, sy0, S.select(last_row, S.i(0), syStart + (y - py0 - 1) * ystep))
                row_cov = S.select(first_row, firstRowCov, S.select(last_row, lastRowCov, S.i(-1)))
                iy = S.select(last_row, sh1 - 1, syf >> 14)
                fy = S.select(last_row, S.i(0), syf & 0x3fff)
                # the column
                first_col = S.eq(x, px0)
                last_col = S.eq(x, px1 - 1)
                sxf = S.select(first_col, sx0, sx0 + step1 + (x - px0 - 1) * xstep)
                ix = sxf >> 14
                fx = sxf & 0x3fff
                flag = S.select(S.ne(ix, sw1 - 1), S.i(1), S.i(0))
                a = alpha14
                a = S.select(row_cov >= 0, (a * row_cov) >> 14, a)
                a = S.select(first_col, (a * firstColCov) >> 14, S.select(last_col, (a * lastColCov) >> 14, a))
                ix = S.select(first_col, ix, S.select(last_col, sw1 - 1, ix))
                flag = S.select(first_col, flag, S.select(last_col, S.i(0), flag))
                iy1 = S.select(S.eq(iy, sh1 - 1), iy, iy + 1)
                # the four taps (a single pixel source is the op's own bytes whatever the coordinates)
                sidx = S.select(S.eq(src, 2), S.i(0), src)
                t00 = src_px(sidx, ix, iy)
                t10 = src_px(sidx, ix + flag, iy)
                t01 = src_px(sidx, ix, iy1)
                t11 = src_px(sidx, ix + flag, iy1)
                is_c = S.eq(src, 2)
                cb = [(cp >> (8 * j)) & 255 for j in range(4)]
                taps = [[S.select(is_c, cb[j], t[j]) for j in range(4)] for t in (t00, t10, t01, t11)]

                def div14(v):
                    return (v + ((v >> 31) & 0x3fff)) >> 14
                w00 = div14((0x4000 - fx) * (0x4000 - fy))
                w10 = div14((0x4000 - fy) * fx)
                w01 = div14((0x4000 - fx) * fy)
                w11 = div14(fy * fx)
                s = [div14(taps[3][j] * w11 + taps[2][j] * w01 + taps[1][j] * w10 + taps[0][j] * w00) & 255 for j in range(4)]
                full = S.eq(a, 0x4000)
                dd = [v.get() for v in d]
                # Over: a premultiplied pixel over what is there
                r9f = 255 - s[3]
                over_full = []
                for j in range(4):
                    t = dd[j] * r9f + 128
                    t = (t + (t >> 8)) >> 8
                    t = t + s[j]
                    over_full.append(S.max(S.min(t, S.i(255)), S.i(0)))
                am = a * 255 + 0x2000
                m = ((am + ((am >> 31) & 0x3fff)) >> 14) & 255
                sa = s[3]
                r9 = 255 - (((sa * m + 0x7f) / 255) & 255)
                over_part = []
                for j in range(4):
                    sc = S.min(s[j], sa) if j < 3 else sa
                    t1 = (sc * m + 0x7f) / 255
                    t2 = (dd[j] * r9 + 0x7f) / 255
                    over_part.append((t1 + t2) & 255)
                # Lerp: the picture by its alpha
                a255 = a * 255
                a8 = ((a255 + ((a255 >> 31) & 0x3fff)) >> 14) & 0xffff
                a8 = S.select(a8 >= 0x8000, a8 - 0x10000, a8)
                lerp_part = []
                for j in range(4):
                    df = s[j] - dd[j]
                    pp = df * a8
                    pp = pp + S.select(pp < 0, S.i(255), S.i(0))
                    lerp_part.append((dd[j] + (pp >> 8)) & 255)
                is_over = S.ne(mix, 0)
                for j in range(4):
                    r = S.select(is_over, S.select(full, over_full[j], over_part[j]), S.select(full, s[j], lerp_part[j]))
                    d[j].set(r)
        S.buf('out').store(i, S.vec4(S.to_f(d[0].get()) * S.f(1.0 / 255.0), S.to_f(d[1].get()) * S.f(1.0 / 255.0), S.to_f(d[2].get()) * S.f(1.0 / 255.0), S.to_f(d[3].get()) * S.f(1.0 / 255.0)))


# ---------------------------------------------------------------------------------------------------------- Gradient Wipe
# gradientWipe() in builtins_vegasrest.cpp, VEGAS's own picture: the planner hands over the picture after the contrast table and its extension beyond the edges (the
# canvas: three bytes in each float of the table, as whole numbers), the byte table of the threshold (lut: 256 floats first) and the taps of the stretch
# (vgw::stretchMean) as the CPU works them out; the kernels do the threshold and the stretch to the frame, the mix is the last pass.
def canvas_byte(S, tab, off, idx):
    """byte idx of the canvas packed three to a float"""
    q = idx / 3
    r = idx % 3
    wv = S.to_u(tab.load(off + q))
    return (wv >> (r * 8)) & 255


@kernel("gw_whole", WH + [('cvOff', 'u'), ('cw', 'u'), ('ch', 'u'), ('n', 'u'), ('m', 'u'), ('ix', 'i'), ('iy', 'i')], [PLANE_OUT, TABLE])
def gw_whole(S):
    # stretchMean(), the whole-number case: the mean of the n x m mask bytes a frame pixel covers, rounded (sum + count / 2) / count
    P = S.p
    with S.in_range(P.w, P.h):
        tab = S.buf('table')
        acc = S.var('u', 0)
        with S.for_(S.u(0), P.m) as j:
            yy = S.max(S.min(P.iy + S.to_i(S.gy * P.m + j), S.to_i(P.ch) - 1), S.i(0))
            with S.for_(S.u(0), P.n) as i:
                xx = S.max(S.min(P.ix + S.to_i(S.gx * P.n + i), S.to_i(P.cw) - 1), S.i(0))
                b = canvas_byte(S, tab, P.cvOff, S.to_u(yy) * P.cw + S.to_u(xx))
                acc.set(acc.get() + S.to_u(tab.load(b)))
        cnt = P.n * P.m
        S.buf('out').store(S.gy * P.w + S.gx, S.to_f((acc.get() + cnt / 2) / cnt))


@kernel("gw_mid", WH + [('cvOff', 'u'), ('cw', 'u'), ('ch', 'u'), ('tx0', 'u'), ('ry0', 'i')], [PLANE_OUT, TABLE])
def gw_mid(S):
    # stretchMean(), the weighted case, across: for each source row ry0 + gy and frame column gx the float (double sum of weight x mask byte)
    P = S.p
    with S.in_range(P.w, P.h):
        tab = S.buf('table')
        first = S.to_i(tab.load(P.tx0 + S.gx))
        cnt = S.to_u(tab.load(P.tx0 + P.w + S.gx))
        off = S.to_u(tab.load(P.tx0 + 2 * P.w + S.gx))
        y = S.to_u(P.ry0 + S.to_i(S.gy))
        s = SFVar(S, 0.0)
        with S.for_(S.u(0), cnt) as k:
            j = S.to_u(S.max(S.min(first + S.to_i(k), S.to_i(P.cw) - 1), S.i(0)))
            b = S.to_u(tab.load(canvas_byte(S, tab, P.cvOff, y * P.cw + j)))
            wv = tab.load(P.tx0 + 3 * P.w + off + k)
            s.set(s.get() + SF.from_f32(S, wv) * SF.from_u32(S, b))
        S.buf('out').store(S.gy * P.w + S.gx, s.get().to_f32())


@kernel("gw_taps", WH + [('ty0', 'u'), ('ry0', 'i'), ('ry1', 'i')], [PLANE_IN, PLANE_OUT, TABLE])
def gw_taps(S):
    # stretchMean(), the weighted case, down: (int)ceil(sum of weight x mid - 0.5) kept in 0..255
    P = S.p
    with S.in_range(P.w, P.h):
        tab = S.buf('table')
        first = S.to_i(tab.load(P.ty0 + S.gy))
        cnt = S.to_u(tab.load(P.ty0 + P.h + S.gy))
        off = S.to_u(tab.load(P.ty0 + 2 * P.h + S.gy))
        s = SFVar(S, 0.0)
        with S.for_(S.u(0), cnt) as k:
            j = S.max(S.min(first + S.to_i(k), P.ry1), P.ry0)
            wv = tab.load(P.ty0 + 3 * P.h + off + k)
            mv = S.buf('in0').load(S.to_u(j - P.ry0) * P.w + S.gx)
            s.set(s.get() + SF.from_f32(S, wv) * SF.from_f32(S, mv))
        t = s.get() - 0.5
        v = (-((t.neg()).floor_i32()))
        v = S.max(S.min(v, S.i(255)), S.i(0))
        S.buf('out').store(S.gy * P.w + S.gx, S.to_f(v))


@kernel("gw_mix", WH + [('levels', 'u'), ('blurred', 'u'), ('hf', 'u'), ('vf', 'u'), ('hm', 'u'), ('vm', 'u')], [PIC_IN, PLANE_IN1, PIC_OUT, TABLE])
def gw_mix(S):
    # the mix of gradientWipe(): the mask pixel is the folded one (Mirror about the middle, then Flip); 8-bit pictures mix in 14-bit fixed point on levels
    # (vgw::weight14, vgw::mix8), float ones as a * w + b * (1 - w)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        def fold(v, n, mirror, flip):
            v = S.select(S.ne(mirror, 0) & (v > (n - 1) / 2), n - 1 - v, v)
            return S.select(S.ne(flip, 0), n - 1 - v, v)
        sx = fold(S.to_i(S.gx), S.to_i(P.w), P.hm, P.hf)
        sy = fold(S.to_i(S.gy), S.to_i(P.h), P.vm, P.vf)
        mf = S.buf('in1').load(S.to_u(sy) * P.w + S.to_u(sx))
        pa = S.buf('in0').load(i)
        pb = to_px(S, i)
        # the mask byte (a blurred mask is rounded: clampd(m + 0.5, 0, 255) cut to a whole number)
        xb = SF.from_f32(S, mf) + 0.5
        xb = SF.sel(S, xb < 0.0, SF.const(S, 0.0), SF.sel(S, xb > 255.0, SF.const(S, 255.0), xb))
        m = S.select(S.ne(P.blurred, 0), xb.trunc_i32(), S.to_i(mf))
        w14 = (m << 14) / 255
        wf = mf / 255.0
        res = []
        for k in range(4):
            a, b = pa[k], pb[k]
            a8 = S.to_i(a * 255.0 + 0.5)
            b8 = S.to_i(b * 255.0 + 0.5)
            lv = S.to_f((a8 * w14 + b8 * (16384 - w14) + 0x2000) >> 14) / 255.0
            fl = a * wf + b * (1.0 - wf)
            res.append(S.select(S.ne(P.levels, 0), lv, fl))
        S.buf('out').store(i, S.vec4(res[0], res[1], res[2], res[3]))


# Gradient Wipe without VEGAS's pictures (Reno's own gradients): the procedural gradient in doubles (wipeShape()), the blur and the contrast stretch on the float plane,
# the mix at the end.
@kernel("gw_proc", WH + [('shape', 'u'), ('hf', 'u'), ('vf', 'u'), ('hm', 'u'), ('vm', 'u'), ('keep', 'u'), ('q0', 'u'), ('q1', 'u')], [PLANE_OUT])
def gw_proc(S):
    # the gradient of gradientWipe(): (float)val for the pixel (gx, gy); q0 / q1 the double aspect (W / H when the aspect is kept)
    P = S.p
    with S.in_range(P.w, P.h):
        Wd, Hd = SF.from_u32(S, P.w), SF.from_u32(S, P.h)
        v = (SF.from_u32(S, S.gy) + 0.5) / Hd
        v = SF.sel(S, S.ne(P.vf, 0), SF.const(S, 1.0) - v, v)
        v = SF.sel(S, S.ne(P.vm, 0), SF.sel(S, v < 0.5, v * 2.0, (SF.const(S, 1.0) - v) * 2.0), v)
        u = (SF.from_u32(S, S.gx) + 0.5) / Wd
        u = SF.sel(S, S.ne(P.hf, 0), SF.const(S, 1.0) - u, u)
        u = SF.sel(S, S.ne(P.hm, 0), SF.sel(S, u < 0.5, u * 2.0, (SF.const(S, 1.0) - u) * 2.0), u)
        asp = SF(S, P.q0, P.q1)
        one = SF.const(S, 1.0)
        plain = SF.sel(S, S.ne(P.keep, 0), (u * asp + v) / (asp + 1.0), (u + v) * 0.5)
        val = SFVar(S, 0.0)
        sh = P.shape
        with S.if_(S.eq(sh, 0)):
            val.set(plain)
        with S.else_():
            du, dv = (u - 0.5).abs(), (v - 0.5).abs()
            mx = SF.sel(S, du < dv, dv, du)
            dx, dy = (u - 0.5) * Wd, (v - 0.5) * Hd
            rad = (dx * dx + dy * dy).sqrt()
            den = Wd * Wd + Hd * Hd
            half = den.sqrt() * 0.5
            den2 = SF.sel(S, half < 1e-9, SF.const(S, 1e-9), half)         # std::max(1e-9, 0.5 * sqrt(W W + H H))
            r = rad / den2
            horiz = S.eq(sh, 14) | S.eq(sh, 16)
            ab = SF.sel(S, horiz, v, u) * 8.0
            fl = ab.floor()
            fr = ab - fl
            odd = S.ne(ab.floor_i32() & 1, 0)
            bars = SF.sel(S, sh >= 16, (fr - 0.5).abs() * 2.0, SF.sel(S, odd, one - fr, fr))
            res = bars
            res = SF.sel(S, S.eq(sh, 13), one - mx * 2.0, res)
            res = SF.sel(S, S.eq(sh, 12), mx * 2.0, res)
            res = SF.sel(S, S.eq(sh, 11), one - r, res)
            res = SF.sel(S, S.eq(sh, 10), r, res)
            res = SF.sel(S, S.eq(sh, 9), dv * 2.0, res)
            res = SF.sel(S, S.eq(sh, 8), du * 2.0, res)
            res = SF.sel(S, S.eq(sh, 7), ((one - u) + (one - v)) * 0.5, res)
            res = SF.sel(S, S.eq(sh, 6), (u + (one - v)) * 0.5, res)
            res = SF.sel(S, S.eq(sh, 5), ((one - u) + v) * 0.5, res)
            res = SF.sel(S, S.eq(sh, 4), one - v, res)
            res = SF.sel(S, S.eq(sh, 3), v, res)
            res = SF.sel(S, S.eq(sh, 2), one - u, res)
            res = SF.sel(S, S.eq(sh, 1), u, res)
            val.set(res)
        S.buf('out').store(S.gy * P.w + S.gx, val.get().to_f32())


@kernel("gw_rowmm", WH + [('pw', 'u')], [PLANE_IN, PLANE_OUT])
def gw_rowmm(S):
    # w = 1, h = rows: out[gy * 2], out[gy * 2 + 1] the row's smallest / largest value (a row of pw values)
    P = S.p
    with S.in_range(P.w, P.h):
        lo = S.var('f', 1e9)
        hi = S.var('f', -1e9)
        with S.for_(S.u(0), P.pw) as x:
            v = S.buf('in0').load(S.gy * P.pw + x)
            lo.set(S.select(v < lo.get(), v, lo.get()))          # std::min(lo, v)
            hi.set(S.select(hi.get() < v, v, hi.get()))          # std::max(hi, v)
        S.buf('out').store(S.gy * 2, lo.get())
        S.buf('out').store(S.gy * 2 + 1, hi.get())


@kernel("gw_mm", WH, [PLANE_IN, PLANE_OUT])
def gw_mm(S):
    # one thread: the smallest / largest of the rows' pairs (w = 1, h = 1; P.w holds the rows through the pair table's length)
    P = S.p
    with S.in_range(P.w, P.h):
        lo = S.var('f', 1e9)
        hi = S.var('f', -1e9)
        with S.for_(S.u(0), P.w) as y:
            a = S.buf('in0').load(y * 2)
            b = S.buf('in0').load(y * 2 + 1)
            lo.set(S.select(a < lo.get(), a, lo.get()))
            hi.set(S.select(hi.get() < b, b, hi.get()))
        S.buf('out').store(0, lo.get())
        S.buf('out').store(1, hi.get())


@kernel("gw_norm", WH, [PLANE_IN, PLANE_IN1, PLANE_OUT])
def gw_norm(S):
    # the contrast stretch of gradientWipe(): (v - lo) / (hi - lo) when hi - lo > 1e-6
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        lo = S.buf('in1').load(S.u(0))
        hi = S.buf('in1').load(S.u(1))
        v = S.buf('in0').load(i)
        d = hi - lo
        S.buf('out').store(i, S.select(d > 1e-6, (v - lo) / d, v))


@kernel("gw_pmix", WH + [('reverse', 'u'), ('soft', 'u'), ('q0', 'u'), ('q1', 'u'), ('q2', 'u'), ('q3', 'u')], [PIC_IN, PLANE_IN1, PIC_OUT, TABLE])
def gw_pmix(S):
    # the last loop of gradientWipe() without VEGAS's pictures: t from the double progress (q0, q1) and blend (q2, q3), a + (b - a) * (float)t
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        pg = SF(S, P.q0, P.q1)
        bl = SF(S, P.q2, P.q3)
        v = SF.from_f32(S, S.buf('in1').load(i))
        v = SF.sel(S, S.ne(P.reverse, 0), SF.const(S, 1.0) - v, v)
        x = (pg * (bl + 1.0) - v) / bl
        x = SF.sel(S, x < 0.0, SF.const(S, 0.0), SF.sel(S, x > 1.0, SF.const(S, 1.0), x))
        hard = SF.sel(S, pg > v, SF.const(S, 1.0), SF.const(S, 0.0))
        tf = SF.sel(S, S.ne(P.soft, 0), x, hard).to_f32()
        pa = S.buf('in0').load(i)
        pb = to_px(S, i)
        S.buf('out').store(i, S.vec4(*[pa[k] + (pb[k] - pa[k]) * tf for k in range(4)]))
