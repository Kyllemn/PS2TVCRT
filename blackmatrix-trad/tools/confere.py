import sys, struct
sys.path.insert(0, sys.argv[4]); import psi_lib as P
a=open(sys.argv[1],'rb').read(); b=open(sys.argv[2],'rb').read(); HS,FIXED,SPEC=P.load_spec(sys.argv[3])
A,_=P.read_cmp2(a); B,_=P.read_cmp2(b); prob=0; mud=0
for k,(x,y) in enumerate(zip(A,B)):
    if x[:8]!=b'PSI22.00': 
        prob+= x!=y; continue
    cx=x[16:16+struct.unpack('<I',x[12:16])[0]]; cy=y[16:16+struct.unpack('<I',y[12:16])[0]]
    ia=P.parse_block(cx,HS,FIXED,SPEC); ib=P.parse_block(cy,HS,FIXED,SPEC)
    if len(ia)!=len(ib): prob+=1; continue
    idxa={v['at']:i for i,v in enumerate(ia)}; idxa[len(cx)]=len(ia)
    idxb={v['at']:i for i,v in enumerate(ib)}; idxb[len(cy)]=len(ib)
    if cx!=cy: mud+=1
    for u,v in zip(ia,ib):
        if (u['op'],u['sub'])!=(v['op'],v['sub']): prob+=1; break
        for (t1,v1),(t2,v2) in zip(u['fields'],v['fields']):
            if t1!=t2: prob+=1
            elif t1=='L' and idxa[v1]!=idxb[v2]: prob+=1
            elif t1 in 'ES' and v1!=v2: prob+=1
print('blocos mudados',mud,'problemas',prob)
