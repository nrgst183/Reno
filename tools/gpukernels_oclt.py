"""The transitions as VEGAS draws them with its OpenCL kernels (GPU acceleration on, Ctx::openCl): float pictures, the 12-bit store in an 8-bit frame.
Each kernel mirrors the pixel function of the CPU port (src/ofx/vfx*.h, builtins_trans.cpp) float operation for float operation: the C++ side
(src/ofx/vktransocl.cpp) builds the same host-side struct the CPU code does and hands its numbers over as push constants or a table; the kernel does the per-pixel part.
Imported by gpukernels.py (see its header). Conventions: in0 is the picture the CPU code calls 'first' (the host swaps the two buffers when the CPU code does),
in1 the second one; both are top-row-first buffers of premultiplied RGBA floats. A kernel's own y counts from the BOTTOM row (y = H - 1 - gy), as the CPU ports do.
"""
import struct
from gpukernels import kernel, PIC_IN, PIC_IN1, PIC_OUT, TABLE, WH, SF
from sf64 import sf_atan2


def f32(x):
    """x rounded to a float, as a Python number"""
    return struct.unpack('<f', struct.pack('<f', x))[0]


FS = lambda *names: [(n, 'f') for n in names]
Q8 = [('q8', 'u')]


# ---------------------------------------------------------------------------------------------------------- shared pieces
def fmin_(S, a, b):
    return S.min(a, b)


def fmax_(S, a, b):
    return S.max(a, b)


def clampf(S, v, lo, hi):
    """clampf of vfxwipes.h: fmin(fmax(v, lo), hi)"""
    return S.min(S.max(v, lo), hi)


def smooth3(S, m):
    """((-2 * m) + 3) * m * m"""
    return ((-2.0 * m) + 3.0) * m * m


def mix4(S, a, b, t):
    """a + (b - a) * t on vec4s (t a float)"""
    return a + (b - a) * t


def zero4(S):
    return S.splat4(0.0)


def store12(S, v):
    """more::oclStore12 of a float: the value kept in 0..1, cut to 12 bits and scaled to a byte with the half down; the level over 255 (not a number: 0)"""
    n = S.to_i(S.floor(S.min(S.max(v, 0.0), 1.0) * 4096.0))
    r = (n * 255 + 2047) >> 12
    out = S.to_f(r) / S.f(255.0)
    return S.select(S.isnan(v), S.f(0.0), out)


def finish(S, i, v, q8):
    """the output pixel: cut with the 12-bit store when the frame is 8-bit (q8 a uint value), else as it is"""
    cut = S.vec4(store12(S, v.x), store12(S, v.y), store12(S, v.z), store12(S, v.w))
    S.buf('out').store(i, S.select(S.ne(q8, 0), cut, v))


def texel(S, buf, W, H, x, y):
    """Pic::texel: x, y signed ints (y counted from the bottom row), a picture of top-first rows; zero outside"""
    inside = (x >= 0) & (y >= 0) & (x < W) & (y < H)
    idx = S.select(inside, S.to_u((H - 1 - y) * W + x), S.u(0))
    return S.select(inside, buf.load(idx), zero4(S))


def bilinear(S, buf, W, H, x, y):
    """bilinear() of vfxwipes.h (four nearest reads, weights in floats); x, y floats"""
    flx, fly = S.floor(x), S.floor(y)
    fx, fy = x - flx, y - fly
    below = S.f(0.99999994)
    fx = S.select(fx > below, below, fx)
    fy = S.select(fy > below, below, fy)
    ix, iy = S.to_i(flx), S.to_i(fly)
    v7, v8 = 1.0 - fy, 1.0 - fx
    w00, w10, w01, w11 = v7 * v8, v7 * fx, fy * v8, fy * fx
    a = texel(S, buf, W, H, ix, iy)
    b = texel(S, buf, W, H, ix + 1, iy)
    c = texel(S, buf, W, H, ix, iy + 1)
    d = texel(S, buf, W, H, ix + 1, iy + 1)
    return ((a * w00 + b * w10) + c * w01) + d * w11


def bilinear_hw(S, buf, W, H, cx, cy):
    """bilinearHw() of vfxwipes.h (the card's linear sampler: weights cut to 8 bits)"""
    x, y = (cx + 0.5) - 0.5, (cy + 0.5) - 0.5
    flx, fly = S.floor(x), S.floor(y)
    fx, fy = x - flx, y - fly
    fx = S.floor(fx * 256.0 + 0.5) * f32(1.0 / 256.0)
    fy = S.floor(fy * 256.0 + 0.5) * f32(1.0 / 256.0)
    x0, y0 = S.to_i(flx), S.to_i(fly)
    a = texel(S, buf, W, H, x0, y0)
    b = texel(S, buf, W, H, x0 + 1, y0)
    c = texel(S, buf, W, H, x0, y0 + 1)
    d = texel(S, buf, W, H, x0 + 1, y0 + 1)
    top = a * (1.0 - fx) + b * fx
    bot = c * (1.0 - fx) + d * fx
    return top * (1.0 - fy) + bot * fy


def colour(S, P, base='c'):
    return S.vec4(getattr(P, base + '0'), getattr(P, base + '1'), getattr(P, base + '2'), getattr(P, base + '3'))


class Px:
    """a pixel of the kernel: its buffer index, and x, y (y from the bottom) as ints and floats"""
    def __init__(self, S):
        P = S.p
        self.W, self.H = S.to_i(P.w), S.to_i(P.h)
        self.i = S.gy * P.w + S.gx
        self.xi = S.to_i(S.gx)
        self.yi = self.H - 1 - S.to_i(S.gy)
        self.x = S.to_f(self.xi)
        self.y = S.to_f(self.yi)


def mixvar(S, v):
    """a vec4 variable"""
    return S.var('v4', v)


# ---------------------------------------------------------------------------------------------------------- Dissolve (dissolvek::pixel)
@kernel("oclt_dissolve", WH + [('type', 'i')] + Q8 + FS('p', 'q0', 'q1', 'q2', 'q3'), [PIC_IN, PIC_IN1, PIC_OUT])
def oclt_dissolve(S):
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        a = S.buf('in0').load(i)
        b = S.buf('in1').load(i)
        p = P.p
        col = S.vec4(P.q0, P.q1, P.q2, P.q3)
        ty = P.type
        o = S.var('v4', zero4(S))

        def mixf(x, y, t):
            return x + (y - x) * t

        def clamp01(v):
            return S.min(S.max(v, 0.0), 1.0)

        def clamp01v(v):
            return S.vec4(clamp01(v.x), clamp01(v.y), clamp01(v.z), clamp01(v.w))

        def with3(v, w):
            return S.vec4(v.x, v.y, v.z, w)

        with S.if_(S.eq(ty, 0)):
            early = p < 0.5
            t = S.select(early, p, S.f(0.5) - p)
            add = S.select(early, S.f(0.0), S.f(1.0))
            s1 = S.select(early, b, a)
            s2 = S.select(early, a, b)
            k = 2.0 * t + add
            o.set(s1 * k + s2)
        with S.if_(S.eq(ty, 1)):
            early = p < 0.5
            t = S.select(early, p, S.f(0.5) - p)
            add = S.select(early, S.f(0.0), S.f(1.0))
            ma = S.vec4(-a.x, -a.y, -a.z, -a.w)
            mb = S.vec4(-b.x, -b.y, -b.z, -b.w)
            s1 = S.select(early, mb, ma)
            s2 = S.select(early, a, b)
            k = 2.0 * t + add
            v = s1 * k + s2
            r = S.vec4(S.abs(v.x), S.abs(v.y), S.abs(v.z), S.abs(v.w))
            o.set(with3(r, mixf(a.w, b.w, p)))
        with S.if_(S.eq(ty, 2)):
            ma = S.vec4(-a.x, -a.y, -a.z, -a.w)
            v = (a + b) * p + ma
            r = S.vec4(S.abs(v.x), S.abs(v.y), S.abs(v.z), S.abs(v.w))
            o.set(with3(r, mixf(a.w, b.w, p)))
        with S.if_(S.eq(ty, 3) | S.eq(ty, 4)):
            s = col + 1.0
            r = S.vec4(1.0 / s.x, 1.0 / s.y, 1.0 / s.z, 1.0 / s.w)
            m = S.max(S.max(r.x, r.y), S.max(r.z, r.w))
            v23 = p * m
            up = clamp01v(s * v23)
            v24 = m - v23
            dn = clamp01v(s * v24)
            o3 = S.vec4(S.max(a.x - up.x, 0.0) + S.max(b.x - dn.x, 0.0), S.max(a.y - up.y, 0.0) + S.max(b.y - dn.y, 0.0),
                        S.max(a.z - up.z, 0.0) + S.max(b.z - dn.z, 0.0), S.max(a.w - up.w, 0.0) + S.max(b.w - dn.w, 0.0))
            o3 = with3(o3, clamp01(o3.w))
            hi = a + up
            lo = a - up
            def morph(k):
                return S.select(a[k] < b[k], S.min(b[k], hi[k]), S.max(b[k], lo[k]))
            o4 = S.vec4(morph(0), morph(1), morph(2), morph(3))
            o.set(S.select(S.eq(ty, 3), o3, o4))
        with S.if_(S.eq(ty, 5)):
            m = (a.x + a.y + a.z) * f32(1.0 / 3.0)
            o.set(S.select(m > p, a, b))
        with S.if_(S.eq(ty, 6)):
            m = (b.x + b.y + b.z) * f32(1.0 / 3.0)
            o.set(S.select(m >= (1.0 - p), b, a))
        with S.if_(S.eq(ty, 7) | S.eq(ty, 8)):
            g = ((a.x + b.x) + (a.y + b.y) + (a.z + b.z)) * f32(1.0 / 6.0)
            is7 = S.eq(ty, 7)
            mid = S.select(is7, S.splat4(g), S.vec4(col.x * col.w, col.y * col.w, col.z * col.w, col.w))
            early = p < 0.5
            t = S.select(early, S.select(is7, clamp01(p * 2.0), p * 2.0), clamp01((p - 0.5) * 2.0))
            s1 = S.select(early, a, mid)
            s2 = S.select(early, mid, b)
            r = mixf(s1, s2, t)
            o.set(with3(r, S.select(is7, mixf(a.w, b.w, p), r.w)))
        with S.if_(ty > 8):
            third = f32(1.0 / 3.0)
            two_thirds = f32(2.0 * third)
            c1 = p < third
            c2 = p < two_thirds
            t1 = p * 3.0
            t2 = clamp01((p - third) * 3.0)
            t3 = clamp01((p - two_thirds) * 3.0)
            o3 = S.select(c1, mixf(a.w, b.w, t1), mixf(a.w, b.w, p))
            o1 = S.select(c1, a.y, S.select(c2, mixf(a.y, b.y, t2), b.y))
            o0 = S.select(c1, mixf(a.x, b.x, t1), b.x)
            o2 = S.select(c1, a.z, S.select(c2, a.z, mixf(a.z, b.z, t3)))
            o.set(S.vec4(o0, o1, o2, o3))
        finish(S, i, o.get(), P.q8)


# ---------------------------------------------------------------------------------------------------------- Barn Door, Push, Swap (vfxwipes.h, vfxswap.h)
def fetch_pos(S, px, along_x, mode, d, colvec, hw=False):
    """the moved picture's read at the pixel with d added to the coordinate along the axis: mode 0 leaves colvec, 1 reads in0, 2 reads in1; the coordinate is
    held to the picture first (fetch() of the CPU ports) unless hw (Swap: the card's filter at the raw coordinate)"""
    dx = S.select(along_x, d, S.f(0.0))
    dy = S.select(along_x, S.f(0.0), d)
    fx, fy = px.x + dx, px.y + dy
    if not hw:
        fx = clampf(S, fx, 0.0, S.to_f(px.W) - 1.0)
        fy = clampf(S, fy, 0.0, S.to_f(px.H) - 1.0)
    v = S.var('v4', colvec)
    smp = bilinear_hw if hw else bilinear
    with S.if_(S.eq(mode, 1)):
        v.set(smp(S, S.buf('in0'), px.W, px.H, fx, fy))
    with S.if_(S.eq(mode, 2)):
        v.set(smp(S, S.buf('in1'), px.W, px.H, fx, fy))
    return v.get()


@kernel("oclt_barndoor", WH + [('along', 'u')] + Q8 + FS('s0', 's1', 's2', 's3', 'c0', 'c1', 'c2', 'c3'), [PIC_IN, PIC_IN1, PIC_OUT])
def oclt_barndoor(S):
    # BarnDoor::pixel; the host passes the two pictures swapped (in0 = i0) when the CPU swaps them
    P = S.p
    with S.in_range(P.w, P.h):
        px = Px(S)
        along = S.ne(P.along, 0)
        nf = S.select(along, S.to_f(P.w), S.to_f(P.h))
        pos = S.select(along, px.x, px.y)
        s0, s1, s2, s3 = P.s0, P.s1, P.s2, P.s3
        col = colour(S, P)
        v29, v30 = s0, s0 + s1
        v31, v32 = s0 - s2, v30 + s2
        v33 = 0.5 * nf + 0.0001
        v36 = nf - v30
        v37 = v36 + s1
        v38 = v36 - s2
        v39 = v37 + s2
        v42 = S.select(0.0 < s2, S.f(1.0) / s2, S.f(1.0))
        mode = S.var('i', 0)
        v41 = S.var('f', 1.0)
        dpos = S.var('f', 0.0)
        with S.if_(pos < v33):
            with S.if_((pos <= v29) | (pos > v30)):
                with S.if_(pos <= v29):
                    v41.set(S.select(pos > v31, (pos - v31) * v42, S.f(0.0)))
                    dpos.set(s3)
                    mode.set(1)
                with S.else_():
                    v41.set(S.select(pos < v32, (v32 - pos) * v42, S.f(0.0)))
                    mode.set(2)
        with S.else_():
            with S.if_((pos <= v36) | (pos > v37)):
                with S.if_(pos <= v36):
                    v41.set(S.select(pos > v38, (pos - v38) * v42, S.f(0.0)))
                    mode.set(2)
                with S.else_():
                    v41.set(S.select(pos < v39, (v39 - pos) * v42, S.f(0.0)))
                    dpos.set(-s3)
                    mode.set(1)
        v40 = fetch_pos(S, px, along, mode.get(), dpos.get(), col)
        o = mix4(S, v40, col, smooth3(S, v41.get()))
        finish(S, px.i, o, P.q8)


@kernel("oclt_push", WH + [('kernel', 'u')] + Q8 + FS('L', 'pushoff', 's0', 's1', 's2', 's3', 'c0', 'c1', 'c2', 'c3'), [PIC_IN, PIC_IN1, PIC_OUT])
def oclt_push(S):
    # Push::pixel: in0 the outgoing picture, in1 the incoming one
    P = S.p
    with S.in_range(P.w, P.h):
        px = Px(S)
        kn = P.kernel
        along_x = kn > 1
        pos = S.select(along_x, px.x, px.y)
        s0, s1, s2, s3, L = P.s0, P.s1, P.s2, P.s3, P.L
        col = colour(S, P)
        v42 = S.select(0.0 < s2, S.f(1.0) / s2, S.f(0.0))
        keep = S.select(1.0 < P.pushoff, s3, S.f(0.0))
        mode = S.var('i', 0)
        v37 = S.var('f', 0.0)
        dpos = S.var('f', 0.0)
        low = S.eq(kn, 0) | S.eq(kn, 2)
        with S.if_(low):
            v33 = s0 + s1
            v34 = s0 - s2
            v35 = v33 + s2
            with S.if_(pos < s0):
                v37.set(S.select((pos > v34) & (0.0 < s2), (pos - v34) * v42, S.f(0.0)))
                dpos.set(keep)
                mode.set(1)
            with S.else_():
                with S.if_(pos > v33):
                    v37.set(S.select((pos < v35) & (0.0 < s2), (v35 - pos) * v42, S.f(0.0)))
                    dpos.set((s3 - L) - s1)
                    mode.set(2)
                with S.else_():
                    v37.set(S.f(1.0))
        with S.else_():
            v33 = s0
            v32 = s0 - s1
            v34 = v32 - s2
            v35 = v33 + s2
            at_or_after = S.select(S.eq(kn, 3), pos >= v33, pos > v33)
            with S.if_(pos < v32):
                v37.set(S.select((pos > v34) & (0.0 < s2), (pos - v34) * v42, S.f(0.0)))
                dpos.set((L - s3) + s1)
                mode.set(2)
            with S.else_():
                with S.if_(at_or_after):
                    v37.set(S.select((pos < v35) & (0.0 < s2), (v35 - pos) * v42, S.f(0.0)))
                    dpos.set(-keep)
                    mode.set(1)
                with S.else_():
                    v37.set(S.f(1.0))
        v36 = fetch_pos(S, px, along_x, mode.get(), dpos.get(), col)
        o = mix4(S, v36, col, smooth3(S, v37.get()))
        finish(S, px.i, o, P.q8)


@kernel("oclt_swap", WH + [('kernel', 'u')] + Q8 + FS('L', 'p', 's0', 's1', 's2', 's3', 'c0', 'c1', 'c2', 'c3'), [PIC_IN, PIC_IN1, PIC_OUT])
def oclt_swap(S):
    # Swap::pixel (the card's filter at the raw coordinate)
    P = S.p
    with S.in_range(P.w, P.h):
        px = Px(S)
        kn = P.kernel
        along_x = kn > 1
        pos = S.select(along_x, px.x, px.y)
        s0, s1, s2, s3, L, p = P.s0, P.s1, P.s2, P.s3, P.L, P.p
        col = colour(S, P)
        early = 0.5 > p
        first = S.select(early, s3, (-s3) + (L + s1))
        second = S.select(early, -s3, s3 - (L + s1))
        v42 = S.select(0.0 < s2, S.f(1.0) / s2, S.f(0.0))
        mode = S.var('i', 0)
        v34 = S.var('f', 0.0)
        dpos = S.var('f', 0.0)
        low = S.eq(kn, 0) | S.eq(kn, 2)
        with S.if_(low):
            v29, v30 = s0, s0 + s1
            v31, v32 = s0 - s2, v30 + s2
            with S.if_(pos < v29):
                v34.set(S.select((pos > v31) & (0.0 < s2), (pos - v31) * v42, S.f(0.0)))
                dpos.set(first)
                mode.set(1)
            with S.else_():
                with S.if_(pos > v30):
                    v34.set(S.select((pos < v32) & (0.0 < s2), (v32 - pos) * v42, S.f(0.0)))
                    dpos.set(second)
                    mode.set(2)
                with S.else_():
                    v34.set(S.f(1.0))
        with S.else_():
            v30, v29 = s0, s0 - s1
            v31, v32 = v29 - s2, v30 + s2
            with S.if_(pos < v29):
                v34.set(S.select((pos > v31) & (0.0 < s2), (pos - v31) * v42, S.f(0.0)))
                dpos.set(first)
                mode.set(2)
            with S.else_():
                with S.if_(pos > v30):
                    v34.set(S.select((pos < v32) & (0.0 < s2), (v32 - pos) * v42, S.f(0.0)))
                    dpos.set(second)
                    mode.set(1)
                with S.else_():
                    v34.set(S.f(1.0))
        v33 = fetch_pos(S, px, along_x, mode.get(), dpos.get(), col, hw=True)
        o = mix4(S, v33, col, smooth3(S, v34.get()))
        finish(S, px.i, o, P.q8)


# ---------------------------------------------------------------------------------------------------------- rectangle tests, shared by Slide, Spiral, Zoom (vfxwipes.h, vfxspiral.h)
def hypot_f(S, a, b):
    """std::hypot of two floats as the C library gives it: the double root of the sum of the squares, rounded to a float"""
    fa, fb = SF.from_f32(S, a), SF.from_f32(S, b)
    return (fa * fa + fb * fb).sqrt().to_f32()


def r_in_incl(S, r, x, y):
    return (r[0] <= x) & (x <= r[2]) & (r[1] <= y) & (y <= r[3])


def r_out_incl(S, r, x, y):
    return (r[0] >= x) | (x >= r[2]) | (r[1] >= y) | (y >= r[3])


def r_in_strict(S, r, x, y):
    return (r[0] < x) & (x < r[2]) & (r[1] < y) & (y < r[3])


def r_out_strict(S, r, x, y):
    return ~r_in_strict(S, r, x, y)


def edge_distance(S, r, x, y, inside):
    """edgeDistance of Slide / Spiral: from the rectangle's edge, inside or outside (inside: the rectangle test of that header)"""
    d20 = S.min(S.abs(x - r[0]), S.abs(x - r[2]))
    d21 = S.min(S.abs(y - r[1]), S.abs(y - r[3]))
    return S.select(inside(S, r, x, y), S.min(d20, d21),
                    S.select((r[0] < x) & (x < r[2]), d21, S.select((r[1] < y) & (y < r[3]), d20, hypot_f(S, d20, d21))))


def far_out(S, outer, x, y, inner, bw, outside, inside):
    """farOut: outside the outer rectangle, or outside the inner one by more than the border"""
    r = outside(S, outer, x, y)
    inner_out = ~inside(S, inner, x, y)
    return r | (inner_out & (edge_distance(S, inner, x, y, inside) > bw))


def tab_floats(S, base, n):
    t = S.buf('table')
    return [t.load(S.u(base + k)) for k in range(n)]


# The rectangles and vectors of the 2D transition library (Slide, Spiral, Zoom) come in a table of 32 floats: v31 (0), v32 (4), v33 (8), v34 (12) each four, v35 (16, two),
# v36 (18, two), v37 (20, two), v38 (22, two), v39 (24, four: the border colour), v40 (28, four).
def rect_params(S):
    t = tab_floats(S, 0, 32)
    return dict(v31=t[0:4], v32=t[4:8], v33=t[8:12], v34=t[12:16], v35=t[16:18], v36=t[18:20], v37=t[20:22], v38=t[22:24], v39=t[24:28], v40=t[28:32])


def moved_read(S, px, R, hw, spiral_order):
    """the moved picture (in1) read at the pixel's place carried back: Slide (software filter): (p - v36) * v38 + v37; Spiral / Zoom (card's filter): v38 * (p - v36) + v37,
    a NaN made 0; held to the picture"""
    x, y = px.x, px.y
    if spiral_order:
        qx = R['v38'][0] * (x - R['v36'][0]) + R['v37'][0]
        qy = R['v38'][1] * (y - R['v36'][1]) + R['v37'][1]
        qx = S.select(S.eq(qx, qx), qx, S.f(0.0))
        qy = S.select(S.eq(qy, qy), qy, S.f(0.0))
    else:
        qx = (x - R['v36'][0]) * R['v38'][0] + R['v37'][0]
        qy = (y - R['v36'][1]) * R['v38'][1] + R['v37'][1]
    qx = clampf(S, qx, 0.0, S.to_f(px.W) - 1.0)
    qy = clampf(S, qy, 0.0, S.to_f(px.H) - 1.0)
    smp = bilinear_hw if hw else bilinear
    return smp(S, S.buf('in1'), px.W, px.H, qx, qy)


@kernel("oclt_smooth", WH + [('m0', 'i'), ('m1', 'i')], [PIC_IN, PIC_OUT])
def oclt_smooth(S):
    # smooth3x3 (m0, m1 both not 1) and Squeeze::smooth (m0 == 1: 1 2 1 down the column, else m1 == 1: five taps along the row): rows counted from the bottom as the CPU port does
    P = S.p
    with S.in_range(P.w, P.h):
        px = Px(S)
        W, H, x, y = px.W, px.H, px.xi, px.yi
        b = S.buf('in0')
        v8, v7 = S.max(x - 1, S.i(0)), S.max(y - 1, S.i(0))
        v10, v13 = S.min(x + 1, W - 1), S.min(y + 1, H - 1)
        T = lambda xx, yy: texel(S, b, W, H, xx, yy)
        c = T(x, y)
        r3 = S.var('v4', zero4(S))
        with S.if_(S.eq(P.m0, 1)):
            r3.set((T(x, v7) + T(x, v13)) * 4.0 + c * 8.0)
        with S.else_():
            with S.if_(S.eq(P.m1, 1)):
                xx = S.max(x - 2, v8)
                v49 = S.min(x + 2, W - 1)
                r3.set((T(xx, y) + T(v49, y)) * 2.0 + (T(v8, y) + T(v10, y)) * 3.5 + c * 5.0)
            with S.else_():
                r3.set(T(v8, v7) + T(v10, v7) + T(v8, v13) + T(v10, v13) + (T(v8, y) + T(x, v7) + T(v10, y) + T(x, v13)) * 2.0 + c * 4.0)
        S.buf('out').store(px.i, r3.get() * 0.0625)


# ---------------------------------------------------------------------------------------------------------- Slide (wipes::Slide::pixel)
@kernel("oclt_slide", WH + [('variant', 'u')] + Q8, [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def oclt_slide(S):
    P = S.p
    with S.in_range(P.w, P.h):
        px = Px(S)
        R = rect_params(S)
        x, y = px.x, px.y
        col = S.vec4(*R['v39'])
        v31, v32, v33, v34, v40 = R['v31'], R['v32'], R['v33'], R['v34'], R['v40']
        bw, ff = R['v35'][0], R['v35'][1]
        pi0 = texel(S, S.buf('in0'), px.W, px.H, px.xi, px.yi)
        pi1 = texel(S, S.buf('in1'), px.W, px.H, px.xi, px.yi)
        unset = S.vec4(pi1.x, S.f(0.0), pi1.z, pi1.w)
        inc, outc = r_in_incl, r_out_incl
        edge = lambda r: edge_distance(S, r, x, y, inc)
        v = S.var('v4', zero4(S))
        v22 = S.var('f', ff)
        out = S.var('v4', zero4(S))
        with S.if_(S.eq(P.variant, 0)):
            with S.if_(outc(S, v31, x, y)):
                out.set(pi0)
            with S.else_():
                with S.if_(inc(S, v40, x, y)):
                    out.set(moved_read(S, px, R, False, False))
                with S.else_():
                    out.set(unset)
        with S.if_(S.eq(P.variant, 2)):
            v.set(col)
            with S.if_(far_out(S, v32, x, y, v31, bw, outc, inc)):
                v.set(pi0)
            with S.else_():
                with S.if_(outc(S, v40, x, y)):
                    v.set(moved_read(S, px, R, False, False))
            out.set(v.get())
        with S.if_(S.eq(P.variant, 1) | S.eq(P.variant, 3)):
            three = S.eq(P.variant, 3)
            with S.if_(~three):
                with S.if_(outc(S, v31, x, y)):
                    v.set(pi0)
                    with S.if_(inc(S, v34, x, y)):
                        v22.set(edge(v31))
                with S.else_():
                    with S.if_(inc(S, v40, x, y)):
                        v.set(moved_read(S, px, R, False, False))
                        with S.if_(outc(S, v33, x, y)):
                            v22.set(edge(v31))
                    with S.else_():
                        v.set(unset)
            with S.else_():
                v.set(col)
                with S.if_(far_out(S, v32, x, y, v31, bw, outc, inc)):
                    v.set(pi0)
                    with S.if_(inc(S, v34, x, y)):
                        v22.set(edge(v31) - bw)
                with S.else_():
                    with S.if_(inc(S, v40, x, y)):
                        v.set(moved_read(S, px, R, False, False))
                        with S.if_(outc(S, v33, x, y)):
                            v22.set(edge(v31))
            m = clampf(S, v22.get(), 0.0, ff) / ff
            m = smooth3(S, m)
            out.set(mix4(S, col, v.get(), m))
        finish(S, px.i, out.get(), P.q8)


# ---------------------------------------------------------------------------------------------------------- Spiral and Zoom (wipes::Spiral::pixel, wipes::Zoom::pixel)
def spiral_zoom_body(S, zoom):
    """in0 the picture outside the rectangle, in1 the moving picture (smoothed first by the host when the CPU port does); the 32 floats of rect_params in the table"""
    P = S.p
    with S.in_range(P.w, P.h):
        px = Px(S)
        R = rect_params(S)
        x, y = px.x, px.y
        col = S.vec4(*R['v39'])
        v31, v32, v33, v34, v40 = R['v31'], R['v32'], R['v33'], R['v34'], R['v40']
        bw, ff = R['v35'][0], R['v35'][1]
        first = texel(S, S.buf('in0'), px.W, px.H, px.xi, px.yi)
        ins, outs = r_in_strict, r_out_strict
        edge31 = lambda: edge_distance(S, v31, x, y, ins)
        rect_a = v32 if zoom else v31
        v = S.var('v4', col)
        v22 = S.var('f', ff)
        out = S.var('v4', zero4(S))
        var = P.variant
        with S.if_(S.eq(var, 0)):
            with S.if_(outs(S, rect_a, x, y)):
                out.set(first)
            with S.else_():
                out.set(moved_read(S, px, R, True, True))
        with S.if_(S.eq(var, 2)):
            with S.if_(far_out(S, v32, x, y, v31, bw, outs, ins)):
                v.set(first)
            with S.else_():
                with S.if_(ins(S, v40, x, y) if zoom else outs(S, v31, x, y)):
                    v.set(moved_read(S, px, R, True, True))
            out.set(v.get())
        with S.if_(S.eq(var, 1)):
            with S.if_(outs(S, rect_a, x, y)):
                v.set(first)
                with S.if_(ins(S, v34, x, y)):
                    v22.set(edge31())
            with S.else_():
                def mv():
                    v.set(moved_read(S, px, R, True, True))
                    with S.if_(outs(S, v33, x, y)):
                        v22.set(edge31())
                if zoom:
                    with S.if_(ins(S, v40, x, y)):
                        mv()
                else:
                    mv()
            m = clampf(S, v22.get(), 0.0, ff) / ff
            out.set(mix4(S, col, v.get(), smooth3(S, m)))
        with S.if_(S.eq(var, 3)):
            with S.if_(far_out(S, v32, x, y, v31, bw, outs, ins)):
                v.set(first)
                with S.if_(ins(S, v34, x, y)):
                    v22.set(edge31() - bw)
            with S.else_():
                with S.if_(ins(S, v40 if zoom else v31, x, y)):
                    v.set(moved_read(S, px, R, True, True))
                    with S.if_(outs(S, v33, x, y)):
                        v22.set(edge31())
            m = clampf(S, v22.get(), 0.0, ff) / ff
            out.set(mix4(S, col, v.get(), smooth3(S, m)))
        finish(S, px.i, out.get(), P.q8)


@kernel("oclt_spiral", WH + [('variant', 'u')] + Q8, [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def oclt_spiral(S):
    spiral_zoom_body(S, False)


@kernel("oclt_zoom", WH + [('variant', 'u')] + Q8, [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def oclt_zoom(S):
    spiral_zoom_body(S, True)


# ---------------------------------------------------------------------------------------------------------- Squeeze (wipes::Squeeze::pixel)
def fetch_to(S, px, along_y, mode, newpos, colvec):
    """Squeeze's fetch: the coordinate along the axis replaced by newpos, held to the picture, the card's filter; mode 0 leaves colvec, 1 reads in0, 2 reads in1"""
    fx = S.select(along_y, px.x, newpos)
    fy = S.select(along_y, newpos, px.y)
    fx = clampf(S, fx, 0.0, S.to_f(px.W) - 1.0)
    fy = clampf(S, fy, 0.0, S.to_f(px.H) - 1.0)
    v = S.var('v4', colvec)
    with S.if_(S.eq(mode, 1)):
        v.set(bilinear_hw(S, S.buf('in0'), px.W, px.H, fx, fy))
    with S.if_(S.eq(mode, 2)):
        v.set(bilinear_hw(S, S.buf('in1'), px.W, px.H, fx, fy))
    return v.get()


@kernel("oclt_squeeze", WH + [('kernel', 'u')] + Q8 + FS('flag', 's0', 's1', 's2', 's3', 'c0', 'c1', 'c2', 'c3'), [PIC_IN, PIC_IN1, PIC_OUT])
def oclt_squeeze(S):
    # in0 the outgoing picture (smoothed first by the host when the CPU port does), in1 the incoming one (likewise)
    P = S.p
    with S.in_range(P.w, P.h):
        px = Px(S)
        kn = P.kernel
        s0, s1, s2, s3, fl = P.s0, P.s1, P.s2, P.s3, P.flag
        col = colour(S, P)
        rc = S.select(0.0 < s2, S.f(1.0) / s2, S.f(0.0))
        along_y = S.eq(kn, 0) | S.eq(kn, 1) | S.eq(kn, 5)
        mid = kn > 3
        len_ = S.select(along_y, S.to_f(px.H), S.to_f(px.W))
        pos = S.select(along_y, px.y, px.x)
        sq = 1.0 < fl
        mode = S.var('i', 0)
        v37 = S.var('f', 1.0)
        npos = S.var('f', 0.0)

        with S.if_(~mid):
            flipped = S.eq(kn, 1) | S.eq(kn, 3)
            v30 = S.select(flipped, s0 - s1, s0)
            v31 = S.select(flipped, s0, s0 + s1)
            v32 = S.select(flipped, v30, s0) - s2
            v33 = v31 + s2
            strict = S.eq(kn, 3)
            low = S.select(strict, pos < v30, pos <= v30)
            high = S.select(strict, pos >= v31, pos > v31)
            k02 = S.eq(kn, 0) | S.eq(kn, 2)
            with S.if_(low | high):
                with S.if_(low):
                    v37.set(S.select((pos > v32) & (0.0 < s2), (pos - v32) * rc, S.f(0.0)))
                    c30 = clampf(S, v30, 0.0001, len_)
                    new_k0 = S.select(sq, pos * len_ / c30, pos)
                    new_k2 = pos * S.select(sq, len_ / c30, S.f(1.0))
                    new_k13 = pos * (len_ / c30)
                    npos.set(S.select(k02, S.select(S.eq(kn, 0), new_k0, new_k2), new_k13))
                    mode.set(S.select(k02, S.i(1), S.i(2)))
                with S.else_():
                    v37.set(S.select((pos < v33) & (0.0 < s2), (v33 - pos) * rc, S.f(0.0)))
                    c31 = clampf(S, v31, 0.0, len_ - 0.0001)
                    q = (pos - c31) * len_ / (len_ - c31)
                    npos.set(S.select(k02, q, S.select(sq, q, pos)))
                    mode.set(S.select(k02, S.i(2), S.i(1)))
        with S.else_():
            v30, v31, v32 = s0, s0 + s1, s0 - s2
            v33 = v31 + s2
            v39 = 0.5 * len_ + 0.0001
            v40 = len_ - v31
            v41 = v40 + s1
            v42 = v40 - s2
            v43 = v41 + s2
            strict = along_y
            with S.if_(pos < v39):
                with S.if_((pos <= v30) | (pos > v31)):
                    with S.if_(S.select(strict, pos < v30, pos <= v30)):
                        v37.set(S.select((pos > v32) & (0.0 < s2), (pos - v32) * rc, S.f(0.0)))
                        c = clampf(S, v30, 0.0001, v39)
                        npos.set(pos * S.select(sq, v39 / c, S.f(1.0)))
                        mode.set(1)
                    with S.else_():
                        v37.set(S.select((pos < v33) & (0.0 < s2), (v33 - pos) * rc, S.f(0.0)))
                        c = clampf(S, v31, 0.0, v39 - 0.0001)
                        npos.set((pos - c) * v39 / (v39 - c))
                        mode.set(2)
            with S.else_():
                with S.if_((pos <= v40) | (pos > v41)):
                    with S.if_(S.select(strict, pos < v40, pos <= v40)):
                        v37.set(S.select((pos > v42) & (0.0 < s2), (pos - v42) * rc, S.f(0.0)))
                        c = clampf(S, v40, v39 + 0.0001, len_)
                        npos.set(v39 + (pos - v39) * v39 / (c - v39))
                        mode.set(2)
                    with S.else_():
                        v37.set(S.select((pos < v43) & (0.0 < s2), (v43 - pos) * rc, S.f(0.0)))
                        c = clampf(S, v41, v39, len_ - 0.0001)
                        npos.set(S.select(sq, v39 + (pos - c) * v39 / (len_ - c), pos))
                        mode.set(1)
        v36 = fetch_to(S, px, along_y, mode.get(), npos.get(), col)
        o = mix4(S, v36, col, smooth3(S, v37.get()))
        finish(S, px.i, o, P.q8)


# ---------------------------------------------------------------------------------------------------------- Split (wipes::Split::pixel)
@kernel("oclt_split", WH + [('variant', 'u'), ('mode', 'u'), ('only_first', 'u')] + Q8, [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def oclt_split(S):
    # in0 the picture cut into quadrants, in1 the one behind (smoothed first by the host when the CPU port does). The table: v29 (2), v30 (4, the border colour), v31 .. v38 (4 each).
    # The CPU port counts the pixel's row from the top here (y = gy), and reads the pictures with the row turned back: the pixel itself.
    P = S.p
    with S.in_range(P.w, P.h):
        px = Px(S)
        W, H = px.W, px.H
        t = tab_floats(S, 0, 38)
        v29, v30 = t[0:2], t[2:6]
        Q = [t[6 + 4 * k: 10 + 4 * k] for k in range(8)]       # v31 .. v38
        v31, v32, v33, v34, v35, v36, v37, v38 = Q
        col = S.vec4(*v30)
        x = px.x
        y = S.to_f(S.to_i(S.gy))
        var = P.variant
        first_px = S.buf('in0').load(px.i)
        behind_px = S.buf('in1').load(px.i)
        v52 = S.max(S.f(1.0), v29[0])
        shifted = var > 1
        v8 = S.select(shifted, v31[2] + v52, v31[2])
        v10 = S.select(shifted, v32[0] - v52, v32[0])
        v7 = S.select(shifted, v31[3] + v52, v31[3])
        v13 = S.select(shifted, v33[1] - v52, v33[1])

        def cross(a, b, c, d):
            return ((a < x) & (b > x)) | ((c < y) & (d > y))

        def dist4(a, b, c, d):
            v17 = S.min(S.abs(x - a), S.abs(x - b))
            v18 = S.min(S.abs(y - c), S.abs(y - d))
            return S.select((a < x) & (x < b), v17, S.select((c < y) & (y < d), v18, S.min(v17, v18)))

        def dist3(a, b, c, d):
            v17 = S.min(S.abs(x - a), S.abs(x - b))
            v18 = S.min(S.abs(y - c), S.abs(y - d))
            inside = (a <= x) & (x <= b) & (c <= y) & (y <= d)
            corner = S.min(S.min(hypot_f(S, a - x, c - y), hypot_f(S, b - x, d - y)), S.min(hypot_f(S, a - x, d - y), hypot_f(S, b - x, c - y)))
            return S.select(inside, corner, S.select((a < x) & (x < b), v17, S.select((c < y) & (y < d), v18, S.min(v17, v18))))

        # the quadrant picture
        qx_ok = ((x <= v8) & (y <= v7))
        q2 = ((x >= v10) & (y <= v7))
        q3 = ((x <= v8) & (y >= v13))
        def pick(a, b, c, d):
            return [S.select(qx_ok, a[k], S.select(q2, b[k], S.select(q3, c[k], d[k]))) for k in range(4)]
        dd = pick(v31, v32, v33, v34)
        ss = pick(v35, v36, v37, v38)
        v57, v58 = (dd[0] + dd[2]) * 0.5, (dd[1] + dd[3]) * 0.5
        v59, v60 = (ss[0] + ss[2]) * 0.5, (ss[1] + ss[3]) * 0.5
        mode = P.mode
        v61, v63 = ss[2] - ss[0], ss[3] - ss[1]
        v62, v64 = S.max(dd[2] - dd[0], S.f(0.0001)), S.max(dd[3] - dd[1], S.f(0.0001))
        v65, ky = v61 / v62, v63 / v64
        xx = S.select(S.eq(mode, 0), (x - v57) + v59, v65 * (x - v57) + v59)
        yy = S.select(S.eq(mode, 0), (y - v58) + v60, ky * (y - v58) + v60)
        xx = S.select(S.eq(xx, xx), xx, S.f(0.0))
        yy = S.select(S.eq(yy, yy), yy, S.f(0.0))
        xx = clampf(S, xx, 0.0, S.to_f(W) - 1.0)
        yy = clampf(S, yy, 0.0, S.to_f(H - 1))
        yy = S.to_f(H - 1) - yy
        quad = S.var('v4', first_px)
        with S.if_(S.ne(mode, 1)):
            quad.set(bilinear_hw(S, S.buf('in0'), W, H, xx, yy))
        qv = quad.get()

        v43, v44 = (v8 + v10) * 0.5, (v7 + v13) * 0.5
        v45 = S.max(S.f(1.0), v29[1])
        inner = S.select(shifted, v52, v45)           # the width the first set of rectangles grows by
        v46 = clampf(S, v8 + inner, v8, v43)
        u1 = clampf(S, v10 - inner, v43, v10)
        v47 = clampf(S, v7 + inner, v7, v44)
        v1 = clampf(S, v13 - inner, v44, v13)
        v48 = S.min(v8 - inner, v8)
        U1 = S.max(v10 + inner, v10)
        v49 = S.min(v7 - inner, v7)
        V1 = S.max(v13 + inner, v13)
        out = S.var('v4', zero4(S))

        def finish_mix(v, d):
            m = S.min(d, S.f(1.0))
            return mix4(S, col, v, smooth3(S, m))

        with S.if_(S.ne(P.only_first, 0)):
            out.set(first_px)
        with S.else_():
            with S.if_(S.eq(var, 0)):
                out.set(S.select(cross(v8, v10, v7, v13), behind_px, qv))
            with S.if_(S.eq(var, 1)):
                in_cross = cross(v8, v10, v7, v13)
                v50 = S.select(in_cross, S.select(~cross(v46, u1, v47, v1), dist3(v8, v10, v7, v13), v45),
                               S.select(cross(v48, U1, v49, V1), dist4(v8, v10, v7, v13), v45))
                v = S.select(in_cross, behind_px, qv)
                out.set(finish_mix(v, v50 / v45))
            with S.if_(S.eq(var, 2)):
                in_cross = cross(v8, v10, v7, v13)
                keep = cross(v46, u1, v47, v1) | (dist3(v48, U1, v49, V1) >= v52 + v52)
                out.set(S.select(in_cross, S.select(keep, behind_px, col), S.select(cross(v48, U1, v49, V1), col, qv)))
            with S.if_(S.eq(var, 3)):
                v53 = clampf(S, v46 + v45, v46, v43)
                r1 = clampf(S, u1 - v45, v43, u1)
                s0 = clampf(S, v47 + v45, v47, v44)
                s1 = clampf(S, v1 - v45, v44, v1)
                v54 = S.min(v48 - v45, v48)
                R1 = S.max(U1 + v45, U1)
                v55 = S.min(v49 - v45, v49)
                S1 = S.max(V1 + v45, V1)
                in_cross = cross(v8, v10, v7, v13)
                with S.if_(cross(v53, r1, s0, s1)):
                    out.set(behind_px)
                with S.else_():
                    with S.if_(~cross(v54, R1, v55, S1)):
                        out.set(qv)
                    with S.else_():
                        v19 = S.select(in_cross, dist3(v48, U1, v49, V1) - v52, dist4(v8, v10, v7, v13))
                        v = S.select(in_cross, behind_px, qv)
                        mixed = mix4(S, col, v, smooth3(S, S.min(S.f(1.0), (v19 - v52) / v45)))
                        out.set(S.select(v19 < v52, col, mixed))
        finish(S, px.i, out.get(), P.q8)


# ---------------------------------------------------------------------------------------------------------- Star Wipe (wipes::StarWipe::pixel)
def atan_f(S, v):
    """(float)std::atan((double)v)"""
    return sf_atan2(S, SF.from_f32(S, v), SF.const(S, 1.0)).to_f32()


@kernel("oclt_starwipe", WH + [('reverse', 'u'), ('sawtooth', 'u'), ('mirror_x', 'u'), ('mirror_y', 'u')] + Q8 +
        FS('cen0', 'cen1', 'v2', 'v3', 'v4', 'v5', 'v49', 'v52', 'maxdist', 'p', 'border', 'feather', 'c0', 'c1', 'c2', 'c3'), [PIC_IN, PIC_IN1, PIC_OUT])
def oclt_starwipe(S):
    # StarWipe::pixel with the per-frame numbers (the centre, the angle's cosine and sine, the arm angle v2, v5, the cycle scale) worked out by the host exactly as the CPU port does
    P = S.p
    with S.in_range(P.w, P.h):
        px = Px(S)
        W, H = px.W, px.H
        v2, v3, v4, v5 = P.v2, P.v3, P.v4, P.v5
        col = colour(S, P)
        xi = px.xi
        yi = px.yi
        xi = S.select(S.ne(P.mirror_x, 0) & (xi > W / 2), W - xi, xi)
        yi = S.select(S.ne(P.mirror_y, 0) & (yi < H / 2), H - yi, yi)
        x, y = S.to_f(xi), S.to_f(yi)
        # dist(..., recip = true)
        at_centre = (S.abs(x - P.cen0) < 0.000001) & (S.abs(y - P.cen1) < 0.000001)
        v7 = x - P.cen0
        v8 = y - P.cen1
        v9 = v7 * v3 + v8 * v4
        v10 = (-v4 * v7) + v8 * v3
        a9 = S.abs(v9) < 0.000001
        a10 = S.abs(v10) < 0.000001
        v11 = S.var('f', 0.0)
        with S.if_(a9):
            v11.set(S.select(v10 > 0.0, S.f(1.5707963), S.select(v10 < 0.0, S.f(4.71238898), S.f(0.0))))
        with S.else_():
            with S.if_(a10):
                v11.set(S.select(v9 > 0.0, S.f(0.0), S.select(v9 < 0.0, S.f(3.1415926), S.f(0.0))))
            with S.else_():
                at = atan_f(S, v10 / v9)
                neg9 = v9 < 0.0
                v11.set(S.select(neg9 & (v10 > 0.0), 3.1415926 + at,
                                 S.select(neg9 & (v10 < 0.0), 3.1415926 + at, S.select((v9 > 0.0) & (v10 < 0.0), 6.2831853 + at, at))))
        v11 = v11.get()
        v13 = v2 * 0.5
        inside = (v11 > v13) & (v11 < (6.2831853 - v13))
        q = (v11 + v13) * (S.f(1.0) / v2)
        v12 = S.select(inside, v2 * S.to_f(S.to_i(q)), S.f(0.0))
        v14, v15 = S.cos(v12), S.sin(v12)
        w7 = v9 * v14 + v10 * v15
        w8 = (-v15 * v9) + v10 * v14
        w7 = S.select(w7 < 0.0, -w7, w7)
        w8 = S.select(w8 < 0.0, -w8, w8)
        v16 = w7 * (-v5) + w8
        v16 = S.max(S.f(1.0), v16)
        v16 = S.select(at_centre, S.f(1.0), v16)
        # the pixel
        v16 = v16 * P.v52
        v16 = S.select(v16 > 1.0, v16 - S.floor(v16), v16)
        saw = S.ne(P.sawtooth, 0)
        v16 = S.select(saw, v16, S.select(v16 < 0.5, v16 * 2.0, (1.0 - v16) * 2.0))
        v16 = S.select(S.ne(P.reverse, 0), 1.0 - v16, v16)
        v16 = clampf(S, v16, 0.0, 1.0)
        v16 = S.select(P.maxdist < 1.0, S.f(1.0), v16)
        v48 = clampf(S, P.p, 0.0, 1.0)
        v53 = P.border < 0.000001
        v54 = P.feather < 0.000001
        v55, v56 = 0.125 * P.border, 0.125 * P.feather
        v49 = P.v49
        a = S.buf('in0').load(px.i)
        b = S.buf('in1').load(px.i)
        o = S.var('v4', zero4(S))
        with S.if_(v53 & v54):
            o.set(S.select(v16 < v48, b, a))
        with S.if_(~v53 & v54):
            v57 = (-v49) * v55 + v48
            v58 = v49 * v55 + v48
            o.set(S.select(v16 < v57, b, S.select(v16 > v58, a, col)))
        with S.if_(v53 & ~v54):
            v57 = (-v49) * v56 + v48
            v58 = v49 * v56 + v48
            v59 = S.f(1.0) / (v58 - v57)
            v60 = clampf(S, (v16 - v57) * v59, 0.0, 1.0)
            o.set(S.select(v16 < v57, b, S.select(v16 > v58, a, mix4(S, b, a, v60))))
        with S.if_(~v53 & ~v54):
            v61 = (-v49) * v55 + v48
            v62 = v49 * v55 + v48
            v63 = (-v49) * v56 + v61
            v64 = v49 * v56 + v62
            v65 = S.f(1.0) / (v61 - v63)
            v66 = S.f(1.0) / (v64 - v62)
            m1 = clampf(S, (v16 - v63) * v65, 0.0, 1.0)
            m2 = clampf(S, (v16 - v62) * v66, 0.0, 1.0)
            o.set(S.select(v16 < v63, b, S.select(v16 > v64, a, S.select((v63 <= v16) & (v16 <= v61), mix4(S, b, col, m1),
                                                                          S.select((v61 < v16) & (v16 < v62), col, mix4(S, col, a, m2))))))
        finish(S, px.i, o.get(), P.q8)


# ---------------------------------------------------------------------------------------------------------- Iris (wipes::Iris::pixel)
class IrisK:
    """the Iris functions of vfxiris.h on kernel values; P is the push block"""
    def __init__(self, S, P, x, y):
        self.S, self.P = S, P
        self.x, self.y = x, y                        # the pixel (ints, y from the bottom)
        self.shape = P.shape
        self.nx, self.ny = P.nx, P.ny
        self.cw, self.ch = P.cellw, P.cellh
        self.ox, self.oy = P.ox, P.oy

    def fa(self, v, *ints):
        """v + int + int ... in floats, left to right"""
        S = self.S
        for i in ints:
            v = v + S.to_f(i)
        return v

    def rel(self, cx, cy):
        S, P = self.S, self.P
        u = S.to_f(self.x) - cx
        w = S.to_f(self.y) - cy
        return u * P.asp0, w * P.asp1

    def line_product(self, a1, a2, a3, a4, u, w):
        p0 = a3 - a1
        p1 = u - a1
        q0 = a2 - w
        q1 = a4 - a2
        return self.S.abs(p0 * q0 + p1 * q1)

    def ellipse(self, cx, cy):
        S, P = self.S, self.P
        u, w = self.rel(cx, cy)
        u = S.select(S.eq(self.shape, 1), u * P.ratio, u)
        w = S.select(S.eq(self.shape, 2), w * P.ratio, w)
        return hypot_f(S, u, w)

    def boxy(self, cx, cy):
        S, P = self.S, self.P
        u, w = self.rel(cx, cy)
        u, w = S.abs(u), S.abs(w)
        return S.select(S.eq(self.shape, 3), S.max(w, P.ratio * u), S.select(S.eq(self.shape, 4), u + w, S.max(u, w)))

    def triangle(self, cx, cy):
        S = self.S
        u, w = self.rel(cx, cy)
        sh = self.shape
        c = lambda v: S.f(v)
        # each shape: (s factor, [(condition, a1..a4)], default a1..a4)
        s6 = c(0.57735027) * u
        s7 = c(1.73205081) * u
        s8 = c(0.57735027) * u
        s9 = c(1.73205081) * u
        k57, m57, k33, m33, k66, m66 = c(0.57735027), c(-0.57735027), c(0.33333333), c(-0.33333333), c(0.66666667), c(-0.66666667)
        z = c(0.0)

        def chain(cases, default):
            res = list(default)
            for cond, vals in reversed(cases):
                res = [S.select(cond, vals[k], res[k]) for k in range(4)]
            return res
        t6 = chain([((u <= 0.0) & (w > s6), (z, k66, m57, m33)), ((w <= s6) & (w < -s6), (m57, m33, k57, m33))], (k57, m33, z, k66))
        t7 = chain([((w > -s7) & (w >= 0.0), (k66, z, m33, k57)), ((w <= -s7) & (w > s7), (m33, k57, m33, m57))], (m33, m57, k66, z))
        t8 = chain([((u >= 0.0) & (w < s8), (z, m66, k57, k33)), ((u < 0.0) & (w <= -s8), (m57, k33, z, m66))], (k57, k33, m57, k33))
        t9 = chain([((w > 0.0) & (w >= s9), (k33, k57, m66, z)), ((w <= 0.0) & (w < -s9), (k33, m57, m66, z))], (k33, m57, k33, k57))
        a = [S.select(S.eq(sh, 6), t6[k], S.select(S.eq(sh, 7), t7[k], S.select(S.eq(sh, 8), t8[k], t9[k]))) for k in range(4)]
        return self.line_product(a[0], a[1], a[2], a[3], u, w)

    def arrow(self, kind, cx, cy):
        S = self.S
        u, w = self.rel(cx, cy)
        c = lambda v: S.f(v)
        z, p4, m4, p8, m8 = c(0.0), c(0.4), c(-0.4), c(0.8), c(-0.8)
        ls, ss = c(1.11803399), c(0.55901699)

        def chain(cases, default):
            res = list(default)
            for cond, vals in reversed(cases):
                res = [S.select(cond, vals[k], res[k]) for k in range(5)]
            return res
        t10 = chain([((u >= 0.0) & (w < -u), (z, m4, p8, m8, ls)), ((u > 0.0) & (w >= -u), (p8, m8, z, p8, ss)), ((u <= 0.0) & (w > u), (z, p8, m8, m8, ss))], (m8, m8, z, m4, ls))
        t11 = chain([((w >= 0.0) & (w > -u), (p8, z, m8, p8, ss)), ((w > 0.0) & (w <= -u), (m8, p8, m4, z, ls)), ((w <= 0.0) & (w > u), (m4, z, m8, m8, ls))], (m8, m8, p8, z, ss))
        t12 = chain([((u > 0.0) & (w >= u), (p8, p8, z, p4, ls)), ((u <= 0.0) & (w > -u), (z, p4, m8, p8, ls)), ((u < 0.0) & (w <= -u), (m8, p8, z, m8, ss))], (z, m8, p8, p8, ss))
        t13 = chain([((w >= 0.0) & (w < u), (p4, z, p8, p8, ls)), ((w > 0.0) & (w >= u), (p8, p8, m8, z, ss)), ((w <= 0.0) & (w < -u), (m8, z, p8, m8, ss))], (p8, m8, p4, z, ls))
        a = [S.select(S.eq(kind, 10), t10[k], S.select(S.eq(kind, 11), t11[k], S.select(S.eq(kind, 12), t12[k], t13[k]))) for k in range(5)]
        return self.line_product(a[0], a[1], a[2], a[3], u, w) * a[4]

    def cell_index(self, pos, origin, size, count):
        S = self.S
        fp = S.to_f(pos)
        whole = S.to_i(fp - origin)
        i = S.to_i(S.to_f(whole) * (S.f(1.0) / S.to_f(size)))
        return S.select(fp < origin, S.i(-1), S.min(i, count - 1))

    @staticmethod
    def mul_sat(S, a, b):
        return S.min(S.max(a * b, S.i(-1073741823)), S.i(1073741823))

    def nearest_of_cells(self, measure):
        """shapes 0 .. 8"""
        S = self.S
        ox, oy, cw, ch, nx, ny = self.ox, self.oy, self.cw, self.ch, self.nx, self.ny
        ix = self.cell_index(self.x, ox, cw, nx)
        iy = self.cell_index(self.y, oy, ch, ny)
        r = S.var('f', 0.0)
        fa = self.fa
        by = self.mul_sat(S, iy, ch)
        bx = self.mul_sat(S, ix, cw)

        def take(cond, cx, cy):
            with S.if_(cond):
                r.set(S.min(measure(cx, cy), r.get()))
        mx, my = S.eq(ix, -1), S.eq(iy, -1)
        with S.if_(mx & my):
            r.set(measure(ox, oy))
        with S.else_():
            with S.if_(mx):
                r.set(measure(ox, fa(oy, by)))
                take(iy + 1 < ny, ox, fa(oy, by, ch))
            with S.else_():
                with S.if_(my):
                    r.set(measure(fa(ox, bx), oy))
                    take(ix + 1 < nx, fa(ox, bx, cw), oy)
                with S.else_():
                    r.set(measure(fa(ox, bx), fa(oy, by)))
                    take((ix + 1 < nx) & (iy + 1 < ny), fa(ox, bx, cw), fa(oy, by, ch))
                    take(ix + 1 < nx, fa(ox, bx, cw), fa(oy, by))
                    take(iy + 1 < ny, fa(ox, bx), fa(oy, by, ch))
        return r.get()

    def arrow_cells(self):
        """shapes 10 .. 13"""
        S = self.S
        ox, oy, cw, ch, nx, ny, s = self.ox, self.oy, self.cw, self.ch, self.nx, self.ny, self.shape
        ix = self.cell_index(self.x, ox, cw, nx)
        iy = self.cell_index(self.y, oy, ch, ny)
        fa = self.fa
        r = S.var('f', 1.0)
        x0, y0 = ox, oy

        def at(kind, cx, cy):
            return self.arrow(kind, cx, cy)

        def take(cond, kind, cx, cy):
            if cond is True:
                r.set(S.min(at(kind, cx, cy), r.get()))
            else:
                with S.if_(cond):
                    r.set(S.min(at(kind, cx, cy), r.get()))
        sk = lambda n: S.eq(s, n)
        i10, i11, i12, i13 = S.i(10), S.i(11), S.i(12), S.i(13)
        S10, S11, S12, S13 = sk(10), sk(11), sk(12), sk(13)
        L = lambda a, b: a < b
        one_lt = lambda n: S.i(1) < n        # 1 < n
        mx, my = S.eq(ix, -1), S.eq(iy, -1)
        by = self.mul_sat(S, iy, ch)
        bx = self.mul_sat(S, ix, cw)
        sI = S.to_i(s)
        with S.if_(mx & my):
            r.set(at(sI, x0, y0))
            with S.if_(S10 | S11):
                take(one_lt(nx) & one_lt(ny), sI, fa(x0, cw), fa(y0, ch))
                take(one_lt(nx), sI, fa(x0, cw), y0)
                take(one_lt(ny), sI, x0, fa(y0, ch))
        with S.else_():
            with S.if_(mx):
                r.set(at(sI, x0, fa(y0, by)))
                take(iy + 1 < ny, sI, x0, fa(y0, by, ch))
                with S.if_(S11):
                    with S.if_(one_lt(nx)):
                        take(True, i11, fa(x0, cw), fa(y0, by))
                        take(S.i(0) < iy, i11, fa(x0, cw), fa(y0, by) - S.to_f(ch))
                        take(iy + 1 < ny, i11, fa(x0, cw), fa(y0, by, ch))
                        take(iy + 2 < ny, i11, fa(x0, cw), fa(y0, by, ch, ch))
                    take(S.i(0) < iy, i11, x0, fa(y0, by) - S.to_f(ch))
                    take(iy + 2 < ny, i11, fa(x0, cw), fa(y0, by, ch, ch))
                with S.if_(S10):
                    take(one_lt(nx) & (iy + 1 < ny), i10, fa(x0, cw), fa(y0, by, ch))
                    take(one_lt(nx) & (iy + 2 < ny), i10, fa(x0, cw), fa(y0, by, ch, ch))
                    take(iy + 2 < ny, i10, x0, fa(y0, by, ch, ch))
                with S.if_(S12):
                    take(one_lt(nx) & (S.i(0) < iy), i12, fa(x0, cw), fa(y0, by) - S.to_f(ch))
                    take(S.i(0) < iy, i12, x0, fa(y0, by) - S.to_f(ch))
                    take(one_lt(nx), i12, fa(x0, cw), fa(y0, by))
            with S.else_():
                with S.if_(my):
                    r.set(at(sI, fa(x0, bx), y0))
                    take(ix + 1 < nx, sI, fa(x0, bx, cw), y0)
                    with S.if_(S10):
                        with S.if_(one_lt(ny)):
                            take(True, i10, fa(x0, bx), fa(y0, ch))
                            take(ix + 1 < nx, i10, fa(x0, bx, cw), fa(y0, ch))
                            take(ix + 2 < nx, i10, fa(x0, bx, cw, cw), fa(y0, ch))
                            take(S.i(0) < ix, i10, fa(x0, bx) - S.to_f(cw), fa(y0, ch))
                        take(S.i(0) < ix, i10, fa(x0, bx) - S.to_f(cw), y0)
                        take(ix + 2 < nx, i10, fa(x0, bx, cw, cw), y0)
                    with S.if_(S13):
                        take(one_lt(ny) & (S.i(0) < ix), i13, fa(x0, bx) - S.to_f(cw), fa(y0, ch))
                        take(one_lt(ny), i13, fa(x0, bx), fa(y0, ch))
                        take(S.i(0) < ix, i13, fa(x0, bx) - S.to_f(cw), y0)
                    with S.if_(S11):
                        take(one_lt(ny) & (ix + 2 < nx), i11, fa(x0, bx, cw, cw), fa(y0, ch))
                        take(one_lt(ny) & (ix + 1 < nx), i11, fa(x0, bx, cw), fa(y0, ch))
                        take(ix + 2 < nx, i11, fa(x0, bx, cw, cw), y0)
                with S.else_():
                    r.set(at(sI, fa(x0, bx), fa(y0, by)))
                    take((ix + 1 < nx) & (iy + 1 < ny), sI, fa(x0, bx, cw), fa(y0, by, ch))
                    take(ix + 1 < nx, sI, fa(x0, bx, cw), fa(y0, by))
                    take(iy + 1 < ny, sI, fa(x0, bx), fa(y0, by, ch))
                    with S.if_(S11):
                        with S.if_(ix + 1 < nx):
                            take(iy + 2 < ny, i11, fa(x0, bx, cw), fa(y0, by, ch, ch))
                            take(S.i(0) < iy, i11, fa(x0, bx, cw), fa(y0, by) - S.to_f(ch))
                        with S.if_(ix + 2 < nx):
                            take(True, i11, fa(x0, bx, cw, cw), fa(y0, by))
                            take(S.i(0) < iy, i11, fa(x0, bx, cw, cw), fa(y0, by) - S.to_f(ch))
                            take(iy + 1 < ny, i11, fa(x0, bx, cw, cw), fa(y0, by, ch))
                            take(iy + 2 < ny, i11, fa(x0, bx, cw, cw), fa(y0, by, ch, ch))
                    with S.if_(S13):
                        take(iy + 2 < ny, i13, fa(x0, bx), fa(y0, by, ch, ch))
                        take(S.i(0) < iy, i13, fa(x0, bx), fa(y0, by) - S.to_f(ch))
                        with S.if_(S.i(0) < ix):
                            take(True, i13, fa(x0, bx) - S.to_f(cw), fa(y0, by))
                            take(iy + 1 < ny, i13, fa(x0, bx) - S.to_f(cw), fa(y0, by, ch))
                            take(iy + 2 < ny, i13, fa(x0, bx) - S.to_f(cw), fa(y0, by, ch, ch))
                            take(S.i(0) < iy, i13, fa(x0, bx) - S.to_f(cw), fa(y0, by) - S.to_f(ch))
                    with S.if_(S10):
                        with S.if_(iy + 1 < ny):
                            take(ix + 2 < nx, i10, fa(x0, bx, cw, cw), fa(y0, by, ch))
                            take(S.i(0) < ix, i10, fa(x0, bx) - S.to_f(cw), fa(y0, by, ch))
                        with S.if_(iy + 2 < ny):
                            take(True, i10, fa(x0, bx), fa(y0, by, ch, ch))
                            take(ix + 2 < nx, i10, fa(x0, bx, cw, cw), fa(y0, by, ch, ch))
                            take(ix + 1 < nx, i10, fa(x0, bx, cw), fa(y0, by, ch, ch))
                            take(S.i(0) < ix, i10, fa(x0, bx) - S.to_f(cw), fa(y0, by, ch, ch))
                    with S.if_(S12):
                        take(ix + 2 < nx, i12, fa(x0, bx, cw, cw), fa(y0, by))
                        take(S.i(0) < ix, i12, fa(x0, bx) - S.to_f(cw), fa(y0, by))
                        with S.if_(S.i(0) < iy):
                            take(True, i12, fa(x0, bx), fa(y0, by) - S.to_f(ch))
                            take(ix + 1 < nx, i12, fa(x0, bx, cw), fa(y0, by) - S.to_f(ch))
                            take(ix + 2 < nx, i12, fa(x0, bx, cw, cw), fa(y0, by) - S.to_f(ch))
                            take(S.i(0) < ix, i12, fa(x0, bx) - S.to_f(cw), fa(y0, by) - S.to_f(ch))
        return r.get()


@kernel("oclt_iris", WH + [('shape', 'i'), ('nx', 'i'), ('ny', 'i'), ('cellw', 'i'), ('cellh', 'i')] + Q8 +
        FS('ratio', 'asp0', 'asp1', 'ox', 'oy', 'maxdist', 'feather', 'border', 'thr', 'c0', 'c1', 'c2', 'c3'), [PIC_IN, PIC_IN1, PIC_OUT])
def oclt_iris(S):
    # Iris::pixel: in0 the picture outside the iris (first), in1 the one inside (second)
    P = S.p
    with S.in_range(P.w, P.h):
        px = Px(S)
        K = IrisK(S, P, px.xi, px.yi)
        shape = P.shape
        a = S.buf('in0').load(px.i)
        b = S.buf('in1').load(px.i)
        col = colour(S, P)
        d = S.var('f', 0.0)

        def measure(cx, cy):
            m = S.var('f', 0.0)
            with S.if_(shape <= 2):
                m.set(K.ellipse(cx, cy))
            with S.if_((shape > 2) & (shape <= 5)):
                m.set(K.boxy(cx, cy))
            with S.if_(shape > 5):
                m.set(K.triangle(cx, cy))
            return m.get()
        with S.if_(shape <= 9):
            d.set(K.nearest_of_cells(measure))
        with S.else_():
            d.set(K.arrow_cells())
        t = d.get() * (S.f(1.0) / P.maxdist)
        fw = S.select(P.feather < 0.01, 0.01 * S.to_f(P.nx) * S.to_f(P.ny), P.feather)
        fw = fw * 0.05
        bw = P.border * 0.05
        no_feather = fw < 0.000001
        no_border = bw < 0.000001
        th = P.thr
        o = S.var('v4', zero4(S))
        with S.if_(no_border & no_feather):
            o.set(S.select(t < th, b, a))
        with S.if_(~no_border & no_feather):
            lo = clampf(S, th - bw, 0.0, th)
            hi = clampf(S, th + bw, th, 1.0)
            o.set(S.select(t < lo, b, S.select(t > hi, a, col)))
        with S.if_(no_border & ~no_feather):
            lo = clampf(S, th - fw, 0.0, th)
            hi = clampf(S, th + fw, th, 1.0)
            inv = S.f(1.0) / (hi - lo)
            m = clampf(S, (t - lo) * inv, 0.0, 0.999)
            o.set(S.select(t < lo, b, S.select(t > hi, a, mix4(S, b, a, m))))
        with S.if_(~no_border & ~no_feather):
            in0 = clampf(S, th - bw, 0.0, th)
            in1 = clampf(S, th + bw, th, 1.0)
            o0 = clampf(S, in0 - fw, 0.0, in0)
            o1 = clampf(S, in1 + fw, in1, 1.0)
            r0 = S.f(1.0) / (in0 - o0)
            r1 = S.f(1.0) / (o1 - in1)
            m1 = clampf(S, (t - o0) * r0, 0.0, 0.999)
            m2 = clampf(S, (t - in1) * r1, 0.0, 0.999)
            o.set(S.select(t < o0, b, S.select(t > o1, a, S.select((o0 <= t) & (t <= in0), mix4(S, b, col, m1),
                                                                    S.select((in0 < t) & (t < in1), col, mix4(S, col, a, m2))))))
        finish(S, px.i, o.get(), P.q8)


# ---------------------------------------------------------------------------------------------------------- the recursive Gaussian (vfxgauss.h passColumns), Flash and Cross Effect's blur
def cut_vec(S, v, q8):
    """the 12-bit store of each channel when q8"""
    cut = S.vec4(store12(S, v.x), store12(S, v.y), store12(S, v.z), store12(S, v.w))
    return S.select(S.ne(q8, 0), cut, v)


@kernel("oclt_gauss", WH + [('horiz', 'u'), ('sumin', 'u')] + Q8, [PIC_IN, PIC_OUT, TABLE])
def oclt_gauss(S):
    # passColumns(): thread (line, pass): pass 0 the causal run, 1 the anti-causal run, along the picture's columns (horiz 0: y from the bottom up) or rows (horiz 1: x).
    # in0 is a picture, or (sumin) two planes of one picture whose sum is the input; the out buffer holds the causal run's picture and then the anti-causal run's.
    # The table: n (5), d (5), m (5), bn (5), bm (5) floats. A result is cut with the 12-bit store when q8. The recursion is the float form without a fused multiply-add.
    P = S.p
    horiz = S.ne(P.horiz, 0)
    lines = S.select(horiz, P.h, P.w)
    n_len = S.select(horiz, P.w, P.h)
    with S.if_(S.gx < lines):
        with S.if_(S.gy < 2):
            T = S.buf('table')
            cf = lambda k: T.load(S.u(k))
            causal = S.eq(S.gy, 0)
            base = S.select(causal, S.u(0), S.u(10))                    # n or m
            ee = S.select(causal, S.u(15), S.u(20))                     # bn or bm
            nn = [S.select(causal, cf(k), cf(10 + k)) for k in range(5)]
            eb = [S.select(causal, cf(15 + k), cf(20 + k)) for k in range(5)]
            d = [cf(5 + k) for k in range(5)]
            plane = P.w * P.h
            src = S.buf('in0')

            def idx(j):
                r_v = S.select(causal, P.h - 1 - j, j)                  # vertical: the row of position j
                x_h = S.select(causal, j, P.w - 1 - j)
                return S.select(horiz, (P.h - 1 - S.gx) * P.w + x_h, r_v * P.w + S.gx)

            def load(j):
                i = idx(j)
                v = src.load(i)
                return S.select(S.ne(P.sumin, 0), v + src.load(i + plane), v)

            def put(j, v):
                S.buf('out').store(idx(j) + S.gy * plane, cut_vec(S, v, P.q8))
            x0 = load(S.u(0))
            v15 = S.select(causal, (-eb[4]) * x0, (nn[4] - eb[4]) * x0)
            v14 = (nn[3] - eb[3]) * x0 + v15
            v13 = (nn[2] - eb[2]) * x0 + v14
            v12 = (nn[1] - eb[1]) * x0 + v13
            v11 = (nn[0] - eb[0]) * x0 + v12
            v22 = S.var('v4', v11)
            put(S.u(0), v11)
            with S.if_(n_len >= 2):
                x1 = load(S.u(1))
                z = zero4(S)
                V16, V17, V18, V19 = S.var('v4', z), S.var('v4', v11 * d[0]), S.var('v4', z), S.var('v4', z)
                V11, V12, V13, V14, V15 = S.var('v4', x1 * nn[0] + v12), S.var('v4', x1 * nn[1] + v13), S.var('v4', x1 * nn[2] + v14), S.var('v4', x1 * nn[3] + v15), \
                    S.var('v4', S.select(causal, z, x1 * nn[4]))
                v22.set(V11.get() - V16.get())
                put(S.u(1), v22.get())
                with S.for_(2, n_len) as i:
                    xi = load(i)
                    w22 = v22.get()
                    o16 = w22 * d[1] + V17.get()
                    o17 = w22 * d[2] + V18.get()
                    o18 = w22 * d[3] + V19.get()
                    o19 = w22 * d[4]
                    V16.set(o16); V17.set(o17); V18.set(o18); V19.set(o19)
                    o11 = xi * nn[0] + V12.get()
                    o12 = xi * nn[1] + V13.get()
                    o13 = xi * nn[2] + V14.get()
                    o14 = xi * nn[3] + V15.get()
                    V11.set(o11); V12.set(o12); V13.set(o13); V14.set(o14)
                    V15.set(S.select(causal, z, xi * nn[4]))
                    v22.set(o11 - o16)
                    put(i, v22.get())


@kernel("oclt_flash", WH + [('blur', 'u')] + Q8 + FS('a2', 's', 't0', 't1', 't2'), [PIC_IN, PIC_IN1, PIC_OUT])
def oclt_flash(S):
    # flashk::pixel: in0 the shown picture, in1 the blur's two planes (r1 then r2) when blur, else the lum is the picture itself
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        src = S.buf('in0').load(i)
        plane = P.w * P.h
        r12 = S.buf('in1')
        lum = S.select(S.ne(P.blur, 0), r12.load(i) + r12.load(i + plane), src)
        v50 = S.vec4(S.min(S.max(lum.x, 0.0), 1.0), S.min(S.max(lum.y, 0.0), 1.0), S.min(S.max(lum.z, 0.0), 1.0), S.min(S.max(lum.w, 0.0), 1.0))
        l = v50.x * 0.299
        l = l + v50.y * 0.587
        l = l + v50.z * 0.114
        l = l + v50.w * 0.0
        l = l * P.s
        v56 = S.vec4(P.t0 * l + P.a2, P.t1 * l + P.a2, P.t2 * l + P.a2, 1.0 * l + P.a2)
        o = v56 + src
        o = S.vec4(o.x, o.y, o.z, S.min(S.f(1.0), v56.w + src.w))
        finish(S, i, o, P.q8)


# ---------------------------------------------------------------------------------------------------------- Cross Effect (crossfx::render)
@kernel("oclt_cross_mix", WH + [('aplanes', 'u'), ('bplanes', 'u')] + Q8 + FS('t'), [PIC_IN, PIC_IN1, PIC_OUT])
def oclt_cross_mix(S):
    # kernel 818: mix4(a, b, t); a / b are, when aplanes / bplanes, the sum of the two planes of a blurred picture (cut with the store when q8: blurPicture's last step)
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        plane = P.w * P.h
        A, B = S.buf('in0'), S.buf('in1')
        a = S.select(S.ne(P.aplanes, 0), cut_vec(S, A.load(i) + A.load(i + plane), P.q8), A.load(i))
        b = S.select(S.ne(P.bplanes, 0), cut_vec(S, B.load(i) + B.load(i + plane), P.q8), B.load(i))
        finish(S, i, mix4(S, a, b, P.t), P.q8)


@kernel("oclt_cross_zoom", WH + [('apply', 'u')] + Q8 + FS('fade', 'sa', 'sb', 'cs0', 'cs1', 'cd0', 'cd1'), [PIC_IN, PIC_IN1, PIC_OUT])
def oclt_cross_zoom(S):
    P = S.p
    with S.in_range(P.w, P.h):
        px = Px(S)
        W, H = px.W, px.H

        def safe(v):
            v = S.select(S.eq(v, v), v, S.f(0.0))
            return S.min(S.max(v, -1.0e6), 1.0e6)
        va = S.var('v4', texel(S, S.buf('in0'), W, H, px.xi, px.yi))
        vb = S.var('v4', texel(S, S.buf('in1'), W, H, px.xi, px.yi))
        with S.if_(S.ne(P.apply, 1)):
            qx = safe((px.x - P.cs0) * P.sa + P.cs0)
            qy = safe((px.y - P.cs1) * P.sa + P.cs1)
            va.set(bilinear_hw(S, S.buf('in0'), W, H, qx, qy))
        with S.if_(S.ne(P.apply, 0)):
            qx = safe((px.x - P.cd0) * P.sb + P.cd0)
            qy = safe((px.y - P.cd1) * P.sb + P.cd1)
            vb.set(bilinear_hw(S, S.buf('in1'), W, H, qx, qy))
        finish(S, px.i, mix4(S, va.get(), vb.get(), P.fade), P.q8)


@kernel("oclt_pixelate", WH + [('horiz', 'u'), ('n', 'u')] + Q8, [PIC_IN, PIC_OUT])
def oclt_pixelate(S):
    # pixelateColumns (horiz 0: thread = column, run from the picture's top row down) / pixelateRows (horiz 1: thread = row, run left to right): block means with the block
    # lengths from a float step; n is ny / nx. The result is cut with the store when q8.
    P = S.p
    horiz = S.ne(P.horiz, 0)
    lines = S.select(horiz, P.h, P.w)
    n_len = S.select(horiz, P.w, P.h)
    with S.if_(S.gx < lines):
        src, dst = S.buf('in0'), S.buf('out')

        def idx(j):
            return S.select(horiz, S.gx * P.w + j, j * P.w + S.gx)
        with S.if_(S.eq(P.n, n_len)):
            with S.for_(0, n_len) as j:
                dst.store(idx(j), cut_vec(S, src.load(idx(j)), P.q8))
        with S.else_():
            nf = S.to_f(P.n)
            lf = S.to_f(n_len)
            v87 = lf / nf                                            # columns: (float)H / (float)ny
            v95 = v87                                                # rows: (float)W / (float)nx
            first_c = S.to_i(v87 * nf - v87 * S.to_f(P.n - 1))
            first_r = S.to_i(v95)
            blk = S.var('i', S.select(horiz, first_r, first_c))
            acc = S.var('v4', zero4(S))
            cnt = S.var('i', 0)
            done = S.var('i', 0)
            with S.for_(0, n_len) as j:
                cnt.set(cnt.get() + 1)
                acc.set(acc.get() + src.load(idx(j)))
                last = S.eq(j, n_len - 1)
                with S.if_(S.eq(cnt.get(), blk.get()) | last):
                    avg = acc.get() / S.to_f(cnt.get())
                    cv = cut_vec(S, avg, P.q8)
                    with S.for_(0, S.to_u(cnt.get())) as k:
                        dst.store(idx(j - k), cv)
                    acc.set(zero4(S))
                    cnt.set(0)
                    done.set(done.get() + 1)
                    d1 = done.get()
                    jf = S.to_f(j)
                    nxt_c = S.select(S.ne(S.to_i(P.n), d1), S.to_i(S.fma(v87, S.to_f(d1 + 1), -jf)), S.to_i(v87))
                    nxt_r = S.select(S.ne(S.to_i(P.n), d1), S.to_i(v95 * S.to_f(d1 + 1) - jf), S.to_i(n_len) - S.to_i(j))
                    blk.set(S.select(horiz, nxt_r, nxt_c))


# ---------------------------------------------------------------------------------------------------------- Page Peel, Page Loop, Page Roll (vfxpage.h)
# in0 is the page (the picture that is drawn on the layers), in1 what lies under it; the table holds the setup floats of the transition (PeelSetup / LoopSetup / RollSetup, in the order
# the C++ planner writes them). The kernels' pictures are zero outside and any input that is not finite reads as 0, as the CPU port makes them.
def sf_asin_f(S, x):
    """asinf of this machine's glibc (2.39: __asinf_finite, the float polynomial without a division; read from the library's code, checked against it for every float in 0..1): the very
    same float operations, so the result is the library's, bit for bit"""
    cf = lambda bits: S.bits_to_f(S.u(bits))
    a, b, c, d, e = cf(0x3d2cb694), cf(0x3cc6141e), cf(0x3d3a3f25), cf(0x3d9980f2), cf(0x3e2aaae4)
    pio2, lo, lop, pio4 = cf(0x3fc90fdb), cf(0xb33bbd2e), cf(0x333bbd2e), cf(0x3f490fdb)
    u = S.f_to_bits(x)
    ix = u & 0x7fffffff
    poly = lambda t: (((((a * t + b) * t + c) * t + d) * t + e) * t)
    # |x| < 0.5
    t0 = x * x
    small = S.select(ix < 0x32000000, x, poly(t0) * x + x)
    # |x| >= 0.5
    ax = S.abs(x)
    t = (1.0 - ax) * 0.5
    p = poly(t)
    s = S.sqrt(t)
    w1 = p * s + s
    w1 = w1 + w1
    r1 = pio2 - (lop + w1)
    w = S.bits_to_f(S.f_to_bits(s) & 0xfffff000)
    cc = (t - w * w) / (s + w)
    pp = (s + s) * p - (lo - (cc + cc))
    q = pio4 - (w + w)
    r2 = pio4 - (pp - q)
    r = S.select(ix > 0x3f799999, r1, r2)
    big = S.select((u >> 31) > 0, -r, r)
    nan = S.bits_to_f(S.u(0x7fc00000))
    one = pio2 * x + x * lo
    return S.select(ix > 0x3f800000, nan, S.select(S.eq(ix, 0x3f800000), one, S.select(ix < 0x3f000000, small, big)))


def cos_f(S, y):
    """cosf of this machine's glibc (2.39, the cosf the library picks on a CPU with FMA; read from the library's code and checked against it for every float up to 2.3, which covers the
    arcs these transitions work with: the double polynomial of sincosf, the quadrant for pi/4 .. 3 pi/4 folded as the library does). The library fuses its multiplies with adds; here each
    double operation rounds on its own, which changes the double by about 1e-16 relative and the float it is rounded to in one case in a billion."""
    D = lambda v: SF.const(S, struct.unpack('<d', struct.pack('<Q', v))[0])
    c0, c1, s1, c2 = D(0x3ff0000000000000), D(0xbfdffffffd0c621c), D(0xbfc555545995a603), D(0x3fa55553e1068f19)
    s2, c3, s3, c4 = D(0x3f81107605230bc4), D(0xbf56c087e89a359d), D(0xbf2994eb3774cf24), D(0x3ef99343027bf8c3)
    hpi, hinv = D(0x3ff921fb54442d18), D(0x41645f306dc9c883)
    ay = S.abs(y)
    e = (S.f_to_bits(ay) >> 20) & 0x7ff
    X = SF.from_f32(S, ay)

    def cospoly(r):
        x2 = r * r
        x4 = x2 * x2
        c01 = c1 * x2 + c0
        c34 = c4 * x2 + c3
        x6 = x2 * x4
        c = x4 * c2 + c01
        return c34 * x6 + c
    small = cospoly(X).to_f32()
    n1 = X * hinv >= SF.const(S, 8388608.0)
    r = SF.sel(S, n1, X - hpi, X)
    x2 = r * r
    xs = r.neg()
    x3 = x2 * xs
    x7 = x2 * x3
    odd = ((s3 * x2 + s2) * x7 + (x3 * s1 + xs)).to_f32()
    even = cospoly(r).to_f32()
    big = S.select(n1, odd, even)
    nan = S.bits_to_f(S.u(0x7fc00000))
    return S.select(e <= 0x3f3, S.select(e <= 0x397, S.f(1.0), small), S.select(e <= 0x42e, big, nan))


def cvt_int(S, v):
    """the card's float to int: a NaN is 0, out of range saturates (only the range inside +-2e9 matters here)"""
    c = S.min(S.max(v, S.f(-2.0e9)), S.f(2.0e9))
    return S.select(S.isnan(v), S.i(0), S.to_i(c))


def page_texel(S, buf, W, H, x, y):
    t = texel(S, buf, W, H, x, y)
    z = S.f(0.0)
    return S.vec4(S.select(S.isfinite(t.x), t.x, z), S.select(S.isfinite(t.y), t.y, z), S.select(S.isfinite(t.z), t.z, z), S.select(S.isfinite(t.w), t.w, z))


def page_bilinear(S, buf, W, H, x, y):
    flx, fly = S.floor(x), S.floor(y)
    fx, fy = x - flx, y - fly
    below = S.f(0.99999994)
    fx = S.select(fx > below, below, fx)
    fy = S.select(fy > below, below, fy)
    ix, iy = cvt_int(S, flx), cvt_int(S, fly)
    v7, v8 = 1.0 - fy, 1.0 - fx
    w00, w10, w01, w11 = v7 * v8, v7 * fx, fy * v8, fy * fx
    a = page_texel(S, buf, W, H, ix, iy)
    b = page_texel(S, buf, W, H, ix + 1, iy)
    c = page_texel(S, buf, W, H, ix, iy + 1)
    d = page_texel(S, buf, W, H, ix + 1, iy + 1)
    return ((a * w00 + b * w10) + c * w01) + d * w11


class PageK:
    """the frame's geometry and drawLayer"""
    def __init__(self, S, px, sn, cs):
        self.S, self.px, self.sn, self.cs = S, px, sn, cs
        self.w2 = S.to_f(px.W) * 0.5
        self.h2 = S.to_f(px.H) * 0.5

    def ab(self, sx, sy):
        yy = (sy - self.h2) * 1.0
        xx = sx - self.w2
        return xx * self.cs + yy * self.sn, xx * (-self.sn) + yy * self.cs

    def draw(self, enable, u, v, alpha, light, lcol, col):
        S, px = self.S, self.px
        W, H = px.W, px.H
        xb = u * self.cs + v * (-self.sn)
        yb = u * self.sn + v * self.cs
        fy = yb * 1.0 + self.h2
        fx = xb + self.w2
        ix, iy = cvt_int(S, fx), cvt_int(S, fy)
        inside = enable & (ix >= 0) & (ix < W) & (iy >= 0) & (iy < H)
        with S.if_(inside):
            fx2 = S.min(S.max(fx, S.f(0.5)), S.to_f(W) - 0.5)
            fy2 = S.min(S.max(fy, S.f(0.5)), S.to_f(H) - 0.5)
            s = page_bilinear(S, S.buf('in0'), W, H, fx2, fy2)
            s = S.select(light > 0.0, s + (lcol - s) * light, s)
            col.set(col.get() + (s - col.get()) * alpha)


def page_frame(S, px, g, q8, sample):
    """the 2 x 2 samples of the pixel; sample(a, b, col) draws one into the variable col"""
    W = px.W
    base = page_texel(S, S.buf('in1'), px.W, px.H, px.xi, px.yi)
    acc = S.var('v4', zero4(S))
    with S.for_(0, 4) as s:
        ox = S.select(S.eq(s & 1, 0), S.f(0.333), S.f(0.666))
        oy = S.select(s < 2, S.f(0.333), S.f(0.666))
        col = S.var('v4', base)
        a, b = g.ab(px.x + ox, px.y + oy)
        sample(a, b, col)
        acc.set(acc.get() + col.get())
    finish(S, px.i, acc.get() * 0.25, q8)


PAGE_PUSH = WH + Q8 + FS('sn', 'cs')
KPI = 3.14159265359


@kernel("oclt_page_peel", PAGE_PUSH, [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def oclt_page_peel(S):
    P = S.p
    with S.in_range(P.w, P.h):
        px = Px(S)
        g = PageK(S, px, P.sn, P.cs)
        (slide, flat_end, c48, c4c, R, iR, edge, persp, opacity, inv, light_base, light_scale) = tab_floats(S, 0, 12)
        lcol = S.vec4(*tab_floats(S, 12, 4))
        black = zero4(S)

        def sample(a, b, col):
            a1 = a - slide
            en = a1 <= edge
            flat = a1 < flat_end
            th = sf_asin_f(S, (a1 - flat_end) * iR)
            cs = cos_f(S, th)
            # layer 0
            al = S.select(flat, S.f(1.0), opacity + (inv * (KPI - th)) * 0.318309886184)
            u0 = S.select(flat, a1, R * th + flat_end)
            sc = persp * (cs - 1.0) + 1.0
            u0 = S.select(flat, u0, u0 * sc)
            v0 = S.select(flat, b, b * sc)
            g.draw(en, u0, v0, al, S.f(0.0), lcol, col)
            # layer 1
            lt = S.select(flat, S.f(0.0), light_base - S.abs(th - 0.785398163397) * light_scale)
            al1 = S.select(flat, opacity, opacity + (inv * th) * 0.318309886184)
            u1 = S.select(flat, (-a1) + c48, (-R) * th + c4c)
            sc1 = S.select(flat, persp * -2.0 + 1.0, persp * (-(1.0 + cs)) + 1.0)
            g.draw(en, u1 * sc1, b * sc1, al1, lt, lcol, col)
        page_frame(S, px, g, P.q8, sample)


@kernel("oclt_page_loop", PAGE_PUSH, [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def oclt_page_loop(S):
    P = S.p
    with S.in_range(P.w, P.h):
        px = Px(S)
        g = PageK(S, px, P.sn, P.cs)
        (p, x0, Rs, circ, ramp, shift, lo, hi, alpha_flat, alpha_end, persp, light_scale) = tab_floats(S, 0, 12)
        lcol = S.vec4(*tab_floats(S, 12, 4))
        R = S.select(Rs < 0.001, S.f(0.001), Rs)
        iR = S.f(1.0) / R
        iramp = S.f(1.0) / ramp

        def mixf(a, b, t):
            return a + (b - a) * t

        def sample(a, b, col):
            # layer 0: flat and on the ramp
            on_ramp = (S.f(1.0) > p) & (a >= x0)
            th = sf_asin_f(S, (a - x0) * iramp)
            gm = 1.0 - (1.0 - cos_f(S, th)) * persp
            u = S.select(on_ramp, gm * ((x0 + circ) + (th * ramp - shift)), S.select(a < x0, a - shift, (a + circ) - shift))
            v = S.select(on_ramp, gm * b, b)
            al = S.select(on_ramp, mixf(alpha_flat, alpha_end, th * 0.636619772368), alpha_flat)
            lt = S.select(on_ramp, light_scale * (1.0 - S.abs((th - 0.785398163397) * 1.273239544735)), S.f(0.0))
            g.draw(S.b(True), u, v, al, lt, lcol, col)
            # layers 1 and 2: the loop
            en = (a > lo) & (a < hi)
            neg = a < x0
            d = S.select(neg, x0 - a, a - x0)
            th2 = sf_asin_f(S, d * iR)
            cs = cos_f(S, th2)
            light_amount = light_scale * (1.0 - S.abs((th2 - 0.785398163397) * 1.273239544735))
            al2 = mixf(alpha_flat, alpha_end, th2 * 0.636619772368)
            gm1 = (cs - 1.0) * persp + 1.0
            w1 = S.select(neg, 6.283185307180 - th2, th2)
            g.draw(en, gm1 * (x0 + (w1 * R - shift)), gm1 * b, al2, S.select(neg, S.f(0.0), light_amount), lcol, col)
            w2 = S.select(neg, KPI + th2, KPI - th2)
            gm2 = (-(1.0 + cs)) * persp + 1.0
            g.draw(en, gm2 * (x0 + (w2 * R - shift)), gm2 * b, al2, S.select(neg, light_amount, S.f(0.0)), lcol, col)
        page_frame(S, px, g, P.q8, sample)


@kernel("oclt_page_roll", PAGE_PUSH, [PIC_IN, PIC_IN1, PIC_OUT, TABLE])
def oclt_page_roll(S):
    P = S.p
    with S.in_range(P.w, P.h):
        px = Px(S)
        g = PageK(S, px, P.sn, P.cs)
        (slide, R, R2, iR, iR2, D, c7c, edge, persp, opacity, inv, light_base, light_scale) = tab_floats(S, 0, 13)
        lcol = S.vec4(*tab_floats(S, 13, 4))
        black = S.vec4(S.f(0.0), S.f(0.0), S.f(0.0), S.f(1.0))
        K318 = 0.3183098862

        def sample(a, b, col):
            a1 = a - slide
            # layer 0: the page and the first turn
            en0 = a1 < edge
            flat = a1 < D
            th = sf_asin_f(S, (a1 - D) * iR)
            al = S.select(flat, S.f(1.0), inv * ((KPI - th) * K318) + opacity)
            sc = (cos_f(S, th) - 1.0) * persp + 1.0
            u = S.select(flat, a1, (R * th + D) * sc)
            v = S.select(flat, b, b * sc)
            g.draw(en0, u, v, al, S.f(0.0), black, col)
            # layer 1: the roll's back
            en1 = (a1 >= D - R) & (a1 < D + R2)
            neg = a1 < D
            d = S.select(neg, D - a1, a1 - D)
            t1 = sf_asin_f(S, d * S.select(neg, iR, iR2))
            w = S.select(neg, KPI - t1, t1 + 6.2831853072)
            base = S.select(neg, c7c, D)
            sc1 = (cos_f(S, t1) - 1.0) * persp + 1.0
            u1 = R * w + base
            g.draw(en1, u1 * sc1, b * sc1, inv * ((KPI - t1) * K318) + opacity, S.f(0.0), black, col)
            # layers 2 and 3
            for layer in (2, 3):
                Rl = R2 if layer == 2 else R
                iRl = iR2 if layer == 2 else iR
                hi = (D + R2) if layer == 2 else edge
                en = (a1 >= D - Rl) & (a1 < hi)
                th3 = sf_asin_f(S, d * iRl)
                lc = S.select(neg, black, lcol)
                v80 = S.select(neg, (1.5707963268 - th3) * 0.5, S.abs(th3 - 0.7853981634))
                v81 = S.select(neg, th3, -th3)
                lt = light_scale * (-v80) + light_base
                if layer == 2:
                    u3 = Rl * (f32(3.0 * f32(3.1415926536)) + v81) + D
                else:
                    u3 = Rl * v81 + c7c
                sc3 = persp * (-(1.0 + cos_f(S, th3))) + 1.0
                g.draw(en, u3 * sc3, b * sc3, inv * (th3 * K318) + opacity, lt, lc, col)
        page_frame(S, px, g, P.q8, sample)
