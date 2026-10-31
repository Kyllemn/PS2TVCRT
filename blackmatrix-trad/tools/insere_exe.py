"""Troca textos do 1ST_READ.BIN no mesmo lugar (mesmo numero de celulas).
Sintaxe do texto: '|' separa campos de largura igual; '/' quebra de linha do
jogo; '¶' completa a linha atual ate 19 celulas (quebra por espacos, como o
original); '{k}' = k celulas vazias; caracteres japoneses/largos vao crus."""
import sys, struct, json, re
sys.path.insert(0, sys.argv[4])
import pares
exe, tsv, mapf, _, out = sys.argv[1:6]
d=bytearray(open(exe,'rb').read()); mapa=json.load(open(mapf))
usados=set(mapa.values()); livres=[c for c in pares.livres() if c not in usados]
LARG=19
def cod(p):
    if p not in mapa: mapa[p]=livres.pop(0)
    return mapa[p]
def cru(ch):
    if ch=='ｰ': return 0xb001
    b=ch.encode('shift_jis'); return b[0]<<8|b[1]
def enc(t):
    codes=[]; linha0=0; buf=''
    def flush():
        nonlocal buf
        for i in range(0,len(buf),2): codes.append(cod(buf[i:i+2].ljust(2)))
        buf=''
    for tok in re.findall(r'\{\d+\}|.',t):
        if tok.startswith('{') and len(tok)>2:
            flush(); codes.extend([0x8140]*int(tok[1:-1]))
        elif tok=='/':
            flush(); codes.append(0xa0ff); linha0=len(codes)
        elif tok=='¶':
            flush(); n=len(codes)-linha0; codes.extend([0x8140]*((-n)%LARG)); linha0=len(codes)
        elif ord(tok)>=0x3000 or tok in '−／':
            flush(); codes.append(cru(tok))
        else: buf+=tok
    flush(); return codes
erros=0
for line in open(tsv,encoding='utf-8'):
    line=line.rstrip('\n')
    if not line or line.startswith('#'): continue
    off,txt=line.split('\t'); off=int(off,16)
    n=struct.unpack('<H',d[off:off+2])[0]
    if '|' in txt:
        campos=txt.split('|'); w=n//len(campos); codes=[]
        for c in campos:
            e=enc(c)
            if len(e)>w: print('NAO CABE',hex(off),repr(c),len(e),'>',w); erros+=1; e=e[:w]
            codes+=e+[0x8140]*(w-len(e))
        codes+=[0x8140]*(n-len(codes))
    else:
        codes=enc(txt)
        if len(codes)>n: print('NAO CABE',hex(off),len(codes),'>',n,repr(txt)); erros+=1; codes=codes[:n]
        codes+=[0x8140]*(n-len(codes))
    d[off+2:off+2+2*n]=struct.pack('<%dH'%n,*codes)
open(out,'wb').write(bytes(d)); json.dump(mapa,open(mapf,'w'),ensure_ascii=False,indent=0)
print('erros',erros,'| pares no mapa',len(mapa))
