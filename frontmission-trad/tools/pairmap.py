import sys,pickle; sys.path.insert(0,'/root/fm1'); from align import *
def prep_ps1(k):
    d,pm,pseq=ps1_mission(k); Pk=[]
    for p,t in pseq:
        if not Pk or Pk[-1]!=t: Pk.append(t)
    return pm,Pk,[feat_ps1(pm[t]) for t in Pk]
def prep_ds(ov):
    strs,dseq=ds_sequence(ov); Dk=[]
    for o,v in dseq:
        if not Dk or Dk[-1]!=v: Dk.append(v)
    return strs,Dk,[feat_ds(strs[v]) for v in Dk]
import json,os
NAMES=json.load(open('/root/fm1/names.json',encoding='utf-8')) if os.path.exists('/root/fm1/names.json') else {}
if __name__=='__main__':
    P={k:prep_ps1(k) for k in range(23,34)}
    D={ov:prep_ds(ov) for ov in range(38,51)}
    res={}
    for k,(pm,Pk,PF) in P.items():
        row=[]
        for ov,(strs,Dk,DF) in D.items():
            pairs=nw(PF,DF,NAMES)
            sc=sum(sim(PF[i],DF[j],NAMES) for i,j in pairs)/max(len(PF),1)
            row.append((round(sc,2),ov))
        row.sort(reverse=True); res[k]=row; print(k,len(Pk),row[:3])
    pickle.dump(res,open('/root/fm1/pairmap.pkl','wb'))
