"""Traduz telas 'SCL ' do EVENT.DAT (instrucoes dos minijogos, legendas de tempo, fins de capitulo).
Edita em 640x480 (a tela real e 640x240 com linhas dobradas) e remonta os blocos 16x16.
uso: img_scl.py tools EVENT_entrada EVENT_saida [pasta_previa]"""
import sys, os, struct, numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
sys.path.insert(0, sys.argv[1]); import scl
src, out = sys.argv[2:4]; prev=sys.argv[4] if len(sys.argv)>4 else None
SANS='/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
SERIF='/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf'
BR=(255,255,255); VM=(150,36,18); CZ=(246,242,246)
VERDE=(41,141,0); AZUL=(0,85,222); AMAR=(222,178,0)
def F(fp,sz): return ImageFont.truetype(fp,sz)
def txt(dr,x,y,t,sz,cor,fonte=SANS,al='e',larg=None):
    f=F(fonte,sz)
    while larg and dr.textlength(t,font=f)>larg and sz>10: sz-=1; f=F(fonte,sz)
    w=dr.textlength(t,font=f)
    if al=='c': x=x-w/2
    elif al=='d': x=x-w
    dr.text((x,y),t,font=f,fill=cor); return x+w
def seg(dr,x,y,partes,sz):
    for t,c in partes: x=txt(dr,x,y,t,sz,c)
def neon(im,x,y,t,cor,sz=44):
    f=F(SANS,sz); m=Image.new('L',im.size,0); ImageDraw.Draw(m).text((x,y),t,font=f,fill=255)
    halo=m.filter(ImageFilter.GaussianBlur(5)); halo=Image.eval(halo,lambda v:min(255,v*3))
    cam=Image.new('RGB',im.size,cor); im.paste(cam,(0,0),halo)
    borda=m.filter(ImageFilter.MaxFilter(3)); im.paste(Image.new('RGB',im.size,cor),(0,0),borda)
    im.paste(Image.new('RGB',im.size,(255,255,255)),(0,0),m)
def apaga(a,x0,y0,x1,y1): a[y0:y1,x0:x1]=0
TITULOS={30:('Pesca',(70,110,255)),31:('Caça',(40,220,60)),32:('Lenha',(255,150,30)),33:('Culinária',(235,40,60))}
def instrucao(k,a):
    ic={}
    if k==30: ic['seta']=a[336:356,226:244].copy(); ic['mol']=a[362:402,75:108].copy()
    if k==33: ic['bal']=a[332:396,74:140].copy()
    apaga(a,20,20,620,{30:104,31:128,32:125,33:104}[k]); apaga(a,{33:326}.get(k,362),120,630,292); apaga(a,20,286,630,445)
    if k==31: apaga(a,20,200,206,236)
    im=Image.fromarray(a.astype(np.uint8)); dr=ImageDraw.Draw(im)
    t,c=TITULOS[k]; neon(im,70,36,'"%s"'%t,c); dr=ImageDraw.Draw(im)
    txt(dr,612,52,'Controles',30,CZ,al='d')
    NAO='Não usado'
    if k==30:
        for y in (130,166,242): txt(dr,366,y,NAO,22,VM)
        txt(dr,366,204,'Puxar a vara',22,BR)
        txt(dr,35,294,'1. Espere o peixe fisgar.',22,BR)
        txt(dr,35,331,'2. Fisgou? Pare',22,BR,larg=186); txt(dr,250,331,'o cursor com o botão A.',22,BR,larg=370)
        txt(dr,35,370,'3.',22,BR); txt(dr,114,370,'apareceu? Aperte várias vezes',22,BR,larg=500)
        txt(dr,114,394,'para puxar o peixe.',22,BR)
        txt(dr,35,418,'※ O peixe depende de onde o cursor parou.',20,BR,larg=590)
        a2=np.array(im); a2[336:356,226:244]=ic['seta']; a2[362:402,75:108]=ic['mol']; return a2
    if k==31:
        for y in (155,191,263): txt(dr,366,y,NAO,22,VM)
        txt(dr,366,227,'Atirar flecha',22,BR)
        txt(dr,200,206,'Mover',22,BR,al='d')
        txt(dr,60,340,'Acerte com a flecha, no momento certo,',22,BR,larg=560)
        txt(dr,60,368,'os animais que saem de trás das árvores.',22,BR,larg=560)
    if k==32:
        for y in (156,192,264): txt(dr,366,y,NAO,22,VM)
        txt(dr,366,228,'Descer o machado',22,BR)
        txt(dr,45,354,'Enquanto ergue o machado, desça-o no',22,BR,larg=570)
        txt(dr,45,382,'momento certo para rachar a lenha.',22,BR,larg=570)
    if k==33:
        for y,(nome,cor) in zip((130,166,203,240),[('verdes',VERDE),('azuis',AZUL),('vermelhos',VM),('amarelos',AMAR)]):
            seg(dr,330,y,[('Ingredientes ',BR),(nome,cor)],22)
        txt(dr,35,304,'1. Os ingredientes aparecem na ordem de preparo.',21,BR,larg=590)
        txt(dr,35,352,'2.',21,BR); txt(dr,148,348,'mostra o ingrediente: aperte',21,BR,larg=470)
        txt(dr,148,376,'o botão da mesma cor para cozinhar.',21,BR,larg=470)
        a2=np.array(im); a2[332:396,74:140]=ic['bal']; return a2
    return np.array(im)
CAP={224:('Capítulo 1','Jardim de Pandora'),282:('Capítulo 2','Prisão de Gólgota'),314:('Capítulo 3','Templo do Sol'),
     332:('Capítulo 4','No meio da jornada'),340:('Capítulo 5','Cidade da Ganância'),350:('Capítulo 6','Cidade da Gula')}
LEG={218:'No dia seguinte',219:'Uma semana depois'}
def preto(k,a):
    a[:]=0; im=Image.fromarray(a.astype(np.uint8)); dr=ImageDraw.Draw(im)
    if k in LEG: txt(dr,320,212,LEG[k],50,BR,SERIF,al='c')
    else:
        c,t=CAP[k]; txt(dr,51,28,c,28,BR,SERIF); txt(dr,320,206,'“%s”'%t,36,BR,SERIF,al='c',larg=600); txt(dr,611,412,'Fim',30,BR,SERIF,al='d')
    return np.array(im)
d=bytearray(open(src,'rb').read())
for k in list(TITULOS)+list(CAP)+list(LEG):
    s,ln=struct.unpack('<II',d[16+k*8:24+k*8]); blk=bytes(d[s*2048:(s+ln)*2048])
    offs,texs,tab=scl.ler(blk); orig=scl.monta(texs,tab)
    a=np.repeat(orig[...,:3],2,0).astype(np.int32)
    a=instrucao(k,a) if k in TITULOS else preto(k,a)
    a=a.astype(np.int32); meia=((a[0::2]+a[1::2])//2).astype(np.uint8)
    img=np.dstack([meia,np.full(meia.shape[:2],255,np.uint8)])
    nt,ntab,n=scl.reconstroi(img,texs,tab)
    novo=scl.grava(blk,offs,nt,ntab); d[s*2048:s*2048+len(novo)]=novo
    if prev: Image.fromarray(np.repeat(meia,2,0)).save(os.path.join(prev,'scl_%d.png'%k))
    print(k,'blocos',n)
open(out,'wb').write(bytes(d)); print('telas SCL ok')
