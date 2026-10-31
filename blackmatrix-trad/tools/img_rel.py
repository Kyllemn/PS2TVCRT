"""Relatorio de batalha (PARAMETA.DAT, PVRT em 0x1bfdd0): rotulos em japones.
uso: img_rel.py tools PARAMETA_entrada PARAMETA_saida"""
import sys
sys.path.insert(0, sys.argv[1]); import pvr, pinta
src, out = sys.argv[2:4]
OFF=0x1bfdd0
SERIF='/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf'
OURO=[(246,180,57),(238,164,49),(222,148,41),(222,139,32),(213,131,24),(197,115,16),(189,98,8)]
ITENS=[(0,112,120,128,'ACUMULADO:'),(0,160,156,176,'TEMPO:'),(0,176,112,192,'TURNOS:')]
d=bytearray(open(src,'rb').read())
pf,df,w,h=pvr.head(d,OFF); img=pvr.decode(d,OFF).copy()
for x0,y0,x1,y1,t in ITENS: pinta.escreve(img,x0,y0,x1,y1,t,OURO,(8,0,0),False,11,alinha='e',fonte=SERIF)
d[OFF+16:OFF+16+w*h*2]=pvr.encode(img,pf,df)
open(out,'wb').write(bytes(d)); print('relatorio ok')
