"""Procura sequencias de caracteres Shift-JIS em u16 (bytes trocados) num arquivo."""
import struct, sys
def ok(v):
    if v in (0xa0ff,0x8140): return True
    h=v>>8
    if not (0x81<=h<=0x9f or 0xe0<=h<=0xea): return False
    try: bytes([h,v&0xff]).decode('shift_jis'); return True
    except: return False
def achar(b,minlen=2):
    w=struct.unpack('<%dH'%(len(b)//2),b[:len(b)//2*2]); i=0; out=[]
    while i<len(w):
        if ok(w[i]):
            j=i
            while j<len(w) and ok(w[j]): j+=1
            t=''.join('/' if v==0xa0ff else bytes([v>>8,v&0xff]).decode('shift_jis') for v in w[i:j])
            if j-i>=minlen and any('぀'<=c<='ヿ' or '一'<=c<='鿿' for c in t):
                out.append((i*2,j-i,w[i-1] if i else None,t))
            i=j
        else: i+=1
    return out
if __name__=='__main__':
    d=open(sys.argv[1],'rb').read()
    n=struct.unpack('<I',d[8:12])[0]
    for k in range(n):
        s,l=struct.unpack('<II',d[16+k*8:24+k*8]); b=d[s*2048:(s+l)*2048]
        r=achar(b)
        if r: print('== entrada',k,b[:16],len(r),'textos'); [print('   %06x n=%d prev=%s %s'%(o,n,hex(p) if p is not None else '-',t)) for o,n,p,t in r[:12]]
