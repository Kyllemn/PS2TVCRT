"""replica traducoes: toda fala com o MESMO ingles de uma ja traduzida recebe o mesmo portugues.
uso: propagate.py I|Z [blocos...]  (sem blocos = todos que tiverem texto)"""
import sys,os,glob;sys.path.insert(0,'/root/fm2')
import dump
c=sys.argv[1]
tm={}
for f in glob.glob('/root/fm2/pt/%s_*.tsv'%c):
    i=int(os.path.basename(f)[:-4].split('_')[1]); info={r['off']:r['en'] for r in dump.strings(c,i)}
    for l in open(f,encoding='utf-8'):
        l=l.rstrip('\n')
        if not l.strip(): continue
        o,t=l.split('\t',1); o=int(o,16)
        if o in info: tm.setdefault(info[o],t)
blocks=[int(x) for x in sys.argv[2:]] if len(sys.argv)>2 else range({'I':455,'Z':660}[c])
add=0
for i in blocks:
    try: rs=dump.strings(c,i)
    except Exception: continue
    p='/root/fm2/pt/%s_%d.tsv'%(c,i)
    have=set()
    if os.path.exists(p):
        have={int(l.split('\t')[0],16) for l in open(p,encoding='utf-8') if l.strip()}
    new=[(r["off"],tm[r["en"]]) for r in rs if r["en"] in tm and r["off"] not in have and len(tm[r["en"]].encode("cp932"))<=r["room"]]
    if new:
        with open(p,'a',encoding='utf-8') as f:
            for o,t in new: f.write('%x\t%s\n'%(o,t))
        add+=len(new)
print('replicadas',add,'memoria',len(tm))
