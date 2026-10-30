"""Traduz as texturas de configuracao/ajuda do SYSTEM.DAT (bloco 8).
uso: img_sys.py tools SYSTEM_entrada SYSTEM_saida"""
import sys, numpy as np
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, sys.argv[1]); import pvr
src, out = sys.argv[2:4]
SERIF='/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf'
SANS='/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
# (offset do PVRT, [(x0,y0,x1,y1, texto, fonte, tamanho, cor, fundo)])
TEX={
 0x6294d0:[ (136,0,227,12,'Mostrar',SERIF,15,(230,230,230),None),
            (103,16,216,28,'Ocultar',SERIF,15,(230,230,230),None),
            (127,32,222,44,'Manual',SERIF,15,(230,230,230),None),
            (87,48,182,60,'Padrão',SERIF,15,(230,230,230),None) ],
 0x6494f0:[ (0,0,53,14,'Exibir',SANS,13,(255,255,255),None),
            (56,0,136,14,'Ocultar',SANS,13,(255,255,255),None),
            (136,0,231,14,'Loja',SANS,13,(255,255,255),None),
            (0,16,82,30,'Status',SANS,13,(255,255,255),None),
            (88,16,188,30,'Inventário',SANS,13,(255,255,255),None),
            (0,32,82,46,'Ritual',SANS,13,(255,255,255),None),
            (88,32,168,46,'Sair',SANS,13,(255,255,255),None),
            (168,32,249,46,'Partir',SANS,13,(255,255,255),None),
            (0,48,83,62,'Equipe',SANS,13,(255,255,255),None),
            (88,48,186,62,'Dividir BP',SANS,13,(255,255,255),None),
            (0,64,111,78,'Pos. inicial',SANS,13,(255,255,255),None),
            (113,64,195,78,'Mover',SANS,13,(255,255,255),None),
            (0,80,82,94,'Atacar',SANS,13,(255,255,255),None),
            (88,80,171,94,'Magia',SANS,13,(255,255,255),None),
            (176,80,256,94,'Itens',SANS,13,(255,255,255),None),
            (0,96,82,110,'Sangue',SANS,13,(255,255,255),None),
            (88,96,168,110,'Reação',SANS,13,(255,255,255),None),
            (0,112,82,126,'Vestir',SANS,13,(255,255,255),None),
            (88,112,197,126,'Vitória',SANS,13,(255,255,255),None),
            (0,128,149,141,'Subir nível',SANS,13,(255,255,255),None),
            (0,144,247,160,'Itens da ajuda',SANS,14,(255,255,255),'bg') ],
}
ESC=np.array([57,57,57])
def escreve(img,x0,y0,x1,y1,t,fp,sz,cor,fundo):
    if fundo=='bg':
        bgc=img[y0+1,x1-2].copy(); img[y0:y1,x0:x1]=bgc
    else: img[y0:y1,x0:x1]=0
    F=ImageFont.truetype(fp,sz); W,H=x1-x0,y1-y0
    m=Image.new('L',(W*2,H*2),0); d=ImageDraw.Draw(m); d.text((0,0),t,font=F,fill=255)
    a=np.array(m).astype(float)/255; ys,xs=np.where(a>0.15)
    a=a[:, xs.min():xs.max()+1]; tw=a.shape[1]
    assert tw<=W, (t,tw,W)
    top=int(np.where(np.array(Image.new('L',(1,1))) is None)[0]) if False else 0
    # alinhamento vertical pela altura de maiuscula
    mm=Image.new('L',(40,H*2),0); ImageDraw.Draw(mm).text((0,0),'H',font=F,fill=255)
    hr=np.where(np.array(mm).any(1))[0]; cap0,cap1=hr[0],hr[-1]
    py=y0+(H-(cap1-cap0+1))//2-cap0; px=x0+(W-tw)//2 if fundo=='bg' else x0+1
    for yy in range(a.shape[0]):
        for xx in range(tw):
            v=a[yy,xx]; Y,X=py+yy,px+xx
            if v<0.25 or not (y0<=Y<y1 and x0<=X<x1): continue
            c=(np.array(cor)*v+ESC*(1-v)).astype(np.uint8)
            img[Y,X,:3]=c; img[Y,X,3]=255
d=bytearray(open(src,'rb').read())
for off,itens in TEX.items():
    pf,df,w,h=pvr.head(d,off); img=pvr.decode(d,off).copy()
    for it in itens: escreve(img,*it)
    d[off+16:off+16+w*h*2]=pvr.encode(img,pf,df)
    Image.fromarray(img).save('%s_%x.png'%(out,off))
open(out,'wb').write(bytes(d)); print('texturas SYSTEM ok')
