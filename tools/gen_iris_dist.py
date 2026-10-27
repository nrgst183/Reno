#!/usr/bin/env python3
"""Turns irisDistance() of src/ofx/builtins_trans.cpp (generated from VEGAS's code: float registers and gotos that only go forward) into the kernel
function in tools/iris_dist_gen.py. Run it again if that C++ function ever changes: python3 tools/gen_iris_dist.py"""
import re, os, sys

here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, '..', 'src', 'ofx', 'builtins_trans.cpp')).read()
a = src.index('float irisDistance(')
b = src.index('\n}\n', a)
body = src[a:b]
start = body.index('switch (shape) {')
switch_end = body.index('}', start)
head = body[:start]
sw = body[start:switch_end]
rest = body[switch_end + 1:]

cases = {}
for m in re.finditer(r'case (\d+): goto (\w+);', sw):
    cases[int(m.group(1))] = m.group(2)
assert len(cases) == 14

# statements of the part after the switch, with labels
stmts = []     # (label or None, text)
for line in rest.split('\n'):
    line = line.strip()
    if not line or line == ';':
        continue
    label = None
    m = re.match(r'(\w+):(.*)$', line)
    if m and not line.startswith('case'):
        label, line = m.group(1), m.group(2).strip()
    parts = [p.strip() for p in re.split(r';\s*', line) if p.strip()]
    # "if (...) goto L;" stays one statement: split on ';' is fine since the goto is inside the same piece
    if not parts:
        stmts.append((label, ''))
        continue
    for k, p in enumerate(parts):
        stmts.append((label if k == 0 else None, p))

# blocks: a new block starts at every label
blocks = []     # list of (label, [stmts])
for label, text in stmts:
    if label is not None or not blocks:
        blocks.append([label, []])
    if text:
        blocks[-1][1].append(text)
blocks = [bl for bl in blocks if bl[0] != 'Lend']
names = [bl[0] for bl in blocks]
ids = {n: i for i, n in enumerate(names) if n}
END = len(blocks)
ids['Lend'] = END
assert names[0] is not None or True

out = []
def emit(ind, s): out.append('    ' * ind + s)

def reg(r, part): return '%s_%s' % (r, part)

def expr_f(tok):
    tok = tok.strip()
    m = re.fullmatch(r'(x\d)\.f', tok)
    if m: return reg(m.group(1), 'f') + '.get()'
    if tok == 'A': return 'A'
    m = re.fullmatch(r'(-?[0-9.]+)f', tok)
    if m: return 'S.f(%s)' % m.group(1)
    raise ValueError(tok)

def stmt(text, ind):
    """a plain statement"""
    m = re.fullmatch(r'(x\d)\.f = (x\d\.f|A|-?[0-9.]+f) ([-+*]) (x\d\.f|A|-?[0-9.]+f)', text)
    if m:
        emit(ind, '%s.set(%s %s %s)' % (reg(m.group(1), 'f'), expr_f(m.group(2)), m.group(3), expr_f(m.group(4))))
        return
    m = re.fullmatch(r'(x\d)\.f = -(x\d)\.f', text)
    if m:
        emit(ind, '%s.set(S.bits_to_f(S.f_to_bits(%s.get()) ^ S.u(0x80000000)))' % (reg(m.group(1), 'f'), reg(m.group(2), 'f')))
        return
    m = re.fullmatch(r'(x\d)\.f = sqrtf\((x\d)\.f\)', text)
    if m:
        emit(ind, '%s.set(fsqrt(S, %s.get()))' % (reg(m.group(1), 'f'), reg(m.group(2), 'f')))
        return
    m = re.fullmatch(r'(x\d)\.f = (-?[0-9.]+f)', text)
    if m:
        emit(ind, '%s.set(S.f(%s))' % (reg(m.group(1), 'f'), m.group(2)[:-1]))
        return
    m = re.fullmatch(r'(x\d)\.d = (-?[0-9.]+)', text)
    if m:
        emit(ind, '%s.set(S.f(%s))' % (reg(m.group(1), 'd'), m.group(2)))
        return
    m = re.fullmatch(r'(x\d)\.d = \(double\)(x\d)\.f', text)
    if m:
        emit(ind, '%s.set(%s.get())' % (reg(m.group(1), 'd'), reg(m.group(2), 'f')))
        return
    m = re.fullmatch(r'(x\d)\.d = std::fabs\((x\d)\.d\)', text)
    if m:
        emit(ind, '%s.set(S.abs(%s.get()))' % (reg(m.group(1), 'd'), reg(m.group(2), 'd')))
        return
    m = re.fullmatch(r'(x\d)\.f = \(float\)(x\d)\.d', text)
    if m:
        emit(ind, '%s.set(%s.get())' % (reg(m.group(1), 'f'), reg(m.group(2), 'd')))
        return
    m = re.fullmatch(r'(x\d) = (x\d)', text)
    if m:
        for part in 'fd':
            emit(ind, '%s.set(%s.get())' % (reg(m.group(1), part), reg(m.group(2), part)))
        return
    m = re.fullmatch(r'cmpl_ = (x\d)\.f', text)
    if m:
        emit(ind, 'cmpl.set(%s.get())' % reg(m.group(1), 'f'))
        return
    m = re.fullmatch(r'cmpr_ = (x\d)\.f', text)
    if m:
        emit(ind, 'cmpr.set(%s.get())' % reg(m.group(1), 'f'))
        return
    raise ValueError('statement: ' + text)

def goto(label, ind):
    emit(ind, 'pc.set(S.u(%d))' % ids[label])

def block_body(stmts_, idx, ind):
    """statements of block idx from the list; ends with the fall through to the next block unless a goto ends it"""
    if not stmts_:
        emit(ind, 'pc.set(S.u(%d))' % (idx + 1))
        return
    text = stmts_[0]
    m = re.fullmatch(r'goto (\w+)', text)
    if m:
        goto(m.group(1), ind)
        return
    m = re.fullmatch(r'if \((.*?)\) goto (\w+)', text)
    if m:
        cond, lab = m.group(1), m.group(2)
        cm = re.fullmatch(r'cmpl_ (<=|<|>=|>) cmpr_', cond)
        if cond == '1':
            goto(lab, ind)
            return
        assert cm, cond
        emit(ind, 'with S.if_(cmpl.get() %s cmpr.get()):' % cm.group(1))
        goto(lab, ind + 1)
        emit(ind, 'with S.else_():')
        block_body(stmts_[1:], idx, ind + 1)
        return
    stmt(text, ind)
    block_body(stmts_[1:], idx, ind)

emit(0, '# generated by tools/gen_iris_dist.py from irisDistance() in src/ofx/builtins_trans.cpp: do not edit by hand')
emit(0, 'from gpukernels import fsqrt')
emit(0, '')
emit(0, '')
emit(0, 'def iris_distance(S, ci, ei, cx, cy, shape, A, sx, sy):')
emit(1, '"""irisDistance(): ci, ei the pixel as floats, shape a uint value; every register is a float variable (the .d halves hold floats too: they only ever carry converted floats)"""')
for r in range(7):
    emit(1, 'x%d_f = S.var("f")' % r)
    emit(1, 'x%d_d = S.var("f")' % r)
    emit(1, 'x%d_f.set(S.f(0.0)); x%d_d.set(S.f(0.0))' % (r, r))
emit(1, 'cmpl = S.var("f"); cmpr = S.var("f")')
emit(1, 'cmpl.set(S.f(0.0)); cmpr.set(S.f(0.0))')
emit(1, 'x0_f.set(sy); x1_f.set(sx); x2_f.set(cx); x3_f.set(cy); x6_f.set(S.f(1.0))')
emit(1, 'x5_f.set(ci)')
emit(1, 'x4_f.set(ei)')
for t in ['x5.f = x5.f - x2.f', 'x4.f = x4.f - x3.f', 'x5.f = x5.f * x1.f', 'x4.f = x4.f * x0.f']:
    stmt(t, 1)
emit(1, 'pc = S.var("u")')
# the switch: block of each shape's label
emit(1, 'pc.set(S.u(%d))' % END)
for sh in range(14):
    emit(1, 'with S.if_(S.eq(shape, %d)):' % sh)
    emit(2, 'pc.set(S.u(%d))' % ids[cases[sh]])
# the blocks in an order in which every jump goes forward (a few jump back to a shared tail in the source)
def targets(i):
    t = []
    for text in blocks[i][1]:
        m = re.fullmatch(r'(?:if \(.*?\) )?goto (\w+)', text)
        if m:
            t.append(ids[m.group(1)])
            if text.startswith('goto') or text.startswith('if (1)'):
                return t
    t.append(i + 1)
    return t
order, seen, state = [], {}, {}
def visit(i):
    if i >= END or state.get(i) == 2: return
    assert state.get(i) != 1, 'a loop in irisDistance'
    state[i] = 1
    for j in targets(i): visit(j)
    state[i] = 2
    order.append(i)
for i in range(END): visit(i)
order.reverse()
for i in order:
    label, st = blocks[i]
    emit(1, 'with S.if_(S.eq(pc.get(), %d)):' % i)
    block_body(st, i, 2)
emit(1, 'return x6_f.get()')
text = '\n'.join(out)
open(os.path.join(here, 'iris_dist_gen.py'), 'w').write(text + '\n')
print('blocks', len(blocks), 'lines', len(out))
