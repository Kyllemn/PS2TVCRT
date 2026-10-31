"""Disco 2: cada bloco CMP2 do arquivo do disco 2 que e identico a um bloco do original do disco 1
recebe o bloco correspondente ja traduzido do disco 1 (mesmo tamanho).
uso: transplanta_blocos.py orig_d1 trad_d1 orig_d2 saida"""
import sys, struct, hashlib
o1, t1, o2, out = sys.argv[1:5]
def blocos(d):
    n = struct.unpack('<I', d[8:12])[0]
    return [(struct.unpack('<I', d[16 + 8 * i:20 + 8 * i])[0] * 2048, struct.unpack('<I', d[20 + 8 * i:24 + 8 * i])[0] * 2048) for i in range(n)]
A = open(o1, 'rb').read(); T = open(t1, 'rb').read(); B = bytearray(open(o2, 'rb').read())
ba, bt, bb = blocos(A), blocos(T), blocos(B)
assert ba == bt
h1 = {}
for i, (o, l) in enumerate(ba): h1.setdefault(hashlib.md5(A[o:o + l]).hexdigest(), i)
n = 0; trad = 0
for j, (o, l) in enumerate(bb):
    i = h1.get(hashlib.md5(B[o:o + l]).hexdigest())
    if i is None: continue
    oa, la = ba[i]; assert la == l
    novo = T[oa:oa + la]
    if novo != A[oa:oa + la]: trad += 1
    B[o:o + l] = novo; n += 1
open(out, 'wb').write(bytes(B)); print('blocos iguais ao d1:', n, '| desses, com traducao:', trad)
