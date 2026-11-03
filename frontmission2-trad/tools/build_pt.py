"""valida as traducoes (pt/*.tsv) e gera fm2_pt.bin"""
import sys,os,glob,struct;sys.path.insert(0,'/root/fm2')
import fm2text, dump, inplace, zbuild, disc2
def width_ok(en,pt,jp=b''):
    le=fm2text.layout(en); lp=fm2text.layout(pt)
    mw=max(max(p) for p in le); ml=max(len(p) for p in le)
    if jp: ml=max(ml,max(len(p) for p in fm2text.layout(jp)))
    return max(max(p) for p in lp)<=max(mw,24) and max(len(p) for p in lp)<=ml, (mw,ml)
def collect(strict=True):
    tr={'I':{},'Z':{}}; erros=[]
    for f in sorted(glob.glob('/root/fm2/pt/*.tsv')):
        c,i=os.path.basename(f)[:-4].split('_'); i=int(i)
        info={r['off']:r for r in dump.strings(c,i)}
        for n,l in enumerate(open(f,encoding='utf-8')):
            l=l.rstrip('\n')
            if not l.strip() or l.startswith('#'): continue
            o,t=l.split('\t',1); o=int(o,16)
            where='%s:%d linha %d (%x)'%(os.path.basename(f),i,n+1,o)
            if o not in info: erros.append(where+' offset desconhecido'); continue
            r=info[o]; en=r['en']
            try: b=t.encode('cp932')
            except UnicodeEncodeError as e: erros.append(where+' caractere invalido: %s'%e); continue
            if b.count(b'{')!=b.count(b'}') and en.count(b'{')==en.count(b'}'): erros.append(where+' chaves desbalanceadas')
            jp=r.get('jp','').encode('cp932') if r.get('jp') else b''
            if fm2text.specials(b)!=fm2text.specials(en) and not (jp and fm2text.specials(b)==fm2text.specials(jp)): erros.append(where+' codigos diferentes do ingles: %s vs %s'%(fm2text.specials(b),fm2text.specials(en)))
            ok,lim=width_ok(en,b,jp)
            if not ok: erros.append(where+' linha/pagina excede (max largura %d, linhas %d): %s'%(lim[0],lim[1],t))
            if len(b)>r['room'] and not (c=='Z' and r['ptrs']): erros.append(where+' nao cabe: %d > %d'%(len(b),r['room']))
            tr[c][(i,o)]=b
    return tr,erros
if __name__=='__main__':
    tr,erros=collect()
    for e in erros: print('ERRO',e)
    if erros: sys.exit(1)
    files={}
    if tr['I']:
        J=open('/root/fm2/jp/I_IDATA.BIN','rb').read(); E=open('/root/fm2/en/I_IDATA.BIN','rb').read()
        files['I/IDATA.BIN']=inplace.apply(J,E,tr['I'])
    if tr['Z']:
        J=open('/root/fm2/jp/Z_ZDATA.BIN','rb').read(); E=open('/root/fm2/en/Z_ZDATA.BIN','rb').read()
        files['Z/ZDATA.BIN']=zbuild.build(J,E,tr['Z'])
    disc2.build(files)
    print('ok:',{k:len(v) for k,v in tr.items()},'textos')
