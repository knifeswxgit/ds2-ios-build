#!/usr/bin/env python3
# DS2 Switch tool: Summarise rex_perfil.log (per-frame GPU/CPU counters) from the console. Usage: perf.py <rex_perfil.log>
import re,sys
txt=open(sys.argv[1],errors='replace').read()
blocks=re.split(r'\n(?===== )',txt)
t=0
print(f"{'#':>3} {'t':>5} {'fps':>5} {'cpu':>4} {'main':>5} | {'gpu':>6} {'xfer':>5} {'res':>5} {'n':>3} {'tex':>4} {'tdD':>5} {'tdC':>5} {'draw':>5} {'nd':>4} | {'draws':>5} {'resPx':>5} {'xD':>4} {'xC':>4} {'fence_ms':>8} {'retr':>4}")
for i,b in enumerate(blocks):
    m=re.match(r'==== ([\d.]+) s \| modo: ([^|]*)\| juego ([\d.]+) fps \| CPU total (\d+)%',b)
    if not m: continue
    t+=float(m.group(1)); fps=float(m.group(3)); cpu=int(m.group(4))
    g=re.search(r'GPU ms por envio medido.*?total ([\d.]+) \| transferencias \(todo\) ([\d.]+).*?\| resolves ([\d.]+) \(([\d.]+) x.*?carga de texturas ([\d.]+).*?profundidad ([\d.]+).*?a color ([\d.]+).*?dibujados del juego ([\d.]+) \(([\d.]+) x',b)
    pf=re.search(r'por fotograma: (\d+) dibujados.*?([\d.]+) resolves \(([\d.]+) pantallas',b)
    td=re.search(r'pixeles x pasadas: profundidad ([\d.]+) pantallas, color ([\d.]+)',b)
    mt=re.search(r'hilo \d+ "Main XThread[^"]*": CPU ([\d.]+)%',b)
    fe=re.search(r'esperadas (\d+) \(([\d.]+) ms\)',b)
    gv=[float(x) for x in g.groups()] if g else [0]*9
    print(f"{i:>3} {t:>5.0f} {fps:>5.1f} {cpu:>4} {float(mt.group(1)) if mt else 0:>5.1f} | {gv[0]:>6.1f} {gv[1]:>5.1f} {gv[2]:>5.1f} {gv[3]:>3.0f} {gv[4]:>4.1f} {gv[5]:>5.1f} {gv[6]:>5.1f} {gv[7]:>5.1f} {gv[8]:>4.0f} | {pf.group(1) if pf else '':>5} {pf.group(3) if pf else '':>5} {td.group(1) if td else '':>4} {td.group(2) if td else '':>4} {fe.group(2) if fe else '':>8}")
