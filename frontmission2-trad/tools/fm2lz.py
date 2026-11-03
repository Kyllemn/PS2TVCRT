"""LZ do Front Mission 2 (rotina 0x8001EA30 do SLPM_873.31).
cabecalho: u32 tamanho descomprimido, u32 tamanho comprimido (com cabecalho).
comando c: 00-3F literais (c&3F)+1 | 40-7F repete o proximo byte (c&3F)+3 vezes
           80-BF copia (c&3F)+2 de dist=byte | C0-FF copia (c&3F)+2 de dist=u16le"""
import struct
def decompress(s):
    usz=struct.unpack('<I',s[:4])[0]; p=8; out=bytearray()
    while len(out)<usz:
        c=s[p]; p+=1; t=c&0xc0; n=c&0x3f
        if t==0x00: out+=s[p:p+n+1]; p+=n+1
        elif t==0x40: out+=bytes([s[p]])*(n+3); p+=1
        else:
            if t==0x80: d=s[p]; p+=1
            else: d=s[p]|(s[p+1]<<8); p+=2
            for _ in range(n+2): out.append(out[-d])
    return bytes(out),p
def compress(data,depth=128):
    """compressor guloso com avaliacao preguicosa (lazy) de 1 passo; saida compativel com decompress"""
    n=len(data); out=bytearray(); lit=bytearray(); table={}
    def flush():
        nonlocal lit
        while lit:
            k=lit[:64]; out.append(len(k)-1); out.extend(k); lit=lit[64:]
    def best(i):
        r=1
        while i+r<n and r<66 and data[i+r]==data[i]: r+=1
        bl=0; bd=0
        key=data[i:i+3]
        if len(key)==3:
            for j in reversed(table.get(key,[])[-depth:]):
                d=i-j
                if d>0xffff: break
                l=3
                while i+l<n and l<65 and data[j+l]==data[i+l]: l+=1
                if l>bl or (l==bl and d<256 and bd>=256): bl,bd=l,d
                if l==65: break
        return r,bl,bd
    def add(k):
        if k+3<=n: table.setdefault(data[k:k+3],[]).append(k)
    i=0
    while i<n:
        r,bl,bd=best(i)
        cost_ok = bl>=3 or (bl==2 and bd<256)
        if r>=3 and r>=bl:
            flush(); out+=bytes([0x40|(r-3),data[i]]); step=r
        elif cost_ok:
            # lazy: se o proximo byte da um casamento bem maior, emite literal agora
            add(i)
            r2,bl2,bd2=best(i+1) if i+1<n else (0,0,0)
            if bl2>bl+1 or r2>bl+1:
                lit.append(data[i]); i+=1; continue
            flush()
            if bd<256: out+=bytes([0x80|(bl-2),bd])
            else: out+=bytes([0xc0|(bl-2),bd&0xff,bd>>8])
            for k in range(i+1,i+bl): add(k)
            i+=bl; continue
        else:
            lit.append(data[i]); step=1
        for k in range(i,i+step): add(k)
        i+=step
    flush()
    return struct.pack('<2I',n,len(out)+8)+bytes(out)
