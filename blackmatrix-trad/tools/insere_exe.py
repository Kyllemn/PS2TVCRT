"""Troca textos do 1ST_READ.BIN no mesmo lugar (mesmo numero de caracteres).
Linha do TSV: offset<TAB>texto ; '|' separa campos de largura fixa
(ex.: 'Sim|Não' num registro de 6 = dois campos de 3)."""
import sys, struct, json
sys.path.insert(0, sys.argv[4])
import pares
exe, tsv, mapf, _, out = sys.argv[1:6]
d=bytearray(open(exe,'rb').read()); mapa=json.load(open(mapf))
usados=set(mapa.values()); livres=[c for c in pares.livres() if c not in usados]
CONV={'（':'(', '）':')', '！':'!', '？':'?', '・':'.', '～':'~', '〜':'~', '　':' '}
def cod(p):
    if p not in mapa: mapa[p]=livres.pop(0)
    return mapa[p]
def enc_linha(t):
    t=''.join(CONV.get(c,c) for c in t)
    return [cod(t[i:i+2].ljust(2)) for i in range(0,len(t),2)]
erros=0
for line in open(tsv,encoding='utf-8'):
    line=line.rstrip('\n')
    if not line or line.startswith('#'): continue
    off,txt=line.split('\t'); off=int(off,16)
    n=struct.unpack('<H',d[off:off+2])[0]
    campos=txt.split('|')
    if len(campos)>1:
        w=n//len(campos); codes=[]
        for c in campos:
            e=enc_linha(c)
            if len(e)>w: print('NAO CABE',hex(off),c); erros+=1; e=e[:w]
            codes+=e+[0x8140]*(w-len(e))
    else:
        codes=[]
        for k,l in enumerate(txt.split('/')):
            if k: codes.append(0xa0ff)
            codes+=enc_linha(l)
        if len(codes)>n: print('NAO CABE',hex(off),len(codes),'>',n,txt); erros+=1; codes=codes[:n]
        codes+=[0x8140]*(n-len(codes))
    d[off+2:off+2+2*n]=struct.pack('<%dH'%n,*codes)
open(out,'wb').write(bytes(d)); json.dump(mapa,open(mapf,'w'),ensure_ascii=False,indent=0)
print('erros',erros,'| pares no mapa',len(mapa))
