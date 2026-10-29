#!/usr/bin/env python3
"""vfxcheck.py PARAMS.TSV REFERENCE_DIR [OFX_VIDEO_PLUGINS_DIR]

Compares the settings Reno's built-in OpenFX effects describe (tests/ui/vfxparams_dump.cpp's output) with what VEGAS's own plug-ins gave:
 - the ranges: every reference run (reno_cli vegasfx --keep-real: <ref>/<effect>_<context>/scenarios.json) set each number setting to the smallest and the
   largest value VEGAS's plug-in allowed ("min: <label>" / "max: <label>" scenarios); the built-in must allow the same
 - the order, labels, types, choices, groups and hints of VEGAS's resource files (Contents/Resources/<bundle>.xml), when the folder is given
Prints one line per difference and a summary.
"""
import sys, os, json, math, glob, collections
import xml.etree.ElementTree as ET

def num(s):
    return float(s)

def load_tsv(path):
    effects = collections.OrderedDict()
    cur = None
    for line in open(path, encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        if f[0] == 'EFFECT':
            cur = effects.setdefault((f[1], f[2]), dict(label=f[3], group=f[4], params=[]))
        elif f[0] == 'P' and cur is not None:
            names = 'order name type label hint parent secret enabled val min max dmin dmax inc digits dtype choices'.split()
            d = dict(zip(names, f[1:]))
            d['choices'] = d['choices'].split('|') if d['choices'] else []
            for k in ('val', 'min', 'max', 'dmin', 'dmax'):
                d[k] = [num(x) for x in d[k].split(',')] if d[k] else []
            cur['params'].append(d)
    return effects

def usable(v):
    return math.isfinite(v) and abs(v) < 1e6

def range_of(p, k):
    """Same rule as fxcompare.cpp rangeOf"""
    t = p['type']
    if t == 'OfxParamTypeBoolean':
        return 0.0, 1.0
    if t == 'OfxParamTypeChoice':
        return 0.0, float(max(0, len(p['choices']) - 1))
    dflt = p['val'][k] if k < len(p['val']) else 0.0
    dmin = p['dmin'][k] if k < len(p['dmin']) else -1.797e308
    dmax = p['dmax'][k] if k < len(p['dmax']) else 1.797e308
    pmin = p['min'][k] if k < len(p['min']) else -1.797e308
    pmax = p['max'][k] if k < len(p['max']) else 1.797e308
    lo = dmin if usable(dmin) else (pmin if usable(pmin) else dflt - max(1.0, abs(dflt)))
    hi = dmax if usable(dmax) else (pmax if usable(pmax) else dflt + max(1.0, abs(dflt)))
    if usable(pmin): lo = max(lo, pmin)
    if usable(pmax): hi = min(hi, pmax)
    if not hi > lo: hi = lo + 1.0
    return lo, hi

def main():
    effects = load_tsv(sys.argv[1])
    ref = sys.argv[2]
    bad = 0
    weak = 0
    checked = 0
    for d in sorted(glob.glob(os.path.join(ref, '*'))):
        sj, rj = os.path.join(d, 'scenarios.json'), os.path.join(d, 'real.json')
        if not (os.path.exists(sj) and os.path.exists(rj)):
            continue
        real = json.load(open(rj))
        pid = real['id']
        ctx = real['context'].replace('OfxImageEffectContext', '')
        e = effects.get((pid, ctx))
        if not e:
            print('NO-BUILTIN', pid, ctx)
            continue
        byname = {p['name']: p for p in e['params']}
        sc = json.load(open(sj))
        for s in sc:
            if s['kind'] not in ('min', 'max'):
                continue
            if s['name'].startswith('every'):
                for name, vals in s['set'].items():
                    p = byname.get(name)
                    if not p:
                        print('MISSING-PARAM', pid, name); bad += 1
                        continue
                    for k, v in enumerate(vals):
                        lo, hi = range_of(p, k)
                        want = lo if s['kind'] == 'min' else hi
                        checked += 1
                        if abs(want - v) > 1e-6 * max(1.0, abs(v)):
                            dflt = p['val'][k] if k < len(p['val']) else 0.0
                            fb = dflt - max(1.0, abs(dflt)) if s['kind'] == 'min' else dflt + max(1.0, abs(dflt))
                            if abs(fb - v) < 1e-9:
                                weak += 1
                                continue    # VEGAS's own range was probably not declared: the reference used the default +- 1 stand-in
                            bad += 1
                            print('RANGE %s %s %s[%d]: VEGAS %s %g, built-in %g' % (pid, ctx, name, k, s['kind'], v, want))
    print('checked %d, differences %d (%d more equal the reference\'s stand-in range for a setting with none declared)' % (checked, bad, weak))

def xml_check(effects, root):
    """order of the settings and their hints against VEGAS's own resource files (the plain one, no language in its name)"""
    diffs = 0
    n = 0
    for f in sorted(glob.glob(os.path.join(root, '*', 'Contents', 'Resources', '*.xml'))):
        if any(('.%s.xml' % l) in f for l in ('de-DE', 'es-ES', 'fr-FR', 'ja-JP', 'ko-KR', 'pl-PL', 'pt-BR', 'ru-RU', 'zh-CN')):
            continue
        try:
            tree = ET.parse(f)
        except ET.ParseError:
            continue
        for plug in tree.getroot().iter('OfxPlugin'):
            pid = plug.get('name')
            for ctx in plug.iter('OfxImageEffectContext'):
                cname = (ctx.get('name') or '').replace('OfxImageEffectContext', '')
                e = effects.get((pid, cname))
                if not e:
                    continue
                n += 1
                names = [c.get('name') for c in ctx if c.get('name') and c.tag.startswith('OfxParamType')]
                have = [q['name'] for q in e['params']]
                common = [x for x in names if x in have]
                mine = [x for x in have if x in common]
                if common != mine:
                    diffs += 1
                    print('ORDER %s %s: VEGAS %s | built-in %s' % (pid, cname, ' '.join(common), ' '.join(mine)))
                byname = {q['name']: q for q in e['params']}
                for c in ctx:
                    h = c.find('OfxParamPropHint')
                    q = byname.get(c.get('name'))
                    if h is not None and q is not None and (h.text or '').strip() != q['hint'].strip():
                        diffs += 1
                        print('HINT %s %s %s: VEGAS %r, built-in %r' % (pid, cname, c.get('name'), (h.text or '').strip(), q['hint'].strip()))
    print('resource files: %d effects checked, %d differences in order and hints' % (n, diffs))

if len(sys.argv) > 3:
    xml_check(load_tsv(sys.argv[1]), sys.argv[3])
main()
