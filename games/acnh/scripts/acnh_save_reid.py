"""Test saves only (never the live save): give a copied ACNH save its own island and player identity: replaces the island ID, player ID and island name in
every encrypted .dat (and landname.dat), recomputes the Murmur3 regions and re-encrypts with the same header.
Build the region finder first: clang -O2 -o scripts/acnh_save_regions scripts/acnh_save_regions.c
usage: acnh_save_reid.py <save/device dir> <old town hex> <new town hex> <old player hex> <new player hex> <old name> <new name>"""
import os, struct, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from acnh_save_crypt import crypt
here = os.path.dirname(os.path.abspath(__file__))
def mm3(b):
    c1,c2,M=0xcc9e2d51,0x1b873593,0xffffffff; h=0
    rot=lambda x,r:((x<<r)|(x>>(32-r)))&M
    for (k,) in struct.iter_unpack('<I', b[:len(b)//4*4]):
        k=(k*c1)&M; k=rot(k,15); k=(k*c2)&M; h^=k; h=rot(h,13); h=(h*5+0xe6546b64)&M
    assert len(b)%4==0
    h^=len(b); h^=h>>16; h=(h*0x85ebca6b)&M; h^=h>>13; h=(h*0xc2b2ae35)&M; h^=h>>16; return h
def regions(path):
    out=subprocess.run([os.path.join(here,'acnh_save_regions'),path,'0x100',hex(os.path.getsize(path))],capture_output=True,text=True,check=True).stdout
    return [(int(l.split('@')[1].split()[0],16), int(l.split('len ')[1],16)) for l in out.splitlines()]
d, ot, nt, op, np_, on, nn = sys.argv[1:8]
assert len(on)==len(nn)
reps=[(bytes.fromhex(ot),bytes.fromhex(nt)),(bytes.fromhex(op),bytes.fromhex(np_)),(on.encode('utf-16le'),nn.encode('utf-16le'))]
for root,_,files in os.walk(d):
    for f in sorted(files):
        if not f.endswith('.dat') or f.endswith('Header.dat'): continue
        p=os.path.join(root,f); hp=p[:-4]+'Header.dat'
        if f=='landname.dat':
            b=open(p,'rb').read(); nb=b.replace(reps[2][0],reps[2][1]); open(p,'wb').write(nb); print(f,'name replaced',b!=nb); continue
        if not os.path.exists(hp): print('skip',p); continue
        h=open(hp,'rb').read(); dec=crypt(h,open(p,'rb').read())
        tmp=p+'.dec'; open(tmp,'wb').write(dec); regs=regions(tmp); os.remove(tmp)
        cover=sum(l+4 for _,l in regs)
        new=bytearray(dec); counts=[]
        for a,b in reps: counts.append(new.count(a)); new=bytearray(bytes(new).replace(a,b))
        for o,l in regs: struct.pack_into('<I',new,o,mm3(bytes(new[o+4:o+4+l])))
        for o,l in regs: assert struct.unpack_from('<I',new,o)[0]==mm3(bytes(new[o+4:o+4+l]))
        open(p,'wb').write(crypt(h,bytes(new)))
        print(os.path.relpath(p,d),'regions',len(regs),'covered %d/%d'%(cover,len(dec)-0x110),'replaced town/player/name',counts)
