"""traduz um bloco com mapa exato F (EN->PT) e trocas de trechos P; grava as que mudaram e lista sobras"""
import sys,re;sys.path.insert(0,'/root/fm2'); import dump, os
NOMES=re.compile(r'^\{(Ven|Gwianda|Henschel|Hordman|Domingo|Saribash|Azad|Chang-Jang|Man|Ace|Crow|MP):\}$')
def run(c,i,F,P,extra_bad=()):
    p='/root/fm2/pt/%s_%d.tsv'%(c,i)
    have={int(l.split('\t')[0],16) for l in open(p,encoding='utf-8') if l.strip()} if os.path.exists(p) else set()
    out=[]; left=[]
    for r in dump.strings(c,i):
        if r['off'] in have: continue
        en=r['en'].decode('cp932')
        if NOMES.match(en): continue
        pt=F.get(en)
        if pt is None:
            pt=en
            for a,b in P: pt=pt.replace(a,b)
        if pt==en or re.search(r"\b(the|you|You|they|is|are|and|what|What|this|This|that|I'm|I|it|we|We)\b",re.sub(r'@\w+','',pt)):
            left.append((r['off'],en,pt)); continue
        out.append('%x\t%s'%(r['off'],pt))
    if out: open(p,'a',encoding='utf-8').write('\n'.join(out)+'\n')
    print(len(out),'gravadas;',len(left),'sobras')
    import os as _o
    if _o.environ.get('V'):
        for o,e,t in left: print('%x\t%s\t=>\t%s'%(o,e,t))
