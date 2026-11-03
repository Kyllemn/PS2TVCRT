"""grava arquivos (mesmo tamanho) por cima na imagem inglesa e gera fm2_pt.bin"""
import sys;sys.path.insert(0,'/root/fm1');sys.path.insert(0,'/root/fm1/bm_tools');sys.path.insert(0,'/root/fm2')
from isopatch import Bin
from isofs import Img
def build(files,src='/root/fm2/fm2_en.bin',dst='/root/fm2/fm2_pt.bin'):
    loc={n:(l,s) for n,l,s in Img(src).walk()}
    b=Bin(src)
    for n,data in files.items():
        l,s=loc[n]
        assert len(data)==s,(n,len(data),s)
        b.write(l,data)
    b.save(dst)
    open(dst[:-4]+'.cue','w').write('FILE "%s" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'%dst.split('/')[-1])
