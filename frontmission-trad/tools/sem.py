import pickle,re
D=pickle.load(open('/root/fm1/dic/jmdict.pkl','rb'))
KANJI=re.compile(r'[一-鿿]')
def stem(w):
    for suf in ('ing','ed','es','s','ly'):
        if len(w)>4 and w.endswith(suf): return w[:-len(suf)]
    return w
SD={}
def jp_set(s):
    s=re.sub(r'\[[0-9A-F]{4}\]','',s).replace('リ','り')
    out=set(); i=0
    while i<len(s):
        hit=None
        for L in range(min(8,len(s)-i),0,-1):
            w=s[i:i+L]
            if w in D and (L>=2 or KANJI.match(w)):
                hit=w; break
        if hit:
            if hit not in SD: SD[hit]={stem(x) for x in D[hit]}
            out|=SD[hit]; i+=len(hit)
        else: i+=1
    return out
def en_set(s):
    s=re.sub(r'\[[0-9A-F]{4}\]',' ',s)
    return {stem(w) for w in re.findall(r"[a-z]+",s.lower()) if len(w)>2}

def jp_coverage(s):
    s=re.sub(r'\[[0-9A-F]{4}\]','',s).replace('\n','')
    if not s: return 0
    cov=0; i=0
    while i<len(s):
        hit=0
        for L in range(min(8,len(s)-i),1,-1):
            if s[i:i+L] in D: hit=L; break
        if hit: cov+=hit; i+=hit
        else:
            if s[i] in '。、！？‥・：「」（）ー': cov+=1
            i+=1
    return cov/len(s)
def en_ok(s):
    t=re.sub(r'\[[0-9A-F]{2,4}\]','',s)
    words=re.findall(r"[A-Za-z']+",t)
    if not words: return False
    vow=sum(1 for w in words if re.search(r'[aeiouyAEIOUY]',w))
    return (' ' in t or len(words)==1 and len(t)<=12) and vow/len(words)>0.8 and sum(1 for c in t if c.islower())>=0.4*sum(1 for c in t if c.isalpha())
