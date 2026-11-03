"""The four 3D transitions (3D Shuffle, 3D Blinds, 3D Fly In/Out, 3D Cascade), drawn as VEGAS's own rasteriser does (src/ofx/vfx3d.h, the front ends vfx3dfx.h). The planner is
src/ofx/vk3d.cpp: it works out the polygons, their edge tables and, row by row, the strips each row is cut into (vfx3d.h's own functions, so the sorting and the
fixed-point set-up are the CPU's), and the kernels do the per-pixel part. Imported by gpukernels.py (see its header).

The CPU draws on an 8-bit surface: here the surface is a buffer of floats holding whole levels (0..255) and the last pass scales it to levels / 255.
 d3_lev       the two pictures as levels (A first, B below it), into one buffer of w x 2h
 d3_init      the surface at the start (nothing, or a copy of a picture)
 d3_copyrect  3D Cascade's strips that have landed (blit::copyRect)
 d3_satrow / d3_satcol   the summed-area tables of the pictures (float sums in the CPU's order: along the row, then down the column)
 d3_poly      one thread per surface row: every polygon in turn, its strips of the row swept column by column as renderRow() does
 d3_fin       levels / 255
The planner's table (after the frame) holds, as bits of floats: the polygons (rows, normal, source picture), for each polygon row the list of strips (mode, count, first), the strips.
A strip is 21 words: xy[8] (14-bit fixed point), uwT, uwTdy, vwT, vwTdy, iwT, iwTdy, uwB, vwB, iwB, iwDx, uwDx, vwDx, minx.
"""
import struct
from gpukernels import kernel, PIC_IN, PIC_OUT, TABLE, WH, SF, to_px
from gpukernels_trans2 import ti

SPAN = 21


def tfl(S, tab, k):
    return tab.load(k)


# ---------------------------------------------------------------------------------------------------------- the pictures as levels, the tables
@kernel("d3_lev", WH, [PIC_IN, PIC_OUT, TABLE])
def d3_lev(S):
    # draw3d() in builtins_trans.cpp: a float picture goes in as whole levels, roundNonNeg(clamp(v) * 255) (the other picture is the table's frame)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        pa = S.buf('in0').load(i)
        pb = to_px(S, i)

        def lv(v):
            c = S.select(S.isfinite(v), v, S.f(0.0))
            c = S.min(S.max(c, 0.0), 1.0)
            x = c * 255.0
            n = S.to_i(x)
            return S.to_f(n + S.select((x - S.to_f(n)) >= 0.5, S.i(1), S.i(0)))
        S.buf('out').store(i, S.vec4(lv(pa[0]), lv(pa[1]), lv(pa[2]), lv(pa[3])))
        S.buf('out').store((P.h + S.gy) * P.w + S.gx, S.vec4(lv(pb[0]), lv(pb[1]), lv(pb[2]), lv(pb[3])))


@kernel("d3_init", WH + [('kind', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def d3_init(S):
    # the surface before the polygons: 0 nothing, 1 the outgoing picture, 2 the incoming one (levels from d3_lev)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        src = S.buf('in0').load(S.select(S.eq(P.kind, 2), (P.h + S.gy) * P.w + S.gx, i))
        z = S.f(0.0)
        S.buf('out').store(i, S.select(S.eq(P.kind, 0), S.vec4(z, z, z, z), src))


@kernel("d3_satrow", WH, [(2, 'out', 'v4', 'rw')], local=(64, 1))
def d3_satrow(S):
    # buildSat(), along the row: a[c] = a[c] + level (float, in order), the two pictures' rows one under the other
    P = S.p
    out = S.buf('out')
    with S.if_(S.gx < P.h * 2):
        a = [S.var('f', 0.0) for _ in range(4)]
        with S.for_(S.u(0), P.w) as x:
            idx = S.gx * P.w + x
            p = out.load(idx)
            for k in range(4):
                a[k].set(a[k].get() + p[k])
            out.store(idx, S.vec4(a[0].get(), a[1].get(), a[2].get(), a[3].get()))


@kernel("d3_satcol", WH, [(2, 'out', 'v4', 'rw')], local=(64, 1))
def d3_satcol(S):
    # buildSat(), down the column: d[y] = (row sum) + d[y - 1]
    P = S.p
    out = S.buf('out')
    with S.if_(S.gx < P.w * 2):
        half = S.gx / P.w
        x = S.gx % P.w
        a = [S.var('f', 0.0) for _ in range(4)]
        with S.for_(S.u(0), P.h) as y:
            idx = (half * P.h + y) * P.w + x
            p = out.load(idx)
            first = S.eq(y, 0)
            r = [S.select(first, p[k], p[k] + a[k].get()) for k in range(4)]
            for k in range(4):
                a[k].set(r[k])
            out.store(idx, S.vec4(r[0], r[1], r[2], r[3]))


@kernel("d3_fin", WH, [PIC_IN, PIC_OUT])
def d3_fin(S):
    # the surface's levels back to 0..1 (c.out.px[k] = out[k] * (1.f / 255.f))
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        v = S.buf('in0').load(i)
        k = S.f(1.0 / 255.0)
        S.buf('out').store(i, S.vec4(v[0] * k, v[1] * k, v[2] * k, v[3] * k))


# ---------------------------------------------------------------------------------------------------------- 3D Cascade's strips that have landed
@kernel("d3_copyrect", WH + [('rcOff', 'u')], [PIC_IN, (2, 'out', 'v4', 'rw'), TABLE])
def d3_copyrect(S):
    # blit::copyRect(): the rectangle of the incoming picture (levels under the outgoing picture's in the buffer in0) onto the same rectangle of the surface. The planner works
    # out the set-up (rectcopy numbers v[0..25], vk3d.cpp); here each pixel of the rectangle is one of: an edge row (the first and last row, every pixel mixed in by its coverage),
    # a row between (the first and last pixel mixed in, the others written: in pairs by the SSE2 code's rounding, an odd one by the plain bilinear read)
    P = S.p
    tab = S.buf('table')
    with S.in_range(P.w, P.h):
        def v(k):
            return ti(S, tab, P.rcOff + k)
        px0, px1, py0, py1 = v(0), v(1), v(2), v(3)
        x = S.to_i(S.gx)
        y = S.to_i(S.gy)
        with S.if_((x >= px0) & (x < px1) & (y >= py0) & (y < py1)):
            sx0, sy0, sx, sy, last, srow = v(4), v(5), v(6), v(7), v(8), v(9)
            fcc, lcc, frc, lrc, fs, ylast, xstep1, xadj = v(10), v(11), v(12), v(13), v(14), v(15), v(16), v(17)
            n = px1 - px0
            nrows = py1 - py0
            k = x - px0
            first_row = S.eq(y, py0)
            last_row = (nrows > 1) & S.eq(y, py1 - 1)
            edge = first_row | last_row
            e = S.select(first_row, S.i(0), S.i(1))
            ysrc = S.select(first_row, sy0, S.select(last_row, ylast, (y - (py0 + 1)) * sy + fs + sy0))
            ysrc = S.max(ysrc, S.i(0))
            r = ysrc / 16384
            fy = ysrc % 16384
            r1 = r + S.select(S.ne(r, srow - 1), S.i(1), S.i(0))
            W = S.to_i(P.w)
            H = S.to_i(P.h)
            src = S.buf('in0')
            out = S.buf('out')

            def tap(row, col):
                px = src.load(S.to_u((H + row) * W + col))
                return [S.to_i(px[c]) for c in range(4)]
            # the column: its position in the source row and what it is mixed with
            nmid = n - 2
            xm = sx0 + xstep1 + (k - 1) * sx             # (a pixel between the first and the last)
            is_first = S.eq(k, 0)
            is_last = (n > 1) & S.eq(k, n - 1)
            xl = sx0 + xstep1 + (n - 2) * sx + xadj
            xc = S.select(is_first, sx0, S.select(is_last, xl, xm))
            i0 = xc / 16384
            fx = xc % 16384
            i1 = i0 + S.select(S.ne(i0, last), S.i(1), S.i(0))
            ua = S.to_u
            # bil(): the 14-bit bilinear of the four pixels
            w00 = ua((0x4000 - fy) * (0x4000 - fx)) >> 4
            w10 = ua((0x4000 - fx) * fy) >> 4
            w01 = ua((0x4000 - fy) * fx) >> 4
            w11 = ua(fx * fy) >> 4
            a00, a10, a01, a11 = tap(r, i0), tap(r1, i0), tap(r, i1), tap(r1, i1)
            bil = [(ua(a00[c]) * w00 + ua(a10[c]) * w10 + ua(a01[c]) * w01 + ua(a11[c]) * w11 + 0x800000) >> 24 for c in range(4)]
            # the pixel at the right end of the source row has no right neighbour: the vertical mix alone (the last pixel of a row between)
            bil_end = [(ua(a00[c] * (0x4000 - fy) + a10[c] * fy + 0x2000)) >> 14 for c in range(4)]
            # bilSse(): the SSE2 code's rounding, for the pixels between in pairs
            j0 = xc >> 14
            f_ = xc & 0x3fff
            j1 = j0 + S.select(S.ne(j0, last), S.i(1), S.i(0))
            A = ((0x3fff - fy) << 2) & 0xffff
            F = (fy << 2) & 0xffff
            a_ = 0x3fff - f_
            s00 = S.to_i(ua(a_ * A) >> 16)
            s01 = S.to_i(ua(f_ * A) >> 16)
            s10 = S.to_i(ua(a_ * F) >> 16)
            s11 = S.to_i(ua(f_ * F) >> 16)
            b00, b10, b01, b11 = tap(r, j0), tap(r1, j0), tap(r, j1), tap(r1, j1)

            def mh(vv, ww):
                return ((vv << 5) * ww) >> 16
            sse = []
            for c in range(4):
                sv = S.min(mh(b10[c], s10) + 4, S.i(65535))
                sv = S.min(sv + mh(b11[c], s11), S.i(65535))
                tv = S.min(mh(b01[c], s01) + mh(b00[c], s00), S.i(65535))
                sv = S.min(sv + tv, S.i(65535))
                sse.append(S.min(sv >> 3, S.i(255)))
            # which pixel it is
            in_pair = (k > 0) & (k < n - 1) & (S.to_i(k - 1) < (nmid & ~1))
            # the alpha it is mixed in with
            alpha_e = S.select(is_first, S.select(S.eq(e, 0), v(18), v(21)), S.select(is_last, S.select(S.eq(e, 0), v(20), v(23)), S.select(S.eq(e, 0), v(19), v(22))))
            alpha_m = S.select(is_first, v(24), v(25))
            alpha = S.select(edge, alpha_e, alpha_m)
            mix_it = edge | is_first | is_last
            vals = [S.select(is_last & (~edge) & S.eq(i0, last), S.to_i(bil_end[c]), S.to_i(bil[c])) for c in range(4)]
            d = out.load(S.gy * P.w + S.gx)
            res = []
            for c in range(4):
                p = S.select(in_pair & (~edge), sse[c], vals[c])
                dc = S.to_i(d[c])
                m = (p - dc) * (alpha & 255) + 128
                dd = dc + ((m + (m >> 8)) >> 8)
                mixed = S.max(S.min(dd, S.i(255)), S.i(0))
                res.append(S.to_f(S.select(mix_it, mixed, p)))
            out.store(S.gy * P.w + S.gx, S.vec4(res[0], res[1], res[2], res[3]))


# ---------------------------------------------------------------------------------------------------------- exact integer pieces
def idiv_trunc(S, a, b, c):
    """cvtt((double)a * (double)b / (double)c) for a >= 0, c > 0 (the product exact in a double, the quotient below 2^15): the quotient truncated towards zero. The double
    division cannot round up to a whole number from below (the fraction is at least 1 / c, far above a double's last place), so this is the exact truncated quotient."""
    neg = b < 0
    bb = S.select(neg, S.i(0) - b, b)
    cc = S.select(c > 0, c, S.i(1))
    qf = S.to_f(a) * S.to_f(bb) / S.to_f(cc)
    q = S.to_i(S.floor(qf))
    r = a * bb - q * cc                     # (wrapping: the true remainder is small, so this is exact)
    for _ in range(2):
        low = r < 0
        q = S.select(low, q - 1, q)
        r = S.select(low, r + cc, r)
        high = r >= cc
        q = S.select(high, q + 1, q)
        r = S.select(high, r - cc, r)
    return S.select(neg, S.i(0) - q, q)


def div_pow2(S, v, s):
    """divPow2(): v / 2^s towards zero"""
    return (v + ((v >> 31) & ((1 << s) - 1))) >> s


def f32_up(x):
    """the smallest float >= x (a Python double)"""
    b = struct.unpack("<I", struct.pack("<f", x))[0]
    f = struct.unpack("<f", struct.pack("<I", b))[0]
    if f >= x:
        return f
    return struct.unpack("<f", struct.pack("<I", b + 1))[0]


# ---------------------------------------------------------------------------------------------------------- the rasteriser
@kernel("d3_poly", WH + [('npoly', 'u'), ('polyOff', 'u'), ('callOff', 'u'), ('spanOff', 'u'), ('mode', 'u'), ('fadeFix', 'i'), ('lx', 'f'), ('ly', 'f'), ('lz', 'f'), ('lint', 'f'),
                         ('cxh', 'u'), ('cxl', 'u'), ('cyh', 'u'), ('cyl', 'u')],
        [PIC_IN, (2, 'out', 'v4', 'rw'), TABLE], local=(64, 1))
def d3_poly(S):
    P = S.p
    tab = S.buf('table')
    sat = S.buf('in0')
    surf = S.buf('out')
    W = S.to_i(P.w)
    H = S.to_i(P.h)
    wf = S.to_f(P.w)
    hf = S.to_f(P.h)
    colS, colE, started, fin, gone, alloc, fresh = [S.local_array(40, 'i') for _ in range(7)]
    iwv, uwv, vwv = [S.local_array(40, 'f') for _ in range(3)]
    Marr = S.local_array(32, 'i')
    msk = S.local_array(40 * 32, 'i')         # the masks of the strips' records, kept from column to column as the CPU's are
    segs = S.local_array(16, 'i')
    Cx = SF(S, P.cxh, P.cxl)
    Cy = SF(S, P.cyh, P.cyl)
    BIG = 4503601774854144.0
    zero_f = S.f(0.0)

    def sp_i(jb, k):
        return ti(S, tab, jb + k)

    def sp_f(jb, k):
        return tab.load(jb + k)

    def satLookup(row0, xx, yy):
        """satLookup(): the table read between its four corners; (xx, yy) in 14-bit fixed point; returns a vec4"""
        ey = yy + ((yy >> 31) & 0x3fff)
        fy = (ey & 0x3fff) - ((yy >> 31) & 0x3fff)
        yi = ey >> 14
        ex = xx + ((xx >> 31) & 0x3fff)
        fx = (ex & 0x3fff) - ((xx >> 31) & 0x3fff)
        xi = ex >> 14
        fyf = S.to_f(fy) * S.f(6.103515625e-05)
        fxf = S.to_f(fx) * S.f(6.103515625e-05)
        xm = xi - 1
        ym = yi - 1

        def at(xq, yq):
            i = (row0 + S.max(yq, S.i(0))) * W + S.max(xq, S.i(0))
            return sat.load(S.to_u(i))
        r9 = at(xm, ym)
        r10 = at(xi, ym)
        rax = at(xm, yi)
        rcx = at(xi, yi)
        z = S.vec4(zero_f, zero_f, zero_f, zero_f)
        z9 = (ym < 0) | (xm < 0)
        z10 = ym < 0
        zax = xm < 0
        r9 = S.select(z9, z, r9)
        r10 = S.select(z10, z, r10)
        rax = S.select(zax, z, rax)
        w6 = S.f(1.0) - fyf
        w5 = S.f(1.0) - fxf
        w4 = w5 * w6
        w5 = w5 * fyf
        w7 = fyf * fxf
        w6 = w6 * fxf
        a = r10 * w6
        b = r9 * w4
        a = a + b
        d = rax * w5
        a = a + d
        e = rcx * w7
        return a + e

    with S.if_(S.gx < P.h):
        yrow = S.to_i(S.gx)
        yf = S.to_f(yrow)
        with S.for_(S.u(0), P.npoly) as pi:
            h0 = P.polyOff + pi * 8
            y0 = ti(S, tab, h0)
            y1 = ti(S, tab, h0 + 1)
            with S.if_((yrow >= y0) & (yrow < y1)):
                goff = S.to_u(ti(S, tab, h0 + 2))
                nrm = [tab.load(h0 + 4 + k) for k in range(3)]
                srow0 = S.select(S.eq(ti(S, tab, h0 + 7), 0), S.i(0), H)
                g = goff + S.to_u(yrow - y0) * 2
                firstCall = S.to_u(ti(S, tab, g))
                ncalls = S.to_u(ti(S, tab, g + 1))
                with S.for_(S.u(0), ncalls) as q:
                    cb = P.callOff + (firstCall + q) * 3
                    sp0 = P.spanOff + S.to_u(ti(S, tab, cb)) * SPAN
                    nsp = S.to_u(ti(S, tab, cb + 1))
                    stale = ti(S, tab, cb + 2)
                    # the records' masks as the call finds them
                    with S.for_(S.u(0), nsp) as j:
                        with S.for_(S.u(0), S.u(32)) as r:
                            msk.store(j * 32 + r, S.select(stale >= 0, ti(S, tab, S.select(stale >= 0, S.to_u(stale) + j * 32 + r, S.u(0))), S.i(0)))
                    # ---- renderRow(): where the row starts
                    xs = S.var('i', 0x7fffffff)
                    with S.for_(S.u(0), nsp) as j:
                        xs.set(S.min(xs.get(), ti(S, tab, sp0 + j * SPAN + 20) >> 14))
                    x = S.var('i', xs.get())
                    bnd = S.var('i', (ti(S, tab, sp0 + 20) + 0x4000) & ~0x3fff)
                    done = S.var('u', S.select(xs.get() >= W, S.u(1), S.u(0)))
                    nextSp = S.var('u', 0)
                    nact = S.var('i', 0)
                    with S.for_(S.u(0), nsp) as j:
                        alloc.store(j, S.i(0))
                        gone.store(j, S.i(0))
                    bot = (yrow + 1) << 14
                    with S.for_(S.u(0), P.w + 3):
                        with S.if_(S.eq(done.get(), 0)):
                            sm = [S.var('i', 0) for _ in range(4)]
                            acc = S.var('f', 0.0)
                            tot = S.var('i', 0)
                            minit = S.var('u', 0)
                            with S.for_(S.u(0), nsp) as j:
                                jb = sp0 + j * SPAN
                                xy = [sp_i(jb, k) for k in range(8)]
                                minL = S.min(xy[0], xy[2])
                                maxL = S.max(xy[0], xy[2])
                                minR = S.min(xy[4], xy[6])
                                maxR = S.max(xy[4], xy[6])
                                # ---- a strip that reaches the column starts here (fragInit)
                                with S.if_(S.eq(j, nextSp.get()) & (sp_i(jb, 20) < bnd.get())):
                                    colS.store(j, minL)
                                    colE.store(j, (minL + 0x4000) & ~0x3fff)
                                    iwv.store(j, sp_f(jb, 16))
                                    uwv.store(j, sp_f(jb, 14))
                                    vwv.store(j, sp_f(jb, 15))
                                    started.store(j, S.i(0))
                                    fin.store(j, S.select((((minL + 0x4000) & ~0x3fff) >= maxR), S.i(1), S.i(0)))
                                    alloc.store(j, S.i(1))
                                    gone.store(j, S.i(0))
                                    fresh.store(j, S.i(1))
                                    nextSp.set(nextSp.get() + 1)
                                    nact.set(nact.get() + 1)
                                with S.if_(S.ne(alloc.load(j), 0) & S.eq(gone.load(j), 0)):
                                    cs = colS.load(j)
                                    ce = colE.load(j)
                                    isfresh = S.ne(fresh.load(j), 0)
                                    # ---- the strip cut to the pixel: segments for the mask (nseg -1: the pixel is filled)
                                    nseg = S.var('i', 0)
                                    fullc = (~isfresh) & (cs > maxL) & (ce < minR)
                                    ty = xy[1]
                                    tb = xy[3]
                                    with S.if_(fullc):
                                        with S.if_(S.eq(ty & 0x3fff, 0) & S.eq(tb - ty, 0x4000)):
                                            nseg.set(S.i(-1))
                                        with S.else_():
                                            rt = ty - bot + 0x4000
                                            rb = tb - bot + 0x4000
                                            for k, v in enumerate([S.i(0), rt, S.i(0), rb, S.i(0x4000), rt, S.i(0x4000), rb]):
                                                segs.store(S.u(k), v)
                                            nseg.set(S.i(2))
                                    with S.else_():
                                        clip_pixel(S, xy, cs, ce, bot, segs, nseg)
                                    # ---- the mask and its coverage
                                    cov = S.var('i', 0)
                                    with S.if_(S.ne(nseg.get(), -1)):
                                        with S.if_(S.eq(minit.get(), 0)):
                                            with S.for_(S.u(0), S.u(32)) as r:
                                                Marr.store(r, S.i(0))
                                            minit.set(S.u(1))
                                        mb = j * 32
                                        build_mask(S, nseg.get(), segs, msk, mb)
                                        c = S.var('i', 0)
                                        with S.for_(S.u(0), S.u(32)) as r:
                                            m1 = msk.load(mb + r) & ~Marr.load(r)
                                            msk.store(mb + r, m1)
                                            Marr.store(r, Marr.load(r) | m1)
                                            c.set(c.get() + S.to_i(S.bit_count(S.to_u(m1))))
                                        cov.set(c.get())
                                    # ---- the colour of the strip at this pixel (fragColour)
                                    iw = iwv.load(j)
                                    uw = uwv.load(j)
                                    vw = vwv.load(j)
                                    iwSt = sp_f(jb, 13)
                                    uwSt = sp_f(jb, 9)
                                    vwSt = sp_f(jb, 11)
                                    w_ = S.f(1.0) / iw
                                    iw2 = iw + iwSt
                                    iw2 = iw2 + sp_f(jb, 17)
                                    w2 = S.f(1.0) / iw2
                                    u2 = uw + uwSt
                                    u2 = u2 + sp_f(jb, 18)
                                    u2 = u2 * w2
                                    v2 = vw + vwSt
                                    v2 = v2 + sp_f(jb, 19)
                                    v2 = v2 * w2
                                    u0 = uw * w_
                                    v0 = vw * w_

                                    def lowword(fv, C):
                                        return (SF.from_f32(S, fv) * C + BIG).lo
                                    X0 = S.to_i(lowword(u0, Cx) + S.u(0x80000000))
                                    Y0 = S.to_i(lowword(v0, Cy) + S.u(0x80000000))
                                    X1r = S.to_i(lowword(u2, Cx))
                                    Y1r = S.to_i(lowword(v2, Cy))
                                    dY = Y1r - Y0 - S.i(0x80000000)
                                    dX = X1r - X0 - S.i(0x80000000)
                                    adx = S.select(dX < 0, S.i(0) - dX, dX)
                                    ady = S.select(dY < 0, S.i(0) - dY, dY)
                                    col = box_filter(S, satLookup, srow0, W, H, X0, Y0, adx, ady)
                                    with S.if_(S.eq(nseg.get(), -1)):
                                        for k in range(4):
                                            sm[k].set(col[k])
                                        cov.set(S.i(1024))
                                        acc.set(w_ * S.f(1024.0) + acc.get())
                                    with S.else_():
                                        for k in range(4):
                                            sm[k].set(sm[k].get() + div_pow2(S, cov.get() * col[k], 10))
                                        acc.set(w_ * S.to_f(cov.get()) + acc.get())
                                    tot.set(tot.get() + cov.get())
                                    # ---- on to the next column (fragAdvance)
                                    with S.if_(S.eq(fin.load(j), 0)):
                                        nxt = ce
                                        tlx = xy[0]
                                        a_iw, a_uw, a_vw = iw, uw, vw
                                        niw = S.var('f', iw)
                                        nuw = S.var('f', uw)
                                        nvw = S.var('f', vw)
                                        with S.if_(tlx >= nxt):
                                            niw.set(iwSt + iw)
                                            nvw.set(vwSt + vw)
                                            nuw.set(uwSt + uw)
                                        with S.else_():
                                            st0 = S.eq(started.load(j), 0)
                                            biw = S.select(st0, sp_f(jb, 12), iw)
                                            buw = S.select(st0, sp_f(jb, 8), uw)
                                            bvw = S.select(st0, sp_f(jb, 10), vw)
                                            started.store(j, S.i(1))
                                            with S.if_(tlx >= cs):
                                                d = S.to_f(nxt - tlx) * S.f(6.103515625e-05)
                                                aa = d * iwSt + biw
                                                bb = d * vwSt + bvw
                                                cc = d * uwSt
                                                niw.set(aa)
                                                nvw.set(bb)
                                                nuw.set(cc + buw)
                                            with S.else_():
                                                niw.set(iwSt + biw)
                                                nvw.set(vwSt + bvw)
                                                nuw.set(uwSt + buw)
                                        iwv.store(j, niw.get())
                                        uwv.store(j, nuw.get())
                                        vwv.store(j, nvw.get())
                                        colS.store(j, nxt)
                                        colE.store(j, nxt + 0x4000)
                                        with S.if_(nxt >= maxR):
                                            fin.store(j, S.i(1))
                                    fresh.store(j, S.i(0))
                                    with S.if_(S.ne(fin.load(j), 0)):
                                        gone.store(j, S.i(1))
                                        nact.set(nact.get() - 1)
                            # ---- the pixel
                            with S.if_(S.ne(tot.get(), 0)):
                                with S.if_((yrow > 0) | (x.get() >= 0)):
                                    s = wf * acc.get()
                                    s = s * wf
                                    s = s / S.to_f(tot.get())
                                    blend_pixel(S, P, surf, yrow, x.get(), yf, sm, tot.get(), s, nrm, wf, hf)
                            x.set(x.get() + 1)
                            bnd.set(bnd.get() + 0x4000)
                            with S.if_(x.get() >= W):
                                done.set(S.u(1))
                            with S.if_(S.eq(nextSp.get(), nsp) & S.eq(nact.get(), 0)):
                                done.set(S.u(1))


def clip_pixel(S, xy, colS, colE, bot, segs, nseg):
    """clipPixel(): the strip's left and right edge cut to the pixel [colS, colE) of the row: up to four segments (A to B, relative to the pixel's bottom left, 14-bit fixed
    point) into segs (4 ints each), their number into nseg"""
    n = S.var('i', 0)

    def setseg(k, ax, ay, bx, by):
        base = S.to_u(k) * 4
        segs.store(base, ax)
        segs.store(base + 1, ay)
        segs.store(base + 2, bx)
        segs.store(base + 3, by)

    def rel(y):
        return y - bot + 0x4000
    # ---- the left edge, p1 the lower x (a tie: the bottom point)
    xa, ya, xb, yb = xy[0], xy[1], xy[2], xy[3]
    sw = xa >= xb
    x1 = S.select(sw, xb, xa)
    y1 = S.select(sw, yb, ya)
    x2 = S.select(sw, xa, xb)
    y2 = S.select(sw, ya, yb)
    c1 = S.select(x1 < colS, S.i(1), S.select(x1 < colE, S.i(2), S.i(4)))
    c2 = S.select(x2 < colS, S.i(8), S.select(x2 < colE, S.i(0x10), S.i(0x20)))
    code = c1 | c2
    Y0 = rel(y1)
    Y1 = rel(y2)
    X0 = x1 - colE + 0x4000
    X1 = x2 - colE + 0x4000
    dx = x2 - x1
    dy = y2 - y1
    qA = idiv_trunc(S, colS - x1, dy, dx)
    qE = idiv_trunc(S, colE - x1, dy, dx)
    yc = qA + y1
    ycr = rel(yc)
    yE = (qE - bot + 0x4000) + y1
    cA = S.eq(code & 9, 9)
    cB = ~cA & S.eq(code & 0x11, 0x11)
    cC = ~cA & ~cB & S.eq(code & 0x21, 0x21)
    cD = ~cA & ~cB & ~cC & S.eq(code & 0x12, 0x12)
    cE = ~cA & ~cB & ~cC & ~cD & S.ne(code & 2, 0) & S.ne(code & 0x20, 0)
    z = S.i(0)
    h4 = S.i(0x4000)
    with S.if_(cA):
        setseg(z, z, Y0, z, Y1)
        n.set(S.i(1))
    with S.if_(cB):
        setseg(z, z, ycr, z, Y0)
        setseg(S.i(1), z, ycr, X1, Y1)
        n.set(S.i(2))
    with S.if_(cC):
        setseg(z, z, ycr, z, Y0)
        setseg(S.i(1), z, ycr, h4, yE)
        n.set(S.i(2))
    with S.if_(cD):
        setseg(z, X0, Y0, X1, Y1)
        n.set(S.i(1))
    with S.if_(cE):
        setseg(z, X0, Y0, h4, yE)
        n.set(S.i(1))
    # ---- the right edge, p1 the lower x
    xa, ya, xb, yb = xy[4], xy[5], xy[6], xy[7]
    lt = xa < xb
    x1 = S.select(lt, xa, xb)
    y1 = S.select(lt, ya, yb)
    x2 = S.select(lt, xb, xa)
    y2 = S.select(lt, yb, ya)
    code = S.select(x1 < colS, S.i(1), S.select(x1 < colE, S.i(2), S.i(4)))
    code = code | S.select(x2 >= colS, S.select(x2 < colE, S.i(0x10), S.i(0x20)), S.i(0))
    X1r = x1 - colE + 0x4000
    X2r = x2 - colE + 0x4000
    Y1r = rel(y1)
    Y2r = rel(y2)
    dx = x2 - x1
    dy = y2 - y1
    qA = idiv_trunc(S, colS - x1, dy, dx)
    qE = idiv_trunc(S, colE - x1, dy, dx)
    yc = qA + y1
    ycr = rel(yc)
    yE = (qE - bot + 0x4000) + y1
    k = n.get()
    ca = S.ne(code & 4, 0) & S.ne(code & 0x20, 0)
    cb = ~ca & S.eq(code & 0x22, 0x22)
    cc = ~ca & ~cb & S.eq(code & 0x12, 0x12)
    cd = ~ca & ~cb & ~cc & S.eq(code & 0x11, 0x11)
    ce = ~ca & ~cb & ~cc & ~cd & S.ne(code & 1, 0) & S.ne(code & 0x20, 0)
    with S.if_(cb):
        setseg(k, X1r, Y1r, h4, yE)
        n.set(k + 1)
    with S.if_(cc):
        setseg(k, X1r, Y1r, X2r, Y2r)
        n.set(k + 1)
    with S.if_(cd):
        setseg(k, z, ycr, X2r, Y2r)
        setseg(k + 1, z, ycr, z, Y1r)
        n.set(k + 2)
    with S.if_(ce):
        setseg(k, z, ycr, h4, yE)
        setseg(k + 1, z, ycr, z, Y1r)
        n.set(k + 2)
    nseg.set(n.get())


def build_mask(S, nseg, segs, M, mb):
    """buildMask(): the 32 x 32 coverage mask (32 words at mb of M) of the segments; with no segment the mask is left as it was, and so are the rows of a first segment whose
    start column is past the pixel (the record keeps what it had)"""
    with S.for_(S.u(0), S.u(4)) as k:
        with S.if_(S.to_i(k) < nseg):
            c = [S.var('i', 0) for _ in range(4)]
            for q in range(4):
                v = segs.load(k * 4 + q)
                c[q].set(S.min(S.max(v, S.i(0)), S.i(0x4000)))
            c0, c1, c2, c3 = [t.get() for t in c]
            gt = c3 > c1
            yb = S.select(gt, c3, c1)
            yt = S.select(gt, c1, c3)
            xt = S.select(gt, c0, c2)
            xb = S.select(gt, c2, c0)
            rowB = yb >> 9
            rowT = yt >> 9
            nr = rowB - rowT
            first = S.eq(k, 0)
            with S.if_(first):
                with S.for_(S.u(0), S.to_u(rowT)) as r:
                    M.store(mb + r, S.i(0))
            dxr = S.select((nr - 1) > 0, (xb - xt) / S.select((nr - 1) > 0, nr - 1, S.i(1)), S.i(0))
            xv = S.var('i', xt)
            with S.for_(S.to_u(rowT), S.to_u(rowB)) as r:
                ri = S.to_i(r)
                sh = xv.get() >> 9
                m = S.to_i(S.u(0xffffffff) >> S.to_u(sh & 31))
                old = M.load(mb + r)
                with S.if_(sh < 32):
                    M.store(mb + r, S.select(first, m, old ^ m))
                first_row = S.eq(ri, rowT)
                P_ = ((rowT << 9) - yt + 0x200) * dxr
                step = div_pow2(S, P_, 14)
                xv.set(xv.get() + S.select(first_row, step, dxr))
            with S.if_(first):
                with S.for_(S.to_u(rowB), S.u(32)) as r:
                    M.store(mb + r, S.i(0))


def box_filter(S, satLookup, row0, W, H, X0, Y0, adx, ady):
    """boxFilter(): the average of the picture over the box at (X0, Y0) of the extent (adx, ady), 14-bit fixed point; the four levels, as ints"""
    dx = S.select(adx > 0x4000, adx, S.i(0x4000))
    dy = S.select(ady > 0x4000, ady, S.i(0x4000))
    dx = S.select(dx > 0x4000, dx * 3 - 0x8000, dx)
    dy = S.select(dy > 0x4000, dy * 3 - 0x8000, dy)
    x1 = X0 + dx
    y1 = Y0 + dy
    xMax = (W << 14) - 0x4000
    yMax = (H << 14) - 0x4000
    x0c = S.select(X0 < 0, S.i(0), S.select(X0 > xMax, xMax, X0))
    y0c = S.select(Y0 < 0, S.i(0), S.select(Y0 > yMax, yMax, Y0))
    x1c = S.select(x1 >= x0c + 1, S.select(x1 > (W << 14) - 1, (W << 14) - 1, x1), x0c + 1)
    y1c = S.select(y1 >= y0c + 1, S.select(y1 > (H << 14) - 1, (H << 14) - 1, y1), y0c + 1)
    f11 = satLookup(row0, x1c, y1c)
    f10 = satLookup(row0, x1c, y0c)
    f01 = satLookup(row0, x0c, y1c)
    f00 = satLookup(row0, x0c, y0c)
    a = S.to_f(y1c - y0c) * S.f(6.103515625e-05)
    b = S.to_f(x1c - x0c) * S.f(6.103515625e-05)
    a = a * b
    inv = S.f(1.0) / a
    out = []
    for c in range(4):
        s = f11[c] - f10[c]
        s = s - f01[c]
        s = s + f00[c]
        v = s * inv
        r = S.select(S.f(0.0) <= v, S.select(S.f(255.0) < v, S.f(255.0), v), S.f(0.0))
        out.append(S.to_i(r))
    return out


def dot3f(S, a, b):
    r = a[1] * b[1]
    q = a[0] * b[0]
    z = a[2] * b[2]
    r = r + q
    return r + z


def norm3(S, v):
    x, y, z = v
    l = x * x
    t = y * y
    l = l + t
    t = z * z
    l = l + t
    ln = S.sqrt(l)
    return [x / ln, y / ln, z / ln]


def pow8(S, q):
    """(float)std::pow(q, 8.0) for a float q: q^8 as the nearest double (the product in double-double), rounded to a float"""
    x = SF.from_f32(S, S.abs(q))
    x2 = x * x                                  # exact (48 bits)
    c = 134217729.0

    def two_prod(a, b):
        t = a * c
        ah = t - (t - a)
        al = a - ah
        t = b * c
        bh = t - (t - b)
        bl = b - bh
        p = a * b
        e = ((ah * bh - p) + ah * bl + al * bh) + al * bl
        return p, e
    p4, e4 = two_prod(x2, x2)
    p8, e8 = two_prod(p4, p4)
    e8 = e8 + (p4 * e4) * 2.0
    return (p8 + e8).to_f32()


def blend_pixel(S, P, surf, yrow, x, yf, sm, cov, s, nrm, wf, hf):
    """blendPixel(): the column's colours lit (the directional light, and the point light when mode is 2), laid over the surface pixel by the coverage and the fade"""
    idx = S.to_u(yrow * S.to_i(P.w) + x)
    dpv = surf.load(idx)
    dp = [S.to_i(dpv[k]) for k in range(4)]
    r11 = S.min(cov, S.i(1024))
    s0, s1, s2, s3 = [S.to_f(sm[k].get()) for k in range(4)]
    xf = S.to_f(x)
    a0 = S.var('f', 0.0)
    a1 = S.var('f', 0.0)
    a2 = S.var('f', 0.0)
    # the directional light (0, 0, 1)
    d = dot3f(S, nrm, [S.f(0.0), S.f(0.0), S.f(1.0)])
    d = S.select(S.f(0.0) > d, -d, d)
    a1.set(a1.get() + s1 * d)
    a2.set(a2.get() + s2 * d)
    a0.set(a0.get() + s0 * d)
    with S.if_(S.eq(P.mode, 2)):
        dy = P.ly - yf
        W2 = wf * wf
        H2 = hf * hf
        V = [wf * S.f(0.5) - xf, hf * S.f(0.5) - yf, S.f(0.0) - s]
        Lv = [P.lx - xf, dy, P.lz - s]
        t8 = dy * dy
        t3 = Lv[2] * Lv[2]
        t2 = Lv[0] * Lv[0]
        t8 = t8 / H2
        t3 = t3 / W2
        t2 = t2 / W2
        d2 = t8 + t2
        d2 = d2 + t3
        small = S.f(9.99999975e-06) > d2
        V = norm3(S, V)
        Ln = norm3(S, Lv)
        dist = S.sqrt(d2)
        dd = dot3f(S, nrm, Ln)
        neg = S.f(0.0) > dd
        Rv = []
        for k in range(3):
            t = (nrm[k] + nrm[k]) * dd
            Rv.append(S.select(neg, -(t - Ln[k]), t - Ln[k]))
        diff = S.select(neg, -dd, dd)
        q = dot3f(S, V, Rv)
        thr = f32_up(0.001)
        cond = S.select(neg, q <= S.f(-thr), q >= S.f(thr))
        spec = S.var('f', 0.0)
        with S.if_(cond & ~small):
            spec.set(pow8(S, q))
        den = dist * S.f(0.0)
        den = den + S.f(0.0)
        den = den + d2
        att = S.var('f', 1.0)
        with S.if_(~(S.f(1.0) > den)):
            av = S.f(1.0) / den
            att.set(S.select(~(av < S.f(1.0)), S.f(1.0), av))
        diffv = S.select(small, S.f(0.0), diff)
        specv = S.select(small, S.f(0.0), spec.get())
        attv = S.select(small, S.f(0.0), att.get())
        a_ = attv * P.lint
        t0 = a_ * S.f(1.0)

        def lit(sc):
            return ((((sc * S.f(0.8)) / S.f(255.0)) * diffv + specv) * t0) * S.f(255.0)
        v1 = lit(s1)
        v2 = lit(s2)
        v0 = lit(s0)
        a1.set(a1.get() + v1)
        a2.set(a2.get() + v2)
        a0.set(a0.get() + v0)
    outs = []
    for av in (a0, a1, a2):
        v = av.get()
        v = S.select(~(v < s3), s3, v)
        outs.append(S.to_i(S.round_even(v)))
    sm[0].set(outs[0])
    sm[1].set(outs[1])
    sm[2].set(outs[2])
    sumv = [sm[k].get() for k in range(4)]
    inv = S.i(1024) - r11
    dstA = dp[3]
    fadeFix = P.fadeFix
    full = S.eq(r11, 1024) & S.eq(fadeFix, 0x4000)
    r11s = S.select(r11 > 0, r11, S.i(1))
    Af = (sumv[3] << 10) / r11s
    Am = S.select(Af < 255, Af, S.i(255))
    # fade 1: the coverage alone
    e1 = (Am - dstA) * r11
    e1 = e1 << 4
    q1 = div_pow2(S, e1, 14)
    newA1 = q1 + dstA
    # fade < 1
    e2 = Am - dstA
    G = S.i(0) - div_pow2(S, (r11 << 4) * fadeFix, 14)
    t2_ = div_pow2(S, G * e2, 14)
    newA2 = dstA - t2_
    nofade = S.eq(fadeFix, 0x4000)
    newA = S.select(nofade, newA1, newA2)
    res = []
    for c in range(3):
        v_1 = div_pow2(S, dp[c] * inv, 10) + sumv[c]
        r1 = S.select(v_1 < newA, v_1, newA)
        ec = dp[c]
        v_2 = div_pow2(S, ec * inv, 10) - ec + sumv[c]
        v_2 = div_pow2(S, v_2 * fadeFix, 14) + ec
        r2 = S.select(v_2 < newA, v_2, newA)
        res.append(S.select(nofade, r1, r2))
    res.append(newA)
    fullv = [sumv[c] for c in range(4)]
    wr = S.select(full, S.f(1.0), S.f(0.0))
    with S.if_(full | (r11 > 0)):
        fin_ = [S.to_f(S.select(full, fullv[c], res[c]) & 255) for c in range(4)]
        surf.store(idx, S.vec4(fin_[0], fin_[1], fin_[2], fin_[3]))
