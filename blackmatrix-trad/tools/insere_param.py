"""Traduz os campos do PARAMETA.DAT: [u16 tamanho][N celulas], linhas de 19."""
import sys, struct, json
sys.path.insert(0, sys.argv[2]); import pares
src, _, campos, trad, mapf, out = sys.argv[1:7]
d=bytearray(open(src,'rb').read()); mapa=json.load(open(mapf))
usados=set(mapa.values()); livres=[c for c in pares.livres() if c not in usados]
def cod(p):
    if p not in mapa: mapa[p]=livres.pop(0)
    return mapa[p]
def enc(t): return [cod(t[i:i+2].ljust(2)) for i in range(0,len(t),2)]
def linha(t,w=19):
    if '»' in t:
        a,b=t.split('»'); ea,eb=enc(a),enc(b)
        r=ea+[0x8140]*(w-len(ea)-len(eb))+eb
    else:
        r=enc(t); r=r+[0x8140]*(w-len(r))
    assert len(r)==w,(t,len(r)); return r
nomes={}; desc={}; ajuda={}
for l in open(trad+'/nomes.tsv',encoding='utf-8'):
    a,b=l.rstrip('\n').split('\t'); nomes[a]=b
dj={}
for l in open(trad+'/desc_jp.txt',encoding='utf-8'):
    p=l.rstrip('\n').split('\t'); dj[(p[1],p[2])]=p[0]
for l in open(trad+'/desc_pt.txt',encoding='utf-8'):
    p=l.rstrip('\n').split('\t'); desc[p[0]]=(p[1],p[2])
hj={}
for l in open(trad+'/help_jp.txt',encoding='utf-8'):
    p=l.rstrip('\n').split('\t'); hj[(p[1],p[2],p[3])]=p[0]
for l in open(trad+'/help_pt.txt',encoding='utf-8'):
    p=l.rstrip('\n').split('\t'); ajuda[p[0]]=(p[1],p[2],p[3])
falta=0; feitos=0
for l in open(campos,encoding='utf-8'):
    k,off,fld,ln,t=l.rstrip('\n').split('\t'); off=int(off,16); fld=int(fld)
    if fld in (6,7,8):
        key=t.rstrip('　')
        if key not in nomes: print('sem traducao:',key); falta+=1; continue
        e=enc(nomes[key])
        if len(e)>fld: print('NAO CABE',key,nomes[key]); falta+=1; continue
        codes=e+[0x8140]*(fld-len(e)); tam=len(e)
    elif fld==38:
        key=(t[:19].rstrip('　'),t[19:].rstrip('　')); i=dj[key]; a,b=desc[i]
        codes=linha(a)+linha(b); tam=struct.unpack('<H',d[off-2:off])[0]
    elif fld==57:
        key=(t[:19].rstrip('　'),t[19:38].rstrip('　'),t[38:].rstrip('　')); i=hj[key]; a,b,c=ajuda[i]
        codes=linha(a)+linha(b)+linha(c); tam=struct.unpack('<H',d[off-2:off])[0]
    else: continue
    d[off-2:off]=struct.pack('<H',tam)
    d[off:off+2*fld]=struct.pack('<%dH'%fld,*codes); feitos+=1
# v5: descricoes com gaiji que o param_txt pulou (offset do campo, linha 1, linha 2)
import os
if os.path.exists(trad+'/param_extra.tsv'):
    for l in open(trad+'/param_extra.tsv',encoding='utf-8'):
        off,a,b=l.rstrip('\n').split('\t'); off=int(off,16)
        assert struct.unpack('<H',d[off-2:off])[0]==38
        d[off:off+76]=struct.pack('<38H',*(linha(a)+linha(b))); feitos+=1
open(out,'wb').write(bytes(d)); json.dump(mapa,open(mapf,'w'),ensure_ascii=False,indent=0)
print('campos traduzidos',feitos,'| problemas',falta,'| pares no mapa',len(mapa))
