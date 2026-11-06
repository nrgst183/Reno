"""Licences: ARM optimized-routines (MIT) and Sun fdlibm notices apply, see third_party/rmath/NOTICE.txt.

Reno's own maths functions (src/core/rmath.h) as Vulkan kernel code: the same algorithm, in the same order of float and double operations, so a kernel gives
the CPU's bits (which are glibc 2.39's; tools/rmath_check.cpp compares the CPU side with the library for every float).

Float functions (atan, atan2, asin) are plain float arithmetic with the kernel's no-contraction rule. The sine and cosine work in doubles (sf64) with the fused
multiply-adds of the C++ side: sf_fma does a*b+c with one rounding by error-free transformations (a product split, a two-sum) - the result is the correctly rounded
one except when the three sums of the correction terms are themselves inexact (about one input in 2^50), where it can be an ulp of the double off, which a float result never shows.
Arguments of sine and cosine beyond +-120 are not handled (NaN comes back): no kernel reaches them.
"""
import struct
from sf64 import SF, _two_sum, _two_prod


def cf(S, bits):
    """a float constant from its bit pattern"""
    return S.bits_to_f(S.u(bits))


def df(S, bits):
    """a double constant (an SF) from its bit pattern"""
    return SF.const(S, struct.unpack('<d', struct.pack('<Q', bits))[0])


def sf_fma(S, a, b, c):
    """a * b + c with a single rounding (see the module text)"""
    p, e = _two_prod(S, a, b)
    s, t = _two_sum(p, c)
    return s + (t + e)


def fma_f(S, a, b, c):
    """std::fma on floats: the product of two floats is exact in a double, the sum is rounded once to a double and then to a float (a tie of the double rounding is the only
    case where this differs from a true fused step: about one input in 2^29; the card's own Fma is not always fused, SwiftShader's is not)"""
    return (SF.from_f32(S, a) * SF.from_f32(S, b) + SF.from_f32(S, c)).to_f32()


# ---------------------------------------------------------------------------------------------------------- sinf, cosf (the sincosf of src/core/rmath.h)
# Kernel size matters (SwiftShader's compiler gives up on huge shaders, and a card spends seconds on them): the polynomials use plain double steps (a product, then
# the sum) instead of the fused ones. That changes the double result by about 1e-16 relative, so a float result differs from the library's for about one input in 10^9
# (one in ~2^29: the double has to land within 1e-16 of a float rounding boundary). The range reduction, where a cancellation would magnify the difference, keeps the
# exact fused step. One call gives both the sine and the cosine, sharing the reduction and the two polynomials.
def _poly_sin(S, x, x2):
    s1, s2, s3 = df(S, 0xbfc555545995a603), df(S, 0x3f81107605230bc4), df(S, 0xbf2994eb3774cf24)
    x3 = x * x2
    t1 = x2 * s3 + s2
    x7 = x3 * x2
    s = x3 * s1 + x
    return x7 * t1 + s


def _poly_cos(S, x2):
    c0, c1, c2 = SF.const(S, 1.0), df(S, 0xbfdffffffd0c621c), df(S, 0x3fa55553e1068f19)
    c3, c4 = df(S, 0xbf56c087e89a359d), df(S, 0x3ef99343027bf8c3)
    x4 = x2 * x2
    cc2 = x2 * c4 + c3
    cc1 = x2 * c1 + c0
    x6 = x4 * x2
    c = x4 * c2 + cc1
    return x6 * cc2 + c


def sincos_f(S, y):
    """(sinf(y), cosf(y)) as glibc 2.39 gives them (|y| < 120: larger arguments give NaN)"""
    ay = S.f_to_bits(y) & 0x7fffffff
    top = ay >> 20
    X = SF.from_f32(S, y)
    small = top < 0x3f4
    r = X * df(S, 0x41645f306dc9c883)
    n0 = (r.trunc_i32() + 0x800000) >> 24
    n = S.select(small, S.i(0), n0)
    nf = SF.from_i32(S, n)
    xr = SF.sel(S, small, X, sf_fma(S, nf.neg(), df(S, 0x3ff921fb54442d18), X))
    x2 = xr * xr
    pv = _poly_sin(S, xr, x2)
    pc = _poly_cos(S, x2)
    nu = S.to_u(n)
    odd = S.ne(nu & 1, 0)
    flip = S.ne(nu & 2, 0)
    sn = SF.sel(S, odd, pc, pv)                      # sin: the cosine polynomial in odd quadrants
    cs = SF.sel(S, odd, pv.neg(), pc)                # cos: minus the sine polynomial in odd quadrants
    sn = SF.sel(S, flip, sn.neg(), sn).to_f32()
    cs = SF.sel(S, flip, cs.neg(), cs).to_f32()
    nan = S.bits_to_f(S.u(0x7fc00000))
    tiny = top < 0x398
    sn = S.select(tiny, y, S.select(top < 0x42f, sn, nan))
    cs = S.select(tiny, S.f(1.0), S.select(top < 0x42f, cs, nan))
    return sn, cs


def sin_f(S, y):
    """sinf as glibc 2.39 gives it (|y| < 120)"""
    return sincos_f(S, y)[0]


def cos_f(S, y):
    """cosf as glibc 2.39 gives it (|y| < 120)"""
    return sincos_f(S, y)[1]


# ---------------------------------------------------------------------------------------------------------- atanf, atan2f, asinf
def atan_f(S, x):
    """atanf (Sun's float arctangent, rmath::atanf)"""
    hi = [cf(S, b) for b in (0x3eed6338, 0x3f490fda, 0x3f7b985e, 0x3fc90fda)]
    lo = [cf(S, b) for b in (0x31ac3769, 0x33222168, 0x33140fb4, 0x33a22168)]
    at = [cf(S, b) for b in (0x3eaaaaab, 0xbe4ccccd, 0x3e124925, 0xbde38e38, 0x3dba2e6e, 0xbd9d8795, 0x3d886b35, 0xbd6ef16b, 0x3d4bda59, 0xbd15a221, 0x3c8569d7)]
    hx = S.f_to_bits(x)
    ix = hx & 0x7fffffff
    ax = S.abs(x)
    one = S.f(1.0)
    x0 = (2.0 * ax - one) / (2.0 + ax)
    x1 = (ax - one) / (ax + one)
    x2 = (ax - 1.5) / (one + 1.5 * ax)
    x3 = -one / ax
    xr = S.select(ix < 0x3ee00000, x,
                  S.select(ix < 0x3f300000, x0, S.select(ix < 0x3f980000, x1, S.select(ix < 0x401c0000, x2, x3))))
    ident = S.select(ix < 0x3ee00000, S.i(-1), S.select(ix < 0x3f300000, S.i(0), S.select(ix < 0x3f980000, S.i(1), S.select(ix < 0x401c0000, S.i(2), S.i(3)))))
    z = xr * xr
    w = z * z
    s1 = z * (at[0] + w * (at[2] + w * (at[4] + w * (at[6] + w * (at[8] + w * at[10])))))
    s2 = w * (at[1] + w * (at[3] + w * (at[5] + w * (at[7] + w * at[9]))))
    small = xr - xr * (s1 + s2)
    hid = S.select(S.eq(ident, 0), hi[0], S.select(S.eq(ident, 1), hi[1], S.select(S.eq(ident, 2), hi[2], hi[3])))
    lod = S.select(S.eq(ident, 0), lo[0], S.select(S.eq(ident, 1), lo[1], S.select(S.eq(ident, 2), lo[2], lo[3])))
    big = hid - ((xr * (s1 + s2) - lod) - xr)
    big = S.select(S.ne(hx >> 31, 0), -big, big)
    res = S.select(ident < 0, small, big)
    res = S.select(ix < 0x31000000, x, res)
    huge = S.select(S.ne(hx >> 31, 0), -hi[3] - lo[3], hi[3] + lo[3])
    res = S.select(ix >= 0x4c000000, huge, res)
    return S.select(ix > 0x7f800000, x + x, res)


def atan2_f(S, y, x):
    """atan2f (Sun's e_atan2f.c, rmath::atan2f)"""
    tiny = S.f(1.0e-30)
    pio4, pio2, pi, pilo = cf(S, 0x3f490fdb), cf(S, 0x3fc90fdb), cf(S, 0x40490fdb), cf(S, 0xb3bbbd2e)
    hx, hy = S.f_to_bits(x), S.f_to_bits(y)
    ix, iy = hx & 0x7fffffff, hy & 0x7fffffff
    ys = S.ne(hy >> 31, 0)
    xs = S.ne(hx >> 31, 0)
    m = ((hy >> 31) & 1) | ((hx >> 30) & 2)
    k = (S.to_i(iy) - S.to_i(ix)) >> 23
    z = S.select(k > 60, pio2 + 0.5 * pilo, S.select(xs & (k < -60), S.f(0.0), atan_f(S, S.abs(y / x))))
    r = S.select(S.eq(m, 0), z, S.select(S.eq(m, 1), -z, S.select(S.eq(m, 2), pi - (z - pilo), (z - pilo) - pi)))
    # the special cases, lowest priority first (the C code returns at the first that applies, in the order: NaN, x == 1, y zero, x zero, x infinite, y infinite)
    r = S.select(S.eq(iy, 0x7f800000) & S.ne(ix, 0x7f800000), S.select(ys, -pio2 - tiny, pio2 + tiny), r)
    r_xi_yi = S.select(S.eq(m, 0), pio4 + tiny, S.select(S.eq(m, 1), -pio4 - tiny, S.select(S.eq(m, 2), 3.0 * pio4 + tiny, -3.0 * pio4 - tiny)))
    r_xi = S.select(S.eq(m, 0), S.f(0.0), S.select(S.eq(m, 1), S.f(-0.0), S.select(S.eq(m, 2), pi + tiny, -pi - tiny)))
    r = S.select(S.eq(ix, 0x7f800000), S.select(S.eq(iy, 0x7f800000), r_xi_yi, r_xi), r)
    r = S.select(S.eq(ix, 0), S.select(ys, -pio2 - tiny, pio2 + tiny), r)
    r_y0 = S.select(m < 2, y, S.select(S.eq(m, 2), pi + tiny, -pi - tiny))
    r = S.select(S.eq(iy, 0), r_y0, r)
    r = S.select(S.eq(hx, 0x3f800000), atan_f(S, y), r)
    return S.select((ix > 0x7f800000) | (iy > 0x7f800000), x + y, r)


def asin_f(S, x):
    """asinf (rmath::asinf: fdlibm's layout in single precision)"""
    a, b, c, d, e = cf(S, 0x3d2cb694), cf(S, 0x3cc6141e), cf(S, 0x3d3a3f25), cf(S, 0x3d9980f2), cf(S, 0x3e2aaae4)
    pio2, lo, lop, pio4 = cf(S, 0x3fc90fdb), cf(S, 0xb33bbd2e), cf(S, 0x333bbd2e), cf(S, 0x3f490fdb)
    u = S.f_to_bits(x)
    ix = u & 0x7fffffff
    poly = lambda t: (((((a * t + b) * t + c) * t + d) * t + e) * t)
    t0 = x * x
    small = S.select(ix < 0x32000000, x, poly(t0) * x + x)
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
    big = S.select(S.ne(u >> 31, 0), -r, r)
    nan = S.bits_to_f(S.u(0x7fc00000))
    one = pio2 * x + x * lo
    return S.select(ix > 0x7f800000, x + x, S.select(ix > 0x3f800000, nan, S.select(S.eq(ix, 0x3f800000), one, S.select(ix < 0x3f000000, small, big))))


# ---------------------------------------------------------------------------------------------------------- logf (ARM's table method, rmath::logf)
_LOGF_TAB = [(0x3ff661ec79f8f3be, 0xbfd57bf7808caade), (0x3ff571ed4aaf883d, 0xbfd2bef0a7c06ddb), (0x3ff49539f0f010b0, 0xbfd01eae7f513a67), (0x3ff3c995b0b80385, 0xbfcb31d8a68224e9),
             (0x3ff30d190c8864a5, 0xbfc6574f0ac07758), (0x3ff25e227b0b8ea0, 0xbfc1aa2bc79c8100), (0x3ff1bb4a4a1a343f, 0xbfba4e76ce8c0e5e), (0x3ff12358f08ae5ba, 0xbfb1973c5a611ccc),
             (0x3ff0953f419900a7, 0xbfa252f438e10c1e), (0x3ff0000000000000, 0x0000000000000000), (0x3fee608cfd9a47ac, 0x3faaa5aa5df25984), (0x3feca4b31f026aa0, 0x3fbc5e53aa362eb4),
             (0x3feb2036576afce6, 0x3fc526e57720db08), (0x3fe9c2d163a1aa2d, 0x3fcbc2860d224770), (0x3fe886e6037841ed, 0x3fd1058bc8a07ee1), (0x3fe767dcf5534862, 0x3fd4043057b6ee09)]


def log_f(S, x):
    """logf as glibc 2.39 gives it (steps unfused except r, see the sine note above; zero, negative, infinity, NaN and subnormal inputs by the special cases)"""
    words = []
    for a, b in _LOGF_TAB:
        words += [a >> 32, a & 0xffffffff, b >> 32, b & 0xffffffff]
    arr = S.const_array(words)
    ix = S.f_to_bits(x)
    ln2, a0, a1, a2 = df(S, 0x3fe62e42fefa39ef), df(S, 0xbfd00ea348b88334), df(S, 0x3fd5575b0be00b6a), df(S, 0xbfdffffef20a4123)
    sub = S.f_to_bits(x * 8388608.0) - (23 << 23)
    normal = (ix - 0x00800000) < (0x7f800000 - 0x00800000)
    is_sub = (~normal) & S.ne(ix * 2, 0) & (ix < 0x7f800000)
    ixn = S.select(is_sub, sub, ix)
    tmp = ixn - 0x3f330000
    i = (tmp >> 19) & 15
    k = S.to_i(tmp) >> 23
    iz = ixn - (tmp & (0x1ff << 23))
    invc = SF(S, arr.load(i * 4), arr.load(i * 4 + 1))
    logc = SF(S, arr.load(i * 4 + 2), arr.load(i * 4 + 3))
    z = SF.from_f32(S, S.bits_to_f(iz))
    r = sf_fma(S, z, invc, SF.const(S, -1.0))
    y0 = SF.from_i32(S, k) * ln2 + logc
    r2 = r * r
    y = a1 * r + a2
    y = a0 * r2 + y
    y = y * r2 + (y0 + r)
    res = y.to_f32()
    inf = S.bits_to_f(S.u(0x7f800000))
    nan = S.bits_to_f(S.u(0xffc00000))
    res = S.select(S.eq(ix * 2, 0), -inf, res)
    res = S.select(S.eq(ix, 0x7f800000), x, res)
    bad = (~normal) & ~is_sub & S.ne(ix * 2, 0) & S.ne(ix, 0x7f800000)
    return S.select(bad, S.select(S.isnan(x), x + x, nan), res)


# ---------------------------------------------------------------------------------------------------------- expf, powf (ARM's table method, rmath::expf and rmath::powf)
_EXP2F_TAB = [
    0x3ff0000000000000, 0x3fefd9b0d3158574, 0x3fefb5586cf9890f, 0x3fef9301d0125b51,
    0x3fef72b83c7d517b, 0x3fef54873168b9aa, 0x3fef387a6e756238, 0x3fef1e9df51fdee1,
    0x3fef06fe0a31b715, 0x3feef1a7373aa9cb, 0x3feedea64c123422, 0x3feece086061892d,
    0x3feebfdad5362a27, 0x3feeb42b569d4f82, 0x3feeab07dd485429, 0x3feea47eb03a5585,
    0x3feea09e667f3bcd, 0x3fee9f75e8ec5f74, 0x3feea11473eb0187, 0x3feea589994cce13,
    0x3feeace5422aa0db, 0x3feeb737b0cdc5e5, 0x3feec49182a3f090, 0x3feed503b23e255d,
    0x3feee89f995ad3ad, 0x3feeff76f2fb5e47, 0x3fef199bdd85529c, 0x3fef3720dcef9069,
    0x3fef5818dcfba487, 0x3fef7c97337b9b5f, 0x3fefa4afa2a490da, 0x3fefd0765b6e4540,
]
_EXP2F_HEAD = [0x42e8000000000000, 0x3fac6af84b912394, 0x3fcebfce50fac4f3, 0x3fe62e42ff0c52d6, 0x4338000000000000, 0x40471547652b82fe, 0x3ebc6af84b912394, 0x3f2ebfce50fac4f3, 0x3f962e42ff0c52d6]
_POWF_TAB = [
    0x3ff661ec79f8f3be, 0xbfdefec65b963019, 0x3ff571ed4aaf883d, 0xbfdb0b6832d4fca4,
    0x3ff49539f0f010b0, 0xbfd7418b0a1fb77b, 0x3ff3c995b0b80385, 0xbfd39de91a6dcf7b,
    0x3ff30d190c8864a5, 0xbfd01d9bf3f2b631, 0x3ff25e227b0b8ea0, 0xbfc97c1d1b3b7af0,
    0x3ff1bb4a4a1a343f, 0xbfc2f9e393af3c9f, 0x3ff12358f08ae5ba, 0xbfb960cbbf788d5c,
    0x3ff0953f419900a7, 0xbfaa6f9db6475fce, 0x3ff0000000000000, 0x0000000000000000,
    0x3fee608cfd9a47ac, 0x3fb338ca9f24f53d, 0x3feca4b31f026aa0, 0x3fc476a9543891ba,
    0x3feb2036576afce6, 0x3fce840b4ac4e4d2, 0x3fe9c2d163a1aa2d, 0x3fd40645f0c6651c,
    0x3fe886e6037841ed, 0x3fd88e9c2c1b9ff8, 0x3fe767dcf5534862, 0x3fdce0a44eb17bcc,
]
_POWF_POLY = [0x3fd27616c9496e0b, 0xbfd71969a075c67a, 0x3fdec70a6ca7badd, 0xbfe7154748bef6c8, 0x3ff71547652ab82b]


def _tab_words(vals):
    words = []
    for v in vals:
        words += [v >> 32, v & 0xffffffff]
    return words


def exp_f(S, x):
    """expf as glibc 2.39 gives it (x = (k + r) ln2/32, a table of 2^(k/32), a cubic in r; the fused steps through sf_fma; the whole float range: results below 2^-126 are
    rounded by the kernel's own double to float conversion)"""
    ux = S.f_to_bits(x)
    abstop = (ux >> 20) & 0x7ff
    arr = S.const_array(_tab_words(_EXP2F_TAB))
    shift, invln2n = df(S, _EXP2F_HEAD[4]), df(S, _EXP2F_HEAD[5])
    c0, c1, c2 = df(S, _EXP2F_HEAD[6]), df(S, _EXP2F_HEAD[7]), df(S, _EXP2F_HEAD[8])
    z = SF.from_f32(S, x) * invln2n
    kd = z + shift
    ki = kd.lo                                              # (the low word of the 64-bit pattern: k plus the bias of the shift)
    r = z - (kd - shift)
    idx = ki & 31
    s = SF(S, arr.load(idx * 2) + ((ki & 0x1ffff) << 15), arr.load(idx * 2 + 1))
    zz = sf_fma(S, c0, r, c1)
    y = sf_fma(S, zz, r * r, sf_fma(S, c2, r, SF.const(S, 1.0)))
    res = (y * s).to_f32()
    inf = S.bits_to_f(S.u(0x7f800000))
    big = abstop >= 0x42b
    res = S.select(big & (x > S.bits_to_f(S.u(0x42b17217))), inf, res)
    res = S.select(big & (x < S.bits_to_f(S.u(0xc2cff1b4))), S.f(0.0), res)
    res = S.select(big & (abstop >= 0x7f8), x + x, res)
    return S.select(S.eq(ux, 0xff800000), S.f(0.0), res)


def _checkint_f(S, iy):
    """checkintf: 0 not an integer, 1 an odd integer, 2 an even one (the bits of a float)"""
    e = (iy >> 23) & 0xff
    sh = S.select((e >= 0x7f) & (e <= 0x7f + 23), S.u(0x7f + 23) - e, S.u(0))
    frac = S.ne(iy & ((S.u(1) << sh) - 1), 0)
    odd = S.ne(iy & (S.u(1) << sh), 0)
    r = S.select(frac, S.u(0), S.select(odd, S.u(1), S.u(2)))
    return S.select(e < 0x7f, S.u(0), S.select(e > 0x7f + 23, S.u(2), r))


def pow_f(S, x, y):
    """powf as glibc 2.39 gives it (log2 of x in double from a 16 entry table and a degree 4 polynomial, times y, then 2^ of it as in expf; all special cases: zero,
    negative, infinite and NaN operands). The polynomial steps are plain double steps (see the sine note above), the argument reduction and the last steps are fused."""
    ix, iy = S.f_to_bits(x), S.f_to_bits(y)
    nan = S.bits_to_f(S.u(0xffc00000))
    inf = S.bits_to_f(S.u(0x7f800000))
    zinf = lambda i: (i * 2 - 1) >= S.u(2 * 0x7f800000 - 1)             # zeroinfnanf (wraps for zero like the C code)
    y_spec = zinf(iy)
    x_notnorm = (ix - 0x00800000) >= S.u(0x7f800000 - 0x00800000)
    x_spec = zinf(ix)
    # --- y zero, infinite or NaN
    ysnan = S.eq(iy & 0x7fc00000, 0x7f800000) & S.ne(iy << 9, 0)
    xsnan = S.eq(ix & 0x7fc00000, 0x7f800000) & S.ne(ix << 9, 0)
    below1 = ix * 2 < S.u(2 * 0x3f800000)
    ypos = S.eq(iy & 0x80000000, 0)
    r_ys = S.select(~(below1 ^ ypos), S.f(0.0), y * y)
    r_ys = S.select(S.eq(ix * 2, 2 * 0x3f800000), S.f(1.0), r_ys)
    r_ys = S.select((ix * 2 > S.u(2 * 0x7f800000)) | (iy * 2 > S.u(2 * 0x7f800000)), x + y, r_ys)
    r_ys = S.select(S.eq(ix, 0x3f800000), S.select(ysnan, x + y, S.f(1.0)), r_ys)
    r_ys = S.select(S.eq(iy * 2, 0), S.select(xsnan, x + y, S.f(1.0)), r_ys)
    # --- x zero, infinite or NaN (y finite, not zero)
    x2 = x * x
    yi = _checkint_f(S, iy)
    x2 = S.select(S.ne(ix & 0x80000000, 0) & S.eq(yi, 1), -x2, x2)
    r_xs = S.select(S.ne(iy & 0x80000000, 0), S.f(1.0) / x2, x2)
    # --- negative x
    xneg = S.ne(ix & 0x80000000, 0)
    r_neg = S.select(S.eq(yi, 0), nan, S.f(0.0))        # (only the invalid case is an answer here; the others continue below)
    sbias = S.select(xneg & S.eq(yi, 1), S.u(0x800 << 5), S.u(0))
    ixa = S.select(xneg, ix & 0x7fffffff, ix)
    # --- subnormal x
    sub = ixa < 0x00800000
    ixs = (S.f_to_bits(S.bits_to_f(ixa) * 8388608.0) & 0x7fffffff) - (23 << 23)
    ixn = S.select(sub, ixs, ixa)
    ixn = S.select(x_notnorm & ~y_spec & ~x_spec, ixn, S.select(x_notnorm, S.u(0x3f800000), ix))
    # --- log2(x)
    arr = S.const_array(_tab_words(_POWF_TAB))
    tmp = ixn - 0x3f330000
    i = (tmp >> 19) & 15
    top = tmp & 0xff800000
    iz = ixn - top
    k = S.to_i(top) >> 23
    invc = SF(S, arr.load(i * 4), arr.load(i * 4 + 1))
    logc = SF(S, arr.load(i * 4 + 2), arr.load(i * 4 + 3))
    zz = SF.from_f32(S, S.bits_to_f(iz))
    r = sf_fma(S, zz, invc, SF.const(S, -1.0))
    y0 = logc + SF.from_i32(S, k)
    r2 = r * r
    A0, A1, A2, A3, A4 = [df(S, b) for b in _POWF_POLY]
    yy = A0 * r + A1
    p = A2 * r + A3
    r4 = r2 * r2
    q = A4 * r + y0
    q = p * r2 + q
    logx = yy * r4 + q
    ylogx = SF.from_f32(S, y) * logx
    over = ylogx > SF.const(S, float.fromhex('0x1.fffffffd1d571p+6'))
    under = ylogx <= SF.const(S, -150.0)
    # --- 2^ylogx
    shift, c0, c1, c2 = df(S, _EXP2F_HEAD[0]), df(S, _EXP2F_HEAD[1]), df(S, _EXP2F_HEAD[2]), df(S, _EXP2F_HEAD[3])
    earr = S.const_array(_tab_words(_EXP2F_TAB))
    kd = ylogx + shift
    ki = kd.lo
    rr = ylogx - (kd - shift)
    idx = ki & 31
    s = SF(S, earr.load(idx * 2) + (((ki + sbias) & 0x1ffff) << 15), earr.load(idx * 2 + 1))
    z = sf_fma(S, c0, rr, c1)
    res = sf_fma(S, z, rr * rr, sf_fma(S, c2, rr, SF.const(S, 1.0)))
    out = (res * s).to_f32()
    ninf = S.bits_to_f(S.u(0xff800000))
    out = S.select(under, S.select(S.ne(sbias, 0), S.f(-0.0), S.f(0.0)), out)
    out = S.select(over, S.select(S.ne(sbias, 0), ninf, inf), out)
    # --- the special cases, in the C code's order of priority
    out = S.select(x_notnorm & xneg & ~x_spec & S.eq(yi, 0), nan, out)
    out = S.select(x_spec, r_xs, out)
    return S.select(y_spec, r_ys, out)
