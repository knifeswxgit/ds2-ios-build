# RC97: guest memory without volatile; compiler barriers on loop back-edges and at sync/lwsync/eieio.
import os,re,sys
gen=sys.argv[1]
pch=os.path.join(gen,'ds2_pch.h'); t=open(pch).read()
n0=0
out=[]
for ln in t.split('\n'):
    if re.match(r'#define REX_(LOAD|STORE)_U(8|16|32|64)\(',ln) and '(volatile u' in ln:
        ln=ln.replace('(volatile u','(u'); n0+=1
    out.append(ln)
t='\n'.join(out)
assert n0==8,n0
t=t.replace('#define REX_MEMORY_SIZE','// RC97: compiler barrier (loop back-edges, sync/lwsync/eieio).\n#define REX_LOOP_BARRIER() __asm__ __volatile__("" ::: "memory")\n\n#define REX_MEMORY_SIZE',1)
assert 'REX_LOOP_BARRIER' in t
open(pch,'w').write(t)
fn=re.compile(r'^DEFINE_REX_FUNC\(')
lab=re.compile(r'^(loc_[0-9A-F]+):')
gt=re.compile(r'(?<![\w{])goto (loc_[0-9A-F]+);')
syn=re.compile(r'^\t// (sync|lwsync|eieio|isync)\b')
back=syncs=0
for f in sorted(os.listdir(gen)):
    if not (f.startswith('ds2_recomp.') and f.endswith('.cpp')): continue
    p=os.path.join(gen,f); lines=open(p).read().split('\n'); seen=set(); res=[]
    for ln in lines:
        if fn.match(ln): seen=set()
        m=lab.match(ln)
        if m: seen.add(m.group(1))
        if 'goto loc_' in ln:
            def ch(mm):
                global back
                if mm.group(1) in seen:
                    back+=1; return '{ REX_LOOP_BARRIER(); goto %s; }'%mm.group(1)
                return mm.group(0)
            ln=gt.sub(ch,ln)
        res.append(ln)
        if syn.match(ln): res.append('\tREX_LOOP_BARRIER();'); syncs+=1
    open(p,'w',newline='\n').write('\n'.join(res))
print('pch macros',n0,'back-edges',back,'syncs',syncs)
