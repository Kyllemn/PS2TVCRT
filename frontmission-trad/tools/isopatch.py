"""Edicao da imagem BIN (MODE2/2352) do PS1: le/grava setores Form1 com EDC/ECC e atualiza o diretorio."""
import struct, numpy as np, sys
sys.path.insert(0,'/tmp/claude-0/-home-user-PS2TVCRT/3556a9de-1f69-5db9-a91d-fdfcea6c5b46/scratchpad/bm_tools')
import cdi
def bcd(n): return ((n//10)<<4)|(n%10)
def msf(lba):
    a=lba+150; return bytes([bcd(a//4500),bcd((a//75)%60),bcd(a%75)])
class Bin:
    def __init__(self,path):
        self.b=bytearray(open(path,'rb').read())
    def raw(self,lba): return self.b[lba*2352:(lba+1)*2352]
    def data(self,lba): return bytes(self.b[lba*2352+24:lba*2352+24+2048])
    def read(self,lba,size):
        out=bytearray()
        for i in range((size+2047)//2048): out+=self.data(lba+i)
        return bytes(out[:size])
    def write(self,lba,payload,last_eof=True):
        n=(len(payload)+2047)//2048
        arr=np.zeros((n,2048),np.uint8)
        p=payload+b'\0'*(n*2048-len(payload)); arr[:]=np.frombuffer(p,np.uint8).reshape(n,2048)
        S=np.zeros((n,2352),np.uint8); S[:,24:2072]=arr
        sub=np.array([0,0,8,0,0,0,8,0],np.uint8)
        S[:,16:24]=sub
        if last_eof: S[-1,16:24]=[0,0,0x89,0,0,0,0x89,0]
        e=np.zeros(n,np.uint32)
        for j in range(16,2072): e=(e>>8)^cdi._E[(e^S[:,j])&0xff]
        S[:,2072:2076]=e.view(np.uint8).reshape(n,4)
        cdi._ecc(S,cdi._P,0x81C); cdi._ecc(S,cdi._Q,0x81C+0xAC)
        for i in range(n):
            L=lba+i
            S[i,0:12]=[0]+[0xff]*10+[0]
            S[i,12:15]=list(msf(L)); S[i,15]=2
            end=(L+1)*2352
            if end>len(self.b): self.b+=b'\0'*(end-len(self.b))
            self.b[L*2352:end]=S[i].tobytes()
    def find_rec(self,name):
        pvd=self.data(16); rl=struct.unpack('<I',pvd[158:162])[0]; rs=struct.unpack('<I',pvd[166:170])[0]
        for s in range((rs+2047)//2048):
            d=self.data(rl+s); o=0
            while o<2048 and d[o]:
                n=d[o]; nm=d[o+33:o+33+d[o+32]]
                if nm.split(b';')[0]==name.encode(): return rl+s,o
                o+=n
        raise KeyError(name)
    def set_rec(self,name,lba,size):
        sec,o=self.find_rec(name); d=bytearray(self.data(sec))
        d[o+2:o+10]=struct.pack('<I',lba)+struct.pack('>I',lba)
        d[o+10:o+18]=struct.pack('<I',size)+struct.pack('>I',size)
        sub=bytes(self.raw(sec)[16:24])
        self.write(sec,bytes(d),last_eof=False)
        self.b[sec*2352+16:sec*2352+24]=sub
        # recalcula EDC/ECC com o subcabecalho original
        self._refix(sec)
    def _refix(self,lba):
        S=np.frombuffer(bytes(self.raw(lba)),np.uint8).copy().reshape(1,2352)
        hdr=S[0,12:16].copy(); S[0,12:16]=0
        e=np.zeros(1,np.uint32)
        for j in range(16,2072): e=(e>>8)^cdi._E[(e^S[:,j])&0xff]
        S[:,2072:2076]=e.view(np.uint8).reshape(1,4)
        cdi._ecc(S,cdi._P,0x81C); cdi._ecc(S,cdi._Q,0x81C+0xAC)
        S[0,12:16]=hdr; self.b[lba*2352:(lba+1)*2352]=S[0].tobytes()
    def save(self,path): open(path,'wb').write(self.b)
