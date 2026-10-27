#!/usr/bin/env python3
"""A small compiler from Python-written compute kernels to SPIR-V (Vulkan 1.0, logical addressing), for Reno's GPU effects.

Kernels are written in Python instead of GLSL so the build needs only Python 3 (no glslang) and a kernel reads like the C++ of the CPU effect it
mirrors. tools/gpukernels.py holds the kernels; tools/build_shaders.sh runs it and writes src/ofx/vkshaders/<name>.spv.h, shaders/push_layouts.h
(the push-constant structs the C++ side fills) and the shader index. Only the generated files are needed to build Reno.

A kernel is a function taking a Shader S.  Values are V objects with a type: 'f' float, 'u' uint, 'i' int, 'b' bool, 'v4' vec4.
Python numbers are turned into constants of the other operand's type.  Operators: + - * / (and unary -), comparisons < <= > >=
and S.eq/S.ne, & | ~ (logical on bools, bitwise on integers), << >> on integers.  Control flow: "with S.if_(c):" (optionally
followed by "with S.else_():"), "with S.for_(a, b) as i:" (counted loop, i a uint/int V), mutable variables with S.var().
"""
import struct, sys

OP = dict(
    Capability=17, MemoryModel=14, EntryPoint=15, ExecutionMode=16, Decorate=71, MemberDecorate=72, ExtInstImport=11, ExtInst=12,
    TypeVoid=19, TypeBool=20, TypeInt=21, TypeFloat=22, TypeVector=23, TypeStruct=30, TypeRuntimeArray=29, TypeArray=28, ConstantComposite=44, ConstantNull=46, TypePointer=32,
    TypeFunction=33, Constant=43, ConstantTrue=41, ConstantFalse=42, Function=54, FunctionEnd=56, Variable=59, Load=61, Store=62,
    AccessChain=65, CompositeConstruct=80, CompositeExtract=81, Label=248, Return=253, Branch=249, BranchConditional=250,
    SelectionMerge=247, LoopMerge=246, Select=169, Bitcast=124, ConvertFToU=109, ConvertFToS=110, ConvertSToF=111, ConvertUToF=112,
    SNegate=126, FNegate=127, IAdd=128, FAdd=129, ISub=130, FSub=131, IMul=132, FMul=133, UDiv=134, SDiv=135, FDiv=136, UMod=137,
    SRem=138, FRem=140, FMod=141, VectorTimesScalar=142, IsNan=156, IsInf=157, LogicalEqual=164, LogicalNotEqual=165, LogicalOr=166,
    LogicalAnd=167, LogicalNot=168, IEqual=170, INotEqual=171, UGreaterThan=172, SGreaterThan=173, UGreaterThanEqual=174,
    SGreaterThanEqual=175, ULessThan=176, SLessThan=177, ULessThanEqual=178, SLessThanEqual=179, FOrdEqual=180, FUnordNotEqual=183,
    FOrdLessThan=184, FOrdGreaterThan=186, FOrdLessThanEqual=188, FOrdGreaterThanEqual=190, ShiftRightLogical=194,
    AtomicIAdd=234, ShiftRightArithmetic=195, ShiftLeftLogical=196, BitwiseOr=197, BitwiseXor=198, BitwiseAnd=199, Not=200)

GLSL = dict(Round=1, RoundEven=2, Trunc=3, FAbs=4, SAbs=5, FSign=6, Floor=8, Ceil=9, Fract=10, Sin=13, Cos=14, Tan=15, Asin=16, Acos=17,
            Atan=18, Atan2=25, Pow=26, Exp=27, Log=28, Exp2=29, Log2=30, Sqrt=31, InverseSqrt=32, FMin=37, UMin=38, SMin=39, FMax=40,
            UMax=41, SMax=42, FClamp=43, UClamp=44, SClamp=45, FMix=46, Fma=50, FindUMsb=75)


def pad_string(s):
    b = s.encode() + b"\0"
    while len(b) % 4:
        b += b"\0"
    return list(struct.unpack("<%dI" % (len(b) // 4), b))


class V:
    """A value in the kernel."""
    def __init__(self, S, id, ty):
        self.S, self.id, self.ty = S, id, ty

    def _bin(self, other, name, rev=False):
        return self.S._arith(name, self, other, rev)
    def __add__(self, o): return self._bin(o, "add")
    def __radd__(self, o): return self._bin(o, "add", True)
    def __sub__(self, o): return self._bin(o, "sub")
    def __rsub__(self, o): return self._bin(o, "sub", True)
    def __mul__(self, o): return self._bin(o, "mul")
    def __rmul__(self, o): return self._bin(o, "mul", True)
    def __truediv__(self, o): return self._bin(o, "div")
    def __rtruediv__(self, o): return self._bin(o, "div", True)
    def __mod__(self, o): return self._bin(o, "mod")
    def __neg__(self): return self.S._neg(self)
    def __lt__(self, o): return self.S._cmp("lt", self, o)
    def __le__(self, o): return self.S._cmp("le", self, o)
    def __gt__(self, o): return self.S._cmp("gt", self, o)
    def __ge__(self, o): return self.S._cmp("ge", self, o)
    def __and__(self, o): return self.S._logic("and", self, o)
    def __rand__(self, o): return self.S._logic("and", self, o)
    def __or__(self, o): return self.S._logic("or", self, o)
    def __ror__(self, o): return self.S._logic("or", self, o)
    def __xor__(self, o): return self.S._logic("xor", self, o)
    def __invert__(self): return self.S._not(self)
    def __lshift__(self, o): return self.S._shift("shl", self, o)
    def __rshift__(self, o): return self.S._shift("shr", self, o)
    def __bool__(self): raise TypeError("a kernel value cannot be used as a Python bool (use S.if_ / S.select)")
    __hash__ = object.__hash__
    def __getitem__(self, k): return self.S.extract(self, k)
    x = property(lambda s: s.S.extract(s, 0))
    y = property(lambda s: s.S.extract(s, 1))
    z = property(lambda s: s.S.extract(s, 2))
    w = property(lambda s: s.S.extract(s, 3))


class Var:
    """A mutable variable (a function-scope OpVariable)."""
    def __init__(self, S, ptr, ty):
        self.S, self.ptr, self.ty = S, ptr, ty
    def get(self): return self.S._load(self.ptr, self.ty)
    def set(self, v):
        v = self.S.conv(v, self.ty)
        self.S._emit("Store", self.ptr, v.id)
    def add(self, v): self.set(self.get() + v)


class ConstArray:
    def __init__(self, S, var, ptrty):
        self.S, self.var, self.ptrty = S, var, ptrty
    def load(self, idx):
        idx = self.S.conv(idx, 'u')
        p = self.S._op("AccessChain", self.ptrty, self.var, idx.id)
        return self.S._load(p, 'u')


class LocalArray:
    """a per-invocation array (Private storage, zero at the start): load(idx), store(idx, val)"""
    def __init__(self, S, var, ptrty, elem):
        self.S, self.var, self.ptrty, self.elem = S, var, ptrty, elem
    def load(self, idx):
        idx = self.S.conv(idx, 'u')
        p = self.S._op("AccessChain", self.ptrty, self.var, idx.id)
        return self.S._load(p, self.elem)
    def store(self, idx, val):
        idx = self.S.conv(idx, 'u')
        val = self.S.conv(val, self.elem)
        p = self.S._op("AccessChain", self.ptrty, self.var, idx.id)
        self.S._emit("Store", p, val.id)


class Buffer:
    def __init__(self, S, var, ptrty, elem):
        self.S, self.var, self.ptrty, self.elem = S, var, ptrty, elem
    def load(self, idx):
        idx = self.S.conv(idx, 'u')
        p = self.S._op("AccessChain", self.ptrty, self.var, self.S.u(0).id, idx.id)
        return self.S._load(p, self.elem)
    def store(self, idx, val):
        idx = self.S.conv(idx, 'u')
        val = self.S.conv(val, self.elem)
        p = self.S._op("AccessChain", self.ptrty, self.var, self.S.u(0).id, idx.id)
        self.S._emit("Store", p, val.id)
    def atomic_add(self, idx, val):
        """buf[idx] += val atomically (a 'u' buffer opened 'rw'); the old value is not returned"""
        S = self.S
        idx = S.conv(idx, 'u')
        val = S.conv(val, 'u')
        p = S._op("AccessChain", self.ptrty, self.var, S.u(0).id, idx.id)
        S._v("AtomicIAdd", 'u', p, S.u(1).id, S.u(0).id, val.id)


class _If:
    def __init__(self, S, cond):
        self.S, self.cond = S, cond
    def __enter__(self):
        S = self.S
        S._flush()
        then, els, merge = S._newid(), S._newid(), S._newid()
        self.st = dict(els=els, merge=merge)
        S._emit_raw("SelectionMerge", merge, 0)
        S._emit_raw("BranchConditional", self.cond.id, then, els)
        S._emit_raw("Label", then)
        return self
    def __exit__(self, *a):
        S = self.S
        S._flush()
        S._emit_raw("Branch", self.st["merge"])
        S.pending = self.st      # the else block (or its absence) is settled by whatever comes next


class _Else:
    def __init__(self, S): self.S = S
    def __enter__(self):
        S = self.S
        st = S.pending
        assert st is not None, "else_ without if_"
        S.pending = None
        self.st = st
        S._emit_raw("Label", st["els"])
        return self
    def __exit__(self, *a):
        S = self.S
        S._flush()
        S._emit_raw("Branch", self.st["merge"])
        S._emit_raw("Label", self.st["merge"])


class _For:
    def __init__(self, S, start, end, step, signed):
        self.S, self.start, self.end, self.step, self.signed = S, start, end, step, signed
    def __enter__(self):
        S = self.S
        S._flush()
        ty = 'i' if self.signed else 'u'
        self.counter = S.var(ty, self.start)
        header, cond, body, cont, merge = [S._newid() for _ in range(5)]
        self.ids = (header, cond, body, cont, merge)
        S._emit_raw("Branch", header)
        S._emit_raw("Label", header)
        S._emit_raw("LoopMerge", merge, cont, 0)
        S._emit_raw("Branch", cond)
        S._emit_raw("Label", cond)
        i = self.counter.get()
        c = S._cmp("lt", i, self.end) if self.step > 0 else S._cmp("gt", i, self.end)
        S._emit_raw("BranchConditional", c.id, body, merge)
        S._emit_raw("Label", body)
        self.i = self.counter.get()
        return self.i
    def __exit__(self, *a):
        S = self.S
        header, cond, body, cont, merge = self.ids
        S._flush()
        S._emit_raw("Branch", cont)
        S._emit_raw("Label", cont)
        self.counter.set(self.counter.get() + self.step)
        S._emit_raw("Branch", header)
        S._emit_raw("Label", merge)


class Shader:
    def __init__(self, name, push=(), bufs=(), local=(16, 16)):
        """push: [(name, 'u'|'i'|'f'), ...] in order (4 bytes each).
        bufs: [(binding, name, elem 'v4'|'f'|'u', 'r'|'w'|'rw'), ...]"""
        self.name, self.push_fields, self.local = name, list(push), local
        self.next = 1
        self.sec = {k: [] for k in ("cap", "ext", "mem", "entry", "mode", "annot", "types")}
        self.body = []           # after the entry label and variables
        self.varinsts = []
        self.pre = []
        self.cache = {}
        self.pending = None
        self.cur = self.body
        self.cap = 1
        self._emit_sec("cap", "Capability", 1)
        self.glsl = self._newid()
        self._emit_sec("ext", "ExtInstImport", self.glsl, pad_string("GLSL.std.450"))
        self._emit_sec("mem", "MemoryModel", 0, 1)
        self.tvoid = self._type("void", "TypeVoid")
        self.tid = {'u': self._type("u", "TypeInt", 32, 0), 'i': self._type("i", "TypeInt", 32, 1), 'f': self._type("f", "TypeFloat", 32),
                    'b': self._type("b", "TypeBool")}
        self.tid['v4'] = self._type("v4", "TypeVector", self.tid['f'], 4)
        self.tid['uv3'] = self._type("uv3", "TypeVector", self.tid['u'], 3)
        self.buffers = {}
        for binding, bname, elem, mode in bufs:
            self.buffers[bname] = self._make_buffer(binding, elem, mode)
        # push constants
        members = [self.tid[t] for _, t in self.push_fields]
        if members:
            pst = self._type(("pcst",) + tuple(members), "TypeStruct", *members)
            self._emit_sec("annot", "Decorate", pst, 2)
            for k in range(len(members)):
                self._emit_sec("annot", "MemberDecorate", pst, k, 35, 4 * k)
            self.pcvar = self._newid()
            self._emit_sec("types", "Variable", self._ptr(9, pst), self.pcvar, 9)
        # global invocation id
        self.gidvar = self._newid()
        self._emit_sec("types", "Variable", self._ptr(1, self.tid['uv3']), self.gidvar, 1)
        self._emit_sec("annot", "Decorate", self.gidvar, 11, 28)
        self.fnty = self._type("fn", "TypeFunction", self.tvoid)
        self.main = self._newid()
        self.entry_label = self._newid()
        self.p = _Push(self)
        self.pushvals = {}

    # ---- ids, instructions
    def _newid(self):
        n = self.next
        self.next += 1
        return n

    @staticmethod
    def _words(operands):
        w = []
        for o in operands:
            if isinstance(o, list): w += o
            else: w.append(o)
        return w

    def _emit_sec(self, sec, op, *operands):
        w = self._words(operands)
        self.sec[sec].append(((len(w) + 1) << 16 | OP[op], w))

    def _type(self, key, op, *operands):
        if key in self.cache: return self.cache[key]
        i = self._newid()
        self._emit_sec("types", op, i, *operands)
        self.cache[key] = i
        return i

    def _ptr(self, sc, t): return self._type(("ptr", sc, t), "TypePointer", sc, t)

    def _emit_raw(self, op, *operands):
        w = self._words(operands)
        self.cur.append(((len(w) + 1) << 16 | OP[op], w))

    def _flush(self):
        if self.pending is not None:
            st = self.pending
            self.pending = None
            self._emit_raw("Label", st["els"])
            self._emit_raw("Branch", st["merge"])
            self._emit_raw("Label", st["merge"])

    def _emit(self, op, *operands):
        self._flush()
        self._emit_raw(op, *operands)

    def _op(self, op, ty, *operands):
        self._flush()
        i = self._newid()
        self._emit_raw(op, ty, i, *operands)
        return i

    def _v(self, op, ty, *operands):
        return V(self, self._op(op, self.tid[ty], *operands), ty)

    def _load(self, ptr, ty):
        return V(self, self._op("Load", self.tid[ty], ptr), ty)

    # ---- types / constants
    def _const(self, ty, bits):
        key = ("c", ty, bits)
        if key in self.cache: return self.cache[key]
        i = self._newid()
        self._emit_sec("types", "Constant", self.tid[ty], i, bits)
        self.cache[key] = i
        return i
    def f(self, x): return V(self, self._const('f', struct.unpack("<I", struct.pack("<f", float(x)))[0]), 'f')
    def u(self, x): return V(self, self._const('u', int(x) & 0xffffffff), 'u')
    def i(self, x): return V(self, self._const('i', int(x) & 0xffffffff), 'i')
    def b(self, x):
        key = ("cb", bool(x))
        if key not in self.cache:
            i = self._newid()
            self._emit_sec("types", "ConstantTrue" if x else "ConstantFalse", self.tid['b'], i)
            self.cache[key] = i
        return V(self, self.cache[key], 'b')

    def conv(self, x, ty):
        """A Python number or a V as a V of type ty."""
        if isinstance(x, V):
            if x.ty != ty:
                raise TypeError("kernel %s: %s value where %s is needed" % (self.name, x.ty, ty))
            return x
        if isinstance(x, bool): return self.b(x)
        if ty == 'f': return self.f(x)
        if ty == 'u': return self.u(x)
        if ty == 'i': return self.i(x)
        if ty == 'b': return self.b(x)
        raise TypeError("cannot make a %s from %r" % (ty, x))

    def vec4(self, a, b, c, d):
        parts = [self.conv(t, 'f') for t in (a, b, c, d)]
        return self._v("CompositeConstruct", 'v4', *[p.id for p in parts])

    def splat4(self, a):
        a = self.conv(a, 'f')
        return self._v("CompositeConstruct", 'v4', a.id, a.id, a.id, a.id)

    def extract(self, v, k):
        assert v.ty == 'v4'
        return self._v("CompositeExtract", 'f', v.id, k)

    # ---- operators
    def _arith(self, name, a, b, rev):
        # a is the V (self), b the other operand as given for (a op b); with rev: other op self
        v, o = a, b
        ty = v.ty if isinstance(v, V) else None
        if isinstance(o, V) and ty == 'v4' and o.ty == 'f':
            if name == "mul" and not rev:
                r = self._v("VectorTimesScalar", 'v4', v.id, o.id)
                self._emit_sec("annot", "Decorate", r.id, 42)
                return r
            ov = self.splat4(o)
            o = ov
        elif isinstance(o, V) and ty == 'f' and o.ty == 'v4':
            v = self.splat4(v)
            ty = 'v4'
        elif ty == 'v4' and not isinstance(o, V):
            o = self.splat4(o)
        else:
            o = self.conv(o, ty)
        x, y = (o, v) if rev else (v, o)
        if ty in ('f', 'v4'):
            op = dict(add="FAdd", sub="FSub", mul="FMul", div="FDiv", mod="FMod")[name]
        elif ty == 'u':
            op = dict(add="IAdd", sub="ISub", mul="IMul", div="UDiv", mod="UMod")[name]
        elif ty == 'i':
            op = dict(add="IAdd", sub="ISub", mul="IMul", div="SDiv", mod="SRem")[name]
        else:
            raise TypeError("arithmetic on %s" % ty)
        if op == "FDiv" and ty == 'f':
            return self._fdiv_exact(x, y)
        if op == "FDiv" and ty == 'v4':
            return self.vec4(*[self._fdiv_exact(self.extract(x, k), self.extract(y, k)) for k in range(4)])
        r = self._v(op, ty, x.id, y.id)
        if op in ("FAdd", "FSub", "FMul"):
            self._emit_sec("annot", "Decorate", r.id, 42)      # NoContraction: a GPU must not fuse a multiply and an add (the CPU rounds each step)
        return r

    def _nc(self, v):
        self._emit_sec("annot", "Decorate", v.id, 42)
        return v

    def _fdiv_exact(self, x, a):
        """x / a rounded the way the CPU's division is (IEEE, to nearest). The hardware divide may be a few ulp off (the Vulkan rule), so the quotient is
        corrected with its exact remainder, worked out with Dekker's error-free product (no fused multiply-add: a software Vulkan driver does not fuse it).
        Where that does not apply (zero, huge or tiny operands, non-finite) the plain divide is the answer."""
        def op(name, p, q):
            return self._nc(self._v(name, 'f', p.id, q.id))
        q0 = op("FDiv", x, a)
        y = op("FDiv", self.f(1.0), a)
        split = self.f(4097.0)
        def parts(v):
            t = op("FMul", split, v)
            hi = op("FSub", t, op("FSub", t, v))
            return hi, op("FSub", v, hi)
        ah, al = parts(a)
        qh, ql = parts(q0)
        p = op("FMul", q0, a)
        # q0 * a = p + e exactly
        e = op("FAdd", op("FAdd", op("FAdd", op("FSub", op("FMul", qh, ah), p), op("FMul", qh, al)), op("FMul", ql, ah)), op("FMul", ql, al))
        # remainder x - q0 * a = (x - p) - e (x - p is exact when q0 is within an ulp or two)
        r = op("FSub", op("FSub", x, p), e)
        q2 = op("FAdd", q0, op("FMul", r, y))
        ax, aa = self.abs(x), self.abs(a)
        ok = self.isfinite(q2) & (aa > 1e-18) & (aa < 1e18) & (ax < 1e18) & ((ax > 1e-18) | self.eq(x, 0.0))
        return self.select(ok, q2, q0)

    def fma(self, a, b, c):
        return self._ext("Fma", 'f', self.conv(a, 'f'), self.conv(b, 'f'), self.conv(c, 'f'))

    def _neg(self, v):
        if v.ty in ('f', 'v4'): return self._v("FNegate", v.ty, v.id)
        return self._v("SNegate", v.ty, v.id)

    def _cmp(self, name, a, b):
        if not isinstance(a, V):
            a, b = b, a
            name = dict(lt="gt", gt="lt", le="ge", ge="le", eq="eq", ne="ne")[name]
        b = self.conv(b, a.ty)
        t = a.ty
        if t == 'f': op = dict(lt="FOrdLessThan", le="FOrdLessThanEqual", gt="FOrdGreaterThan", ge="FOrdGreaterThanEqual", eq="FOrdEqual", ne="FUnordNotEqual")[name]
        elif t == 'u': op = dict(lt="ULessThan", le="ULessThanEqual", gt="UGreaterThan", ge="UGreaterThanEqual", eq="IEqual", ne="INotEqual")[name]
        elif t == 'i': op = dict(lt="SLessThan", le="SLessThanEqual", gt="SGreaterThan", ge="SGreaterThanEqual", eq="IEqual", ne="INotEqual")[name]
        elif t == 'b': op = dict(eq="LogicalEqual", ne="LogicalNotEqual")[name]
        else: raise TypeError("comparison of %s" % t)
        return self._v(op, 'b', a.id, b.id)
    def eq(self, a, b): return self._cmp("eq", a, b)
    def ne(self, a, b): return self._cmp("ne", a, b)

    def _logic(self, name, a, b):
        b = self.conv(b, a.ty)
        if a.ty == 'b': op = dict(and_="LogicalAnd", or_="LogicalOr", xor_="LogicalNotEqual")[name + "_"]
        else: op = dict(and_="BitwiseAnd", or_="BitwiseOr", xor_="BitwiseXor")[name + "_"]
        return self._v(op, a.ty, a.id, b.id)
    def _not(self, a):
        return self._v("LogicalNot" if a.ty == 'b' else "Not", a.ty, a.id)
    def _shift(self, name, a, b):
        b = self.conv(b, 'u') if not isinstance(b, V) else b
        op = "ShiftLeftLogical" if name == "shl" else ("ShiftRightLogical" if a.ty == 'u' else "ShiftRightArithmetic")
        return self._v(op, a.ty, a.id, b.id)

    # ---- functions
    def _ext(self, name, ty, *args):
        return self._v("ExtInst", ty, self.glsl, GLSL[name], *[a.id for a in args])
    def _f1(self, name, x):
        if not isinstance(x, V): x = self.f(x)
        return self._ext(name, x.ty, x)
    def sqrt_hw(self, x): return self._f1("Sqrt", x)       # the driver's own Sqrt (Vulkan allows it to be a few ulp off); only for special values
    def sqrt(self, x):
        """sqrtf, correctly rounded: worked out in the software double (sf64), which is exact; the hardware Sqrt answers zero, negative, non-finite and denormal operands"""
        if not isinstance(x, V): x = self.f(x)
        if x.ty != 'f': return self._f1("Sqrt", x)
        from sf64 import SF
        ex = SF.from_f32(self, x).sqrt().to_f32()
        return self.select((x >= 1.1754944e-38) & (x < 3.0e38), ex, self.sqrt_hw(x))
    def floor(self, x): return self._f1("Floor", x)
    def ceil(self, x): return self._f1("Ceil", x)
    def fract(self, x): return self._f1("Fract", x)
    def trunc(self, x): return self._f1("Trunc", x)
    def atan2_f(self, y, x): return self._ext("Atan2", 'f', self.conv(y, 'f'), self.conv(x, 'f'))      # the driver's own (only a starting guess for sf64)
    def find_umsb(self, x): return self._ext("FindUMsb", 'u', x)      # (the index of the highest set bit; every bit of -1 for 0)
    def round_even(self, x): return self._f1("RoundEven", x)       # (lrintf)
    # The elementary functions are worked out in the software double (sf64) and rounded to a float, not by the driver's Exp/Log/Sin/Cos/Atan2/Pow (Vulkan
    # allows those several ulp of error, and they differ from one GPU to the next). The result is the double function rounded to a float, which is what a
    # C library's float function gives but for a very rare last bit.
    def _sf1(self, fn, x):
        import sf64
        x = self.conv(x, 'f') if not isinstance(x, V) else x
        return getattr(sf64, fn)(self, sf64.SF.from_f32(self, x)).to_f32()
    def exp(self, x): return self._sf1("sf_exp", x)
    def log(self, x): return self._sf1("sf_log", x)
    def sin(self, x): return self._sf1("sf_sin", x)
    def cos(self, x): return self._sf1("sf_cos", x)
    def atan2(self, y, x):
        import sf64
        y, x = self.conv(y, 'f'), self.conv(x, 'f')
        return sf64.sf_atan2(self, sf64.SF.from_f32(self, y), sf64.SF.from_f32(self, x)).to_f32()
    def pow(self, x, y):
        import sf64
        x, y = self.conv(x, 'f'), self.conv(y, 'f')
        return sf64.sf_pow(self, sf64.SF.from_f32(self, x), sf64.SF.from_f32(self, y)).to_f32()
    def fmod(self, x, y):
        """C's fmod (the sign of x), exact: the remainder x - q y of the truncated quotient is worked out with Dekker's error-free product (the hardware
        FRem may use x - y trunc(x / y), which rounds). Operands whose quotient is 2^23 or more, zero, or non-finite use the hardware FRem."""
        x, y = self.conv(x, 'f'), self.conv(y, 'f')
        def op(name, p, q): return self._nc(self._v(name, 'f', p.id, q.id))
        ax, ay = self.abs(x), self.abs(y)
        q = self.trunc(self._fdiv_exact(ax, ay))
        split = self.f(4097.0)
        def parts(v):
            t = op("FMul", split, v)
            hi = op("FSub", t, op("FSub", t, v))
            return hi, op("FSub", v, hi)
        qh, ql = parts(q)
        yh, yl = parts(ay)
        p = op("FMul", q, ay)
        e = op("FAdd", op("FAdd", op("FAdd", op("FSub", op("FMul", qh, yh), p), op("FMul", qh, yl)), op("FMul", ql, yh)), op("FMul", ql, yl))
        r = op("FSub", op("FSub", ax, p), e)                      # ax - q ay, exactly (q is at most one too big or one too small)
        r = self.select(r < 0.0, op("FAdd", r, ay), r)
        r = self.select(r >= ay, op("FSub", r, ay), r)
        res = self.select(x < 0.0, self._v("FNegate", 'f', r.id), r)
        ok = (ay > 1e-18) & (ay < 1e18) & (ax < 8388608.0 * ay) & (ax < 1e18)
        return self.select(ok, res, self._v("FRem", 'f', x.id, y.id))
    def abs(self, x):
        if not isinstance(x, V): x = self.f(x)
        return self._ext("SAbs" if x.ty == 'i' else "FAbs", x.ty, x)
    def _mm(self, f, u, s, a, b):
        if not isinstance(a, V): a, b = b, a
        b = self.conv(b, a.ty) if a.ty != 'v4' else (b if isinstance(b, V) else self.splat4(b))
        nm = f if a.ty in ('f', 'v4') else (u if a.ty == 'u' else s)
        return self._ext(nm, a.ty, a, b)
    def min(self, a, b): return self._mm("FMin", "UMin", "SMin", a, b)
    def max(self, a, b): return self._mm("FMax", "UMax", "SMax", a, b)
    def clamp(self, x, lo, hi):
        if x.ty == 'f': return self._ext("FClamp", 'f', x, self.conv(lo, 'f'), self.conv(hi, 'f'))
        return self.min(self.max(x, lo), hi)
    def mix(self, a, b, t):
        return self._ext("FMix", a.ty, a, self.conv(b, a.ty) if a.ty != 'v4' else b, self.conv(t, 'f') if a.ty != 'v4' else (t if t.ty == 'v4' else self.splat4(t)))
    def select(self, c, a, b):
        """c ? a : b (a scalar condition; vec4 operands too)"""
        if not isinstance(a, V) and not isinstance(b, V): raise TypeError("select needs a kernel value")
        ty = a.ty if isinstance(a, V) else b.ty
        a, b = self.conv(a, ty), self.conv(b, ty)
        if ty == 'v4':
            bv4 = self._type(("bv4",), "TypeVector", self.tid['b'], 4)
            cv = self._op("CompositeConstruct", bv4, c.id, c.id, c.id, c.id)
            return self._v("Select", 'v4', cv, a.id, b.id)
        return self._v("Select", ty, c.id, a.id, b.id)
    def isnan(self, x): return self._v("IsNan", 'b', x.id)
    def isinf(self, x): return self._v("IsInf", 'b', x.id)
    def isfinite(self, x): return ~(self.isnan(x) | self.isinf(x))
    def to_f(self, x):
        if x.ty == 'f': return x
        return self._v("ConvertUToF" if x.ty == 'u' else "ConvertSToF", 'f', x.id)
    def to_u(self, x):
        if x.ty == 'u': return x
        if x.ty == 'i': return self._v("Bitcast", 'u', x.id)
        return self._v("ConvertFToU", 'u', x.id)
    def to_i(self, x):
        if x.ty == 'i': return x
        if x.ty == 'u': return self._v("Bitcast", 'i', x.id)
        return self._v("ConvertFToS", 'i', x.id)
    def bits_to_f(self, x): return self._v("Bitcast", 'f', x.id)
    def f_to_bits(self, x): return self._v("Bitcast", 'u', x.id)

    # ---- variables, buffers, control flow
    def var(self, ty, init=None):
        ptr = self._newid()
        self.varinsts.append(((4 << 16) | OP["Variable"], [self._ptr(7, self.tid[ty]), ptr, 7]))
        v = Var(self, ptr, ty)
        if init is not None: v.set(init)
        return v
    def _make_buffer(self, binding, elem, mode):
        et = self.tid[elem] if elem != 'v4' else self.tid['v4']
        arr = self._type(("arr", et), "TypeRuntimeArray", et)
        self._emit_sec("annot", "Decorate", arr, 6, 16 if elem == 'v4' else 4)
        st = self._type(("st", arr), "TypeStruct", arr)
        self._emit_sec("annot", "Decorate", st, 3)
        self._emit_sec("annot", "MemberDecorate", st, 0, 35, 0)
        if mode == 'r': self._emit_sec("annot", "MemberDecorate", st, 0, 24)
        elif mode == 'w': self._emit_sec("annot", "MemberDecorate", st, 0, 25)
        var = self._newid()
        self._emit_sec("types", "Variable", self._ptr(2, st), var, 2)
        self._emit_sec("annot", "Decorate", var, 34, 0)
        self._emit_sec("annot", "Decorate", var, 33, binding)
        return Buffer(self, var, self._ptr(2, et), elem)

    def loop(self, n, fn):
        """runs fn(i) for i = 0 .. n - 1 (n a number or a uint value) as a loop in the kernel"""
        with self.for_(0, n) as i:
            fn(i)

    def const_array(self, words):
        """a constant array of uint words in the kernel (private memory); returns an object with load(index)"""
        words = [int(w) & 0xffffffff for w in words]
        key = ("carr",) + tuple(words)
        if key in self.cache: return self.cache[key]
        n = self._const('u', len(words))
        arr = self._type(("arrty", len(words)), "TypeArray", self.tid['u'], n)
        elems = [self._const('u', w) for w in words]
        init = self._newid()
        self._emit_sec("types", "ConstantComposite", arr, init, *elems)
        var = self._newid()
        self._emit_sec("types", "Variable", self._ptr(6, arr), var, 6, init)
        obj = ConstArray(self, var, self._ptr(6, self.tid['u']))
        self.cache[key] = obj
        return obj
    def local_array(self, n, ty='f'):
        """an array of n values (ty 'f', 'u' or 'i') private to the invocation, all zero at the start"""
        key = ("larr", n, ty)
        cnt = self._const('u', n)
        arr = self._type(("arrty", n, ty), "TypeArray", self.tid[ty], cnt)
        null = self._newid()
        self._emit_sec("types", "ConstantNull", arr, null)
        var = self._newid()
        self._emit_sec("types", "Variable", self._ptr(6, arr), var, 6, null)
        # (a private variable is a separate one each time it is asked for)
        return LocalArray(self, var, self._ptr(6, self.tid[ty]), ty)
    def buf(self, name): return self.buffers[name]
    def if_(self, c): return _If(self, c)
    def else_(self): return _Else(self)
    def for_(self, start, end, step=1, signed=False):
        return _For(self, start, end, step, signed)

    def in_range(self, w, h):
        """Everything inside runs for the invocations x < w, y < h."""
        c = self._cmp("lt", self.gx, w) & self._cmp("lt", self.gy, h)
        return self.if_(c)

    # ---- assemble
    def begin(self):
        self.cur = self.body
        gv = self._load(self.gidvar, 'uv3')
        self.gx = self._v("CompositeExtract", 'u', gv.id, 0)
        self.gy = self._v("CompositeExtract", 'u', gv.id, 1)
        # push constants are all read at the start
        for k, (n, t) in enumerate(self.push_fields):
            p = self._op("AccessChain", self._ptr(9, self.tid[t]), self.pcvar, self._const('u', k))
            self.pushvals[n] = self._load(p, t)

    def finish(self):
        self._flush()
        self._emit_raw("Return")
        out = [0x07230203, 0x00010000, 0, 0, 0]
        def put(lst):
            for head, words in lst:
                out.append(head)
                out.extend(words)
        ep = self._words([5, self.main, pad_string("main"), self.gidvar])
        for key in ("cap", "ext", "mem"):
            put(self.sec[key])
        put([(((len(ep) + 1) << 16) | OP["EntryPoint"], ep)])
        put([((6 << 16) | OP["ExecutionMode"], [self.main, 17, self.local[0], self.local[1], 1])])
        put(self.sec["annot"])
        put(self.sec["types"])
        put([((5 << 16) | OP["Function"], [self.tvoid, self.main, 0, self.fnty]), ((2 << 16) | OP["Label"], [self.entry_label])])
        put(self.varinsts)
        put(self.body)
        out.append((1 << 16) | OP["FunctionEnd"])
        out[3] = self.next
        return out


class _Push:
    def __init__(self, S): object.__setattr__(self, "S", S)
    def __getattr__(self, name):
        S = object.__getattribute__(self, "S")
        if name not in S.pushvals: raise AttributeError("kernel %s has no push constant %s" % (S.name, name))
        return S.pushvals[name]


def compile_kernel(name, push, bufs, body, local=(16, 16)):
    S = Shader(name, push, bufs, local)
    S.begin()
    body(S)
    return S, S.finish()


def c_array(name, words):
    lines = ["// Generated by tools/gpukernels.py (tools/spvdsl.py): do not edit.", "static const uint32_t spv_%s[] = {" % name]
    for i in range(0, len(words), 8):
        lines.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",")
    lines.append("};")
    return "\n".join(lines) + "\n"
