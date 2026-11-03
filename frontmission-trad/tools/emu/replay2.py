import sys; sys.path.insert(0,'/root/fm1/emu'); from lr import Emu
S='/tmp/claude-0/-home-user-PS2TVCRT/3556a9de-1f69-5db9-a91d-fdfcea6c5b46/scratchpad/'
cue,tag,N=sys.argv[1],sys.argv[2],int(sys.argv[3])
e=Emu(cue); e.load(open('/root/fm1/emu/state2.bin','rb').read())
def seq(s):
    for tok in s.split():
        if tok.startswith('w'): e.run(int(tok[1:]))
        elif tok.startswith('x'):
            for i in range(int(tok[1:])): e.run(6,['A']); e.run(40)
        elif tok.startswith('shot'): e.shot(S+'%s_%s.png'%(tag,tok[4:]))
        else:
            b,h=tok.split(':'); e.run(int(h),[b])
seq(sys.argv[4])
n=0
for f in range(N):
    e.run(1,['A'] if f%90<6 else [])
    if f%90==45: e.shot(S+'%s_%03d.png'%(tag,n)); n+=1
open('/root/fm1/emu/%s_fim.bin'%tag,'wb').write(e.save())
