import sys, struct
d=open(sys.argv[1],'rb').read()
n=struct.unpack('<I',d[8:12])[0]
ents=[struct.unpack('<II',d[16+i*8:24+i*8]) for i in range(n)]
k=int(sys.argv[2]); a=int(sys.argv[3],16) if len(sys.argv)>3 else 0; nw=int(sys.argv[4]) if len(sys.argv)>4 else 80
s,l=ents[k]; b=d[s*2048:(s+l)*2048]
size=struct.unpack('<I',b[12:16])[0]; code=b[16:16+size]
print('bloco',k,'size %x'%size)
out=[]
for o in range(a, min(size, a+nw*2), 2):
    v=struct.unpack('<H',code[o:o+2])[0]; hi=v>>8
    t='%x'%v
    if 0x81<=hi<=0x9f or 0xe0<=hi<=0xef:
        try: t=bytes([hi,v&0xff]).decode('shift_jis')
        except: pass
    out.append(('|' if o%4==0 else '')+t)
for i in range(0,len(out),20): print('%04x: '%(a+i*2)+' '.join(out[i:i+20]))
