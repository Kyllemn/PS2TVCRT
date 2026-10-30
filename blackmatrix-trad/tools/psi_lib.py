"""Leitura/escrita do script PSI22 (SCENARIO.DAT) do Black/Matrix AD."""
import struct, json
def load_spec(path):
    J=json.load(open(path))
    HS=set(J['hs']); FIXED={int(k,16):v for k,v in J['fixed'].items()}
    SPEC={}
    for k,v in J['spec'].items():
        a,b=k.split(':'); SPEC[(int(a,16),None if b=='-' else int(b,16))]=v
    return HS,FIXED,SPEC
def W(c,i): return struct.unpack('<H',c[i:i+2])[0]
def U(c,i): return struct.unpack('<I',c[i:i+4])[0]
def parse_expr(c,i):
    toks=[]
    while True:
        t=W(c,i); i+=2
        if t==0: return toks,i
        if t==1:
            if i%4: i+=2
            toks.append((1,U(c,i))); i+=4
        elif t in (2,3,4): toks.append((t,W(c,i))); i+=2
        else: toks.append((t,None))
def parse_block(c,HS,FIXED,SPEC):
    """lista de instrucoes: dict(at, op, sub, fields=[(tipo, valor)])"""
    ins=[]; i=0
    while i<len(c):
        at=i; op=W(c,i); i+=2; sub=None
        if op in FIXED: fmt=FIXED[op]
        else:
            if op in HS: sub=W(c,i); i+=2
            fmt=SPEC[(op,sub)]
        fields=[]
        for f in fmt:
            if f in 'EV':
                t,i=parse_expr(c,i); fields.append(('E',t))
            elif f=='S': fields.append(('S',W(c,i))); i+=2
            elif f=='L':
                if i%4: i+=2
                fields.append(('L',U(c,i))); i+=4
            elif f=='T':
                n=W(c,i); fields.append(('T',list(struct.unpack('<%dH'%n,c[i+2:i+2+2*n])))); i+=2+2*n
        ins.append({'at':at,'op':op,'sub':sub,'fields':fields})
    assert i==len(c)
    return ins
def build_block(ins):
    """serializa; devolve bytes e mapa at_antigo->at_novo. Saltos sao corrigidos."""
    def emit(remap):
        out=bytearray(); amap={}
        def w(v): out.extend(struct.pack('<H',v))
        def align():
            if len(out)%4: w(0)
        for x in ins:
            amap[x['at']]=len(out)
            w(x['op'])
            if x['sub'] is not None: w(x['sub'])
            for t,v in x['fields']:
                if t=='E':
                    for tag,val in v:
                        w(tag)
                        if tag==1: align(); out.extend(struct.pack('<I',val))
                        elif tag in (2,3,4): w(val)
                    w(0)
                elif t=='S': w(v)
                elif t=='L':
                    align(); out.extend(struct.pack('<I',remap(v)))
                elif t=='T':
                    w(len(v)); [w(ch) for ch in v]
        return bytes(out),amap
    oldsize=None
    b1,amap=emit(lambda v:v)
    end_old=max(x['at'] for x in ins)  # placeholder
    def remap(v,amap=amap):
        if v in amap: return amap[v]
        return v  # alvo = fim do bloco tratado abaixo
    # alvo igual ao tamanho antigo = fim do bloco
    return b1,amap
def rebuild(ins,oldlen):
    b,amap=build_block(ins)
    amap[oldlen]=None
    # segunda passada com os enderecos novos (tamanho dos alinhamentos nao depende dos valores)
    newlen=len(b)
    def rm(v):
        if v==oldlen: return newlen
        return amap[v]
    out,_=_emit(ins,rm)
    assert len(out)==newlen
    return out
def _emit(ins,remap):
    out=bytearray(); amap={}
    def w(v): out.extend(struct.pack('<H',v))
    def align():
        if len(out)%4: w(0)
    for x in ins:
        amap[x['at']]=len(out)
        w(x['op'])
        if x['sub'] is not None: w(x['sub'])
        for t,v in x['fields']:
            if t=='E':
                for tag,val in v:
                    w(tag)
                    if tag==1: align(); out.extend(struct.pack('<I',val))
                    elif tag in (2,3,4): w(val)
                w(0)
            elif t=='S': w(v)
            elif t=='L': align(); out.extend(struct.pack('<I',remap(v)))
            elif t=='T': w(len(v)); [w(ch) for ch in v]
    return bytes(out),amap
def read_cmp2(d):
    n=struct.unpack('<I',d[8:12])[0]
    ents=[struct.unpack('<II',d[16+i*8:24+i*8]) for i in range(n)]
    return [d[s*2048:(s+l)*2048] for s,l in ents], ents
def write_cmp2(orig,blocks):
    """reconstroi o CMP2 com os mesmos 16 bytes de cabecalho e tabela nova."""
    n=len(blocks)
    hdr=bytearray(orig[:16]); 
    tbl_secs=(16+8*n+2047)//2048
    first=struct.unpack('<II',orig[16:24])[0]
    pos=max(first,tbl_secs)
    tbl=bytearray(); body=bytearray()
    for b in blocks:
        ns=(len(b)+2047)//2048
        tbl+=struct.pack('<II',pos,ns)
        body+=b+b'\0'*(ns*2048-len(b)); pos+=ns
    head=hdr+tbl
    head+=orig[len(head):first*2048] if len(head)<first*2048 else b''
    head=head[:first*2048] if len(head)>=first*2048 else head+b'\0'*(first*2048-len(head))
    return bytes(head)+bytes(body)
