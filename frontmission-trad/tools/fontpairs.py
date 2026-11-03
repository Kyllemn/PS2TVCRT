"""Fonte de pares: cada glifo 16x16 do PS1 = 2 letras de 8x16."""
import sys,re; sys.path.insert(0,'/root/fm1')
from latin import cell
T={}
for l in open('/root/fm1/tbl/fm1st.tbl',encoding='utf-8'):
    i,c=l.rstrip('\n').split('\t'); T[int(i)]=c
RESERVE=set(i for i,c in T.items() if c.startswith('〔') or re.match(r'[Ａ-Ｚａ-ｚ０-９＄＋＝×∞○△□／～（）“”Ⅱ-Ⅳ∨．]',c))
RESERVE|=set(range(1427,1440))
FREE=[i for i in range(1440) if i not in RESERVE]
def code_bytes(g):
    if g<0xc0: return bytes([g])
    x=g-0xc0; return bytes([0xc0+x//0xc0, x%0xc0])
def tiles_for(pair):
    """bytes de 2 tiles (64 bytes) no formato da fonte PS1: por linha b0=plano alto, b1=plano baixo"""
    out=bytearray()
    for ch in pair:
        g=cell(ch); t=bytearray()
        for y in range(16):
            b0=b1=0
            for x in range(8):
                v=g[y][x]
                b0|=((v>>1)&1)<<(7-x); b1|=(v&1)<<(7-x)
            t+=bytes([b0,b1])
        out+=t
    return bytes(out)
def assign(pair_counts):
    """pares mais frequentes nos codigos de 1 byte"""
    order=[p for p,_ in sorted(pair_counts.items(),key=lambda t:-t[1])]
    one=[g for g in FREE if g<0xc0]; two=[g for g in FREE if g>=0xc0]
    slots=one+two
    assert len(order)<=len(slots), (len(order),len(slots))
    return {p:slots[k] for k,p in enumerate(order)}
