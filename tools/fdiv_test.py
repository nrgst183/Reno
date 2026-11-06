"""Checks tools/fdiv_exact.py (the kernels' float division) with the numpy emulation of tools/rmath_gpu_test.py: with a card divide that is wrong by up to 2.5 ulp (the
Vulkan allowance) in the quotient and in the reciprocal, every result must still be numpy's IEEE division bit for bit, while the older one-step correction is not.
    python3 tools/fdiv_test.py [count]"""
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rmath_gpu_test import S, E
from fdiv_exact import fdiv_exact

np.seterr(all='ignore')


def sloppy(rng):
    """a divide that is off by up to 2 ulp (quotient) and 2^-21 relative (the reciprocal)"""
    def hw(x, a):
        q = (x.a / a.a).astype(np.float32)
        k = rng.integers(-2, 3, q.shape)
        bits = q.view(np.int32).astype(np.int64)
        sgn = np.where(q < 0, -1, 1)
        q = (bits + k * sgn).astype(np.int32).view(np.float32)
        if np.all(x.a == 1.0):                      # the reciprocal: relative error up to ~2^-21
            q = (q * (1.0 + (rng.random(q.shape) * 2 - 1) * 2.0 ** -21)).astype(np.float32)
        return E(q, 'f')
    return hw


def old(S, x, a, hw):
    """the one-step correction this replaces"""
    q0 = hw(x, a)
    y = hw(S.f(1.0), a)
    split = S.f(4097.0)
    def parts(v):
        t = split * v
        hi = t - (t - v)
        return hi, v - hi
    ah, al = parts(a); qh, ql = parts(q0)
    p = q0 * a
    e = (((qh * ah - p) + qh * al) + ql * ah) + ql * al
    r = (x - p) - e
    return q0 + r * y


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 2000000
    rng = np.random.default_rng(5)
    hw = sloppy(rng)
    ex = rng.integers(-14, 14, n); ea = rng.integers(-14, 14, n)
    x = ((1 + rng.random(n)) * np.exp2(ex) * np.where(rng.random(n) < 0.5, -1, 1)).astype(np.float32)
    a = ((1 + rng.random(n)) * np.exp2(ea) * np.where(rng.random(n) < 0.5, -1, 1)).astype(np.float32)
    # near ties: x = a (q + half an ulp of q) rounded to a float
    q = ((1 + rng.random(n)) * np.exp2(rng.integers(-8, 8, n))).astype(np.float32)
    u = np.spacing(q)
    xt = ((a.astype(np.float64)) * (q.astype(np.float64) + u.astype(np.float64) * 0.5)).astype(np.float32)
    x[: n // 2] = xt[: n // 2]
    # exact quotients and powers of two
    x[n // 2: n // 2 + 1000] = a[n // 2: n // 2 + 1000] * np.float32(4.0)
    a[n // 2 + 1000: n // 2 + 2000] = np.float32(1.0)
    ref = (x / a).astype(np.float32)
    got = fdiv_exact(S, E(x, 'f'), E(a, 'f'), hw).a
    bad = (got.view(np.uint32) != ref.view(np.uint32))
    print('fdiv_exact: %d divisions, %d differ' % (n, bad.sum()))
    for i in np.nonzero(bad)[0][:5]:
        print('   %r / %r got %r want %r' % (x[i], a[i], got[i], ref[i]))
    o = old(S, E(x, 'f'), E(a, 'f'), hw).a
    print('the older one-step correction: %d differ' % (o.view(np.uint32) != ref.view(np.uint32)).sum())
    return 1 if bad.any() else 0


if __name__ == '__main__':
    sys.exit(main())
