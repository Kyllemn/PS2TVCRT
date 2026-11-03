import sys,json,struct,collections,pickle; sys.path.insert(0,'/root/fm1')
import fontpairs as FP, wp16
from en_enc import ds_to_units, rewrap
from build_mission import rebuild, encode
M=json.load(open('/root/fm1/match_missions.json'))
trans=collections.defaultdict(dict)
for o in M:
    if o['en']: trans[o['file']][o['off']]=rewrap(o['en'])
NAMES=['None','Royd','Ryuji','Natalie','Keith','J.J.','Frederick','Meihua','Paul','Hans','Gregorio','Molly','Ralph','Yeehin','Alder','Bobby','Porunga','Gentz','Peewie']
def pc(texts,cnt,w=1):
    for en in texts:
        for u in ds_to_units(en):
            if u[0]=='t':
                t=u[1]+(' ' if len(u[1])%2 else '')
                for k in range(0,len(t),2): cnt[t[k:k+2]]+=w
cnt=collections.Counter()
pc(NAMES,cnt,10**6)          # nomes primeiro: codigos de 1 byte
for f,tt in trans.items(): pc(tt.values(),cnt)
pmap=FP.assign(cnt)
print('pares',len(pmap),'livres',len(FP.FREE))
cb=FP.code_bytes
out={}
for f in sorted(trans):
    nd,info=rebuild(f,trans[f],pmap,cb); out[f]=nd; print(f,info)
# nomes (arquivo 22 + tabela de ponteiros no game.bin)
d22=bytearray(open('/root/fm1/pak/22.raw','rb').read()); B=0x80191000
g=bytearray(open('/root/fm1/pak/00.bin','rb').read())
ptrs=[0x75af0+4*i for i in range(19)]
old=[struct.unpack('<I',g[p:p+4])[0]-B for p in ptrs]
lo=min(old); hi=max(old); 
while d22[hi]!=0xff: hi+=1
hi+=1
blk=bytearray()
for p,nm in zip(ptrs,NAMES):
    e=encode(nm,pmap,cb); assert len(e)-1<=6,(nm,len(e))
    struct.pack_into('<I',g,p,B+lo+len(blk)); blk+=e
assert len(blk)<=hi-lo,(len(blk),hi-lo)
d22[lo:hi]=bytes(blk)+b'\xff'*(hi-lo-len(blk))
out[22]=bytes(d22)
out[0]=wp16.compress(bytes(g))
F=bytearray(open('/root/fm1/pak/51.raw','rb').read())
for p,gi in pmap.items(): F[gi*64:gi*64+64]=FP.tiles_for(p)
out[51]=bytes(F)
pickle.dump(out,open('/root/fm1/proto_files.pkl','wb')); pickle.dump(pmap,open('/root/fm1/pmap.pkl','wb'))
