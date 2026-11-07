#!/usr/bin/env python3
# DS2 Switch tool: Unpack FromSoftware BND4 archives (shader/*.bnd) into files. Usage: bnd4.py <file.bnd> <out_dir>. Output is game data: never commit it.
import struct,sys,os,re
def bnd4(d):
    assert d[:4]==b'BND4'
    be=d[9]==1
    E='>' if be else '<'
    n,=struct.unpack_from(E+'i',d,0x0C)
    fhs,=struct.unpack_from(E+'q',d,0x20)
    unicode=d[0x30]; fmt=d[0x31]
    rev=lambda b: int('{:08b}'.format(b)[::-1],2)
    # SoulsFormats: format byte may be bit-reversed when big endian? (BitBigEndian)
    ents=[]
    for i in range(n):
        p=0x40+i*fhs
        pp=p+8
        csize,=struct.unpack_from(E+'q',d,pp); pp+=8
        usize=csize
        f=fmt
        if fhs>=0x24: usize,=struct.unpack_from(E+'q',d,pp); pp+=8
        if fhs in (0x24,0x20,0x1C) or True:
            doff,=struct.unpack_from(E+'I',d,pp); pp+=4
        fid,=struct.unpack_from(E+'i',d,pp); pp+=4
        noff,=struct.unpack_from(E+'I',d,pp); pp+=4
        if unicode:
            e=noff
            while d[e:e+2]!=b'\0\0': e+=2
            name=d[noff:e].decode('utf-16-be' if be else 'utf-16-le')
        else:
            name=d[noff:d.index(b'\0',noff)].decode('cp932')
        ents.append((name,doff,csize,usize))
    return ents
if __name__=='__main__':
    src,out=sys.argv[1],sys.argv[2]
    d=open(src,'rb').read()
    e=bnd4(d)
    os.makedirs(out,exist_ok=True)
    for name,off,cs,us in e:
        b=d[off:off+cs]
        import zlib
        if b[:2]==b'\x78\x9c' or b[:2]==b'\x78\xda':
            b=zlib.decompress(b)
        rel=name.replace('\\','/')
        rel=re.sub(r'^[A-Za-z]:/','',rel).lstrip('/')
        fn=os.path.join(out,rel)
        os.makedirs(os.path.dirname(fn),exist_ok=True)
        open(fn,'wb').write(b)
    print(src,len(e),'files; first',e[0][0],d[e[0][1]:e[0][1]+4].hex(),'sizes',e[0][2],e[0][3])
