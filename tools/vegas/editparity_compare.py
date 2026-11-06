#!/usr/bin/env python3
"""Compares the scenario folders RenoEdits.cs made in VEGAS with the ones Reno made (by running the same script in Reno,
or with tests/ui/editparity_replay.cpp), field by field.

    editparity_compare.py <VEGAS folder> <Reno folder> [--tol TICKS] [--only NAME[,NAME...]] [--show-same] [--json FILE]
    editparity_compare.py --selftest

Every scenario folder has a result.json with the snapshots "before", "mid" and "after" (the project as the program reports it
after each step: event positions and lengths in 100 ns ticks, take offsets, fades, groups, envelopes, keys, effects) and the
list of operations. Differences are printed as  scenario/snapshot track T event E field: vegas=... reno=...
Exit code: the number of scenarios that differ (scenarios in which VEGAS itself failed are listed as N/A and not counted).

What is compared: the number of tracks and events, each event's start, length, rate, mute / lock / loop, name, fades (length
and curve), group membership (as sets of events: group numbers differ between the programs), transition, take offsets, which
take is active, whether it plays reversed, envelope points, Pan/Crop key times, effect names; markers and regions. What is
not: the selection (except after the program's own Edit commands), colours and anything a program does not report. Take offsets of reversed events are shown but only
counted when --strict-reversed is given (the two programs measure a reversed subclip's offset from different ends).
"""
import argparse
import json
import os
import sys


def load(path):
    with open(path, 'r', encoding='utf-8-sig') as f:
        return json.load(f)


class Diff:
    def __init__(self):
        self.items = []
        self.standin = False    # VEGAS's side was done by the script through the API, not by VEGAS's own command
        self.na = None          # why VEGAS's side cannot be compared (the scenario or one of its commands failed in VEGAS)

    def add(self, where, field, a, b):
        self.items.append((where, field, a, b))


def close(a, b, tol):
    return abs(a - b) <= tol


def groups_of(proj):
    """group number -> set of (track, index); events outside any group are left out"""
    out = {}
    for t in proj.get('tracks', []):
        for e in t.get('events', []):
            g = e.get('group', -1)
            if g is not None and g >= 0:
                out.setdefault(g, set()).add((e.get('track'), e.get('index')))
    return sorted(sorted(v) for v in out.values())


def event_reversed(e):
    if e.get('reversed'):
        return True
    for t in e.get('takes', []):
        if t.get('subclipReversed'):
            return True
    return False


def compare_curve(d, where, field, a, b):
    if a is not None and b is not None and a != b:
        d.add(where, field, a, b)


def compare_env(d, where, ea, eb, tol, ytol=1e-6):
    ta = {e.get('type'): e for e in ea}
    tb = {e.get('type'): e for e in eb}
    for k in sorted(set(ta) | set(tb)):
        if k not in ta or k not in tb:
            # VEGAS may report an envelope with no points that the other program does not list
            pa = len(ta[k].get('points', [])) if k in ta else 0
            pb = len(tb[k].get('points', [])) if k in tb else 0
            if pa or pb:
                d.add(where, 'envelope ' + k, 'present' if k in ta else 'absent', 'present' if k in tb else 'absent')
            continue
        pa, pb = ta[k].get('points', []), tb[k].get('points', [])
        if len(pa) != len(pb):
            d.add(where, 'envelope %s points' % k, [(p['x'], round(p['y'], 4)) for p in pa], [(p['x'], round(p['y'], 4)) for p in pb])
            continue
        for i, (x, y) in enumerate(zip(pa, pb)):
            if not close(x['x'], y['x'], tol):
                d.add(where, 'envelope %s point %d time' % (k, i), x['x'], y['x'])
            if not close(x['y'], y['y'], ytol * max(1.0, abs(x['y']))):
                d.add(where, 'envelope %s point %d value' % (k, i), x['y'], y['y'])
            if x.get('curve') != y.get('curve'):
                d.add(where, 'envelope %s point %d curve' % (k, i), x.get('curve'), y.get('curve'))


# The selection afterwards is compared for scenarios that use the program's own commands (an API call does not change it).
COMPARE_SELECTION = [False]


def compare_event(d, where, a, b, tol, strict_rev):
    if COMPARE_SELECTION[0] and 'selected' in a and 'selected' in b and a['selected'] != b['selected']:
        d.add(where, 'selected', a['selected'], b['selected'])
    for f in ('name', 'kind', 'start', 'length', 'mute', 'locked', 'loop'):
        if f in a and f in b:
            if f in ('start', 'length'):
                if not close(a[f], b[f], tol):
                    d.add(where, f, a[f], b[f])
            elif a[f] != b[f]:
                d.add(where, f, a[f], b[f])
    if 'playbackRate' in a and 'playbackRate' in b and not close(a['playbackRate'], b['playbackRate'], 1e-9 * max(1.0, abs(a['playbackRate']))):
        d.add(where, 'playbackRate', a['playbackRate'], b['playbackRate'])
    for side in ('fadeIn', 'fadeOut'):
        fa, fb = a.get(side, {}), b.get(side, {})
        if 'length' in fa and 'length' in fb and not close(fa['length'], fb['length'], tol):
            d.add(where, side + '.length', fa['length'], fb['length'])
        compare_curve(d, where, side + '.curve', fa.get('curve'), fb.get('curve'))
    compare_curve(d, where, 'fadeIn.reciprocalCurve', a.get('fadeIn', {}).get('reciprocalCurve'), b.get('fadeIn', {}).get('reciprocalCurve'))
    if a.get('transition') != b.get('transition') and ('transition' in a or 'transition' in b):
        d.add(where, 'transition', a.get('transition'), b.get('transition'))
    ra, rb = event_reversed(a), event_reversed(b)
    if ra != rb:
        d.add(where, 'reversed', ra, rb)
    ta, tb = a.get('takes', []), b.get('takes', [])
    if len(ta) != len(tb):
        d.add(where, 'take count', len(ta), len(tb))
    else:
        for i, (x, y) in enumerate(zip(ta, tb)):
            if x.get('active') != y.get('active'):
                d.add(where, 'take %d active' % i, x.get('active'), y.get('active'))
            if 'offset' in x and 'offset' in y and (strict_rev or not (ra or rb)):
                if not close(x['offset'], y['offset'], tol):
                    d.add(where, 'take %d offset' % i, x['offset'], y['offset'])
    ka, kb = a.get('panCrop', []), b.get('panCrop', [])
    # a video event always has one key at 0 in VEGAS (the unmoved picture); Reno lists no key for it
    if len(ka) == 1 and ka[0].get('position') == 0 and not kb:
        ka = []
    if len(kb) == 1 and kb[0].get('position') == 0 and not ka:
        kb = []
    if len(ka) != len(kb):
        d.add(where, 'pan/crop key count', [k['position'] for k in ka], [k['position'] for k in kb])
    else:
        for i, (x, y) in enumerate(zip(ka, kb)):
            if not close(x['position'], y['position'], tol):
                d.add(where, 'pan/crop key %d time' % i, x['position'], y['position'])
    compare_env(d, where, a.get('envelopes', []), b.get('envelopes', []), tol)
    ea = [x.get('name') for x in a.get('effects', [])]
    eb = [x.get('name') for x in b.get('effects', [])]
    if ea != eb:
        d.add(where, 'effects', ea, eb)


def compare_project(d, prefix, a, b, tol, strict_rev):
    ta, tb = a.get('tracks', []), b.get('tracks', [])
    if len(ta) != len(tb):
        d.add(prefix, 'track count', len(ta), len(tb))
    for ti, (x, y) in enumerate(zip(ta, tb)):
        ea, eb = x.get('events', []), y.get('events', [])
        if len(ea) != len(eb):
            d.add('%s track %d' % (prefix, ti), 'event count (start,length)',
                  [(e.get('start'), e.get('length')) for e in ea], [(e.get('start'), e.get('length')) for e in eb])
            continue
        compare_env(d, '%s track %d' % (prefix, ti), x.get('envelopes', []), y.get('envelopes', []), tol)
        for ei, (p, q) in enumerate(zip(ea, eb)):
            compare_event(d, '%s track %d event %d' % (prefix, ti, ei), p, q, tol, strict_rev)
    ga, gb = groups_of(a), groups_of(b)
    if ga != gb:
        d.add(prefix, 'groups', ga, gb)
    for key in ('markers', 'regions'):
        la, lb = a.get(key, []), b.get(key, [])
        if len(la) != len(lb):
            d.add(prefix, key + ' count', len(la), len(lb))
        else:
            for i, (m, n) in enumerate(zip(la, lb)):
                if not close(m.get('position', 0), n.get('position', 0), tol) or not close(m.get('length', 0), n.get('length', 0), tol):
                    d.add(prefix, '%s %d' % (key, i), m, n)


def compare_scenario(name, ra, rb, tol, strict_rev):
    d = Diff()
    # A scenario in which VEGAS itself failed (no media, a command that did nothing) says nothing about Reno: its "after" is not
    # what the operations should give, so it is listed as N/A and not counted as a difference.
    refused = [o for o in ra.get('ops', []) if o.get('kind') != 'snapshot' and o.get('ok') is False]
    if ra.get('error'):
        d.na = 'VEGAS scenario error: ' + ra['error'].splitlines()[0]
    elif refused:
        d.na = 'VEGAS refused %d operation(s), first: %s: %s' % (len(refused), refused[0].get('op'), (refused[0].get('error') or '')[:100])
    if d.na:
        return d
    COMPARE_SELECTION[0] = any(o.get('kind') == 'ui' for o in ra.get('ops', []))
    if ra.get('error'):
        d.add(name, 'VEGAS scenario error', ra['error'].splitlines()[0], None)
    if rb.get('error'):
        d.add(name, 'Reno scenario error', None, str(rb['error']).splitlines()[0])
    uiOps = COMPARE_SELECTION[0]
    for snap in ('before', 'mid', 'after'):
        a, b = ra.get(snap), rb.get(snap)
        COMPARE_SELECTION[0] = uiOps and snap != 'before'      # (the program selected the events after 'before' was taken)
        if isinstance(a, dict) and isinstance(b, dict):
            compare_project(d, '%s/%s' % (name, snap), a, b, tol, strict_rev)
        elif (a is None) != (b is None):
            d.add('%s/%s' % (name, snap), 'snapshot', 'present' if a else 'absent', 'present' if b else 'absent')
    # the operations: one the other program refused is worth knowing even if the result looks the same
    oa = [o for o in ra.get('ops', []) if o.get('kind') != 'snapshot']
    ob = [o for o in rb.get('ops', []) if o.get('kind') != 'snapshot']
    for i, (p, q) in enumerate(zip(oa, ob)):
        if p.get('ok') != q.get('ok'):
            d.add('%s op %d (%s)' % (name, i, p.get('op')), 'worked', '%s %s' % (p.get('ok'), p.get('error') or ''), '%s %s' % (q.get('ok'), q.get('error') or ''))
    d.standin = any(o.get('viaApi') for o in ra.get('ops', []))
    return d


def run(a_dir, b_dir, tol, only, show_same, strict_rev, out_json):
    names = sorted(n for n in os.listdir(a_dir) if os.path.isfile(os.path.join(a_dir, n, 'result.json')))
    if only:
        names = [n for n in names if n in only]
    failed = 0
    na = 0
    standin = 0
    report = {}
    for n in names:
        pb = os.path.join(b_dir, n, 'result.json')
        if not os.path.isfile(pb):
            print('MISSING %s: no result in %s' % (n, b_dir))
            failed += 1
            report[n] = ['missing']
            continue
        d = compare_scenario(n, load(os.path.join(a_dir, n, 'result.json')), load(pb), tol, strict_rev)
        if d.na:
            na += 1
            print('N/A  %s: %s' % (n, d.na))
            report[n] = ['n/a: ' + d.na]
            continue
        if d.items and d.standin:
            standin += 1
            print('STAND-IN %s: %d difference(s), but VEGAS did not run the command (the script did it through the API)' % (n, len(d.items)))
            for where, field, x, y in d.items:
                print('   %s %s: vegas=%s reno=%s' % (where, field, x, y))
            report[n] = ['stand-in: %s %s: vegas=%s reno=%s' % i for i in d.items]
            continue
        if d.items:
            failed += 1
            print('DIFF %s: %d difference(s)' % (n, len(d.items)))
            for where, field, x, y in d.items:
                print('   %s %s: vegas=%s reno=%s' % (where, field, x, y))
        elif show_same:
            print('SAME %s' % n)
        report[n] = ['%s %s: vegas=%s reno=%s' % i for i in d.items]
    print('%d scenario(s): %d compared and differ, %d N/A (VEGAS could not run them), %d differ only from a stand-in' % (len(names), failed, na, standin))
    if out_json:
        with open(out_json, 'w') as f:
            json.dump(report, f, indent=1)
    return failed


def selftest():
    def ev(t, i, start, length, off=0, **kw):
        e = {'track': t, 'index': i, 'kind': 'audio', 'name': 'x', 'start': start, 'length': length, 'end': start + length,
             'playbackRate': 1.0, 'mute': False, 'locked': False, 'loop': True, 'group': -1,
             'fadeIn': {'length': 0, 'curve': 'Linear'}, 'fadeOut': {'length': 0, 'curve': 'Linear'},
             'takes': [{'active': True, 'offset': off}], 'envelopes': [], 'panCrop': [], 'effects': []}
        e.update(kw)
        return e
    def proj(*events):
        return {'tracks': [{'index': 0, 'events': list(events)}], 'markers': [], 'regions': []}
    a = {'before': proj(ev(0, 0, 0, 100)), 'after': proj(ev(0, 0, 0, 30, 0), ev(0, 1, 30, 70, 30)), 'ops': []}
    same = compare_scenario('t', a, json.loads(json.dumps(a)), 1, False)
    assert not same.items, same.items
    b = json.loads(json.dumps(a))
    b['after']['tracks'][0]['events'][1]['takes'][0]['offset'] = 35
    b['after']['tracks'][0]['events'][0]['fadeOut']['length'] = 5
    d = compare_scenario('t', a, b, 1, False)
    fields = sorted(f for _, f, _, _ in d.items)
    assert fields == ['fadeOut.length', 'take 0 offset'], fields
    c = json.loads(json.dumps(a))
    c['after']['tracks'][0]['events'][1]['group'] = 0
    c['after']['tracks'][0]['events'][0]['group'] = 0
    d = compare_scenario('t', a, c, 1, False)
    assert [f for _, f, _, _ in d.items] == ['groups'], d.items
    print('editparity_compare selftest: ok')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('vegas', nargs='?')
    ap.add_argument('reno', nargs='?')
    ap.add_argument('--tol', type=int, default=0, help='ticks (100 ns) two times may differ by')
    ap.add_argument('--only', default='', help='comma separated scenario names')
    ap.add_argument('--show-same', action='store_true')
    ap.add_argument('--strict-reversed', action='store_true')
    ap.add_argument('--json')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.vegas or not a.reno:
        ap.error('two folders needed')
    only = set(x for x in a.only.split(',') if x)
    return run(a.vegas, a.reno, a.tol, only, a.show_same, a.strict_reversed, a.json)


if __name__ == '__main__':
    sys.exit(min(main(), 125))
