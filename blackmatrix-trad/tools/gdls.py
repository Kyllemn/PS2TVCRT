import sys, struct
F=open(sys.argv[1],'rb'); BASE=45000
def sec(lba,n=1):
    out=b''
    for i in range(n):
        F.seek((lba-BASE+i)*2352+16); out+=F.read(2048)
    return out
pvd=sec(BASE+16)
assert pvd[1:6]==b'CD001'
rl=struct.unpack('<I',pvd[158:162])[0]; rs=struct.unpack('<I',pvd[166:170])[0]
files=[]
def walk(lba,size,path):
    d=sec(lba,(size+2047)//2048); o=0
    while o<len(d):
        l=d[o]
        if l==0: o=(o//2048+1)*2048; continue
        nl=d[o+32]; name=d[o+33:o+33+nl]; flg=d[o+25]
        flba=struct.unpack('<I',d[o+2:o+6])[0]; fs=struct.unpack('<I',d[o+10:o+14])[0]
        if name not in (b'\x00',b'\x01'):
            n=name.decode('ascii','replace').split(';')[0]
            if flg&2: walk(flba,fs,path+n+'/')
            else: files.append((path+n,flba,fs))
        o+=l
walk(rl,rs,'/')
for p,l,s in files: print("%-40s %8d %10d"%(p,l,s))
print(len(files),"arquivos", sum(s for _,_,s in files)//1048576,"MB")
