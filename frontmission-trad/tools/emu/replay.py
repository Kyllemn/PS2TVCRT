import sys; sys.path.insert(0,'/root/fm1/emu'); from lr import Emu
S='/tmp/claude-0/-home-user-PS2TVCRT/3556a9de-1f69-5db9-a91d-fdfcea6c5b46/scratchpad/'
cue=sys.argv[1]; tag=sys.argv[2]
e=Emu(cue)
e.run(2400)
for k in range(3): e.run(8,['START']); e.run(120)
for f in range(20000): e.run(1,['A'] if f%60<6 else [])
def seq(s):
    for tok in s.split():
        if tok.startswith('w'): e.run(int(tok[1:]))
        elif tok.startswith('x'):
            for i in range(int(tok[1:])): e.run(6,['A']); e.run(40)
        else:
            b,h=tok.split(':'); e.run(int(h),[b])
seq("START:8 w120 x3 w60")
seq("START:8 w60 A:8 w60 A:8 w90 DOWN:6 w30 A:8 w90")
open('/root/fm1/emu/%s_inicio.bin'%tag,'wb').write(e.save())
n=0
for f in range(int(sys.argv[3])):
    e.run(1,['A'] if f%90<6 else [])
    if f%90==45:
        e.shot(S+'%s_%03d.png'%(tag,n)); n+=1
open('/root/fm1/emu/%s_fim.bin'%tag,'wb').write(e.save())
print(n)
