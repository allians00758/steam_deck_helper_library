#!/usr/bin/env python3
"""Check PE exports without a Developer Command Prompt or SDK tools."""
import pathlib,struct,sys
if len(sys.argv)<3:raise SystemExit("usage: verify_pe.py FILE EXPORT...")
p=pathlib.Path(sys.argv[1]);s=p.read_bytes()
u=lambda fmt,at:struct.unpack_from(fmt,s,at)
assert s[:2]==b"MZ"
pe=u("<I",0x3c)[0];assert s[pe:pe+4]==b"PE\0\0"
n=u("<H",pe+6)[0];opt=pe+24;magic=u("<H",opt)[0]
assert magic in (0x10b,0x20b)
dd=opt+(112 if magic==0x20b else 96)
exp=u("<I",dd)[0];assert exp
sec=opt+u("<H",pe+20)[0];sections=[]
for i in range(n):
    k=sec+40*i;sz=u("<I",k+8)[0];virt=u("<I",k+12)[0];rawsz=u("<I",k+16)[0];raw=u("<I",k+20)[0]
    sections.append((virt,virt+max(sz,rawsz),raw))
def off(rva):
    for a,b,c in sections:
        if a<=rva<b:return c+rva-a
    raise ValueError("unmapped RVA "+str(rva))
d=off(exp);cnt=u("<I",d+24)[0];nrva=u("<I",d+32)[0];names=set()
for i in range(cnt):
    nr=u("<I",off(nrva)+i*4)[0];at=off(nr);end=s.index(b"\0",at);names.add(s[at:end].decode("ascii"))
for symbol in sys.argv[2:]:
    good=symbol in names
    print("EXPORT_"+symbol+"="+("PASS" if good else "FAIL"))
    if not good:raise SystemExit(2)
print("PE_EXPORTS=PASS")
