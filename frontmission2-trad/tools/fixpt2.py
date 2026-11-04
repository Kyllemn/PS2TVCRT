import sys
f=sys.argv[1]; d={}
for l in sys.stdin.read().splitlines():
    if '\t' in l: o,t=l.split('\t',1); d[o]=t
L=open(f).read().splitlines()
L=[(l.split('\t')[0]+'\t'+d.pop(l.split('\t')[0])) if l.split('\t')[0] in d else l for l in L]
L+=[o+'\t'+t for o,t in d.items()]
open(f,'w').write('\n'.join(L)+'\n')
