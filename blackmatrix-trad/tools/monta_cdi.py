"""Monta o CDI de boot automatico (CD-R) a partir do track03 do GD ja traduzido.
- arquivos na mesma ordem do GD; SFD trocados pelos reencodados (pasta_sfd)
- 1ST_READ (gdFs): FAD 45150 (inicio do GD) -> 11852 e FAD 45166 (CD001) -> 11868; depois o arquivo e embaralhado
- IP.BIN = 16 primeiros setores do track03
- logo opcional na tela da SEGA: imagem MR em IP.BIN+0x3820 (ate 8192 bytes, como o makeip do KallistiOS)
uso: monta_cdi.py track03.bin pasta_sfd saida.cdi [logo.mr]"""
import sys, os, struct, hashlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import cdi
track, pasta_sfd, saida = sys.argv[1:4]; logo = sys.argv[4] if len(sys.argv) > 4 else None
BASE = 45000
f = open(track, 'rb')
def sec(l, n=1):
    out = bytearray()
    for i in range(n):
        f.seek((l - BASE + i) * 2352 + 16); out += f.read(2048)
    return bytes(out)
ipbin = sec(BASE, 16); pvd = sec(BASE + 16)
if logo:
    mr = open(logo, 'rb').read()
    assert mr[:2] == b'MR' and len(mr) <= 8192 and struct.unpack('<I', mr[2:6])[0] == len(mr)
    assert ipbin[0x3820:0x5820] == b'\0' * 8192, 'area do logo no IP.BIN nao esta vazia'
    ipbin = ipbin[:0x3820] + mr + ipbin[0x3820 + len(mr):]
    print('logo MR', len(mr), 'bytes em IP.BIN+0x3820')
rl, = struct.unpack('<I', pvd[158:162]); rs, = struct.unpack('<I', pvd[166:170])
raiz = sec(rl, (rs + 2047) // 2048)
arqs = []; o = 0; data_dir = pvd[156 + 18:156 + 25]
while o < len(raiz):
    n = raiz[o]
    if n == 0: o = (o // 2048 + 1) * 2048; continue
    el, = struct.unpack('<I', raiz[o + 2:o + 6]); sz, = struct.unpack('<I', raiz[o + 10:o + 14])
    nl = raiz[o + 32]; nm = raiz[o + 33:o + 33 + nl]
    if nm not in (b'\0', b'\1'): arqs.append((el, nm.decode().split(';')[0], sz))
    o += n
arqs.sort()
lista = []; trocados = 0
for el, nm, sz in arqs:
    d = sec(el, (sz + 2047) // 2048)[:sz]
    if nm.endswith('.SFD'):
        d = open(os.path.join(pasta_sfd, nm), 'rb').read(); trocados += 1
    if nm == '1ST_READ.BIN':
        d = bytearray(d)
        for off in (0x8e754, 0x8eb20):                         # gdFs: FAD minimo (inicio da area de dados)
            assert struct.unpack('<I', d[off:off + 4])[0] == 45150, hex(off)
            d[off:off + 4] = struct.pack('<I', cdi.MSINFO + 150)
        off = 0xd2844                                          # gdFs: FAD do descritor de volume (CD001)
        assert struct.unpack('<I', d[off:off + 4])[0] == 45166 and d[off + 8:off + 13] == b'CD001'
        d[off:off + 4] = struct.pack('<I', cdi.MSINFO + 150 + 16)
        # nao pode sobrar nenhuma constante do GD
        assert all(struct.unpack('<I', d[i:i + 4])[0] not in (45150, 45166) for i in range(0, len(d) - 3, 2))
        exe = bytes(d); d = cdi.scramble(exe)
        assert cdi.descramble(d) == exe
    lista.append((nm, d))
print('arquivos', len(lista), '| SFD trocados', trocados)
iso, lbas = cdi.monta_iso(ipbin, pvd, lista, data_dir)
mb = len(iso) / 2 ** 20
print('ISO %.1f MiB (%d setores), termina no LBA %d' % (mb, len(iso) // 2048, cdi.MSINFO + len(iso) // 2048))
assert cdi.MSINFO + len(iso) // 2048 + 2 < 359849, 'nao cabe num CD-R de 80 min'
def prog(a, b):
    if a == b or a % (4096 * 32) == 0: print('  setores %d/%d' % (a, b), flush=True)
blocos = cdi.grava_cdi(iso, saida, progresso=prog)
print('CDI gravado:', saida, '| setores da faixa de dados', blocos, '| %.1f MiB' % (os.path.getsize(saida) / 2 ** 20))
open(saida + '.lbas.txt', 'w').write(''.join('%s\t%d\t%s\n' % (nm, lbas[nm], hashlib.md5(d).hexdigest()) for nm, d in lista))
