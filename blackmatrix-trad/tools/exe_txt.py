"""Lista os textos com prefixo de tamanho (u16 n + n caracteres u16) do 1ST_READ."""
import struct, sys
d=open(sys.argv[1],'rb').read()
a=int(sys.argv[2],16); b=int(sys.argv[3],16)
def ch(v):
    if v==0xa0ff: return '/'
    if not (0x81<=(v>>8)<=0x9f or 0xe0<=(v>>8)<=0xea): return None
    try: return bytes([v>>8,v&0xff]).decode('shift_jis')
    except: return None
i=a
while i<b:
    n=struct.unpack('<H',d[i:i+2])[0]
    if 1<=n<=80 and i+2+2*n<=len(d):
        t=[ch(struct.unpack('<H',d[i+2+2*k:i+4+2*k])[0]) for k in range(n)]
        if all(x for x in t) and (n>1 or t[0]!='　'):
            print('%06x\t%d\t%s'%(i,n,''.join(t))); i+=2+2*n; continue
    i+=2
