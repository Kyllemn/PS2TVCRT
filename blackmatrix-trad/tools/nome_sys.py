"""Traduz (registros [u16 tamanho][7 celulas]) a tela de nome (SYSTEM.DAT): rotulos do teclado e nomes padrao, com a fonte de pares.
uso: nome_sys.py tools SYSTEM_entrada mapa.json SYSTEM_saida"""
import sys, json
sys.path.insert(0, sys.argv[1]); import pares
src, mapf, out = sys.argv[2:5]
d=bytearray(open(src,'rb').read()); mapa=json.load(open(mapf))
usados=set(mapa.values()); livres=[c for c in pares.livres() if c not in usados]
def cod(p):
    if p not in mapa: mapa[p]=livres.pop(0)
    return mapa[p]
def enc(t,n):
    r=[cod(t[i:i+2].ljust(2)) for i in range(0,len(t),2)]
    assert len(r)<=n,(t,n); return r+[0x8140]*(n-len(r))
def put(off,cells):
    for i,c in enumerate(cells): d[off+2*i]=c&255; d[off+2*i+1]=c>>8
ROT=['Hira','Kata','ABC','Apag','Fim']
for i,t in enumerate(ROT): put(0x1cd8f2+4*i,enc(t,2))
# nomes padrao: com as letras largas do teclado ABC do proprio jogo (a fonte grande do campo de nome
# nao tem os pares; com pares o campo mostrava quadrados). Maximo de 6 letras, como o campo.
NOMES=['Abel','Domina','Prica','Clage','Michet','Praha','Zero','Vaikun','Jul']
def larga(t):
    r=[]
    for ch in t:
        if 'A'<=ch<='Z': r.append(0x8260+ord(ch)-65)
        elif 'a'<=ch<='z': r.append(0x8281+ord(ch)-97)
        else: raise ValueError(ch)
    return r
for i,t in enumerate(NOMES):
    c=larga(t); assert len(c)<=6,t
    d[0x1cd9b0+0x10*i]=len(c); d[0x1cd9b1+0x10*i]=0; put(0x1cd9b2+0x10*i,c+[0x8140]*(7-len(c)))
open(out,'wb').write(bytes(d)); json.dump(mapa,open(mapf,'w'),ensure_ascii=False,indent=0)
print('tela de nome ok | pares',len(mapa))
