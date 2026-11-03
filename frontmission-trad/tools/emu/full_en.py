import sys,pickle; sys.path.insert(0,'/root/fm1/emu'); from lr import Emu
S='/tmp/claude-0/-home-user-PS2TVCRT/3556a9de-1f69-5db9-a91d-fdfcea6c5b46/scratchpad/'
cue=sys.argv[1]; tag=sys.argv[2]
e=Emu(cue); e.run(2400)
for k in range(3): e.run(8,['START']); e.run(120)
for f in range(18000): e.run(1,['A'] if f%60<6 else [])
def seq(s):
    for tok in s.split():
        if tok.startswith('w'): e.run(int(tok[1:]))
        elif tok.startswith('x'):
            for i in range(int(tok[1:])): e.run(6,['A']); e.run(40)
        else:
            b,h=tok.split(':'); e.run(int(h),[b])
seq("START:8 w120 x3 w60 START:8 w60 A:8 w60 A:8 w90 DOWN:6 w30 A:8 w90")
for f in range(52000): e.run(1,['A'] if f%60<6 else [])
open('/root/fm1/emu/%s_cidade.bin'%tag,'wb').write(e.save()); e.shot(S+'%s_cidade.png'%tag)
