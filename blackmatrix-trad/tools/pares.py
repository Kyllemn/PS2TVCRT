"""Gera glifos de 2 letras (32x16) para a fonte MFD e o mapa par->codigo.
Na tela, o jogo desenha o glifo de 32x16 com ~0,8x na largura e ~2x na
altura (medido na foto do t04): desenhamos na proporcao da tela e
convertemos para a textura."""
import numpy as np
from PIL import Image, ImageDraw, ImageFont
FONT='/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf'
SX,SY=1.1,2.0                 # escala textura -> tela (medida nas fotos)
TW,TH=32,16                   # textura
UW=24                         # o jogo so desenha as colunas 0-23 da textura
CW,CH=int(round(UW*SX*4)),int(round(TH*SY*4))   # tela em 4x (super-amostragem)
F=ImageFont.truetype(FONT,int(CH*0.74))
F2=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',int(CH*0.74))
FINOS=set('lj')
import os
def livres():
    """Ordem: (1) codigos com byte baixo invalido no Shift-JIS (nenhum texto
    usa); (2) kanji de nivel 1 que nao aparecem em nenhum texto do jogo;
    (3) kanji que so apareciam nos textos que foram traduzidos."""
    out=[]
    for hi in range(0x89,0x98):
        for lo in list(range(0x00,0x40))+[0x7f,0xfd,0xfe]:
            out.append(hi<<8|lo)
    usados=set()
    f=os.path.join(os.path.dirname(os.path.abspath(__file__)),'kanji_usados.txt')
    if os.path.exists(f): usados={int(x,16) for x in open(f).read().split()}
    kanji=[]
    for hi in range(0x88,0x99):
        for lo in list(range(0x40,0x7f))+list(range(0x80,0xfd)):
            c=hi<<8|lo
            if 0x889f<=c<=0x9872: kanji.append(c)
    out+=[c for c in kanji if c not in usados]+[c for c in kanji if c in usados]
    return out
def indice(code): return (code+0x7961)&0xffff
def glifo_par(par):
    im=Image.new('L',(CW,CH),0); d=ImageDraw.Draw(im)
    half=CW/2
    for k,ch in enumerate(par):
        f=F2 if ch in FINOS else F
        bb=d.textbbox((0,0),ch,font=f)
        w=bb[2]-bb[0]
        x=k*half+(half-w)/2-bb[0]
        d.text((x,CH*0.06),ch,font=f,fill=255)
    a=np.array(im.resize((UW,TH),Image.BOX),dtype=np.float32)
    g=np.zeros((TH,TW),np.uint8); g[:,:UW][a>=80]=1
    # 'i' desenhado a mao: na resolucao da fonte o pingo grudava na haste
    for k,ch in enumerate(par):
        if ch in 'iíìî':
            x0=k*12; g[:,x0:x0+12]=0
            t,b=_xh()
            g[t:b+1,x0+5:x0+7]=1
            if ch=='i': g[t-3:t-1,x0+5:x0+7]=1
            elif ch=='í': g[t-3,x0+7]=1; g[t-2,x0+6]=1
            elif ch=='ì': g[t-3,x0+4]=1; g[t-2,x0+5]=1
            else: g[t-3,x0+5:x0+7]=1; g[t-2,x0+4]=1; g[t-2,x0+7]=1
    return g
_XH=None
def _xh():
    global _XH
    if _XH is None:
        im=Image.new('L',(CW,CH),0); ImageDraw.Draw(im).text((0,CH*0.06),'n',font=F,fill=255)
        a=np.array(im.resize((UW,TH),Image.BOX))>=80
        rows=np.where(a.any(axis=1))[0]; _XH=(int(rows[0]),int(rows[-1]))
    return _XH
def tela(g):
    """simula a tela: textura 32x16 -> proporcao real"""
    im=Image.fromarray((g[:,:UW]*255).astype(np.uint8))
    return np.array(im.resize((int(UW*SX*2),int(TH*SY*2)),Image.NEAREST))
def preview(linhas,arq):
    rows=[]
    for t in linhas:
        ps=[t[i:i+2].ljust(2) for i in range(0,len(t),2)]
        r=np.hstack([tela(glifo_par(p)) for p in ps]); rows.append(np.pad(r,((0,6),(0,1000-r.shape[1]))))
    Image.fromarray(np.vstack(rows)).save(arq)
