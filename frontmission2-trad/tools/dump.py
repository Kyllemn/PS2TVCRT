"""uso: dump.py I|Z sub  -> off<TAB>room<TAB>ptr?<TAB>EN"""
import sys,struct;sys.path.insert(0,'/root/fm2')
from zdata import *; import zstrings, fm2lz
def load(c):
    n={'I':'I_IDATA.BIN','Z':'Z_ZDATA.BIN'}[c]
    return open('/root/fm2/jp/'+n,'rb').read(), open('/root/fm2/en/'+n,'rb').read()
def strings(c,i):
    J,E=load(c); a=sub(J,i); b=sub(E,i)
    if struct.unpack('<I',b[4:8])[0]==len(b) and len(b)>8:
        b=fm2lz.decompress(b)[0]
        if struct.unpack('<I',a[4:8])[0]==len(a): a=fm2lz.decompress(a)[0]
    return zstrings.analyze(a,b)
if __name__=='__main__':
    c=sys.argv[1]; i=int(sys.argv[2])
    for r in strings(c,i):
        print('%x\t%d\t%s\t%s'%(r['off'],r['room'],'P' if r['ptrs'] else '-',r['en'].decode('cp932')))
