import sys,struct,os
f=open(sys.argv[1],'rb'); out=sys.argv[2]
def sec(l):
    f.seek(l*2352); d=f.read(2352); return d[24:24+2048]
def rd(l,n):
    b=b''
    for i in range((n+2047)//2048): b+=sec(l+i)
    return b[:n]
pvd=sec(16); assert pvd[1:6]==b'CD001'
rl,rs=struct.unpack('<I',pvd[158:162])[0],struct.unpack('<I',pvd[166:170])[0]
lst=[]
def walk(l,s,path):
    d=rd(l,s); o=0
    while o<len(d):
        n=d[o]
        if n==0: o=(o//2048+1)*2048; continue
        el=struct.unpack('<I',d[o+2:o+6])[0]; sz=struct.unpack('<I',d[o+10:o+14])[0]; fl=d[o+25]
        nl=d[o+32]; nm=d[o+33:o+33+nl]
        if nm not in (b'\0',b'\1'):
            nm=nm.decode().split(';')[0]; p=os.path.join(path,nm)
            if fl&2: walk(el,sz,p)
            else:
                lst.append((el,sz,p)); os.makedirs(os.path.join(out,path),exist_ok=True)
                open(os.path.join(out,p),'wb').write(rd(el,sz))
        o+=n
walk(rl,rs,'')
for el,sz,p in sorted(lst): print(el,sz,p)
