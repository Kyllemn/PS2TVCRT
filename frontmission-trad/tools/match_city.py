import sys,json,struct; sys.path.insert(0,'/root/fm1'); from align import *
from sem import jp_coverage,en_ok
import re
B=0x801B0000
d=open('/root/fm1/pak/41.bin','rb').read()
starts=set(i+1 for i in range(len(d)-1) if d[i]==0xff)
ptrs=[]
for o in range(0,len(d)-3,4):
    v=struct.unpack('<I',d[o:o+4])[0]
    if B<=v<B+len(d): ptrs.append((o,v-B))
msg={}
for o,t in ptrs:
    if t in starts or t==0 or d[t-1] in (0,0xff):
        tk,_=ps1_tokens(d,t); s=ps1_str(tk)
        body=re.sub(r'\[[0-9A-F]{4}\]','',s).strip()
        if s and jp_coverage(s)>=0.68 and (re.match(r'^[^\n]{1,20}：\n',s) or (body and body[-1] in '。！？‥」') ):
            msg[t]=s
Pk=[]
for o,t in ptrs:
    if t in msg and (not Pk or Pk[-1]!=t): Pk.append(t)
strs,dseq=ds_sequence(37); Dk=[]
for o,v in dseq:
    if en_ok(strs[v]) and (not Dk or Dk[-1]!=v): Dk.append(v)
names=json.load(open('/root/fm1/names.json'))
PF=[feat_ps1(msg[t]) for t in Pk]; DF=[feat_ds(strs[v]) for v in Dk]
print('PS1',len(Pk),'DS',len(Dk))
for it in range(2):
    pairs=nw(PF,DF,names); names.update(learn(PF,DF,pairs))
pairs=nw(PF,DF,names); m=dict(pairs)
OUT=[]
for i in range(len(Pk)):
    j=m.get(i); sc=sim(PF[i],DF[j],names) if j is not None else -9
    ref=j if j is not None else 0
    best=(sc,j)
    if best[0]<2.5:
        for jj in range(len(DF)):
            s2=sim(PF[i],DF[jj],names)-0.01*abs(jj-ref)
            if s2>best[0]+0.8: best=(s2,jj)
    sc,j=best
    OUT.append(dict(file=41,off=Pk[i],jp=msg[Pk[i]],ov=37,doff=Dk[j] if j is not None else None,en=strs[Dk[j]] if j is not None else '',score=round(sc,2)))
json.dump(OUT,open('/root/fm1/match_city.json','w'),ensure_ascii=False,indent=0)
json.dump(names,open('/root/fm1/names.json','w'),ensure_ascii=False,indent=0)
import collections
print(collections.Counter('alta' if o['score']>=4 else ('media' if o['score']>=2 else 'baixa') for o in OUT))
for o in OUT[:25]: print(o['score'],o['jp'].replace('\n','/')[:40],'||',o['en'].replace('\n','/')[:55])
