"""uso: step.py estado_in estado_out roteiro   (roteiro: 'START:8 w120 A:6 w60 x10 ...'; xN = repete A N vezes)"""
import sys; sys.path.insert(0,'/root/fm1/emu'); from lr import Emu
S='/tmp/claude-0/-home-user-PS2TVCRT/3556a9de-1f69-5db9-a91d-fdfcea6c5b46/scratchpad/'
e=Emu('/root/fm1/fm1st.cue')
if sys.argv[1]!='-': e.load(open(sys.argv[1],'rb').read())
n=0
for tok in sys.argv[3].split():
    if tok.startswith('w'): e.run(int(tok[1:]))
    elif tok.startswith('x'):
        for i in range(int(tok[1:])): e.run(6,['A']); e.run(40)
    elif tok.startswith('shot'):
        e.shot(S+'st_%s.png'%tok[4:])
    else:
        b,h=tok.split(':'); e.run(int(h),b.split('+'))
open(sys.argv[2],'wb').write(e.save())
e.shot(S+'st_last.png')
