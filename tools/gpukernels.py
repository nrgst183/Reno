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
from sf64 import SF

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

def pixel_row(S, body, xy=False):
    """The per-pixel colour effects: body(S, r, g, b) -> (r, g, b) on the straight colour; the pixel is premultiplied again,
    and a pixel with no alpha becomes all zeros (v4::pixelRow in builtins_vec.h)."""
    with S.in_range(S.p.w, S.p.h):
        i = S.gy * S.p.w + S.gx
        p = S.buf('in0').load(i)
        r, g, b, a, valid = unpremul(S, p)
        r, g, b = body(S, r, g, b, S.gx, S.gy) if xy else body(S, r, g, b)
        z = S.f(0.0)
        S.buf('out').store(i, S.vec4(S.select(valid, r * a, z), S.select(valid, g * a, z), S.select(valid, b * a, z), S.select(valid, a, z)))


# ---- 8-bit (VEGAS's whole-level maths, src/ofx/vegas8.h): a picture value as the level it came from, and back
def to_level(S, v):
    """v8::toLevel: the value clamped to 0..1 times 255, halves rounded up; an int."""
    c = S.min(S.max(v, 0.0), 1.0)
    sc = c * 255.0
    i = S.to_i(sc)
    return i + S.select((sc - S.to_f(i)) >= 0.5, S.i(1), S.i(0))

def from_level(S, i):
    return S.to_f(i) * S.f(1.0 / 255.0)



def round_magic(S, x):
    """v8::roundMagic of a float: the nearest whole number with halves up (the nudge of 1.5e-8 only decides exact halves); an int"""
    fl = S.floor(x)
    return S.to_i(fl) + S.select((x - fl) >= 0.5, S.i(1), S.i(0))



def fpow(S, x, y):
    """powf of floats, x >= 0: the double power rounded to a float (what a C library's powf gives but for a rare last bit)"""
    from sf64 import sf_pow
    return sf_pow(S, SF.from_f32(S, x), SF.from_f32(S, y)).to_f32()

def fsqrt(S, x):
    """the float square root, correctly rounded (computed in double)"""
    return SF.from_f32(S, x).sqrt().to_f32()

def fmin(S, a, b):
    """std::min(a, b): b < a ? b : a"""
    return S.select(b < a, b, a)

def fmax(S, a, b):
    """std::max(a, b): a < b ? b : a"""
    return S.select(a < b, b, a)

def dpush(*names):
    """push constant fields for doubles: name_h, name_l (the halves of the bits)"""
    out = []
    for n in names:
        out += [(n + '_h', 'u'), (n + '_l', 'u')]
    return out

def pd(P, name):
    """the double of dpush's two fields, as an SF"""
    return SF(P.S, getattr(P, name + '_h'), getattr(P, name + '_l'))

def ld_sf(S, tab, i):
    """a double from two table words (the bits of its high and low halves held in floats)"""
    return SF(S, S.f_to_bits(tab.load(i)), S.f_to_bits(tab.load(i + 1)))

class SFVar:
    """a double kept in two uint variables (for sums in a loop): get() / set()"""
    def __init__(self, S, init=0.0):
        self.S = S
        init = SF.of(S, init)
        self.hv, self.lv = S.var('u', init.hi), S.var('u', init.lo)
    def set(self, v):
        self.hv.set(v.hi)
        self.lv.set(v.lo)
    def get(self):
        return SF(self.S, self.hv.get(), self.lv.get())

def sdiv_pos(S, a, b):
    """a / b for ints with b >= 0 (0 where b is 0), like the CPU's guarded integer division"""
    return S.select(S.ne(b, 0), a / S.max(b, S.i(1)), S.i(0))

# ---------------------------------------------------------------------------------------------------------- effects
@kernel("invert8", WH + [('wt', 'i')], [PIC_IN, PIC_OUT])
def invert8(S):
    # invertRows() with c.levels (Vfx1's invertProcessor): integer steps on the levels
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        a = to_level(S, p.w)
        out = []
        for k in range(3):
            v = to_level(S, p[k])
            opaque = S.eq(a, 255)
            t = S.select(S.eq(a, 0), S.i(0), S.min(S.i(255), sdiv_pos(S, v * 255, a)))
            pr = S.select(opaque, 255 - v, ((255 - t) * a) >> 8)
            prod = (v - pr) * P.wt
            lv = (pr + ((prod + S.select(prod < 0, S.i(255), S.i(0))) >> 8)) & 255
            out.append(from_level(S, lv))
        S.buf('out').store(i, S.vec4(out[0], out[1], out[2], from_level(S, a)))


@kernel("invert", WH + [('k', 'f')], [PIC_IN, PIC_OUT])
def invert(S):
    # invertRows() in builtins_filters.cpp: 8-bit levels, a step of floor(|d| * k) + 1 towards the inverse
    def body(S, r, g, b):
        def one(c):
            lv = S.floor(c * 255.0 + 0.5)
            d = 255.0 - 2.0 * lv
            ad = S.abs(d)
            step = S.min(ad, S.floor(ad * S.p.k + 0.0001) + 1.0)
            return (lv + S.select(d < 0.0, S.f(0.0) - step, step)) / 255.0
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
    # blackAndWhiteRows() in builtins_filters.cpp: 8-bit levels, grey = floor(sum / 3), steps of ceil(|d| * k - 1e-5); the divisions are the CPU's
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        a = p.w
        has = a > 1e-6
        lv = [S.floor(S.select(has, p[k] / a, S.f(0.0)) * 255.0 + 0.5) for k in range(3)]
        grey = S.floor((lv[0] + lv[1] + lv[2]) / 3.0)
        out = []
        for k in range(3):
            d = grey - lv[k]
            step = S.ceil(S.abs(d) * P.k - 0.00001)
            o = lv[k] + S.select(d < 0.0, S.f(0.0) - step, step)
            out.append(o / 255.0 * a)
        z = S.f(0.0)
        S.buf('out').store(i, S.select(a > 0.0, S.vec4(out[0], out[1], out[2], a), S.vec4(z, z, z, z)))

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

@kernel("levels", WH + [('channel', 'u'), ('straight', 'u'), ('glt1', 'u')] + dpush('i0', 'i1', 'o0', 'domain', 'width', 'k0', 'slope', 'a1', 'a', 'inv', 'G'), [PIC_IN, PIC_OUT, TABLE])
def levels(S):
    # levels() in builtins_filters.cpp: the straight colour (0 where alpha is tiny), the chosen channels through the curve (in doubles, as the CPU does), premultiplied again.
    # A value that is exactly a byte level's float (level * the float 1/255: any 8-bit picture's opaque pixel) takes the CPU's own answer for that value from the table
    # (levelsLut256 in builtins_filters.cpp, the same function); only the other values (alpha below 1, floats) take the emulated doubles.
    P = S.p
    tab = S.buf('table')
    from sf64 import sf_pow
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        a = p.w
        has = a > 1e-6
        r = S.select(has, p.x / a, S.f(0.0))
        g = S.select(has, p.y / a, S.f(0.0))
        b = S.select(has, p.z / a, S.f(0.0))
        i0, i1, o0, domain, width = pd(P, 'i0'), pd(P, 'i1'), pd(P, 'o0'), pd(P, 'domain'), pd(P, 'width')
        k0, slope, a1, aa, inv, G = pd(P, 'k0'), pd(P, 'slope'), pd(P, 'a1'), pd(P, 'a'), pd(P, 'inv'), pd(P, 'G')
        def f(v):
            x = SF.from_f32(S, v)
            xin = SF.sel(S, i0 > x, i0, SF.sel(S, x < i1, x, i1))
            t = xin - i0
            rs = (t * domain) / width
            tt = t / width
            big = tt >= k0
            lt1 = S.ne(P.glt1, 0)
            # one power per pixel: base and exponent of the branch taken
            base = SF.sel(S, lt1, tt, (aa + tt) * inv)
            pw = sf_pow(S, base, G)
            c_lt = SF.sel(S, big, pw * a1 - aa, tt * slope)          # G < 1
            c_ge = SF.sel(S, big, pw, tt * slope)                    # G >= 1
            curved = domain * SF.sel(S, lt1, c_lt, c_ge)
            res = SF.sel(S, S.ne(P.straight, 0), rs, curved)
            return (res + o0).to_f32()
        ch = P.channel
        vals = [r, g, b, a]
        outs = [S.var('f', vals[k]) for k in range(4)]

        def per_channel(c):
            v = S.select(S.eq(c, 0), r, S.select(S.eq(c, 1), g, S.select(S.eq(c, 2), b, a)))
            lvl = to_level(S, v)
            fast = S.eq(S.f_to_bits(v), S.f_to_bits(from_level(S, lvl)))
            fvv = S.var('f', tab.load(S.to_u(lvl)))
            with S.if_(~fast):
                fvv.set(f(v))
            fv = fvv.get()
            rgb = S.eq(ch, 0) | S.eq(ch, 4) | S.eq(ch, c + 1)
            use = S.select(S.eq(c, 3), S.eq(ch, 4), rgb)
            nv = S.select(S.eq(c, 3), clamp01(S, fv), fv)
            for k in range(4):
                outs[k].set(S.select(use & S.eq(c, k), nv, outs[k].get()))
        S.loop(4, per_channel)
        r2, g2, b2, a2 = [o.get() for o in outs]
        S.buf('out').store(i, S.vec4(r2 * a2, g2 * a2, b2 * a2, a2))

@kernel("saturationadjust8", WH, [PIC_IN, PIC_OUT, TABLE])
def saturationadjust8(S):
    # saturationAdjust8() in builtins_more_colour.cpp: VEGAS's 8-bit steps in doubles (tools/sf64.py: the same roundings as the CPU's)
    P = S.p
    with S.in_range(P.w, P.h):
        tab = S.buf('table')
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        R = SF.from_i32(S, to_level(S, p.x))
        G = SF.from_i32(S, to_level(S, p.y))
        B = SF.from_i32(S, to_level(S, p.z))
        ai = to_level(S, p.w)
        Y = (G * 0.587 + R * 0.299) + B * 0.114
        dG, dR, dB = Y - G, Y - R, Y - B
        d = ((dG * dG + dR * dR) + dB * dB).sqrt()
        x = d / ld_sf(S, tab, 42)
        x1, x2, x3 = ld_sf(S, tab, 0), ld_sf(S, tab, 2), ld_sf(S, tab, 4)
        # the piece x is in: the first inner position above it, else the last piece
        seg = S.select(x1 > x, S.u(0), S.select(x2 > x, S.u(1), S.select(x3 > x, S.u(2), S.u(3))))
        base = 6 + seg * 9
        xlo, den = ld_sf(S, tab, base), ld_sf(S, tab, base + 2)
        has = S.ne(S.f_to_bits(tab.load(base + 4)), 0)
        v0, v1 = ld_sf(S, tab, base + 5), ld_sf(S, tab, base + 7)
        t = SF.sel(S, has, (x - xlo) / den, SF.const(S, 0.0))
        t2 = t * t
        t3 = t2 * t
        h00 = ((t3 + t3) - t2 * 3.0) + 1.0
        h01 = t2 * 3.0 - (t3 + t3)
        g = SF.from_f32(S, (v0 * h00 + v1 * h01).to_f32())
        f = g + 1.0
        u = 1.0 - f
        wG, wR, wB = u * 0.587, u * 0.299, u * 0.114
        outR = ((wR + f) * R + G * wG) + B * wB
        outG = ((wG + f) * G + R * wR) + B * wB
        outB = (wB + f) * B + (G * wG + R * wR)
        al = SF.from_i32(S, ai)
        def fin(v):
            v = SF.sel(S, al > v, v, al)
            v = SF.sel(S, v > 0.0, v, SF.const(S, 0.0))
            return from_level(S, v.trunc_i32())
        S.buf('out').store(i, S.vec4(fin(outR), fin(outG), fin(outB), from_level(S, ai)))


@kernel("colorcorrector8", WH + [('flags', 'u')], [PIC_IN, PIC_OUT, TABLE])
def colorcorrector8(S):
    # ccRow8() in builtins_filters2.cpp: the CPU's tables (4 floats a, b, y, s for 1024 steps of Y, then the wheels-only integers) and the same float steps
    P = S.p
    with S.in_range(P.w, P.h):
        tab = S.buf('table')
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        rr, gg, bb, aa = to_level(S, p.x), to_level(S, p.y), to_level(S, p.z), to_level(S, p.w)
        fl = P.flags & 31      # (bit 0x20: the luminance mask of Color Corrector (Secondary) picks the entry)
        Y = S.to_f(gg) * 0.587
        Y = Y + S.to_f(rr) * 0.299
        Y = Y + S.to_f(bb) * 0.114
        # flags == 4: the integer tint
        idx4 = S.to_u(S.max(S.min(round_magic(S, Y * 4.0), S.i(1023)), S.i(0)))
        def d(k): return S.to_i(tab.load(4096 + idx4 * 4 + k))
        def hold(v): return S.max(S.i(0), S.min(v, aa))
        w_r, w_g, w_b = hold(rr + d(2)), hold(gg + d(1)), hold(bb + d(0))
        # the general case
        U = (S.to_f(bb) - Y) * 0.492
        V = (S.to_f(rr) - Y) * 0.877
        need = S.ne(fl & 5, 0) | S.ne(P.flags & 32, 0)
        idx = S.select(need, idx4, S.u(0))
        ea, eb, ey, es = tab.load(idx * 4), tab.load(idx * 4 + 1), tab.load(idx * 4 + 2), tab.load(idx * 4 + 3)
        Yp = S.select(S.ne(fl & 1, 0), ey, Y)
        t4 = S.ne(fl & 4, 0)
        U = S.select(t4, U + eb, U)
        V = S.select(t4, V + ea, V)
        t2 = S.ne(fl & 2, 0)
        U = S.select(t2, U * es, U)
        V = S.select(t2, V * es, V)
        fa = S.to_f(aa)
        nb = U * 2.032520294189453
        nb = nb + Yp
        ng = Yp - U * 0.394731342792511
        ng = ng - V * 0.5808091759681702
        nr = V * 1.1402509212493896
        nr = nr + Yp
        def cut(v): return S.select(S.f(0.0) > v, S.f(0.0), S.select(fa < v, fa, v))
        g_r, g_g, g_b = round_magic(S, cut(nr)) & 255, round_magic(S, cut(ng)) & 255, round_magic(S, cut(nb)) & 255
        only4 = S.eq(fl, 4)
        anyf = S.ne(fl, 0)
        o_r = S.select(only4, w_r, S.select(anyf, g_r, rr))
        o_g = S.select(only4, w_g, S.select(anyf, g_g, gg))
        o_b = S.select(only4, w_b, S.select(anyf, g_b, bb))
        S.buf('out').store(i, S.vec4(from_level(S, o_r), from_level(S, o_g), from_level(S, o_b), from_level(S, aa)))


@kernel("colorcurves8", WH, [PIC_IN, PIC_OUT, TABLE])
def colorcurves8(S):
    # colorCurvesRows() with c.levels: three byte tables on the levels, a colour held to the alpha's level
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        a = to_level(S, p.w)
        out = []
        for k in range(3):
            r = S.to_i(S.buf('table').load(S.u(k * 256) + S.to_u(to_level(S, p[k]))))
            out.append(from_level(S, S.min(r, a)))
        S.buf('out').store(i, S.vec4(out[0], out[1], out[2], p.w))

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
        # atan2f(y, x) * (float)(180 / pi): the double atan2 rounded to a float (what the C library's atan2f gives but for a rare last bit); a colour with no
        # chroma (U, V about 1e-5 or less) has hue 0 - as on the CPU, so a grey's rounding noise cannot throw it into a hue range
        from sf64 import sf_atan2
        q = sf_atan2(S, SF.from_f32(S, y), SF.from_f32(S, x)).to_f32()
        return S.select(S.max(S.abs(x), S.abs(y)) < 1e-5, S.f(0.0), q * S.f(180.0 / 3.14159265358979323846))
    def fmod360(x):
        # fmodf(x, 360): exact (done in doubles; the quotient of a float is never within rounding of a whole number)
        xd = SF.from_f32(S, x)
        n = (xd / 360.0).trunc()
        return (xd - n * 360.0).to_f32()
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
            d0 = S.abs(fmod360(hue - P.hc))
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
        yg = S.select(base > 0.0, fpow(S, S.max(base, 1e-30), P.ga), S.f(0.0))
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

def sf_nonfinite(S, x):
    return S.eq(x.hi & 0x7ff00000, 0x7ff00000)

def sample_on_bg_d(S, buf, w, h, x, y, bg):
    """sampleOnBg() in builtins_filters.cpp with the position in doubles (x, y are SF): bilinear, the taps outside the frame are the colour bg (a vec4); a point far
    outside, or not finite, is the colour bg."""
    wf, hf = SF.from_u32(S, w), SF.from_u32(S, h)
    xs, ys = x - 0.5, y - 0.5
    x0f, y0f = xs.floor(), ys.floor()
    far = (x0f < -2.0) | (y0f < -2.0) | (x0f > wf + 1.0) | (y0f > hf + 1.0) | sf_nonfinite(S, x) | sf_nonfinite(S, y)
    x0, y0 = xs.floor_i32(), ys.floor_i32()
    fx = (xs - x0f).to_f32()
    fy = (ys - y0f).to_f32()
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
    acc = acc + taps[3] * wt[3]
    return S.select(far, bg, acc)

@kernel("mirror", WH + dpush('kx', 'ky', 'px0', 'py0', 'ex', 'ey', 'tie') + [('bgr', 'f'), ('bgg', 'f'), ('bgb', 'f'), ('bga', 'f'), ('straight', 'u')], [PIC_IN, PIC_OUT])
def mirror(S):
    # mirror() in builtins_filters.cpp, float pictures: the CPU's positions and distances are doubles, so they are here (the software double)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        kx, ky, px0, py0, ex, ey, tie = [pd(P, n) for n in ('kx', 'ky', 'px0', 'py0', 'ex', 'ey', 'tie')]
        xc = SF.from_u32(S, S.gx) + 0.5
        yc = SF.from_u32(S, S.gy) + 0.5
        d = (xc - px0) * kx + (yc - py0) * ky
        res = S.var('v4')
        bg = S.vec4(P.bgr, P.bgg, P.bgb, P.bga)
        with S.if_((d - tie) <= 1e-6):
            sx = xc - (d * 2.0) * kx
            sy = yc - (d * 2.0) * ky
            with S.if_(S.ne(P.straight, 0)):
                wf, hf = SF.from_u32(S, P.w), SF.from_u32(S, P.h)
                oob = (sx < -0.5 - 1e-6) | (sy < -0.5 - 1e-6) | (sx > wf + 0.5 + 1e-6) | (sy > hf + 0.5 + 1e-6)
                with S.if_(oob):
                    res.set(bg)
                with S.else_():
                    res.set(sample_clamped_d(S, S.buf('in0'), P.w, P.h, sx, sy))
            with S.else_():
                res.set(sample_on_bg_d(S, S.buf('in0'), P.w, P.h, sx + ex, sy + ey, bg))
        with S.else_():
            res.set(S.buf('in0').load(i))
        S.buf('out').store(i, res.get())

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
        yg = S.select(base > 0.0, fpow(S, S.max(base, 1e-30), P.ga), S.f(0.0))
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

@kernel("blackrestore", WH + [('thr', 'f')], [PIC_IN, PIC_OUT])
def blackrestore(S):
    P = S.p
    def body(S, r, g, b):
        m = S.max(r, S.max(g, b))
        low = m <= P.thr
        z = S.f(0.0)
        return S.select(low, z, r), S.select(low, z, g), S.select(low, z, b)
    pixel_row(S, body)

# ---------------------------------------------------------------------------------------------------------- blur mixes, pixelate
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


def sample_clamped_d(S, buf, w, h, x, y):
    """Buf::sampleClamped(double x, double y): the same weights, the same order of sums, the position in doubles (x, y are SF)"""
    wf, hf = SF.from_u32(S, w), SF.from_u32(S, h)
    cx = SF.sel(S, x < 0.5, SF.const(S, 0.5), SF.sel(S, x > wf - 0.5, wf - 0.5, x))
    cy = SF.sel(S, y < 0.5, SF.const(S, 0.5), SF.sel(S, y > hf - 0.5, hf - 0.5, y))
    xs, ys = cx - 0.5, cy - 0.5
    x0, y0 = xs.floor_i32(), ys.floor_i32()
    fx = (xs - SF.from_i32(S, x0)).to_f32()
    fy = (ys - SF.from_i32(S, y0)).to_f32()
    ix, iy = 1.0 - fx, 1.0 - fy
    wi, hi = S.to_i(w), S.to_i(h)
    acc = S.vec4(S.f(0.0), S.f(0.0), S.f(0.0), S.f(0.0))
    for (di, dj, wgt) in ((0, 0, ix * iy), (1, 0, fx * iy), (0, 1, ix * fy), (1, 1, fx * fy)):
        xx, yy = x0 + di, y0 + dj
        ok = (xx >= 0) & (yy >= 0) & (xx < wi) & (yy < hi)
        idx = S.to_u(S.max(S.min(yy, hi - 1), S.i(0))) * w + S.to_u(S.max(S.min(xx, wi - 1), S.i(0)))
        t = buf.load(idx) * wgt
        acc = S.select(ok, acc + t, acc)
    return acc

def area_coords(S):
    P = S.p
    dx = (S.to_f(S.gx) + 0.5 - P.px0) / P.rx
    dy = (S.to_f(S.gy) + 0.5 - P.py0) / P.ry
    r = S.sqrt(dx * dx + dy * dy)
    return dx, dy, r

AREA = WH + [('px0', 'f'), ('py0', 'f'), ('rx', 'f'), ('ry', 'f')]

@kernel("swirl", WH + dpush('px0', 'py0', 'rx', 'ry', 'turn'), [PIC_IN, PIC_OUT])
def swirl(S):
    # swirl() in builtins_distort.cpp, in doubles
    from sf64 import sf_sincos
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        px0, py0, rx, ry, turn = pd(P, 'px0'), pd(P, 'py0'), pd(P, 'rx'), pd(P, 'ry'), pd(P, 'turn')
        dx = (SF.from_u32(S, S.gx) + 0.5 - px0) / rx
        dy = (SF.from_u32(S, S.gy) + 0.5 - py0) / ry
        r = (dx * dx + dy * dy).sqrt()
        f = (1.0 - r) * (1.0 - r)
        ang = turn * f
        sa, ca = sf_sincos(S, ang)
        ux, uy = dx * rx, dy * ry
        samp = sample_clamped_d(S, S.buf('in0'), P.w, P.h, px0 + ux * ca - uy * sa, py0 + ux * sa + uy * ca)
        S.buf('out').store(i, S.select(r >= 1.0, S.buf('in0').load(i), samp))

@kernel("pinchpunch", WH + dpush('px0', 'py0', 'rx', 'ry', 'amount'), [PIC_IN, PIC_OUT])
def pinchpunch(S):
    from sf64 import sf_sin
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        px0, py0, rx, ry, amount = pd(P, 'px0'), pd(P, 'py0'), pd(P, 'rx'), pd(P, 'ry'), pd(P, 'amount')
        dx = (SF.from_u32(S, S.gx) + 0.5 - px0) / rx
        dy = (SF.from_u32(S, S.gy) + 0.5 - py0) / ry
        r = (dx * dx + dy * dy).sqrt()
        sn = sf_sin(S, r * 3.141592653589793)
        k = 1.0 - amount * 0.6 * sn * sn * (1.0 - r)
        samp = sample_clamped_d(S, S.buf('in0'), P.w, P.h, px0 + dx * k * rx, py0 + dy * k * ry)
        S.buf('out').store(i, S.select((r >= 1.0) | (r < 1e-12), S.buf('in0').load(i), samp))

@kernel("wave", WH + dpush('vn', 'hn', 'va', 'ha', 'vp', 'hp'), [PIC_IN, PIC_OUT])
def wave(S):
    # wave() in builtins_distort.cpp, in doubles
    from sf64 import sf_sin
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        vn, hn, va, ha, vp, hp = pd(P, 'vn'), pd(P, 'hn'), pd(P, 'va'), pd(P, 'ha'), pd(P, 'vp'), pd(P, 'hp')
        xc = SF.from_u32(S, S.gx) + 0.5
        yc = SF.from_u32(S, S.gy) + 0.5
        fy = yc / SF.from_u32(S, S.max(P.h, 1))
        fx = xc / SF.from_u32(S, S.max(P.w, 1))
        sx = ha * sf_sin(S, (hn * fy + hp) * 6.283185307179586)
        sy = va * sf_sin(S, (vn * fx + vp) * 6.283185307179586)
        S.buf('out').store(i, sample_clamped_d(S, S.buf('in0'), P.w, P.h, xc + sx, yc + sy))

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
    # broadcastColors() in builtins_more_colour.cpp, operation by operation
    P = S.p
    def soft_low(x, L, T):
        # broadcastSoftLow(): T <= L + 1e-7 a hard limit; x >= T: x; else a power curve (only evaluated where it is used)
        hard = T <= L + 1e-7
        res = S.var('f', S.select(hard, fmax(S, x, L), x))
        with S.if_(~hard & (x < T)):
            base = fmin(S, S.f(1.0), fmax(S, S.f(0.0), x / T))
            res.set(L + (T - L) * fpow(S, base, T / (T - L)))
        return res.get()
    def body(S, r, g, b):
        y0 = 0.299 * r + 0.587 * g + 0.114 * b
        y1 = S.select(P.ymin > 0.0, soft_low(y0, P.ymin, P.ymin * (1.0 + P.symin)), fmax(S, y0, P.ymin))
        y = S.select(P.ymax < 1.0, 1.0 - soft_low(1.0 - y1, 1.0 - P.ymax, (1.0 - P.ymax) * (1.0 + P.symax)), fmin(S, y1, P.ymax))
        d = y - y0
        cb = b + d - y
        cr = r + d - y
        p1 = 0.492 * cb
        p2 = 0.877 * cr
        uv = fsqrt(S, p1 * p1 + p2 * p2)
        top = S.f(0.63214)
        uv1 = S.select(P.clim < top, top - soft_low(top - uv, top - P.clim, (top - P.clim) * (1.0 + P.sc)), fmin(S, uv, P.clim))
        f = S.select(uv > 1e-9, uv1 / S.select(uv > 1e-9, uv, S.f(1.0)), S.f(1.0))
        uv2 = uv * f
        hi0 = y + uv2
        lo0 = y - uv2
        wh = 0.2 * P.scmax * fmax(S, S.f(0.0), P.cmax - y)
        wl = 0.2 * P.scmin * fmax(S, S.f(0.0), y - P.cmin)
        th = P.cmax + wh - hi0
        wh_ok = wh > 1e-9
        hi_k = S.select(hi0 >= P.cmax + wh, P.cmax, S.select(hi0 > P.cmax - wh, P.cmax - th * th / (4.0 * S.select(wh_ok, wh, S.f(1.0))), hi0))
        hi = S.select(wh_ok, hi_k, fmin(S, hi0, P.cmax))
        tl2 = lo0 - P.cmin + wl
        wl_ok = wl > 1e-9
        lo_k = S.select(lo0 <= P.cmin - wl, P.cmin, S.select(lo0 < P.cmin + wl, P.cmin + tl2 * tl2 / (4.0 * S.select(wl_ok, wl, S.f(1.0))), lo0))
        lo = S.select(wl_ok, lo_k, fmax(S, lo0, P.cmin))
        uc = S.select(uv2 > 1e-9, uv2, S.f(1.0))
        s = fmin(S, S.f(1.0), fmax(S, S.f(0.0), fmin(S, (hi - y) / uc, (y - lo) / uc)))
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
            idx = S.to_u(S.min(S.max(yy, 0.0), hf - 1.0)) * w + S.to_u(S.min(S.max(xx, 0.0), wf - 1.0))      # (kept inside the picture; the tap counts only when 'inside')
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

# ---------------------------------------------------------------------------------------------------------- generators (no input picture)
@kernel("fill_px", WH + [('r', 'f'), ('g', 'f'), ('b', 'f'), ('a', 'f')], [PIC_OUT])
def fill_px(S):
    # Solid Color: the planner works the colour out in double precision, as solidColor() does
    with S.in_range(S.p.w, S.p.h):
        S.buf('out').store(S.gy * S.p.w + S.gx, S.vec4(S.p.r, S.p.g, S.p.b, S.p.a))

@kernel("colorgradient", WH + [('type', 'u')], [PIC_OUT, TABLE])
def colorgradient(S):
    # colorGradient() in builtins_gen.cpp, in doubles; the table holds the doubles: cx W, cy H, cos, sin, size, aspect, aspect^2, size / 2, then colour 1 and 2 (r g b a)
    P = S.p
    with S.in_range(P.w, P.h):
        tab = S.buf('table')
        d = [ld_sf(S, tab, 2 * k) for k in range(16)]
        cxw, cyh, ca, sa, size, aspect, asp2, hsz = d[:8]
        hf = SF.from_u32(S, P.h)
        dx = ((SF.from_u32(S, S.gx) + 0.5) - cxw) / hf
        dy = ((hf - (SF.from_u32(S, S.gy) + 0.5)) - cyh) / hf
        u = dx * ca + dy * sa
        v = dx.neg() * sa + dy * ca
        t_lin = u / size + 0.5
        t_ell = ((u * u) / asp2 + v * v).sqrt() / hsz
        au, av = u.abs(), v.abs()
        q = au / aspect
        t_rect = SF.sel(S, q > av, q, av) / hsz            # std::max(a, b): a < b ? b : a
        t_rect = SF.sel(S, q < av, av, q) / hsz
        t = SF.sel(S, S.eq(P.type, 0), t_lin, SF.sel(S, S.eq(P.type, 1), t_ell, t_rect))
        t = SF.sel(S, t < 0.0, SF.const(S, 0.0), SF.sel(S, t > 1.0, SF.const(S, 1.0), t))
        def lerp(a, b): return a + (b - a) * t
        c1, c2 = d[8:12], d[12:16]
        m = [lerp(c1[k], c2[k]) for k in range(4)]
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4((m[0] * m[3]).to_f32(), (m[1] * m[3]).to_f32(), (m[2] * m[3]).to_f32(), m[3].to_f32()))

@kernel("checkerboard", WH + [('pa0', 'f'), ('pa1', 'f'), ('pa2', 'f'), ('pa3', 'f'), ('pb0', 'f'), ('pb1', 'f'), ('pb2', 'f'), ('pb3', 'f')], [PIC_OUT, TABLE])
def checkerboard(S):
    # checkerboard() in builtins_gen.cpp: the table holds the horizontal wave of every column, then the vertical one of every row (worked out in double precision)
    P = S.p
    with S.in_range(P.w, P.h):
        tab = S.buf('table')
        sx = tab.load(S.gx)
        sy = tab.load(P.w + S.gy)
        pa = [P.pa0, P.pa1, P.pa2, P.pa3]
        pb = [P.pb0, P.pb1, P.pb2, P.pb3]
        res = []
        for k in range(4):
            m1 = S.floor((pa[k] + (pb[k] - pa[k]) * sx) * 255.0 + 0.0001)
            m2 = S.floor((pb[k] + (pa[k] - pb[k]) * sx) * 255.0 + 0.0001)
            res.append(S.floor(m1 + (m2 - m1) * sy + 0.0001) / 255.0)
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(res[0], res[1], res[2], res[3]))

@kernel("testpattern", WH + [('type', 'u'), ('barw', 'u')], [PIC_OUT, TABLE])
def testpattern(S):
    # testPattern() in builtins_gen.cpp. Table: bars (7 x 3 levels), castle (7 x 3), then per column 4 numbers (inside the porch columns, ramp level, step), then per row 2 (inside the porch
    # rows, step row); all worked out by the planner as the CPU does.
    P = S.p
    with S.in_range(P.w, P.h):
        tab = S.buf('table')
        W, H = P.w, P.h
        x, y = S.gx, S.gy
        yb = H - 1 - y
        def lv(k): return S.to_u(tab.load(k if not isinstance(k, int) else S.u(k)))
        colbase = S.u(42) + x * 4
        rowbase = S.u(42) + W * 4 + y * 2
        col_in = S.ne(S.to_u(tab.load(colbase)), 0)
        bar = S.to_u(tab.load(colbase + 1))
        ramp = S.to_u(tab.load(colbase + 2))
        kstep = S.to_u(tab.load(colbase + 3))
        row_in = S.ne(S.to_u(tab.load(rowbase)), 0)
        step_row = S.to_u(tab.load(rowbase + 1))
        R = S.var('u', 7); G = S.var('u', 7); B = S.var('u', 7)
        def setv(r, g, b):
            R.set(r); G.set(g); B.set(b)
        def bars(white):
            br, bg, bb = lv(bar * 3), lv(bar * 3 + 1), lv(bar * 3 + 2)
            use = white & S.eq(bar, 0)
            setv(S.select(use, S.u(235), br), S.select(use, S.u(235), bg), S.select(use, S.u(235), bb))
        t = P.type
        step_lvl = lambda k: S.select(S.eq(k, 0), S.u(70), S.select(S.eq(k, 1), S.u(125), S.select(S.eq(k, 2), S.u(180), S.u(235))))
        with S.if_(S.eq(t, 0) | S.eq(t, 1)):
            bars(S.eq(t, 1))
        with S.if_(S.eq(t, 2)):
            fx = S.to_f(x) * 255.0 / S.to_f(W)
            setv((y + 1) * 255 / H, S.to_u(S.floor(fx)), S.to_u(S.floor(255.0 - fx)))
        with S.if_(S.eq(t, 3)):
            with S.if_(row_in):
                with S.if_((x * 5 >= W) & (x * 10 < W * 3)):
                    setv(S.u(16), S.u(16), S.u(16))
                with S.if_((x * 10 >= W * 3) & (x * 10 < W * 4)):
                    setv(S.u(25), S.u(25), S.u(25))
                with S.if_((x * 10 >= W * 4) & (x * 2 < W)):
                    setv(S.u(16), S.u(16), S.u(16))
                with S.if_((x * 2 >= W) & col_in):
                    l = step_lvl(step_row)
                    setv(l, l, l)
        with S.if_(S.eq(t, 4)):
            with S.if_(row_in & col_in):
                setv(ramp, ramp, ramp)
        with S.if_(S.eq(t, 5) | S.eq(t, 6)):
            with S.if_(y * 3 < 2 * H):
                bars(S.eq(t, 6))
            with S.if_((y * 3 >= 2 * H) & (y * 4 < 3 * H)):
                setv(lv(21 + bar * 3), lv(22 + bar * 3), lv(23 + bar * 3))
            with S.if_(y * 4 >= 3 * H):
                xl = P.barw * 5
                u3 = P.barw / 3
                with S.if_(x >= xl):
                    setv(S.u(16), S.u(16), S.u(16))
                    with S.if_(x < xl + u3): setv(S.u(7), S.u(7), S.u(7))
                    with S.if_((x >= xl + u3) & (x < xl + 2 * u3)): setv(S.u(16), S.u(16), S.u(16))
                    with S.if_((x >= xl + 2 * u3) & (x < xl + 3 * u3)): setv(S.u(25), S.u(25), S.u(25))
                    with S.if_(x >= xl + 3 * u3): setv(S.u(16), S.u(16), S.u(16))
                with S.else_():
                    setv(S.u(16), S.u(16), S.u(16))
                    with S.if_(S.eq(t, 5)):
                        with S.if_(x < W / 6): setv(S.u(113), S.u(40), S.u(7))
                        with S.if_((x >= W / 6) & (x < W * 2 / 6)): setv(S.u(235), S.u(235), S.u(235))
                        with S.if_((x >= W * 2 / 6) & (x < W * 3 / 6)): setv(S.u(165), S.u(7), S.u(70))
        with S.if_(S.eq(t, 7) | S.eq(t, 8)):
            with S.if_(row_in & col_in):
                l = step_lvl(S.u(3) - kstep)
                setv(S.select(S.eq(t, 7), l, S.u(7)), l, l)
        out = S.vec4(S.to_f(R.get()) / 255.0, S.to_f(G.get()) / 255.0, S.to_f(B.get()) / 255.0, S.f(1.0))
        S.buf('out').store(y * W + x, out)

# ---------------------------------------------------------------------------------------------------------- creative effects (builtins_more_creative.cpp)
@kernel("crop", WH + [('cx', 'f'), ('cy', 'f'), ('cs', 'f'), ('sn', 'f'), ('hx', 'f'), ('hy', 'f'), ('op', 'f')], [PIC_IN, PIC_OUT])
def crop(S):
    # cropFx(): the part of the frame inside a turned box is kept (times the opacity), the rest is transparent
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        u = S.to_f(S.gx) + 0.5 - P.cx
        v = S.to_f(S.gy) + 0.5 - P.cy
        ru = u * P.cs + v * P.sn
        rv = v * P.cs - u * P.sn
        inside = (S.abs(ru) <= P.hx) & (S.abs(rv) <= P.hy)
        p = S.buf('in0').load(i)
        S.buf('out').store(i, S.select(inside, p * P.op, S.splat4(0.0)))

def hash_rnd(S, a, b, k, seed):
    """The integer hash of the grain effects: 16 bits of it (u)."""
    h = a * 374761393 + b * 668265263 + k * 2246822519 + seed
    h = (h ^ (h >> 13)) * 1274126177
    return (h ^ (h >> 16)) & 65535

@kernel("filmgrain", WH + [('cell', 'u'), ('seed', 'u'), ('amount', 'f'), ('amc', 'f')], [PIC_IN, PIC_OUT])
def filmgrain(S):
    # filmGrain(): a grain value per cell of the picture, the same for every pixel of it
    P = S.p
    def body(S, r, g, b, x, y):
        gx, gy = x / P.cell, y / P.cell
        def rnd(k): return S.to_f(hash_rnd(S, gx, gy, S.u(k), P.seed)) / 32767.5 - 1.0
        n0 = rnd(0) * P.amount * 0.35
        r2 = r + (n0 + rnd(1) * P.amc * 0.2)
        g2 = g + (n0 + rnd(2) * P.amc * 0.2)
        b2 = b + (n0 + rnd(3) * P.amc * 0.2)
        return clamp01(S, r2), clamp01(S, g2), clamp01(S, b2)
    pixel_row(S, body, True)

@kernel("filmeffects", WH + [('frame', 'u'), ('grey', 'u'), ('particles', 'u'), ('type', 'u'), ('c2', 'u'), ('c3', 'u'), ('c4', 'u'), ('c40', 'u'),
                              ('tint', 'f'), ('cr', 'f'), ('cg', 'f'), ('cb', 'f'), ('grain', 'f'), ('gk', 'f'), ('scratch', 'f'), ('dust', 'f'), ('hair', 'f'), ('amount', 'f')], [PIC_IN, PIC_OUT])
def filmeffects(S):
    # filmEffects(): grey / tint, grain, and one kind of particle
    P = S.p
    def body(S, r, g, b, x, y):
        seed = P.frame * 3266489917
        def rnd(a, bb, k): return S.to_f(hash_rnd(S, a, bb, S.u(k), seed)) / 65535.0
        gr = S.ne(P.grey, 0)
        l = luma(S, r, g, b)
        r = S.select(gr, l + (P.cr - 0.5) * P.tint, r)
        g = S.select(gr, l + (P.cg - 0.5) * P.tint, g)
        b = S.select(gr, l + (P.cb - 0.5) * P.tint, b)
        n = (rnd(x, y, 7) - 0.5) * P.grain * 0.5 * P.gk
        r = r + n; g = g + n; b = b + n
        on = S.ne(P.particles, 0)
        scr = on & S.eq(P.type, 4) & (rnd(x / P.c3, S.u(0), 11) < P.scratch)
        dust = on & ~scr & S.eq(P.type, 0) & (rnd(x / P.c2, y / P.c2, 13) > P.dust)
        fl = on & ~scr & ~dust & S.eq(P.type, 1)
        hair = on & ~scr & ~dust & ~fl & S.eq(P.type, 2) & (rnd(x / P.c4, y / P.c40, 19) > P.hair)
        f = 1.0 + (rnd(S.u(0), S.u(0), 17) - 0.5) * P.amount * 0.3
        mul = S.select(dust, S.f(0.2), S.select(hair, S.f(0.1), S.select(fl, f, S.f(1.0))))
        add = S.select(scr, S.f(0.35), S.f(0.0))
        r = r * mul + add; g = g * mul + add; b = b * mul + add
        return clamp01(S, r), clamp01(S, g), clamp01(S, b)
    pixel_row(S, body, True)

@kernel("bumpmap", WH + [('type', 'u'), ('chan', 'u'), ('sx', 'f'), ('sy', 'f'), ('sz', 'f'), ('ax', 'f'), ('ay', 'f'), ('az', 'f'), ('hs', 'f'), ('amb2', 'f'), ('i8', 'f'),
                         ('kd', 'f'), ('ks', 'f'), ('power', 'f'), ('cut', 'f'), ('icut', 'f'), ('unit', 'f'), ('cx', 'f'), ('cy', 'f')], [PIC_IN, PIC_OUT])
def bumpmap(S):
    # bumpMapRender() in builtins_more_creative.cpp: a Prewitt gradient of the chosen channel is the normal; a Phong light on the colour
    P = S.p
    with S.in_range(P.w, P.h):
        W = P.w
        x, y = S.gx, S.gy
        inb = S.buf('in0')
        up = S.select(y > 0, y - 1, S.u(0))
        dn = S.min(y + 1, P.h - 1)
        def h(xx, yy):
            q = inb.load(yy * W + xx)
            return S.select(S.eq(P.chan, 0), q.x, S.select(S.eq(P.chan, 1), q.y, S.select(S.eq(P.chan, 2), q.z, q.w)))
        gx = S.var('f', 0.0)
        gy = S.var('f', 0.0)
        with S.if_(S.eq(W, 1)):
            gy.set(3.0 * (h(x, up) - h(x, dn)))
        with S.if_(W > 1):
            with S.if_(S.eq(x, 0)):
                gx.set(((h(S.u(0), dn) - h(S.u(1), dn)) + (h(S.u(0), y) - h(S.u(1), y))) + (h(S.u(0), up) - h(S.u(1), up)))
                gy.set((h(S.u(0), up) - h(S.u(0), dn)) + 2.0 * (h(S.u(1), up) - h(S.u(1), dn)))
            with S.else_():
                with S.if_(S.eq(x, W - 1)):
                    gx.set(((h(x - 1, dn) - h(x, dn)) + (h(x - 1, y) - h(x, y))) + (h(x - 1, up) - h(x, up)))
                    gy.set((h(x - 1, up) - h(x - 1, dn)) + 2.0 * (h(x, up) - h(x, dn)))
                with S.else_():
                    gx.set(((h(x - 1, dn) - h(x + 1, dn)) + (h(x - 1, y) - h(x + 1, y))) + (h(x - 1, up) - h(x + 1, up)))
                    gy.set(((h(x - 1, up) - h(x - 1, dn)) + (h(x + 1, up) - h(x + 1, dn))) + (h(x, up) - h(x, dn)))
        nx0, ny0 = gx.get() * P.hs, gy.get() * P.hs
        nl = S.sqrt(nx0 * nx0 + ny0 * ny0 + 1.0)
        nx, ny, nz = nx0 / nl, ny0 / nl, S.f(1.0) / nl
        # the light: directional (type 0) from (ax, ay, az); the others a point at (sx, sy, sz)
        px = (S.to_f(x) - P.cx) * P.unit
        py = (S.to_f(y) - P.cy) * P.unit
        pl = [P.sx - px, P.sy - py, P.sz]
        d = S.sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2])
        pos = d > 0.0
        inv0 = S.f(1.0) / S.select(pos, d, S.f(1.0))
        inv = S.select(pos, inv0, S.f(100.0))
        plx = S.select(pos, pl[0] * inv, S.f(0.0))
        ply = S.select(pos, pl[1] * inv, S.f(0.0))
        plz = S.select(pos, pl[2] * inv, S.f(1.0))
        a = S.min(S.max((plx * P.ax + ply * P.ay + plz * P.az - P.cut) * P.icut, 0.0), 1.0)
        att_spot = a * a * 2.0
        att = S.select(S.eq(P.type, 1), att_spot, S.f(1.0)) * inv * 2.0
        directional = S.eq(P.type, 0)
        lx = S.select(directional, P.ax, plx)
        ly = S.select(directional, P.ay, ply)
        lz = S.select(directional, P.az, plz)
        att = S.select(directional, S.f(1.0), att)
        ndl = nx * lx + ny * ly + nz * lz
        rz = nz * 2.0 * ndl - lz
        spec = fpow(S, S.max(rz, 0.01), P.power)
        k = S.max(0.0, P.i8 * (P.kd * ndl + P.ks * spec) * att + P.amb2)
        s = inb.load(y * W + x)
        S.buf('out').store(y * W + x, S.vec4(s.x * k, s.y * k, s.z * k, s.w))

# ---------------------------------------------------------------------------------------------------------- colour tables (builtins_color.cpp)
def lut_apply(S, tab, off, n, dmin, dmax, tetra, rgb):
    """lutApply(): one straight colour through a 3D table at tab[off ...] (n a side, r fastest); trilinear or (tetra true) tetrahedral. off: uint, n: uint."""
    nf = S.to_f(n)
    p = []
    for k in range(3):
        span = dmax[k] - dmin[k]
        v = S.select(span > 1e-12, (rgb[k] - dmin[k]) / S.select(span > 1e-12, span, S.f(1.0)), S.f(0.0))
        v = S.select(v < 0.0, S.f(0.0), S.select(v > 1.0, S.f(1.0), v))
        p.append(v * (nf - 1.0))
    i0 = [S.min(S.to_u(p[k]), n - 2) for k in range(3)]
    f = [p[k] - S.to_f(i0[k]) for k in range(3)]
    base = off + ((i0[2] * n + i0[1]) * n + i0[0]) * 3
    sr, sg, sb = S.u(3), n * 3, n * n * 3
    def at(r, g, b, k):
        return tab.load(base + (sr * r) + (sg * g) + (sb * b) + k)
    out = []
    for k in range(3):
        c = {(r, g, b): at(r, g, b, k) for r in (0, 1) for g in (0, 1) for b in (0, 1)}
        c00 = c[0, 0, 0] * (1.0 - f[0]) + c[1, 0, 0] * f[0]
        c10 = c[0, 1, 0] * (1.0 - f[0]) + c[1, 1, 0] * f[0]
        c01 = c[0, 0, 1] * (1.0 - f[0]) + c[1, 0, 1] * f[0]
        c11 = c[0, 1, 1] * (1.0 - f[0]) + c[1, 1, 1] * f[0]
        c0 = c00 * (1.0 - f[1]) + c10 * f[1]
        c1 = c01 * (1.0 - f[1]) + c11 * f[1]
        tri = c0 * (1.0 - f[2]) + c1 * f[2]
        fr, fg, fb = f
        c000, c111 = c[0, 0, 0], c[1, 1, 1]
        t_a = c000 + fr * (c[1, 0, 0] - c000) + fg * (c[1, 1, 0] - c[1, 0, 0]) + fb * (c111 - c[1, 1, 0])
        t_b = c000 + fr * (c[1, 0, 0] - c000) + fb * (c[1, 0, 1] - c[1, 0, 0]) + fg * (c111 - c[1, 0, 1])
        t_c = c000 + fb * (c[0, 0, 1] - c000) + fr * (c[1, 0, 1] - c[0, 0, 1]) + fg * (c111 - c[1, 0, 1])
        t_d = c000 + fb * (c[0, 0, 1] - c000) + fg * (c[0, 1, 1] - c[0, 0, 1]) + fr * (c111 - c[0, 1, 1])
        t_e = c000 + fg * (c[0, 1, 0] - c000) + fb * (c[0, 1, 1] - c[0, 1, 0]) + fr * (c111 - c[0, 1, 1])
        t_f = c000 + fg * (c[0, 1, 0] - c000) + fr * (c[1, 1, 0] - c[0, 1, 0]) + fb * (c111 - c[1, 1, 0])
        upper = S.select(fg >= fb, t_a, S.select(fr >= fb, t_b, t_c))
        lower = S.select(fb >= fg, t_d, S.select(fb >= fr, t_e, t_f))
        tet = S.select(fr >= fg, upper, lower)
        out.append(S.select(S.ne(tetra, 0), tet, tri))
    return out

@kernel("lutfilter", WH + [('n', 'u'), ('tetra', 'u'), ('gain', 'f'), ('d0', 'f'), ('d1', 'f'), ('d2', 'f'), ('e0', 'f'), ('e1', 'f'), ('e2', 'f')], [PIC_IN, PIC_OUT, TABLE])
def lutfilter(S):
    # lutFilter(): the straight colour through the table, mixed in by Gain
    P = S.p
    def body(S, r, g, b):
        v = lut_apply(S, S.buf('table'), S.u(0), P.n, [P.d0, P.d1, P.d2], [P.e0, P.e1, P.e2], P.tetra, [r, g, b])
        return r + (v[0] - r) * P.gain, g + (v[1] - g) * P.gain, b + (v[2] - b) * P.gain
    pixel_row(S, body)

def rgb_to_hsv(S, r, g, b):
    mx = S.max(r, S.max(g, b))
    mn = S.min(r, S.min(g, b))
    d = mx - mn
    s = S.select(mx > 1e-9, d / S.select(mx > 1e-9, mx, S.f(1.0)), S.f(0.0))
    dd = S.select(d < 1e-9, S.f(1.0), d)
    hr = (g - b) / dd + S.select(g < b, S.f(6.0), S.f(0.0))
    hg = (b - r) / dd + 2.0
    hb = (r - g) / dd + 4.0
    h = S.select(S.eq(mx, r), hr, S.select(S.eq(mx, g), hg, hb)) * 60.0
    return S.select(d < 1e-9, S.f(0.0), h), s, mx

def hsv_to_rgb(S, h, s, v):
    h = S.fmod(h, 360.0)
    h = S.select(h < 0.0, h + 360.0, h)
    hh = h / 60.0
    i = S.to_u(hh)
    f = hh - S.to_f(i)
    p = v * (1.0 - s)
    q = v * (1.0 - s * f)
    t = v * (1.0 - s * (1.0 - f))
    i = i % 6
    def pick(a0, a1, a2, a3, a4, a5):
        return S.select(S.eq(i, 0), a0, S.select(S.eq(i, 1), a1, S.select(S.eq(i, 2), a2, S.select(S.eq(i, 3), a3, S.select(S.eq(i, 4), a4, a5)))))
    return pick(v, q, p, p, t, v), pick(t, v, v, q, p, p), pick(p, p, t, v, v, q)

@kernel("colorgrading", WH + [('flags', 'u'), ('inmin', 'f'), ('inscale', 'f'), ('expo', 'f'), ('gain0', 'f'), ('gain1', 'f'), ('gain2', 'f'), ('lift0', 'f'), ('lift1', 'f'), ('lift2', 'f'),
                              ('ig0', 'f'), ('ig1', 'f'), ('ig2', 'f'), ('off0', 'f'), ('off1', 'f'), ('off2', 'f'), ('sat', 'f'), ('hue', 'f'), ('hsvsat', 'f'), ('hsvval', 'f'),
                              ('outmin', 'f'), ('outspan', 'f')], [PIC_IN, PIC_OUT, TABLE])
def colorgrading(S):
    # colorGrading() in builtins_color.cpp. Table: two LUT headers of 10 floats (gain, tetra, n, dmin x3, dmax x3, data offset), then 4 curves of 1025, then the LUT data
    P = S.p
    tab = S.buf('table')
    def flag(bit): return S.ne(P.flags & bit, 0)
    def body(S, r, g, b):
        v = [r, g, b]
        v = [S.select(flag(1), (c - P.inmin) * P.inscale, c) for c in v]
        v = [S.select(flag(2), c * P.expo, c) for c in v]
        gain = [P.gain0, P.gain1, P.gain2]; lift = [P.lift0, P.lift1, P.lift2]; ig = [P.ig0, P.ig1, P.ig2]; off = [P.off0, P.off1, P.off2]
        nv = []
        for k in range(3):
            t = gain[k] * (v[k] + lift[k] * (1.0 - v[k]))
            t2 = S.select(t >= 0.0, S.pow(S.max(t, 0.0), ig[k]), S.f(0.0) - S.pow(S.max(S.f(0.0) - t, 0.0), ig[k]))
            nv.append(S.select(flag(4), t2 + off[k], v[k]))
        v = nv
        l = luma(S, v[0], v[1], v[2])
        v = [S.select(flag(8), l + (c - l) * P.sat, c) for c in v]
        # hue / saturation / value
        cl = [S.min(S.f(1.0), S.max(S.f(0.0), c)) for c in v]
        h, s, vv = rgb_to_hsv(S, cl[0], cl[1], cl[2])
        h = h + P.hue
        s = S.min(S.f(1.0), S.max(S.f(0.0), s * P.hsvsat))
        vv = S.max(S.f(0.0), vv * P.hsvval)
        hr, hg, hb = hsv_to_rgb(S, h, s, vv)
        v = [S.select(flag(16), x, c) for x, c in zip((hr, hg, hb), v)]
        # curves
        def lookup(cb, x):
            x = S.select(x < 0.0, S.f(0.0), S.select(x > 1.0, S.f(1.0), x))
            pp = x * 1024.0
            i = S.min(S.to_u(pp), 1023)
            a = tab.load(S.u(20 + cb * 1025) + i)
            b2 = tab.load(S.u(20 + cb * 1025 + 1) + i)
            return a + (b2 - a) * (pp - S.to_f(i))
        cv = []
        for k in range(3):
            x = v[k]
            x = S.select(flag(32) & flag(128 << k), lookup(k, x), x)
            x = S.select(flag(32) & flag(128 << 3), lookup(3, x), x)
            cv.append(x)
        v = cv
        v = [S.select(flag(64), P.outmin + c * P.outspan, c) for c in v]
        for slot in range(2):
            hb0 = slot * 10
            gn = tab.load(S.u(hb0))
            tet = S.to_u(tab.load(S.u(hb0 + 1)))
            n = S.to_u(tab.load(S.u(hb0 + 2)))
            dmin = [tab.load(S.u(hb0 + 3 + k)) for k in range(3)]
            dmax = [tab.load(S.u(hb0 + 6 + k)) for k in range(3)]
            offs = S.to_u(tab.load(S.u(hb0 + 9)))
            w = lut_apply(S, tab, offs, S.max(n, S.u(2)), dmin, dmax, tet, v)
            on = flag(4096 << slot)
            v = [S.select(on, c + (x - c) * gn, c) for x, c in zip(w, v)]
        return v[0], v[1], v[2]
    pixel_row(S, body)

# ---------------------------------------------------------------------------------------------------------- Color Match (builtins_color.cpp)
def cbrt_f(S, t):
    """cbrt of a positive float as the CPU has it: (float)cbrt((double)t). The double cube root is x ** (1/3) in the software double with 1/3 as a double
    (its error is under one double ulp, about 1e-16: the rounded float differs from the library's only for a value within that of a rounding boundary)."""
    import sf64
    x = SF.from_f32(S, S.max(t, 1e-30))
    return sf64.sf_pow(S, x, SF.const(S, 1.0 / 3.0)).to_f32()

def srgb_tab(S, tab, base, v):
    """tab(): a table of 4097 values read with linear interpolation, v clamped to 0..1."""
    v = S.select(v < 0.0, S.f(0.0), S.select(v > 1.0, S.f(1.0), v))
    p = v * 4096.0
    i = S.min(S.to_u(p), 4095)
    a = tab.load(S.u(base) + i)
    b = tab.load(S.u(base + 1) + i)
    return a + (b - a) * (p - S.to_f(i))

def to_lab(S, tab, r, g, b):
    lr, lg, lb = srgb_tab(S, tab, 0, r), srgb_tab(S, tab, 0, g), srgb_tab(S, tab, 0, b)
    X = (0.4124564 * lr + 0.3575761 * lg + 0.1804375 * lb) / 0.95047
    Y = 0.2126729 * lr + 0.7151522 * lg + 0.0721750 * lb
    Z = (0.0193339 * lr + 0.1191920 * lg + 0.9503041 * lb) / 1.08883
    def f(t): return S.select(t > 0.008856, cbrt_f(S, t), 7.787 * t + S.f(16.0 / 116.0))
    fx, fy, fz = f(X), f(Y), f(Z)
    return 116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)

@kernel("colormatch_rows", WH, [PIC_IN, PIC_OUT, TABLE])
def colormatch_rows(S):
    # the 160 x 90 samples of colorMatch()'s frame statistics, one thread each: out[j * 160 + i] = (L, a, b, 1 for a sample that counts else 0)
    # (the table: the two sRGB tables (4097 each), then the 160 sample columns and 90 sample rows)
    P = S.p
    with S.if_((S.gx < 160) & (S.gy < 90)):
        tab = S.buf('table')
        y = S.to_u(tab.load(S.u(8194 + 160) + S.gy))
        x = S.to_u(tab.load(S.u(8194) + S.gx))
        p = S.buf('in0').load(y * P.w + x)
        a = p.w
        has = a > 1e-6
        r = S.select(has, p.x / a, S.f(0.0))
        g = S.select(has, p.y / a, S.f(0.0))
        b = S.select(has, p.z / a, S.f(0.0))
        L, A, B = to_lab(S, tab, r, g, b)
        cnt = S.select(a > 0.0, S.f(1.0), S.f(0.0))
        S.buf('out').store(S.gy * 160 + S.gx, S.vec4(L, A, B, cnt))

@kernel("colormatch_params", WH, [PIC_IN, PIC_OUT, TABLE])
def colormatch_params(S):
    # the numbers the picture pass needs, all in doubles as the CPU has them. table: the 12 statistics (doubles as two words each), the captured-target flag,
    # the strength, the lightness flag. Without a captured target the target's statistics are the samples' (summed in the CPU's order: row by row, one thread).
    P = S.p
    with S.if_(S.eq(S.gx, 0) & S.eq(S.gy, 0)):
        tab = S.buf('table')
        captured = S.ne(S.f_to_bits(tab.load(S.u(24))), 0)
        zero = SF.const(S, 0.0)
        sums = [SFVar(S, 0.0) for _ in range(3)]
        sqs = [SFVar(S, 0.0) for _ in range(3)]
        count = S.var('u', S.u(0))
        with S.if_(~captured):
            with S.for_(0, 14400) as k:
                v = S.buf('in0').load(k)
                with S.if_(v.w > 0.5):
                    for q in range(3):
                        d = SF.from_f32(S, v[q])
                        sums[q].set(sums[q].get() + d)
                        sqs[q].set(sqs[q].get() + d * d)
                    count.set(count.get() + 1)
        cnt = SF.from_u32(S, count.get())
        mul, tm, sm = [], [], []
        for k in range(3):
            ss = ld_sf(S, tab, 2 * (2 * k + 1))
            smean = ld_sf(S, tab, 2 * (2 * k))
            mean_t = sums[k].get() / cnt
            var_t = sqs[k].get() / cnt - mean_t * mean_t
            std_t = SF.sel(S, var_t > 0.0, var_t, zero).sqrt()
            tmean = SF.sel(S, captured, ld_sf(S, tab, 2 * (6 + 2 * k)), mean_t)
            tstd = SF.sel(S, captured, ld_sf(S, tab, 2 * (7 + 2 * k)), std_t)
            ratio = ss / SF.sel(S, tstd > 0.05, tstd, SF.const(S, 1.0))
            ratio = SF.sel(S, ratio > 0.0, ratio, zero)
            ratio = SF.sel(S, ratio < 8.0, ratio, SF.const(S, 8.0))
            m = SF.sel(S, tstd > 0.05, ratio, SF.const(S, 1.0))
            mul.append(m.to_f32()); tm.append(tmean.to_f32()); sm.append(smean.to_f32())
        ok = S.select(captured | (count.get() >= 4), S.f(1.0), S.f(0.0))
        S.buf('out').store(S.u(0), S.vec4(tm[0], tm[1], tm[2], ok))
        S.buf('out').store(S.u(1), S.vec4(mul[0], mul[1], mul[2], tab.load(S.u(25))))
        S.buf('out').store(S.u(2), S.vec4(sm[0], sm[1], sm[2], tab.load(S.u(26))))

@kernel("colormatch", WH, [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def colormatch(S):
    # colorMatch(): to Lab, (x - target mean) * scale + source mean, back; mixed in by the strength. Table: the sRGB decode (4097) and encode (4097) tables
    P = S.p
    with S.in_range(P.w, P.h):
        tab = S.buf('table')
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        q0, q1, q2 = S.buf('in1').load(S.u(0)), S.buf('in1').load(S.u(1)), S.buf('in1').load(S.u(2))
        ok = q0.w > 0.5
        a = p.w
        has = a > 1e-6
        r = S.select(has, p.x / a, S.f(0.0))
        g = S.select(has, p.y / a, S.f(0.0))
        b = S.select(has, p.z / a, S.f(0.0))
        L, A, B = to_lab(S, tab, r, g, b)
        light = q2.w > 0.5
        L2 = S.select(light, (L - q0.x) * q1.x + q2.x, L)
        A2 = (A - q0.y) * q1.y + q2.y
        B2 = (B - q0.z) * q1.z + q2.z
        L2 = S.min(S.f(100.0), S.max(S.f(0.0), L2))
        fy = (L2 + 16.0) / 116.0
        fx = fy + A2 / 500.0
        fz = fy - B2 / 200.0
        def finv(t):
            t3 = t * t * t
            return S.select(t3 > 0.008856, t3, (t - S.f(16.0 / 116.0)) / 7.787)
        X = finv(fx) * 0.95047
        Y = finv(fy)
        Z = finv(fz) * 1.08883
        lr = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z
        lg = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z
        lb = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z
        v0, v1, v2 = srgb_tab(S, tab, 4097, lr), srgb_tab(S, tab, 4097, lg), srgb_tab(S, tab, 4097, lb)
        s = q1.w
        o0, o1, o2 = r + (v0 - r) * s, g + (v1 - g) * s, b + (v2 - b) * s
        valid = a > 0.0
        z = S.f(0.0)
        res = S.vec4(S.select(valid, o0 * a, z), S.select(valid, o1 * a, z), S.select(valid, o2 * a, z), S.select(valid, a, z))
        S.buf('out').store(i, S.select(ok, res, p))

# ---------------------------------------------------------------------------------------------------------- Displacement Map, Height Map, Mesh Warp (builtins_vegasrest.cpp, builtins_magix.cpp)
def channel_of(S, p, which):
    """channelOf(): the straight colour channel 'which' (0..2, a kernel value) of a premultiplied pixel, held to 0..1."""
    big = p.w > 1e-5
    a = S.select(big, S.f(1.0) / S.select(big, p.w, S.f(1.0)), S.f(0.0))
    v = S.select(S.eq(which, 0), p.x * a, S.select(S.eq(which, 1), p.y * a, p.z * a))
    return clamp01(S, v)

@kernel("heightplane", WH + [('ch', 'u'), ('inv', 'u'), ('alpha', 'u'), ('flip', 'u')], [PIC_IN, PLANE_OUT])
def heightplane(S):
    # the height of heightMap(): one number per pixel
    P = S.p
    with S.in_range(P.w, P.h):
        ry = S.select(S.ne(P.flip, 0), P.h - 1 - S.gy, S.gy)
        p = S.buf('in0').load(ry * P.w + S.gx)
        r, g, b = channel_of(S, p, S.u(0)), channel_of(S, p, S.u(1)), channel_of(S, p, S.u(2))
        one = channel_of(S, p, S.select(P.ch > 0, P.ch - 1, S.u(0)))
        v = S.select(S.eq(P.ch, 0), luma(S, r, g, b), one)
        v = S.select(S.ne(P.inv, 0), 1.0 - v, v)
        v = S.select(S.ne(P.alpha, 0), v * p.w, v)
        S.buf('out').store(S.gy * P.w + S.gx, v)

@kernel("tvsimulator", WH + [('phos', 'f'), ('grill', 'f'), ('inter', 'f'), ('scan', 'f'), ('stat', 'f'), ('scale', 'f'), ('full', 'u'), ('t7', 'u')],
        [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def tvsimulator(S):
    # tvSimulator(): the CPU's sample position is a pair of doubles. The table holds (H = the height, W = the width): the line wobble of each row as a double (2 words a row),
    # its brightness band as a float (H words), the row's sample y as a double (2 words a row) and each column's x without the wobble as a double (2 words a column);
    # the position is the column's x plus the row's wobble, in the software double
    P = S.p
    with S.in_range(P.w, P.h):
        tab = S.buf('table')
        wob = ld_sf(S, tab, S.gy * 2)
        bandv = tab.load(P.h * 2 + S.gy)
        zy = ld_sf(S, tab, P.h * 3 + S.gy * 2)
        zx = ld_sf(S, tab, P.h * 5 + S.gx * 2) + wob
        p = sample_clamped_d(S, S.buf('in0'), P.w, P.h, zx, zy)
        g = sample_clamped_d(S, S.buf('in1'), P.w, P.h, zx, zy)
        k = P.phos * 0.5
        o = [p[c] + (g[c] * 1.1 - p[c]) * k for c in range(3)]
        o[2] = o[2] + P.phos * 0.04 * g[3]
        col = S.gx % 3
        full = S.ne(P.full, 0)
        grill_on = P.grill > 0.0
        res = []
        own = 1.0 + P.grill * 0.6
        other = 1.0 - P.grill * 0.5
        mean = S.select(grill_on, (own + 2.0 * other) / 3.0, S.f(1.0))
        for c in range(3):
            iscol = S.eq(col, c)
            v = o[c] * (1.0 - P.grill * 0.5 * S.select(iscol, S.f(0.0), S.f(1.0)))
            v = S.select(grill_on, v * (1.0 + P.grill * 0.6 * S.select(iscol, S.f(1.0), S.f(0.0))), v)
            res.append(S.select(full, v, o[c] * mean))
        odd = S.ne(S.gy % 2, 0)
        line = S.select(full, 1.0 - P.inter * 0.5 * S.select(odd, S.f(1.0), S.f(0.0)), 1.0 - P.inter * 0.25)
        res = [c * (line * bandv) for c in res]
        h = S.gx * 374761393 + (S.gy + P.t7) * 668265263
        h = (h ^ (h >> 13)) * 1274126177
        n = (S.to_f((h ^ (h >> 16)) & 65535) / 65535.0 - 0.5) * P.stat * 0.8 * P.scale
        res = [S.select(P.stat > 0.0, c + n * g[3], c) for c in res]
        a = p.w
        res = [S.min(S.max(c, 0.0), a) for c in res]
        S.buf('out').store(S.gy * P.w + S.gx, S.vec4(res[0], res[1], res[2], a))

# ---------------------------------------------------------------------------------------------------------- Lens Flare (builtins_lensflare.cpp)
@kernel("lensflare", WH + [('n', 'u'), ('blend', 'f')], [PIC_IN, PIC_OUT, TABLE])
def lensflare(S):
    # VEGAS's Lens Flare: the frame in 8 bits, each flare added in turn (the CPU's box / place arithmetic is in the table, 16 floats a flare from 96;
    # the ray lengths at 0, the hexagon at 64 as x[6] y[6] nx[6] ny[6])
    P = S.p
    with S.in_range(P.w, P.h):
        tab = S.buf('table')
        i = S.gy * P.w + S.gx
        p = S.buf('in0').load(i)
        def q8(v): return S.round_even(clamp01(S, v) * 255.0)
        q = [S.var('f', q8(p[k])) for k in range(3)]
        A = q8(p[3])
        row = S.to_i(P.h - 1 - S.gy)
        cxi = S.to_i(S.gx)
        with S.for_(0, P.n) as n:
            base = 96 + n * 16
            def ti(k): return S.to_i(tab.load(base + k))
            def tf(k): return tab.load(base + k)
            ty, edi, r13, endy, ebx, r14, r12 = ti(0), ti(1), ti(2), ti(3), ti(4), ti(5), ti(6)
            inbox = (row >= edi) & (row < edi + (endy - r13)) & (cxi >= ebx) & (cxi < ebx + (r12 - r14))
            with S.if_(inbox):
                fy, fx, r, I = tf(7), tf(8), tf(9), tf(13)
                yy = S.to_f(r13 + (row - edi))
                xx = S.to_f(r14 + (cxi - ebx))
                v = 1.0 - (yy - fy) / r
                u = 1.0 - (xx - fx) / r
                d = S.sqrt(u * u + v * v)
                # the hexagon
                m = None
                for h in range(6):
                    t0 = (v - tab.load(70 + h)) * tab.load(76 + h)
                    t1 = (u - tab.load(64 + h)) * tab.load(82 + h)
                    sh = t0 + t1
                    m = sh if m is None else S.min(sh, m)
                lt1 = d < 1.0
                # type 0, 1, 2
                s0 = 1.0 - d
                s1 = S.select(d > 0.8, (1.0 - d) * 5.0, S.select(d > 0.75, (d - 0.75) * 5.0 + 0.75, d))
                t = 1.0 - d
                s2 = S.select(S.f(0.1) > t, t * 10.0, (0.15 - t) * 20.0)
                hit2 = lt1 & (d >= 0.85)
                # type 3 (the hexagon with soft edges), 4
                s3 = S.select(S.f(0.2) > m, clamp01(S, m / 0.2), clamp01(S, (0.8 - (m - 0.2)) / 0.8))
                s4 = m * m
                hhit = m >= 0.0
                # type 5: the rays
                ang = S.atan2(v, u)
                # (the CPU's doubles: the angle plus pi rounded to a float, scaled to 0..180, the table read with a double blend, the cube of 1 - q)
                ad = SF.from_f32(S, (SF.from_f32(S, ang) + 3.141592653589793).to_f32())
                ad = ad / 6.283185307179586 * 3.0
                ad = ad * 60.0
                fr = ad - ad.trunc()
                ii = S.to_u(ad.trunc_i32())
                t0 = SF.from_f32(S, tab.load(ii & 63))
                t1 = SF.from_f32(S, tab.load((ii + 1) & 63))
                w = (((SF.const(S, 1.0) - fr) * t0) + (fr * t1)).to_f32()
                qq = S.select(S.f(0.0) >= d, S.f(0.0), S.select(d < w, d / w, S.f(1.0)))
                xq = SF.const(S, 1.0) - SF.from_f32(S, qq)
                s5 = ((xq * xq) * xq).to_f32()
                s5 = S.select(d < 0.3, (d / 0.3) * (s5 - 1.0) + 1.0, s5)
                s = S.select(S.eq(ty, 0), s0, S.select(S.eq(ty, 1), s1, S.select(S.eq(ty, 2), s2, S.select(S.eq(ty, 3), s3, S.select(S.eq(ty, 4), s4, s5)))))
                hit = S.select(S.eq(ty, 2), hit2, S.select(S.eq(ty, 3) | S.eq(ty, 4), hhit, lt1))
                for k in range(3):
                    cin = q[k].get()
                    o = s * tf(10 + k) * I
                    sm = o * 255.0 + cin
                    mm = S.min(A, sm)
                    val = mm * P.blend + cin * (1.0 - P.blend)
                    nq = S.to_f(S.to_u(S.to_i(val)) % 256)
                    q[k].set(S.select(hit, nq, cin))
        S.buf('out').store(i, S.vec4(q[0].get() / 255.0, q[1].get() / 255.0, q[2].get() / 255.0, A / 255.0))


def load_extra_kernels():
    """the kernels of tools/gpukernels_*.py (one file per area), in the order of their names"""
    import glob, importlib
    sys.modules.setdefault('gpukernels', sys.modules[__name__])
    for path in sorted(glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gpukernels_*.py'))):
        importlib.import_module(os.path.basename(path)[:-3])

def main():
    load_extra_kernels()
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
