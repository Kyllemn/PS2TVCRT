"""Telas de troca de disco (SYSTEM.DAT blocos 5 e 6: PAC com duas telas SCL).
uso: img_disco.py tools SYSTEM_entrada SYSTEM_saida [pasta_previa]"""
import sys, os, struct, numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
sys.path.insert(0, sys.argv[1]); import scl
src, out = sys.argv[2:4]; prev=sys.argv[4] if len(sys.argv)>4 else None
F=lambda sz: ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',sz)
NOTA=['※ Aperte o botão OPEN, abra a tampa','e troque o disco.']
def telas(n):
    ins='Insira o disco %d.'%n
    return {0x40:[((222,259,75,565),[ins],'c',34),((368,413,69,569),NOTA,'e',19)],
            0x60a40:[((44,99,263,376),['AVISO'],'c',44),
                     ((186,245,109,532),['Este não é o disco de BLACK/MATRIX','ou não há disco no aparelho.'],'c',20),
                     ((292,329,75,565),[ins],'c',34),((368,413,69,569),NOTA,'e',19)]}
def apaga(a,y0,y1,x0,x1):
    y0-=6;y1+=7;x0-=8;x1+=9; b=a[y0:y1,x0:x1]
    m=np.array(Image.fromarray(((b.min(2)>110)*255).astype(np.uint8)).filter(ImageFilter.MaxFilter(5)))>0
    b[m]=0
    for _ in range(300):
        v=(np.roll(b,1,0)+np.roll(b,-1,0)+np.roll(b,1,1)+np.roll(b,-1,1))/4; b[m]=v[m]
    a[y0:y1,x0:x1]=b
d=bytearray(open(src,'rb').read())
for k,ndisco in ((5,1),(6,2)):
    s,ln=struct.unpack('<II',d[16+k*8:24+k*8]); base=s*2048
    for a0,itens in telas(ndisco).items():
        a1={0x40:0x60a40,0x60a40:0xc1440}[a0]; sub=bytes(d[base+a0:base+a1])
        offs,texs,tab=scl.ler(sub); a=np.repeat(scl.monta(texs,tab)[...,:3],2,0).astype(float)
        for (y0,y1,x0,x1),_,_,_ in itens: apaga(a,y0,y1,x0,x1)
        im=Image.fromarray(a.astype(np.uint8)); dr=ImageDraw.Draw(im)
        for (y0,y1,x0,x1),linhas,al,sz in itens:
            f=F(sz); hl=(y1-y0+1)/len(linhas)
            for i,t in enumerate(linhas):
                w=dr.textlength(t,font=f); x=320-w/2 if al=='c' else x0
                y=y0+i*hl+(hl-sz*1.15)/2
                dr.text((x+2,y+2),t,font=f,fill=(20,0,0)); dr.text((x,y),t,font=f,fill=(240,240,240))
        b=np.array(im).astype(np.int32); meia=((b[0::2]+b[1::2])//2).astype(np.uint8)
        img=np.dstack([meia,np.full(meia.shape[:2],255,np.uint8)])
        nt,ntab,n=scl.reconstroi(img,texs,tab); novo=scl.grava(sub,offs,nt,ntab)
        assert len(novo)==len(sub); d[base+a0:base+a1]=novo
        if prev: Image.fromarray(np.repeat(meia,2,0)).save(os.path.join(prev,'disco%d_%x.png'%(k,a0)))
        print('disco',ndisco,hex(a0),'blocos',n)
open(out,'wb').write(bytes(d)); print('troca de disco ok')
