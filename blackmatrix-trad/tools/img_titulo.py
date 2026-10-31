"""Tela de titulo (SYSTEM.DAT, PAC do bloco 1, segunda SCL em 0x11ba00):
acrescenta o credito da traducao embaixo de 'Disc 1'. Desenha em 640x480
e volta para 640x240 tirando a media de cada par de linhas.
uso: img_titulo.py tools SYSTEM saida [previa.png]"""
import sys, numpy as np
sys.path.insert(0, sys.argv[1]); import scl
from PIL import Image, ImageDraw, ImageFont, ImageFilter
src, out = sys.argv[2:4]
A,B=0x11ba00,0x17c590
d=bytearray(open(src,'rb').read()); blk=bytes(d[A:B]); assert blk[:4]==b'SCL '
offs,texs,tab=scl.ler(blk); img=scl.monta(texs,tab)
TXT='Tradução: Leonardo Torres'
big=np.repeat(img,2,0).astype(np.float64)                     # 640x480
F=ImageFont.truetype('/usr/share/fonts/truetype/freefont/FreeSerif.ttf',22)
m=Image.new('L',(640,480),0); dr=ImageDraw.Draw(m)
w=dr.textlength(TXT,font=F); x=600-w; y=262                   # alinhado a direita do 'Disc 1'
dr.text((x,y),TXT,font=F,fill=255)
som=np.array(m.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(1.5)),np.float64)/255
let=np.array(m,np.float64)/255
for c in range(3): big[...,c]=big[...,c]*(1-0.85*som)           # sombra escura em volta
cor=(236,222,222)
for c in range(3): big[...,c]=big[...,c]*(1-let)+cor[c]*let
novo=((big[0::2]+big[1::2])/2).round().clip(0,255).astype(np.uint8); novo[...,3]=img[...,3]
nt,ntab,n=scl.reconstroi(novo,texs,tab)
d[A:B]=scl.grava(blk,offs,nt,ntab)
open(out,'wb').write(bytes(d)); print('titulo ok, blocos',n)
if len(sys.argv)>4: Image.fromarray(np.repeat(novo,2,0)).save(sys.argv[4])
