"""A float division that is exactly the IEEE one (round to nearest even) whatever the card's own divide does.

Vulkan lets OpFDiv be off by 2.5 ulp, and a real card (NVIDIA's divide is accurate to 2 ulp) may be. A first correction with the exact remainder and a reciprocal from
the same divide is right except when the true quotient lies within about 2^-22 of a rounding tie (the reciprocal's error then decides): a few results per million
divisions, which is how a one-pixel difference appears on a real GPU and never in a software driver. This version decides the last bit with exact arithmetic only:

    q1 = q0 + r0 y                         (r0 = x - q0 a, exact; y the card's 1 / a)        q1 is the correct result or its neighbour on one side
    r1 = x - q1 a                          (exact, by Dekker's product)                      its sign says which side the true quotient is on
    move to that neighbour when |r1| > gap a / 2, or when equal and q1's last bit is odd (ties go to even)

where gap is the distance to that neighbour (half of the usual one below a power of two). Every step is a plain float add, subtract or multiply (the kernel DSL decorates
them NoContraction) or a comparison, so the result does not depend on the card. Written against an interface S (abs, select, f_to_bits, bits_to_f, comparisons and operators)
that both the kernel DSL and the numpy emulator of tools/rmath_gpu_test.py have; hw(x, a) is the card's own divide.
Operands whose quotient or error terms could leave the float range (|a| outside 1e-18..1e18, |x| outside 1e-18..1e18, a quotient under 1e-25 or over 1e25) get the first
correction only, as before; zero numerator gives the card's zero."""


def fdiv_exact(S, x, a, hw):
    one = S.f(1.0)
    q0 = hw(x, a)
    y = hw(one, a)
    split = S.f(4097.0)

    def parts(v):
        t = split * v
        hi = t - (t - v)
        return hi, v - hi

    ah, al = parts(a)

    def residual(q):
        """x - q a exactly (q within an ulp or two of x / a)"""
        qh, ql = parts(q)
        p = q * a
        e = (((qh * ah - p) + qh * al) + ql * ah) + ql * al
        return (x - p) - e

    r0 = residual(q0)
    q1 = q0 + r0 * y
    r1 = residual(q1)
    # the neighbour on the side of the true quotient
    qpos = q1 > 0.0
    tpos = ~((r1 >= 0.0) ^ (a > 0.0))
    m = S.f_to_bits(q1) & 0x7fffffff
    away = ~(tpos ^ qpos)
    nb_m = S.select(away, m + 1, m - 1)
    nb = S.bits_to_f(nb_m)
    aq = S.bits_to_f(m)
    gap = S.abs(nb - aq)
    half = (gap * S.abs(a)) * 0.5
    ar = S.abs(r1)
    odd = S.ne(m & 1, 0)
    move = (ar > half) | (S.eq(ar, half) & odd)
    sign = S.f_to_bits(q1) & 0x80000000
    qn = S.bits_to_f(sign | S.select(move, nb_m, m))
    qn = S.select(S.eq(r1, 0.0), q1, qn)
    ax, aa, aq1 = S.abs(x), S.abs(a), S.abs(q1)
    ok = (aa > 1e-18) & (aa < 1e18) & (ax > 1e-18) & (ax < 1e18) & (aq1 > 1e-25) & (aq1 < 1e25)
    first = S.select((aq1 <= 3.4e38) & (aa > 1e-18) & (aa < 1e18) & (ax < 1e18) & ((ax > 1e-18) | S.eq(x, 0.0)), q1, q0)
    return S.select(ok, qn, first)
