"""Traduz a lista de capitulos e Porta/Slot (tela de salvar), SYSTEM.DAT PVRT em 0x608810.
uso: img_cap.py tools SYSTEM_entrada SYSTEM_saida"""
import sys
sys.path.insert(0, sys.argv[1]); import pvr, pinta
from PIL import Image
src, out = sys.argv[2:4]
OFF=0x608810
COR=[(230,164,49),(213,139,32),(197,115,16),(180,90,8)]
ITENS=[(28,16*i,83,16*i+15,'Cap. %d'%(i+1),'d') for i in range(10)]+[(0,160,83,175,'Cap. 11','d')]
ITENS+=[(0,176,83,191,'Cap. final','d'),(0,192,80,207,'Porta','e'),(0,208,80,223,'Slot','e')]
d=bytearray(open(src,'rb').read())
pf,df,w,h=pvr.head(d,OFF); img=pvr.decode(d,OFF).copy()
for x0,y0,x1,y1,t,al in ITENS: pinta.escreve(img,x0,y0,x1,y1,t,COR,None,False,10,alinha=al,fonte=pinta.REG)
d[OFF+16:OFF+16+w*h*2]=pvr.encode(img,pf,df)
open(out,'wb').write(bytes(d)); Image.fromarray(img).save(out+'.png'); print('capitulos ok')
