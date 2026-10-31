"""Junta os lotes trad/pt_*.tsv (idx<TAB>texto) com trad/fila.tsv e gera bloco<TAB>pos<TAB>texto
para todas as ocorrencias do roteiro (mesmo texto japones = mesma traducao)."""
import sys, glob
trad, rot, out = sys.argv[1:4]
d={}
for f in sorted(glob.glob(trad+'/pt_*.tsv')):
    for l in open(f,encoding='utf-8'):
        l=l.rstrip('\n')
        if l: i,t=l.split('\t',1); d[i]=t
fila={}
for l in open(trad+'/fila.tsv',encoding='utf-8'):
    i,b,ty,jp=l.rstrip('\n').split('\t',3); fila.setdefault(jp,i)
# trocas pontuais por bloco/posicao (ex.: credito na primeira fala de cada abertura)
import os
esp={}
if os.path.exists(trad+'/especial.tsv'):
    for l in open(trad+'/especial.tsv',encoding='utf-8'):
        b,at,t=l.rstrip('\n').split('\t'); esp[(int(b),at)]=t
o=open(out,'w',encoding='utf-8'); n=0
for l in open(rot,encoding='utf-8'):
    b,at,ty,jp=l.rstrip('\n').split('\t',3)
    o.write('%d\t%s\t%s\n'%(int(b),at,esp.get((int(b),at),d[fila[jp]]))); n+=1
print('roteiro',n,'linhas')
