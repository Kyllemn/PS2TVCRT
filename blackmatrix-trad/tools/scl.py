"""Telas 'SCL ' (imagem 640x240 montada com blocos 16x16 de varias texturas 256x256).
Tabela: u32 em 0x20, 40x15; valor v -> textura v>>16, coluna (v>>12)&15, linha (v>>4)&0xff."""
import struct, re, numpy as np, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import pvr
def ler(blk):
    offs=[m.start() for m in re.finditer(b'PVRT',blk)]
    texs=[pvr.decode(blk,o) for o in offs]
    tab=np.frombuffer(blk[0x20:0x20+600*4],dtype='<u4').reshape(15,40)
    return offs,texs,tab
def monta(texs,tab):
    img=np.zeros((240,640,4),np.uint8)
    for r in range(15):
        for c in range(40):
            v=int(tab[r,c]); t=v>>16; tx=(v>>12)&15; ty=(v>>4)&0xff
            if t<len(texs): img[r*16:r*16+16,c*16:c*16+16]=texs[t][ty*16:ty*16+16,tx*16:tx*16+16]
    return img
def desmonta(img,texs,tab):
    for r in range(15):
        for c in range(40):
            v=int(tab[r,c]); t=v>>16; tx=(v>>12)&15; ty=(v>>4)&0xff
            if t<len(texs): texs[t][ty*16:ty*16+16,tx*16:tx*16+16]=img[r*16:r*16+16,c*16:c*16+16]
def reconstroi(img,texs,tab):
    """Refaz texturas+tabela a partir da imagem 640x240 (blocos iguais sao reaproveitados)."""
    cap=len(texs)*256; uni={}; nova=tab.copy(); novas=[np.zeros_like(t) for t in texs]
    for r in range(15):
        for c in range(40):
            b=img[r*16:r*16+16,c*16:c*16+16]; k=b.tobytes()
            if k not in uni:
                i=len(uni); assert i<cap,'blocos demais'
                uni[k]=i; t,s=divmod(i,256); ty,tx=divmod(s,16)
                novas[t][ty*16:ty*16+16,tx*16:tx*16+16]=b
            i=uni[k]; t,s=divmod(i,256); ty,tx=divmod(s,16)
            nova[r,c]=(t<<16)|(tx<<12)|(ty<<4)
    return novas,nova,len(uni)
def grava(blk,offs,texs,tab):
    blk=bytearray(blk)
    for o,t in zip(offs,texs):
        pf,df,w,h=pvr.head(blk,o); blk[o+16:o+16+w*h*2]=pvr.encode(t,pf,df)
    blk[0x20:0x20+600*4]=tab.astype('<u4').tobytes()
    return bytes(blk)
