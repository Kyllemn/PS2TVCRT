"""Todas as referencias a strings do arquivo 22 (0x80191000): u32 em tabelas, 'c0 19' em scripts, lui/addiu no codigo."""
import os,re,struct,sys; sys.path.insert(0,'/root/fm1')
from mipsrefs import lui_addiu_refs
B=0x80191000; END=0x7918
def is_table(x,o):
    for dlt in (-4,4):
        p=o+dlt
        if 0<=p<=len(x)-4 and 0x80010000<=struct.unpack('<I',x[p:p+4])[0]<0x80200000: return True
    return False
def collect(files):
    """files: nome -> bytes (descomprimido; exe sem cabecalho). devolve lista (arq, tipo, pos, alvo, extra)"""
    R=[]
    for nm,x in files.items():
        for o in range(0,len(x)-3,4):
            v=struct.unpack('<I',x[o:o+4])[0]
            if B<=v<B+END and is_table(x,o): R.append((nm,'u32',o,v-B,None))
        for m in re.finditer(rb'\xc0\x19',x):
            t=struct.unpack('<H',x[m.start()+2:m.start()+4])[0]-0x1000
            if 0<=t<END: R.append((nm,'c019',m.start(),t,None))
        if nm in CODE:
            for pl,plo,a,rt in lui_addiu_refs(x):
                if B<=a<B+END: R.append((nm,'lui',pl,a-B,plo))
    return R
CODE={'00','02','05','34','36','40','44','45','EXE'}
def patch(x,kind,pos,newoff,extra):
    x=bytearray(x); a=B+newoff
    if kind=='u32': struct.pack_into('<I',x,pos,a)
    elif kind=='c019': struct.pack_into('<H',x,pos+2,(a&0xffff))
    elif kind=='lui':
        hi=(a+0x8000)>>16; lo=a&0xffff
        wl=struct.unpack('<I',x[pos:pos+4])[0]; wo=struct.unpack('<I',x[extra:extra+4])[0]
        op=wo>>26
        if op==0x0d: hi=a>>16   # ori: sem extensao de sinal
        struct.pack_into('<I',x,pos,(wl&0xffff0000)|hi)
        struct.pack_into('<I',x,extra,(wo&0xffff0000)|lo)
    return bytes(x)
