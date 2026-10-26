#!/usr/bin/env python3
# Writes src/scripting/ScriptPortal.Vegas.Unsupported.cs: every public class, property, method, field and enumeration
# of VEGAS Pro 18's ScriptPortal.Vegas that Reno's ScriptPortal.Vegas.cs does not have, each throwing a
# NotSupportedException that names it ("Renderer.ClassID is not supported by Reno."). With it, every VEGAS script
# compiles against Reno; what Reno cannot do fails only when the script gets there, with the member's name.
#
#   gen_unsupported.py <VEGAS's API> <Reno's API> <out.cs>
# Both APIs are listings made by tests/scripting/dump_api.ps1 (from VEGAS's ScriptPortal.Vegas.dll and from Reno's,
# compiled from ScriptPortal.Vegas.cs alone). Rerun it after adding members to ScriptPortal.Vegas.cs (the classes
# there are 'partial', so the two files add up to one class).
#
# Left out on purpose: VEGAS's COM plumbing (I...COM, DomainManager, ScriptHost), its dockable windows and application
# extensions (DockableControl, ICustomCommandModule - those are not scripts), its internal dialogs and settings files
# (BurgerMenuConfig, XmlSetting...) and members whose types live in VEGAS's other assemblies.
import re, sys, collections

SKIP_TYPES = re.compile(r'COM$|COM\b|^(IDomainManager|DomainManager|IScriptHost|ScriptHost|ICustomCommandModule|CaptureDockWnd|'
                        r'VegasCapturePanel|PrintWizard|COMStreamWrapper|IShellLinkW|MS|SFDIBPIXEL|DockableControl|IDockView|IDockWindow|'
                        r'CaptionDoubleClickEventHandler|CaptionDoubleClickEventArgs|IBurgerMenuConfig|BurgerMenuConfig|BurgerMenuConfigItem|'
                        r'IPreferenceDialogConfig|PreferenceDialogConfig|PreferenceEntry|ISelPastAttrConfig|SelPastAttrConfig|IFileDialogArgs|'
                        r'FileDialogArgs|IInfoDialogArgs|InfoDialogArgs|IRunRenderDialog|INetRenderDialogArgs|IDataExchange|DataExchange|'
                        r'XmlSetting|\w+Setting|ListSetting`1|XmlSettings|LicenseInfo|IMediaMarker|IMediaPoolIterator|'
                        r'IProjectProgressUpdateCOM|IRenderArgs|IMissingMediaEventArgs|IRenderStatusEventArgs|IMessageEventArgs|'
                        r'IFileOpenInfoList|CustomCommand|CommandCategory|VegasEventID|AppChangeFlag|ProjectChangeFlag|'
                        r'OFX\w*)$')
PRIM = {'Boolean': 'bool', 'Int32': 'int', 'UInt32': 'uint', 'Int64': 'long', 'UInt64': 'ulong', 'Double': 'double', 'Single': 'float',
        'String': 'string', 'Void': 'void', 'Object': 'object', 'Byte': 'byte', 'Char': 'char', 'Int16': 'short', 'UInt16': 'ushort',
        'IntPtr': 'System.IntPtr', 'UIntPtr': 'System.UIntPtr', 'SByte': 'sbyte'}
CS_KEYWORDS = {'object', 'string', 'base', 'params', 'ref', 'out', 'in', 'event', 'operator', 'class', 'checked', 'fixed', 'lock', 'default'}


def parse(fn):
    types = collections.OrderedDict()
    cur = None
    for line in open(fn, encoding='utf-8'):
        line = line.rstrip('\n')
        if line.startswith('references:'):
            break
        if not line.startswith('  '):
            m = re.match(r'(class|interface|enum|struct)( abstract)? (\S+)( \(nested in (\w+)\))? : ?(\S*)( \[(.*)\])?', line)
            cur = None
            if m:
                cur = m.group(3)
                if m.group(5): cur = m.group(5) + '+' + cur.lstrip('.')
                types[cur] = {'kind': m.group(1), 'abstract': bool(m.group(2)), 'base': m.group(6), 'ifaces': m.group(8) or '', 'members': [],
                              'nested': m.group(5)}
            continue
        if cur: types[cur]['members'].append(line.strip())
    return types


def short(t): return t.split('.')[-1]


def key(m):
    if m.startswith('prop '):
        mm = re.match(r'prop (static )?(\S+) (\w+)(\[[^\]]*\])?', m)
        return 'P:' + mm.group(3) + ('[]' if mm.group(4) else '')
    if m.startswith('event '): return 'E:' + m.split()[1]
    if m.startswith('field '):
        mm = re.match(r'field (static )?(\S+) (\w+)', m)
        return 'F:' + mm.group(3)
    if re.match(r'^\w+ = ', m): return 'V:' + m.split()[0]
    mm = re.match(r'(?:(?:static|protected|virtual) )*(\S+) ([\w.]+)\((.*)\)$', m)
    if not mm: return 'X:' + m
    # (overloads differ by their parameters' types)
    return 'M:%s(%s)' % (mm.group(2), ','.join(re.sub(r' \w+$', '', a) for a in split_args(mm.group(3))))


def split_args(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '<': depth += 1
        if ch == '>': depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip()); cur = ''
            continue
        cur += ch
    if cur.strip(): out.append(cur.strip())
    return out


def cstype(t, gen=None):
    """VEGAS's dump type name -> C#, or None when the type is not available to Reno."""
    t = t.strip()
    if t.startswith('ref ') or t.startswith('out '):
        r = cstype(t[4:], gen)
        return None if r is None else t[:4] + r
    if t.endswith('[]'):
        r = cstype(t[:-2], gen)
        return None if r is None else r + '[]'
    m = re.match(r'([\w.`]+)<(.*)>$', t)
    if m:
        base = m.group(1)
        args = [cstype(a, gen) for a in split_args(m.group(2))]
        if None in args: return None
        b = re.sub(r'`\d+$', '', base)
        if b.startswith('System.') or b in OK_TYPES: return b + '<' + ', '.join(args) + '>'
        return None
    if re.match(r'!\d+$', t): return gen[int(t[1:])] if gen else None
    if t in PRIM: return PRIM[t]
    if t.startswith('System.'): return t
    if t in OK_TYPES: return t.replace('+', '.')
    return None


def generate(vegas, reno):
    rn = {short(k): v for k, v in reno.items()}
    global OK_TYPES
    missing_types = [k for k in vegas if k.startswith('ScriptPortal.Vegas.') or vegas[k]['nested']]
    OK_TYPES = set()
    for k, t in vegas.items():
        s = short(k) if not t['nested'] else k
        if t['nested']:
            if t['nested'] in ('SummaryProperties', 'Vegas') and t['kind'] == 'enum': OK_TYPES.add(k.split('+')[1])
            continue
        if not k.startswith('ScriptPortal.Vegas.'): continue
        sn = re.sub(r'`\d+$', '', s)
        if SKIP_TYPES.search(sn) and sn not in rn: continue
        OK_TYPES.add(sn)
    OK_TYPES |= set(rn.keys())
    out = []
    W = out.append
    nm = 0
    nt = 0

    def have_chain(s):
        have = set()
        t2 = rn.get(s)
        n = 0
        while t2 is not None and n < 12:
            have |= set(key(m) for m in t2['members'])
            # (a generic base's members, whose types are its parameters: by name and count)
            have |= set(re.sub(r'\((.*)\)$', lambda g: '/%d' % len(split_args(g.group(1))), key(m)) for m in t2['members'] if '!' in m)
            n += 1
            raw = re.sub(r'<.*', '', t2['base']).split('.')[-1] if t2['base'] else None
            b = re.sub(r'`\d+$', '', raw) if raw else None
            t2 = (rn.get(raw) if raw else None) or rn.get(b) or next((v for k2, v in rn.items() if re.sub(r'`\d+$', '', k2) == b), None)
        return have

    def throw(owner, member):
        return 'throw new NotSupportedException("%s.%s is not supported by Reno.");' % (owner, member)

    def members(owner, t, have, is_iface, gen=None, ctor_name=None):
        nonlocal nm
        lines = []
        seen = set()
        for m in t['members']:
            if m.startswith('protected'): continue
            k = key(m)
            if k in have or k in seen: continue
            if re.sub(r'\((.*)\)$', lambda g: '/%d' % len(split_args(g.group(1))), k) in have: continue
            if re.search(r'op_(In)?[Ee]quality|op_Implicit|op_Explicit|GetHashCode\(\)|Equals\(|ToString\(\)|CompareTo\(', m): continue
            seen.add(k)
            if m.startswith('prop '):
                mm = re.match(r'prop (static )?(\S+) (\w+)(\[([^\]]*)\])? \{( get;)?( set;)? \}', m)
                st, typ, name, idx = mm.group(1), cstype(mm.group(2), gen), mm.group(3), mm.group(5)
                if typ is None: continue
                if idx is not None:
                    its = [cstype(a, gen) for a in split_args(idx)]
                    if None in its: continue
                    head = '%s this[%s]' % (typ, ', '.join('%s p%d' % (a, i) for i, a in enumerate(its)))
                    label = 'this[]'
                else:
                    head = typ + ' ' + name
                    label = name
                if is_iface:
                    lines.append('        %s {%s%s }' % (head, ' get;' if mm.group(6) else '', ' set;' if mm.group(7) else ''))
                else:
                    acc = ''
                    if mm.group(6): acc += ' get { %s }' % throw(owner, label)
                    if mm.group(7): acc += ' set { %s }' % throw(owner, label)
                    lines.append('        public %s%s {%s }' % ('static ' if st else '', head, acc))
                nm += 1
            elif m.startswith('event '):
                if is_iface: continue
                lines.append('        public event EventHandler %s;' % m.split()[1])
                nm += 1
            elif m.startswith('field '):
                mm = re.match(r'field (static )?(\S+) (\w+)( = (.*))?$', m)
                typ = cstype(mm.group(2), gen)
                if typ is None or is_iface: continue
                val = mm.group(5)
                if val is not None and typ in ('int', 'uint', 'long', 'double', 'bool', 'string'):
                    v = val
                    if typ == 'bool': v = v.lower()
                    if typ == 'string': v = '"%s"' % v.replace('\\', '\\\\').replace('"', '\\"')
                    lines.append('        public const %s %s = %s;' % (typ, mm.group(3), v))
                elif mm.group(1):
                    lines.append('        public static readonly %s %s;' % (typ, mm.group(3)))
                else:
                    lines.append('        public %s %s;' % (typ, mm.group(3)))
                nm += 1
            else:
                mm = re.match(r'((?:(?:static|virtual) )*)(\S+) ([\w.]+)\((.*)\)$', m)
                if not mm: continue
                mods, ret, name, args = mm.group(1), mm.group(2), mm.group(3), mm.group(4)
                if (name != '.ctor' and '.' in name) or name in ('Invoke', 'BeginInvoke', 'EndInvoke', 'Finalize'): continue
                params = []
                ok = True
                for i, a in enumerate(split_args(args)):
                    am = re.match(r'(.*) (\w+)$', a)
                    pt = cstype(am.group(1), gen)
                    if pt is None: ok = False; break
                    pn = am.group(2)
                    if pn in CS_KEYWORDS: pn = '@' + pn
                    params.append(pt + ' ' + pn)
                if not ok: continue
                if name == '.ctor':
                    if is_iface: continue
                    lines.append('        public %s(%s) { %s }' % (ctor_name or owner, ', '.join(params), throw(owner, owner)))
                    nm += 1
                    continue
                if name.startswith('.'): continue
                rt = cstype(ret, gen)
                if rt is None: continue
                if is_iface:
                    lines.append('        %s %s(%s);' % (rt, name, ', '.join(params)))
                else:
                    lines.append('        public %s%s %s(%s) { %s }' % ('static ' if 'static' in mods else '', rt, name, ', '.join(params), throw(owner, name)))
                nm += 1
        return lines

    for k, t in vegas.items():
        if t['nested']: continue
        if not k.startswith('ScriptPortal.Vegas.'): continue
        s = short(k)
        sname = re.sub(r'`\d+$', '', s)
        if sname not in OK_TYPES: continue
        if s in rn or sname in rn:
            have = have_chain(s if s in rn else sname)
            r = rn.get(s) or rn.get(sname)
            if t['kind'] == 'enum':
                continue
            gen = None
            gp = re.match(r'.*`(\d+)$', s)
            if gp: gen = ['T%d' % i for i in range(int(gp.group(1)))] if int(gp.group(1)) > 1 else ['T']
            body = members(sname, t, have, t['kind'] == 'interface', gen)
            # nested enumerations of an existing class
            nested = [(nk, nt_) for nk, nt_ in vegas.items() if nt_['nested'] == sname and nt_['kind'] == 'enum']
            for nk, nt_ in nested:
                en = nk.split('+')[1]
                if 'E:' + en in have or any(m.startswith('enum ' + en) for m in r['members']): continue
                body.append('        public enum %s : long' % en)
                body.append('        {')
                body += ['            %s,' % v for v in nt_['members']]
                body.append('        }')
            if not body: continue
            kw = 'interface' if t['kind'] == 'interface' else 'class'
            g = '<%s>' % ', '.join(gen) if gen else ''
            W('    public partial %s %s%s' % (kw, sname, g))
            W('    {')
            out.extend(body)
            W('    }')
            W('')
            continue
        # a type Reno does not have at all
        nt += 1
        if t['kind'] == 'enum':
            vals = [re.match(r'(\w+) = (-?\d+)', v) for v in t['members']]
            big = any(v and not (-2**31 <= int(v.group(2)) < 2**31) for v in vals)
            W('    public enum %s%s' % (sname, ' : long' if big else ''))
            W('    {')
            out.extend('        %s = %s,' % (v.group(1), v.group(2)) for v in vals if v)
            W('    }')
            W('')
            continue
        gen = None
        gp = re.match(r'.*`(\d+)$', s)
        if gp: gen = ['T%d' % i for i in range(int(gp.group(1)))] if int(gp.group(1)) > 1 else ['T']
        g = '<%s>' % ', '.join(gen) if gen else ''
        base = t['base']
        bcs = None
        if base == 'System.MulticastDelegate':
            inv = [m for m in t['members'] if ' Invoke(' in m]
            mm = re.match(r'virtual (\S+) Invoke\((.*)\)', inv[0]) if inv else None
            if mm:
                params = []
                for a in split_args(mm.group(2)):
                    am = re.match(r'(.*) (\w+)$', a)
                    params.append((cstype(am.group(1)) or 'object') + ' ' + am.group(2))
                W('    public delegate %s %s(%s);' % (cstype(mm.group(1)) or 'void', sname, ', '.join(params)))
                W('')
            continue
        if base and base not in ('System.Object', 'System.ValueType'):
            bcs = cstype(base, gen)
            if bcs is None and re.match(r'[\w`]+<', base):
                bm = re.match(r'([\w`]+)<(.*)>', base)
                bname = re.sub(r'`\d+', '', bm.group(1))
                bargs = [cstype(a, gen) for a in split_args(bm.group(2))]
                if bname in OK_TYPES and None not in bargs: bcs = bname + '<' + ', '.join(bargs) + '>'
            if bcs is None: bcs = None if base.startswith('ScriptPortal.') else None
        kind = t['kind']
        if kind == 'interface':
            W('    public interface %s%s' % (sname, g))
        elif kind == 'struct':
            W('    public struct %s%s' % (sname, g))
        else:
            where = ''
            if gen and bcs and bcs.startswith('BaseMarkerList<'): where = ' where T : Marker'
            elif gen and bcs and re.match(r'Base\w*List<', bcs): where = ' where %s : class' % re.match(r'Base\w*List<(\w+)', bcs).group(1)
            bases = ([bcs] if bcs else []) + (['IDisposable'] if 'System.IDisposable' in t['ifaces'] else [])
            W('    public %sclass %s%s%s%s' % ('abstract ' if t['abstract'] else '', sname, g, (' : ' + ', '.join(bases)) if bases else '', where))
        W('    {')
        inherited = set()
        if bcs:
            inherited = have_chain(re.sub(r'<.*', '', bcs))
            if re.match(r'Base\w*List<', bcs): inherited = inherited | have_chain('BaseList`1') | have_chain('BaseList')
        body = members(sname, t, inherited, kind == 'interface', gen, ctor_name=sname)
        if kind == 'class':
            has_ctor = any(re.match(r'\s+public %s\(' % re.escape(sname), l) for l in body)
            marker_list = bool(bcs and bcs.startswith('BaseMarkerList<'))
            if marker_list:
                # (VEGAS's media markers: Reno's media have none)
                body = [l.replace('{ throw new NotSupportedException("%s.%s is not supported by Reno."); }' % (sname, sname),
                                  ': base(-1, -1) { throw new NotSupportedException("%s is not supported by Reno."); }' % sname) for l in body]
            if not has_ctor:
                body.insert(0, '        internal %s()%s { }' % (sname, ' : base(-1, -1)' if marker_list else ''))
            # a list of VEGAS's: going through it says it is not supported
            if bcs and re.match(r'Base\w*List<', bcs):
                body.append('        internal override List<%s> Items() { throw new NotSupportedException("%s is not supported by Reno."); }' % (bcs[bcs.index('<') + 1:-1], sname))
        nested = [(nk, nt_) for nk, nt_ in vegas.items() if nt_['nested'] == sname and nt_['kind'] == 'enum']
        for nk, nt_ in nested:
            en = nk.split('+')[1]
            body.append('        public enum %s : long' % en)
            body.append('        {')
            body += ['            %s,' % v for v in nt_['members']]
            body.append('        }')
        out.extend(body)
        W('    }')
        W('')
    return out, nm, nt


def main():
    vegas = parse(sys.argv[1])
    reno = parse(sys.argv[2])
    body, nm, nt = generate(vegas, reno)
    head = """// GENERATED by tests/scripting/gen_unsupported.py - do not edit (edit ScriptPortal.Vegas.cs and run it again).
//
// The parts of VEGAS Pro 18's ScriptPortal.Vegas that Reno does not have (%d members, %d types): each throws a
// NotSupportedException naming itself, so that every VEGAS script compiles against Reno and stops, if it gets there,
// with the name of what Reno cannot do. Compiled together with ScriptPortal.Vegas.cs (its classes are 'partial').
using System;
using System.Collections;
using System.Collections.Generic;
using System.Windows.Forms;

#pragma warning disable 67, 649, 169, 414

namespace ScriptPortal.Vegas
{
""" % (nm, nt)
    text = head + '\n'.join(body).rstrip() + '\n}\n'
    open(sys.argv[3], 'w', encoding='utf-8', newline='\n').write(text)
    print('gen_unsupported: %d members, %d types' % (nm, nt))


if __name__ == '__main__':
    main()
