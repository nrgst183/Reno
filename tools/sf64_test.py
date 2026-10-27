"""Checks tools/sf64.py against the machine's doubles (numpy) with a numpy emulation of the kernel DSL's integer operations: python3 tools/sf64_test.py"""
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sf64
from sf64 import SF

M32 = 0xffffffff


class EV:
    def __init__(self, a, ty):
        self.a, self.ty = a, ty

    def _o(self, o):
        if isinstance(o, EV):
            return o.a
        if self.ty == 'u':
            return np.uint32(o & M32)
        if self.ty == 'i':
            return np.int32(o)
        return np.float32(o)

    def _wrap(self, x):
        if self.ty == 'u':
            return EV((np.asarray(x, dtype=np.int64) & M32).astype(np.uint32), 'u')
        if self.ty == 'i':
            return EV((((np.asarray(x, dtype=np.int64) + 2**31) & M32) - 2**31).astype(np.int32), 'i')
        return EV(np.asarray(x, dtype=np.float32), 'f')

    def _i64(self, o):
        return np.asarray(self.a, dtype=np.int64), np.asarray(self._o(o), dtype=np.int64)

    def __add__(self, o): x, y = self._i64(o); return self._wrap(x + y)
    __radd__ = __add__
    def __sub__(self, o): x, y = self._i64(o); return self._wrap(x - y)
    def __rsub__(self, o): x, y = self._i64(o); return self._wrap(y - x)
    def __mul__(self, o): x, y = self._i64(o); return self._wrap((x * y) & M32 if self.ty == 'u' else x * y)
    __rmul__ = __mul__
    def __and__(self, o):
        if self.ty == 'b': return EV(self.a & o.a, 'b')
        return self._wrap(np.asarray(self.a, dtype=np.int64) & np.asarray(self._o(o), dtype=np.int64))
    def __or__(self, o):
        if self.ty == 'b': return EV(self.a | o.a, 'b')
        return self._wrap(np.asarray(self.a, dtype=np.int64) | np.asarray(self._o(o), dtype=np.int64))
    def __xor__(self, o):
        return self._wrap(np.asarray(self.a, dtype=np.int64) ^ np.asarray(self._o(o), dtype=np.int64))
    def __invert__(self):
        if self.ty == 'b': return EV(~self.a, 'b')
        return self._wrap(~np.asarray(self.a, dtype=np.int64))
    def __lshift__(self, o):
        n = np.asarray(o.a if isinstance(o, EV) else o, dtype=np.int64)
        assert (n >= 0).all() and (n < 32).all(), "shift out of range"
        return self._wrap(np.asarray(self.a, dtype=np.int64) << n)
    def __rshift__(self, o):
        n = np.asarray(o.a if isinstance(o, EV) else o, dtype=np.int64)
        assert (n >= 0).all() and (n < 32).all(), "shift out of range"
        return self._wrap(np.asarray(self.a, dtype=np.int64) >> n)
    def _c(self, o, f): return EV(f(self.a, self._o(o)), 'b')
    def __lt__(self, o): return self._c(o, np.less)
    def __le__(self, o): return self._c(o, np.less_equal)
    def __gt__(self, o): return self._c(o, np.greater)
    def __ge__(self, o): return self._c(o, np.greater_equal)
    __hash__ = object.__hash__


class EVar:
    def __init__(self, v): self.v = v
    def get(self): return EV(self.v.a.copy() if hasattr(self.v.a, 'copy') else self.v.a, self.v.ty)
    def set(self, x):
        a = x.a
        if np.ndim(a) > 0 and np.ndim(self.v.a) == 0: pass
        self.v = EV(np.array(a, copy=True), x.ty)


class EArr:
    def __init__(self, words): self.w = np.array(words, dtype=np.uint32)
    def load(self, idx):
        i = idx.a if isinstance(idx, EV) else idx
        return EV(self.w[np.asarray(i, dtype=np.int64)], 'u')


class ES:
    def var(self, ty, init=0):
        if isinstance(init, EV): return EVar(init)
        return EVar(self.u(init) if ty == 'u' else self.i(init))
    def loop(self, n, fn):
        for k in range(n): fn(EV(np.uint32(k), 'u'))
    def const_array(self, words): return EArr(words)
    def u(self, c): return EV(np.uint32(c & M32), 'u')
    def i(self, c): return EV(np.int32(c), 'i')
    def eq(self, a, b):
        return EV(a.a == (b.a if isinstance(b, EV) else a._o(b)), 'b')
    def ne(self, a, b):
        return EV(a.a != (b.a if isinstance(b, EV) else a._o(b)), 'b')
    def select(self, c, a, b):
        a = a if isinstance(a, EV) else None
        return EV(np.where(c.a, a.a, b.a), a.ty)
    def find_umsb(self, x):
        v = np.asarray(x.a, dtype=np.uint64)
        r = np.zeros(v.shape, dtype=np.int64) - 1
        for k in range(32):
            r = np.where((v >> np.uint64(k)) & np.uint64(1) == 1, k, r)
        return EV((r & M32).astype(np.uint32), 'u')
    def atan2_f(self, y, x): return EV(np.arctan2(y.a, x.a).astype(np.float32), 'f')
    def f_to_bits(self, f): return EV(np.asarray(f.a, dtype=np.float32).view(np.uint32), 'u')
    def bits_to_f(self, u): return EV(np.asarray(u.a, dtype=np.uint32).view(np.float32), 'f')
    def to_i(self, u): return EV(np.asarray(u.a).astype(np.uint32).view(np.int32) if u.ty == 'u' else u.a, 'i')
    def to_u(self, i): return EV(np.asarray(i.a).astype(np.int32).view(np.uint32) if i.ty == 'i' else i.a, 'u')


S = ES()
rng = np.random.default_rng(12345)


def mk(d):
    b = np.asarray(d, dtype=np.float64).view(np.uint64)
    return SF(S, EV((b >> np.uint64(32)).astype(np.uint32), 'u'), EV((b & np.uint64(M32)).astype(np.uint32), 'u'))


def val(x):
    b = (x.hi.a.astype(np.uint64) << np.uint64(32)) | x.lo.a.astype(np.uint64)
    return b.view(np.float64)


def sample(n):
    out = []
    # random mantissas, exponents spread but not extreme
    m = rng.random(n) + 0.5
    e = rng.integers(-40, 40, n)
    s = rng.choice([-1.0, 1.0], n)
    out.append(s * m * np.exp2(e))
    # near-ties and integers, small ranges
    out.append(rng.integers(-1000, 1000, n).astype(np.float64) + rng.choice([0.0, 0.5, 0.25, 1e-9], n))
    out.append(rng.random(n) * 255)
    out.append(np.float64(np.float32(rng.random(n))) * rng.choice([1, 255, 1000], n))
    return np.concatenate(out)


def check(name, got, want, a=None, b=None):
    got = np.asarray(got); want = np.asarray(want)
    ok = (got.view(np.uint64) == want.view(np.uint64)) | ((got == 0) & (want == 0))
    if not ok.all():
        bad = np.where(~ok)[0][:5]
        print("FAIL", name, (~ok).sum(), "of", ok.size)
        for j in bad:
            print("   a=%r b=%r got=%r want=%r" % (a[j] if a is not None else None, b[j] if b is not None else None, got[j], want[j]))
        return False
    print("ok  ", name, ok.size)
    return True


def main():
    n = 60000
    A, B = sample(n), sample(n)[::-1].copy()
    # pairs that cancel
    B[: n // 4] = A[: n // 4] * (1 + rng.random(n // 4) * 1e-9)
    B[n // 4: n // 2] = -A[n // 4: n // 2] * (1 + rng.random(n // 4) * 1e-3)
    A = np.concatenate([A, [0.0, 1.0, 3.0, 255.0, 0.5, 1e-3]]); B = np.concatenate([B, [0.0, 3.0, 3.0, 255.0, 0.5, 1e-3]])
    a, b = mk(A), mk(B)
    allok = True
    with np.errstate(all='ignore'):
        allok &= check("add", val(a + b), A + B, A, B)
        allok &= check("sub", val(a - b), A - B, A, B)
        allok &= check("mul", val(a * b), A * B, A, B)
        nz = B != 0
        Bn = np.where(nz, B, 1.0)
        allok &= check("div", val(a / mk(Bn)), A / Bn, A, Bn)
        P = np.abs(A)
        allok &= check("sqrt", val(mk(P).sqrt()), np.sqrt(P), P, P)
        allok &= check("lt", np.where((a < b).a, 1.0, 0.0), np.where(A < B, 1.0, 0.0))
        allok &= check("ge", np.where((a >= b).a, 1.0, 0.0), np.where(A >= B, 1.0, 0.0))
        allok &= check("f32", val(mk(np.float64(np.float32(A * 0 + 1)))) if False else np.asarray(a.to_f32().a, dtype=np.float64), np.float64(np.float32(A)), A, A)
        F = np.float32(np.concatenate([rng.random(20000) * 300 - 50, np.exp2(rng.integers(-30, 30, 20000)) * (rng.random(20000) + 0.5)]))
        allok &= check("from_f32", val(SF.from_f32(S, EV(F, 'f'))), np.float64(F), F, F)
        I = rng.integers(-2**31 + 1, 2**31 - 1, 40000).astype(np.int32)
        I[:1000] = rng.integers(-300, 300, 1000)
        allok &= check("from_i32", val(SF.from_i32(S, EV(I, 'i'))), np.float64(I), I, I)
        T = np.concatenate([A[np.abs(A) < 2e9], np.array([0.0, -0.5, 0.5, -1.0, 1.0, -1.5, 2.999999999])])
        t = mk(T)
        allok &= check("trunc_i32", np.float64(t.trunc_i32().a), np.float64(np.trunc(T)), T, T)
        allok &= check("floor_i32", np.float64(t.floor_i32().a), np.float64(np.floor(T)), T, T)
        allok &= check("trunc", val(t.trunc()), np.trunc(T), T, T)
        allok &= check("floor", val(t.floor()), np.floor(T), T, T)
    print("ALL OK" if allok else "FAILURES")
    return 0 if allok else 1


def ulps(got, want):
    return np.abs(got.view(np.int64) - want.view(np.int64)).astype(np.float64)


def fn_tests():
    n = 20000
    ok = True
    with np.errstate(all='ignore'):
        def rep(name, got, want, extra=None):
            u = ulps(np.asarray(got), np.asarray(want))
            rel = np.abs((got - want) / np.where(want == 0, 1, want))
            print("%-8s max %6.1f ulp  mean %.3f  max rel %.2e  >2ulp: %d of %d" % (name, u.max(), u.mean(), rel.max(), (u > 2).sum(), u.size))
            return rel.max() < 1e-14
        X = rng.random(n) * 40 - 20
        ok &= rep("exp", val(sf64.sf_exp(S, mk(X))), np.exp(X))
        X = rng.random(n) * 1400 - 700
        ok &= rep("exp big", val(sf64.sf_exp(S, mk(X))), np.exp(X))
        X = np.exp(rng.random(n) * 80 - 40)
        ok &= rep("log", val(sf64.sf_log(S, mk(X))), np.log(X))
        X = rng.random(n) + 1e-3; Y = rng.random(n) * 30
        ok &= rep("pow", val(sf64.sf_pow(S, mk(X), mk(Y))), np.power(X, Y))
        X = rng.random(n) * 40 - 20
        s, c = sf64.sf_sincos(S, mk(X))
        ok &= rep("sin", val(s), np.sin(X))
        ok &= rep("cos", val(c), np.cos(X))
        X = rng.random(n) * 4000 - 2000
        ok &= rep("sin big", val(sf64.sf_sin(S, mk(X))), np.sin(X))
        Y = rng.random(n) * 2000 - 1000; X = rng.random(n) * 2000 - 1000
        got = val(sf64.sf_atan2(S, mk(Y), mk(X)))
        want = np.arctan2(Y, X)
        u = np.abs(got - want)
        print("atan2    max abs %.2e" % u.max())
        ok &= u.max() < 1e-14
    return ok


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "fn":
        sys.exit(0 if fn_tests() else 1)
    sys.exit(main())
