"""Icones de tipo de ataque das armas (SYSTEM.DAT, PVRT 0x21890, linha y16-27, 6 icones de 16px):
縦横突殴鞭弓 -> VE HO ES IM CH AR (vertical, horizontal, estocada, impacto, chicote, arco).
Letras 5x7 desenhadas a mao, com o degrade e a sombra do original.
uso: img_icone.py tools SYSTEM_entrada SYSTEM_saida [previa.png]"""
import sys, numpy as np
sys.path.insert(0, sys.argv[1]); import pvr
src, out = sys.argv[2:4]; prev=sys.argv[4] if len(sys.argv)>4 else None
G={'V':'10001 10001 10001 10001 01010 01010 00100','E':'11111 10000 10000 11110 10000 10000 11111',
   'H':'10001 10001 10001 11111 10001 10001 10001','O':'01110 10001 10001 10001 10001 10001 01110',
   'S':'01111 10000 10000 01110 00001 00001 11110','I':'111 010 010 010 010 010 111',
   'M':'10001 11011 10101 10101 10001 10001 10001','C':'01111 10000 10000 10000 10000 10000 01111',
   'A':'01110 10001 10001 11111 10001 10001 10001','R':'11110 10001 10001 11110 10100 10010 10001'}
SIG=['VE','HO','ES','IM','CH','AR']
TOPO=np.array([224,128,64]); BASE=np.array([144,64,32]); SOMBRA=(112,48,16)
OFF=0x21890
d=bytearray(open(src,'rb').read()); pf,df,w,h=pvr.head(d,OFF); img=pvr.decode(d,OFF)
for i,s in enumerate(SIG):
    x0=i*16; cel=img[16:28,x0:x0+15]
    assert (cel[...,3]==255).all()
    cel[...,:3]=0
    gl=[np.array([[int(c) for c in r] for r in G[ch].split()]) for ch in s]
    larg=sum(g.shape[1] for g in gl)+1; px=(15-larg)//2; py=2
    m=np.zeros((12,15),bool)
    for g in gl:
        m[py:py+7,px:px+g.shape[1]]|=g.astype(bool); px+=g.shape[1]+1
    som=np.zeros_like(m); som[1:,1:]=m[:-1,:-1]; som&=~m
    cel[som,:3]=(56,24,8)
    for y in range(12):
        t=min(1,max(0,(y-py)/6)); cor=(TOPO*(1-t)+BASE*t).astype(int)
        cel[y][m[y],:3]=cor
    img[16:28,x0:x0+15]=cel
d[OFF+16:OFF+16+w*h*2]=pvr.encode(img,pf,df)
open(out,'wb').write(bytes(d)); print('icones ok')
if prev:
    from PIL import Image
    v=pvr.decode(d,OFF)[0:48,0:112]; Image.fromarray(v).resize((896,384),Image.NEAREST).save(prev)
