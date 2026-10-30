import sys, struct, collections
d=open(sys.argv[1],'rb').read()
n=struct.unpack('<I',d[8:12])[0]
ents=[struct.unpack('<II',d[16+i*8:24+i*8]) for i in range(n)]
TEXT={0x14,0x15,0x20}
SINGLE={0x12,0x17,0x18,0x19}
class Fail(Exception): pass
def parse(code):
    i=0; out=[]
    def w(): 
        nonlocal i
        if i+2>len(code): raise Fail('fim')
        v=struct.unpack('<H',code[i:i+2])[0]; i+=2; return v
    def u32():
        nonlocal i
        if i%4: 
            if w()!=0: raise Fail('pad!=0 @%x'%(i-2))
        v=struct.unpack('<I',code[i:i+4])[0]; i+=4; return v
    def expr():
        while True:
            t=w()
            if t==0: return
            if t==1: u32()
            elif t in (2,3,4): w()
            elif t>=0x80: pass
            else: raise Fail('expr tag %x @%x'%(t,i-2))
    while i<len(code):
        at=i; op=w()
        if op in TEXT:
            c=w(); i+=2*c; out.append((at,op,c))
        elif op==0x1d: c=w(); i+=2*c; u32(); out.append((at,op,c))
        elif op==2: u32(); expr(); out.append((at,op))
        elif op==3: u32(); out.append((at,op))
        elif op in SINGLE: out.append((at,op))
        elif op<0x40: expr(); out.append((at,op))
        else: raise Fail('op %x @%x'%(op,at))
    return out
fails=collections.Counter(); ok=0; ex={}
for k,(s,l) in enumerate(ents):
    b=d[s*2048:(s+l)*2048]
    if b[:8]!=b'PSI22.00': continue
    size=struct.unpack('<I',b[12:16])[0]
    try: parse(b[16:16+size]); ok+=1
    except Fail as e:
        key=str(e).split(' @')[0]; fails[key]+=1; ex.setdefault(key,(k,str(e)))
print('ok',ok,'falhas',sum(fails.values()))
for k,v in fails.most_common(15): print(v,k,ex[k])
