"""uso: fixpt.py arquivo.tsv  (le pares 'off<TAB>texto' da entrada padrao; texto vazio remove a linha)"""
import sys
p=sys.argv[1]; L=open(p,encoding='utf-8').read().splitlines()
fx={}
for l in sys.stdin.read().splitlines():
    if l.strip(): o,_,t=l.partition('\t'); fx[o]=t
out=[]
for l in L:
    o=l.split('\t')[0]
    if o in fx:
        if fx[o]: out.append(o+'\t'+fx.pop(o))
        else: fx.pop(o)
    else: out.append(l)
for o,t in fx.items():
    if t: out.append(o+'\t'+t)
open(p,'w',encoding='utf-8').write('\n'.join(out)+'\n')
