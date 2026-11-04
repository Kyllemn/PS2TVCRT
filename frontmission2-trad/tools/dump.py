"""uso: dump.py I|Z sub  -> off<TAB>room<TAB>ptr?<TAB>EN"""
import sys,struct;sys.path.insert(0,'/root/fm2')
from zdata import *; import zstrings, fm2lz
FLAT={'P':'Z_ZPROG.BIN','X':'SLPM_873.31','V':'I_IOVERLAY.BIN','O':'OPROG.BIN'}
def load(c):
    n={'I':'I_IDATA.BIN','Z':'Z_ZDATA.BIN','K':'K_KDATA.BIN'}.get(c) or FLAT[c]
    return open('/root/fm2/jp/'+n,'rb').read(), open('/root/fm2/en/'+n,'rb').read()
def strings(c,i):
    J,E=load(c)
    if c in FLAT: a,b=J,E
    else: a=sub(J,i); b=sub(E,i)
    if c not in FLAT and struct.unpack('<I',b[4:8])[0]==len(b) and len(b)>8:
        b=fm2lz.decompress(b)[0]
        if struct.unpack('<I',a[4:8])[0]==len(a): a=fm2lz.decompress(a)[0]
    rs=zstrings.analyze(a,b)
    for r in rs:
        o=r['off']; r['jp']=''
        if r['ptrs']:
            jo=struct.unpack('<I',a[r['ptrs'][0]:r['ptrs'][0]+4])[0]
            if jo<len(a):
                e=a.find(b'\0',jo)
                try: r['jp']=a[jo:e].decode('cp932')
                except UnicodeDecodeError: pass
            continue
        if o<len(a) and (o==0 or a[o-1]==0) and a[o]:
            e=a.find(b'\0',o)
            try: r['jp']=a[o:e].decode('cp932')
            except UnicodeDecodeError: pass
    return rs
if __name__=='__main__':
    c=sys.argv[1]; i=int(sys.argv[2])
    for r in strings(c,i):
        print('%x\t%d\t%s\t%s'%(r['off'],r['room'],'P' if r['ptrs'] else '-',r['en'].decode('cp932')))
        if r['jp'] and r['jp'].encode('cp932')!=r['en']: print('\t\tJP\t'+r['jp'])
