import sys, os
img, outdir = sys.argv[1], sys.argv[2]; BASE=45000
os.makedirs(outdir, exist_ok=True)
F=open(img,'rb')
for line in open(sys.argv[3]):
    p=line.split()
    if len(p)!=3 or not p[0].startswith('/'): continue
    name,lba,size=p[0][1:],int(p[1]),int(p[2])
    if name.endswith('.SFD') or name in ('PCM.DAT',): continue
    out=open(os.path.join(outdir,name),'wb'); left=size; s=lba
    while left>0:
        F.seek((s-BASE)*2352+16); b=F.read(2048); out.write(b[:min(2048,left)]); left-=2048; s+=1
