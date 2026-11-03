"""preenche falas pendentes com alternativas curtas: ALT[en] = [opcoes em ordem de preferencia]"""
import sys,os,re;sys.path.insert(0,'/root/fm2'); import dump
def run(c,ALT):
    import glob
    n=0
    for f in glob.glob('/root/fm2/pt/%s_*.tsv'%c):
        i=int(os.path.basename(f)[:-4].split('_')[1])
        have={int(l.split('\t')[0],16) for l in open(f,encoding='utf-8') if l.strip()}
        add=[]
        for r in dump.strings(c,i):
            if r['off'] in have: continue
            en=r['en'].decode('cp932')
            for alt in ALT.get(en,[]):
                if len(alt.encode('cp932'))<=r['room']: add.append('%x\t%s'%(r['off'],alt)); break
        if add: open(f,'a',encoding='utf-8').write('\n'.join(add)+'\n'); n+=len(add)
    print('preenchidas',n)
