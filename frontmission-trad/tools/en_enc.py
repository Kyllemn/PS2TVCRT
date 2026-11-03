"""Codificacao do ingles (texto decodificado do DS) para o PS1 com fonte de pares."""
import re
DSCH={0x50:' ',0x57:"'",0x5c:',',0x5e:'.',0x4f:'?',0x51:'!',0x4a:':',0x5d:'-',0x52:'"'}
for i in range(10): DSCH[0x40+i]=str(i)
def ds_to_units(s):
    """texto do ds_str -> lista de unidades: ('t',texto) ou ('c',[bytes de controle]) ou ('nl',)"""
    out=[]; buf=''
    i=0
    while i<len(s):
        if s[i]=='\n':
            if buf: out.append(('t',buf)); buf=''
            out.append(('nl',)); i+=1; continue
        m=re.match(r'\[([0-9A-F]{2})([0-9A-F]{2})?\]',s[i:])
        if m:
            if m.group(2):
                if buf: out.append(('t',buf)); buf=''
                out.append(('c',[int(m.group(1),16),int(m.group(2),16)]))
            else:
                buf+=DSCH.get(int(m.group(1),16),'?')
            i+=m.end(); continue
        buf+=s[i]; i+=1
    if buf: out.append(('t',buf))
    return out
def pairs_of(units):
    """pares de letras por linha; o texto apos controle continua a contagem da linha"""
    res=[]; col=0
    for u in units:
        if u[0]=='t':
            t=u[1]
            if len(t)%2: t=t+' '
            for k in range(0,len(t),2): res.append(t[k:k+2])
        elif u[0]=='nl': pass
    return res

W=30
def plain_len(s):
    return len(re.sub(r'\[[0-9A-F]{4}\]','',re.sub(r'\[[0-9A-F]{2}\]','x',s)))
def rewrap(s, width=W):
    """refaz as quebras do texto (formato ds_str) para no maximo `width` colunas, nos espacos"""
    lines=s.split('\n'); out=[]
    head=[]
    if lines and re.match(r'^[^ ]{0,24}:$',lines[0]) or (lines and re.match(r'^\[FC[0-9A-F]{2}\]:$',lines[0])):
        head=[lines[0]]; lines=lines[1:]
    paras=[]; cur=''
    for l in lines:
        if l=='':
            if cur: paras.append(cur); cur=''
            paras.append(''); continue
        if cur and (plain_len(cur)+1+plain_len(l)>width or re.search(r'[.!?"]$|\.\.\.$|\[F[89A-F][0-9A-F]{2}\]$',cur)) and plain_len(l)<=width and plain_len(cur)<=width:
            paras.append(cur); cur=l
        else:
            cur=(cur+' '+l) if cur else l
    if cur: paras.append(cur)
    res=[]
    for p in paras:
        if p=='': res.append(''); continue
        words=p.split(' '); line=''
        for w in words:
            if not line: line=w
            elif plain_len(line)+1+plain_len(w)<=width: line+=' '+w
            else: res.append(line); line=w
        res.append(line)
    return '\n'.join(head+res)
