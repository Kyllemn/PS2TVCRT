import sys; sys.path.insert(0,'/root/fm1/emu'); from lr import Emu
S='/tmp/claude-0/-home-user-PS2TVCRT/3556a9de-1f69-5db9-a91d-fdfcea6c5b46/scratchpad/'
cue,st,out,N,tag=sys.argv[1],sys.argv[2],sys.argv[3],int(sys.argv[4]),sys.argv[5]
btn=sys.argv[6] if len(sys.argv)>6 else 'A'
e=Emu(cue); e.load(open(st,'rb').read())
n=0
for f in range(N):
    e.run(1,[btn] if f%60<6 else [])
    if f%(N//16)==0: e.shot(S+'%s_%02d.png'%(tag,n)); n+=1
open(out,'wb').write(e.save()); e.shot(S+'%s_fim.png'%tag)
from PIL import Image
import glob
fs=sorted(glob.glob(S+'%s_[0-9]*.png'%tag))+[S+'%s_fim.png'%tag]
m=Image.new('RGB',(1280,(len(fs)+3)//4*240))
for k,f in enumerate(fs): m.paste(Image.open(f).convert('RGB').resize((320,240)),((k%4)*320,(k//4)*240))
m.save(S+'%s_grid.png'%tag)
