import sys,struct,re,json; sys.path.insert(0,'/root/fm1')
from en_enc import ds_to_units
B=0x8918; LIMIT=0x10000-B   # o arquivo inteiro precisa ficar abaixo de 0x801A0000
def msg_end(d,s):
    i=s
    while i<len(d):
        b=d[i]
        if b==0xff: return i+1
        if b>=0xf0 and b!=0xfe: i+=2; continue
        if 0xc0<=b<=0xcf: i+=2; continue
        i+=1
    return i
def encode(en,pmap,cb):
    out=bytearray()
    for u in ds_to_units(en):
        if u[0]=='nl': out.append(0xfe)
        elif u[0]=='c': out+=bytes(u[1])
        else:
            t=u[1]
            if len(t)%2: t+=' '
            for k in range(0,len(t),2): out+=cb(pmap[t[k:k+2]])
    out.append(0xff); return bytes(out)
def rebuild(k, trans, pmap, cb):
    """trans: offset_antigo -> texto ingles (ds_str). devolve novo arquivo"""
    d=bytearray(open('/root/fm1/pak/%02d.raw'%k,'rb').read())
    refs=[(m.start(),struct.unpack('<H',d[m.start()+2:m.start()+4])[0]-B) for m in re.finditer(rb'\xc0\x19',d)]
    starts=set(i+1 for i in range(len(d)-1) if d[i]==0xff)
    msgs=sorted(set(t for p,t in refs if t in starts or t in trans))
    lo=min(msgs); hi=max(msg_end(d,s) for s in msgs)
    # mensagens dentro do bloco
    inblk=[s for s in msgs if lo<=s<hi]
    used_end=len(bytes(d).rstrip(b'\0'))
    newpos={}; blk=bytearray(); over=bytearray(); over_base=(used_end+3)&~3
    seen={}
    for s in inblk:
        e=msg_end(d,s)
        data=encode(trans[s],pmap,cb) if s in trans else bytes(d[s:e])
        if data in seen: newpos[s]=seen[data]; continue
        if lo+len(blk)+len(data)<=hi:
            newpos[s]=lo+len(blk); blk+=data
        else:
            newpos[s]=over_base+len(over); over+=data
        seen[data]=newpos[s]
    d[lo:hi]=bytes(blk)+b'\xff'*(hi-lo-len(blk))
    if over:
        if len(d)<over_base+len(over): d+=b'\0'*(over_base+len(over)-len(d))
        d[over_base:over_base+len(over)]=over
    assert len(d)<=LIMIT, ('estourou memoria',k,hex(len(d)))
    for p,t in refs:
        if t in newpos:
            struct.pack_into('<H',d,p+2,newpos[t]+B)
    return bytes(d), dict(bloco=hi-lo,usado=len(blk),overflow=len(over),tam=len(d))
