#!/usr/bin/env python3
"""dlgcheck.py FXDLG.TXT [AUDIO_X64_FOLDER]

Compares every built-in audio effect's page (the dump tests/ui/fxdlg_dump.cpp writes) with the dialog template of the VEGAS DirectX plug-in it
reproduces (the English dialogs of the DLLs in "Shared Plug-Ins/Audio_x64"; read by dlgparse.py).  The dialog for a page is the one whose captions
overlap most.  Per page it lists
  - captions the dialog has and the page does not (and the page's captions the dialog does not have), mnemonics (&) included
  - the number of each kind of control: slider/fader, edit, combo box, check box, radio button, push button, graph
  - the order of the captions (reading order: top to bottom, then left to right)
  - size and proportions of the page (dialog units are about 1.75 px wide and 1.9 px high at the size the pages are built for)
Texts VEGAS fills in at run time (values, "Some Length") are skipped.
"""
import sys, os, re, glob, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dlgparse

def read_dump(path):
    pages = collections.OrderedDict()
    cur = None
    for line in open(path, encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        if f[0] == 'PAGE':
            cur = pages.setdefault(f[1], dict(name=f[2], w=int(f[3]), h=int(f[4]), params=[], presets=[], widgets=[]))
        elif cur is None:
            continue
        elif f[0] == 'PARAM':
            cur['params'].append(dict(i=int(f[1]), name=f[2], lo=float(f[3]), hi=float(f[4]), val=float(f[5]), has=f[6] == '1'))
        elif f[0] == 'PRESET':
            cur['presets'].append(f[2])
        elif f[0] == 'W':
            f += [''] * (9 - len(f))
            cur['widgets'].append(dict(cls=f[1], name=f[2], text=f[3], x=int(f[4]), y=int(f[5]), w=int(f[6]), h=int(f[7]), extra=f[8]))
    return pages

READOUT = re.compile(r'^[-+]?[\d.,:]+\s*(%|dB|Hz|kHz|ms|s|sec|semitones?|cents|samples)?$|^-?Inf\.?$|^Some Length$|^$|^dB 0$|^0\|dB$|^[\d.]+ :1$|^[\d.]+ oct\.$|^[\d.]+k$|^(Left|Right): [-\d.]+ %$|^Off$|^\(dynamically displayed\)$|^Unavailable with Percentage$|^Down Major 7th \+$|^ratio$')

def norm(t):
    t = t.replace('\r', '').replace('\n', '|').strip()
    t = re.sub(r'\s*\|\s*', '|', t)
    return re.sub(r'\s+', ' ', t)

def page_caps(p):
    caps = []
    for w in p['widgets']:
        if w['cls'] in ('QLabel', 'QCheckBox', 'QRadioButton', 'QGroupBox', 'QPushButton', 'QToolButton', 'QAbstractButton') and norm(w['text']) and not READOUT.match(norm(w['text'])):
            caps.append((w['y'], w['x'], norm(w['text']), w['cls'], w['w']))
    return caps

# texts VEGAS fills in or switches at run time: the mix column heading names the effect or the mode and changes
# (the original comment follows)
# the mix column's heading names the effect (or the mode) and changes at run time; the page has the right one for its setting
DYNAMIC = ('Wah-Wah', 'Flange', 'Reverb', 'Distortion', 'Stutter')
SWITCH = ('Pos/Neg', 'Nega&tive', 'Posi&tive', '(Hz):', 'Below (50 to 15,000 Hz):', 'Center (50 to 15,000 Hz):', 'Above (50 to 15,000 Hz):',
          '&Final length (50.0 to 500.0 %):', '&Final Percentage (50.0 to 500.0 %):', '&Bandwidth (oct):', '&Rolloff (dB/oct):', 'EQ', 'Channels')     # Distortion's button names the polarity it edits

def dlg_caps(d):
    caps = []
    for c in d['ctrls']:
        if not c.get('style', 0x10000000) & 0x10000000:
            continue    # a control the dialog does not show (the digit labels the faders' Alt+digit goes through)
        k = dlgparse.kind(c)
        if k in ('label', 'check', 'radio', 'push', 'group') and norm(c['text']) and not READOUT.match(norm(c['text'])):
            caps.append((c['y'], c['x'], norm(c['text']), k, c['w'], bool(c['style'] & 1) and c['cls'] == 'Static'))
    return caps

def words(caps):
    s = set()
    for _, _, t, *_r in caps:
        for w in re.findall(r'[A-Za-z]{3,}', t.replace('&', '')):
            s.add(w.lower())
    return s

def kinds_dialog(d):
    n = collections.Counter()
    for c in d['ctrls']:
        k = dlgparse.kind(c)
        if k == 'slider' or k == 'fader': n['slider'] += 1
        elif k in ('edit', 'combo', 'check', 'radio', 'push', 'envelope', 'graph', 'spinner'): n[k] += 1
    return n

def kinds_page(p):
    n = collections.Counter()
    for w in p['widgets']:
        c = w['cls']
        if c == 'QSlider': n['slider'] += 1
        elif c == 'QLineEdit': n['edit'] += 1
        elif c == 'QComboBox': n['combo'] += 1
        elif c == 'QCheckBox': n['check'] += 1
        elif c == 'QRadioButton': n['radio'] += 1
        elif c == 'QPushButton' and not w['name'].startswith('dxEqTab'): n['push'] += 1
    return n

def allowed(uid, dll):
    # which DLL a page can reproduce (the effect families live in their own DLLs)
    if uid.startswith('reno.xp'): return dll.startswith('sfxpfx') or (dll.startswith('sfppack1') and 'timestretch' in uid)
    if uid in ('reno.trkvolume', 'reno.trkpan'): return dll.startswith('sffrgpnv')
    if uid.startswith('reno.trk'): return dll.startswith('sftrkfx1')
    if uid == 'reno.resonantfilter': return dll.startswith('sfresfilter')
    return dll.startswith('sfppack')

def main():
    if len(sys.argv) < 3:
        sys.exit('usage: dlgcheck.py <fxdlg_dump output> <VEGAS "Shared Plug-Ins/Audio_x64" folder>')
    dump = sys.argv[1]
    folder = sys.argv[2]
    pages = read_dump(dump)
    dialogs = []
    strings = {}
    for f in sorted(glob.glob(os.path.join(folder, '*.dll'))):
        try:
            dl, st = dlgparse.load(f)
        except Exception:
            continue
        strings[os.path.basename(f)] = set(v.replace('&', '').strip() for v in st.values())
        for i, d in dl.items():
            if len(d['ctrls']) > 3:
                dialogs.append((os.path.basename(f), i, d))
    summary = []
    for uid, p in pages.items():
        pc = [c for c in page_caps(p) if not re.match(r'^&\d$', c[2])]    # (the faders' hidden digit labels)
        pc = [c for c in pc if c[2] not in SWITCH and not (uid.startswith('reno.xp') and c[2] in DYNAMIC)]
        pw = words(pc)
        best, bs = None, 0
        pset0 = collections.Counter(c[2] for c in pc)
        for f, i, d in dialogs:
            if not allowed(uid, f): continue
            dc0 = dlg_caps(d)
            dw = words(dc0)
            if not dw or not pw: continue
            exact = sum((pset0 & collections.Counter(c[2] for c in dc0)).values())
            s = len(dw & pw) / len(dw | pw) + exact * 0.01
            if s > bs: best, bs = (f, i, d), s
        print('=' * 100)
        print('%s (%s): page %dx%d px, %d widgets, %d settings, %d presets' % (uid, p['name'], p['w'], p['h'], len(p['widgets']), len(p['params']), len(p['presets'])))
        if not best or bs < 0.3:
            print('  no dialog matches (best %.2f %s)' % (bs, best[:2] if best else ''))
            summary.append((uid, 'no dialog', 0))
            continue
        f, i, d = best
        print('  dialog %s #%d (%dx%d DLU), caption overlap %.2f' % (f, i, d['w'], d['h'], bs))
        dc = [c for c in dlg_caps(d) if not re.match(r'^&\d$', c[2])]
        dc = [c for c in dc if c[2] not in SWITCH and not (uid.startswith('reno.xp') and c[2] in DYNAMIC)]
        problems = 0
        # exact captions
        pset = collections.Counter(c[2] for c in pc)
        dset = collections.Counter(c[2] for c in dc)
        for t, n in (dset - pset).items():
            print('  MISSING on page: %r' % t); problems += 1
        for t, n in (pset - dset).items():
            print('  EXTRA on page:   %r' % t); problems += 1
        # preset names and list items against the DLL's string table
        sset = strings.get(f, set())
        miss = [n for n in p['presets'] if n.replace('&', '').strip() not in sset]
        if p['presets']:
            print('  presets: %d, %d of their names are in the DLL string table%s' % (len(p['presets']), len(p['presets']) - len(miss), (' - not found: ' + ', '.join(miss[:10])) if miss else ''))
        for w in p['widgets']:
            if w['cls'] == 'QComboBox' and w['extra']:
                bad = [it for it in w['extra'].split('|') if it.replace('&', '').strip() not in sset]
                if bad and len(bad) < len(w['extra'].split('|')):
                    print('  COMBO %s: items not in the string table: %s' % (w['name'], bad[:8]))
        # kinds
        kd, kp = kinds_dialog(d), kinds_page(p)
        for k in sorted(set(kd) | set(kp)):
            if k in ('envelope', 'graph', 'spinner'): continue
            if kd[k] != kp[k]:
                print('  COUNT %-7s dialog %d, page %d' % (k, kd[k], kp[k])); problems += 1
        # order
        order_d = [c[2] for c in sorted(dc, key=lambda c: (c[0] // 6, c[1]))]
        order_p = [c[2] for c in sorted(pc, key=lambda c: (c[0] // 12, c[1]))]
        common = [t for t in order_d if t in pset]
        common_p = [t for t in order_p if t in dset]
        if common != common_p:
            print('  ORDER differs:'); print('     dialog:', common); print('     page:  ', common_p); problems += 1
        # places: the captions the page shares with the dialog, put where the dialog has them (a straight-line fit of page position against dialog
        # position per direction; what is more than 6 dialog units off the fit is listed)
        pos_d = {}
        for y, x, t, k, cw, mid in dc: pos_d.setdefault(t, (x + cw / 2.0 if mid else x, y, mid))
        pairs = []
        seen = set()
        for y, x, t, k, cw in pc:
            if t in pos_d and t not in seen and list(pset.elements()).count(t) == 1:
                seen.add(t)
                pairs.append((t, pos_d[t][0], pos_d[t][1], x + cw / 2.0 if pos_d[t][2] else x, y))
        def fit(us, vs):
            n = len(us); mu = sum(us) / n; mv = sum(vs) / n
            suu = sum((u - mu) ** 2 for u in us)
            if max(us) - min(us) < 40: return 1.8, mv - 1.8 * mu    # (captions in one column or row only: the scale cannot be fitted, take the usual one)
            a = sum((u - mu) * (v - mv) for u, v in zip(us, vs)) / suu
            return a, mv - a * mu
        if len(pairs) >= 4:
            ax, bx = fit([q[1] for q in pairs], [q[3] for q in pairs])
            ay, by = fit([q[2] for q in pairs], [q[4] for q in pairs])
            # one robust refit without the worst fifth
            def resid(q): return (abs(q[3] - (ax * q[1] + bx)) / max(ax, 0.5), abs(q[4] - (ay * q[2] + by)) / max(ay, 0.5))
            keep = sorted(pairs, key=lambda q: max(resid(q)))[:max(4, len(pairs) * 4 // 5)]
            ax, bx = fit([q[1] for q in keep], [q[3] for q in keep])
            ay, by = fit([q[2] for q in keep], [q[4] for q in keep])
            out = [(q[0],) + resid(q) for q in pairs if resid(q)[0] > 6 or resid(q)[1] > 6]
            print('  layout fit: %.2f px/DLU across, %.2f down' % (ax, ay))
            if out:
                print('  PLACE %d of %d captions are not where the dialog has them (after fitting the scale):' % (len(out), len(pairs)))
                for t, rx, ry in out[:14]:
                    print('       %-50r %.0f units across, %.0f down off' % (t, rx, ry))
                problems += 1
        # size
        sx, sy = p['w'] / d['w'], p['h'] / d['h']
        print('  scale: %.2f px/DLU wide, %.2f px/DLU high' % (sx, sy))
        summary.append((uid, '%s#%d' % (f, i), problems))
    print()
    print('SUMMARY')
    for uid, d, n in summary:
        print('  %-22s %-24s %s' % (uid, d, 'ok' if n == 0 else '%d differences' % n))

if __name__ == "__main__":
    main()
