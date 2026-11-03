"""Reconstrucao generica: arquivo de texto + arquivos com referencias 'c0 BB LL HH' (endereco 0x80BBHHLL)."""
import sys,struct,re; sys.path.insert(0,'/root/fm1')
from build_mission import msg_end, encode
def rebuild_text(data, base, refs_files, bank, trans, pmap, cb, limit, region_lo=0):
    """data: bytes do arquivo de texto (carregado em base); refs_files: {nome: bytes};
    trans: offset->ingles. devolve (novo_data, {nome: novo_bytes}, info)"""
    d=bytearray(data); B=base&0xffff
    starts=set(i+1 for i in range(len(d)-1) if d[i]==0xff)
    allrefs={}
    for nm,x in refs_files.items():
        allrefs[nm]=[(m.start(),struct.unpack('<H',x[m.start()+2:m.start()+4])[0]-B) for m in re.finditer(bytes([0xc0,bank]),x)]
    msgs=sorted(set(t for rl in allrefs.values() for p,t in rl if region_lo<=t<len(d) and (t in starts or t in trans)))
    lo=min(msgs); hi=max(msg_end(d,s) for s in msgs)
    used_end=len(bytes(d).rstrip(b'\0'))
    newpos={}; blk=bytearray(); over=bytearray(); ob=(max(used_end,hi)+3)&~3
    seen={}
    for s in msgs:
        e=msg_end(d,s)
        enc=encode(trans[s],pmap,cb) if s in trans else bytes(d[s:e])
        if enc in seen: newpos[s]=seen[enc]; continue
        if lo+len(blk)+len(enc)<=hi: newpos[s]=lo+len(blk); blk+=enc
        else: newpos[s]=ob+len(over); over+=enc
        seen[enc]=newpos[s]
    d[lo:hi]=bytes(blk)+b'\xff'*(hi-lo-len(blk))
    if over:
        if len(d)<ob+len(over): d+=b'\0'*(ob+len(over)-len(d))
        d[ob:ob+len(over)]=over
    assert len(d)<=limit,('estourou',hex(len(d)),hex(limit))
    out={}
    for nm,x in refs_files.items():
        y=bytearray(x)
        for p,t in allrefs[nm]:
            if t in newpos: struct.pack_into('<H',y,p+2,(newpos[t]+B)&0xffff)
        out[nm]=bytes(y)
    return bytes(d),out,dict(bloco=hi-lo,usado=len(blk),overflow=len(over),tam=len(d))
