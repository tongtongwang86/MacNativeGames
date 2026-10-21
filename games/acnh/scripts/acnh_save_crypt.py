"""ACNH save encryption (header-derived AES-128-CTR key and counter, as NHSE documents it): crypt(header, data)
decrypts and encrypts. usage: acnh_save_crypt.py <xHeader.dat> <x.dat> <out>"""
import struct, sys
import subprocess
M=0xffffffff
class Sead:
    def __init__(s,seed):
        a=0x6C078965; c=[0]*4; x=seed
        for i in range(4):
            x=(a*(x^(x>>30))+i+1)&M; c[i]=x
        s.c=c
    def u32(s):
        c=s.c; n=(c[0]^(c[0]<<11))&M
        c[0],c[1],c[2]=c[1],c[2],c[3]
        c[3]=(n^(n>>8)^c[3]^(c[3]>>19))&M
        return c[3]
def param(d,i):
    r=Sead(d[d[i]&0x7f]); it=(d[d[i+1]&0x7f]&0xf)+1
    for _ in range(it): r.u32(); r.u32()
    return bytes((r.u32()>>24)&0xff for _ in range(16))
def crypt(header,data):
    d=struct.unpack_from('<128I',header,0x100)
    key,ctr=param(d,0),param(d,2)
    return subprocess.run(['openssl','enc','-aes-128-ctr','-K',key.hex(),'-iv',ctr.hex(),'-nopad'],input=data,capture_output=True,check=True).stdout
if __name__=='__main__':
    h=open(sys.argv[1],'rb').read(); e=open(sys.argv[2],'rb').read()
    open(sys.argv[3],'wb').write(crypt(h,e))
