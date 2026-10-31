"""Procura texto japones fora dos pixels das texturas (PVRT) e fora do roteiro.
uso: varre_jp2.py arquivo..."""
import sys, re, struct, numpy as np, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import pvr
usados={int(x,16) for x in open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'kanji_usados.txt')).read().split()}
def plaus(c): return 0x8140<=c<=0x81fc or 0x824f<=c<=0x8396 or 0x839f<=c<=0x83d6 or c in usados or c==0xa0ff
def ch(c):
    if c==0xa0ff: return '/'
    try: return bytes([c>>8,c&255]).decode('shift_jis')
    except: return '?'
for f in sys.argv[1:]:
    d=bytearray(open(f,'rb').read())
    for m in re.finditer(b'PVRT',d):
        o=m.start()
        try: pf,df,w,h=pvr.head(d,o)
        except: continue
        if w in (8,16,32,64,128,256,512,1024) and h in (8,16,32,64,128,256,512,1024):
            d[o+16:o+16+w*h*2]=b'\0'*min(w*h*2,len(d)-o-16)
    for al in (0,1):
        v=np.frombuffer(bytes(d[al:len(d)-((len(d)-al)&1)]),dtype='<u2').astype(np.int64)
        i=0; n=len(v)
        while i<n:
            if plaus(int(v[i])) and v[i]!=0x8140:
                s=i
                while i<n and plaus(int(v[i])): i+=1
                r=[int(x) for x in v[s:i]]
                while r and r[-1] in (0x8140,0xa0ff): r.pop()
                nk=sum(0x829f<=c<=0x8396 or 0x889f<=c<=0x9872 for c in r)
                pre=int(v[s-1]) if s else -1; pos=int(v[i]) if i<n else -1
                if len(r)>=2 and nk>=2 and (pre==len(r) or pos in (0,0xffff) or nk>=4):
                    print('%s\t%x\t%s'%(os.path.basename(f),al+2*s,''.join(ch(c) for c in r)))
            else: i+=1
