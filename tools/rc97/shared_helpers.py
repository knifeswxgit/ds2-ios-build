import re,glob,sys
gen=sys.argv[1]
helpers={}
for l in open(gen+'/ds2_init.cpp'):
    m=re.search(r'\{ 0x([0-9A-F]{8}), (__(?:save|rest)\w+) \}',l)
    if m: helpers[m.group(1).lower()]=m.group(2)
fn_re=re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]+)\)')
ctxnv=re.compile(r'ctx\.(r(1[4-9]|2\d|3[01])|f(1[4-9]|2\d|3[01]))\.')
res=[]
for f in glob.glob(gen+'/ds2_recomp.*.cpp'):
    cur=None
    for ln in open(f):
        m=fn_re.match(ln)
        if m: cur=m.group(1); shared=False; hs=set(); continue
        if not cur: continue
        if 's_r' not in ln and 'ctx.r' in ln and ctxnv.search(ln) and 'const auto s_' not in ln and not re.match(r'\s*r\d+ = ctx\.r\d+;',ln) and not re.match(r'\s*ctx\.(r|f)\d+ = s_',ln) and not re.match(r'\s*ctx\.(r|f)\d+ = (r|f)\d+;',ln): shared=True
        m=re.search(r'// (bl|b) 0x([0-9a-f]{8})',ln)
        if m and m.group(2) in helpers: hs.add(helpers[m.group(2)])
        if ln.startswith('}'):
            if shared: res.append((cur,sorted(hs)))
            cur=None
print('shared functions:',len(res)); print('with helpers:',sum(1 for r in res if r[1]))
for n,h in res:
    if h: print(n,' '.join(h))
