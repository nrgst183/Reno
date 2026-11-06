"""Checks the kernel code of tools/rmath_gpu.py with a numpy emulation of the kernel DSL, against the C library, bit for bit:
    python3 tools/rmath_gpu_test.py [count]        (needs g++ for the small C program that calls the library; 'count' random inputs per function, default 200000)
Floats are numpy float32 (IEEE, round to nearest even, no fusing), so the emulation does exactly the kernel's arithmetic."""
import sys, os, subprocess, tempfile
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

M32 = 0xffffffff
np.seterr(all='ignore')


class E:
    def __init__(self, a, ty):
        self.ty = ty
        dt = {'f': np.float32, 'u': np.uint32, 'i': np.int32, 'b': np.bool_}[ty]
        self.a = np.asarray(a, dtype=dt)

    def _o(self, o):
        if isinstance(o, E):
            return o.a
        if self.ty == 'u': return np.uint32(o & M32)
        if self.ty == 'i': return np.int32(o)
        return np.float32(o)

    def _w(self, x): return E(x, self.ty)
    def __add__(self, o): return self._w(self.a + self._o(o))
    __radd__ = __add__
    def __sub__(self, o): return self._w(self.a - self._o(o))
    def __rsub__(self, o): return self._w(self._o(o) - self.a)
    def __mul__(self, o): return self._w(self.a * self._o(o))
    __rmul__ = __mul__
    def __truediv__(self, o): return self._w(self.a / self._o(o))
    def __rtruediv__(self, o): return self._w(self._o(o) / self.a)
    def __neg__(self): return self._w(-self.a)
    def __and__(self, o):
        if self.ty == 'b': return E(self.a & o.a, 'b')
        return self._w(self.a & self._o(o))
    def __or__(self, o):
        if self.ty == 'b': return E(self.a | o.a, 'b')
        return self._w(self.a | self._o(o))
    def __xor__(self, o): return self._w(self.a ^ self._o(o))
    def __invert__(self):
        if self.ty == 'b': return E(~self.a, 'b')
        return self._w(~self.a)
    def __lshift__(self, o):
        n = o.a if isinstance(o, E) else o
        assert (np.asarray(n) >= 0).all() and (np.asarray(n) < 32).all(), "shift out of range"
        return self._w(self.a << np.asarray(n).astype(self.a.dtype))
    def __rshift__(self, o):
        n = o.a if isinstance(o, E) else o
        assert (np.asarray(n) >= 0).all() and (np.asarray(n) < 32).all(), "shift out of range"
        return self._w(self.a >> np.asarray(n).astype(self.a.dtype))
    def _c(self, o, f): return E(f(self.a, self._o(o)), 'b')
    def __lt__(self, o): return self._c(o, np.less)
    def __le__(self, o): return self._c(o, np.less_equal)
    def __gt__(self, o): return self._c(o, np.greater)
    def __ge__(self, o): return self._c(o, np.greater_equal)
    __hash__ = object.__hash__


class EVar:
    def __init__(self, v): self.v = v
    def get(self): return E(self.v.a.copy(), self.v.ty)
    def set(self, x): self.v = E(x.a.copy(), x.ty)


class EArr:
    def __init__(self, words): self.w = np.array(words, dtype=np.uint32)
    def load(self, idx): return E(self.w[np.asarray(idx.a if isinstance(idx, E) else idx, dtype=np.int64)], 'u')


class ES:
    def var(self, ty, init=0):
        if isinstance(init, E): return EVar(init)
        return EVar(self.u(init) if ty == 'u' else self.i(init))
    def loop(self, n, fn):
        for k in range(n): fn(E(np.uint32(k), 'u'))
    def const_array(self, words): return EArr(words)
    def u(self, c): return E(np.uint32(c & M32), 'u')
    def i(self, c): return E(np.int32(c), 'i')
    def f(self, c): return E(np.float32(c), 'f')
    def eq(self, a, b): return E(a.a == (b.a if isinstance(b, E) else a._o(b)), 'b')
    def ne(self, a, b): return E(a.a != (b.a if isinstance(b, E) else a._o(b)), 'b')
    def select(self, c, a, b):
        ref = a if isinstance(a, E) else b
        a = a if isinstance(a, E) else E(ref._o(a), ref.ty)
        b = b if isinstance(b, E) else E(ref._o(b), ref.ty)
        return E(np.where(c.a, a.a, b.a), a.ty)
    def abs(self, x): return E(np.abs(x.a), x.ty)
    def min(self, a, b): return E(np.minimum(a.a, a._o(b)), a.ty)
    def max(self, a, b): return E(np.maximum(a.a, a._o(b)), a.ty)
    def isnan(self, x): return E(np.isnan(x.a), 'b')
    def sqrt(self, x): return E(np.sqrt(x.a), 'f')
    def find_umsb(self, x):
        v = np.asarray(x.a, dtype=np.uint64)
        r = np.zeros(v.shape, dtype=np.int64) - 1
        for k in range(32):
            r = np.where((v >> np.uint64(k)) & np.uint64(1) == 1, k, r)
        return E((r & M32).astype(np.uint32), 'u')
    def f_to_bits(self, f): return E(np.asarray(f.a, dtype=np.float32).view(np.uint32), 'u')
    def bits_to_f(self, u): return E(np.asarray(u.a, dtype=np.uint32).view(np.float32), 'f')
    def to_i(self, u): return E(np.asarray(u.a).astype(np.uint32).view(np.int32) if u.ty == 'u' else u.a, 'i')
    def to_u(self, i): return E(np.asarray(i.a).astype(np.int32).view(np.uint32) if i.ty == 'i' else i.a, 'u')
    def to_f(self, x): return E(x.a.astype(np.float32), 'f')


S = ES()


def DENORM(ref):
    """results below 2^-126 (denormal floats): a card may flush them to zero, the kernels do not promise them"""
    return (np.abs(ref) < np.float32(1.1754944e-38)) & (ref != 0)

CSRC = r'''
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main(int argc, char** argv) {
    // usage: dump <fn> <in> <out> [in2]: float inputs (and second inputs), the library's float results
    const char* fn = argv[1];
    FILE* f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f) / 4; fseek(f, 0, SEEK_SET);
    float* x = (float*)malloc(n * 4); if (fread(x, 4, n, f) != (size_t)n) return 2; fclose(f);
    float* y = nullptr;
    if (argc > 4) { FILE* g = fopen(argv[4], "rb"); y = (float*)malloc(n * 4); if (fread(y, 4, n, g) != (size_t)n) return 2; fclose(g); }
    float* r = (float*)malloc(n * 4);
    for (long i = 0; i < n; ++i) {
        volatile float v = x[i]; volatile float w = y ? y[i] : 0.f;
        if (!strcmp(fn, "sinf")) r[i] = sinf(v); else if (!strcmp(fn, "cosf")) r[i] = cosf(v); else if (!strcmp(fn, "atanf")) r[i] = atanf(v);
        else if (!strcmp(fn, "asinf")) r[i] = asinf(v); else if (!strcmp(fn, "atan2f")) r[i] = atan2f(v, w); else if (!strcmp(fn, "logf")) r[i] = logf(v);
        else if (!strcmp(fn, "expf")) r[i] = expf(v); else if (!strcmp(fn, "powf")) r[i] = powf(v, w); else return 3;
    }
    FILE* o = fopen(argv[3], "wb"); fwrite(r, 4, n, o); fclose(o);
    return 0;
}
'''


def build_dump(d):
    src = os.path.join(d, 'dump.cpp'); exe = os.path.join(d, 'dump')
    open(src, 'w').write(CSRC)
    subprocess.check_call(['g++', '-O1', '-fno-builtin', src, '-o', exe, '-lm'])
    return exe


def lib(exe, d, fn, x, y=None):
    xi, ro = os.path.join(d, 'x.bin'), os.path.join(d, 'r.bin')
    x.astype(np.float32).tofile(xi)
    args = [exe, fn, xi, ro]
    if y is not None:
        yi = os.path.join(d, 'y.bin'); y.astype(np.float32).tofile(yi); args.append(yi)
    subprocess.check_call(args)
    return np.fromfile(ro, dtype=np.float32)


def samples(rng, n, kind):
    bits = rng.integers(0, 2**32, n, dtype=np.uint64).astype(np.uint32)
    if kind == 'trig':          # up to +-120 (the kernels' range): mostly small magnitudes, plus the quadrant borders
        v = (rng.random(n) * 2 - 1) * np.where(rng.random(n) < 0.5, 8.0, 120.0)
        v[: n // 10] = rng.choice([0.75, 1.5707964, 3.1415927, 4.712389, 6.2831855, 0.785398], n // 10) * (1 + (rng.random(n // 10) - 0.5) * 1e-6)
        return v.astype(np.float32)
    if kind == 'asin':
        return ((rng.random(n) * 2 - 1) * 1.0002).astype(np.float32)
    if kind == 'atan':
        e = rng.integers(-40, 40, n)
        return ((rng.random(n) * 2 - 1) * np.exp2(e.astype(np.float64))).astype(np.float32)
    if kind == 'log':
        e = rng.integers(-30, 30, n)
        v = (rng.random(n) * 2) * np.exp2(e.astype(np.float64))
        v[: n // 20] = bits[: n // 20].view(np.float32)
        v[n // 20: n // 10] = 0.0
        return v.astype(np.float32)
    return bits.view(np.float32)


def main():
    import rmath_gpu as G
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200000
    rng = np.random.default_rng(99)
    d = tempfile.mkdtemp()
    exe = build_dump(d)
    bad = 0
    cases = [('sinf', G.sin_f, 'trig'), ('cosf', G.cos_f, 'trig'), ('atanf', G.atan_f, 'atan'), ('asinf', G.asin_f, 'asin'), ('logf', G.log_f, 'log')]
    for name, fn, kind in cases:
        x = samples(rng, n, kind)
        got = fn(S, E(x, 'f')).a
        ref = lib(exe, d, name, x)
        nan_ok = np.isnan(got) & np.isnan(ref)
        diff = (got.view(np.uint32) != ref.view(np.uint32)) & ~nan_ok
        print('%s: %d inputs, %d differ' % (name, n, diff.sum()))
        for i in np.nonzero(diff)[0][:5]:
            print('   x=%r got %r lib %r' % (x[i], got[i], ref[i]))
        bad += int(diff.sum())
    # atan2f: pairs
    x = samples(rng, n, 'atan'); y = samples(rng, n, 'atan')
    x[: n // 20] = 0.0; y[n // 20: n // 10] = 0.0; x[n // 10: n // 8] = 0.0; y[n // 10: n // 8] = 0.0; x[n // 8: n // 7] = np.float32(1.0); x[n // 7: n // 6] = -0.0
    got = G.atan2_f(S, E(y, 'f'), E(x, 'f')).a
    ref = lib(exe, d, 'atan2f', y, x)
    nan_ok = np.isnan(got) & np.isnan(ref)
    diff = (got.view(np.uint32) != ref.view(np.uint32)) & ~nan_ok
    print('atan2f: %d inputs, %d differ' % (n, diff.sum()))
    for i in np.nonzero(diff)[0][:5]:
        print('   y=%r x=%r got %r lib %r' % (y[i], x[i], got[i], ref[i]))
    bad += int(diff.sum())
    # expf: the whole range (the results below 2^-126 are denormal floats) and the special values
    x = np.concatenate([((rng.random(n) * 2 - 1) * np.where(rng.random(n) < 0.7, 12.0, 105.0)).astype(np.float32), rng.integers(0, 2**32, n // 10, dtype=np.uint64).astype(np.uint32).view(np.float32),
                        np.array([0.0, -0.0, np.inf, -np.inf, np.nan, 88.72283, 88.7228317, 88.8, -87.3, -103.9, -104.0, -110, 1.0, -1.0], dtype=np.float32)])
    got = G.exp_f(S, E(x, 'f')).a
    ref = lib(exe, d, 'expf', x)
    nan_ok = np.isnan(got) & np.isnan(ref)
    diff = (got.view(np.uint32) != ref.view(np.uint32)) & ~nan_ok & ~DENORM(ref)
    print('expf: %d inputs, %d differ' % (len(x), diff.sum()))
    for i in np.nonzero(diff)[0][:5]:
        print('   x=%r got %r lib %r' % (x[i], got[i], ref[i]))
    bad += int(diff.sum())
    # powf: bases around the ranges effects use (0..8, mostly), exponents small and large, signs, and every special value in pairs
    m = n
    bx = np.where(rng.random(m) < 0.8, rng.random(m) * 8, np.exp2(rng.integers(-130, 128, m).astype(np.float64)) * rng.random(m)).astype(np.float32)
    by = np.where(rng.random(m) < 0.6, rng.random(m) * 6 - 1, (rng.random(m) * 2 - 1) * np.where(rng.random(m) < 0.5, 40.0, 4.0)).astype(np.float32)
    sp = np.array([0.0, -0.0, 1.0, -1.0, 2.0, -2.0, 0.5, -0.5, 3.0, np.inf, -np.inf, np.nan, 1e-45, -1e-45, 1e-39, 3e38, 1e30, 0.1, 2.5, -2.5], dtype=np.float32)
    px, py = np.meshgrid(sp, sp)
    bx = np.concatenate([bx, px.ravel()]); by = np.concatenate([by, py.ravel()])
    neg = rng.random(len(bx)) < 0.05
    bx[: m][neg[: m]] *= -1
    got = G.pow_f(S, E(bx, 'f'), E(by, 'f')).a
    ref = lib(exe, d, 'powf', bx, by)
    nan_ok = np.isnan(got) & np.isnan(ref)
    diff = (got.view(np.uint32) != ref.view(np.uint32)) & ~nan_ok & ~DENORM(ref)
    print('powf: %d inputs, %d differ' % (len(bx), diff.sum()))
    for i in np.nonzero(diff)[0][:8]:
        print('   x=%r y=%r got %r lib %r' % (bx[i], by[i], got[i], ref[i]))
    bad += int(diff.sum())
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
