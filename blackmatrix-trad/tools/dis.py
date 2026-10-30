import sys, struct, capstone
d=open(sys.argv[1],'rb').read(); BASE=0x8c010000
md=capstone.Cs(capstone.CS_ARCH_SH, capstone.CS_MODE_SH4|capstone.CS_MODE_LITTLE_ENDIAN)
def dis(addr,n):
    off=addr-BASE
    for ins in md.disasm(d[off:off+n*2],addr):
        extra=''
        if ins.mnemonic.startswith('mov.l') and 'pc' in ins.op_str:
            # mov.l @(disp,pc),rn
            try:
                disp=int(ins.op_str.split('(')[1].split(',')[0],0)
                lit=((ins.address+4)&~3)+disp
                extra=' ; =%08x'%struct.unpack('<I',d[lit-BASE:lit-BASE+4])[0]
            except Exception: pass
        print('%08x: %-8s %s%s'%(ins.address,ins.mnemonic,ins.op_str,extra))
if __name__=='__main__':
    dis(int(sys.argv[2],16),int(sys.argv[3]))
