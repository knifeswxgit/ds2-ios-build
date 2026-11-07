# RC97: sub_X(ctx, base); -> __imp__sub_X(ctx, base); for functions without a hook (adapted from tools/llamadas_directas.py).
import os,re,sys
gen=sys.argv[1]; R='/home/claude/ds2_switch'
hooked=set()
for root in [R+'/app/src',R+'/sdk/src',R+'/sdk/include',R+'/app/CMakeLists.txt']:
    paths=[root] if os.path.isfile(root) else [os.path.join(d,f) for d,_,fs in os.walk(root) for f in fs if not f.endswith(('.a','.o','.png','.jpg','.bin'))]
    for p in paths:
        try: t=open(p,encoding='utf-8',errors='ignore').read()
        except OSError: continue
        for m in re.findall(r'(?<![0-9A-Fa-f])(82[0-9A-Fa-f]{6})(?![0-9A-Fa-f])',t): hooked.add('sub_'+m.upper())
call=re.compile(r'(?<![\w])sub_([0-9A-F]{8})\(ctx, base\);')
n=0;files=0
for f in sorted(os.listdir(gen)):
    if not (f.startswith('ds2_recomp.') and f.endswith('.cpp')): continue
    p=os.path.join(gen,f); t=open(p).read()
    def ch(m):
        global n
        if 'sub_'+m.group(1) in hooked: return m.group(0)
        n+=1; return '__imp__sub_%s(ctx, base);'%m.group(1)
    u=call.sub(ch,t)
    if u!=t: open(p,'w',newline='\n').write(u); files+=1
print('direct',n,'files',files,'hooked',len(hooked)); open(gen+'/../hooked97.txt','w').write('\n'.join(sorted(hooked)))
