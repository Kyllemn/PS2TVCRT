import sys,json,re,struct; sys.path.insert(0,'/root/fm1')
from fmlib import ps1_tokens
from glossario_equip import en_name
from build_mission import msg_end
TR={}
for fn in ('ui_tr1.tsv','ui_tr2.tsv'):
    for l in open('/root/fm1/'+fn,encoding='utf-8'):
        if '\t' in l:
            a,b=l.rstrip('\n').split('\t',1); TR[a.replace('/','\n')]=b.replace('\\n','\n')
NM=json.load(open('/root/fm1/names.json',encoding='utf-8'))
def split_tokens(d,t):
    """devolve lista de (texto_unidade, bytes) por glifo/controle"""
    out=[]; i=t
    T={}
    from fmlib import fmtext
    while i<len(d) and d[i]!=0xff:
        b=d[i]
        if b==0xfe: out.append(('\n',b'\xfe')); i+=1; continue
        if b>=0xf0: out.append(('[%02X%02X]'%(b,d[i+1]),bytes(d[i:i+2]))); i+=2; continue
        if 0xc0<=b<=0xcf: g=0xc0+(b-0xc0)*0xc0+d[i+1]; out.append((fmtext.OCR.get(g,'?'),bytes(d[i:i+2]))); i+=2; continue
        out.append((fmtext.OCR.get(b,'?'),bytes([b]))); i+=1
    return out
def plan(d,rows):
    """offset -> ('en', texto) ou ('mix', [(texto ou bytes)])"""
    res={}
    for o in rows:
        t,jp=o['off'],o['jp']
        if jp in TR: res[t]=('en',TR[jp]); continue
        m=re.match(r'^([^：/\n]{1,20})：',jp)
        if m:
            nm=m.group(1); base=re.sub(r'　?(ボディ|アーム|レッグ)$','',nm); suf=nm[len(base):].strip('　')
            en=en_name(base) or NM.get(nm) or TR.get(nm)
            if en:
                en+={'ボディ':' Body','アーム':' Arm','レッグ':' Legs'}.get(suf,'')
                toks=split_tokens(d,t)
                k=0; acc=''
                while k<len(toks) and acc!=nm+'：': acc+=toks[k][0]; k+=1
                rest=[]
                for txt,by in toks[k:]:
                    if txt=='　': rest.append(' ')
                    elif txt=='\n': rest.append('\n')
                    else: rest.append(by)
                res[t]=('mix',[en+':']+rest); continue
        if len(jp)==1: res[t]=('grid',jp)
    return res

HIRA='あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよャゅょぁぃぅェぉっらリるれろわをんがぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷペぽ'
KATA='アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨャュョァィゥェォッラリルレロワヲヴンガギグゲゴザジズゼゾダヂヅデドバビブベボパピプべポ'
UP='ABCDEFGHIJKLMNOPQRSTUVWXYZ'
LO='abcdefghijklmnopqrstuvwxyz'
def grid_text(rows,P):
    """mapeia as entradas da grade (na ordem do arquivo) para letras; devolve offset->texto ou None (manter)"""
    out={}; hi=ka=0; seen_kata=False
    for o in rows:
        if P.get(o['off'],('',))[0]!='grid': continue
        c=o['jp']
        if c in HIRA and not seen_kata:
            out[o['off']]=(UP[hi]+' ') if hi<len(UP) else '  '; hi+=1
            if hi>=len(HIRA): seen_kata=True
        elif c in KATA:
            seen_kata=True
            out[o['off']]=(LO[ka]+' ') if ka<len(LO) else '  '; ka+=1
    return out
def ui_texts(d,rows):
    """todos os textos ingleses do arquivo 22 (para contar pares)"""
    P=plan(d,rows); G=grid_text(rows,P); T=[]
    for t,(k,v) in P.items():
        if k=='en': T.append(v)
        elif k=='mix': T.append(v[0])
    return P,G,T
def apply(d,rows,pmap,cb,encode):
    P,G,_=ui_texts(d,rows); d=bytearray(d); over=[]
    def put(t,enc):
        e=msg_end(d,t); budget=e-t-1
        if len(enc)-1<=budget:
            d[t:t+len(enc)]=enc
            for i in range(t+len(enc),e): d[i]=0xff
            return True
        over.append((t,len(enc)-1,budget)); return False
    for t,(k,v) in P.items():
        if k=='en': put(t,encode(v,pmap,cb))
        elif k=='mix':
            name=v[0][:-1]; w=name.split(' ')
            cands=[name+':',name]
            if len(w)>1 and w[-1] in ('Body','Arm','Legs'): cands+=[' '.join(w[:-1])+' '+w[-1][0]+':',' '.join(w[:-1])+w[-1][0]]
            cands+=[w[0]+':',w[0]]
            cands=[c for c in cands if all(c2 in pmap for c2 in [ (c+(' ' if len(c)%2 else ''))[i:i+2] for i in range(0,len(c),2)])] or [name+':']
            for ci,c in enumerate(cands):
                out=bytearray(encode(c,pmap,cb)[:-1])
                for part in v[1:]:
                    if isinstance(part,str): out+=encode(part,pmap,cb)[:-1]
                    else: out+=part
                out.append(0xff)
                e=msg_end(d,t)
                if len(out)-1<=e-t-1 or ci==len(cands)-1:
                    put(t,bytes(out)); break
        elif k=='grid' and t in G:
            put(t,encode(G[t],pmap,cb))
    return bytes(d),over

def shorten_to(text,limit_bytes,pmap,cb,encode):
    """corta no limite de palavras ate caber; devolve bytes codificados"""
    import re as _r
    t=text
    while True:
        try: e=encode(t,pmap,cb)
        except KeyError: e=None
        if e is not None and len(e)-1<=limit_bytes: return e
        if len(t)<=1: return encode(' ',pmap,cb) if limit_bytes>=1 else b'\xff'
        # tira a ultima palavra; se for uma palavra so, tira letras
        parts=t.rsplit(' ',1)
        t=parts[0] if len(parts)>1 and parts[0] else t[:-1]
def apply_reloc(d,rows,pmap,cb,encode,refs_ok):
    """como apply, mas o que nao cabe vai encurtado no lugar e completo nas sobras.
    refs_ok: offsets que tem referencias conhecidas. devolve (novo, realocados{old:new})"""
    nd,over=apply(d,rows,pmap,cb,encode)
    nd=bytearray(nd); P,G,_=ui_texts(d,rows)
    full={}
    for t,n,b in over:
        k,v=P[t]
        if k=='en': text=v
        elif k=='grid': text=G[t]
        else: text=None
        if text is None:
            continue
        full[t]=encode(text,pmap,cb)
        e=msg_end(d,t)
        s=shorten_to(text.split('[F907]')[0] if '[F907]' in text else text,e-t-1,pmap,cb,encode)
        nd[t:t+len(s)]=s
        for i in range(t+len(s),e): nd[i]=0xff
    # sobras: sequencias de FF de padding depois do fim de cada string (nao comecam em inicio referenciado)
    starts=set(o['off'] for o in rows)
    holes=[]; i=0; n=len(nd)
    while i<0x7918:
        if nd[i]==0xff and i+1<0x7918 and nd[i+1]==0xff:
            j=i+1
            while j<0x7918 and nd[j]==0xff and (j not in starts): j+=1
            if j-i-1>=4: holes.append([i+1,j])   # deixa o FF terminador anterior
            i=j
        else: i+=1
    moved={}
    for t,enc in sorted(full.items(),key=lambda x:-len(x[1])):
        if t not in refs_ok: continue
        for h in holes:
            if h[1]-h[0]>=len(enc):
                nd[h[0]:h[0]+len(enc)]=enc; moved[t]=h[0]; h[0]+=len(enc); break
    return bytes(nd),moved,over
