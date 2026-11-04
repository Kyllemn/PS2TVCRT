"""troca texto no lugar (capacidade fixa) em subarquivos de contêiner (IDATA etc.)"""
import sys,struct;sys.path.insert(0,'/root/fm2')
from zdata import *; import zstrings
def apply(J,E,trans):
    """trans: {(sub,off): bytes}. Nao muda tamanho de nada; confere que so sobrescreve o proprio texto ou zeros."""
    out=bytearray(E); ee=entries(E)
    bysub={}
    for (s,o),v in trans.items(): bysub.setdefault(s,{})[o]=v
    for i,items in bysub.items():
        A=sub(J,i); B=bytearray(sub(E,i))
        info={r['off']:r for r in zstrings.analyze(A,bytes(B))}
        for o,new in items.items():
            r=info[o]; end=B.index(b'\0',o)
            if len(new)>r['room']: raise ValueError('sub %d off %x: %d > %d'%(i,o,len(new),r['room']))
            if any(B[end:o+len(new)+1]): raise ValueError('sub %d off %x: sobreporia dados'%(i,o))
            B[o:end]=b'\0'*(end-o); B[o:o+len(new)]=new
        s,sz=ee[i]; assert len(B)==sz
        out[s*2048:s*2048+sz]=B
    return bytes(out)

def apply_flat(J,E,trans):
    """arquivo plano (exe/overlay): trans {(0,off): bytes}"""
    B=bytearray(E); info={r['off']:r for r in zstrings.analyze(J,E)}
    for (_,o),new in trans.items():
        r=info[o]; end=B.index(b'\0',o)
        if len(new)>r['room']: raise ValueError('off %x: %d > %d'%(o,len(new),r['room']))
        if any(B[end:o+len(new)+1]): raise ValueError('off %x: sobreporia dados'%o)
        B[o:end]=b'\0'*(end-o); B[o:o+len(new)]=new
    assert len(B)==len(E)
    return bytes(B)
