# compare_defs.py XML.JSON MINE.TSV: xmlparams.py's output against Reno's own list of the same effects' settings
import json,collections,sys
if len(sys.argv) != 3: sys.exit('usage: compare_defs.py XML.JSON MINE.TSV')
x=json.load(open(sys.argv[1]))
mine=collections.defaultdict(dict)
for l in open(sys.argv[2]):
    f=l.rstrip('\n').split('\t')
    mine[f[1]][f[3]]=dict(type=f[2],label=f[4],secret=f[5],choices=f[6].split('|') if f[6] else [],d=f[7],r=f[8])
for n,ps in x.items():
    print('==',n)
    names=set()
    for p in ps:
        names.add(p['name'])
        m=mine[n].get(p['name'])
        if not m: print('  MISSING',p['type'],p['name'],'|',p['label']); continue
        if p['label'] and m['label']!=p['label']: print('  LABEL',p['name'],repr(m['label']),'->',repr(p['label']))
        if p['choices'] and m['choices']!=p['choices']: print('  CHOICES',p['name'],m['choices'],'->',p['choices'])
    for k,m in mine[n].items():
        if k not in names: print('  hidden-or-extra (mine)',k,m['secret'])
