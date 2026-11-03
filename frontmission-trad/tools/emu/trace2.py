import sys,struct,pickle; sys.path.insert(0,'/root/fm1/emu'); from lr import Emu
S='/tmp/claude-0/-home-user-PS2TVCRT/3556a9de-1f69-5db9-a91d-fdfcea6c5b46/scratchpad/'
e=Emu('/root/fm1/fm1st.cue')
if len(sys.argv)>2: e.load(open(sys.argv[2],'rb').read())
else:
    e.run(2400)
    for k in range(3): e.run(8,['START']); e.run(120)
if len(sys.argv)>3:
    for tok in sys.argv[3].split():
        b,h=tok.split(':'); e.run(int(h),[b] if b!='w' else [])
seen={}; shots=0; last=None
N=int(sys.argv[1])
for f in range(N):
    btn=['A'] if (f%60)<6 else []
    e.run(1,btn)
    r=e.ram()
    for w in (0,1):
        p=struct.unpack('<I',r[0xC1A80+w*0xF0+0x3C:0xC1A80+w*0xF0+0x40])[0]
        if 0x80000000<=p<0x80200000 and p not in seen:
            a=p&0x1fffff; seen[p]=(f,r[a:a+64])
            if shots<40 and (last is None or f-last>120):
                e.run(30); e.shot(S+'t2_%03d.png'%shots); shots+=1; last=f
pickle.dump(seen,open(('/root/fm1/emu/seen_%s.pkl'%sys.argv[4] if len(sys.argv)>4 else '/root/fm1/emu/seen2.pkl'),'wb'))
open(('/root/fm1/emu/st_%s.bin'%sys.argv[4]) if len(sys.argv)>4 else '/root/fm1/emu/state2.bin','wb').write(e.save())
e.shot(S+'t2_fim.png')
print(len(seen)); 
import collections; print(collections.Counter(hex(p&0xfff000) for p in seen).most_common(20))
