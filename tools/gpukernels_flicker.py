"""Kernels of Flicker Control (builtins_analysis_flicker.cpp, vfxflicker.h): the temporal blend over up to 6 frames, and Addition, with the Show mask option. tools/gpukernels.py loads this file.
The frames other than the one being drawn are fetched by the CPU and arrive in the table (W x H x 4 floats each); the C++ side is src/ofx/vkfft.cpp."""
import struct
from gpukernels import kernel, PIC_IN, PIC_OUT, TABLE, WH
from sf64 import SF
from gpukernels_blur import store12


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def in_level(S, v, q):
    """inLevel(v, q): q ? (float)(int)(min(1, max(0, v)) * 255 + 0.5) / 255 : v (the pictures of an 8-bit frame are its bytes / 255)"""
    c = S.min(S.max(v, S.f(0.0)), S.f(1.0))
    lv = S.to_f(S.to_i(c * 255.0 + 0.5))
    return S.select(q, lv / 255.0, v)


def stdmax(S, a, b):
    """std::max(a, b): a < b ? b : a"""
    return S.select(a < b, b, a)


def stdmin(S, a, b):
    """std::min(a, b): b < a ? b : a"""
    return S.select(b < a, b, a)


def tone_of(S, type_, m):
    """vfxfl::toneOf: type 0 highlights, 1 midtones (in doubles), 2 shadows, else 1"""
    md = SF.from_f32(S, m) - 0.5
    t1 = (SF.const(S, 1.0) - ((SF.const(S, 4.0) * md) * md)).to_f32()
    t2 = S.f(1.0) - m * m
    t0 = S.f(1.0) - (S.f(1.0) - m) * (S.f(1.0) - m)
    return S.select(S.eq(type_, 2), t2, S.select(S.eq(type_, 1), t1, S.select(S.eq(type_, 0), t0, S.f(1.0))))


def max3(S, p):
    return stdmax(S, p.x, stdmax(S, p.y, p.z))


def mask_pixel(S, a, r):
    """vfxfl::maskPixel: 5 times the largest channel difference as grey, alpha of a"""
    k = S.f(f32(1.0 / f32(f32(0.2) - 0.0)))
    d = S.f(0.0)
    for ca, cr in ((a.x, r.x), (a.y, r.y), (a.z, r.z)):
        df = S.select(ca > cr, ca - cr, cr - ca)
        d = stdmax(S, d, df)
    v = stdmax(S, S.f(0.0), stdmin(S, S.f(1.0), k * (d - 0.0)))
    return S.vec4(v, v, v, a.w)


def store_all(S, q8, v):
    return S.vec4(S.select(q8, store12(S, v.x), v.x), S.select(q8, store12(S, v.y), v.y), S.select(q8, store12(S, v.z), v.z), S.select(q8, store12(S, v.w), v.w))


def _lv4(S, p, q):
    return S.vec4(in_level(S, p.x, q), in_level(S, p.y, q), in_level(S, p.z, q), in_level(S, p.w, q))


@kernel("fl_blend", WH + [('type', 'u'), ('cen', 'u'), ('nf', 'u'), ('selfk', 'u'), ('before', 'u'), ('flags', 'u')] + [('m%d' % k, 'f') for k in range(9)] + [('w%d' % k, 'f') for k in range(6)],
        [PIC_IN, PIC_OUT, TABLE])
def fl_blend(S):
    """temporalBlend(): the six slots (a slot past the frames the window found, or whose motion position fell outside the frame, has no weight and the frame's own pixel), then
    vfxfl::blendPixel, the mask if asked, the 8-bit store. selfk: the slot that is the frame itself (the card's picture); the other slots are the table's frames in order."""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        q8 = S.ne(P.flags & 4, 0)
        motion = S.ne(P.flags & 1, 0)
        showmask = S.ne(P.flags & 2, 0)
        tab = S.buf('table')
        own = _lv4(S, S.buf('in0').load(i), q8)
        wh = P.w * P.h
        def tload(valid, frame, idx):
            e = S.select(valid, (frame * wh + idx) * 4, S.u(0))
            return S.vec4(tab.load(e), tab.load(e + 1), tab.load(e + 2), tab.load(e + 3))
        mp = [getattr(P, 'm%d' % k) for k in range(9)]
        xf, yf = S.to_f(S.gx), S.to_f(P.h - 1 - S.gy)
        den = mp[6] * xf + mp[7] * yf + 1.0
        xp = (mp[0] * xf + mp[1] * yf + mp[2]) / den
        yp = (mp[3] * xf + mp[4] * yf + mp[5]) / den
        outside = (xp < 0.0) | (xp > S.to_f(P.w - 1)) | (yp < 0.0) | (yp > S.to_f(P.h - 1))
        cx = S.to_u(S.min(S.max(S.to_i(S.floor(S.select(outside, S.f(0.0), xp))), S.i(0)), S.to_i(P.w) - 1))
        cy = S.to_u(S.min(S.max(S.to_i(S.floor(S.select(outside, S.f(0.0), yp))), S.i(0)), S.to_i(P.h) - 1))
        slot_px, slot_w = [], []
        mi = (P.h - 1 - cy) * P.w + cx
        for k in range(6):
            used = P.nf > k
            isself = S.eq(P.selfk, k)
            # the table frame of the slot: the slots before the frame itself come first
            tf = S.select(P.selfk < k, S.u(k) - 1, S.u(k))
            intab = used & ~isself
            base = S.select(isself, own, _lv4(S, tload(intab, tf, i), q8))
            moved = S.select(isself, _lv4(S, S.buf('in0').load(S.select(isself, mi, S.u(0))), q8), _lv4(S, tload(intab, tf, mi), q8))
            wk = getattr(P, 'w%d' % k)
            mv = motion & S.ne(P.before, k)
            px = S.select(used, S.select(mv & ~outside, moved, base), own)
            w = S.select(used, S.select(mv & outside, S.f(0.0), wk), S.f(0.0))
            slot_px.append(px)
            slot_w.append(w)
        t = P.type
        if True:
            # Shadow: the plain weighted sum
            sh = slot_px[0] * slot_w[0]
            for k in range(1, 6):
                sh = sh + slot_px[k] * slot_w[k]
            wk = []
            for k in range(6):
                m = max3(S, slot_px[k])
                wk.append(slot_w[k] * S.select(S.ne(P.cen, k), tone_of(S, t, m), S.f(1.0)))
            sm = wk[0]
            for k in range(1, 6):
                sm = sm + wk[k]
            v = slot_px[0] * wk[0]
            for k in range(1, 6):
                v = v + slot_px[k] * wk[k]
            bl = S.select(S.eq(sm, 0.0), v / 1.0, v / sm)
            r = S.select(S.eq(t, 2), sh, bl)
        cenpx = slot_px[0]
        for k in range(1, 6):
            cenpx = S.select(S.eq(P.cen, k), slot_px[k], cenpx)
        r = S.select(showmask, mask_pixel(S, cenpx, r), r)
        S.buf('out').store(i, store_all(S, q8, r))


@kernel("fl_add", WH + [('type', 'u'), ('flags', 'u'), ('amount', 'f'), ('pad', 'u')], [PIC_IN, PIC_OUT])
def fl_add(S):
    """additionRender(): vfxfl::additionPixel on the frame's levels, the mask if asked, the 8-bit store"""
    P = S.p
    with S.in_range(P.w, P.h):
        i = S.gy * P.w + S.gx
        q8 = S.ne(P.flags & 4, 0)
        p = _lv4(S, S.buf('in0').load(i), q8)
        t = tone_of(S, P.type, max3(S, p))
        def ch(c):
            return stdmax(S, S.f(0.0), stdmin(S, S.f(1.0), c + (t * P.amount) * p.w))
        r = S.vec4(ch(p.x), ch(p.y), ch(p.z), p.w)
        r = S.select(S.ne(P.flags & 2, 0), mask_pixel(S, p, r), r)
        S.buf('out').store(i, store_all(S, q8, r))
