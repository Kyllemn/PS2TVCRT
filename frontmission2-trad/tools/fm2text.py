"""tokenizacao e validacao do texto do FM2 (formato do patch ingles)"""
import re
def tokens(b):
    """bytes -> lista de (tipo, valor). tipos: t=texto ascii, j=char sjis, nl='|', pg='?!', nm=@xxxx, cd=$xx, o='{', c='}'"""
    out=[]; i=0; asc=False; n=len(b)
    while i<n:
        c=b[i]
        if c==0x7b: out.append(('o','{')); asc=True; i+=1; continue
        if c==0x7d: out.append(('c','}')); asc=False; i+=1; continue
        if c==0x7c: out.append(('nl','|')); i+=1; continue
        if asc:
            out.append(('t',chr(c))); i+=1; continue
        if c==0x3f and i+1<n and b[i+1]==0x21: out.append(('pg','?!')); i+=2; continue
        if c==0x40 and i+4<n: out.append(('nm',b[i:i+5].decode('latin1'))); i+=5; continue
        if c==0x24 and i+2<n: out.append(('cd',b[i:i+3].decode('latin1'))); i+=3; continue
        if c>=0x80 and i+1<n: out.append(('j',b[i:i+2].decode('cp932','replace'))); i+=2; continue
        out.append(('x',chr(c))); i+=1
    return out
def specials(b):
    """sequencia que precisa ser preservada: nomes, codigos $, paginas"""
    return [v for t,v in tokens(b) if t in ('nm','cd','x')]
def layout(b):
    """lista de paginas; cada pagina = lista de larguras de linha (ascii=1, sjis=2, nome=?)"""
    pages=[[0]]
    for t,v in tokens(b):
        if t=='nl': pages[-1].append(0)
        elif t=='pg': pages.append([0])
        elif t=='t': pages[-1][-1]+=1
        elif t=='j': pages[-1][-1]+=2
        elif t=='nm': pages[-1][-1]+=8
    return pages
