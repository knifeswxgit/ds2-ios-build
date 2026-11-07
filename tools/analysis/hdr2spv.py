#!/usr/bin/env python3
# DS2 Switch tool: Extract a SPIR-V blob from a generated C header (shaders/vulkan_spirv/*.h). Usage: hdr2spv.py <header> <out.spv>
import re,sys,struct
s=open(sys.argv[1]).read()
s=s[s.find('#endif'):]
m=re.search(r'\{(.*?)\}', s, re.S)
vals=[int(v,16) for v in re.findall(r'0x[0-9A-Fa-f]+', m.group(1))]
open(sys.argv[2],'wb').write(struct.pack('<%dI'%len(vals),*vals))
print(len(vals))
