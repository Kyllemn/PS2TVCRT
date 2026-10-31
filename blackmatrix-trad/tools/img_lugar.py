"""Traduz as placas de nome de lugar (EVENT.DAT). uso: img_lugar.py tools EVENT_entrada EVENT_saida"""
import sys, numpy as np
sys.path.insert(0, sys.argv[1]); import pvr, pinta
src, out = sys.argv[2:4]
LISTA=['Ação livre','Jardim de Pandora','Cidade','Prisão de Gólgota','Colina de Gólgota','Cidade de Ribeira',
 'Templo do Sol','Pequena colina','Arena','Vestiário','Pavilhão oeste','Vale de Kederon','Casa',
 'Casa do Filipe','Cidade da Ganância/Pandemônio','Mansão de Mammon','Pátio','Quintal','Canaã, cidade da Gula',
 'Mansão de Baal','Salão principal','Cozinha','Grande salão','Fora da mansão']
d=bytearray(open(src,'rb').read())
import re
offs=[m.start() for m in re.finditer(b'PVRT',d)]
# as placas: textura com texto branco no topo e a barra marrom em x>=250
alvo=[]
for o in offs:
    try: pf,df,w,h=pvr.head(d,o)
    except: continue
    if (w,h)!=(256,256) or pf!=0: continue
    im=pvr.decode(d,o)
    if tuple(im[10,252,:3])==(90,49,8) and im[0:90,0:248,3].any() and not im[200:256,0:248,3].any(): alvo.append(o)
import hashlib
unicos=[]
for o in alvo:
    h=hashlib.md5(bytes(d[o+16:o+16+131072])).hexdigest()
    if h not in unicos: unicos.append(h)
assert len(unicos)==len(LISTA),len(unicos)
nome={h:t for h,t in zip(unicos,LISTA)}
BR=[(213,213,213),(213,213,213),(205,205,205),(156,156,156)]
for o in alvo:
    t=nome[hashlib.md5(bytes(d[o+16:o+16+131072])).hexdigest()]
    pf,df,w,h=pvr.head(d,o); img=pvr.decode(d,o).copy()
    a=img[0:90,0:248,3]>0; ys=np.where(a.any(1))[0]; y0,y1=ys.min(),ys.max()+1
    img[0:90,0:248]=0
    linhas=t.split('/'); hl=(y1-y0)//len(linhas)
    for i,ln in enumerate(linhas):
        pinta.escreve(img,2,y0+i*hl,246,y0+(i+1)*hl,ln,BR,(8,8,8),False,min(15,hl-6),limpa=False)
    d[o+16:o+16+w*h*2]=pvr.encode(img,pf,df)
open(out,'wb').write(bytes(d)); print('placas',len(alvo))
