"""relatorio: falas sem traducao nos blocos ja iniciados (ignora rotulos de nome repetidos)"""
import sys,os,glob,re;sys.path.insert(0,'/root/fm2')
import dump
c=sys.argv[1] if len(sys.argv)>1 else 'I'
NOMES=re.compile(rb'^\{(Ven|Gwianda|Henschel|Hordman|Domingo|Saribash|Azad|Chang-Jang|Ace|Crow|Delshad|MP|Man|Royd):\}$')
tot=falta=0
for f in sorted(glob.glob('/root/fm2/pt/%s_*.tsv'%c),key=lambda x:int(x.split('_')[-1][:-4])):
    i=int(os.path.basename(f)[:-4].split('_')[1])
    have={int(l.split('\t')[0],16) for l in open(f,encoding='utf-8') if l.strip()}
    rs=[r for r in dump.strings(c,i) if not NOMES.match(r['en'])]
    miss=[r for r in rs if r['off'] not in have]
    tot+=len(rs); falta+=len(miss)
    for r in miss: print('%s_%d\t%x\t%d\t%s'%(c,i,r['off'],r['room'],r['en'].decode('cp932')))
print('total',tot,'faltando',falta,file=sys.stderr)
