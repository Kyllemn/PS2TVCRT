"""Insere o texto traduzido com fonte de pares (2 letras por glifo).
uso: insere2.py SCENARIO SYSTEM spec.json traducao.tsv tools_dir saida_SCENARIO saida_SYSTEM mapa.json"""
import sys, struct, json, os
sys.path.insert(0, sys.argv[5])
import psi_lib as P, mfd, pares
scn, sysf, specf, tsv, _, out_scn, out_sys, mapf = sys.argv[1:9]
d=open(scn,'rb').read(); HS,FIXED,SPEC=P.load_spec(specf)
CONV={'（':'(', '）':')', '！':'!', '？':'?', '・':'.', '～':'~', '〜':'~', '　':' ', '，':',', '．':'.', '「':'"', '」':'"', '…':'...'}
mapa=json.load(open(mapf)) if os.path.exists(mapf) else {}
livres=[c for c in pares.livres() if c not in set(mapa.values())]
def cod(par):
    if par not in mapa: mapa[par]=livres.pop(0)
    return mapa[par]
def enc(t):
    t=''.join(CONV.get(ch,ch) for ch in t)
    out=[]
    for k,linha in enumerate(t.split('/')):
        if k: out.append(0xa0ff)
        for i in range(0,len(linha),2):
            out.append(cod(linha[i:i+2].ljust(2)))
    return out
trocas={}
for line in open(tsv,encoding='utf-8'):
    line=line.rstrip('\n')
    if not line: continue
    k,at,txt=line.split('\t'); trocas[(int(k),int(at,16))]=enc(txt)
blocks,ents=P.read_cmp2(d); new=[]; feitas=0
for k,b in enumerate(blocks):
    if b[:8]!=b'PSI22.00': new.append(b); continue
    size=struct.unpack('<I',b[12:16])[0]; c=b[16:16+size]
    ins=P.parse_block(c,HS,FIXED,SPEC); mud=False
    for x in ins:
        if (k,x['at']) in trocas:
            for fi,(t,v) in enumerate(x['fields']):
                if t=='T': x['fields'][fi]=('T',trocas[(k,x['at'])]); feitas+=1; mud=True
    if not mud: new.append(b); continue
    o=P.rebuild(ins,len(c)); P.parse_block(o,HS,FIXED,SPEC)
    nb=b[:12]+struct.pack('<I',len(o))+o
    nb=nb+b'\0'*(len(b)-len(nb)) if len(nb)<=len(b) else nb
    new.append(nb)
open(out_scn,'wb').write(P.write_cmp2(d,new))
# fonte
s=bytearray(open(sysf,'rb').read()); FB=0x424e0+16
assert s[0x424e0:0x424e4]==b'MFD '
for par,code in mapa.items():
    i=pares.indice(code); s[FB+i*128:FB+i*128+128]=mfd.encode(pares.glifo_par(par))
open(out_sys,'wb').write(bytes(s))
json.dump(mapa,open(mapf,'w'),ensure_ascii=False,indent=0)
print('trocas',feitas,'de',len(trocas),'| pares usados',len(mapa),'| livres restantes',len(livres))
