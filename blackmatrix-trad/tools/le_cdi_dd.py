"""Le de volta um CDI dados/dados: boot pela sessao 2 (como a BIOS), arquivos na sessao 1, md5, EDC, 1ST_READ."""
import sys, os, struct, hashlib, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import cdsec, cdi
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')); from cdi_info import info
p = sys.argv[1]; lst = sys.argv[2] if len(sys.argv) > 2 else p + '.lbas.txt'
ver, hl, fx, tam, fim = info(p); f = open(p, 'rb')
assert len(fx) == 2 and all(x['setor'] == 2336 and x['modo'] == 2 for x in fx) and fx[0]['lba'] == 0
print('tamanho confere:', fim + hl == tam)
def setor(t, lba):
    x = fx[t]; f.seek(x['arquivo_pos'] + (x['pregap'] + lba - x['lba']) * 2336); return f.read(2336)
def dados(lba, n, t=0): return b''.join(setor(t, lba + i)[8:2056] for i in range(n))
# como a BIOS: IP.BIN e PVD da ultima sessao
ip = setor(1, fx[1]['lba'])[8:2056]; pvd = setor(1, fx[1]['lba'] + 16)[8:2056]
print('sessao 2: IP.BIN', ip[:15], '| PVD', pvd[1:6], '| igual ao da sessao 1:', pvd == dados(16, 1))
rl, = struct.unpack('<I', pvd[158:162]); rs, = struct.unpack('<I', pvd[166:170])
raiz = dados(rl, (rs + 2047) // 2048); o = 0; achados = {}
while o < len(raiz):
    n = raiz[o]
    if n == 0: o = (o // 2048 + 1) * 2048; continue
    el, = struct.unpack('<I', raiz[o + 2:o + 6]); sz, = struct.unpack('<I', raiz[o + 10:o + 14]); nm = raiz[o + 33:o + 33 + raiz[o + 32]]
    if nm not in (b'\0', b'\1'): achados[nm.decode().split(';')[0]] = (el, sz)
    o += n
ref = {l.split('\t')[0]: (int(l.split('\t')[1]), l.split('\t')[2].strip()) for l in open(lst)}
ruim = 0
for nm, (el, md5) in ref.items():
    e, sz = achados[nm]; d = dados(e, (sz + 2047) // 2048)[:sz]
    if e != el or hashlib.md5(d).hexdigest() != md5: print('DIFERENTE', nm); ruim += 1
print('arquivos', len(ref), '| diferentes', ruim)
e, sz = achados['1ST_READ.BIN']; exe = cdi.descramble(dados(e, (sz + 2047) // 2048)[:sz])
print('1ST_READ: inicio', exe[:4].hex(), '| FAD minimo', struct.unpack('<I', exe[0x8e754:0x8e758])[0], struct.unpack('<I', exe[0x8eb20:0x8eb24])[0], '| FAD do CD001', struct.unpack('<I', exe[0xd2844:0xd2848])[0])
print('setor no FAD 166 (o gdFs le aqui):', dados(16, 1)[:6])
random.seed(2); err = 0
for _ in range(2000):
    t = random.choice([0, 0, 0, 1]); x = fx[t]; l = x['lba'] + random.randrange(0, 18 if t else x['comp'] - 2); s = setor(t, l)
    err += cdsec.edc(s[0:2056]).to_bytes(4, 'little') != s[2056:2060]
print('EDC 2000 setores: erros', err)
