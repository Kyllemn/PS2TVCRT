"""Traduz os rotulos dos menus (acampamento/loja) em SYSTEM.DAT, PVRT em 0x21890.
uso: img_menu.py tools SYSTEM_entrada SYSTEM_saida"""
import sys
sys.path.insert(0, sys.argv[1]); import pvr, pinta
from PIL import Image
src, out = sys.argv[2:4]
OFF=0x21890
LAR=[(230,139,74),(230,139,74),(213,123,65),(172,90,41)]
ITENS=[(0,96,56,110,'Subir nível'),(110,96,141,110,'Poder'),(148,96,180,110,'Compra'),(182,96,213,110,'Vender'),
 (0,112,31,126,'Status'),(62,112,93,126,'Baú'),(110,112,141,126,'Equip.'),(142,112,160,126,'Arm.'),(160,112,217,126,'Acessório'),
 (0,128,57,142,'Equipar'),(62,128,104,142,'Remover'),
 (0,144,57,158,'Dist. auto'),(62,144,104,158,'Descartar'),
 (0,160,57,174,'Dist. manual'),(62,160,93,174,'Voltar'),
 (0,176,57,190,'Dist. num.'),(62,176,93,190,'Arma'),
 (0,192,30,206,'Magia'),(30,192,61,206,'Infusão'),(62,192,93,206,'Entra'),
 (0,208,30,222,'OK'),(30,208,61,222,'Coagul.'),(62,208,107,222,'Fica fora'),(110,208,190,222,'Consumível'),
 (0,224,30,238,'Sair'),(62,224,117,238,'Zerar'),(126,224,186,238,'Unidades')]
d=bytearray(open(src,'rb').read())
pf,df,w,h=pvr.head(d,OFF); img=pvr.decode(d,OFF).copy()
for x0,y0,x1,y1,t in ITENS: pinta.escreve(img,x0,y0,x1,y1,t,LAR,None,False,9,fonte=pinta.REG,margem=1)
d[OFF+16:OFF+16+w*h*2]=pvr.encode(img,pf,df)
open(out,'wb').write(bytes(d)); Image.fromarray(img).save(out+'.png'); print('menus ok')
