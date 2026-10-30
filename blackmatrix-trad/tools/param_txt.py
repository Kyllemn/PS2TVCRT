"""Extrai os campos de texto do PARAMETA.DAT: [u16 tamanho][campo de N celulas]."""
import struct, sys
sys.path.insert(0, sys.argv[2]); import acha_txt
d=open(sys.argv[1],'rb').read()
n=struct.unpack('<I',d[8:12])[0]; out=open(sys.argv[3],'w',encoding='utf-8'); tot=0; uniq={}
for k in range(n):
    s,l=struct.unpack('<II',d[16+k*8:24+k*8]); b=d[s*2048:(s+l)*2048]
    if k in (10,11): continue          # graficos
    for o,cnt,prev,t in acha_txt.achar(b):
        if prev is None: continue
        # campo valido: palavra anterior = tamanho do texto (<= campo)
        fld=cnt
        if not (1<=prev<=fld+2): continue
        absoff=s*2048+o
        vis=t.rstrip('　')
        out.write('%d\t%06x\t%d\t%d\t%s\n'%(k,absoff,fld,prev,t)); tot+=1
        uniq.setdefault(vis,0); uniq[vis]+=1
print('campos',tot,'unicos',len(uniq),'caracteres unicos',sum(len(x) for x in uniq))
