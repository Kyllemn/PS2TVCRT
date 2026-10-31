"""Desenha texto em pixel nas texturas: cores em degrade (cima->baixo), contorno opcional,
italico opcional, aperta na horizontal quando nao cabe."""
import numpy as np
from PIL import Image, ImageDraw, ImageFont
REG='/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
BOLD='/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
def mascara(t,alt,ital,fonte=BOLD):
    for sz in range(24,6,-1):
        F=ImageFont.truetype(fonte,sz); m=Image.new('L',(200,50),0); ImageDraw.Draw(m).text((4,4),'H',font=F,fill=255)
        r=np.where(np.array(m).any(1))[0]
        if r[-1]-r[0]+1<=alt: break
    m=Image.new('L',(300,50),0); ImageDraw.Draw(m).text((8,4),t,font=F,fill=255)
    if ital: m=m.transform(m.size,Image.AFFINE,(1,0.22,-4,0,1,0),Image.BICUBIC)
    a=np.array(m)>110; ys,xs=np.where(a)
    return a[r[0]-3:, xs.min():xs.max()+1], 3
def escreve(img,x0,y0,x1,y1,t,cores,cont,ital,alt,limpa=True,alinha='c',fonte=None,margem=0):
    H=y1-y0; W=x1-x0-(2 if cont else 0)-2*margem
    a,desl=mascara(t,alt,ital,fonte or BOLD)
    if fonte is None and a.shape[1]>W*1.12: a,desl=mascara(t,alt,ital,REG)
    if a.shape[1]>W:
        a=np.array(Image.fromarray((a*255).astype(np.uint8)).resize((W,a.shape[0]),Image.BOX))>100
    if limpa: img[y0:y1,x0:x1]=0
    py=y0+(H-alt)//2-desl
    px=x0+margem+(1 if cont else 0)+({'c':(W-a.shape[1])//2,'d':W-a.shape[1]}.get(alinha,0))
    ys,xs=np.where(a)
    if cont:
        for dy in(-1,0,1):
            for dx in(-1,0,1):
                Y,X=py+ys+dy,px+xs+dx; k=(Y>=y0)&(Y<y1)&(X>=x0)&(X<x1)
                img[Y[k],X[k],:3]=cont; img[Y[k],X[k],3]=255
    for y,x in zip(ys,xs):
        Y,X=py+y,px+x
        if y0<=Y<y1 and x0<=X<x1:
            c=cores[min(len(cores)-1,max(0,(y-desl)*len(cores)//alt))]
            img[Y,X,:3]=c; img[Y,X,3]=255
