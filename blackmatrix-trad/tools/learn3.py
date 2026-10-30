import sys, struct, collections, itertools
d=open(sys.argv[1],'rb').read()
n=struct.unpack('<I',d[8:12])[0]
ents=[struct.unpack('<II',d[16+i*8:24+i*8]) for i in range(n)]
blocks=[]
for k,(s,l) in enumerate(ents):
    b=d[s*2048:(s+l)*2048]
    if b[:8]!=b'PSI22.00': continue
    size=struct.unpack('<I',b[12:16])[0]; blocks.append((k,b[16:16+size]))
blocks.sort(key=lambda x:len(x[1]))
def W(c,i): return struct.unpack('<H',c[i:i+2])[0] if i+2<=len(c) else None
def expr(c,i,needvar=False):
    first=True
    while True:
        t=W(c,i)
        if t is None: return None
        i+=2
        if t==0: return None if first else i
        if first and needvar and t not in (2,3,4): return None
        first=False
        if t==1:
            if i%4:
                if W(c,i)!=0: return None
                i+=2
            if i+4>len(c): return None
            i+=4
        elif t in (2,3,4): i+=2
        elif t>=0x80: pass
        else: return None
def field(c,i,f):
    if i is None: return None
    if f=='E': return expr(c,i)
    if f=='V': return expr(c,i,True)
    if f=='S': return i+2 if i+2<=len(c) else None
    if f=='L':
        if i%4:
            if W(c,i)!=0: return None
            i+=2
        return i+4 if i+4<=len(c) else None
    if f=='T':
        x=W(c,i); return None if x is None or i+2+2*x>len(c) else i+2+2*x
FIXED={0x14:'T',0x15:'T',0x20:'T',0x1d:'TL',0x02:'LE',0x03:'L',0x12:'',0x17:'',0x18:'',0x01:'V',0x1c:'EEE',0x10:'E',0x16:'E',0x2a:'',0x0c:''}
HS={0x08,0x09,0x0a,0x0b}
CANDS=['']+['E'*k for k in range(1,9)]+['S'+'E'*k for k in range(0,5)]+['SS'+'E'*k for k in range(0,5)]+['SL'+'E'*k for k in range(0,4)]+['L'+'E'*k for k in range(0,4)]+['SSS','SSSE','SSL','SSLE']
SPEC={(0xa,1):'E',(0xa,0x10):'E',(0xa,2):'E',(0xa,0x24):'EE',(0xa,0x27):'EEEE',(8,0x25):'',(8,0x26):'',(8,0x18):'',(8,0x19):'E',(9,0x29):'SLE',(0xb,1):'',(0xb,2):'EE',(8,4):'E',(9,5):'E',(8,5):'',(9,6):'',(0xa,0x25):'E',(0xa,0x20):'',(0xa,0x1e):'',(0xa,5):'EE',(0xa,4):'EE',(8,0xb):'E',(8,0xd):'',(0xa,0x26):'E',(0xa,0x29):'E',(0xa,3):'E',(9,0x1c):'EEE',(9,0x15):'E',(0xa,0x21):'',(9,0x20):'E',(0xb,0x12):'EEE',(8,0x14):'EEEE',(8,0x15):'ET',(0xb,0xb):'EEE',(0xb,0x17):'EE',(0xb,0x14):'EEE',(0xb,0x13):'EEE',(0xb,0x15):'EE'}
def step(c,i,spec):
    """devolve lista de (nova_pos, chave_aprendida ou None, fmt)"""
    op=W(c,i)
    if op is None: return []
    j=i+2
    if op in FIXED:
        for f in FIXED[op]: j=field(c,j,f)
        return [(j,None,None)] if j is not None else []
    if op==0 or op>0x60: return []
    sub=None
    if op in HS:
        sub=W(c,j); j+=2
        if sub is None: return []
    key=(op,sub)
    fmts=[spec[key]] if key in spec else CANDS
    out=[]
    for fm in fmts:
        k=j
        for f in fm: k=field(c,k,f)
        if k is not None: out.append((k,key if key not in spec else None,fm))
    return out
def dfs(c,i,spec,depth):
    if i==len(c): return spec
    if depth==0: return spec
    for (ni,key,fm) in step(c,i,spec):
        if ni>len(c): continue
        sp=spec
        if key is not None: sp=dict(spec); sp[key]=fm
        r=dfs(c,ni,sp,depth-1)
        if r is not None: return r
    return None
def full(c,spec):
    i=0
    while i<len(c):
        st=step(c,i,spec)
        if len(st)!=1 or st[0][1] is not None: return False,i
        i=st[0][0]
    return i==len(c),i
changed=True; rounds=0
while changed and rounds<30:
    changed=False; rounds+=1
    for k,c in blocks:
        i=0
        while i<len(c):
            st=step(c,i,SPEC)
            if not st: break
            if len(st)==1 and st[0][1] is None: i=st[0][0]; continue
            r=dfs(c,i,SPEC,10)
            if r is None: break
            if r!=SPEC: SPEC=r; changed=True
            st=step(c,i,SPEC)
            if len(st)!=1: break
            i=st[0][0]
ok=sum(1 for k,c in blocks if full(c,SPEC)[0])
print('ok',ok,'de',len(blocks),'rodadas',rounds)
bad=[(k,full(c,SPEC)[1]) for k,c in blocks if not full(c,SPEC)[0]]
print('ruins',bad[:20])
for (op,sub),f in sorted(SPEC.items(), key=lambda x:(x[0][0],x[0][1] if x[0][1] is not None else -1)):
    print('%02x:%s=%s'%(op,'%x'%sub if sub is not None else '-',f), end='  ')
print()
import json
json.dump({'hs':sorted(HS),'fixed':{('%x'%k):v for k,v in FIXED.items()},'spec':{('%x:%s'%(k[0],'-' if k[1] is None else '%x'%k[1])):v for k,v in SPEC.items()}},open(sys.argv[1]+'.spec.json','w'),indent=0)
