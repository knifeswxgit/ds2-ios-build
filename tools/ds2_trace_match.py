#!/usr/bin/env python3
"""Maps each UnresolvedCall target of a `rexglue --log-level trace codegen` run to its Scan region and GapFill segment.

Recomputes Scan regions from the image (split on zero words, as phase_scan.cpp does) and replays the
`GapFill: split segment` / `GapFill: registered sub_` trace lines. Read-only.
Usage: python3 tools/ds2_trace_match.py <repo> <trace stdout log> out/ds2_image.bin config/unresolved_pairs.txt \
           out/reports/classification.tsv <out.tsv>
"""
import re,sys,struct,bisect,csv,collections
sys.path.insert(0,sys.argv[1]+'/tools')
import ds2_imagedump as D, ds2_analyze_unresolved as A
T=sys.argv[2]; IMG=sys.argv[3]; PAIRS=sys.argv[4]; CLS=sys.argv[5]
B=0x82000000
img=open(IMG,'rb').read(); pe=D.parse_pe(img,B); pd=D.pdata_functions(img,B,pe)
pst=[p[0] for p in pd]; pen=[p[0]+p[1] for p in pd]
def in_pdata(a):
    i=bisect.bisect_right(pst,a)-1; return pst[i] if i>=0 and a<pen[i] else None
w=lambda a: struct.unpack_from('>I',img,a-B)[0]
secs={s['name']:(B+s['va'],B+s['va']+s['vsize']) for s in pe['sections'] if s['mapped']}
# Scan regions (zero-word split; exception-handler 12-byte skip ignored -> only affects region boundaries at zero words)
lo,hi=secs['.text']; regions=[]; st=None
for a in range(lo,hi,4):
    if w(a)==0:
        if st is not None: regions.append((st,a)); st=None
    elif st is None: st=a
if st is not None: regions.append((st,hi))
rst=[r[0] for r in regions]
splits={}; reg={}; unres={}
pat_s=re.compile(r'GapFill: split segment 0x([0-9A-F]{8})-0x([0-9A-F]{8}) \((.+?) at 0x([0-9A-F]{8})\)')
pat_r=re.compile(r'GapFill: registered sub_([0-9A-F]{8}) \(0x([0-9A-F]{8})-0x([0-9A-F]{8})')
pat_u=re.compile(r'added unresolved jump 0x([0-9A-F]{8})->0x([0-9A-F]{8}) to function 0x([0-9A-F]{8})')
nreg_lines=0
for l in open(T,errors='replace'):
    if 'GapFill: split' in l:
        m=pat_s.search(l); s,e=int(m[1],16),int(m[2],16); splits[s]=(e,m[3])
    elif 'GapFill: registered sub_' in l:
        m=pat_r.search(l); reg[int(m[2],16)]=int(m[3],16)
    elif 'added unresolved jump' in l:
        m=pat_u.search(l); unres[(int(m[1],16),int(m[2],16))]=int(m[3],16)
print(f'scan regions recomputed: {len(regions)}; splits parsed {len(splits)}; registered {len(reg)}; unresolved-jump lines {len(unres)}')
regs=sorted(reg); 
def seg_of(t):
    i=bisect.bisect_right(rst,t)-1; r0,r1=regions[i]
    assert r0<=t<r1, hex(t)
    s=r0
    while s in splits and splits[s][0]<=t: s=splits[s][0]
    e,why=splits.get(s,(r1,'region end'))
    return (r0,r1),s,e,why
known_entry=set(pst)
def describe(t):
    (r0,r1),s,e,why=seg_of(t)
    status='REGISTERED' if s in reg else ('start=PDATA entry' if s in known_entry else ('start inside PDATA fn 0x%08X'%in_pdata(s) if in_pdata(s) else 'start not registered, not in PDATA'))
    return dict(region=f'{r0:08X}-{r1:08X}',seg=f'{s:08X}-{e:08X}',seg_end_reason=why,seg_start_status=status, t_off=t-s,
        prev_of_t=A.mnem(w(t-4)), seg_start_prev=A.mnem(w(s-4)) if s>lo else '-')
pairs=[tuple(int(x,16) for x in l.split()) for l in open(PAIRS)]
cls={int(r['target'],16):r for r in csv.DictReader(open(CLS),delimiter='\t')}
rows=[]
for t,src in pairs:
    d=describe(t); f=unres.get((src,t))
    d['src_fn']=f'{f:08X}' if f else '?'
    d['src_fn_kind']='GAP_FILL' if f in reg else ('PDATA' if f in known_entry else '?')
    d['class']=cls[t]['class']; d['t']=f'{t:08X}'; d['src']=f'{src:08X}'
    rows.append(d)
cnt=collections.Counter((r['class'],r['seg_start_status'].split(' 0x')[0],r['seg_end_reason'] if r['seg_end_reason'] in('region end',) else r['seg_end_reason'].split()[0]) for r in rows)
print('\n(class, segment-start status, segment end) counts:')
for k,v in sorted(cnt.items()): print(f'  {v:2d}  {k}')
print('\nsource function kind:',collections.Counter(r['src_fn_kind'] for r in rows))
with open(sys.argv[6],'w') as f:
    keys=['t','src','class','region','seg','seg_end_reason','seg_start_status','t_off','prev_of_t','seg_start_prev','src_fn','src_fn_kind']
    f.write('\t'.join(keys)+'\n')
    for r in rows: f.write('\t'.join(str(r[k]) for k in keys)+'\n')
print('\n15 second-order exits:')
for x in [0x823B4400,0x823AEAD0,0x823AD038,0x823AEB08,0x823B0BD0,0x823AEB30,0x823AEB38,0x823AEC40,0x823B2B60,0x823AEC80,0x823B2BA0,0x823AECB8,0x823AEDE0,0x823E0120,0x8259F4F8]:
    d=describe(x); print(f'  {x:08X}: seg {d["seg"]} +0x{d["t_off"]:X} end={d["seg_end_reason"]}; {d["seg_start_status"]}; registered_exactly={x in reg}')
