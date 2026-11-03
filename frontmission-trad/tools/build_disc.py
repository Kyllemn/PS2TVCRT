import sys,struct,pickle; sys.path.insert(0,'/root/fm1')
from isopatch import Bin
def rebuild_pak(newfiles):
    a=open('/root/fm1/ps1/PACKAGE.PAK','rb').read()
    w=list(struct.unpack('<512I',a[:2048])); ents=[]; i=4
    while i+1<512 and (w[i] or w[i+1] or i<4+2*59):
        if i>=4+2*59: break
        ents.append((w[i],w[i+1])); i+=2
    out=bytearray(2048)
    neww=w[:]
    for k,(o,s) in enumerate(ents):
        data=newfiles.get(k, a[o:o+s]) if isinstance(k,int) else None
        off=len(out)
        out+=data; out+=b'\0'*((-len(out))%2048)
        neww[4+2*k]=off; neww[5+2*k]=len(data) if k in newfiles else s
        if k in newfiles: neww[5+2*k]=(len(data)+2047)//2048*2048
    out[:2048]=struct.pack('<512I',*neww)
    return bytes(out)
def build(newfiles,out_bin):
    pak=rebuild_pak(newfiles)
    b=Bin('/root/fm1/fm1st.bin')
    L=26588; n=(len(pak)+2047)//2048
    if 'EXE' in newfiles:
        ex=newfiles['EXE']; assert len(ex)==397312
        b.write(18255,ex)
    b.write(L,pak)
    b.set_rec('PACKAGE.PAK',L,len(pak))
    dummy_end=26588+27648000//2048
    b.set_rec('DUMMY.BIN',L+n,(dummy_end-(L+n))*2048)
    b.save(out_bin)
    return len(pak)
if __name__=='__main__':
    files=pickle.load(open('/root/fm1/proto_files.pkl','rb'))
    n=build(files,'/root/fm1/emu/fm_en.bin')
    open('/root/fm1/emu/fm_en.cue','w').write('FILE "fm_en.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
    print('PAK',n)
