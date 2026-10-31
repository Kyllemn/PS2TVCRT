"""Leitor generico de CDI (logica do cdirip): versao, sessoes, faixas."""
import sys, struct, os
MARK=bytes([0,0,1,0,0,0,0xff,0xff,0xff,0xff])
def info(p):
    f=open(p,'rb'); tam=os.path.getsize(p)
    f.seek(tam-8); ver,hl=struct.unpack('<II',f.read(8))
    pos = tam-hl if ver==0x80000006 else hl
    f.seek(pos); h=f.read(tam-pos); o=0
    def rd(n):
        nonlocal o; b=h[o:o+n]; o+=n; return b
    def u32(): return struct.unpack('<I',rd(4))[0]
    def u16(): return struct.unpack('<H',rd(2))[0]
    ns=u16(); faixas=[]; posicao=0
    for s in range(ns):
        nt=u16()
        for t in range(nt):
            tv=u32()
            if tv!=0: o+=8
            m1=rd(10); m2=rd(10); assert m1==MARK and m2==MARK,(s,t,hex(o))
            o+=4; nl=h[o]; o+=1; nome=rd(nl); o+=11+4+4
            tv=u32()
            if tv==0x80000000: o+=8
            o+=2; pregap=u32(); comp=u32(); o+=6; modo=u32(); o+=12; lba=u32(); total=u32(); o+=16; ss=u32()
            tam_s={0:2048,1:2336,2:2352}[ss]
            o+=29
            if ver!=0x80000004:
                o+=5; tv=u32()
                if tv==0xffffffff: o+=78
            faixas.append(dict(sessao=s+1,faixa=t+1,pregap=pregap,comp=comp,modo=modo,lba=lba,total=total,setor=tam_s,arquivo_pos=posicao))
            posicao+=(pregap+comp)*tam_s
        o+=12
        if ver!=0x80000004: o+=1
    return ver,hl,faixas,tam,posicao
if __name__=='__main__':
    for p in sys.argv[1:]:
        ver,hl,fx,tam,fim=info(p)
        print(os.path.basename(p),'versao',hex(ver),'cabecalho',hl,'bytes | tamanho',tam,'| dados de faixas ate',fim)
        for x in fx: print('  ',x)
