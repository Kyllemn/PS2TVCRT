"""Wp16 (Square): LZ em palavras de 16 bits. Flags de 32 bits (LSB primeiro, 1=literal), backref d<<5|c:
d palavras para tras (0=2048), c+2 palavras; janela inicial de 2048 palavras zeradas."""
import struct
def decompress(d):
    assert d[:4]==b'Wp16'
    size=struct.unpack('<I',d[4:8])[0]
    nw=(size+1)//2
    out=[0]*2048; p=8
    while len(out)-2048<nw:
        flags=struct.unpack('<I',d[p:p+4])[0]; p+=4
        for i in range(32):
            if len(out)-2048>=nw: break
            w=d[p]|d[p+1]<<8; p+=2
            if flags&1: out.append(w)
            else:
                c=(w&31)+2; b=(w>>5)&0x7ff or 0x800
                s=len(out)-b
                for j in range(c): out.append(out[s+j])
            flags>>=1
    b=bytearray()
    for w in out[2048:2048+nw]: b+=struct.pack('<H',w)
    return bytes(b[:size]), p

def compress(data):
    """Compressor guloso (busca de casamentos em janela de 2047 palavras)."""
    if len(data)%2: data=data+b'\0'
    words=list(struct.unpack('<%dH'%(len(data)//2),data))
    win=[0]*2048+words
    out=bytearray(b'Wp16'+struct.pack('<I',len(data)))
    i=2048; n=len(win)
    from collections import defaultdict
    idx=defaultdict(list)
    for k in range(0,2048): idx[(win[k],win[k+1])].append(k)
    group=[]; flags=0
    def flush():
        nonlocal group,flags
        if group:
            out.extend(struct.pack('<I',flags))
            for g in group: out.extend(struct.pack('<H',g))
        group=[]; flags=0
    while i<n:
        best=0; bpos=0
        if i+1<n:
            for k in reversed(idx.get((win[i],win[i+1]),[])):
                if i-k>2047: break
                l=0
                while l<33 and i+l<n and win[k+l]==win[i+l]: l+=1
                if l>best: best=l; bpos=k
                if l==33: break
        if best>=2:
            group.append(((i-bpos)<<5)|(best-2))
            adv=best
        else:
            flags|=1<<len(group); group.append(win[i]); adv=1
        for t in range(adv):
            if i+t+1<n: idx[(win[i+t],win[i+t+1])].append(i+t)
        i+=adv
        if len(group)==32: flush()
    flush()
    return bytes(out)
