"""reconstroi o ZDATA a partir da versao inglesa.
trans: {(sub, off): bytes_novos}. Cada subarquivo fica no MESMO setor da versao inglesa (nada muda de lugar)."""
import sys,struct;sys.path.insert(0,'/root/fm2')
from zdata import *; import fm2lz
def patch_sub(A,B,items):
    """items: lista de (off, novo, room, ptrs). devolve bytes descomprimidos novos"""
    B=bytearray(B); extra=bytearray(); base=len(B); base=(base+3)&~3
    pad=base-len(B)
    for off,new,room,ptrs in items:
        old_end=B.index(b'\0',off)
        if len(new)<=room:
            B[off:old_end]=b'\0'*(old_end-off)   # apaga o antigo (mesmo tamanho)
            B[off:off+len(new)]=new
        else:
            if not ptrs: raise ValueError('nao cabe e sem ponteiro conhecido: off %x'%off)
            pos=base+len(extra); extra+=new+b'\0'
            while len(extra)%4: extra+=b'\0'
            for p in ptrs: struct.pack_into('<I',B,p,pos)
    if extra: B+=b'\0'*pad+extra
    return bytes(B)
def build(J,E,trans,recompress_all=False):
    ee=entries(E); n=len(ee)
    bysub={}
    for (s,o),v in trans.items(): bysub.setdefault(s,[]).append((o,v))
    out=bytearray(E)
    for i in range(n):
        if i not in bysub and not recompress_all: continue
        b=sub(E,i)
        if struct.unpack('<I',b[4:8])[0]!=len(b): continue
        B=fm2lz.decompress(b)[0]
        a=sub(J,i); A=fm2lz.decompress(a)[0] if struct.unpack('<I',a[4:8])[0]==len(a) else a
        items=[]
        if i in bysub:
            import zstrings
            info={r['off']:r for r in zstrings.analyze(A,B)}
            for o,v in bysub[i]:
                r=info[o]; items.append((o,v,r['room'],r['ptrs']))
        nb=fm2lz.compress(patch_sub(A,B,items) if items else B)
        s,sz=ee[i]
        lim=(ee[i+1][0] if i+1<n else len(E)//2048)-s
        # espaco disponivel ate o proximo subarquivo (em setores)
        nxt=min([x[0] for x in ee if x[0]>s]+[len(E)//2048])
        if len(nb)>(nxt-s)*2048: raise ValueError('sub %d nao cabe no lugar'%i)
        out[s*2048:s*2048+(nxt-s)*2048]=nb+b'\0'*((nxt-s)*2048-len(nb))
        struct.pack_into('<2I',out,8+8*i,s,len(nb))
    return bytes(out)
