"""Tela de escolha do parceiro (SYSTEM.DAT bloco 3, SCL): apaga o japones (reconstroi o fundo)
e escreve o texto em PT. uso: img_escolha.py tools SYSTEM_entrada SYSTEM_saida [previa.png]"""
import sys, struct, numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
sys.path.insert(0, sys.argv[1]); import scl
src, out = sys.argv[2:4]; prev=sys.argv[4] if len(sys.argv)>4 else None
K=3; TEXTO='Escolha seu mestre'
d=bytearray(open(src,'rb').read())
s,ln=struct.unpack('<II',d[16+K*8:24+K*8]); blk=bytes(d[s*2048:(s+ln)*2048])
offs,texs,tab=scl.ler(blk); a=np.repeat(scl.monta(texs,tab)[...,:3],2,0).astype(float)
y0,y1,x0,x1=196,286,40,604
b=a[y0:y1,x0:x1]; ver=(b[...,0]>b[...,1]+40)
m=Image.fromarray((ver*255).astype(np.uint8)).filter(ImageFilter.MaxFilter(7)); mask=np.array(m)>0
b[mask]=0
for _ in range(400):
    viz=(np.roll(b,1,0)+np.roll(b,-1,0)+np.roll(b,1,1)+np.roll(b,-1,1))/4
    b[mask]=viz[mask]
a[y0:y1,x0:x1]=b
im=Image.fromarray(a.astype(np.uint8))
f=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',46)
w=ImageDraw.Draw(im).textlength(TEXTO,font=f); x=320-w/2; y=212
mk=Image.new('L',(im.width+40,im.height),0); ImageDraw.Draw(mk).text((x,y),TEXTO,font=f,fill=255)
mk=mk.transform(mk.size,Image.AFFINE,(1,0.25,-60,0,1,0),Image.BICUBIC).crop((0,0,im.width,im.height))
cont=mk.filter(ImageFilter.MaxFilter(5))
im.paste(Image.new('RGB',im.size,(30,4,0)),(0,0),cont)
grad=np.zeros((im.height,im.width,3),np.uint8)
for yy in range(im.height):
    t=min(1,max(0,(yy-y)/50)); grad[yy]=(int(170-80*t),int(22-14*t),0)
im.paste(Image.fromarray(grad),(0,0),mk)
a=np.array(im).astype(np.int32); meia=((a[0::2]+a[1::2])//2).astype(np.uint8)
img=np.dstack([meia,np.full(meia.shape[:2],255,np.uint8)])
nt,ntab,n=scl.reconstroi(img,texs,tab); novo=scl.grava(blk,offs,nt,ntab); d[s*2048:s*2048+len(novo)]=novo
if prev: Image.fromarray(np.repeat(meia,2,0)).save(prev)
open(out,'wb').write(bytes(d)); print('escolha ok, blocos',n)
