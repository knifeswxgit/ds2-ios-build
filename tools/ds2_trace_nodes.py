#!/usr/bin/env python3
"""Reads FunctionNode lines of a trace log: for each target, the nearest function below it, its block/instruction count.
Usage: python3 tools/ds2_trace_nodes.py <trace stdout log> <trace-unresolved-match.tsv>
"""
import re,sys,csv,bisect,collections
T,M=sys.argv[1],sys.argv[2]
pat=re.compile(r'FunctionNode 0x([0-9A-F]{8}) \(([^)]*)\): (\w+) with (\d+) blocks?, (\d+) (?:instructions|labels)?')
pat2=re.compile(r'FunctionNode 0x([0-9A-F]{8}) \(([^)]*)\): (\w+) with (\d+) blocks, (\d+) instructions')
last={}; states=collections.defaultdict(list)
for l in open(T,errors='replace'):
    if 'FunctionNode 0x' in l:
        m=pat2.search(l)
        if m:
            a=int(m[1],16); last[a]=(m[3],int(m[4]),int(m[5])); states[a].append(m[3])
print('function nodes with block/instr info:',len(last))
bases=sorted(last)
rows=list(csv.DictReader(open(M),delimiter='\t'))
out=collections.Counter()
for r in rows:
    t=int(r['t'],16); s=int(r['seg'][:8],16)
    i=bisect.bisect_right(bases,t)-1; nb=bases[i]; st=last[nb]
    seginfo=last.get(s)
    lin_end=nb+4*st[2]
    verdict='target beyond linear extent of nearest fn' if (st[1]==1 and lin_end<=t) else ('nearest fn multi-block (extent unknown)' if st[1]>1 else 'INSIDE?')
    out[verdict]+=1
    print(f"{r['t']} seg_start {s:08X}:{'%s %db/%di'%seginfo if seginfo else 'no node'} | nearest fn<=t {nb:08X} {st[0]} {st[1]}b/{st[2]}i ext_end {lin_end:08X} t-nb=+{t-nb:#x} | {verdict}")
print(out)
for x in [0x823B4400,0x823AEAD0,0x823AD038,0x823AEB08,0x823B0BD0,0x823AEB30,0x823AEB38,0x823AEC40,0x823B2B60,0x823AEC80,0x823B2BA0,0x823AECB8,0x823AEDE0,0x823E0120,0x8259F4F8]:
    print(f'{x:08X}', last.get(x), states.get(x))
