import sys, struct, json
d=open(sys.argv[1],'rb').read(); J=json.load(open(sys.argv[2]))
HS=set(J['hs']); FIXED={int(k,16):v for k,v in J['fixed'].items()}
SPEC={}
for k,v in J['spec'].items():
    a,b=k.split(':'); SPEC[(int(a,16),None if b=='-' else int(b,16))]=v
n=struct.unpack('<I',d[8:12])[0]
ents=[struct.unpack('<II',d[16+i*8:24+i*8]) for i in range(n)]
def W(c,i): return struct.unpack('<H',c[i:i+2])[0]
def expr(c,i):
    while True:
        t=W(c,i); i+=2
        if t==0: return i
        if t==1:
            if i%4: i+=2
            i+=4
        elif t in (2,3,4): i+=2
import collections
CODES=collections.Counter()
def ch(v):
    if v==0xa0ff: return '/'
    try:
        if v>=0x8140 and (v>>8) not in (0xff,): return bytes([v>>8,v&0xff]).decode('shift_jis')
    except Exception: pass
    CODES[v]+=1; return '{%04x}'%v
def sj(c,i,nch):
    return ''.join(ch(v) for v in struct.unpack('<%dH'%nch,c[i:i+2*nch]))
out=open(sys.argv[3],'w',encoding='utf-8'); tot=0; nst=0
for k,(s,l) in enumerate(ents):
    b=d[s*2048:(s+l)*2048]
    if b[:8]!=b'PSI22.00': continue
    size=struct.unpack('<I',b[12:16])[0]; c=b[16:16+size]; i=0
    while i<len(c):
        at=i; op=W(c,i); i+=2; sub=None
        if op in FIXED: fmt=FIXED[op]
        else:
            if op in HS: sub=W(c,i); i+=2
            fmt=SPEC[(op,sub)]
        for f in fmt:
            if f in 'EV': i=expr(c,i)
            elif f=='S': i+=2
            elif f=='L':
                if i%4: i+=2
                i+=4
            elif f=='T':
                nch=W(c,i); t=sj(c,i+2,nch)
                tipo={0x14:'FALA',0x15:'FALA2',0x20:'PERGUNTA',0x1d:'OPCAO',0x08:'NOME'}.get(op,'%x'%op)
                out.write('%03d\t%04x\t%s\t%s\n'%(k,at,tipo,t)); tot+=nch; nst+=1
                i+=2+2*nch
print('textos',nst,'caracteres',tot); print('codigos especiais',[('%04x'%k,v) for k,v in CODES.most_common(20)])
