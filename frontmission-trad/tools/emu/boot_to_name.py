import sys; sys.path.insert(0,'/root/fm1/emu'); from lr import Emu
S='/tmp/claude-0/-home-user-PS2TVCRT/3556a9de-1f69-5db9-a91d-fdfcea6c5b46/scratchpad/'
e=Emu(sys.argv[1]); e.run(2400)
for k in range(3): e.run(8,['START']); e.run(120)
states={}
for f in range(20000):
    e.run(1,['A'] if f%60<6 else [])
    if f%600==300:
        e.shot(S+'bn_%05d.png'%f)
    if f in (17000,18000,18600,19000,19300,19600): states[f]=e.save()
import pickle; pickle.dump(states,open('/root/fm1/emu/bn_states.pkl','wb'))
