"""Descricao do save no VMU (1ST_READ 0x86afe, 34 bytes SJIS).
O jogo grava o capitulo nos bytes 12-13 e copiava o nome (celulas da fonte
do jogo, que viram lixo no menu do Dreamcast) nos bytes 18-29: o laco do nome
e desligado trocando 'mov #12,r7' por 'mov #0,r7' em 0x1f58."""
import sys
src,out=sys.argv[1:3]; d=bytearray(open(src,'rb').read())
O=0x86afe
import hashlib
assert hashlib.md5(d[O:O+10]).hexdigest()=='0d41a6296ab265e7b209a12c8607dbc0'
t=b'B.Matrix Cap'+'　'.encode('shift_jis')+b'  PT-BR'.ljust(16)+b'  '
assert len(t)==32; d[O:O+32]=t
assert d[0x1f58:0x1f5a]==b'\x0c\xe7'; d[0x1f58]=0
open(out,'wb').write(bytes(d)); print('vmu ok')
