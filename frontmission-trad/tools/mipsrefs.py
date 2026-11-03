import struct
def lui_addiu_refs(x, base_vaddr_check=None):
    """procura enderecos montados por lui/addiu (ou lui/ori) a ate 12 instrucoes de distancia.
    devolve lista de (pos_lui, pos_lo, endereco, reg)"""
    W=[struct.unpack('<I',x[o:o+4])[0] for o in range(0,len(x)//4*4,4)]
    out=[]
    for i,w in enumerate(W):
        if (w>>26)==0x0f:   # lui
            rt=(w>>16)&31; hi=w&0xffff
            for j in range(i+1,min(len(W),i+12)):
                v=W[j]; op=v>>26
                if op in (0x09,0x0d) and ((v>>21)&31)==rt:
                    imm=v&0xffff
                    if op==0x09 and imm&0x8000: imm-=0x10000
                    out.append((i*4,j*4,((hi<<16)+imm)&0xffffffff,rt))
                    break
                if ((v>>16)&31)==rt and op not in (0x2b,0x29,0x28,0x04,0x05): break
    return out
