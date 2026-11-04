"""troca uma traducao (texto inteiro) por outra em todos os pt/*.tsv. stdin: velho<TAB>novo"""
import sys,glob
pairs=[l.rstrip('\n').split('\t') for l in sys.stdin if '\t' in l]
for f in glob.glob('/root/fm2/pt/*.tsv'):
    L=open(f,encoding='utf-8').read().split('\n'); ch=False
    for k,l in enumerate(L):
        if '\t' not in l: continue
        o,t=l.split('\t',1)
        for a,b in pairs:
            if t==a: L[k]=o+'\t'+b; ch=True
    if ch: open(f,'w',encoding='utf-8').write('\n'.join(L))
