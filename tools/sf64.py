"""IEEE double precision in 32-bit integer arithmetic, for the Vulkan kernels (tools/spvdsl.py).

A double is a pair of uint values (hi, lo). Add, subtract, multiply, divide and square root round to nearest even like the CPU's, so a kernel can do
the same double arithmetic as the C++ code on any device (a GPU without shaderFloat64 included). Not covered, as the effects never need them: NaN and
infinity inputs, subnormal numbers (they are taken as zero, and a result that small is zero), overflow (an infinity of the right sign is made).

Written against an interface (S) with: u(c), select(c, a, b), find_umsb(x), f_to_bits(f), bits_to_f(u), to_i(u), to_u(i), to_f(u/i), and the operators
+ - * & | ^ ~ << >> and comparisons on values (the kernel DSL's V, or the numpy emulator in tools/sf64_test.py that checks this file against numpy)."""


class W:
    """a 64-bit unsigned value (hi, lo)"""
    def __init__(self, hi, lo):
        self.hi, self.lo = hi, lo


def _b(S, c):
    return S.select(c, S.u(1), S.u(0))


def w_add(S, a, b):
    lo = a.lo + b.lo
    return W(a.hi + b.hi + _b(S, lo < a.lo), lo)


def w_sub(S, a, b):
    lo = a.lo - b.lo
    return W(a.hi - b.hi - _b(S, a.lo < b.lo), lo)


def w_ge(S, a, b):
    return (a.hi > b.hi) | (S.eq(a.hi, b.hi) & (a.lo >= b.lo))


def w_lt(S, a, b):
    return (a.hi < b.hi) | (S.eq(a.hi, b.hi) & (a.lo < b.lo))


def w_is0(S, a):
    return S.eq(a.hi | a.lo, 0)


def w_sel(S, c, a, b):
    return W(S.select(c, a.hi, b.hi), S.select(c, a.lo, b.lo))


def w_shl(S, x, n):
    """x << n for n in 0..63"""
    s = n & 31
    big = n >= 32
    hi = (x.hi << s) | ((x.lo >> 1) >> (31 - s))
    lo = x.lo << s
    return W(S.select(big, x.lo << s, hi), S.select(big, S.u(0), lo))


def w_shr(S, x, n):
    """x >> n for n in 0..63"""
    s = n & 31
    big = n >= 32
    lo = (x.lo >> s) | ((x.hi << 1) << (31 - s))
    hi = x.hi >> s
    return W(S.select(big, S.u(0), hi), S.select(big, x.hi >> s, lo))


def w_shr_sticky(S, x, n):
    """(x >> n, 1 if a set bit was shifted out) for n in 0..64"""
    n63 = S.select(n > 63, S.u(63), n)
    r = w_shr(S, x, n63)
    r = w_sel(S, n > 63, W(S.u(0), S.u(0)), r)
    back = w_shl(S, r, n63)
    lost = S.select(n > 63, ~w_is0(S, x), ~(S.eq(back.hi, x.hi) & S.eq(back.lo, x.lo)))
    return r, lost


def w_msb(S, x):
    """index of the highest set bit (x not 0)"""
    return S.select(S.ne(x.hi, 0), S.find_umsb(x.hi) + 32, S.find_umsb(x.lo))


def mul32(S, a, b):
    """a * b as (hi, lo)"""
    a0, a1, b0, b1 = a & 0xffff, a >> 16, b & 0xffff, b >> 16
    p00, p01, p10, p11 = a0 * b0, a0 * b1, a1 * b0, a1 * b1
    mid = p01 + p10
    c1 = _b(S, mid < p01)
    lo = p00 + (mid << 16)
    c2 = _b(S, lo < p00)
    hi = p11 + (mid >> 16) + (c1 << 16) + c2
    return hi, lo


class SF:
    """a double in a kernel: hi, lo are uint values"""
    def __init__(self, S, hi, lo):
        self.S, self.hi, self.lo = S, hi, lo

    # ---- construction
    @staticmethod
    def const(S, x):
        import struct
        b = struct.unpack("<Q", struct.pack("<d", float(x)))[0]
        return SF(S, S.u(b >> 32), S.u(b & 0xffffffff))

    @staticmethod
    def of(S, x):
        return x if isinstance(x, SF) else SF.const(S, x)

    @staticmethod
    def from_f32(S, v):
        bits = S.f_to_bits(v)
        sign = bits & 0x80000000
        e8 = (bits >> 23) & 255
        frac = bits & 0x7fffff
        hi = sign | ((e8 + 896) << 20) | (frac >> 3)
        lo = (frac & 7) << 29
        z = S.eq(e8, 0)
        return SF(S, S.select(z, sign, hi), S.select(z, S.u(0), lo))

    @staticmethod
    def from_u32(S, v):
        zero = S.eq(v, 0)
        v1 = S.select(zero, S.u(1), v)
        p = S.find_umsb(v1)
        sig = w_shl(S, W(S.u(0), v1), S.u(52) - p)
        hi = ((p + 1023) << 20) | (sig.hi & 0xfffff)
        return SF(S, S.select(zero, S.u(0), hi), S.select(zero, S.u(0), sig.lo))

    @staticmethod
    def from_i32(S, v):
        neg = v < 0
        mag = S.select(neg, S.to_u(S.i(0) - v), S.to_u(v))
        r = SF.from_u32(S, mag)
        return SF(S, r.hi | S.select(neg & S.ne(mag, 0), S.u(0x80000000), S.u(0)), r.lo)

    @staticmethod
    def sel(S, c, a, b):
        return SF(S, S.select(c, a.hi, b.hi), S.select(c, a.lo, b.lo))

    # ---- pieces
    def _unpack(self):
        S = self.S
        sign = self.hi >> 31
        e = (self.hi >> 20) & 0x7ff
        z = S.eq(e, 0)
        sh = S.select(z, S.u(0), (self.hi & 0xfffff) | 0x100000)
        sl = S.select(z, S.u(0), self.lo)
        return sign, e, W(sh, sl), z

    @staticmethod
    def _pack(S, sign, e, sig, zero=None):
        """e: u32 holding a signed exponent; sig: 53 bits with the leading one"""
        ei = S.to_i(e)
        hi = (sign << 31) | ((S.to_u(ei) & 0x7ff) << 20) | (sig.hi & 0xfffff)
        under = ei <= 0
        over = ei >= 2047
        z = under if zero is None else (under | zero)
        hi = S.select(over, (sign << 31) | 0x7ff00000, hi)
        lo = S.select(over, S.u(0), sig.lo)
        return SF(S, S.select(z, sign << 31, hi), S.select(z, S.u(0), lo))

    def _key(self):
        S = self.S
        m = self.hi & 0x7fffffff
        return m, self.lo

    def is_zero(self):
        S = self.S
        return S.eq(self.hi & 0x7ff00000, 0)

    def neg(self):
        return SF(self.S, self.hi ^ 0x80000000, self.lo)

    def abs(self):
        return SF(self.S, self.hi & 0x7fffffff, self.lo)

    __neg__ = neg

    # ---- arithmetic
    def __add__(self, o):
        return _add(self.S, self, SF.of(self.S, o))

    def __radd__(self, o):
        return _add(self.S, SF.of(self.S, o), self)

    def __sub__(self, o):
        o = SF.of(self.S, o)
        return _add(self.S, self, o.neg())

    def __rsub__(self, o):
        return _add(self.S, SF.of(self.S, o), self.neg())

    def __mul__(self, o):
        return _mul(self.S, self, SF.of(self.S, o))

    __rmul__ = __mul__

    def __truediv__(self, o):
        return _div(self.S, self, SF.of(self.S, o))

    def __rtruediv__(self, o):
        return _div(self.S, SF.of(self.S, o), self)

    def sqrt(self):
        return _sqrt(self.S, self)

    # ---- comparisons (booleans)
    def _ord(self):
        """(hi, lo) that order as unsigned like the numbers do (zeros alike)"""
        S = self.S
        z = self.is_zero()
        neg = S.ne(self.hi >> 31, 0) & ~z
        hi = S.select(z, S.u(0x80000000), S.select(neg, ~self.hi, self.hi | 0x80000000))
        lo = S.select(z, S.u(0), S.select(neg, ~self.lo, self.lo))
        return W(hi, lo)

    def __lt__(self, o):
        S = self.S
        return w_lt(S, self._ord(), SF.of(S, o)._ord())

    def __ge__(self, o):
        S = self.S
        return ~w_lt(S, self._ord(), SF.of(S, o)._ord())

    def __gt__(self, o):
        S = self.S
        return w_lt(S, SF.of(S, o)._ord(), self._ord())

    def __le__(self, o):
        S = self.S
        return ~w_lt(S, SF.of(S, o)._ord(), self._ord())

    def eq(self, o):
        S = self.S
        a, b = self._ord(), SF.of(S, o)._ord()
        return S.eq(a.hi, b.hi) & S.eq(a.lo, b.lo)

    # ---- conversions
    def to_f32(self):
        """the float nearest to the double (to nearest even; tiny values are 0)"""
        S = self.S
        sign, e, sig, z = self._unpack()
        mant = w_shr(S, sig, S.u(29)).lo                     # 24 bits with the leading one
        rest_hi = sig.lo & 0x1fffffff
        rnd = S.ne(sig.lo & 0x10000000, 0)
        sticky = S.ne(sig.lo & 0x0fffffff, 0)
        up = rnd & (sticky | S.ne(mant & 1, 0))
        mant = mant + _b(S, up)
        carry = S.ne(mant & 0x1000000, 0)
        mant = S.select(carry, mant >> 1, mant)
        e8 = S.to_i(e) - 896 + S.to_i(_b(S, carry))
        bits = (sign << 31) | (S.to_u(e8) << 23) | (mant & 0x7fffff)
        bits = S.select(e8 >= 255, (sign << 31) | 0x7f800000, bits)
        bits = S.select((e8 <= 0) | z, sign << 31, bits)
        return S.bits_to_f(bits)

    def trunc_i32(self):
        """(int)x for |x| < 2^31"""
        S = self.S
        sign, e, sig, z = self._unpack()
        ex = S.to_i(e) - 1023
        small = ex < 0
        sh = S.to_u(S.select(small, S.i(52), S.i(52) - ex))
        sh = S.select(sh > 52, S.u(52), sh)
        mag = w_shr(S, sig, sh).lo
        mag = S.select(small | z, S.u(0), mag)
        return S.select(S.ne(sign, 0), S.i(0) - S.to_i(mag), S.to_i(mag))

    def floor_i32(self):
        """(int)floor(x) for |x| < 2^31"""
        S = self.S
        t = self.trunc_i32()
        back = SF.from_i32(S, t)
        # when truncation moved a negative number up, take one off
        lower = self < back
        return S.select(lower, t - 1, t)

    def trunc(self):
        """trunc(x) as a double (x of any size)"""
        S = self.S
        sign, e, sig, z = self._unpack()
        ex = S.to_i(e) - 1023
        # the fraction bits are the low (52 - ex) bits of the significand: cleared
        keep = S.to_u(S.select(ex < 0, S.i(0), S.select(ex > 52, S.i(52), ex)))     # whole-number bits above the leading one
        fr = S.u(52) - keep                                            # fraction bits
        mask_shift = w_shl(S, w_shr(S, sig, fr), fr)
        out = _pack_keep(S, sign, e, mask_shift)
        zero = SF(S, sign << 31, S.u(0))
        return SF.sel(S, (ex < 0) | z, zero, SF.sel(S, ex >= 52, self, out))

    def floor(self):
        S = self.S
        t = self.trunc()
        return SF.sel(S, self < t, t - 1.0, t)

    def min(self, o):
        o = SF.of(self.S, o)
        return SF.sel(self.S, o < self, o, self)

    def max(self, o):
        o = SF.of(self.S, o)
        return SF.sel(self.S, o > self, o, self)


def _pack_keep(S, sign, e, sig):
    hi = (sign << 31) | (e << 20) | (sig.hi & 0xfffff)
    return SF(S, hi, sig.lo)


def _round_pack(S, sign, e, mant, rnd, sticky, zero):
    """mant: 53-bit W; round to nearest even with the round bit and sticky, renormalise and pack"""
    up = rnd & (sticky | S.ne(mant.lo & 1, 0))
    m = w_add(S, mant, W(S.u(0), _b(S, up)))
    carry = S.ne(m.hi & 0x200000, 0)
    m = w_sel(S, carry, w_shr(S, m, S.u(1)), m)
    e = e + _b(S, carry)
    return SF._pack(S, sign, e, m, zero)


def _add(S, a, b):
    ka, kb = a._key(), b._key()
    swap = (ka[0] < kb[0]) | (S.eq(ka[0], kb[0]) & (ka[1] < kb[1]))
    A = SF.sel(S, swap, b, a)
    B = SF.sel(S, swap, a, b)
    sa, ea, siga, za = A._unpack()
    sb, eb, sigb, zb = B._unpack()
    d = ea - eb
    sA = w_shl(S, siga, S.u(3))
    sB0 = w_shl(S, sigb, S.u(3))
    sB, lost = w_shr_sticky(S, sB0, S.select(d > 60, S.u(60), d))
    sB = W(sB.hi, sB.lo | _b(S, lost))
    same = S.eq(sa, sb)
    tot = w_sel(S, same, w_add(S, sA, sB), w_sub(S, sA, sB))
    zero = w_is0(S, tot)
    tot = w_sel(S, zero, W(S.u(0), S.u(1)), tot)
    p = w_msb(S, tot)                                         # 55 or 56 without cancellation
    over = p > 55
    # one too many bits: shift right by one, keeping what falls out
    r1 = w_shr(S, tot, S.u(1))
    r1 = W(r1.hi, r1.lo | (tot.lo & 1))
    sh = S.select(over, S.u(0), S.u(55) - p)
    n = w_sel(S, over, r1, w_shl(S, tot, sh))
    e = ea + _b(S, over) - sh
    low = n.lo & 7
    mant = w_shr(S, n, S.u(3))
    up = (low > 4) | (S.eq(low, 4) & S.ne(mant.lo & 1, 0))
    m = w_add(S, mant, W(S.u(0), _b(S, up)))
    carry = S.ne(m.hi & 0x200000, 0)
    m = w_sel(S, carry, w_shr(S, m, S.u(1)), m)
    e = e + _b(S, carry)
    r = SF._pack(S, sa, e, m, zero)
    # an exact zero from x - x is +0
    return SF(S, S.select(zero, S.u(0), r.hi), r.lo)


def _mul(S, a, b):
    sa, ea, A, za = a._unpack()
    sb, eb, B, zb = b._unpack()
    h0, l0 = mul32(S, A.lo, B.lo)
    h1, l1 = mul32(S, A.lo, B.hi)
    h2, l2 = mul32(S, A.hi, B.lo)
    h3, l3 = mul32(S, A.hi, B.hi)
    L0 = l0
    t1 = h0 + l1
    c1a = _b(S, t1 < h0)
    L1 = t1 + l2
    c1b = _b(S, L1 < t1)
    carry1 = c1a + c1b
    t2 = h1 + h2
    c2a = _b(S, t2 < h1)
    t2b = t2 + l3
    c2b = _b(S, t2b < t2)
    L2 = t2b + carry1
    c2c = _b(S, L2 < t2b)
    L3 = h3 + c2a + c2b + c2c
    top = S.ne(L3 & 0x200, 0)                                    # the product has 106 bits
    # top: mant = P >> 53, round = bit 52; otherwise mant = P >> 52, round = bit 51
    m1 = W((L2 >> 21) | (L3 << 11), (L1 >> 21) | (L2 << 11))
    m0 = W((L2 >> 20) | (L3 << 12), (L1 >> 20) | (L2 << 12))
    mant = w_sel(S, top, m1, m0)
    rnd = S.select(top, S.ne(L1 & 0x100000, 0), S.ne(L1 & 0x80000, 0))
    sticky = S.select(top, S.ne(L1 & 0xfffff, 0), S.ne(L1 & 0x7ffff, 0)) | S.ne(L0, 0)
    e = ea + eb - 1023 + _b(S, top)
    return _round_pack(S, sa ^ sb, e, mant, rnd, sticky, za | zb)


def _div(S, a, b):
    sa, ea, A, za = a._unpack()
    sb, eb, B, zb = b._unpack()
    less = w_lt(S, A, B)
    R0 = w_sel(S, less, w_shl(S, A, S.u(1)), A)
    Rh, Rl, qh, ql = S.var('u', R0.hi), S.var('u', R0.lo), S.var('u', 0), S.var('u', 0)

    def step(_):
        R = W(Rh.get(), Rl.get())
        ge = w_ge(S, R, B)
        R = w_sel(S, ge, w_sub(S, R, B), R)
        ql_, qh_ = ql.get(), qh.get()
        qh.set((qh_ << 1) | (ql_ >> 31))
        ql.set((ql_ << 1) | _b(S, ge))
        R = w_shl(S, R, S.u(1))
        Rh.set(R.hi)
        Rl.set(R.lo)
    S.loop(54, step)
    q = W(qh.get(), ql.get())
    R = W(Rh.get(), Rl.get())
    mant = w_shr(S, q, S.u(1))
    rnd = S.ne(q.lo & 1, 0)
    sticky = ~w_is0(S, R)
    e = ea - eb + 1023 - _b(S, less)
    return _round_pack(S, sa ^ sb, e, mant, rnd, sticky, za)


def _sqrt(S, a):
    sign, e, M, z = a._unpack()
    ex = e - 1023                                               # as unsigned wrap; parity is what matters
    odd = S.ne(ex & 1, 0)
    # radicand M' = M (even exponent) or 2 M (odd); the root of M' 2^56 has 55 bits
    Mp = w_sel(S, odd, w_shl(S, M, S.u(1)), M)
    rh, rl, mh, ml = S.var('u', 0), S.var('u', 0), S.var('u', 0), S.var('u', 0)      # root, remainder (hi, lo)

    def step(j):
        sh = S.select(j < 27, S.u(52) - j * 2, S.u(0))
        pair = S.select(j < 27, w_shr(S, Mp, sh).lo & 3, S.u(0))
        rem = W(mh.get(), ml.get())
        root = W(rh.get(), rl.get())
        rem = W((rem.hi << 2) | (rem.lo >> 30), (rem.lo << 2) | pair)
        trial = W((root.hi << 2) | (root.lo >> 30), (root.lo << 2) | 1)
        ge = w_ge(S, rem, trial)
        rem = w_sel(S, ge, w_sub(S, rem, trial), rem)
        root = W((root.hi << 1) | (root.lo >> 31), (root.lo << 1) | _b(S, ge))
        mh.set(rem.hi)
        ml.set(rem.lo)
        rh.set(root.hi)
        rl.set(root.lo)
    S.loop(55, step)
    root = W(rh.get(), rl.get())
    rem = W(mh.get(), ml.get())
    mant = w_shr(S, root, S.u(2))
    rnd = S.ne(root.lo & 2, 0)
    sticky = S.ne(root.lo & 1, 0) | ~w_is0(S, rem)
    # exponent: (ex - odd) / 2 + 1023 with the sign kept (arithmetic shift)
    exs = S.to_i(ex) - S.to_i(_b(S, odd))
    half = exs >> 1
    ne = S.to_u(half + 1023)
    r = _round_pack(S, S.u(0), ne, mant, rnd, sticky, z)
    return r


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
# elementary functions in these doubles: accurate to about 1e-15 relative (a few ulp), far below the 24 bits of the float every result ends up as; they are
# NOT the C library's own digits (a library's results differ from each other in the last bits too), only the same value to better than a float's rounding.

LN2_HI = 6.93147180369123816490e-01       # (the high part has trailing zero bits: k * LN2_HI is exact for small k)
LN2_LO = 1.90821492927058770002e-10
LOG2E = 1.4426950408889634
SQRT2 = 1.4142135623730951


def _horner(S, z, coeffs):
    """c0 + z (c1 + z (c2 + ...)) as a loop over a constant array of the coefficients (one multiply and one add in the kernel)"""
    import struct
    words = []
    for c in reversed(coeffs[:-1]):                 # the loop's order: the second highest first
        b = struct.unpack("<Q", struct.pack("<d", float(c)))[0]
        words += [b >> 32, b & 0xffffffff]
    arr = S.const_array(words)
    top = SF.const(S, coeffs[-1])
    ah, al = S.var('u', top.hi), S.var('u', top.lo)

    def step(i):
        acc = SF(S, ah.get(), al.get()) * z + SF(S, arr.load(i * 2), arr.load(i * 2 + 1))
        ah.set(acc.hi)
        al.set(acc.lo)
    S.loop(len(coeffs) - 1, step)
    return SF(S, ah.get(), al.get())


def sf_exp(S, x, xlo=None):
    """e ** (x + xlo): xlo is a small correction to x (the part x lost in rounding)"""
    big = x > 709.0
    small = x < -708.0
    xc = x.min(709.0).max(-708.0)
    k = (xc * LOG2E + 0.5).floor_i32()
    kf = SF.from_i32(S, k)
    r = (xc - kf * LN2_HI) - kf * LN2_LO
    if xlo is not None:
        r = r + xlo
    import math
    p = _horner(S, r, [1.0 / math.factorial(i) for i in range(0, 15)])
    scale = SF(S, S.to_u(k + 1023) << 20, S.u(0))
    res = p * scale
    res = SF.sel(S, small, SF.const(S, 0.0), res)
    return SF.sel(S, big, SF.const(S, float('inf')), res)


def _two_sum(a, b):
    s = a + b
    bb = s - a
    return s, (a - (s - bb)) + (b - bb)


def _two_prod(S, a, b):
    """(p, e) with p = a b rounded and p + e = a b exactly (Dekker's split)"""
    c = 134217729.0
    t = a * c
    ah = t - (t - a)
    al = a - ah
    t = b * c
    bh = t - (t - b)
    bl = b - bh
    p = a * b
    e = ((ah * bh - p) + ah * bl + al * bh) + al * bl
    return p, e


def sf_log2(S, x):
    """(hi, lo): the natural logarithm of x > 0 as a rounded double and what the rounding lost (to about 1e-16 absolute)"""
    e0 = S.to_i((x.hi >> 20) & 0x7ff) - 1023
    m = SF(S, (x.hi & 0x000fffff) | 0x3ff00000, x.lo)              # [1, 2)
    hi_half = m > SQRT2
    m = SF.sel(S, hi_half, m * 0.5, m)
    e = SF.from_i32(S, e0 + S.select(hi_half, S.i(1), S.i(0)))
    f = m - 1.0
    s = f / (f + 2.0)
    z = s * s
    # 2 s (1 + z/3 + z^2/5 + ...)
    p = _horner(S, z, [1.0 / (2 * i + 1) for i in range(0, 14)])
    lg = s * 2.0 * p
    rest = e * LN2_LO + lg
    return _two_sum(e * LN2_HI, rest)


def sf_log(S, x):
    """natural logarithm of x > 0 (x <= 0 gives a large negative number)"""
    hi, lo = sf_log2(S, x)
    return hi + lo


def sf_pow(S, x, y):
    """x ** y for x > 0 (x == 0: 0 for y > 0, else 1 for y == 0)"""
    lh, ll = sf_log2(S, x)
    th, tl = _two_prod(S, y, lh)
    tl = tl + y * ll
    r = sf_exp(S, th, tl)
    r = SF.sel(S, x.eq(0.0), SF.sel(S, y.eq(0.0), SF.const(S, 1.0), SF.const(S, 0.0)), r)
    return r


_P1 = 1.57079632673412561417e+00
_P2 = 6.07710050630396597660e-11
_P3 = 2.02226624871116645580e-21
_P4 = 8.47842766036889956997e-32
_TWO_OVER_PI = 0.6366197723675814


def _sincos_core(S, x):
    """(sin r, cos r, k) with x = k pi/2 + r"""
    import math
    k = (x * _TWO_OVER_PI + SF.sel(S, x < 0.0, SF.const(S, -0.5), SF.const(S, 0.5))).trunc_i32()
    kf = SF.from_i32(S, k)
    r = ((x - kf * _P1) - kf * _P2) - kf * _P3 - kf * _P4
    z = r * r
    sc = [(-1.0) ** i / math.factorial(2 * i + 1) for i in range(0, 12)]
    cc = [(-1.0) ** i / math.factorial(2 * i) for i in range(0, 12)]
    sn = r * _horner(S, z, sc)
    cs = _horner(S, z, cc)
    return sn, cs, k


def sf_sin(S, x):
    sn, cs, k = _sincos_core(S, x)
    q = k & 3
    v = SF.sel(S, S.eq(q, 0), sn, SF.sel(S, S.eq(q, 1), cs, SF.sel(S, S.eq(q, 2), sn.neg(), cs.neg())))
    return v


def sf_cos(S, x):
    sn, cs, k = _sincos_core(S, x)
    q = k & 3
    v = SF.sel(S, S.eq(q, 0), cs, SF.sel(S, S.eq(q, 1), sn.neg(), SF.sel(S, S.eq(q, 2), cs.neg(), sn)))
    return v


def sf_sincos(S, x):
    sn, cs, k = _sincos_core(S, x)
    q = k & 3
    s = SF.sel(S, S.eq(q, 0), sn, SF.sel(S, S.eq(q, 1), cs, SF.sel(S, S.eq(q, 2), sn.neg(), cs.neg())))
    c = SF.sel(S, S.eq(q, 0), cs, SF.sel(S, S.eq(q, 1), sn.neg(), SF.sel(S, S.eq(q, 2), cs.neg(), sn)))
    return s, c


def sf_atan2(S, y, x):
    """atan2(y, x): the float library's angle refined by one Newton step on the doubles' sine and cosine"""
    a0 = S.atan2_f(y.to_f32(), x.to_f32())
    a = SF.from_f32(S, a0)
    s, c = sf_sincos(S, a)
    num = y * c - x * s
    den = x * c + y * s
    corr = num / SF.sel(S, den.eq(0.0), SF.const(S, 1.0), den)
    both0 = x.eq(0.0) & y.eq(0.0)
    return SF.sel(S, both0, SF.const(S, 0.0), a + corr)
