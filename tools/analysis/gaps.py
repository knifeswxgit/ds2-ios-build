#!/usr/bin/env python3
# DS2 Switch tool: Find gaps (stalls) between log lines / frames in a console log. Usage: gaps.py <log>
import sys,re
from datetime import datetime
ts=lambda s: datetime.strptime(s,"%Y-%m-%d %H:%M:%S.%f").timestamp()
prev=None; events=[]; n=0
pres=[]
cre=[];pres_w=[];rec=[]
for line in open(sys.argv[1],errors='replace'):
    if not line.startswith('['): continue
    t=line[1:24]
    if 'XELOG_GPU PRESENT' in line: pres.append(ts(t))
    elif 'Creating graphics pipeline' in line: cre.append((ts(t),'PS' in line))
    elif 'hit native submission' in line: pres_w.append(ts(t))
    elif 'recovered native submission' in line: rec.append(ts(t))
import bisect
print("presents",len(pres),"creations",len(cre),"pressure",len(pres_w),len(rec))
d=[b-a for a,b in zip(pres,pres[1:])]
import statistics
print("median frame",statistics.median(d))
# pressure wait durations
w=[]
j=0
for a in pres_w:
    k=bisect.bisect_left(rec,a)
    if k<len(rec): w.append(rec[k]-a)
print("pressure waits: n",len(w),"median",statistics.median(w) if w else 0,"sum",sum(w))
big=[(d[i],pres[i]) for i in range(len(d)) if d[i]>0.4]
print("gaps>0.4s:",len(big))
for g,t0 in big:
    t1=t0+g
    c=[x for x in cre if t0<=x[0]<=t1]
    pw=[x for x in pres_w if t0<=x<=t1]
    print(datetime.fromtimestamp(t0).strftime("%H:%M:%S.%f")[:-3], "gap %.2f"%g, "creations",len(c),"(PS %d)"%sum(1 for x in c if x[1]), "pressure",len(pw))
