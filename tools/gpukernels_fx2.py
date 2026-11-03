"""Kernels of the video filters and generators that follow VEGAS's own code on the CPU and are drawn identically here: TV Simulator, Film Grain, Film Effects, Light Rays,
Color Gradient, Bump Map, Histogram, Scene Rotation (tools/gpukernels.py loads this file; see its header for the conventions). The C++ side is src/ofx/vkfx2.cpp."""
from gpukernels import kernel, PIC_IN, PIC_IN1, PIC_OUT, TABLE, WH, to_level, from_level, dpush, pd, ld_sf, SFVar
from sf64 import SF

TABU = (3, 'table', 'u', 'r')           # a table of whole numbers (the bits of ints)


def ti(S, idx):
    """the int at index idx of the table (a uint value)"""
    return S.to_i(S.buf('table').load(idx))


def div14(S, x):
    """vftv::div14: x / 16384 towards zero"""
    return S.select(x < 0, (x + 16383) >> 14, x >> 14)


def lvl4(S, p):
    """the four levels of a picture pixel (ints, r g b a)"""
    return [to_level(S, p.x), to_level(S, p.y), to_level(S, p.z), to_level(S, p.w)]


# ---------------------------------------------------------------------------------------------------------- TV Simulator (vfxtv.h render8)
@kernel("tv_shift", WH, [PIC_IN, PIC_OUT, TABU])
def tv_shift(S):
    # shiftRow8 + samplePixel: a row moved sideways by the noise (the numbers of the row from vftv::prepare8), the result as levels (floats) for the main pass
    P = S.p
    with S.in_range(P.w, P.h):
        row = S.gy
        base = row * 152
        sr, step, pa, pb, mixw, above, below = [ti(S, base + k) for k in (0, 2, 3, 4, 5, 6, 7)]
        W = S.to_i(P.w)
        W14 = S.to_i(P.w << 14)
        w1 = S.to_i(P.w) - 1
        i = S.to_i(S.gx)
        in0 = S.buf('in0')

        def rem(a, b):
            return S.select(S.eq(b, 0), S.i(0), a % S.select(S.eq(b, 0), S.i(1), b))

        def tap(x):
            neg = x < 0
            ge = x >= W14
            x1 = x + W14
            x1 = S.select(x1 < 0, S.i(0), x1)
            xn = S.select(neg, x1, S.select(ge, rem(x, W14), x))
            r = S.select(neg, above, S.select(ge, below, sr))
            idx = S.min(S.max(div14(S, xn), 0), w1)
            return xn, r, idx

        def sample(x):
            xn, r, idx = tap(x)
            frac = xn % 16384
            x2 = xn + 0x4000
            _, r2, idx2 = tap(x2)
            p1 = lvl4(S, in0.load(S.to_u(r) * P.w + S.to_u(idx)))
            p2 = lvl4(S, in0.load(S.to_u(r2) * P.w + S.to_u(idx2)))
            wt = 0x4000 - frac
            return [div14(S, p1[c] * wt + p2[c] * frac) & 255 for c in range(4)]
        s1 = sample(pa + i * step)
        s2 = sample(pb + i * step)
        # no mix (one tap) unless the move between two rows is more than 1.4 pixels
        two = mixw >= 0
        one = s1
        res = []
        for c in range(4):
            v = (s2[c] + s1[c]) >> 1
            if c < 3:
                v = div14(S, v * mixw) & 255
            res.append(S.select(two, v, one[c]))
        S.buf('out').store(row * P.w + S.gx, S.vec4(S.to_f(res[0]), S.to_f(res[1]), S.to_f(res[2]), S.to_f(res[3])))


@kernel("tv_main", WH + [('mix14', 'i'), ('xstart', 'i'), ('xstep', 'i'), ('flags', 'u'), ('pad', 'u')], [PIC_IN, PIC_IN1, PIC_OUT, TABU])
def tv_main(S):
    # the per-pixel part of render8: scanlines, aperture grill, static and the gamma table, all in whole numbers on the levels
    P = S.p
    with S.in_range(P.w, P.h):
        row = S.gy
        base = row * 152
        sr = ti(S, base)
        scan = ti(S, base + 1)
        x = S.to_i(S.gx)
        jit = S.ne(P.flags & 4, 0)
        a = S.buf('in0').load(S.to_u(sr) * P.w + S.gx)
        b = S.buf('in1').load(row * P.w + S.gx)
        lv = lvl4(S, a)
        jv = [S.to_i(b.x), S.to_i(b.y), S.to_i(b.z), S.to_i(b.w)]
        s = [S.select(jit, jv[k], lv[k]) for k in range(4)]          # r g b a
        bgra = [s[2], s[1], s[0], s[3]]
        xpos = P.xstart + x * P.xstep
        xf = xpos % 16384
        xi = div14(S, xpos)
        i0 = xi % 4
        i1 = i0 + 1
        wl = 0x4000 - xf
        j0 = S.to_u(S.select(i0 < 0, S.i(0), i0))
        j1 = S.to_u(S.select(i1 < 0, S.i(0), i1))
        pc = []
        for c in range(3):
            pc.append(div14(S, ti(S, base + 8 + j0 * 3 + c) * wl + ti(S, base + 8 + j1 * 3 + c) * xf) & 255)
        n = S.var('i', S.i(0))
        with S.if_(S.ne(P.flags & 2, 0)):
            xj = xi % 128
            xj = S.to_u(S.select(xj < 0, S.i(0), xj))
            n.set(div14(S, ti(S, base + 23 + xj + 1) * xf + ti(S, base + 23 + xj) * wl))
        alpha = bgra[3]
        d = []
        for c in range(3):
            pv = pc[c] + n.get()
            sc = bgra[c]
            a2 = pv * sc
            a2 = a2 + a2
            t1 = (a2 + S.select(a2 < 0, S.i(255), S.i(0))) >> 8
            q4 = (pv + S.select(pv < 0, S.i(3), S.i(0))) >> 2
            e = q4 + t1
            e = e * P.mix14
            e = e + (0x4000 - P.mix14) * sc
            e = div14(S, e)
            e = e * scan
            e = div14(S, e)
            e = S.select(e < 0, S.i(0), S.select(e > alpha, alpha, e))
            d.append(e & 255)
        ph = S.ne(P.flags & 1, 0)
        lutbase = P.h * 152
        out = []
        for c in range(3):
            l = ti(S, lutbase + S.to_u(d[c]) * 3 + c)
            out.append(S.select(ph, S.select(l >= alpha, alpha, l), d[c]))
        S.buf('out').store(row * P.w + S.gx, S.vec4(from_level(S, out[2]), from_level(S, out[1]), from_level(S, out[0]), from_level(S, alpha)))


# ---------------------------------------------------------------------------------------------------------- Color Gradient (colorGradient() in builtins_gen.cpp)
@kernel("cgrad", WH + [('type', 'u'), ('n', 'u')] + dpush('sx', 'sy', 'ddx', 'ddy', 'dd', 'cx', 'cy', 'rx', 'ry', 'ca', 'sa'), [PIC_OUT, TABLE])
def cgrad(S):
    # the position of the pixel along the shape in doubles, then the colour stops (the table: position, r, g, b, a of each as doubles) mixed in doubles
    P = S.p
    with S.in_range(P.w, P.h):
        tab = S.buf('table')
        W = SF.from_u32(S, P.w)
        H = SF.from_u32(S, P.h)
        u = SF.from_u32(S, S.gx) / W
        v = SF.from_u32(S, S.gy) / H
        zero = SF.const(S, 0.0)
        # linear
        ddx, ddy, dd = pd(P, 'ddx'), pd(P, 'ddy'), pd(P, 'dd')
        tl = (((u - pd(P, 'sx')) * ddx) + ((v - pd(P, 'sy')) * ddy)) / dd
        tl = SF.sel(S, dd > 1e-12, tl, zero)
        # elliptical / rectangular
        px = (u - pd(P, 'cx')) * W
        py = (v - pd(P, 'cy')) * H
        ca, sa = pd(P, 'ca'), pd(P, 'sa')
        qx = ((px * ca) + (py * sa)) / (pd(P, 'rx') * W)
        qy = (((px * sa).neg()) + (py * ca)) / (pd(P, 'ry') * H)
        te = ((qx * qx) + (qy * qy)).sqrt()
        tr = SF.sel(S, qx.abs() < qy.abs(), qy.abs(), qx.abs())
        t = SF.sel(S, S.eq(P.type, 0), tl, SF.sel(S, S.eq(P.type, 1), te, tr))
        t = SF.sel(S, t < 0.0, zero, SF.sel(S, t > 1.0, SF.const(S, 1.0), t))
        n = P.n
        def stop(k, j):
            return ld_sf(S, tab, (k * 5 + j) * 2)
        col = [SFVar(S, 0.0) for _ in range(4)]
        found = S.var('u', S.u(0))
        kv = S.var('u', S.u(1))
        with S.for_(1, n) as k:
            hit = S.eq(found.get(), 0) & ~(stop(k, 0) < t)
            with S.if_(hit):
                found.set(S.u(1))
                kv.set(k)
        first = stop(S.u(0), 0)
        lastpos = stop(n - 1, 0)
        k = kv.get()
        d = stop(k, 0) - stop(k - 1, 0)
        tt = (t - stop(k - 1, 0)) / d
        for j in range(4):
            a, b = stop(k - 1, j + 1), stop(k, j + 1)
            mixv = a + ((b - a) * tt)
            v1 = SF.sel(S, d <= 1e-9, b, mixv)
            v2 = SF.sel(S, t >= lastpos, stop(n - 1, j + 1), v1)
            v3 = SF.sel(S, t <= first, stop(S.u(0), j + 1), v2)
            col[j].set(v3)
        r, g, b, a = [c.get() for c in col]
        i = S.gy * P.w + S.gx
        S.buf('out').store(i, S.vec4((r * a).to_f32(), (g * a).to_f32(), (b * a).to_f32(), a.to_f32()))


# ---------------------------------------------------------------------------------------------------------- Film Grain (filmGrain() in builtins_more_creative.cpp)
def rint_i32(S, x):
    """(int)lrint(x) of a double (halves to even; |x| < 2^52)"""
    big = 4503599627370496.0
    up = (x + big) - big
    dn = (x - big) + big
    return SF.sel(S, x < 0.0, dn, up).trunc_i32()


@kernel("film_grain", WH + [('colour', 'u'), ('levels', 'u'), ('wt', 'i'), ('amount_f', 'f')] + dpush('chroma', 'omc'), [PIC_IN, PIC_OUT, TABLE])
def film_grain(S):
    # the table: 3 planes of 4096 doubles, then per column (the cells c0 and c1 as bits, the fraction and 1 - it as doubles), then the same per row
    P = S.p
    with S.in_range(P.w, P.h):
        tab = S.buf('table')
        cb = 24576 + S.gx * 6
        rb = 24576 + P.w * 6 + S.gy * 6
        c0, c1 = S.f_to_bits(tab.load(cb)), S.f_to_bits(tab.load(cb + 1))
        fx, omfx = ld_sf(S, tab, cb + 2), ld_sf(S, tab, cb + 4)
        r0, r1 = S.f_to_bits(tab.load(rb)), S.f_to_bits(tab.load(rb + 1))
        fy, omfy = ld_sf(S, tab, rb + 2), ld_sf(S, tab, rb + 4)

        def plane(p):
            base = p * 4096
            def t(r, c):
                return ld_sf(S, tab, (base + r + c) * 2)
            a = (omfx * t(r0, c0)) + (fx * t(r0, c1))
            b = (omfx * t(r1, c0)) + (fx * t(r1, c1))
            return (a * omfy) + (b * fy)
        n0 = plane(0)
        m = [SFVar(S, 0.0), SFVar(S, 0.0), SFVar(S, 0.0)]
        m[0].set(n0)
        m[1].set(n0)
        m[2].set(n0)
        with S.if_(S.ne(P.colour, 0)):
            n1, n2 = plane(1), plane(2)
            om = pd(P, 'omc')
            ch = pd(P, 'chroma')
            m[1].set((ch * n1) + (om * n0))
            m[2].set((ch * n2) + (om * n0))
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        sv = [s.x, s.y, s.z]
        out = []
        lev = S.ne(P.levels, 0)
        al = to_level(S, s.w)
        for k in range(3):
            mm = m[2 - k].get()
            # 8-bit: (int)lrint(m * 1.3 * level), cut to 0..alpha, mixed in whole numbers
            lv = to_level(S, sv[k])
            tt = rint_i32(S, (mm * 1.3) * SF.from_i32(S, lv))
            nv = S.select(tt < 0, S.i(0), S.select(tt > al, al, tt)) & 255
            e = (nv - lv) * P.wt + 128
            r = lv + (((e >> 8) + e) >> 8)
            r = S.select(r < 0, S.i(0), S.select(r > 255, S.i(255), r))
            o8 = from_level(S, r)
            # float: the float arithmetic as it comes
            nm = mm.to_f32() * 1.3
            noisy = sv[k] * nm
            of = (noisy - sv[k]) * P.amount_f + sv[k]
            out.append(S.select(lev, o8, of))
        oa = S.select(lev, from_level(S, al), s.w)
        S.buf('out').store(i, S.vec4(out[0], out[1], out[2], oa))


# ---------------------------------------------------------------------------------------------------------- Light Rays (vfxlr::render in vfxlightrays.h)
# The CPU draws the frame in four quarters around the light's pixel, each row a step further out, each pixel reading the RESULT drawn so far (bilinear samples on the way to the
# light). A pixel only reads pixels nearer the light (in both the row and the column of its quarter), so the quarters are drawn here in diagonals: all pixels of a quarter
# whose row + column (counted from the light) is the same are independent. What the CPU's read finds in a pixel not drawn yet is the original picture (its output starts as a
# copy): the kernel reads the result buffer (in BGRA order, as floats of whole numbers) for the pixels drawn earlier and the original for the others, in the CPU's own order.
LR_EDGE = (255, 0, 0, 0)        # what a sample beyond the right or bottom edge reads (blue, green, red, alpha)


def lr_levels(S, p):
    """the bytes of a picture pixel in the CPU's order: blue, green, red, alpha (ints)"""
    return [to_level(S, p.z), to_level(S, p.y), to_level(S, p.x), to_level(S, p.w)]


@kernel("lr_zone", WH + [('zone', 'u'), ('t', 'u'), ('amin', 'u'), ('cnt', 'u'), ('lxc', 'i'), ('lyc', 'i'), ('flags', 'u'), ('sens_q', 'i'), ('str_q', 'i'), ('noise_q', 'i'), ('pad', 'u')]
        + dpush('inv_rw', 'inv_rh', 'inner', 'feather_inv'), [PIC_IN, (2, 'out', 'v4', 'rw'), TABU], local=(64, 1))
def lr_zone(S):
    P = S.p
    with S.in_range(P.cnt, S.u(1)):
        W, H = S.to_i(P.w), S.to_i(P.h)
        lxc, lyc = P.lxc, P.lyc
        a = S.to_i(P.amin + S.gx)
        b = S.to_i(P.t) - a
        zone = P.zone
        right = S.ne(zone & 1, 0)
        down = S.ne(zone & 2, 0)
        x = S.select(right, lxc + 1 + b, lxc - b)
        y = S.select(down, lyc + 1 + a, lyc - a)
        bound = S.ne(P.flags & 1, 0)
        z1exec, z3exec = S.ne(P.flags & 4, 0), S.ne(P.flags & 8, 0)
        src = S.buf('in0')
        res = S.buf('out')

        def idx(X, Y):
            return S.to_u(Y) * P.w + S.to_u(X)

        sp = lr_levels(S, src.load(idx(x, y)))
        dx = lxc - x
        dy = lyc - y
        dist = SF.from_i32(S, dx * dx + dy * dy).sqrt()
        # the radius mask
        mask = S.var('i', S.i(0x10000))
        with S.if_(bound):
            ad = SF.from_i32(S, S.abs(dx)) * pd(P, 'inv_rw')
            bd = SF.from_i32(S, S.abs(dy)) * pd(P, 'inv_rh')
            d = ((bd * bd) + (ad * ad)).sqrt()
            inner = pd(P, 'inner')
            m_in = S.i(0x10000) - (((d - inner) * pd(P, 'feather_inv')) * 65536.0).trunc_i32()
            mask.set(S.select(d > inner, S.select(d >= 1.0, S.i(0), m_in), S.i(0x10000)))
        zero = SF.const(S, 0.0)
        inv = SF.sel(S, dist.eq(zero), zero, SF.const(S, 1.0) / dist)
        adx, ady = S.abs(dx), S.abs(dy)
        near = (adx < 3) & (ady < 3)
        steps = S.select(near, S.max(adx, ady), S.i(3))
        w = SF.sel(S, near, SF.sel(S, S.eq(steps, 0), zero, SF.const(S, 1.0) / SF.from_i32(S, steps)), SF.const(S, 1.0) / 3.0)
        vx = SF.from_i32(S, dx) * inv
        vy = SF.from_i32(S, dy) * inv
        def clamp99(v):
            return SF.sel(S, v < -0.99, SF.const(S, -0.99), SF.sel(S, SF.const(S, 0.99) < v, SF.const(S, 0.99), v))
        ux, uy = clamp99(vx), clamp99(vy)
        masked = [S.select(bound, (sp[k] * mask.get()) >> 16, sp[k]) for k in range(3)] + [sp[3]]
        # (the corner that is the pixel itself reads the original, with the mask on its colour when the radius is on)
        selfv = [S.select(bound, masked[k], sp[k]) for k in range(4)]

        aP = a
        bP = b
        def seen(X, Y):
            """what the CPU's read of dst finds at pixel (X, Y) for this pixel: the result when it was drawn earlier, else the original"""
            Xc = S.max(S.min(X, W - 1), 0)
            Yc = S.max(S.min(Y, H - 1), 0)
            zq = S.select(X > lxc, S.i(1), S.i(0)) + S.select(Y > lyc, S.i(2), S.i(0))
            aq = S.select(S.ne(zq & 2, 0), Y - (lyc + 1), lyc - Y)
            bq = S.select(S.ne(zq & 1, 0), X - (lxc + 1), lxc - X)
            zp = S.to_i(zone)
            earlier = (zq < zp) | (S.eq(zq, zp) & ((aq < aP) | (S.eq(aq, aP) & (bq < bP))))
            isL0 = S.eq(X, lxc) & S.eq(Y, lyc)
            isM = S.eq(X, lxc) & S.eq(Y, lyc + 1)
            use_src = ~earlier | (isL0 & (zp >= 1) & z1exec) | (isM & S.eq(zp, 3) & z3exec)
            ii = idx(Xc, Yc)
            s = lr_levels(S, src.load(ii))
            r = res.load(ii)
            rv = [S.to_i(r.x), S.to_i(r.y), S.to_i(r.z), S.to_i(r.w)]
            return [S.select(use_src, s[k], rv[k]) for k in range(4)]

        px = SFVar(S, 0.0)
        py = SFVar(S, 0.0)
        px.set(SF.from_i32(S, x))
        py.set(SF.from_i32(S, y))
        acc = [SFVar(S, 0.0) for _ in range(4)]
        for st in range(3):
            with S.if_(steps > st):
                npx = px.get() + ux
                npy = py.get() + uy
                px.set(npx)
                py.set(npy)
                x0 = npx.trunc_i32()
                y0 = npy.trunc_i32()
                fxd = npx - SF.from_i32(S, x0)
                fyd = npy - SF.from_i32(S, y0)
                fx = (fxd * 256.0).trunc_i32()
                fy = (fyd * 256.0).trunc_i32()
                wx = 256 - fx
                wy = 256 - fy
                A = seen(x0, y0)
                xe = S.eq(x0, W - 1)
                ye = S.eq(y0, H - 1)
                edge = [S.i(v) for v in LR_EDGE]
                Bv = seen(x0 + 1, y0)
                Cv = seen(x0, y0 + 1)
                Dv = seen(x0 + 1, y0 + 1)
                B = [S.select(xe, edge[k], Bv[k]) for k in range(4)]
                C = [S.select(ye, edge[k], Cv[k]) for k in range(4)]
                D = [S.select(xe | ye, edge[k], Dv[k]) for k in range(4)]
                near_self = (((npx - SF.from_i32(S, x)).abs()) < 1.0) & (((npy - SF.from_i32(S, y)).abs()) < 1.0)
                exact = fxd.eq(zero) & fyd.eq(zero)
                A = [S.select(near_self & (exact | S.eq(zone, 0)), selfv[k], A[k]) for k in range(4)]
                B = [S.select(near_self & ~exact & S.eq(zone, 1), selfv[k], B[k]) for k in range(4)]
                C = [S.select(near_self & ~exact & S.eq(zone, 2), selfv[k], C[k]) for k in range(4)]
                D = [S.select(near_self & ~exact & S.eq(zone, 3), selfv[k], D[k]) for k in range(4)]
                for k in range(4):
                    top = B[k] * fx + A[k] * wx
                    bot = D[k] * fx + C[k] * wx
                    got = ((top * wy + bot * fy) >> 16) & 255
                    acc[k].set((SF.from_i32(S, got) * w) + acc[k].get())
        ray = [S.select(S.eq(steps, 0), sp[k], acc[k].get().trunc_i32()) for k in range(4)]
        # mixed with the original
        sb = masked
        ray_avg = S.to_u((ray[0] & 255) + (ray[1] & 255) + (ray[2] & 255)) / 3
        src_avg = S.to_u(sb[0] + sb[1] + sb[2]) / 3
        sens = S.to_u(P.sens_q)
        f = (S.to_u(P.str_q) * sens) >> 16
        rw = S.select(src_avg > ray_avg, (ray_avg - src_avg + 256) * f, sens)
        sw = S.u(0x10000) - rw
        sbv = [S.var('i', sb[k]) for k in range(4)]
        with S.if_(S.ne(P.noise_q, 0)):
            s = S.to_i(S.buf('table').load(S.to_u(y) * P.w + S.to_u(x)))
            sa = S.to_i(src_avg)
            e = sa * s
            e = e + e
            e = (e + S.select(e < 0, S.i(0xffff), S.i(0))) >> 16
            e = e - sa
            e = e * P.noise_q
            delta = (e + S.select(e < 0, S.i(0xff), S.i(0))) >> 8
            with S.if_(sa > 1):
                for k in range(3):
                    cur = sb[k]
                    t_up = (255 - cur) * delta
                    t_dn = delta * cur
                    up = (cur + ((t_up >> 8) & 255)) & 255
                    dn = (cur + (((t_dn + 0xff) >> 8) & 255)) & 255
                    sbv[k].set(S.select(delta > 0, up, S.select(delta < 0, dn, cur)))
        sb = [v.get() for v in sbv]
        outv = []
        for k in range(4):
            v = ((S.to_u(sb[k]) * sw) + (S.to_u(ray[k] & 255) * rw)) >> 16
            outv.append(S.to_f(S.to_i(v & 255)))
        res.store(idx(x, y), S.vec4(outv[0], outv[1], outv[2], outv[3]))


@kernel("lr_final", WH + [('lxc', 'i'), ('lyc', 'i'), ('flags', 'u'), ('blend_q', 'i'), ('tint_r', 'i'), ('tint_g', 'i'), ('tint_b', 'i'), ('pad', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def lr_final(S):
    # the pixels the CPU copies back from the original (the light's own pixel when the right quarters run, and the one below it when the lower right one does), then the tint and Blend
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = lr_levels(S, S.buf('in0').load(i))
        r = S.buf('in1').load(i)
        d = [S.to_i(r.x), S.to_i(r.y), S.to_i(r.z), S.to_i(r.w)]
        x, y = S.to_i(S.gx), S.to_i(S.gy)
        isL0 = S.eq(x, P.lxc) & S.eq(y, P.lyc) & S.ne(P.flags & 4, 0)
        isM = S.eq(x, P.lxc) & S.eq(y, P.lyc + 1) & S.ne(P.flags & 8, 0)
        d = [S.select(isL0 | isM, s[k], d[k]) for k in range(4)]
        m = S.to_u(s[3]) * S.to_u(P.blend_q)
        t = m / 255
        r9 = S.u(0x10000) - t
        v0 = (S.to_u(d[0]) * S.to_u(P.tint_b)) >> 8 & 255
        v1 = (S.to_u(d[1]) * S.to_u(P.tint_g)) >> 8 & 255
        v2 = (S.to_u(d[2]) * S.to_u(P.tint_r)) >> 8 & 255
        o2 = ((S.to_u(s[2]) * t + v2 * r9) >> 16) & 255
        o1 = ((S.to_u(s[1]) * t + v1 * r9) >> 16) & 255
        o0 = ((S.to_u(s[0]) * t + v0 * r9) >> 16) & 255
        o3 = ((S.to_u(s[3]) * t + S.to_u(d[3]) * r9) >> 16) & 255
        tint_on = S.ne(P.flags & 2, 0)
        fin = [S.select(tint_on, S.to_i(o0), d[0]), S.select(tint_on, S.to_i(o1), d[1]), S.select(tint_on, S.to_i(o2), d[2]), S.select(tint_on, S.to_i(o3), d[3])]
        S.buf('out').store(i, S.vec4(from_level(S, fin[2]), from_level(S, fin[1]), from_level(S, fin[0]), from_level(S, fin[3])))


# ---------------------------------------------------------------------------------------------------------- Film Effects (vfxfilm.h)
# The picture work of one frame is a list of passes made on the CPU while it runs the particle simulation (which keeps its state from frame to frame there): the grey and the
# grain, the tint, the flashes (tints), the jitters (the picture moved) and the scattered additions of the scratches, dust and hair, each pixel's additions in the order they were made.
# The 8-bit pictures are the levels (ints) of the CPU's bytes, the float ones the CPU's floats; every pass leaves them as the CPU would.
def clamp8(S, v):
    return S.select(v < 0, S.i(0), S.select(v > 255, S.i(255), v))


def add_f(S, p, d):
    """vfxfilm::addF on a pixel (r, g, b, a floats): through the colour when 0 < alpha < 1, plain (and alpha 1) from alpha 1 up, black when alpha is 0; returns the new (r, g, b, a)"""
    r, g, b, a = p
    z = S.f(0.0)
    mid = (a > 1e-6) & (1.0 > a)
    top = (a > 1e-6) & ~(1.0 > a)
    om = [(r / a + d) * a, (g / a + d) * a, (b / a + d) * a, a]
    ot = [d + r, d + g, d + b, S.f(1.0)]
    return [S.select(mid, om[k], S.select(top, ot[k], z)) for k in range(4)]


@kernel("filmfx_load", WH + [('bytes', 'u'), ('grey', 'u')], [PIC_IN, PIC_OUT])
def film_load(S):
    # the picture as the effect takes it (8-bit: the levels), turned grey when asked
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        by = S.ne(P.bytes, 0)
        gy = S.ne(P.grey, 0)
        lv = [to_level(S, p.x), to_level(S, p.y), to_level(S, p.z), to_level(S, p.w)]
        g8 = (lv[0] + lv[1] + lv[2]) / 3
        v8 = S.min(g8, lv[3])
        o8 = [S.select(gy, v8, lv[k]) for k in range(3)] + [lv[3]]
        res8 = S.vec4(from_level(S, o8[0]), from_level(S, o8[1]), from_level(S, o8[2]), from_level(S, o8[3]))
        gf = ((p.x + p.y) + p.z) / 3.0
        resf = S.select(gy, S.vec4(gf, gf, gf, p.w), p)
        S.buf('out').store(i, S.select(by, res8, resf))


@kernel("filmfx_grain", WH + [('bytes', 'u'), ('k', 'i'), ('amount_f', 'f'), ('pad', 'u')], [(2, 'out', 'v4', 'rw'), TABU])
def film_grain_pass(S):
    # vfxfilm::grain: one number of the generator for each pixel (the table), in memory order
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('out').load(i)
        w = S.buf('table').load(i)
        by = S.ne(P.bytes, 0)
        lv = [to_level(S, p.x), to_level(S, p.y), to_level(S, p.z), to_level(S, p.w)]
        d = (((S.to_i(w & 255)) - 0x7f) * P.k) / 16384
        o8 = [clamp8(S, lv[k] + d) for k in range(3)]
        a8 = S.select(lv[3] < 255, clamp8(S, lv[3] + d), lv[3])
        res8 = S.vec4(from_level(S, o8[0]), from_level(S, o8[1]), from_level(S, o8[2]), from_level(S, a8))
        n = ((S.to_f(S.to_i(w & 0xffff) - 0x8000) * 3.0517578125e-05) * 0.5) * P.amount_f
        of = add_f(S, [p.x, p.y, p.z, p.w], n)
        resf = S.vec4(of[0], of[1], of[2], of[3])
        S.buf('out').store(i, S.select(by, res8, resf))


@kernel("filmfx_tint", WH + [('bytes', 'u'), ('a8', 'i'), ('c0', 'i'), ('c1', 'i'), ('c2', 'i'), ('af', 'f'), ('cf0', 'f'), ('cf1', 'f'), ('cf2', 'f'), ('pad', 'u')], [(2, 'out', 'v4', 'rw')])
def film_tint(S):
    # vfxfilm::tintPass toward (c0, c1, c2) = (red, green, blue) by the strength
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('out').load(i)
        by = S.ne(P.bytes, 0)
        cs8 = (P.c0, P.c1, P.c2)
        csf = (P.cf0, P.cf1, P.cf2)
        pv = (p.x, p.y, p.z)
        o8, of = [], []
        for k in range(3):
            lv = to_level(S, pv[k])
            e = (cs8[k] - lv) * P.a8
            o8.append(from_level(S, (lv + ((e + S.select(e < 0, S.i(255), S.i(0))) >> 8)) & 255))
            of.append((csf[k] - pv[k]) * P.af + pv[k])
        res8 = S.vec4(o8[0], o8[1], o8[2], p.w)
        resf = S.vec4(of[0], of[1], of[2], p.w)
        S.buf('out').store(i, S.select(by, res8, resf))


@kernel("filmfx_jitter", WH + [('bytes', 'u'), ('dx', 'i'), ('dy', 'i'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def film_jitter(S):
    # vfxfilm::drawObj type 4: the whole picture moved by (dx, dy), opaque black coming in
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        sx = S.to_i(S.gx) + P.dx
        sy = S.to_i(S.gy) + P.dy
        inside = (sx >= 0) & (sx < S.to_i(P.w)) & (sy >= 0) & (sy < S.to_i(P.h))
        q = S.buf('in0').load(S.to_u(S.max(S.min(sy, S.to_i(P.h) - 1), 0)) * P.w + S.to_u(S.max(S.min(sx, S.to_i(P.w) - 1), 0)))
        S.buf('out').store(i, S.select(inside, q, S.vec4(0.0, 0.0, 0.0, 1.0)))


@kernel("filmfx_scatter", [('w', 'u'), ('h', 'u'), ('bytes', 'u'), ('ng', 'u')], [(2, 'out', 'v4', 'rw'), TABU], local=(64, 1))
def film_scatter(S):
    # vfxfilm::add8 / addF of the scratches, dust and hair: one thread for each pixel that is hit, its additions (table: groups of (pixel, first, count), then the numbers) in order
    P = S.p
    with S.in_range(P.ng, S.u(1)):
        tab = S.buf('table')
        pix = tab.load(S.gx * 3)
        first = tab.load(S.gx * 3 + 1)
        cnt = tab.load(S.gx * 3 + 2)
        base = P.ng * 3 + first
        p = S.buf('out').load(pix)
        by = S.ne(P.bytes, 0)
        v8 = [S.var('i', to_level(S, p.x)), S.var('i', to_level(S, p.y)), S.var('i', to_level(S, p.z))]
        vf = [S.var('f', p.x), S.var('f', p.y), S.var('f', p.z), S.var('f', p.w)]
        with S.for_(0, cnt) as e:
            raw = tab.load(base + e)
            with S.if_(by):
                d = S.to_i(raw)
                for k in range(3):
                    v8[k].set(clamp8(S, v8[k].get() + d))
            with S.else_():
                o = add_f(S, [vf[0].get(), vf[1].get(), vf[2].get(), vf[3].get()], S.bits_to_f(raw))
                for k in range(4):
                    vf[k].set(o[k])
        res8 = S.vec4(from_level(S, v8[0].get()), from_level(S, v8[1].get()), from_level(S, v8[2].get()), p.w)
        resf = S.vec4(vf[0].get(), vf[1].get(), vf[2].get(), vf[3].get())
        S.buf('out').store(pix, S.select(by, res8, resf))


# ---------------------------------------------------------------------------------------------------------- Histogram: Lab types and the logarithmic scale (histogramFx() in builtins_vegasrest.cpp)
def lab_f(S, r, g, b):
    """rgbToLabApprox: the float arithmetic as the CPU has it (powf and cbrtf as the double functions rounded to a float); returns L, A, B"""
    from gpukernels import fpow, cbrt_f
    def c01(v): return S.select(v < 0.0, S.f(0.0), S.select(v > 1.0, S.f(1.0), v))

    def lin(v):
        v = c01(v)
        return S.select(v <= 0.04045, v / 12.92, fpow(S, (v + 0.055) / 1.055, S.f(2.4)))
    lr, lg, lb = lin(r), lin(g), lin(b)
    X = (0.4124 * lr + 0.3576 * lg + 0.1805 * lb) / 0.95047
    Y = 0.2126 * lr + 0.7152 * lg + 0.0722 * lb
    Z = (0.0193 * lr + 0.1192 * lg + 0.9505 * lb) / 1.08883

    def f(t): return S.select(t > 0.008856, cbrt_f(S, t), 7.787 * t + S.f(16.0 / 116.0))
    fx, fy, fz = f(X), f(Y), f(Z)
    return 116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)


@kernel("hist_countlab", WH + [('type', 'u'), ('pad', 'u')] + dpush('hdr'), [(0, 'src', 'v4', 'r'), (2, 'hist', 'u', 'rw')])
def hist_countlab(S):
    # the counting of hist_count for Lab's L (5), a (6) and b (any other)
    from gpukernels_filters import fdiv
    P = S.p
    with S.in_range(P.w, P.h):
        p = S.buf('src').load(S.gy * P.w + S.gx)
        a = p.w
        with S.if_(a > 1e-5):
            r = fdiv(S, p.x, a)
            g = fdiv(S, p.y, a)
            b = fdiv(S, p.z, a)
            L, A, B = lab_f(S, r, g, b)
            v = S.select(S.eq(P.type, 5), L / 100.0, S.select(S.eq(P.type, 6), A / 256.0 + 0.5, B / 256.0 + 0.5))
            q = SF.from_f32(S, v) / pd(P, 'hdr')
            q = SF.sel(S, q < 0.0, SF.const(S, 0.0), SF.sel(S, q > 0.99999, SF.const(S, 0.99999), q))
            bin_ = S.min((q * 128.0).trunc_i32(), S.i(127))
            S.buf('hist').atomic_add(S.to_u(bin_), S.u(1))


@kernel("hist_drawlog", WH + [('x0', 'i'), ('y0', 'i'), ('bw', 'i'), ('bh', 'i'), ('back', 'f'), ('cr', 'f'), ('cg', 'f'), ('cb', 'f')], [(0, 'src', 'v4', 'r'), (1, 'hist', 'u', 'r'), (2, 'out', 'v4', 'w')])
def hist_drawlog(S):
    # hist_draw with the bar heights log1p(count) / log1p(largest count)
    from sf64 import sf_log
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
            # (log1p of a whole number is the logarithm of the exact sum)
            one = SF.const(S, 1.0)
            hgt = sf_log(S, SF.from_u32(S, bv.get()) + one) / sf_log(S, SF.from_u32(S, mxv.get()) + one)
            fb = 1.0 - ((SF.from_i32(S, y - P.y0) + 0.5) / SF.from_i32(S, P.bh))
            k = P.back * 0.6
            om = 1.0 - k
            bgc = S.vec4(o.x * om, o.y * om, o.z * om, o.w + (1.0 - o.w) * k)
            bar = S.vec4(P.cr * 0.9, P.cg * 0.9, P.cb * 0.9, S.f(1.0))
            drawn = S.select(~(fb > hgt), bar, bgc)
            res.set(S.select(S.ne(mxv.get(), 0), drawn, o))
        S.buf('out').store(i, res.get())


# ---------------------------------------------------------------------------------------------------------- Rays / Glint "Mask only" (maskOnly() in builtins_velvet.cpp)
@kernel("mask_only", WH, [PIC_OUT, TABLE])
def mask_only(S):
    # the mask itself, white over opaque black: (m, m, m, 1); table: the mask, a float for each pixel
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        m = S.buf('table').load(i)
        S.buf('out').store(i, S.vec4(m, m, m, S.f(1.0)))
