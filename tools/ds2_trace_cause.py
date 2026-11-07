#!/usr/bin/env python3
"""Per target: which instruction precedes the entry to declare (why GapFill did not split there) and what happened to
the GapFill segment start (registered / pre-existing / skipped / removed before SEALED). Read-only.
Usage: python3 tools/ds2_trace_cause.py <repo> <trace stdout log> <trace-unresolved-match.tsv> out/ds2_image.bin \
           out/reports/classification.tsv <out.tsv>
"""
import re,sys,csv,struct,collections
sys.path.insert(0,sys.argv[1]+'/tools')
import ds2_imagedump as D, ds2_analyze_unresolved as A
T,M,IMG,CLS,OUT=sys.argv[2:7]
B=0x82000000; img=open(IMG,'rb').read(); w=lambda a: struct.unpack_from('>I',img,a-B)[0]
pe=D.parse_pe(img,B); pd=D.pdata_functions(img,B,pe); pdset={p[0] for p in pd}
pre=set(); sealed=set(); gapreg=set(); phase=None
pn=re.compile(r'FunctionNode 0x([0-9A-F]{8})'); pr=re.compile(r'GapFill: registered sub_([0-9A-F]{8})')
for l in open(T,errors='replace'):
    if 'phase  Default.xex: GapFill' in l: phase='gap'
    if l.startswith('FunctionNode 0x'):
        a=int(pn.match(l)[1],16)
        if phase is None: pre.add(a)
        if ': SEALED' in l: sealed.add(a)
    elif 'GapFill: registered sub_' in l: gapreg.add(int(pr.search(l)[1],16))
known=pre|pdset
def btarget(a):
    x=w(a); li=A.sext(x&0x03FFFFFC,26); return (li&0xFFFFFFFF) if x&2 else (a+li)&0xFFFFFFFF
def boundary(a):
    p=w(a-4); m=A.mnem(p)
    if m=='b': return 'b->%08X (%s before GapFill)'%(btarget(a-4),'KNOWN' if btarget(a-4) in known else 'unknown')
    if m=='bctr': return 'bctr (never a split point)'
    if m=='blr': return 'blr (split point)'
    return 'falls through (%s)'%m
cls={int(r['target'],16):r for r in csv.DictReader(open(CLS),delimiter='\t')}
rows=list(csv.DictReader(open(M),delimiter='\t')); out=[]
for r in rows:
    t=int(r['t'],16); s=int(r['seg'][:8],16); c=cls[t]; entry=int(c['declare_entry'],16)
    ss=('GapFill-registered, later removed (no SEALED)' if s in gapreg and s not in sealed else
        'GapFill-registered, SEALED' if s in gapreg else
        'existed before GapFill (%s)'%('PDATA' if s in pdset else 'Discover') if s in known else
        'not registered: start inside PDATA fn' if 'inside PDATA' in r['seg_start_status'] else 'not registered (other)')
    b_entry=boundary(entry)
    if b_entry.startswith('b->') and 'unknown' in b_entry: why='entry not split: preceded by b to unknown'
    elif b_entry.startswith('bctr'): why='entry not split: preceded by bctr'
    elif b_entry.startswith('b->'): why='preceded by b to KNOWN (should split) - CHECK'
    elif b_entry.startswith('blr'): why='preceded by blr (split) - CHECK'
    else: why='entry falls through - CHECK'
    out.append((r['t'],c['class'],f'{entry:08X}',b_entry,r['seg'],ss,why))
with open(OUT,'w') as f:
    f.write('target\tclass\tentry_to_declare\tinstr_before_entry\tgapfill_segment\tsegment_start_fate\tmissing_split\n')
    for o in out: f.write('\t'.join(o)+'\n')
print('pre-GapFill functions:',len(pre),' PDATA:',len(pdset),' GapFill registered:',len(gapreg),' SEALED:',len(sealed))
print(collections.Counter(o[6] for o in out))
print(collections.Counter(o[5] for o in out))
for o in out: print(' | '.join(o))
