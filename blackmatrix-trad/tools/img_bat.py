"""Traduz os textos de batalha (textura em CHAR.DAT, PVRT em 0x1350).
uso: img_bat.py tools CHAR_entrada CHAR_saida"""
import sys, numpy as np
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, sys.argv[1]); import pvr
src, out = sys.argv[2:4]
OFF=0x1350
REG='/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
BOLD='/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
EST={ # (cores de cima para baixo do preenchimento, contorno, italico)
 'ora':([(238,189,32),(238,189,32),(205,156,24),(180,131,16)],(41,16,0),True),
 'red':([(180,49,16),(180,49,16),(148,41,8),(123,32,16)],(16,0,0),True),
 'pen':([(180,49,16)],(16,0,0),False),
 'bra':([(213,213,213)],(8,8,8),False),
 'bri':([(213,213,213),(213,213,213),(180,180,180),(148,148,148)],(24,24,24),True),
 'cin':([(115,115,115)],(0,0,0),False),
}
# (x0,y0,x1,y1, texto, estilo, altura da letra)
ITENS=[(176,0,199,14,'Erro!','red',9),(199,0,224,14,'Pedra','ora',9),
 (176,16,200,30,'Ven.','ora',9),(200,16,224,30,'Paral.','ora',9),
 (177,32,224,46,'Status','ora',9),
 (97,48,128,62,'Defesa','bri',9),(128,48,159,62,'Sono','ora',9),
 (160,48,191,62,'Sobe','ora',9),(191,48,224,62,'Desce','ora',9),
 (112,64,145,79,'Normal','bra',10),(145,64,176,79,'Poder','bra',10),
 (144,80,185,95,'Pensando','pen',10),(185,80,216,95,'Posição','bra',10),
 (145,96,176,111,'inicial','bra',10),(177,96,224,111,'Reação','bra',10),
 (145,112,176,127,'Mudar','bra',10),(177,112,224,127,'modo','bra',10),
 (191,208,224,223,'Mover','bra',10),(191,224,224,239,'Atacar','bra',10),(191,240,224,255,'Magia','bra',10),
 (1,224,31,239,'Normal','cin',10),(1,240,31,255,'Poder','cin',10)]
def mascara(t,alt,ital,fonte=BOLD):
    for sz in range(20,6,-1):
        F=ImageFont.truetype(fonte,sz); m=Image.new('L',(200,40),0); d=ImageDraw.Draw(m); d.text((4,4),'H',font=F,fill=255)
        r=np.where(np.array(m).any(1))[0]
        if r[-1]-r[0]+1<=alt: break
    m=Image.new('L',(220,40),0); d=ImageDraw.Draw(m); d.text((8,4),t,font=F,fill=255)
    if ital: m=m.transform(m.size,Image.AFFINE,(1,0.22,-4,0,1,0),Image.BICUBIC)
    a=np.array(m)>110; ys,xs=np.where(a); top=r[0]
    return a[top-3:, xs.min():xs.max()+1], 3
def escreve(img,x0,y0,x1,y1,t,est,alt):
    cores,cont,ital=EST[est]
    H=y1-y0; W=x1-x0-2; a,desl=mascara(t,alt,ital)
    if a.shape[1]>W*1.12: a,desl=mascara(t,alt,ital,REG)
    if a.shape[1]>W:  # aperta na horizontal
        im=Image.fromarray((a*255).astype(np.uint8)).resize((W,a.shape[0]),Image.BOX); a=np.array(im)>100
    img[y0:y1,x0:x1]=0
    py=y0+(H-alt)//2-desl; px=x0+1+(W-a.shape[1])//2
    ys,xs=np.where(a)
    for dy in(-1,0,1):
        for dx in(-1,0,1):
            Y,X=py+ys+dy,px+xs+dx; k=(Y>=y0)&(Y<y1)&(X>=x0)&(X<x1)
            img[Y[k],X[k],:3]=cont; img[Y[k],X[k],3]=255
    for y,x in zip(ys,xs):
        Y,X=py+y,px+x
        if y0<=Y<y1 and x0<=X<x1:
            c=cores[min(len(cores)-1,max(0,(y-desl)*len(cores)//alt))]
            img[Y,X,:3]=c; img[Y,X,3]=255
d=bytearray(open(src,'rb').read())
pf,df,w,h=pvr.head(d,OFF); img=pvr.decode(d,OFF).copy()
for it in ITENS: escreve(img,*it)
d[OFF+16:OFF+16+w*h*2]=pvr.encode(img,pf,df)
open(out,'wb').write(bytes(d)); Image.fromarray(img).save(out+'.png'); print('batalha ok')
