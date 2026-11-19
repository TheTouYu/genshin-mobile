#!/usr/bin/env python3
"""列出 YuanShen.exe 中所有 E8/E9 rel32 指向指定 VA 的调用点。用法: callers.py VA [VA...]"""
import struct, sys, collections
P='/home/h/.ys/YuanShen.exe'
d=open(P,'rb').read()
pe=struct.unpack_from('<I',d,0x3c)[0]
nsec=struct.unpack_from('<H',d,pe+6)[0]
opt=pe+24
ib=struct.unpack_from('<Q',d,opt+24)[0]
sechdr=opt+struct.unpack_from('<H',d,pe+20)[0]
secs=[]
for i in range(nsec):
    o=sechdr+i*40
    name=d[o:o+8].rstrip(b'\0').decode('latin1')
    vs,va,rs,ra=struct.unpack_from('<IIII',d,o+8)
    secs.append((name,va,vs,ra,rs))
sites=collections.defaultdict(list)
for name,sva,vs,ra,rs in secs:
    if name not in ('.text','il2cpp'): continue
    blob=d[ra:ra+rs]
    n=len(blob)
    for i in range(n-5):
        b=blob[i]
        if b==0xE8 or b==0xE9:
            tgt=ib+sva+i+5+struct.unpack_from('<i',blob,i+1)[0]
            sites[tgt].append(('call' if b==0xE8 else 'jmp', ib+sva+i))
for a in sys.argv[1:]:
    va=int(a,16)
    lst=sites.get(va,[])
    print(f'== {va:#x} callers={len(lst)}')
    for kind,site in lst[:20]:
        print(f'   {kind} @ {site:#x}  (rva {site-ib:#x})')
