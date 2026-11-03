"""lista falas sem traducao de um bloco (unicas), pronto pra traduzir"""
import sys,os,re;sys.path.insert(0,'/root/fm2')
import dump
c,i=sys.argv[1],int(sys.argv[2])
NOMES=re.compile(rb'^\{(Ven|Gwianda|Henschel|Hordman|Domingo|Saribash|Azad|Chang-Jang|Ace|Crow|Delshad|MP|Man|Royd):\}$')
p='/root/fm2/pt/%s_%d.tsv'%(c,i)
have={int(l.split('\t')[0],16) for l in open(p,encoding='utf-8') if l.strip()} if os.path.exists(p) else set()
seen=set()
for r in dump.strings(c,i):
    if r['off'] in have or NOMES.match(r['en']) or r['en'] in seen: continue
    seen.add(r['en']); print('%x\t%d\t%s'%(r['off'],r['room'],r['en'].decode('cp932')))
