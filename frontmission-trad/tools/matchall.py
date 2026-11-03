import sys,pickle,json; sys.path.insert(0,'/root/fm1'); from pairmap import *
OUT=[]
pairsmap={k:k+15 for k in range(23,34)}
names=dict(NAMES)
for k,ov in pairsmap.items():
    pm,Pk,PF=prep_ps1(k); strs,Dk,DF=prep_ds(ov)
    for it in range(2):
        pairs=nw(PF,DF,names); names.update(learn(PF,DF,pairs))
    pairs=nw(PF,DF,names)
    m={i:j for i,j in pairs}
    for i in range(len(Pk)):
        j=m.get(i); sc=sim(PF[i],DF[j],names) if j is not None else -9
        # janela local
        ref=j
        if ref is None:
            prev=[m[x] for x in range(i-1,-1,-1) if x in m][:1]; ref=prev[0] if prev else 0
        best=(sc,j)
        for jj in range(max(0,ref-8),min(len(DF),ref+9)):
            s2=sim(PF[i],DF[jj],names)-0.15*abs(jj-ref)
            if s2>best[0]+0.5: best=(s2,jj)
        if best[0]<2.5:
            for jj in range(len(DF)):
                s2=sim(PF[i],DF[jj],names)-0.01*abs(jj-ref)
                if s2>best[0]+0.8: best=(s2,jj)
        sc,j=best
        OUT.append(dict(file=k,off=Pk[i],jp=pm[Pk[i]],ov=ov,doff=Dk[j] if j is not None else None,en=strs[Dk[j]] if j is not None else '',score=round(sc,2)))
json.dump(OUT,open('/root/fm1/match_missions.json','w'),ensure_ascii=False,indent=0)
json.dump(names,open('/root/fm1/names.json','w'),ensure_ascii=False,indent=0)
import collections
c=collections.Counter('alta' if o['score']>=4 else ('media' if o['score']>=2 else 'baixa') for o in OUT)
print(len(OUT),c); print(names)
