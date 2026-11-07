import re,sys,glob,os
lst=[l.strip().upper() for l in open(sys.argv[1]) if l.strip()]
d=sys.argv[2]; new=[]
files={f:open(f).read() for f in glob.glob(d+'/ds2_overrides.*.toml')}
for a in lst:
    pat=re.compile(r'^("0x%s"\s*=\s*\{)([^}]*)\}'%a, re.M|re.I)
    hit=False
    for f,t in files.items():
        m=pat.search(t)
        if m:
            if 'share_registers' in m.group(2): hit=True; break
            body=m.group(2).strip()
            rep=m.group(1)+(' '+body+', share_registers = true }' if body else ' share_registers = true }')
            files[f]=t[:m.start()]+rep+t[m.end():]; hit=True; break
    if not hit: new.append(a)
for f,t in files.items(): open(f,'w').write(t)
p=d+'/ds2_overrides.share.toml'
old=open(p).read() if os.path.exists(p) else '# RC97: functions that read r14-r31/cr/ctr/xer before writing them (tools lee_antes): keep registers in ctx.\n\n[functions]\n'
open(p,'w').write(old+''.join('"0x%s" = { share_registers = true }\n'%a for a in new))
print('edited',len(lst)-len(new),'new',len(new))
