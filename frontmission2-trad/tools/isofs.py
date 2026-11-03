"""lista arquivos de uma imagem MODE2/2352 (ISO9660), recursivo"""
import struct,sys
class Img:
    def __init__(s,p): s.f=open(p,'rb')
    def sec(s,n): s.f.seek(n*2352+24); return s.f.read(2048)
    def read(s,lba,size): return b''.join(s.sec(lba+i) for i in range((size+2047)//2048))[:size]
    def walk(s,lba=None,size=None,path=''):
        if lba is None:
            pvd=s.sec(16); r=pvd[156:190]; lba=struct.unpack('<I',r[2:6])[0]; size=struct.unpack('<I',r[10:14])[0]
        d=s.read(lba,size); p=0; out=[]
        while p<len(d):
            l=d[p]
            if l==0: p=(p//2048+1)*2048; continue
            nl=d[p+32]; nm=d[p+33:p+33+nl]; fl=d[p+25]
            a=struct.unpack('<I',d[p+2:p+6])[0]; sz=struct.unpack('<I',d[p+10:p+14])[0]
            if nm not in (b'\0',b'\1'):
                n=nm.decode('ascii','replace').split(';')[0]
                if fl&2: out+=s.walk(a,sz,path+n+'/')
                else: out.append((path+n,a,sz))
            p+=l
        return out
if __name__=='__main__':
    for x in Img(sys.argv[1]).walk(): print(*x)
