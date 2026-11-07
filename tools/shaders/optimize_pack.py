import struct, subprocess, sys, os, tempfile, xxhash
from concurrent.futures import ProcessPoolExecutor
SRC='/home/claude/swtc/rc75pack/ds2_shaders_rc75.spvlib'
DST='/home/claude/swtc/rc89pack/ds2_shaders_rc89.spvlib'
P="--merge-return --inline-entry-points-exhaustive --eliminate-dead-functions --private-to-local --eliminate-local-single-block --eliminate-local-single-store --scalar-replacement=100 --convert-local-access-chains --ssa-rewrite --ccp --loop-invariant-code-motion --eliminate-dead-code-aggressive --simplify-instructions --redundancy-elimination --vector-dce --eliminate-dead-inserts --eliminate-dead-branches --merge-blocks --eliminate-dead-code-aggressive --cfg-cleanup".split()
def opt(spv):
    d=tempfile.mkdtemp(dir='/tmp/claude-0')
    a=os.path.join(d,'a.spv'); b=os.path.join(d,'b.spv')
    open(a,'wb').write(spv)
    ok=subprocess.run(['spirv-opt']+P+[a,'-o',b],capture_output=True).returncode==0 and \
       subprocess.run(['spirv-val','--target-env','vulkan1.2','--scalar-block-layout',b],capture_output=True).returncode==0
    out=open(b,'rb').read() if ok else spv
    for f in (a,b):
        try: os.remove(f)
        except OSError: pass
    os.rmdir(d)
    if len(out)%4 or len(out)>=len(spv): return spv, False
    return out, True
def work(item):
    hdr, payload = item
    r=list(struct.unpack('<10Q',hdr))
    u=r[4]*4; s=r[5]
    spv=payload[u:u+s]
    new, changed = opt(spv)
    if changed:
        payload=payload[:u]+new+payload[u+s:]
        r[5]=len(new); r[9]=xxhash.xxh3_64_intdigest(payload)
    return struct.pack('<10Q',*r)+payload, changed, s, len(new)
def records(f):
    while True:
        h=f.read(80)
        if len(h)<80: return
        r=struct.unpack('<10Q',h)
        size=r[4]*4+r[5]+(r[6]+r[7])*4
        yield h, f.read(size)
f=open(SRC,'rb'); out=open(DST,'wb'); out.write(f.read(16))
n=chg=b0=b1=0
import itertools
with ProcessPoolExecutor(2) as ex:
    it=records(f)
    while True:
        batch=list(itertools.islice(it,256))
        if not batch: break
        for rec,changed,s0,s1 in ex.map(work, batch, chunksize=8):
            out.write(rec); n+=1; chg+=changed; b0+=s0; b1+=s1 if changed else s0
        if n%2048==0: print(n,chg,b0>>20,b1>>20,flush=True)
out.close(); print('done',n,chg,b0>>20,b1>>20,flush=True)
