import re,glob,sys
gen=sys.argv[1]; flagged={}
for l in open('lee1f.txt'):
    if 'setFromMask' in l: continue
    n=l.split()[0]; regs=l.split('|')[0].split()[1:]; flagged[n]=regs
helpers={}
for l in open(gen+'/ds2_init.cpp'):
    m=re.search(r'\{ 0x([0-9A-F]{8}), (__(?:save|rest)\w+) \}',l)
    if m: helpers[m.group(1).lower()]=m.group(2)
fn_re=re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]+)\)')
bodies={}
for f in glob.glob(gen+'/ds2_recomp.*.cpp'):
    cur=None
    for ln in open(f):
        m=fn_re.match(ln)
        if m and m.group(1) in flagged: cur=m.group(1); bodies[cur]=[]; continue
        if cur:
            bodies[cur].append(ln.rstrip())
            if ln.startswith('}'): cur=None
for n,regs in flagged.items():
    b=bodies.get(n,[]); asm=[l.strip()[3:] for l in b if l.strip().startswith('// ')][:14]
    saves=set(); frame=False
    for a in asm:
        m=re.match(r'bl 0x([0-9a-f]+)',a)
        if m and m.group(1) in helpers:
            h=helpers[m.group(1)]; k=int(h.rsplit('_',1)[1])
            if h.startswith('__savegprlr'): saves|={'r%d'%i for i in range(k,32)}
            if h.startswith('__savefpr'): saves|={'f%d'%i for i in range(k,32)}
            if h.startswith('__savevmx'): saves|={'v%d'%i for i in (range(k,32) if k<64 else range(k,128))}
        m=re.match(r'(std|stw|stfd) ([rf]\d+),-?\d+\(r1\)',a)
        if m: saves.add(m.group(2))
        m=re.match(r'stvx128 (v\d+),r1',a)
        if m: saves.add(m.group(1))
        if a.startswith('stwu r1,') or a.startswith('stdu r1,'): frame=True
    nv=[r for r in regs if r[0] in 'rfv']
    owned=frame and all(r in saves for r in nv)
    print('OWN' if owned else 'PIECE', n, ' '.join(regs), '| frame' if frame else '| noframe', 'saves:',len(saves), '|', ' ; '.join(asm[:4])[:110])
