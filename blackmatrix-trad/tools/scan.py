import sys, struct, collections
d=open(sys.argv[1],'rb').read()
n=struct.unpack('<I',d[8:12])[0]
ents=[struct.unpack('<II',d[16+i*8:24+i*8]) for i in range(n)]
def sw(b): return bytes(x for i in range(0,len(b),2) for x in (b[i+1],b[i]))
def sjis_ok(b):
    b=sw(b)
    if len(b)%2: return False
    for i in range(0,len(b),2):
        h=b[i]
        if not (0x81<=h<=0x9f or 0xe0<=h<=0xef): return False
    try: b.decode('shift_jis'); return True
    except: return False
ops=collections.Counter(); total=0; nstr=0; samples=[]
for k,(s,l) in enumerate(ents):
    blk=d[s*2048:(s+l)*2048]
    if blk[:8]!=b'PSI22.00': continue
    i=0
    while i<len(blk)-4:
        op,ln=struct.unpack('<HH',blk[i:i+4])
        if 1<=ln<=200 and sjis_ok(blk[i+4:i+4+ln*2]):
            t=sw(blk[i+4:i+4+ln*2]).decode('shift_jis'); ops[op]+=1; total+=ln; nstr+=1
            if len(samples)<25 and op==0x14: samples.append((k,t))
            i+=4+ln*2; continue
        i+=2
print("entradas",n,"strings",nstr,"caracteres",total)
print("opcodes de texto:",ops.most_common(10))
for k,t in samples: print(k,t)
