"""Monta o CDI de boot automatico (CD-R) a partir do track03 do GD ja traduzido.
- arquivos na mesma ordem do GD; SFD trocados pelos reencodados (pasta_sfd)
- layout dados/dados (igual ao Volgarr, cdi4dc -d): ISO na sessao 1 a partir do LBA 0, sessao 2 so com o boot
- 1ST_READ (gdFs): FAD 45150 (inicio do GD) -> 150 e FAD 45166 (CD001) -> 166; depois o arquivo e embaralhado
- IP.BIN = 16 primeiros setores do track03, com a TOC1 (0x104) apontando para a faixa de dados do CD
- IP.BIN sem trava de regiao (JUE), como o MSR e o Volgarr
- logo opcional na tela da SEGA: imagem MR em IP.BIN+0x3820 (ate 8192 bytes, como o makeip do KallistiOS)
uso: monta_cdi.py track03.bin pasta_sfd saida.cdi [logo.mr]"""
import sys, os, struct, hashlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import cdi
BASE_ISO = 0
track, pasta_sfd, saida = sys.argv[1:4]; logo = sys.argv[4] if len(sys.argv) > 4 else None
BASE = 45000
f = open(track, 'rb')
def sec(l, n=1):
    out = bytearray()
    for i in range(n):
        f.seek((l - BASE + i) * 2352 + 16); out += f.read(2048)
    return bytes(out)
ipbin = sec(BASE, 16); pvd = sec(BASE + 16)
# TOC1 do IP.BIN (tabela de faixas do GD: dados em FAD 45150) -> faixa de dados do CD em FAD BASE_ISO+150
assert ipbin[0x100:0x108] == b'TOC1' + struct.pack('<I', 0x41000000 | 45150)
ipbin = ipbin[:0x104] + struct.pack('<I', 0x41000000 | (BASE_ISO + 150)) + ipbin[0x108:]
# sem trava de regiao (como todo CD-R de boot automatico): simbolos de area JUE + textos das 3 regioes
assert ipbin[0x30:0x38] == b'J       '
ipbin = ipbin[:0x30] + b'JUE     ' + ipbin[0x38:]
reg = bytearray(ipbin[0x3700:0x3800])
for k, txt in enumerate((b'For JAPAN,TAIWAN,PHILIPINES.', b'For USA and CANADA.', b'For EUROPE.')):
    o = k * 32; assert reg[o:o + 4] == b'\x0e\xa0\x09\x00'
    reg[o + 4:o + 32] = txt.ljust(28)
ipbin = ipbin[:0x3700] + bytes(reg) + ipbin[0x3800:]
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
            d[off:off + 4] = struct.pack('<I', BASE_ISO + 150)
        off = 0xd2844                                          # gdFs: FAD do descritor de volume (CD001)
        assert struct.unpack('<I', d[off:off + 4])[0] == 45166 and d[off + 8:off + 13] == b'CD001'
        d[off:off + 4] = struct.pack('<I', BASE_ISO + 150 + 16)
        # nao pode sobrar nenhuma constante do GD
        assert all(struct.unpack('<I', d[i:i + 4])[0] not in (45150, 45166) for i in range(0, len(d) - 3, 2))
        exe = bytes(d); d = cdi.scramble(exe)
        assert cdi.descramble(d) == exe
    lista.append((nm, d))
print('arquivos', len(lista), '| SFD trocados', trocados)
iso, lbas = cdi.monta_iso(ipbin, pvd, lista, data_dir, BASE_ISO)
mb = len(iso) / 2 ** 20
fim = len(iso) // 2048 + 2 + 11400 + 302
print('ISO %.1f MiB (%d setores), disco termina no LBA %d' % (mb, len(iso) // 2048, fim))
assert fim < 359849, 'nao cabe num CD-R de 80 min'
def prog(a, b):
    if a == b or a % (4096 * 32) == 0: print('  setores %d/%d' % (a, b), flush=True)
blocos, lba2 = cdi.grava_cdi_dd(iso, saida, progresso=prog)
print('CDI gravado:', saida, '| faixa 1:', blocos, 'setores | faixa de boot no LBA', lba2, '| %.1f MiB' % (os.path.getsize(saida) / 2 ** 20))
open(saida + '.lbas.txt', 'w').write(''.join('%s\t%d\t%s\n' % (nm, lbas[nm], hashlib.md5(d).hexdigest()) for nm, d in lista))
