#!/usr/bin/env python3
"""Reads the dialog templates (English, 1033) and the string tables out of VEGAS's audio plug-in DLLs, as data.
Used by dlgcheck.py; run on its own it prints the dialogs of the DLL(s) given.
PE reading is done here (no pefile needed)."""
import struct, sys

class PE:
    def __init__(s, path):
        d = s.d = open(path, 'rb').read()
        o = struct.unpack_from('<I', d, 0x3c)[0]
        s.nsec = struct.unpack_from('<H', d, o + 6)[0]
        osz = struct.unpack_from('<H', d, o + 20)[0]
        opt = o + 24
        s.dirs = [struct.unpack_from('<II', d, opt + 112 + 8 * i) for i in range(16)]
        s.secs = []
        so = opt + osz
        for i in range(s.nsec):
            n = d[so:so + 8].rstrip(b'\0').decode()
            vs, va, rs, ra = struct.unpack_from('<IIII', d, so + 8)
            s.secs.append((n, va, vs, ra, rs))
            so += 40

    def off(s, rva):
        for n, va, vs, ra, rs in s.secs:
            if va <= rva < va + max(vs, rs):
                return rva - va + ra

    def res(s):
        rva = s.dirs[2][0]
        b = s.off(rva)
        d = s.d
        out = []
        def name(e):
            if e & 0x80000000:
                p = b + (e & 0x7fffffff)
                n = struct.unpack_from('<H', d, p)[0]
                return d[p + 2:p + 2 + 2 * n].decode('utf-16le')
            return e
        def walk(p, path):
            nn, ni = struct.unpack_from('<HH', d, p + 12)
            for i in range(nn + ni):
                e, t = struct.unpack_from('<II', d, p + 16 + 8 * i)
                if t & 0x80000000:
                    walk(b + (t & 0x7fffffff), path + [name(e)])
                else:
                    drva, sz = struct.unpack_from('<II', d, b + t)
                    out.append((path + [name(e)], d[s.off(drva):s.off(drva) + sz]))
        walk(b, [])
        return out

def _sz(d, p):
    w = struct.unpack_from('<H', d, p)[0]
    if w == 0xffff:
        return struct.unpack_from('<H', d, p + 2)[0], p + 4
    e = p
    while struct.unpack_from('<H', d, e)[0]:
        e += 2
    return d[p:e].decode('utf-16le'), e + 2

CLS = {0x80: 'Button', 0x81: 'Edit', 0x82: 'Static', 0x83: 'ListBox', 0x84: 'ScrollBar', 0x85: 'ComboBox'}

def parse_dialog(d):
    dv, sig = struct.unpack_from('<HH', d, 0)
    assert sig == 0xffff
    help_, ex, style, n, x, y, cx, cy = struct.unpack_from('<IIIHhhhh', d, 4)
    p = 26
    menu, p = _sz(d, p)
    cls, p = _sz(d, p)
    title, p = _sz(d, p)
    if style & 0x40:
        p += 6
        font, p = _sz(d, p)
    ctrls = []
    for i in range(n):
        p = (p + 3) & ~3
        hid, ex, st, x, y, cx, cy, id_ = struct.unpack_from('<IIIhhhhI', d, p)
        p += 24
        c, p = _sz(d, p)
        t, p = _sz(d, p)
        ext = struct.unpack_from('<H', d, p)[0]
        p += 2 + ext
        if id_ == 0xffffffff:
            id_ = -1
        ctrls.append(dict(id=id_, cls=CLS.get(c, c), x=x, y=y, w=cx, h=cy, style=st, text=t))
    return dict(w=cx if False else struct.unpack_from('<hh', d, 22)[0], h=struct.unpack_from('<hh', d, 22)[1], title=title, ctrls=ctrls)

def load(path):
    """dialogs {id: dialog}, strings {id: text}"""
    pe = PE(path)
    dlgs, strs = {}, {}
    for path_, data in pe.res():
        if len(path_) < 3 or path_[2] != 1033:
            continue
        if path_[0] == 5:
            try:
                dlgs[path_[1]] = parse_dialog(data)
            except Exception as e:
                pass
        elif path_[0] == 6:
            base = (path_[1] - 1) * 16
            q = 0
            for i in range(16):
                n = struct.unpack_from('<H', data, q)[0]
                s = data[q + 2:q + 2 + 2 * n].decode('utf-16le')
                q += 2 + 2 * n
                if s:
                    strs[base + i] = s
    return dlgs, strs

def kind(c):
    """what a dialog control is: label, check, radio, push, group, edit, combo, slider, fader, spinner, graph, other"""
    st = c['style']
    cl = c['cls']
    if cl == 'Static':
        if c['w'] > 0 and c['h'] <= 1:
            return 'line'
        return 'label'
    if cl == 'Button':
        t = st & 0xf
        return {0: 'push', 1: 'push', 2: 'check', 3: 'check', 4: 'radio', 9: 'radio', 7: 'group', 5: 'check', 6: 'check'}.get(t, 'push')
    if cl == 'Edit':
        return 'edit'
    if cl == 'ComboBox':
        return 'combo'
    if isinstance(cl, str):
        l = cl.lower()
        for k in ('trackbar', 'fader', 'spinner', 'envelope', 'graph', 'meter', 'logo', 'static', 'knob', 'display', 'button'):
            if k in l:
                return {'trackbar': 'slider'}.get(k, k)
    return 'other'

if __name__ == '__main__':
    for f in sys.argv[1:]:
        dl, st = load(f)
        for k, v in sorted(dl.items()):
            print('dialog', k, v['w'], v['h'])
            for c in v['ctrls']:
                print('  ', c['id'], kind(c), c['cls'], c['x'], c['y'], c['w'], c['h'], repr(c['text']))
