"""Cartoes de abertura de capitulo (EVENT.DAT, telas SCL 140-145): apaga o japones
(texto preto com contorno branco por cima do selo), reconstroi o fundo e escreve em PT.
uso: img_abertura.py tools EVENT_entrada EVENT_saida [pasta_previa]"""
import sys, os, struct, numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
sys.path.insert(0, sys.argv[1]); import scl
src, out = sys.argv[2:4]; prev=sys.argv[4] if len(sys.argv)>4 else None
SERIF='/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf'
CAP={140:'Jardim de Pandora',141:'Prisão de Gólgota',142:'Templo do Sol',
     143:'No meio da jornada',144:'Cidade da Ganância',145:'Cidade da Gula'}
ROT={k:'Capítulo %d'%(k-139) for k in CAP}
import os, json
if os.environ.get('CAPITULOS'):   # disco 2: {"bloco": ["rotulo", "titulo"], ...}
    J=json.load(open(os.environ['CAPITULOS'],encoding='utf-8'))
    CAP={int(k):v[1] for k,v in J.items()}; ROT={int(k):v[0] for k,v in J.items()}
Y0,Y1,X0,X1=140,346,96,548          # area do texto em 640x480
NX0,NX1=X0,X1   # area normal; AREA_X alarga so as margens, onde so apaga pixel quase preto (letra), nao o fundo escuro
if os.environ.get('AREA_X'): X0,X1=map(int,os.environ['AREA_X'].split(','))
def linha(im,t,y,sz,larg):
    f=ImageFont.truetype(SERIF,sz)
    while ImageDraw.Draw(im).textlength(t,font=f)>larg: sz-=1; f=ImageFont.truetype(SERIF,sz)
    w=ImageDraw.Draw(im).textlength(t,font=f)
    m=Image.new('L',im.size,0); ImageDraw.Draw(m).text((320-w/2,y),t,font=f,fill=255)
    im.paste(Image.new('RGB',im.size,(255,255,255)),(0,0),m.filter(ImageFilter.MaxFilter(7)))
    im.paste(Image.new('RGB',im.size,(0,0,0)),(0,0),m)
d=bytearray(open(src,'rb').read())
for k,titulo in CAP.items():
    s,ln=struct.unpack('<II',d[16+k*8:24+k*8]); blk=bytes(d[s*2048:(s+ln)*2048]); assert blk[:4]==b'SCL '
    offs,texs,tab=scl.ler(blk); a=np.repeat(scl.monta(texs,tab)[...,:3],2,0).astype(float)
    b=a[Y0:Y1,X0:X1]; esc=b.sum(2)<110
    if (X0,X1)!=(NX0,NX1):
        xs=np.arange(X0,X1); marg=(xs<NX0)|(xs>=NX1)
        esc[:,marg]=(b[:,marg].sum(2)<40)
        ys=np.arange(Y0,Y1); esc[(ys<220)|(ys>330),:]&=~marg
    mask=np.array(Image.fromarray((esc*255).astype(np.uint8)).filter(ImageFilter.MaxFilter(11)))>0
    b[mask]=b[~mask].mean(0)
    for _ in range(400):
        viz=(np.roll(b,1,0)+np.roll(b,-1,0)+np.roll(b,1,1)+np.roll(b,-1,1))/4
        b[mask]=viz[mask]
    a[Y0:Y1,X0:X1]=b
    im=Image.fromarray(a.astype(np.uint8))
    linha(im,ROT[k],156,58,400)
    if '/' in titulo:
        t1,t2=titulo.split('/'); linha(im,t1,236,44,430); linha(im,t2,288,44,430)
    else: linha(im,titulo,256,54,430)
    a=np.array(im).astype(np.int32); meia=((a[0::2]+a[1::2])//2).astype(np.uint8)
    img=np.dstack([meia,np.full(meia.shape[:2],255,np.uint8)])
    nt,ntab,n=scl.reconstroi(img,texs,tab); novo=scl.grava(blk,offs,nt,ntab); d[s*2048:s*2048+len(novo)]=novo
    if prev: Image.fromarray(np.repeat(meia,2,0)).save(os.path.join(prev,'abertura_%d.png'%k))
    print(k,'blocos',n)
open(out,'wb').write(bytes(d)); print('aberturas ok')
