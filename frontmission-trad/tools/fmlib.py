import sys,struct,re,pickle
sys.path.insert(0,'/root/fm1'); import fmtext
T={}
for l in open('/root/fm1/tbl/fm1st.tbl',encoding='utf-8'):
    i,c=l.rstrip('\n').split('\t'); T[int(i)]=c
fmtext.OCR.clear(); fmtext.OCR.update(T)
from fmtext import ps1_tokens, ps1_str, ds_tokens, ds_str
OVINFO=pickle.load(open('/root/fm1/ds_ovinfo.pkl','rb'))
def ds_strings(ov):
    """mapa offset->texto de todas as strings validas do overlay"""
    d=open('/root/fm1/ds_ov/ov%03d.bin'%ov,'rb').read()
    out={}
    i=0
    while True:
        j=d.find(b'\xff',i)
        if j<0: break
        k=i
        while k<j and d[k]==0: k+=1
        t,e=ds_tokens(d,k); s=ds_str(t)
        good=sum(1 for c in s if c.isalnum() or c in " .,!?':\n-[]")/max(1,len(s))
        if len(s)>=2 and good>0.85 and e==j+1: out[k]=s
        i=j+1
    return d,out
def ds_sequence(ov):
    d,strs=ds_strings(ov); base=OVINFO[ov][0]
    seq=[]
    for o in range(0,len(d)-3,4):
        v=struct.unpack('<I',d[o:o+4])[0]-base
        if v in strs: seq.append((o,v))
    return strs,seq
def ps1_mission(k, base=0x8918, rng=None):
    d=open('/root/fm1/pak/%02d.raw'%k,'rb').read()
    starts=set(i+1 for i in range(len(d)-1) if d[i]==0xff)
    refs=[(m.start(),struct.unpack('<H',d[m.start()+2:m.start()+4])[0]-base) for m in re.finditer(rb'\xc0\x19',d)]
    msgs={}
    lo=min(t for p,t in refs if t in starts)-0x40
    for p,t in refs:
        if t in starts or (0<=t<len(d) and (t==lo or d[t-1]==0xff)):
            if 0<=t<len(d):
                tk,_=ps1_tokens(d,t); msgs[t]=ps1_str(tk)
    seq=[(p,t) for p,t in refs if t in msgs]
    return d,msgs,seq
