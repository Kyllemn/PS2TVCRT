"""Le um CDI de volta (confere o que o monta_cdi gerou): cabecalho, faixas, ISO, arquivos, EDC."""
import sys, os, struct, hashlib, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import cdsec, cdi
p = sys.argv[1]; lst = sys.argv[2] if len(sys.argv) > 2 else p + '.lbas.txt'
f = open(p, 'rb'); tam = os.path.getsize(p)
f.seek(tam - 8); ver, hl = struct.unpack('<II', f.read(8)); assert ver == 0x80000006, hex(ver)
f.seek(tam - hl); h = f.read(hl)
nsess, = struct.unpack('<H', h[0:2]); o = 4
faixas = []
for s in range(nsess):
    nt, = struct.unpack('<H', h[o:o + 2]); o += 2 + 4 if s == 0 else 2
    # cada faixa: 20 bytes de marcas, 4, nome, 31, bloco de 195
    o2 = h.find(cdi.MARK * 2, o - 6); o = o2 + 20 + 4; nl = h[o]; o += 1 + nl + 31
    b = h[o:o + 195]
    faixas.append(dict(pregap=struct.unpack('<I', b[2:6])[0], comp=struct.unpack('<I', b[6:10])[0], modo=b[0x10],
                       lba=struct.unpack('<I', b[0x20:0x24])[0], total=struct.unpack('<I', b[0x24:0x28])[0], setor=b[0x38]))
    o += 195
print('sessoes', nsess, faixas)
fa, fd = faixas
assert fa['modo'] == 0 and fa['setor'] == 2 and fa['comp'] == 302 and fa['pregap'] == 150
assert fd['modo'] == 2 and fd['setor'] == 1 and fd['lba'] == 11702 and fd['pregap'] == 150
tamanho_esperado = (150 + 302) * 2352 + (150 + fd['comp']) * 2336 + hl
print('tamanho do arquivo confere com as faixas:', tamanho_esperado == tam)
ini = (150 + 302) * 2352 + 150 * 2336
def setor(lba):
    f.seek(ini + (lba - 11702) * 2336); return f.read(2336)
def dados(lba, n):
    return b''.join(setor(lba + i)[8:2056] for i in range(n))
pvd = dados(11702 + 16, 1); assert pvd[1:6] == b'CD001'
print('IP.BIN ok:', setor(11702)[8:40] == b'SEGA SEGAKATANA SEGA ENTERPRISES', '| volume', pvd[40:72].strip())
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
print('arquivos lidos de volta', len(ref), '| diferentes', ruim, '| no diretorio', len(achados))
exe = cdi.descramble(dados(*[achados['1ST_READ.BIN'][0], (achados['1ST_READ.BIN'][1] + 2047) // 2048])[:achados['1ST_READ.BIN'][1]])
print('1ST_READ desembaralhado comeca com codigo SH4 valido:', exe[:4].hex(), '| LBA minimo:', struct.unpack('<I', exe[0x8e754:0x8e758])[0])
# EDC de setores aleatorios
random.seed(1); err = 0
for _ in range(2000):
    l = random.randrange(11702, 11702 + fd['comp'] - 2); s = setor(l)
    err += cdsec.edc(s[0:2056]).to_bytes(4, 'little') != s[2056:2060]
print('EDC em 2000 setores aleatorios: erros', err)
