"""Confere um lote de traducao: idx<TAB>texto. Linhas <= 38 letras,
numero de linhas <= max(3, linhas do japones)."""
import sys
fila={}
for l in open(sys.argv[1],encoding='utf-8'):
    p=l.rstrip('\n').split('\t'); fila[p[0]]=p[3]
bad=0; n=0
for f in sys.argv[2:]:
    for l in open(f,encoding='utf-8'):
        l=l.rstrip('\n')
        if not l: continue
        i,t=l.split('\t',1); n+=1
        if i not in fila: print('idx desconhecido',i); bad+=1; continue
        lin=t.split('/'); jl=fila[i].count('/')+1
        if any(len(x)>38 for x in lin) or len(lin)>max(3,jl):
            print(i,[len(x) for x in lin],t); bad+=1
print('linhas',n,'problemas',bad)
