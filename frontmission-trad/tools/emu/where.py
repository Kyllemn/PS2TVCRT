import sys,os; sys.path.insert(0,'/root/fm1/emu'); from lr import Emu
e=Emu('/root/fm1/fm1st.cue'); e.load(open(sys.argv[1],'rb').read()); e.run(2)
r=e.ram()
for f in sorted(os.listdir('/root/fm1/pak')):
    d=open('/root/fm1/pak/'+f,'rb').read()
    hits={}
    for o in range(0,len(d)-32,max(32,len(d)//64)):
        ch=d[o:o+32]
        if ch.count(0)>16 or len(set(ch))<8: continue
        p=r.find(ch)
        if p>=0: hits[p-o]=hits.get(p-o,0)+1
    if hits:
        b,n=max(hits.items(),key=lambda t:t[1])
        if n>=3: print(f,len(d),'base 0x%08x'%(0x80000000+b),n)
