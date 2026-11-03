import sys,json,struct,re,collections; sys.path.insert(0,'/root/fm1'); from align import *
from sem import en_ok
d41=open('/root/fm1/pak/41.bin','rb').read()
d37,strs=ds_strings(37); base37=OVINFO[37][0]
good={o for o,s in strs.items() if en_ok(s)}
names=json.load(open('/root/fm1/names.json'))
OUT=[]
for sf,dov in ((42,15),(43,22)):
    x=open('/root/fm1/pak/%02d.raw'%sf,'rb').read()
    Pk=[]
    for m in re.finditer(rb'\xc0\x1b',x):
        t=struct.unpack('<H',x[m.start()+2:m.start()+4])[0]
        if t<len(d41) and (not Pk or Pk[-1]!=t): Pk.append(t)
    y=open('/root/fm1/ds_ov/ov%03d.bin'%dov,'rb').read()
    Dk=[]
    for o in range(0,len(y)-3,4):
        v=struct.unpack('<I',y[o:o+4])[0]-base37
        if v in good and (not Dk or Dk[-1]!=v): Dk.append(v)
    msg={t:ps1_str(ps1_tokens(d41,t)[0]) for t in Pk}
    PF=[feat_ps1(msg[t]) for t in Pk]; DF=[feat_ds(strs[v]) for v in Dk]
    print(sf,dov,'PS1',len(Pk),'DS',len(Dk))
    for it in range(2):
        pairs=nw(PF,DF,names); names.update(learn(PF,DF,pairs))
    pairs=nw(PF,DF,names); m=dict(pairs)
    for i in range(len(Pk)):
        j=m.get(i); sc=sim(PF[i],DF[j],names) if j is not None else -9
        ref=j if j is not None else 0; best=(sc,j)
        if best[0]<2.5:
            for jj in range(len(DF)):
                s2=sim(PF[i],DF[jj],names)-0.01*abs(jj-ref)
                if s2>best[0]+0.8: best=(s2,jj)
        sc,j=best
        OUT.append(dict(file=41,script=sf,off=Pk[i],jp=msg[Pk[i]],ov=37,doff=Dk[j] if j is not None else None,en=strs[Dk[j]] if j is not None else '',score=round(sc,2)))
json.dump(OUT,open('/root/fm1/match_city.json','w'),ensure_ascii=False,indent=0)
json.dump(names,open('/root/fm1/names.json','w'),ensure_ascii=False,indent=0)
print(collections.Counter('alta' if o['score']>=4 else ('media' if o['score']>=2 else 'baixa') for o in OUT))
import random; random.seed(3)
for o in random.sample(OUT,25): print(o['score'],o['jp'].replace('\n','/')[:42],'||',o['en'].replace('\n','/')[:58])
