#!/usr/bin/env python3
"""Reno's GPU kernels, written in the Python kernel language of spvdsl.py and compiled to SPIR-V.
Usage: gpukernels.py <src/ofx/vkshaders folder> [--only name,name]
Writes <name>.spv.h for every kernel and push_layouts.h (one struct per kernel, the push constants in order).

Conventions (see src/ofx/vkplan.h): picture buffers hold premultiplied RGBA floats, TOP row first, like the CPU effects' Buf; the
kernel for pixel (x, y) is invoked with S.gx, S.gy; bindings: 0 in0, 1 in1, 2 out, 3 table.
Every kernel mirrors the CPU effect it is named for (src/ofx/builtins*.cpp) formula for formula; vkcheck compares them.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spvdsl import compile_kernel, c_array

KERNELS = []

def kernel(name, push=(), bufs=(), local=(16, 16)):
    def deco(fn):
        KERNELS.append((name, list(push), list(bufs), local, fn))
        return fn
    return deco

PIC_IN = (0, 'in0', 'v4', 'r')
PIC_IN1 = (1, 'in1', 'v4', 'r')
PIC_OUT = (2, 'out', 'v4', 'w')
TABLE = (3, 'table', 'f', 'r')
WH = [('w', 'u'), ('h', 'u')]

# ---------------------------------------------------------------------------------------------------------- frame conversion
@kernel("unpack_u8", WH + [('bgra', 'u'), ('pad', 'u')], [(0, 'raw', 'u', 'r'), PIC_OUT])
def unpack_u8(S):
    with S.in_range(S.p.w, S.p.h):
        v = S.buf('raw').load((S.p.h - 1 - S.gy) * S.p.w + S.gx)         # the host's rows are bottom first
        def byte(sh): return S.to_f((v >> sh) & 255)
        b0, b1, b2, b3 = byte(0), byte(8), byte(16), byte(24)
        bgra = S.ne(S.p.bgra, 0)
        r = S.select(bgra, b2, b0)
        b = S.select(bgra, b0, b2)
        k = S.f(1.0 / 255.0)                                              # (the float nearest 1/255, as the CPU's pixrows)
        S.buf('out').store(S.gy * S.p.w + S.gx, S.vec4(r, b1, b, b3) * k)

@kernel("unpack_f32", WH + [('bgra', 'u'), ('pad', 'u')], [(0, 'raw', 'v4', 'r'), PIC_OUT])
def unpack_f32(S):
    with S.in_range(S.p.w, S.p.h):
        S.buf('out').store(S.gy * S.p.w + S.gx, S.buf('raw').load((S.p.h - 1 - S.gy) * S.p.w + S.gx))

@kernel("pack_u8", WH + [('bgra', 'u'), ('pad', 'u')], [(0, 'in0', 'v4', 'r'), (2, 'raw', 'u', 'w')])
def pack_u8(S):
    # what store() does in builtins.cpp: alpha made sane and limited to 0..1, a colour never above alpha, rounded halves up
    with S.in_range(S.p.w, S.p.h):
        p = S.buf('in0').load(S.gy * S.p.w + S.gx)
        a3 = S.select(p.w >= 0.0, p.w, S.f(0.0))
        a = S.select(a3 > 1.0, S.f(1.0), a3)
        def rnd(vf):
            fl = S.floor(vf)
            return S.to_u(S.select((vf - fl) >= 0.5, fl + 1.0, fl))
        def chan(c):
            q = S.min(S.max(c, 0.0), a)
            q = S.select(S.isfinite(c), q, S.f(0.0))
            return rnd(q * 255.0)
        ch = [chan(p.x), chan(p.y), chan(p.z)]
        al = rnd(a * 255.0)
        bgra = S.ne(S.p.bgra, 0)
        byte0 = S.select(bgra, ch[2], ch[0])
        byte2 = S.select(bgra, ch[0], ch[2])
        word = byte0 | (ch[1] << 8) | (byte2 << 16) | (al << 24)
        S.buf('raw').store((S.p.h - 1 - S.gy) * S.p.w + S.gx, word)

@kernel("pack_f32", WH + [('bgra', 'u'), ('pad', 'u')], [(0, 'in0', 'v4', 'r'), (2, 'raw', 'v4', 'w')])
def pack_f32(S):
    with S.in_range(S.p.w, S.p.h):
        p = S.buf('in0').load(S.gy * S.p.w + S.gx)
        a3 = S.select(p.w >= 0.0, p.w, S.f(0.0))
        def fin(c): return S.select(S.isfinite(c), c, S.f(0.0))
        S.buf('raw').store((S.p.h - 1 - S.gy) * S.p.w + S.gx, S.vec4(fin(p.x), fin(p.y), fin(p.z), fin(a3)))

@kernel("copy_px", WH, [PIC_IN, PIC_OUT])
def copy_px(S):
    with S.in_range(S.p.w, S.p.h):
        i = S.gy * S.p.w + S.gx
        S.buf('out').store(i, S.buf('in0').load(i))

# ---------------------------------------------------------------------------------------------------------- shared pieces
def unpremul(S, p):
    """(r, g, b, a, has, valid) of a premultiplied pixel: the straight colour (0 where alpha is tiny), as pixelRow() does."""
    a = p.w
    valid = a > 0.0
    has = a > 1e-6
    r = S.select(has, p.x / a, S.f(0.0))
    g = S.select(has, p.y / a, S.f(0.0))
    b = S.select(has, p.z / a, S.f(0.0))
    return r, g, b, a, valid

def pixel_row(S, body):
    """The per-pixel colour effects: body(S, r, g, b) -> (r, g, b) on the straight colour; the pixel is premultiplied again,
    and a pixel with no alpha becomes all zeros (v4::pixelRow in builtins_vec.h)."""
    with S.in_range(S.p.w, S.p.h):
        i = S.gy * S.p.w + S.gx
        p = S.buf('in0').load(i)
        r, g, b, a, valid = unpremul(S, p)
        r, g, b = body(S, r, g, b)
        z = S.f(0.0)
        S.buf('out').store(i, S.vec4(S.select(valid, r * a, z), S.select(valid, g * a, z), S.select(valid, b * a, z), S.select(valid, a, z)))

# ---------------------------------------------------------------------------------------------------------- effects
@kernel("invert", WH + [('k', 'f')], [PIC_IN, PIC_OUT])
def invert(S):
    # invertRows() in builtins_filters.cpp: 8-bit levels, a step of floor(|d| * k) + 1 towards the inverse
    def body(S, r, g, b):
        def one(c):
            lv = S.floor(c * 255.0 + 0.5)
            d = 255.0 - 2.0 * lv
            ad = S.abs(d)
            step = S.min(ad, S.floor(ad * S.p.k + 0.0001) + 1.0)
            return (lv + S.select(d < 0.0, S.f(0.0) - step, step)) * (1.0 / 255.0)
        return one(r), one(g), one(b)
    pixel_row(S, body)



def luma(S, r, g, b):
    return 0.2126 * r + 0.7152 * g + 0.0722 * b          # (lumaV: the sums in this order)

def clamp01(S, v):
    return S.min(S.max(v, 0.0), 1.0)                       # (v4::clamp01: max with 0 first, then min with 1)

def sample_clamped(S, buf, w, h, x, y):
    """Buf::sampleClamped: bilinear at (x, y) in pixels (centres at +0.5), the position kept inside the frame; w, h uint values."""
    wf, hf = S.to_f(w), S.to_f(h)
    cx = S.select(x < 0.5, S.f(0.5), S.select(x > wf - 0.5, wf - 0.5, x))
    cy = S.select(y < 0.5, S.f(0.5), S.select(y > hf - 0.5, hf - 0.5, y))
    xs, ys = cx - 0.5, cy - 0.5
    x0f, y0f = S.floor(xs), S.floor(ys)
    fx, fy = xs - x0f, ys - y0f
    x0, y0 = S.to_u(x0f), S.to_u(y0f)
    x1, y1 = S.min(x0 + 1, w - 1), S.min(y0 + 1, h - 1)
    ix, iy = 1.0 - fx, 1.0 - fy
    w00, w10, w01, w11 = ix * iy, fx * iy, ix * fy, fx * fy
    p00 = buf.load(y0 * w + x0)
    p10 = buf.load(y0 * w + x1)
    p01 = buf.load(y1 * w + x0)
    p11 = buf.load(y1 * w + x1)
    return (S.splat4(0.0) + p00 * w00) + p10 * w10 + p01 * w01 + p11 * w11

# ---------------------------------------------------------------------------------------------------------- colour effects
@kernel("brightnessandcontrast", WH + [('bright', 'f'), ('center', 'f'), ('f', 'f')], [PIC_IN, PIC_OUT])
def brightnessandcontrast(S):
    def body(S, r, g, b):
        P = S.p
        # (brightnessContrastRow: limited to 0..1, then truncated to a whole 1/255 as VEGAS does)
        def one(c):
            v = clamp01(S, (c + P.bright - P.center) * P.f + P.center)
            return S.floor(v * 255.0 + 0.001) * S.f(1.0 / 255.0)
        return tuple(one(c) for c in (r, g, b))
    pixel_row(S, body)

@kernel("blackandwhite", WH + [('k', 'f')], [PIC_IN, PIC_OUT])
def blackandwhite(S):
    # blackAndWhiteRows() in builtins_filters.cpp: 8-bit levels, grey = floor(mean), steps of ceil(|d| * k)
    def body(S, r, g, b):
        lr = S.floor(r * 255.0 + 0.5)
        lg = S.floor(g * 255.0 + 0.5)
        lb = S.floor(b * 255.0 + 0.5)
        grey = S.floor((lr + lg + lb) * (1.0 / 3.0) + 0.0001)
        def one(l):
            d = grey - l
            step = S.ceil(S.abs(d) * S.p.k - 0.00001)
            return (l + S.select(d < 0.0, S.f(0.0) - step, step)) * (1.0 / 255.0)
        return one(lr), one(lg), one(lb)
    pixel_row(S, body)

@kernel("sepia", WH + [('cr', 'f'), ('cg', 'f'), ('cb', 'f'), ('p', 'f'), ('q0', 'f'), ('falloff', 'f')], [PIC_IN, PIC_OUT])
def sepia(S):
    # sepiaRows() in builtins_filters.cpp
    def body(S, r, g, b):
        P = S.p
        m = (r + g + b) * (1.0 / 3.0)
        q = S.min(S.f(1.0), S.max(S.f(0.0), P.q0 - P.falloff * m))
        iq = 1.0 - q
        def mix(x, cv):
            grey = x + (m - x) * P.p
            return iq * grey + q * cv
        return mix(r, P.cr), mix(g, P.cg), mix(b, P.cb)
    pixel_row(S, body)

@kernel("threshold", WH + [('t', 'f')], [PIC_IN, PIC_OUT])
def threshold(S):
    def body(S, r, g, b):
        v = S.select(luma(S, r, g, b) >= S.p.t - 1e-5, S.f(1.0), S.f(0.0))
        return v, v, v
    pixel_row(S, body)

@kernel("colorbalance", WH + [('dr', 'f'), ('dg', 'f'), ('db', 'f'), ('type', 'u'), ('keep', 'u')], [PIC_IN, PIC_OUT])
def colorbalance(S):
    def body(S, r, g, b):
        P = S.p
        m = clamp01(S, (r + g + b) * (1.0 / 3.0))
        w0 = 1.0 - (1.0 - m) * (1.0 - m)
        w1 = 4.0 * m * (1.0 - m)
        w2 = 1.0 - m * m
        w = S.select(S.eq(P.type, 0), w0, S.select(S.eq(P.type, 1), w1, w2))
        tr, tg, tb = P.dr * w, P.dg * w, P.db * w
        k = S.ne(P.keep, 0)
        sr = S.select(k, tr - 0.5 * (tg + tb), tr)
        sg = S.select(k, tg - 0.5 * (tr + tb), tg)
        sb = S.select(k, tb - 0.5 * (tr + tg), tb)
        def cut(v):
            u = v * 255.0 + S.select(v < 0.0, S.f(-1e-3), S.f(1e-3))
            return S.select(u < 0.0, S.ceil(u), S.floor(u)) * (1.0 / 255.0)
        return clamp01(S, r + cut(sr)), clamp01(S, g + cut(sg)), clamp01(S, b + cut(sb))
    pixel_row(S, body)

def rgb_to_hsl(S, r, g, b):
    mx = S.max(r, S.max(g, b))
    mn = S.min(r, S.min(g, b))
    l = (mx + mn) * 0.5
    d = mx - mn
    flat = d < 1e-6
    s = S.select(l > 0.5, d / S.max(2.0 - mx - mn, 1e-6), d / S.max(mx + mn, 1e-6))
    hr = (g - b) / d + S.select(g < b, S.f(6.0), S.f(0.0))
    hg = (b - r) / d + 2.0
    hb = (r - g) / d + 4.0
    h = S.select(S.eq(mx, r), hr, S.select(S.eq(mx, g), hg, hb)) / 6.0
    return S.select(flat, S.f(0.0), h), S.select(flat, S.f(0.0), s), l

def hue2rgb(S, p, q, t):
    t = t - S.floor(t)
    a = p + (q - p) * 6.0 * t
    c = p + (q - p) * (S.f(2.0 / 3.0) - t) * 6.0
    return S.select(t < S.f(1.0 / 6.0), a, S.select(t < 0.5, q, S.select(t < S.f(2.0 / 3.0), c, p)))

def hsl_to_rgb(S, h, s, l):
    q = S.select(l < 0.5, l * (1.0 + s), l + s - l * s)
    p = 2.0 * l - q
    r = hue2rgb(S, p, q, h + S.f(1.0 / 3.0))
    g = hue2rgb(S, p, q, h)
    b = hue2rgb(S, p, q, h - S.f(1.0 / 3.0))
    gray = s <= 0.0
    return S.select(gray, l, r), S.select(gray, l, g), S.select(gray, l, b)

@kernel("hsladjust", WH + [('hue', 'f'), ('sat', 'f'), ('lum', 'f')], [PIC_IN, PIC_OUT])
def hsladjust(S):
    def body(S, r, g, b):
        P = S.p
        h, s, l = rgb_to_hsl(S, clamp01(S, r), clamp01(S, g), clamp01(S, b))
        return hsl_to_rgb(S, h + P.hue, clamp01(S, s * P.sat), clamp01(S, l * P.lum))
    pixel_row(S, body)

@kernel("levels", WH + [('channel', 'u'), ('i0', 'f'), ('i1', 'f'), ('o0', 'f'), ('o1', 'f'), ('curve', 'u')], [PIC_IN, PIC_OUT, TABLE])
def levels(S):
    # levels() in builtins_filters.cpp: the straight colour (0 where alpha is tiny), the chosen channels through the curve, premultiplied again
    with S.in_range(S.p.w, S.p.h):
        P = S.p
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        a = p.w
        has = a > 1e-6
        r = S.select(has, p.x / a, S.f(0.0))
        g = S.select(has, p.y / a, S.f(0.0))
        b = S.select(has, p.z / a, S.f(0.0))
        span = P.i1 - P.i0
        den = S.select(S.abs(span) < 0.003921568859368563, S.f(0.003921568859368563), span)
        def f(v):
            t = (v - P.i0) / den
            t = clamp01(S, t)
            x = t * 4096.0
            k = S.min(S.to_u(S.floor(x)), 4095)
            lo = S.buf('table').load(k)
            hi = S.buf('table').load(k + 1)
            curved = lo + (hi - lo) * (x - S.to_f(k))
            return P.o0 + (P.o1 - P.o0) * S.select(S.ne(P.curve, 0), curved, t)
        ch = P.channel
        r2 = S.select(S.eq(ch, 0) | S.eq(ch, 1) | S.eq(ch, 4), f(r), r)
        g2 = S.select(S.eq(ch, 0) | S.eq(ch, 2) | S.eq(ch, 4), f(g), g)
        b2 = S.select(S.eq(ch, 0) | S.eq(ch, 3) | S.eq(ch, 4), f(b), b)
        a2 = S.select(S.eq(ch, 4), clamp01(S, f(a)), a)
        S.buf('out').store(i, S.vec4(r2 * a2, g2 * a2, b2 * a2, a2))

@kernel("colorcurves", WH, [PIC_IN, PIC_OUT, TABLE])
def colorcurves(S):
    # colorCurvesRows: three tables of 1024 values (the master curve folded in); a pixel with no alpha is copied
    with S.in_range(S.p.w, S.p.h):
        i = S.gy * S.p.w + S.gx
        p = S.buf('in0').load(i)
        a = p.w
        inv = 1.0 / a
        def look(ch, v):
            base = ch * 1024
            f = v * 1023.0
            k = S.to_u(S.floor(f))
            lo = S.buf('table').load(base + S.min(k, 1022))
            hi = S.buf('table').load(base + S.min(k, 1022) + 1)
            interp = lo + (hi - lo) * (f - S.to_f(S.min(k, 1022)))
            return S.select(~(v > 0.0), S.buf('table').load(S.u(base)), S.select(v >= 1.0, S.buf('table').load(S.u(base + 1023)), interp))
        r = look(0, p.x * inv) * a
        g = look(1, p.y * inv) * a
        b = look(2, p.z * inv) * a
        tiny = ~(a > 1e-6)
        S.buf('out').store(i, S.select(tiny, p, S.vec4(r, g, b, a)))

@kernel("colorcorrectorsecondary",
        WH + [('cr', 'f'), ('sr', 'f'), ('sat', 'f'), ('cu', 'f'), ('cv', 'f'), ('gain', 'f'), ('off', 'f'), ('alpha', 'f'), ('gc', 'f'), ('ga', 'f'), ('flags', 'u'),
              ('l0', 'f'), ('l1', 'f'), ('ls', 'f'), ('s0', 'f'), ('s1', 'f'), ('ss', 'f'), ('hc', 'f'), ('hw', 'f'), ('hs', 'f')],
        [PIC_IN, PIC_OUT])
def colorcorrectorsecondary(S):
    # colorCorrectorSecondaryRows in builtins_filters2.cpp
    P = S.p
    POLY = [0.00758003534, -0.0561729743, 1.58064343, -7.07356105, 20.7903083, -20.3405720, 6.09177422]
    def fall(u):                                    # secondaryFall: a limit's fall-off, u = 1 - distance / smooth
        r = S.f(POLY[6])
        for q in range(5, -1, -1):
            r = r * u + POLY[q]
        r = S.min(S.max(r, 0.0), 1.0)
        return S.select(u > 0.0, S.select(u >= 1.0, S.f(1.0), r), S.f(0.0))
    def in_range(x, lo, hi, smooth):                # secondaryRange (lo <= hi: the planner swapped them)
        inside = (x >= lo) & (x <= hi)
        d = S.select(x < lo, lo - x, x - hi)
        t = S.select(smooth > 1e-6, fall(1.0 - d / smooth), S.f(0.0))
        return S.select(inside, S.f(1.0), t)
    def atan2_deg(y, x):
        # our own atan2 in degrees (a driver's is only approximate): a polynomial for the ratio of the smaller to the larger, then the
        # octant; a colour with no chroma (U, V about 1e-5 or less) has hue 0 - the CPU does the same, so a grey's rounding noise cannot
        # throw it into a hue range
        ax, ay = S.abs(x), S.abs(y)
        mx, mn = S.max(ax, ay), S.min(ax, ay)
        t = mn / S.max(mx, 1e-30)
        t2 = t * t
        q = t * (0.9998660 + t2 * (-0.3302995 + t2 * (0.1801410 + t2 * (-0.0851330 + t2 * 0.0208351))))     # atan(t) in radians, |error| < 1e-5
        q = S.select(ay > ax, 1.5707963267948966 - q, q)
        q = S.select(x < 0.0, 3.14159265358979323846 - q, q)
        q = S.select(y < 0.0, -q, q)
        return S.select(mx < 1e-5, S.f(0.0), q * S.f(180.0 / 3.14159265358979323846))
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        a = p.w
        valid = a > 0.0
        has = a > 1e-6
        r = S.select(has, p.x / a, S.f(0.0))
        g = S.select(has, p.y / a, S.f(0.0))
        b = S.select(has, p.z / a, S.f(0.0))
        Y = 0.299 * r + 0.587 * g + 0.114 * b
        U = 0.492 * (b - Y)
        V = 0.877 * (r - Y)
        fl = P.flags
        def flag(k): return S.ne(fl & (1 << k), 0)
        m = S.var('f', 1.0)
        with S.if_(flag(0)):
            m.set(m.get() * in_range(Y * 255.0, P.l0, P.l1, P.ls))
        with S.if_(flag(1)):
            m.set(m.get() * in_range(S.sqrt(U * U + V * V) * 255.0, P.s0, P.s1, P.ss))
        with S.if_(flag(2) & S.ne(m.get(), 0.0)):
            hue = atan2_deg(V, U)
            d0 = S.abs(S.fmod(hue - P.hc, 360.0))
            d = S.select(d0 > 180.0, 360.0 - d0, d0)
            dd = d - P.hw * 0.5
            inn = S.select(dd <= 0.0, S.f(1.0), S.select(P.hs > 1e-6, fall(1.0 - dd / P.hs), S.f(0.0)))
            m.set(m.get() * inn)
        with S.if_(flag(4)):
            m.set(1.0 - m.get())
        mm = m.get()
        # the change: hue turned, saturation scaled, the tint added; gain, gamma and offset on Y alone
        ut = U + P.cu
        vt = V + P.cv
        u2 = (ut * P.cr - vt * P.sr) * P.sat
        v2 = (ut * P.sr + vt * P.cr) * P.sat
        yp = S.max(Y * P.gain, 0.0)
        base = (yp + P.gc) / (1.0 + P.gc)
        yg = S.select(base > 0.0, S.pow(base, P.ga), S.f(0.0))
        yp = S.select(flag(5), yg, yp) + P.off
        yp = S.min(S.max(yp, 0.0), 1.0)
        nr = yp + v2 / 0.877
        nb = yp + u2 / 0.492
        ng = (yp - 0.299 * nr - 0.114 * nb) / 0.587
        oa = a * (1.0 + (P.alpha - 1.0) * mm)
        changed = S.vec4(clamp01(S, r + (nr - r) * mm) * oa, clamp01(S, g + (ng - g) * mm) * oa, clamp01(S, b + (nb - b) * mm) * oa, oa)
        res = S.select(mm > 0.0, changed, p)
        v = mm * a
        res = S.select(flag(3), S.vec4(v, v, v, v), res)
        S.buf('out').store(i, S.select(valid, res, S.splat4(0.0)))



# ---------------------------------------------------------------------------------------------------------- blurs
PLANE_IN = (0, 'in0', 'f', 'r')
PLANE_IN1 = (1, 'in1', 'f', 'r')
PLANE_OUT = (2, 'out', 'f', 'w')

def make_blur(name, vertical, plane):
    """A Gaussian pass along a row (or a column): blurLine() in builtins.cpp - the centre tap, then the pairs of taps either side
    (the edge pixel repeated), the weights from the table k[0..r].  A picture's channels are chosen by a mask (bit 0 red .. 3 alpha)."""
    push = WH + [('r', 'u')] + ([] if plane else [('mask', 'u')])
    bufs = [PLANE_IN, PLANE_OUT, TABLE] if plane else [PIC_IN, PIC_OUT, TABLE]
    @kernel(name, push, bufs)
    def blur(S):
        P = S.p
        with S.in_range(P.w, P.h):
            n = P.h if vertical else P.w
            pos = S.gy if vertical else S.gx
            def idx(q): return q * P.w + S.gx if vertical else S.gy * P.w + q
            src = S.buf('in0')
            centre = src.load(idx(pos))
            tab = S.buf('table')
            if plane:
                acc = S.var('f', centre * tab.load(S.u(0)))
            else:
                acc = S.var('v4', centre * tab.load(S.u(0)))
            with S.for_(1, P.r + 1) as k:
                a = S.select(pos >= k, pos - k, S.u(0))
                b = S.min(pos + k, n - 1)
                acc.set(acc.get() + (src.load(idx(a)) + src.load(idx(b))) * tab.load(k))
            res = acc.get()
            if not plane:
                m = P.mask
                def pick(bit, new, old): return S.select(S.ne(m & (1 << bit), 0), new, old)
                res = S.vec4(pick(0, res.x, centre.x), pick(1, res.y, centre.y), pick(2, res.z, centre.z), pick(3, res.w, centre.w))
            S.buf('out').store(S.gy * P.w + S.gx, res)
    return blur

make_blur("blur_h", False, False)
make_blur("blur_v", True, False)
make_blur("blurp_h", False, True)
make_blur("blurp_v", True, True)

@kernel("deriche", WH + [('vert', 'u'), ('inlv', 'u'), ('outlv', 'u'), ('pad', 'u'),
                    ('n0', 'f'), ('n1', 'f'), ('n2', 'f'), ('n3', 'f'), ('d1', 'f'), ('d2', 'f'), ('d3', 'f'), ('d4', 'f'),
                    ('m1', 'f'), ('m2', 'f'), ('m3', 'f'), ('m4', 'f'), ('kn', 'f'), ('km', 'f')], [PIC_IN, PIC_OUT], local=(64, 1))
def deriche(S):
    """One line (a row, or a column with vert) of VEGAS's Deriche Gaussian (vegasDericheBlurSigma() in builtins_deriche.cpp), one
    invocation per line, all four channels at once: the causal half forwards into out, then the anti-causal half backwards added
    to it. The edge values go on for ever outside (their filter state xL * kn, xR * km). inlv: the input is a picture (0..1) taken
    to levels (1; 2: whole ones); outlv: the output turned back into 0..1 (1; 2: rounded to whole levels first); 0: levels in / out.
    (The CPU works in doubles; floats here.)"""
    P = S.p
    vert = S.ne(P.vert, 0)
    n = S.select(vert, P.h, P.w)
    lines = S.select(vert, P.w, P.h)
    with S.if_(S.gx < lines):
        line = S.gx
        def idx(q): return S.select(vert, q * P.w + line, line * P.w + q)
        src = S.buf('in0')
        out = S.buf('out')
        def get(q):
            v = src.load(idx(q))
            sc = v * 255.0
            r = S.floor(S.min(S.max(sc, 0.0), 255.0) + 0.5)
            return S.select(S.ne(P.inlv, 0), S.select(S.ne(P.inlv, 1), r, sc), v)
        # (columns run from the bottom row up, as the CPU's)
        def pos(t): return S.select(vert, n - 1 - t, t)
        xL = get(pos(S.u(0)))
        x1 = S.var('v4', xL); x2 = S.var('v4', xL); x3 = S.var('v4', xL)
        y0 = xL * P.kn
        y1 = S.var('v4', y0); y2 = S.var('v4', y0); y3 = S.var('v4', y0); y4 = S.var('v4', y0)
        with S.for_(0, n) as t:
            x0 = get(pos(t))
            y = x0 * P.n0 + x1.get() * P.n1 + x2.get() * P.n2 + x3.get() * P.n3 - (y1.get() * P.d1 + y2.get() * P.d2 + y3.get() * P.d3 + y4.get() * P.d4)
            out.store(idx(pos(t)), y)
            x3.set(x2.get()); x2.set(x1.get()); x1.set(x0)
            y4.set(y3.get()); y3.set(y2.get()); y2.set(y1.get()); y1.set(y)
        xR = get(pos(n - 1))
        a1 = S.var('v4', xR); a2 = S.var('v4', xR); a3 = S.var('v4', xR); a4 = S.var('v4', xR)
        z0 = xR * P.km
        z1 = S.var('v4', z0); z2 = S.var('v4', z0); z3 = S.var('v4', z0); z4 = S.var('v4', z0)
        with S.for_(0, n) as t:
            q = pos(n - 1 - t)
            x0 = get(q)
            z = a1.get() * P.m1 + a2.get() * P.m2 + a3.get() * P.m3 + a4.get() * P.m4 - (z1.get() * P.d1 + z2.get() * P.d2 + z3.get() * P.d3 + z4.get() * P.d4)
            tot = out.load(idx(q)) + z
            r = S.min(S.max(S.floor(tot + 0.5), 0.0), 255.0)
            out.store(idx(q), S.select(S.ne(P.outlv, 0), S.select(S.ne(P.outlv, 1), r, tot) * (1.0 / 255.0), tot))
            a4.set(a3.get()); a3.set(a2.get()); a2.set(a1.get()); a1.set(x0)
            z4.set(z3.get()); z3.set(z2.get()); z2.set(z1.get()); z1.set(z)

@kernel("sharpen_mix", WH + [('t', 'f'), ('lv', 'u')], [PIC_IN, PIC_OUT])
def sharpen_mix(S):
    # sharpen() in builtins_filters.cpp: the pixel plus (12 pixel - the weighted 8 neighbours) * t, never above alpha, never below 0;
    # the edges repeat the border pixels. t = 1 / (17 - 16 Amount); the planner passes 0 for Amount 0 (a copy)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        inb = S.buf('in0')
        x0 = S.to_i(S.gx)
        y0 = S.to_i(S.gy)
        xl = S.to_u(S.max(x0 - 1, 0))
        xr = S.to_u(S.min(x0 + 1, S.to_i(P.w) - 1))
        yu = S.to_u(S.max(y0 - 1, 0))
        yd = S.to_u(S.min(y0 + 1, S.to_i(P.h) - 1))
        c = inb.load(i)
        a = c.w
        ul = inb.load(yu * P.w + xl)
        ur = inb.load(yu * P.w + xr)
        dl = inb.load(yd * P.w + xl)
        dr = inb.load(yd * P.w + xr)
        um = inb.load(yu * P.w + S.gx)
        dm = inb.load(yd * P.w + S.gx)
        ml = inb.load(S.gy * P.w + xl)
        mr = inb.load(S.gy * P.w + xr)
        def ch(cv, ulv, urv, dlv, drv, umv, dmv, mlv, mrv):
            corners = ulv + urv + dlv + drv
            sides = umv + dmv + mlv + mrv
            v = cv + (cv * 12.0 - corners - sides * 2.0) * P.t
            # (8-bit pictures: cut to whole levels, floor8() in builtins_impl.h)
            vc = S.min(S.max(v, 0.0), 2.0)
            q = S.floor(vc * 255.0 + 0.001) / 255.0
            return S.min(S.select(S.ne(P.lv, 0), q, S.max(v, 0.0)), a)
        S.buf('out').store(i, S.vec4(ch(c.x, ul.x, ur.x, dl.x, dr.x, um.x, dm.x, ml.x, mr.x), ch(c.y, ul.y, ur.y, dl.y, dr.y, um.y, dm.y, ml.y, mr.y),
                                     ch(c.z, ul.z, ur.z, dl.z, dr.z, um.z, dm.z, ml.z, mr.z), a))

def sample_on_bg(S, buf, w, h, x, y, bg):
    """sampleOnBg() in builtins_filters.cpp: bilinear at (x, y); the taps outside the frame are the colour bg (a vec4)."""
    xs, ys = x - 0.5, y - 0.5
    x0f, y0f = S.floor(xs), S.floor(ys)
    fx, fy = xs - x0f, ys - y0f
    x0, y0 = S.to_i(x0f), S.to_i(y0f)
    ix, iy = 1.0 - fx, 1.0 - fy
    wt = [ix * iy, fx * iy, ix * fy, fx * fy]
    taps = []
    for t in range(4):
        tx = x0 + (t & 1)
        ty = y0 + (t >> 1)
        inside = (tx >= 0) & (ty >= 0) & (tx < S.to_i(w)) & (ty < S.to_i(h))
        cx = S.to_u(S.clamp(tx, 0, S.to_i(w) - 1))
        cy = S.to_u(S.clamp(ty, 0, S.to_i(h) - 1))
        px = buf.load(cy * w + cx)
        taps.append(S.vec4(S.select(inside, px.x, bg.x), S.select(inside, px.y, bg.y), S.select(inside, px.z, bg.z), S.select(inside, px.w, bg.w)))
    acc = S.splat4(0.0) + taps[0] * wt[0]
    acc = acc + taps[1] * wt[1]
    acc = acc + taps[2] * wt[2]
    return acc + taps[3] * wt[3]

@kernel("mirror", WH + [('kx', 'f'), ('ky', 'f'), ('px0', 'f'), ('py0', 'f'), ('bgr', 'f'), ('bgg', 'f'), ('bgb', 'f'), ('bga', 'f'),
                        ('ex', 'f'), ('ey', 'f'), ('tie', 'f'), ('straight', 'u')], [PIC_IN, PIC_OUT])
def mirror(S):
    # mirror() in builtins_filters.cpp
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        xc = S.to_f(S.gx) + 0.5
        yc = S.to_f(S.gy) + 0.5
        d = (xc - P.px0) * P.kx + (yc - P.py0) * P.ky
        res = S.var('v4')
        bg = S.vec4(P.bgr, P.bgg, P.bgb, P.bga)
        with S.if_((d - P.tie) <= 0.000001):
            sx = xc - 2.0 * d * P.kx
            sy = yc - 2.0 * d * P.ky
            with S.if_(S.ne(P.straight, 0)):
                oob = (sx < -0.5000011) | (sy < -0.5000011) | (sx > S.to_f(P.w) + 0.5000011) | (sy > S.to_f(P.h) + 0.5000011)
                with S.if_(oob):
                    res.set(bg)
                with S.else_():
                    res.set(sample_clamped(S, S.buf('in0'), P.w, P.h, sx, sy))
            with S.else_():
                res.set(sample_on_bg(S, S.buf('in0'), P.w, P.h, sx + P.ex, sy + P.ey, bg))
        with S.else_():
            res.set(S.buf('in0').load(i))
        S.buf('out').store(i, res.get())

# ---------------------------------------------------------------------------------------------------------- glow, chroma key
@kernel("glow_mask", WH, [PIC_IN, PLANE_OUT])
def glow_mask(S):
    # glow() in builtins_filters.cpp: the average of R, G, B times alpha (blurred next; Suppression is taken off after the blur)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        r, g, b, a, valid = unpremul(S, S.buf('in0').load(i))
        l = (r + g + b) * (1.0 / 3.0)
        S.buf('out').store(i, l * a)

@kernel("glow_mix", WH + [('gain', 'f'), ('thr', 'f'), ('cr', 'f'), ('cg', 'f'), ('cb', 'f')], [PIC_IN, PLANE_IN1, PIC_OUT])
def glow_mix(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        k = clamp01(S, (S.buf('in1').load(i) - P.thr) * P.gain)
        ik = 1.0 - k
        S.buf('out').store(i, S.vec4(P.cr * k + s.x * ik, P.cg * k + s.y * ik, P.cb * k + s.z * ik, k + s.w * ik))

@kernel("chroma_mask", WH + [('kr', 'f'), ('kg', 'f'), ('kb', 'f'), ('kh', 'f'), ('ks', 'f'), ('kl', 'f'), ('low', 'f'), ('cut', 'u')], [PIC_IN, PLANE_OUT])
def chroma_mask(S):
    # chromaKeyDistance() and chromaKeyAlpha8() in builtins_filters.cpp: the 8-bit alpha (0..255) of a pixel; the key's HSL
    # (kh in degrees) comes from the planner, cut: the thresholds make a hard cut at 'low'
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        r0, g0, b0, a, valid = unpremul(S, S.buf('in0').load(i))
        r = clamp01(S, r0)
        g = clamp01(S, g0)
        b = clamp01(S, b0)
        mx = S.max(r, S.max(g, b))
        mn = S.min(r, S.min(g, b))
        d = mx - mn
        has = d > 0.0
        dd = S.max(d, S.f(1e-20))
        hr = (g - b) / dd
        hr = S.select(hr < 0.0, hr + 6.0, hr)
        h = S.select(S.eq(mx, r), hr, S.select(S.eq(mx, g), (b - r) / dd + 2.0, (r - g) / dd + 4.0))
        h = S.select(has, h * 60.0, S.f(0.0))
        l = (mx + mn) * 0.5
        sden = S.select(l <= 0.5, mx + mn, 2.0 - mx - mn)
        sat = S.select(has, d / S.max(sden, S.f(1e-20)), S.f(0.0))
        dh = S.abs(h - P.kh)
        dh = S.min(dh, 360.0 - dh)
        dr = r - P.kr
        dg = g - P.kg
        db = b - P.kb
        dist = 0.8660254037844386 * S.sqrt(dr * dr + dg * dg + db * db) + (dh / 180.0 + S.abs(sat - P.ks) + S.abs(l - P.kl)) / 6.0
        hard = S.select(dist >= P.low, S.f(1.0), S.f(0.0))
        ramp = S.min(S.max((dist - P.low) / S.max(1.0 - P.low, S.f(1e-6)), S.f(0.0)), S.f(1.0))
        al = S.select(S.ne(P.cut, 0), hard, ramp)
        S.buf('out').store(i, S.min(S.f(255.0), S.floor(al * 255.0 + 1e-6)))

@kernel("chroma_mix", WH + [('maskonly', 'u'), ('despill', 'u'), ('blurred', 'u')], [PIC_IN, PLANE_IN1, PIC_OUT])
def chroma_mix(S):
    # the end of chromaKeyer(): the 8-bit alpha (a blurred one truncated a little under), the straight colour (despilled)
    # times it truncated to 8 bits, or the alpha as grey
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        m0 = S.buf('in1').load(i)
        m8 = S.min(S.f(255.0), S.max(S.f(0.0), S.select(S.ne(P.blurred, 0), S.floor(m0 - 0.005), m0)))
        r, g, b, a, valid = unpremul(S, s)
        bb = S.select(S.eq(P.despill, 1), S.min(b, g), b)
        gg = S.select(S.eq(P.despill, 2), S.min(g, r), g)
        ma = m8 / 255.0 * a
        def q(v): return S.floor(v * m8 + 1e-3) / 255.0 * a
        shown = S.vec4(ma, ma, ma, ma)
        keyed = S.vec4(q(r), q(gg), q(bb), ma)
        S.buf('out').store(i, S.select(S.ne(P.maskonly, 0), shown, keyed))

# ---------------------------------------------------------------------------------------------------------- convolution kernel
@kernel("unpremul_px", WH, [PIC_IN, PIC_OUT])
def unpremul_px(S):
    # the straight colour (0 where alpha is tiny), alpha kept: what convolutionKernel() works on
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        a = p.w
        has = a > 1e-6
        S.buf('out').store(i, S.vec4(S.select(has, p.x / a, S.f(0.0)), S.select(has, p.y / a, S.f(0.0)), S.select(has, p.z / a, S.f(0.0)), a))

@kernel("conv3x3", WH + [('k0', 'f'), ('k1', 'f'), ('k2', 'f'), ('k3', 'f'), ('k4', 'f'), ('k5', 'f'), ('k6', 'f'), ('k7', 'f'), ('k8', 'f'),
                         ('cr', 'f'), ('sr', 'f'), ('scale', 'f'), ('mode', 'u'), ('mul', 'f'), ('off', 'f'), ('lv', 'u')], [PIC_IN, PIC_OUT])
def conv3x3(S):
    # convolutionKernel(): the taps in the plain loop's order (a tap of 0 adds nothing); mode 0: whole-pixel neighbours (edges clamped),
    # 1: turned (neighbours read between the pixels), 2: a smaller frame (the offsets are a fraction of a pixel)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        inb = S.buf('in0')
        xc = S.to_f(S.gx) + 0.5
        yc = S.to_f(S.gy) + 0.5
        acc = S.var('v4', S.splat4(0.0))
        ks = [P.k0, P.k1, P.k2, P.k3, P.k4, P.k5, P.k6, P.k7, P.k8]
        for t in range(9):
            row, col = t // 3, t % 3
            fdx, fdy = float(col - 1), float(row - 1)
            with S.if_(S.ne(ks[t], 0.0)):
                tap = S.var('v4')
                with S.if_(S.eq(P.mode, 0)):
                    nx = S.to_u(S.clamp(S.to_i(S.gx) + (col - 1), 0, S.to_i(P.w) - 1))
                    ny = S.to_u(S.clamp(S.to_i(S.gy) + (row - 1), 0, S.to_i(P.h) - 1))
                    tap.set(inb.load(ny * P.w + nx))
                with S.else_():
                    ox = fdx * P.cr - fdy * P.sr
                    oy = fdx * P.sr + fdy * P.cr
                    sc = S.select(S.eq(P.mode, 2), P.scale, S.f(1.0))
                    tap.set(sample_clamped(S, inb, P.w, P.h, xc + ox * sc, yc + oy * sc))
                t4 = tap.get()
                # (the taps in levels: a value that is a whole level up to float noise is made that level - as the CPU does)
                def lev(v):
                    t_ = v * 255.0
                    r_ = S.floor(t_ + 0.5)
                    return S.select(S.ne(P.lv, 0) & (S.abs(t_ - r_) < 0.002), r_, t_)
                acc.set(S.vec4(acc.get().x + ks[t] * lev(t4.x), acc.get().y + ks[t] * lev(t4.y), acc.get().z + ks[t] * lev(t4.z), 0.0))
        a = inb.load(i).w
        c = acc.get()
        def fin(v):
            r = v * P.mul + P.off
            q = S.floor(S.min(S.max(r, 0.0), 255.0))
            return (S.select(S.ne(P.lv, 0), q, r) / 255.0) * a
        S.buf('out').store(i, S.vec4(fin(c.x), fin(c.y), fin(c.z), a))

# ---------------------------------------------------------------------------------------------------------- more colour effects
@kernel("colorcorrector", WH + [('sat', 'f'), ('gain', 'f'), ('gc', 'f'), ('ga', 'f'), ('gamma_on', 'u'), ('off', 'f'), ('du0', 'f'), ('du1', 'f'), ('du2', 'f'),
                                ('dv0', 'f'), ('dv1', 'f'), ('dv2', 'f')], [PIC_IN, PIC_OUT])
def colorcorrector(S):
    # colorCorrectorRows in builtins_filters2.cpp
    P = S.p
    def body(S, r, g, b):
        Y = 0.299 * r + 0.587 * g + 0.114 * b
        U = 0.492 * (b - Y)
        V = 0.877 * (r - Y)
        yp = S.max(Y * P.gain, 0.0)
        base = (yp + P.gc) / (1.0 + P.gc)
        yg = S.select(base > 0.0, S.pow(base, P.ga), S.f(0.0))
        yp = S.select(S.ne(P.gamma_on, 0), yg, yp) + P.off
        yp = S.min(S.max(yp, 0.0), 1.0)
        e0 = yp / 0.402
        e1 = (yp - 0.5) / 0.2414
        e2 = (yp - 1.0) / 0.402
        wl = S.exp(-(e0 * e0))
        wm = S.exp(-(e1 * e1))
        wh = S.exp(-(e2 * e2))
        u2 = (U + P.du0 * wl + P.du1 * wm + P.du2 * wh) * P.sat
        v2 = (V + P.dv0 * wl + P.dv1 * wm + P.dv2 * wh) * P.sat
        nr = yp + v2 / 0.877
        nb = yp + u2 / 0.492
        ng = (yp - 0.299 * nr - 0.114 * nb) / 0.587
        return clamp01(S, nr), clamp01(S, ng), clamp01(S, nb)
    pixel_row(S, body)

@kernel("saturationadjust", WH + [('amount', 'f'), ('centre', 'f'), ('halfspread', 'f'), ('low', 'f'), ('high', 'f')], [PIC_IN, PIC_OUT])
def saturationadjust(S):
    # saturationAdjust() in builtins_more_colour.cpp (centre and halfspread arrive multiplied by the 0.9272 scale)
    P = S.p
    def body(S, r, g, b):
        y = 0.299 * r + 0.587 * g + 0.114 * b
        dr, dg, db = r - y, g - y, b - y
        x = S.sqrt(dr * dr + dg * dg + db * db)
        t = clamp01(S, S.abs(x - P.centre) / P.halfspread)
        bump = 1.0 - t * t * (3.0 - 2.0 * t)
        side = S.select(x < P.centre, P.low, S.select(x > P.centre, P.high, S.f(0.0)))
        f = 1.0 + P.amount * bump + side * (1.0 - bump)
        def cut(v):
            return S.floor(clamp01(S, v) * 255.0 + 0.001) * (1.0 / 255.0)
        return cut(y + dr * f), cut(y + dg * f), cut(y + db * f)
    pixel_row(S, body)

def srgb_to_lin(S, v):
    return S.select(v <= 0.04045, v / 12.92, S.pow((v + 0.055) / 1.055, 2.4))

def lin_to_srgb(S, v):
    return S.select(v <= 0.0031308, v * 12.92, 1.055 * S.pow(S.max(v, 0.0), S.f(1.0) / S.f(2.4)) - 0.055)

@kernel("labadjust", WH + [('light', 'f'), ('da', 'f'), ('db', 'f'), ('des', 'f'), ('invert', 'u')], [PIC_IN, PIC_OUT])
def labadjust(S):
    # labAdjust() in builtins_more_colour.cpp (the 8-bit result is truncated like there)
    P = S.p
    def body(S, r, g, b):
        lr, lg, lb = (srgb_to_lin(S, clamp01(S, c)) for c in (r, g, b))
        X = (0.4124564 * lr + 0.3575761 * lg + 0.1804375 * lb) / 0.95047
        Y = 0.2126729 * lr + 0.7151522 * lg + 0.0721750 * lb
        Z = (0.0193339 * lr + 0.1191920 * lg + 0.9503041 * lb) / 1.08883
        def f(t): return S.select(t > 216.0 / 24389.0, S.pow(t, S.f(1.0) / S.f(3.0)), (24389.0 / 27.0 * t + 16.0) / 116.0)
        X, Y, Z = f(X), f(Y), f(Z)
        L = 116.0 * Y - 16.0
        A = 500.0 * (X - Y)
        B = 200.0 * (Y - Z)
        L = S.select(S.ne(P.invert, 0), 95.58 - L, L) * P.light
        A = (A + P.da * 100.0) * (1.0 - P.des)
        B = (B + P.db * 100.0) * (1.0 - P.des)
        Y = (L + 16.0) / 116.0
        X = Y + A / 500.0
        Z = Y - B / 200.0
        def fi(t):
            t3 = t * t * t
            return S.select(t3 > 216.0 / 24389.0, t3, (116.0 * t - 16.0) / (24389.0 / 27.0))
        X = fi(X) * 0.95047
        Y = fi(Y)
        Z = fi(Z) * 1.08883
        lr2 = 3.24045484 * X - 1.53713885 * Y - 0.49853155 * Z
        lg2 = -0.96926639 * X + 1.87601093 * Y + 0.04155608 * Z
        lb2 = 0.05564342 * X - 0.20402585 * Y + 1.05722516 * Z
        def q(v):
            return S.floor(clamp01(S, lin_to_srgb(S, v)) * 255.0 + 0.001) * (1.0 / 255.0)
        return q(lr2), q(lg2), q(lb2)
    pixel_row(S, body)

@kernel("blackrestore", WH + [('thr', 'f')], [PIC_IN, PIC_OUT])
def blackrestore(S):
    P = S.p
    def body(S, r, g, b):
        m = S.max(r, S.max(g, b))
        low = m <= P.thr
        z = S.f(0.0)
        return S.select(low, z, r), S.select(low, z, g), S.select(low, z, b)
    pixel_row(S, body)

@kernel("channelblend", WH + [('flags', 'u'), ('pad', 'u')] + [('w%d' % i, 'f') for i in range(20)], [PIC_IN, PIC_OUT])
def channelblend(S):
    # channelBlend() in builtins_more_colour.cpp: no zeroing of pixels without alpha here
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        ins = [p.x, p.y, p.z, p.w]
        outs = []
        for o in range(4):
            ws = [getattr(P, 'w%d' % (o * 5 + k)) for k in range(5)]
            sm = ws[4]
            wsum = S.abs(ws[4])
            for k in range(4):
                sm = sm + ws[k] * ins[k]
                wsum = wsum + S.abs(ws[k])
            sm = S.select(S.ne(P.flags, 0), S.select(wsum > 1e-6, sm / S.max(wsum, S.f(1e-6)), S.f(0.0)), sm)
            outs.append(clamp01(S, sm))
        S.buf('out').store(i, S.vec4(outs[0], outs[1], outs[2], outs[3]))

@kernel("vignette", WH + [('px0', 'f'), ('py0', 'f'), ('cs', 'f'), ('sn', 'f'), ('ix', 'f'), ('iy', 'f'), ('inner', 'f'), ('invspan', 'f'),
                          ('expo', 'f'), ('invert', 'u'), ('cr', 'f'), ('cg', 'f'), ('cb', 'f'), ('ca', 'f')], [PIC_IN, PIC_OUT])
def vignette(S):
    # vignette() in builtins_more_creative.cpp
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        u = S.to_f(S.gx) + 0.5 - P.px0
        v = S.to_f(S.gy) + 0.5 - P.py0
        ru = (u * P.cs + v * P.sn) * P.ix
        rv = (-u * P.sn + v * P.cs) * P.iy
        d = S.sqrt(ru * ru + rv * rv)
        t = (d - P.inner) * P.invspan
        tp = S.max(t, S.f(1e-30))
        m = S.select(t <= 0.0, S.f(0.0), S.min(S.f(1.0), S.pow(tp, P.expo)))
        m2 = S.select(S.ne(P.invert, 0), 1.0 - m, m)
        k = m2 * P.ca
        S.buf('out').store(i, S.vec4(s.x * (1.0 - k) + P.cr * k * s.w, s.y * (1.0 - k) + P.cg * k * s.w, s.z * (1.0 - k) + P.cb * k * s.w, s.w))

# ---------------------------------------------------------------------------------------------------------- blur mixes, pixelate
@kernel("unsharp_mix", WH + [('amount', 'f'), ('thr', 'f'), ('lv', 'u')], [PIC_IN, PIC_IN1, PIC_OUT])
def unsharp_mix(S):
    # unsharpMask() in builtins_more_blur.cpp: in0 the frame, in1 its blur; every channel, in levels (whole ones for 8-bit pictures)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        bl = S.buf('in1').load(i)
        q = S.ne(P.lv, 0)
        res = []
        for ch in (0, 1, 2, 3):
            def lev(v): return S.select(q, S.floor(S.min(S.max(v, 0.0), 1.0) * 255.0 + 0.5), v * 255.0)
            sv, bv = lev(s[ch]), lev(bl[ch])
            v = S.min(S.max(sv + (sv - bv) * P.amount, 0.0), 255.0)
            v = S.select(q, S.trunc(v), v)
            res.append(S.select(S.abs(v - bv) > P.thr, v, sv) * (1.0 / 255.0))
        S.buf('out').store(i, S.vec4(res[0], res[1], res[2], res[3]))

@kernel("blend_mix", WH + [('k', 'f')], [PIC_IN, PIC_IN1, PIC_OUT])
def blend_mix(S):
    # blendOver() in builtins_more_internal.h: in0 the frame, in1 the effect's picture
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        s = S.buf('in0').load(i)
        e = S.buf('in1').load(i)
        mixed = s + (e - s) * P.k
        S.buf('out').store(i, S.select(P.k >= 0.999, e, mixed))

@kernel("pixelate_blocks", WH + [('nbx', 'u'), ('nby', 'u'), ('kx', 'u'), ('ky', 'u'), ('lv', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT, TABLE])
def pixelate_blocks(S):
    # one invocation per cell: the average of its window (pixelate() in builtins_filters.cpp). The table holds the cells' first pixels
    # across (nbx + 1 of them, the last one W) and then those down counted from the bottom row (nby + 1)
    P = S.p
    with S.in_range(P.nbx, P.nby):
        W, H = S.to_i(P.w), S.to_i(P.h)
        x0 = S.to_i(S.to_u(S.buf('table').load(S.gx)))
        y0 = S.to_i(S.to_u(S.buf('table').load(P.nbx + 1 + S.gy)))
        x1 = S.min(x0 + S.to_i(P.kx), W)
        y1 = S.min(y0 + S.to_i(P.ky), H)
        tot = S.var('v4', S.splat4(0.0))
        with S.for_(y0, y1, 1, True) as yb:
            row = S.var('v4', S.splat4(0.0))
            with S.for_(x0, x1, 1, True) as x:
                row.set(row.get() + S.buf('in0').load(S.to_u(H - 1 - yb) * P.w + S.to_u(x)))
            tot.set(tot.get() + row.get())
        n = S.to_f(x1 - x0) * S.to_f(y1 - y0)
        m = tot.get() / n
        def q(v):
            return S.select(S.ne(P.lv, 0), S.floor(S.min(S.max(v, 0.0), 2.0) * 255.0 + 0.001) / 255.0, v)
        S.buf('out').store(S.gy * P.nbx + S.gx, S.vec4(q(m.x), q(m.y), q(m.z), q(m.w)))

@kernel("pixelate_apply", WH + [('nbx', 'u'), ('pad', 'u')], [PIC_IN, PIC_OUT, TABLE])
def pixelate_apply(S):
    # each pixel takes its cell's value; the table holds the cell of every column and then of every row (counted from the top row)
    P = S.p
    with S.in_range(P.w, P.h):
        cx = S.to_u(S.buf('table').load(S.gx))
        cy = S.to_u(S.buf('table').load(P.w + S.gy))
        S.buf('out').store(S.gy * P.w + S.gx, S.buf('in0').load(cy * P.nbx + cx))

@kernel("linearblur", WH + [('n', 'u'), ('lv', 'u'), ('sx', 'f'), ('sy', 'f')], [PIC_IN, PIC_OUT, TABLE])
def linearblur(S):
    # linearBlur() in builtins_filters.cpp: in0 the frame smoothed by (1 4 1) / 6 across and down; the table the weights from the centre out
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        xf, yf = S.to_f(S.gx) + 0.5, S.to_f(S.gy) + 0.5
        acc = S.var('v4', sample_clamped(S, S.buf('in0'), P.w, P.h, xf, yf) * S.buf('table').load(S.u(0)))
        with S.for_(S.i(1), S.to_i(P.n) + 1, 1, True) as k:
            w = S.buf('table').load(S.to_u(k))
            kf = S.to_f(k)
            a = sample_clamped(S, S.buf('in0'), P.w, P.h, xf + kf * P.sx, yf + kf * P.sy)
            b = sample_clamped(S, S.buf('in0'), P.w, P.h, xf - kf * P.sx, yf - kf * P.sy)
            acc.set(acc.get() + (a + b) * w)
        v = acc.get()
        def q(x):
            return S.select(S.ne(P.lv, 0), S.floor(S.min(S.max(x, 0.0), 2.0) * 255.0 + 0.5) / 255.0, x)
        S.buf('out').store(i, S.vec4(q(v.x), q(v.y), q(v.z), q(v.w)))

# ---------------------------------------------------------------------------------------------------------- distortions
def sin_poly(S, x):
    """sin(x) to about 1e-7 on any argument (a GPU's own sin is only good to 2^-11 and only between -pi and pi): the argument is
    brought to -pi..pi, folded to -pi/2..pi/2, and a degree 11 series is summed."""
    twopi_hi, twopi_lo = 6.28125, 0.0019353071795864769      # 2 pi in two parts, so the reduction loses nothing
    k = S.floor(x * S.f(1.0 / 6.283185307179586) + 0.5)
    r = (x - k * twopi_hi) - k * twopi_lo
    hp = S.f(1.5707963267948966)
    r = S.select(r > hp, S.f(3.141592653589793) - r, S.select(r < -hp, S.f(-3.141592653589793) - r, r))
    r2 = r * r
    p = r * (1.0 + r2 * (-1.0 / 6.0 + r2 * (1.0 / 120.0 + r2 * (-1.0 / 5040.0 + r2 * (1.0 / 362880.0 + r2 * (-1.0 / 39916800.0))))))
    return p

def cos_poly(S, x):
    return sin_poly(S, x + S.f(1.5707963267948966))

def area_coords(S):
    P = S.p
    dx = (S.to_f(S.gx) + 0.5 - P.px0) / P.rx
    dy = (S.to_f(S.gy) + 0.5 - P.py0) / P.ry
    r = S.sqrt(dx * dx + dy * dy)
    return dx, dy, r

AREA = WH + [('px0', 'f'), ('py0', 'f'), ('rx', 'f'), ('ry', 'f')]

@kernel("spherize", WH + [('cxp', 'f'), ('cyp', 'f'), ('kx', 'f'), ('ky', 'f'), ('amount', 'f'), ('q8', 'u')], [PIC_IN, PIC_OUT])
def spherize(S):
    # spherize() in builtins_filters2.cpp (VEGAS's CPU processor): rows bottom first, a point inside the ellipse is read from the
    # centre + (dx, dy) * (1 - 0.8 Amount cos(r pi / 2)), bilinearly with 8-bit weights (8-bit pictures) or plainly (float ones)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        wf, hf = S.to_f(P.w), S.to_f(P.h)
        j = hf - 1.0 - S.to_f(S.gy)
        dy = j - P.cyp
        dx = S.to_f(S.gx) - P.cxp
        yy0 = 2.0 * dy / hf * P.ky
        xx = dx * P.kx
        r2 = xx * xx + yy0 * yy0
        r = S.sqrt(S.min(r2, 1.0))
        f = 1.0 - S.cos(r * 3.1415926 * 0.5) * P.amount * 0.8
        sy = f * dy + P.cyp
        sx = f * dx + P.cxp
        iy = S.floor(sy + 1.5e-8)
        ix = S.floor(sx + 1.5e-8)
        y0 = S.clamp(iy, 0.0, hf - 1.0)
        y1 = S.clamp(iy + 1.0, 0.0, hf - 1.0)
        x0 = S.clamp(ix, 0.0, wf - 1.0)
        x1 = S.clamp(ix + 1.0, 0.0, wf - 1.0)
        def at(xf, yf):
            return S.buf('in0').load(S.to_u(hf - 1.0 - yf) * P.w + S.to_u(xf))
        p00, p10, p01, p11 = at(x0, y0), at(x1, y0), at(x0, y1), at(x1, y1)
        frx, fry = sx - ix, sy - iy
        q = S.ne(P.q8, 0)
        fx = S.select(q, S.trunc(frx * 256.0) * (1.0 / 256.0), frx)
        fy = S.select(q, S.trunc(fry * 256.0) * (1.0 / 256.0), fry)
        def lv(p): return S.select(q, S.floor(p * 255.0 + 0.5), p * 255.0)
        a = (lv(p00) * (1.0 - fx) + lv(p10) * fx) * (1.0 - fy) + (lv(p01) * (1.0 - fx) + lv(p11) * fx) * fy
        v = S.select(q, S.floor(a * (1.0 + 1e-7)), a) * (1.0 / 255.0)
        S.buf('out').store(i, S.select(r2 >= 1.0, S.buf('in0').load(i), v))

@kernel("swirl", AREA + [('turn', 'f')], [PIC_IN, PIC_OUT])
def swirl(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        dx, dy, r = area_coords(S)
        f = (1.0 - r) * (1.0 - r)
        ang = P.turn * f
        ca, sa = cos_poly(S, ang), sin_poly(S, ang)
        ux, uy = dx * P.rx, dy * P.ry
        samp = sample_clamped(S, S.buf('in0'), P.w, P.h, P.px0 + ux * ca - uy * sa, P.py0 + ux * sa + uy * ca)
        S.buf('out').store(i, S.select(r >= 1.0, S.buf('in0').load(i), samp))

@kernel("pinchpunch", AREA + [('amount', 'f')], [PIC_IN, PIC_OUT])
def pinchpunch(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        dx, dy, r = area_coords(S)
        sn = sin_poly(S, S.f(3.141592653589793) * r)
        k = 1.0 - P.amount * 0.6 * sn * sn * (1.0 - r)
        samp = sample_clamped(S, S.buf('in0'), P.w, P.h, P.px0 + dx * k * P.rx, P.py0 + dy * k * P.ry)
        S.buf('out').store(i, S.select((r >= 1.0) | (r < 1e-12), S.buf('in0').load(i), samp))

@kernel("wave", WH + [('vn', 'f'), ('hn', 'f'), ('va', 'f'), ('ha', 'f'), ('vp', 'f'), ('hp', 'f'), ('invw', 'f'), ('invh', 'f')], [PIC_IN, PIC_OUT])
def wave(S):
    # wave() in builtins_distort.cpp; the sine of 2 pi t is taken of the fractional part of t
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        xc, yc = S.to_f(S.gx) + 0.5, S.to_f(S.gy) + 0.5
        fy = yc * P.invh
        fx = xc * P.invw
        ty = P.hn * fy + P.hp
        tx = P.vn * fx + P.vp
        twopi = S.f(6.283185307179586)
        sx = P.ha * sin_poly(S, (ty - S.floor(ty)) * twopi)
        sy = P.va * sin_poly(S, (tx - S.floor(tx)) * twopi)
        S.buf('out').store(i, sample_clamped(S, S.buf('in0'), P.w, P.h, xc + sx, yc + sy))

# ---------------------------------------------------------------------------------------------------------- noise, border, white balance, min and max
def hash3(S, x, y, z):
    """hash3() in builtins_filters.cpp: three integers mixed to one."""
    h = x * S.u(374761393) + y * S.u(668265263) + z * S.u(2147483647)
    h = (h ^ (h >> 13)) * S.u(1274126177)
    return h ^ (h >> 16)

@kernel("addnoise", WH + [('frame', 'u'), ('level', 'f'), ('mono', 'u'), ('gauss', 'u')], [PIC_IN, PIC_OUT])
def addnoise(S):
    P = S.p
    unit = S.f(4294967295.0)
    def rnd(x, y, k):
        h = hash3(S, x, y, P.frame * S.u(3) + S.u(k))
        flat = S.to_f(h) / unit * 2.0 - 1.0
        sm = S.f(0.0)
        for i in range(6):
            sm = sm + S.to_f(hash3(S, h, S.u(i), S.u(k))) / unit
        bell = (sm - 3.0) * 2.82842712 - 1.0
        return S.select(S.ne(P.gauss, 0), bell, flat)
    def body(S, r, g, b):
        n0 = rnd(S.gx, S.gy, 0) * P.level * 0.5
        mono = S.ne(P.mono, 0)
        n1 = S.select(mono, n0, rnd(S.gx, S.gy, 1) * P.level * 0.5)
        n2 = S.select(mono, n0, rnd(S.gx, S.gy, 2) * P.level * 0.5)
        return clamp01(S, r + n0), clamp01(S, g + n1), clamp01(S, b + n2)
    pixel_row(S, body)

@kernel("border", WH + [('type', 'u'), ('size', 'f'), ('cr', 'f'), ('cg', 'f'), ('cb', 'f'), ('ca', 'f'), ('lx', 'f'), ('ly', 'f')], [PIC_IN, PIC_OUT])
def border(S):
    # border() in builtins_filters.cpp
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        o = S.buf('in0').load(i)
        xc, yc = S.to_f(S.gx) + 0.5, S.to_f(S.gy) + 0.5
        dl, dr = xc, S.to_f(P.w) - xc
        dt, db = yc, S.to_f(P.h) - yc
        d = S.min(S.min(dl, dr), S.min(dt, db))
        res = S.var('v4', o)
        with S.if_(d < P.size):
            with S.if_(S.eq(P.type, 2)):
                k = P.ca
                res.set(S.vec4(P.cr * k + o.x * (1.0 - k), P.cg * k + o.y * (1.0 - k), P.cb * k + o.z * (1.0 - k), k + o.w * (1.0 - k)))
            with S.else_():
                with S.if_(S.eq(P.type, 0)):
                    t = S.clamp(d / P.size, 0.0, 1.0)
                    k = t * t * (3.0 - 2.0 * t)
                    res.set(o * k)
                with S.else_():
                    nx = S.select(S.eq(d, dl), S.f(-1.0), S.select(S.eq(d, dr), S.f(1.0), S.f(0.0)))
                    ny = S.select(S.eq(d, dl) | S.eq(d, dr), S.f(0.0), S.select(S.eq(d, dt), S.f(-1.0), S.f(1.0)))
                    lit = -(nx * P.lx + ny * P.ly)
                    k = (1.0 - d / P.size) * P.ca
                    r, g, b, al, valid = unpremul(S, o)
                    pos = lit > 0.0
                    sc = 1.0 + k * lit
                    r2 = S.select(pos, r + (P.cr - r) * k * lit, r * sc)
                    g2 = S.select(pos, g + (P.cg - g) * k * lit, g * sc)
                    b2 = S.select(pos, b + (P.cb - b) * k * lit, b * sc)
                    res.set(S.vec4(r2 * al, g2 * al, b2 * al, al))
        S.buf('out').store(i, res.get())

@kernel("whitebalance", WH + [('rm', 'f'), ('bm', 'f'), ('gm', 'f'), ('r2', 'f'), ('b2', 'f'), ('bright', 'f'), ('post', 'f')], [PIC_IN, PIC_OUT])
def whitebalance(S):
    # whiteBalance() in builtins_more_colour.cpp without the histogram equalising (post: the stretch of HasData without a histogram)
    P = S.p
    def body(S, r, g, b):
        y0 = 0.299 * r + 0.587 * g + 0.114 * b
        dy = clamp01(S, P.bright * y0) - y0
        r = r + dy
        g = g + dy
        b = b + dy
        r = r * P.rm
        b = b * P.bm
        g = g * P.gm
        r = r * P.r2
        b = b * P.b2
        return clamp01(S, clamp01(S, r) * P.post), clamp01(S, clamp01(S, g) * P.post), clamp01(S, clamp01(S, b) * P.post)
    pixel_row(S, body)

@kernel("minmax", WH + [('ry', 'u'), ('maximum', 'u')], [PIC_IN, PIC_OUT, TABLE])
def minmax(S):
    # neighbourhood(c, false, maximum) in builtins_more_blur.cpp: the table holds each row's half width (the round shape's, worked out on the host)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        inb = S.buf('in0')
        mx = S.ne(P.maximum, 0)
        acc = S.var('v4', S.splat4(0.0))
        first = S.var('u', S.u(1))
        ry = S.to_i(P.ry)
        with S.for_(-ry, ry + 1, 1, True) as j:
            hw = S.to_i(S.to_u(S.buf('table').load(S.to_u(j + ry))))
            yy = S.to_u(S.clamp(S.to_i(S.gy) + j, 0, S.to_i(P.h) - 1))
            with S.for_(-hw, hw + 1, 1, True) as k:
                xx = S.to_u(S.clamp(S.to_i(S.gx) + k, 0, S.to_i(P.w) - 1))
                v = inb.load(yy * P.w + xx)
                cur = acc.get()
                better = S.select(mx, S.max(cur, v), S.min(cur, v))
                acc.set(S.select(S.ne(first.get(), 0), v, better))
                first.set(S.u(0))
        a = acc.get()
        S.buf('out').store(i, S.vec4(S.min(S.max(a.x, 0.0), a.w), S.min(S.max(a.y, 0.0), a.w), S.min(S.max(a.z, 0.0), a.w), a.w))

# ---------------------------------------------------------------------------------------------------------- gradient map, mask generator, broadcast colors, chroma blur
@kernel("gradientmap", WH + [('amount', 'f'), ('add', 'u')], [PIC_IN, PIC_OUT])
def gradientmap(S):
    P = S.p
    def body(S, r, g, b):
        l = luma(S, r, g, b)
        gr, gg, gb = clamp01(S, l * 3.0), clamp01(S, l * 3.0 - 1.0), clamp01(S, l * 3.0 - 2.0)
        add = S.ne(P.add, 0)
        return (S.select(add, clamp01(S, r + (gr - 0.5) * P.amount), r + (gr - r) * P.amount),
                S.select(add, clamp01(S, g + (gg - 0.5) * P.amount), g + (gg - g) * P.amount),
                S.select(add, clamp01(S, b + (gb - 0.5) * P.amount), b + (gb - b) * P.amount))
    pixel_row(S, body)

@kernel("maskgenerator", WH + [('type', 'u'), ('invert', 'u'), ('li', 'f'), ('hi', 'f'), ('lo', 'f'), ('ho', 'f'), ('lv', 'u')], [PIC_IN, PIC_OUT])
def maskgenerator(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        r, g, b, a, valid = unpremul(S, S.buf('in0').load(i))
        v = S.select(S.eq(P.type, 0), (r + g + b) * (1.0 / 3.0), S.select(S.eq(P.type, 1), r, S.select(S.eq(P.type, 2), g, S.select(S.eq(P.type, 3), b, a))))
        v = S.select(S.ne(P.lv, 0), S.floor(v * 255.0 + 0.5) / 255.0, v)
        v = S.select(S.ne(P.invert, 0), 1.0 - v, v)
        t = S.select(P.hi > P.li, clamp01(S, (v - P.li) / S.max(S.f(1e-9), P.hi - P.li)), S.select(v > P.li, S.f(1.0), S.f(0.0)))
        v = clamp01(S, P.lo + (P.ho - P.lo) * t)
        S.buf('out').store(i, S.vec4(v, v, v, v))

@kernel("broadcastcolors", WH + [('ymin', 'f'), ('ymax', 'f'), ('symin', 'f'), ('symax', 'f'), ('clim', 'f'), ('sc', 'f'), ('cmin', 'f'), ('cmax', 'f'),
                                  ('scmin', 'f'), ('scmax', 'f')], [PIC_IN, PIC_OUT])
def broadcastcolors(S):
    # broadcastColors() in builtins_more_colour.cpp
    P = S.p
    def soft_low(x, L, T):
        # broadcastSoftLow(): x <= 0 -> L, x >= T -> x, a power curve between (T <= L: a hard limit)
        knee = T > L + 1e-7
        tl = S.max(T - L, S.f(1e-7))
        curve = L + tl * S.pow(S.min(S.f(1.0), S.max(S.f(0.0), x / S.max(T, S.f(1e-7)))), T / tl)
        return S.select(knee, S.select(x >= T, x, curve), S.max(x, L))
    def body(S, r, g, b):
        y0 = 0.299 * r + 0.587 * g + 0.114 * b
        y1 = S.select(P.ymin > 0.0, soft_low(y0, P.ymin, P.ymin * (1.0 + P.symin)), S.max(y0, P.ymin))
        y = S.select(P.ymax < 1.0, 1.0 - soft_low(1.0 - y1, 1.0 - P.ymax, (1.0 - P.ymax) * (1.0 + P.symax)), S.min(y1, P.ymax))
        d = y - y0
        cb = b + d - y
        cr = r + d - y
        p1 = 0.492 * cb
        p2 = 0.877 * cr
        uv = S.sqrt(p1 * p1 + p2 * p2)
        top = 0.63214
        uv1 = S.select(P.clim < top, top - soft_low(top - uv, top - P.clim, (top - P.clim) * (1.0 + P.sc)), S.min(uv, P.clim))
        f = S.select(uv > 1e-9, uv1 / S.max(uv, S.f(1e-9)), S.f(1.0))
        uv2 = uv * f
        hi0 = y + uv2
        lo0 = y - uv2
        wh = 0.2 * P.scmax * S.max(S.f(0.0), P.cmax - y)
        wl = 0.2 * P.scmin * S.max(S.f(0.0), y - P.cmin)
        th = P.cmax + wh - hi0
        hi_k = S.select(hi0 >= P.cmax + wh, P.cmax, S.select(hi0 > P.cmax - wh, P.cmax - th * th / (4.0 * S.max(wh, S.f(1e-9))), hi0))
        hi = S.select(wh > 1e-9, hi_k, S.min(hi0, P.cmax))
        tl2 = lo0 - P.cmin + wl
        lo_k = S.select(lo0 <= P.cmin - wl, P.cmin, S.select(lo0 < P.cmin + wl, P.cmin + tl2 * tl2 / (4.0 * S.max(wl, S.f(1e-9))), lo0))
        lo = S.select(wl > 1e-9, lo_k, S.max(lo0, P.cmin))
        uc = S.max(uv2, S.f(1e-9))
        s = S.min(S.f(1.0), S.max(S.f(0.0), S.min((hi - y) / uc, (y - lo) / uc)))
        f = S.select(uv2 > 1e-9, f * s, f)
        cb = cb * f
        cr = cr * f
        r2 = y + cr
        b2 = y + cb
        g2 = (y - 0.299 * r2 - 0.114 * b2) / 0.587
        return clamp01(S, r2), clamp01(S, g2), clamp01(S, b2)
    pixel_row(S, body)

@kernel("chroma_split", WH, [PIC_IN, PIC_OUT])
def chroma_split(S):
    # chromaBlur() in builtins_more_blur.cpp: (luma, blue difference, red difference, alpha) per pixel
    with S.in_range(S.p.w, S.p.h):
        i = S.gy * S.p.w + S.gx
        r, g, b, a, valid = unpremul(S, S.buf('in0').load(i))
        y = 0.114 * r + 0.587 * g + 0.299 * b
        S.buf('out').store(i, S.vec4(y, b - y, r - y, a))

@kernel("chroma_join", WH + [('lv', 'u')], [PIC_IN, PIC_OUT])
def chroma_join(S):
    with S.in_range(S.p.w, S.p.h):
        i = S.gy * S.p.w + S.gx
        t = S.buf('in0').load(i)
        a = t.w
        r = t.x + t.z
        b = t.x + t.y
        g = (t.x - 0.114 * r - 0.299 * b) / 0.587
        def q(v):
            return S.select(S.ne(S.p.lv, 0), S.floor(S.min(S.max(v, 0.0), 2.0) * 255.0 + 0.001) / 255.0, clamp01(S, v))
        S.buf('out').store(i, S.vec4(q(r) * a, q(g) * a, q(b) * a, a))


# ---------------------------------------------------------------------------------------------------------- transitions
# A transition's outgoing frame is buffer 0 (in0) like any effect's picture; the incoming frame comes as the table (w * h * 4
# floats, premultiplied, top row first). The kernels mirror the functions of builtins_trans.cpp.
def to_px(S, i):
    t = S.buf('table')
    b = i * 4
    return S.vec4(t.load(b), t.load(b + 1), t.load(b + 2), t.load(b + 3))

def sample_edge(S, load, w, h, x, y):
    """Buf::sample for a position in pixels (centres at +0.5): bilinear, taps outside the frame are zero, nothing beyond a pixel out."""
    wf, hf = S.to_f(w), S.to_f(h)
    xs, ys = x - 0.5, y - 0.5
    x0f, y0f = S.floor(xs), S.floor(ys)
    fx, fy = xs - x0f, ys - y0f
    near = (xs >= -1.0) & (ys >= -1.0) & (xs <= wf) & (ys <= hf)
    acc = S.splat4(0.0)
    for j in (0, 1):
        for i in (0, 1):
            xx = x0f + float(i)
            yy = y0f + float(j)
            inside = near & (xx >= 0.0) & (yy >= 0.0) & (xx < wf) & (yy < hf)
            wgt = (fx if i else 1.0 - fx) * (fy if j else 1.0 - fy)
            idx = S.to_u(S.max(yy, 0.0)) * w + S.to_u(S.max(xx, 0.0))
            acc = acc + S.select(inside, load(idx) * wgt, S.splat4(0.0))
    return acc

def smooth01(S, t):
    t = S.min(S.max(t, 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)

def mix_px(S, a, b, t):
    return a + (b - a) * t

def border_ring(S, o, P, am, R, Wd):
    """The border strip's soft ring over the picture o: am is the distance from the ring's centre line."""
    t = smooth01(S, (R - S.abs(am)) / Wd + 0.5)
    return mix_px(S, o, S.vec4(P.bcr, P.bcg, P.bcb, P.bca), t)

@kernel("wipe_linear", WH + [('dx', 'f'), ('dy', 'f'), ('span', 'f'), ('fw', 'f'), ('edge', 'f')], [PIC_IN, PIC_OUT, TABLE])
def wipe_linear(S):
    # linearWipe(): s runs 0..1 along the angle, the frame arrives where s < edge, with a smoothstep of width w
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        xc = S.to_f(S.gx) + 0.5
        yc = S.to_f(S.gy) + 0.5
        s = ((xc - S.to_f(P.w) * 0.5) * P.dx + (yc - S.to_f(P.h) * 0.5) * P.dy) / P.span + 0.5
        hard = S.select(s < P.edge, S.f(1.0), S.f(0.0))
        t = S.select(P.fw <= 1e-9, hard, smooth01(S, (P.edge - s) / P.fw))
        S.buf('out').store(i, mix_px(S, S.buf('in0').load(i), to_px(S, i), t))

@kernel("wipe_clock", WH + [('ccw', 'u'), ('pad', 'u'), ('f', 'f'), ('edge', 'f'), ('nex', 'f'), ('ney', 'f'), ('uex', 'f'), ('uey', 'f')], [PIC_IN, PIC_OUT, TABLE])
def wipe_clock(S):
    # clockWipe(): the angle from 12 o'clock in degrees about VEGAS's centre (half a pixel right of and a pixel below the
    # middle; counter-clockwise mirrors the picture); a soft edge is a smoothstep of width f centred on the hand, a hard
    # one the anti-aliased wedge between the 12 o'clock ray and the hand (each moved the counter-clockwise way)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        xc = S.to_f(S.gx) + 0.5
        yc = S.to_f(S.gy) + 0.5
        cx = S.to_f(P.w) * 0.5 + 0.5
        cy = S.to_f(P.h) * 0.5 + 1.0
        dx = S.select(S.ne(P.ccw, 0), cx - xc, xc - cx)
        dy = yc - cy
        a0 = S.atan2(dx, -dy) * (180.0 / 3.14159265358979)
        a = S.select(a0 < 0.0, a0 + 360.0, a0)
        soft = smooth01(S, (P.edge - a) / S.max(P.f, 1e-9) + 0.5)
        hard = S.select(a < P.edge, S.f(1.0), S.f(0.0))
        d0 = dx + 0.5
        d1 = dx * P.nex + dy * P.ney - 0.71
        w0 = S.select(S.abs(d0) < 1.0, S.clamp(-dy - 0.5, 0.0, 1.0), S.f(0.0))
        w1 = S.select(S.abs(d1) < 1.0, S.clamp(dx * P.uex + dy * P.uey - 0.5, 0.0, 1.0), S.f(0.0))
        t0 = hard + (S.clamp(0.5 + d0, 0.0, 1.0) - hard) * w0
        t1 = hard + (S.clamp(0.5 + d1, 0.0, 1.0) - hard) * w1
        both = S.select(P.edge <= 180.0, S.min(t0, t1), S.max(t0, t1))
        h = S.select((w0 > 0.0) & (w1 > 0.0), both, S.select(w0 > 0.0, t0, S.select(w1 > 0.0, t1, hard)))
        h2 = S.select(P.edge <= 0.0, S.f(0.0), S.select(P.edge >= 360.0, S.f(1.0), h))
        t = S.select(P.f <= 1e-9, h2, soft)
        S.buf('out').store(i, mix_px(S, S.buf('in0').load(i), to_px(S, i), t))

PICK_BORDER = [('bcr', 'f'), ('bcg', 'f'), ('bcb', 'f'), ('bca', 'f')]

@kernel("push_frames", WH + [('horiz', 'u'), ('neg', 'u'), ('pushoff', 'u'), ('border', 'u'), ('seam', 'f'), ('centre', 'f'), ('shift', 'f'), ('len', 'f'),
                              ('R', 'f'), ('Wd', 'f')] + PICK_BORDER + [('early', 'u')], [PIC_IN, PIC_OUT, TABLE])
def push_frames(S):
    # pushFx()
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        xc = S.to_f(S.gx) + 0.5
        yc = S.to_f(S.gy) + 0.5
        horiz = S.ne(P.horiz, 0)
        neg = S.ne(P.neg, 0)
        border = S.ne(P.border, 0)
        a = S.select(horiz, xc, yc)
        one = S.select(S.ne(P.early, 0), S.f(1.0), S.f(0.0))          # (a hard border: the incoming frame starts a pixel early)
        inb = S.select(neg, a >= P.seam - one, a < P.seam)
        res = S.var('v4')
        with S.if_(inb):
            ib = S.select(neg, a - P.seam + one, a - P.seam + P.len)
            bx = S.select(horiz, ib, xc)
            by = S.select(horiz, yc, ib)
            res.set(sample_edge(S, lambda k: to_px(S, k), P.w, P.h, bx, by))
        with S.else_():
            fa = S.select(S.ne(P.pushoff, 0), S.select(neg, a + P.shift, a - P.shift), a)
            fx = S.select(horiz, fa, xc)
            fy = S.select(horiz, yc, fa)
            res.set(sample_edge(S, lambda k: S.buf('in0').load(k), P.w, P.h, fx, fy))
        o = res.get()
        o = S.select(border, border_ring(S, o, P, a - P.centre, P.R, P.Wd), o)
        S.buf('out').store(i, o)

@kernel("barndoor_frames", WH + [('along', 'u'), ('inward', 'u'), ('border', 'u'), ('hasw', 'u'), ('T', 'f'), ('len', 'f'), ('e', 'f'), ('cen', 'f'), ('R', 'f'), ('Wd', 'f')] + PICK_BORDER,
        [PIC_IN, PIC_OUT, TABLE])
def barndoor_frames(S):
    # barndoor()
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        xc = S.to_f(S.gx) + 0.5
        yc = S.to_f(S.gy) + 0.5
        alongx = S.ne(P.along, 0)
        border = S.ne(P.border, 0)
        a = S.select(alongx, xc, yc)
        far = a > P.T
        am = S.select(far, P.len - a, a)
        moving = am < P.e
        one = S.select(border, S.f(1.0), S.f(0.0))
        onec = S.select(S.ne(P.hasw & 1, 0), S.f(1.0), S.f(0.0))   # bit 0: a size; bit 1: a hard border (the far half a pixel late)
        onef = S.select(S.ne(P.hasw & 2, 0), S.f(1.0), S.f(0.0))
        sa0 = S.clamp(S.select(far, a - (P.T - P.e) + onef, a + (P.T - P.e) - onec), 0.5, P.len - 0.5)
        sa = S.select(moving, sa0, a)
        frompic = S.select(S.ne(P.inward, 0), ~moving, moving)
        sx = S.select(alongx, sa, xc)
        sy = S.select(alongx, yc, sa)
        res = S.var('v4')
        with S.if_(frompic):
            res.set(sample_edge(S, lambda k: S.buf('in0').load(k), P.w, P.h, sx, sy))
        with S.else_():
            res.set(sample_edge(S, lambda k: to_px(S, k), P.w, P.h, sx, sy))
        o = res.get()
        o = S.select(border, border_ring(S, o, P, am - (P.cen - one), P.R, P.Wd), o)
        S.buf('out').store(i, o)

@kernel("slide_frames", WH + [('inward', 'u'), ('border', 'u'), ('ox', 'f'), ('oy', 'f'), ('bw', 'f'), ('R', 'f'), ('Wd', 'f')] + PICK_BORDER, [PIC_IN, PIC_OUT, TABLE])
def slide_frames(S):
    # slideFx(): the moving frame is laid at (ox, oy); the border is a ring round (or inside) its edge
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        xc = S.to_f(S.gx) + 0.5
        yc = S.to_f(S.gy) + 0.5
        wf, hf = S.to_f(P.w), S.to_f(P.h)
        qx = xc - P.ox
        qy = yc - P.oy
        inside = (qx >= 0.0) & (qx < wf) & (qy >= 0.0) & (qy < hf)
        moving_in = S.ne(P.inward, 0)
        res = S.var('v4')
        with S.if_(inside):
            with S.if_(moving_in):
                res.set(sample_edge(S, lambda k: to_px(S, k), P.w, P.h, qx, qy))
            with S.else_():
                res.set(sample_edge(S, lambda k: S.buf('in0').load(k), P.w, P.h, qx, qy))
        with S.else_():
            with S.if_(moving_in):
                res.set(S.buf('in0').load(i))
            with S.else_():
                res.set(to_px(S, i))
        o = res.get()
        dx = S.max(S.max(-qx, qx - wf), 0.0)
        dy = S.max(S.max(-qy, qy - hf), 0.0)
        sd_out = S.sqrt(dx * dx + dy * dy)
        sd_in = -S.min(S.min(qx, wf - qx), S.min(qy, hf - qy))
        sd = S.select(inside, sd_in, sd_out)
        o = S.select(S.ne(P.border, 0), border_ring(S, o, P, sd - P.bw, P.R, P.Wd), o)
        S.buf('out').store(i, o)


def main():
    out = sys.argv[1]
    only = None
    if "--only" in sys.argv:
        only = set(sys.argv[sys.argv.index("--only") + 1].split(","))
    layouts = ["// Generated by tools/gpukernels.py: the push constants of every kernel, in the order the kernel reads them.",
               "#pragma once", "#include <cstdint>", "namespace reno { namespace gpu { namespace push {"]
    for name, push, bufs, local, fn in KERNELS:
        S, words = compile_kernel(name, push, bufs, fn, local)
        if only is None or name in only:
            with open(os.path.join(out, name + ".spv.h"), "w", newline="\n") as fh:
                fh.write(c_array(name, words))
        ctype = dict(u="uint32_t", i="int32_t", f="float")
        camel = "".join(w.capitalize() for w in name.split("_"))
        layouts.append("struct %s { %s };" % (camel, " ".join("%s %s;" % (ctype[t], n) for n, t in push) if push else "int none;"))
    layouts.append("} } }")
    with open(os.path.join(out, "push_layouts.h"), "w", newline="\n") as fh:
        fh.write("\n".join(layouts) + "\n")
    print("%d kernels" % len(KERNELS))

if __name__ == "__main__":
    main()
