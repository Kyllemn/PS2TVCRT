"""Codificador MR (imagem do logo na tela de licenca, dentro do IP.BIN) + testes de tamanho."""
from PIL import Image, ImageDraw
import struct
def mr(p):
    pal=p.getpalette(); w,h=p.size; data=list(p.getdata()); ncol=max(data)+1
    out=bytearray(); i=0; n=len(data)
    while i<n:
        r=1
        while r<0x17f and i+r<n and data[i+r]==data[i]: r+=1
        c=data[i]
        if r>0xff: out+=bytes([0x82,0x80|(r-0x100),c])
        elif r>0x7f: out+=bytes([0x81,r,c])
        elif r>1: out+=bytes([0x80|r,c])
        else: out+=bytes([c])
        i+=r
    palb=b''.join(bytes([pal[3*k+2],pal[3*k+1],pal[3*k],0]) for k in range(ncol))
    off=0x1e+len(palb)
    return b'MR'+struct.pack('<7I',off+len(out),0,off,w,h,0,ncol)+palb+out
GRAY=(192,192,192)
def compoe(src,alt,painel,nc):
    sc=(alt-(12 if painel else 0))/src.height
    lg=src.resize((round(src.width*sc),round(src.height*sc)),Image.LANCZOS)
    W=lg.width+(20 if painel else 0); H=alt
    base=Image.new('RGBA',(W,H),GRAY+(255,))
    if painel: ImageDraw.Draw(base).rounded_rectangle((0,0,W-1,H-1),radius=10,fill=(255,255,255,255),outline=(96,96,96,255),width=2)
    base.alpha_composite(lg,((W-lg.width)//2,(H-lg.height)//2))
    return base.convert('RGB').quantize(colors=nc,method=Image.MEDIANCUT,dither=Image.NONE)
