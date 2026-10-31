"""Remonta um SFD (Sofdec): mantem as packs de cabecalho (SofdecStream etc.) e os pacotes de audio ADX
originais byte a byte, troca o video MPEG-1 por um novo .m1v (mesma contagem de quadros e GOP).
Pacotes de 2048 bytes como o original: video com 2018 bytes de dados + 12 de cabecalho
(enchimento + STD '60 2e' + PTS/DTS), PTS/DTS no pacote onde comeca a figura, como o SFDMUX.
uso: sfd_mux.py original.SFD novo.m1v saida.SFD"""
import sys, struct, bisect
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from sfdlib import packets, ts
def enc_ts(pre, v):
    return bytes([(pre<<4)|(((v>>30)&7)<<1)|1, (v>>22)&0xff, (((v>>15)&0x7f)<<1)|1, (v>>7)&0xff, ((v&0x7f)<<1)|1])
def pack_hdr(scr, mux=b'\x80\x2a\x59'):
    return b'\x00\x00\x01\xba'+enc_ts(2, scr)+mux
FPS={2:24,5:30,3:25,4:30000/1001}
def main(orig, m1v, out):
    d=open(orig,'rb').read(); es=open(m1v,'rb').read()
    P=packets(d)
    # cabecalho: tudo antes da primeira pack com audio/video
    i0=min(p[6] for p in P if p[1] in (0xC0,0xE0))-12
    head=d[:i0]; mux=d[8:11]
    assert d[i0:i0+4]==b'\x00\x00\x01\xba'
    # audio original (pacote inteiro, sem a pack)
    aud=[]; last=0
    for scr,sid,h,p,pts,dts,o in P:
        if sid==0xC0:
            ln,=struct.unpack('>H',d[o+4:o+6]); raw=d[o:o+6+ln]; assert len(raw)==2036
            if pts is not None: last=pts
            aud.append((last,raw))
    # primeiro PTS de video do original (base)
    vpts=[p[4] for p in P if p[1]==0xE0 and p[4] is not None]; base=min(vpts)
    # figuras do novo video
    i=es.find(b'\x00\x00\x01\xb3'); fr=es[i+7]&15; dur=round(90000/FPS[fr])
    pics=[]; gopbase=0; ngop=0; k=0
    j=0
    while True:
        j=es.find(b'\x00\x00\x01',j)
        if j<0: break
        c=es[j+3]
        if c==0xB8: gopbase+=ngop; ngop=0
        elif c==0x00:
            tref=(es[j+4]<<2)|(es[j+5]>>6); ty=(es[j+5]>>3)&7
            pts=base+(gopbase+tref)*dur
            dts=base+(k+1)*dur if ty in (1,2) else None
            pics.append((j,pts,dts,base+(k+1)*dur)); ngop+=1; k+=1
        j+=3
    starts=[p[0] for p in pics]
    # pacotes de video
    vid=[]; N=2018
    for o in range(0,len(es),N):
        chunk=es[o:o+N]
        a=bisect.bisect_left(starts,o)                         # primeira figura que comeca neste pedaco
        tsb=b'\x0f'
        if a<len(pics) and pics[a][0]<o+len(chunk):
            _,pts,dts,_=pics[a]
            tsb=enc_ts(3,pts)+enc_ts(1,dts) if dts is not None else enc_ts(2,pts)
        cur=bisect.bisect_right(starts,o)-1                    # figura que contem o primeiro byte
        t=pics[cur][3] if cur>=0 else base
        body=b'\xff'*(10-len(tsb)+(1 if len(tsb)==1 else 0)) if len(tsb)!=10 else b''
        body=b'\xff'*(12-2-len(tsb))+b'\x60\x2e'+tsb
        assert len(body)==12
        pk=b'\x00\x00\x01\xe0'+struct.pack('>H',len(body)+len(chunk))+body+chunk
        vid.append((t-dur,pk))
    # intercala por tempo (audio pelo PTS, video um quadro antes da decodificacao)
    todos=sorted([(t,0,n,pk) for n,(t,pk) in enumerate(aud)]+[(t,1,n,pk) for n,(t,pk) in enumerate(vid)])
    outb=bytearray(head); scr=ts(d[i0+4:i0+9]); first=True
    for t,_,_,pk in todos:
        if not first: scr=max(scr+680, t-45000)
        first=False
        b=pack_hdr(scr,mux)+pk
        if len(b)<2048:
            pad=2048-len(b); assert pad>=6
            b+=b'\x00\x00\x01\xbe'+struct.pack('>H',pad-6)+b'\xff'*(pad-6)
        assert len(b)==2048,len(b); outb+=b
    outb+=b'\x00\x00\x01\xb9'; outb+=b'\xff'*((-len(outb))%2048)
    open(out,'wb').write(bytes(outb))
    print('%s: %d quadros, video %d pacotes, audio %d, %.1f MiB -> %.1f MiB'%(out.split('/')[-1],len(pics),len(vid),len(aud),len(d)/2**20,len(outb)/2**20))
if __name__=='__main__': main(*sys.argv[1:4])
