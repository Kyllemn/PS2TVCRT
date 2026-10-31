"""Itens de campo (EVENT.DAT, tabela IDCO em 0xe3000): registros de 0x20 bytes,
u16 tamanho em +0x10 e nome em +0x12 (9 celulas). Usa o mesmo mapa de pares.
uso: idco.py tools EVENT trad/idco_pt.tsv mapa.json saida"""
import sys, struct, json
sys.path.insert(0, sys.argv[1]); import pares
_, src, tsv, mapf, out = sys.argv[1:6]
d=bytearray(open(src,'rb').read()); mapa=json.load(open(mapf))
livres=[c for c in pares.livres() if c not in set(mapa.values())]
def cod(p):
    if p not in mapa: mapa[p]=livres.pop(0)
    return mapa[p]
T={}
for l in open(tsv,encoding='utf-8'):
    a,b=l.rstrip('\n').split('\t'); T[a]=b
O=0xe3000; assert d[O:O+4]==b'IDCO'
n=0; vistos=set(); i=0
while True:
    r=O+i*0x20; ln,=struct.unpack('<H',d[r+0x10:r+0x12])
    if not 0<ln<=9: break
    c=struct.unpack('<9H',d[r+0x12:r+0x24])
    jp=b''.join(struct.pack('>H',x) for x in c[:ln]).decode('shift_jis')
    pt=T[jp]; e=[cod(pt[k:k+2].ljust(2)) for k in range(0,len(pt),2)]
    assert len(e)<=9,(jp,pt)
    d[r+0x10:r+0x24]=struct.pack('<H9H',len(e),*(e+[0x8140]*(9-len(e))))
    n+=1; vistos.add(jp); i+=1
assert vistos==set(T),set(T)-vistos
open(out,'wb').write(bytes(d)); json.dump(mapa,open(mapf,'w'),ensure_ascii=False,indent=0)
print('itens de campo',n,'| nomes',len(vistos),'| pares no mapa',len(mapa))
