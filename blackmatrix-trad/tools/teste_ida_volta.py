import sys, struct
sys.path.insert(0, sys.argv[3])
import psi_lib as P
d=open(sys.argv[1],'rb').read(); HS,FIXED,SPEC=P.load_spec(sys.argv[2])
blocks,ents=P.read_cmp2(d); new=[]; dif=0
for k,b in enumerate(blocks):
    if b[:8]!=b'PSI22.00': new.append(b); continue
    size=struct.unpack('<I',b[12:16])[0]; c=b[16:16+size]
    ins=P.parse_block(c,HS,FIXED,SPEC)
    out=P.rebuild(ins,len(c))
    if out!=c: dif+=1
    nb=b[:12]+struct.pack('<I',len(out))+out
    nb=nb+b[len(nb):] if len(nb)<=len(b) else nb
    new.append(nb)
d2=P.write_cmp2(d,new)
print('blocos diferentes:',dif,' arquivo identico:', d2==d, len(d2), len(d))
