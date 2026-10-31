"""Procura texto japones (Shift-JIS em u16, ordem little-endian) que sobrou nos arquivos.
Filtro anti-ruido: a sequencia precisa ter >=2 letras validas e (tamanho u16 antes dela igual
ao numero de letras, ou >=3 kana). uso: varre_jp.py arquivo... > lista.tsv"""
import sys, numpy as np
def ok(c): return 0x8140<=c<=0x84be or 0x889f<=c<=0x9872 or 0x989f<=c<=0x9ffc or 0xe040<=c<=0xeaa4
def kana(c): return 0x829f<=c<=0x8396
def ch(c):
    if c==0x8140: return '　'
    try: return bytes([c>>8,c&255]).decode('shift_jis')
    except: return '?'
for f in sys.argv[1:]:
    d=open(f,'rb').read(); vistos=set()
    for al in (0,1):
        v=np.frombuffer(d[al:len(d)-((len(d)-al)&1)],dtype='<u2').astype(np.int64)
        g=((v>=0x8140)&(v<=0x84be))|((v>=0x889f)&(v<=0x9872))|((v>=0x989f)&(v<=0x9ffc))
        idx=np.where(g)[0]
        if not len(idx): continue
        brk=np.where(np.diff(idx)!=1)[0]; ini=np.r_[idx[:1],idx[brk+1]]; fim=np.r_[idx[brk],idx[-1:]]
        for s,e in zip(ini,fim):
            r=[int(x) for x in v[s:e+1]]
            # tira espacos das pontas
            while r and r[-1]==0x8140: r.pop()
            t=0
            while t<len(r) and r[t]==0x8140: t+=1
            r=r[t:]
            if len(r)<2: continue
            nk=sum(kana(c) for c in r); pre=int(v[s-1]) if s>0 else -1
            if not (pre in (len(r),e-s+1) or nk>=3): continue
            if len(set(r))==1: continue
            txt=''.join(ch(c) for c in r)
            if '?' in txt: continue
            off=al+2*(s+t)
            if (txt,off) in vistos: continue
            vistos.add((txt,off)); print('%s\t%x\t%s'%(f.split('/')[-1],off,txt))
