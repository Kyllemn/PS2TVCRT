"""levanta as strings traduziveis do ZDATA (versao inglesa) com capacidade e ponteiros conhecidos"""
import sys,struct,re,json;sys.path.insert(0,'/root/fm2')
from zdata import *; import fm2lz
TXT=re.compile(rb'\{[^{}]*[A-Za-z][^{}]*\}')
def texto_valido(s):
    try: t=s.decode('cp932')
    except UnicodeDecodeError: return False
    if any(ord(c)<0x20 and c not in '\n' for c in t): return False
    eng=''.join(re.findall(r'\{([^{}]*)\}',t))
    if not eng: return False
    ok=sum(1 for c in eng if c.isascii() and (c.isalnum() or c in " .,'!?-:;()\"&/%$@*|"))
    return ok>=0.9*len(eng)
def analyze(A,B):
    LA=len(A); P={}
    for k in range(0,LA-3,4):
        x=struct.unpack('<I',A[k:k+4])[0]; y=struct.unpack('<I',B[k:k+4])[0]
        if x!=y and LA<=y<len(B): P.setdefault(y,[]).append(k)
    # todas as referencias u32 alinhadas a cada alvo (para saber se ha refs nao mapeadas)
    out=[]; k=0; n=len(B)
    while k<n:
        if B[k]==0: k+=1; continue
        e=B.find(b'\0',k); e=n if e<0 else e
        s=B[k:e]
        if TXT.search(s) and texto_valido(s):
            cap=e-k
            # capacidade segura: o maior entre o texto ingles e o japones original na mesma posicao
            # (no anexo: ate o proximo alinhamento de 4, so sobre bytes nulos)
            room=e-k
            if k<LA and (k==0 or A[k-1]==0) and A[k]!=0:
                ej=A.find(b'\0',k); room=max(room,(ej if ej>=0 else LA)-k)
            elif k>=LA:
                j=e+1
                while j%4 and j<n and B[j]==0: j+=1
                room=j-k-1
            out.append(dict(off=k,en=s,room=room,ptrs=P.get(k,[]),anexo=k>=LA))
        k=e+1
    return out
if __name__=='__main__':
    J=open('/root/fm2/jp/Z_ZDATA.BIN','rb').read(); E=open('/root/fm2/en/Z_ZDATA.BIN','rb').read()
    rows=[]
    for i in range(len(entries(J))):
        a=sub(J,i); b=sub(E,i)
        if struct.unpack('<I',b[4:8])[0]!=len(b): continue
        A=fm2lz.decompress(a)[0] if struct.unpack('<I',a[4:8])[0]==len(a) else a
        B=fm2lz.decompress(b)[0]
        for r in analyze(A,B):
            r['sub']=i; r['en']=r['en'].decode('latin1'); rows.append(r)
    json.dump(rows,open('/root/fm2/zstr_en.json','w'))
    import collections
    c=collections.Counter((r['anexo'],bool(r['ptrs'])) for r in rows)
    print(len(rows),c, sum(len(r['en']) for r in rows))
