"""Traduz os botoes do HUD (textura em EVENT.DAT, offset do PVRT 0x53310)."""
import sys, numpy as np
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, sys.argv[1]); import pvr
src, out = sys.argv[2:4]
OFF=0x53310
F=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',9)
ESQ=['Menu','Itens','Cancelar','Correr','OK','Olhar','Falar','Pegar']
DIR=['Largar','Trocar','Escolha','Comer','Dar','Usar','Fechar','Colocar']
LINHAS=[112,128,144,160,176,192,208,224]
CLARO=(222,222,222,255); ESCURO=(24,24,24,255)
_m=Image.new('L',(64,16),0); ImageDraw.Draw(_m).text((2,1),'M',font=F,fill=255)
TOPO=int(np.where(np.array(_m).any(1))[0][0])
def texto(img,w,x0,x1,y0):
    m=Image.new('L',(64,16),0); d=ImageDraw.Draw(m); d.fontmode='1'; d.text((2,1),w,font=F,fill=255)
    a=np.array(m)>0; ys,xs=np.where(a); a=a[TOPO:, xs.min():xs.max()+1]
    h,wd=a.shape; px=x0+(x1-x0-wd)//2; py=y0+1
    assert wd+2<=x1-x0, (w,wd)
    for dy in(-1,0,1):
        for dx in(-1,0,1):
            ys,xs=np.where(a); img[py+ys+dy,px+xs+dx]=ESCURO
    ys,xs=np.where(a); img[py+ys,px+xs]=CLARO
d=bytearray(open(src,'rb').read())
pf,df,w,h=pvr.head(d,OFF); img=pvr.decode(d,OFF).copy()
for i,y in enumerate(LINHAS):
    img[y-1:y+14,0:42]=0; img[y-1:y+14,42:81]=0
    texto(img,ESQ[i],0,42,y); texto(img,DIR[i],42,81,y)
d[OFF+16:OFF+16+w*h*2]=pvr.encode(img,pf,df)
open(out,'wb').write(bytes(d))
Image.fromarray(img).save(out+'.png')
print('HUD ok')
