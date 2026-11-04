"""roda a ROM no FCEUmm (referencia) com o mesmo roteiro do runrom e grava frames.
uso: refrun.py rom prefixo frames [ini fim botoes_hex]..."""
import sys; sys.path.insert(0,'/root/fm1/emu')
import lr
lr.CORE='/opt/ref/libretro-fceumm/fceumm_libretro.so'
from lr import Emu
rom,pref,frames=sys.argv[1],sys.argv[2],int(sys.argv[3])
sch=[(int(sys.argv[i]),int(sys.argv[i+1]),int(sys.argv[i+2],16)) for i in range(4,len(sys.argv),3)]
names=['A','B','SELECT','START','UP','DOWN','LEFT','RIGHT']
e=Emu(rom)
for fr in range(frames):
    m=0
    for a,b,v in sch:
        if a<=fr<b: m|=v
    e.run(1,[names[i] for i in range(8) if m>>i&1])
    if fr%30==29: e.shot('%s%05d.png'%(pref,fr+1))
