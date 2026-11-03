import sys,json,struct,collections,pickle; sys.path.insert(0,'/root/fm1')
import fontpairs as FP, wp16
from en_enc import ds_to_units, rewrap
from build_mission import rebuild, encode
M=json.load(open('/root/fm1/match_missions.json'))
C=json.load(open('/root/fm1/match_city.json'))
city={o['off']:rewrap(o['en']) for o in C if o['en']}
trans=collections.defaultdict(dict)
for o in M:
    if o['en']: trans[o['file']][o['off']]=rewrap(o['en'])
# revisao manual dos pares suspeitos (rev1.tsv: indice em suspeitos.json -> ingles)
SUS=json.load(open('/root/fm1/suspeitos.json'))
for ln in open('/root/fm1/rev1.tsv',encoding='utf-8'):
    ln=ln.rstrip('\n')
    if not ln.strip(): continue
    i,en=ln.split('\t',1); e=SUS[int(i)]; en=rewrap(en.replace('\\n','\n'))
    if e['file']==41: city[e['off']]=en
    else: trans[e['file']][e['off']]=en
# correcoes por arquivo/offset (rev2.tsv: arquivo, offset, ingles)
for ln in open('/root/fm1/rev2.tsv',encoding='utf-8'):
    ln=ln.rstrip('\n')
    if not ln.strip(): continue
    f,o,en=ln.split('\t',2); en=rewrap(en.replace('\\n','\n'))
    if int(f)==41: city[int(o)]=en
    else: trans[int(f)][int(o)]=en
# rotulos curtos na cidade (limite de 64 KB)
LBL={'Wanzer Dealer:':'Dealer:','Weapon Dealer:':'Dealer:','Shopkeeper:':'Owner:','Koichi Sakata:':'Sakata:'}
for k,v in list(city.items()):
    h,_,r=v.partition('\n')
    if h in LBL and r: city[k]=LBL[h]+'\n'+r
NAMES=['None','Royd','Ryuji','Natalie','Keith','J.J.','Frederick','Meihua','Paul','Hans','Gregorio','Molly','Ralph','Yeehin','Alder','Bobby','Porunga','Gentz','Peewie']
import build_ui22 as U22
D22=open('/root/fm1/pak/22.raw','rb').read(); R22=json.load(open('/root/fm1/ui22b.json'))
P22,G22,T22=U22.ui_texts(D22,R22)
def pc(texts,cnt,w=1):
    for en in texts:
        for u in ds_to_units(en):
            if u[0]=='t':
                t=u[1]+(' ' if len(u[1])%2 else '')
                for k in range(0,len(t),2): cnt[t[k:k+2]]+=w
cnt=collections.Counter()
pc(NAMES,cnt,10**6)          # nomes primeiro: codigos de 1 byte
pc(list(G22.values()),cnt,10**5)
pc(T22,cnt)
for f,tt in trans.items(): pc(tt.values(),cnt)
pc(city.values(),cnt,4)   # cidade tem limite de 64 KB: prioriza seus pares nos codigos de 1 byte
pmap=FP.assign(cnt)
print('pares',len(pmap),'livres',len(FP.FREE))
cb=FP.code_bytes
out={}
for f in sorted(trans):
    nd,info=rebuild(f,trans[f],pmap,cb); out[f]=nd; print(f,info)
# cidades: texto no 41 (Wp16), referencias nos scripts 42 e 43
from build_text import rebuild_text
d41,_=wp16.decompress(open('/root/fm1/ps1/PACKAGE.PAK','rb').read()[struct.unpack('<I',open('/root/fm1/ps1/PACKAGE.PAK','rb').read()[4*(4+2*41):4*(4+2*41)+4])[0]:])
rf={42:open('/root/fm1/pak/42.raw','rb').read(),43:open('/root/fm1/pak/43.raw','rb').read()}
nd,nr,info=rebuild_text(d41,0x801B0000,rf,0x1b,city,pmap,cb,0x10000,0x7000)
print('cidade',info)
out[41]=wp16.compress(nd); out.update(nr)
# arquivo 22 (interface) no lugar
import refs22 as RF, re as _re
R=pickle.load(open('/root/fm1/refs22.pkl','rb'))
nd22,moved,over22=U22.apply_reloc(D22,R22,pmap,cb,encode,set(r[3] for r in R))
print('ui22: excedentes',len(over22),'realocados',len(moved))
out[22]=nd22
# faixas de texto onde 'c0 19' e letra japonesa, nao referencia
def blocos_texto(k):
    d=open('/root/fm1/pak/%02d.raw'%k,'rb').read()
    refs=[(m.start(),struct.unpack('<H',d[m.start()+2:m.start()+4])[0]-0x8918) for m in _re.finditer(rb'\xc0\x19',d)]
    st=set(i+1 for i in range(len(d)-1) if d[i]==0xff)
    ms=[t for p,t in refs if t in st]
    from build_mission import msg_end
    return (min(ms),max(msg_end(d,x) for x in ms))
BLK={k:blocos_texto(k) for k in range(23,34)}
def valido(r):
    nm,kind,pos=r[0],r[1],r[2]
    if kind=='c019':
        k=int(nm) if nm.isdigit() else -1
        if k in BLK and BLK[k][0]<=pos<BLK[k][1]: return False
        if k==41 and pos>=0x7000: return False
    return True
PAKB=open('/root/fm1/ps1/PACKAGE.PAK','rb').read()
def entry(k):
    o,sz=struct.unpack('<2I',PAKB[4*(4+2*k):4*(4+2*k)+8]); return PAKB[o:o+sz]
cur={}
def get(nm):
    if nm in cur: return cur[nm]
    if nm=='EXE': cur[nm]=open('/root/fm1/ps1/SLPM_873.17','rb').read()[2048:]
    else:
        k=int(nm)
        if k in out:
            x=out[k]; cur[nm]=wp16.decompress(x)[0] if x[:4]==b'Wp16' else x
        else:
            e=entry(k); cur[nm]=wp16.decompress(e)[0] if e[:4]==b'Wp16' else open('/root/fm1/pak/%02d.raw'%k,'rb').read()
    return cur[nm]
npat=0
for r in R:
    if r[3] in moved and valido(r):
        cur[r[0]]=RF.patch(get(r[0]),r[1],r[2],moved[r[3]],r[4]); npat+=1
print('referencias corrigidas',npat,'em',sorted(cur))
# nome do protagonista nas tabelas ASCII (romanizacao japonesa 'Roid' -> 'Royd' do DS, mesmo tamanho)
for nm in ('00','36','47','49'):
    x=get(nm)
    if b'Roid@' in x: cur[nm]=x.replace(b'Roid@',b'Royd@')
for nm,x in cur.items():
    if nm=='EXE':
        ex=open('/root/fm1/ps1/SLPM_873.17','rb').read(); out['EXE']=ex[:2048]+x
    else:
        k=int(nm); comp=entry(k)[:4]==b'Wp16'
        out[k]=wp16.compress(x) if comp else x
F=bytearray(open('/root/fm1/pak/51.raw','rb').read())
for p,gi in pmap.items(): F[gi*64:gi*64+64]=FP.tiles_for(p)
out[51]=bytes(F)
pickle.dump(out,open('/root/fm1/proto_files.pkl','wb')); pickle.dump(pmap,open('/root/fm1/pmap.pkl','wb'))
