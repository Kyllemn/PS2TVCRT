"""Gera glifos de 2 letras (32x16) para a fonte MFD e o mapa par->codigo.
Na tela, o jogo desenha o glifo de 32x16 com ~0,8x na largura e ~2x na
altura (medido na foto do t04): desenhamos na proporcao da tela e
convertemos para a textura."""
import numpy as np
from PIL import Image, ImageDraw, ImageFont
FONT='/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf'
SX,SY=0.8,2.0                 # escala textura -> tela
TW,TH=32,16                   # textura
CW,CH=int(round(TW*SX*4)),int(round(TH*SY*4))   # tela em 4x (super-amostragem)
F=ImageFont.truetype(FONT,int(CH*0.78))
def livres():
    out=[]
    for hi in range(0x89,0x98):
        for lo in list(range(0x00,0x40))+[0x7f,0xfd,0xfe]:
            out.append(hi<<8|lo)
    return out
def indice(code): return (code+0x7961)&0xffff
def glifo_par(par):
    im=Image.new('L',(CW,CH),0); d=ImageDraw.Draw(im)
    half=CW/2
    for k,ch in enumerate(par):
        bb=d.textbbox((0,0),ch,font=F)
        w=bb[2]-bb[0]
        x=k*half+(half-w)/2-bb[0]
        d.text((x,CH*0.06),ch,font=F,fill=255)
    a=np.array(im.resize((TW,TH),Image.BOX),dtype=np.float32)
    g=np.zeros((TH,TW),np.uint8); g[a>=80]=1
    return g
def tela(g):
    """simula a tela: textura 32x16 -> proporcao real"""
    im=Image.fromarray((g*255).astype(np.uint8))
    return np.array(im.resize((int(TW*SX*2),int(TH*SY*2)),Image.NEAREST))
def preview(linhas,arq):
    rows=[]
    for t in linhas:
        ps=[t[i:i+2].ljust(2) for i in range(0,len(t),2)]
        r=np.hstack([tela(glifo_par(p)) for p in ps]); rows.append(np.pad(r,((0,6),(0,1000-r.shape[1]))))
    Image.fromarray(np.vstack(rows)).save(arq)
