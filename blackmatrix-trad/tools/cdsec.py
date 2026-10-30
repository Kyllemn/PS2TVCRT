"""Setor CD Modo 1 (2352) com EDC/ECC (algoritmo do ECM, Neill Corlett)."""
F=[0]*256; B=[0]*256; E=[0]*256
for i in range(256):
    j=((i<<1)^(0x11D if i&0x80 else 0))&0xff
    F[i]=j; B[i^j]=i
    e=i
    for _ in range(8): e=(e>>1)^(0xD8018001 if e&1 else 0)
    E[i]=e
def edc(data):
    e=0
    for b in data: e=(e>>8)^E[(e^b)&0xff]
    return e
def _ecc(s,base,major_count,minor_count,major_mult,minor_inc,dest):
    size=major_count*minor_count
    for major in range(major_count):
        index=(major>>1)*major_mult+(major&1); a=0; b=0
        for _ in range(minor_count):
            t=s[base+index]; index+=minor_inc
            if index>=size: index-=size
            a^=t; b^=t; a=F[a]
        a=B[F[a]^b]
        s[dest+major]=a; s[dest+major+major_count]=a^b
def bcd(x): return ((x//10)<<4)|(x%10)
def mode1(lba,data):
    assert len(data)==2048
    s=bytearray(2352)
    s[0]=0; s[1:11]=b'\xff'*10; s[11]=0
    f=lba+150; s[12]=bcd(f//4500); s[13]=bcd((f//75)%60); s[14]=bcd(f%75); s[15]=1
    s[16:16+2048]=data
    e=edc(s[0:0x810]); s[0x810:0x814]=e.to_bytes(4,'little')
    _ecc(s,0xC,86,24,2,86,0x81C)
    _ecc(s,0xC,52,43,86,88,0x81C+0xAC)
    return bytes(s)
