import sys, struct, unicodedata
sys.path.insert(0, sys.argv[4])
import psi_lib as P
src, specf, tsv, _, out = sys.argv[1:6]
d=open(src,'rb').read(); HS,FIXED,SPEC=P.load_spec(specf)
def enc(t):
    # sem acentos nesta fase: remove acentos e usa letras de largura cheia
    t=''.join(ch for ch in unicodedata.normalize('NFD',t) if unicodedata.category(ch)!='Mn')
    t=t.replace('～','〜')
    codes=[]
    for ch in t:
        o=ord(ch)
        if ch=='/': codes.append(0xa0ff); continue
        if 'A'<=ch<='Z': codes.append(0x8260+o-65); continue
        if 'a'<=ch<='z': codes.append(0x8281+o-97); continue
        if '0'<=ch<='9': codes.append(0x824f+o-48); continue
        m={' ':0x8140,',':0x8143,'.':0x8144,'!':0x8149,'?':0x8148,'-':0x815b,':':0x8146}
        if ch in m: codes.append(m[ch]); continue
        b=ch.encode('shift_jis'); codes.append(b[0]<<8|b[1])
    return codes
trocas={}
for line in open(tsv,encoding='utf-8'):
    line=line.rstrip('\n')
    if not line: continue
    k,at,txt=line.split('\t'); trocas[(int(k),int(at,16))]=enc(txt)
blocks,ents=P.read_cmp2(d); new=[]; feitas=0
for k,b in enumerate(blocks):
    if b[:8]!=b'PSI22.00': new.append(b); continue
    size=struct.unpack('<I',b[12:16])[0]; c=b[16:16+size]
    ins=P.parse_block(c,HS,FIXED,SPEC)
    for x in ins:
        if (k,x['at']) in trocas:
            for fi,(t,v) in enumerate(x['fields']):
                if t=='T': x['fields'][fi]=('T',trocas[(k,x['at'])]); feitas+=1
    o=P.rebuild(ins,len(c))
    # confere: o bloco novo tem que ser lido de novo sem erro
    P.parse_block(o,HS,FIXED,SPEC)
    nb=b[:12]+struct.pack('<I',len(o))+o
    if len(nb)<=len(b): nb=nb+b'\0'*(len(b)-len(nb)) if o!=c else b
    new.append(nb)
open(out,'wb').write(P.write_cmp2(d,new))
print('trocas feitas',feitas,'de',len(trocas))
