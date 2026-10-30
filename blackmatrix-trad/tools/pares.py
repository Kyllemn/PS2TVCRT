"""Gera glifos de 2 letras (32x16) para a fonte MFD e o mapa par->codigo."""
import numpy as np
from PIL import Image, ImageDraw, ImageFont
FONT='/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf'
F=ImageFont.truetype(FONT,26)
def livres():
    # codigos com byte baixo invalido no Shift-JIS dentro da faixa de kanji:
    # nenhum texto japones usa; o indice da fonte e linear nessa faixa.
    out=[]
    for hi in range(0x89,0x98):
        for lo in list(range(0x00,0x40))+[0x7f,0xfd,0xfe]:
            out.append(hi<<8|lo)
    return out
def indice(code): return (code+0x7961)&0xffff
def glifo_par(par):
    # desenha em 32x32 (pixel quadrado) e reduz a altura para 16:
    # na TV (640x240) cada linha da fonte aparece com o dobro da altura.
    im=Image.new('L',(32,32),0); d=ImageDraw.Draw(im)
    for k,ch in enumerate(par):
        w=d.textlength(ch,font=F)
        x=k*16+(16-w)/2
        d.text((x,1),ch,font=F,fill=255)
    a=np.array(im,dtype=np.float32).reshape(16,2,32).mean(axis=1)
    g=np.zeros((16,32),np.uint8); g[a>=96]=1
    return g
def preview(pares,arq):
    rows=[np.hstack([glifo_par(p) for p in pares[i:i+16]]) for i in range(0,len(pares)-len(pares)%16 or 16,16) if pares[i:i+16]]
    w=max(r.shape[1] for r in rows)
    rows=[np.pad(r,((0,0),(0,w-r.shape[1]))) for r in rows]
    a=np.vstack(rows)*255
    Image.fromarray(a.astype(np.uint8)).resize((w*2,a.shape[0]*4),Image.NEAREST).save(arq)
