"""Monta o CD da coletanea Final Fantasy: menu + Origins (FF1+FF2) + FF III (NES no PS1) + FF IV.

Ordem no disco: licenca (0-15), PVD (16), path tables (18-19), raiz (20), FF III (o leitor de CD
do emulador de NES dele so funciona com os arquivos perto do inicio), menu, FF IV (a tabela de
posicoes MSF no SLUS_013.60 e corrigida), Origins (com --ff2-lba: posicao do DATA.BIN e indice
dele corrigidos), subdiretorios. Os jogos sao copiados em bloco, setor bruto (so o endereco no
cabecalho muda; Mode 2 nao inclui o endereco no ECC).

uso: build_ffcol.py ffiv.bin origins.bin ff3.bin MENU.EXE MENU.DAT saida.bin [--ff2-lba]
  --ff2-lba : corrige no FF2/START.UP a constante com o LBA do FF2/DATA.BIN
"""
import sys, os, struct
import cdsector as cd

args = [a for a in sys.argv[1:] if not a.startswith('--')]
fiv, forg, fff3, menu_exe, menu_dat, out = args
PATCH_FF2 = '--ff2-lba' in sys.argv

IV, ORG, FF3 = cd.Bin(fiv), cd.Bin(forg), cd.Bin(fff3)
ivf, orf, f3f = IV.files(), ORG.files(), FF3.files()

sectors = {}          # lba -> setor bruto (2352)
entries = []          # (caminho, lba, tamanho, registro original ou None)


def put_file_data(lba, data, rec=None, path=None):
    n = max(1, (len(data) + 2047) // 2048)
    for i in range(n):
        sectors[lba + i] = cd.form1(lba + i, data[i * 2048:(i + 1) * 2048], 0x89 if i == n - 1 else 0x08)
    if path: entries.append((path, lba, len(data), rec))
    return lba + n


# ---- 1) FF III (emulador de NES) logo depois do diretorio, como na ISO original ----
# o leitor de CD proprio dele trava se precisar de um salto longo do fim para o inicio do disco
# licenca (setores 0-15) da ISO do FF III: japonesa, a mesma que ja funciona no console. Com a
# licenca americana do FF IV o drive responde diferente e o leitor de CD do imbNES trava.
for i in range(16): sectors[i] = (FF3, i)
sectors[17] = (IV, 17)
nxt = 21
f_files = [(p, l, s, r) for p, l, s, r in f3f if not p.endswith('/') and p != 'SYSTEM.CNF;1']
f_start = min(l for p, l, s, r in f_files); f_last = max(l + (s + 2047) // 2048 for p, l, s, r in f_files)
d3 = nxt - f_start
for l in range(f_start, f_last): sectors[l + d3] = (FF3, l)
for p, l, s, r in f_files: entries.append((p, l + d3, s, r))
nxt = f_last + d3
print('FF III: setores %d..%d' % (f_start + d3, nxt - 1))

# ---- 2) menu ----
cnf = b'BOOT = cdrom:\\MENU.EXE;1\r\nTCB = 4\r\nEVENT = 16\r\nSTACK = 801FFF00\r\n'
nxt = put_file_data(nxt, cnf, None, 'SYSTEM.CNF;1')
nxt = put_file_data(nxt, open(menu_exe, 'rb').read(), None, 'MENU.EXE;1')
nxt = put_file_data(nxt, open(menu_dat, 'rb').read(), None, 'MENU.DAT;1')

# ---- 3) FF IV em bloco; a tabela de posicoes (MSF) no SLUS_013.60 e corrigida ----
iv_files = [(p, l, s, r) for p, l, s, r in ivf if not p.endswith('/')]
iv_start = min(l for p, l, s, r in iv_files)
iv_last = max(l + (s + 2047) // 2048 for p, l, s, r in iv_files)
div = nxt - iv_start
for l in range(iv_start, iv_last): sectors[l + div] = (IV, l)
for p, l, s, r in ivf:
    if p.endswith('/'): entries.append((p, None, 0, r))
    elif p != 'SYSTEM.CNF;1': entries.append((p, l + div, s, r))
nxt = iv_last + div
print('FF IV: setores %d..%d (deslocamento %+d)' % (iv_start + div, nxt - 1, div))


def unbcd(b): return (b >> 4) * 10 + (b & 15)


slus = [(l, s) for p, l, s, r in ivf if p == 'SLUS_013.60;1'][0]
TAB = 0xa8e38                      # 26 entradas MSF de 4 bytes (uma por arquivo do disco)
lba_tab = slus[0] + TAB // 2048
assert (TAB % 2048) + 26 * 4 <= 2048
d = bytearray(IV.data(lba_tab)); o0 = TAB % 2048
orig_lbas = sorted(l for p, l, s, r in iv_files)
for k in range(26):
    m = d[o0 + 4 * k:o0 + 4 * k + 3]
    l = (unbcd(m[0]) * 60 + unbcd(m[1])) * 75 + unbcd(m[2]) - 150
    assert l in orig_lbas, (k, l)
    d[o0 + 4 * k:o0 + 4 * k + 3] = cd.msf(l + div)
L = lba_tab + div
sectors[L] = cd.form1(L, bytes(d), IV.raw(lba_tab)[18])
print('FF IV: 26 posicoes corrigidas no SLUS_013.60')

# ---- 4) Origins: bloco continuo ----
skip_org = {'SYSTEM.CNF;1', 'DATA3.BIN;1'}
o_files = [(p, l, s, r) for p, l, s, r in orf if not p.endswith('/') and p not in skip_org]
o_start = min(l for p, l, s, r in o_files)
o_last = max(l + (s + 2047) // 2048 for p, l, s, r in o_files)
delta = nxt - o_start
for l in range(o_start, o_last): sectors[l + delta] = (ORG, l)
for p, l, s, r in orf:
    if p.endswith('/'): entries.append((p, None, 0, r))
    elif p not in skip_org: entries.append((p, l + delta, s, r))
print('Origins: setores %d..%d (deslocamento %+d)' % (o_start + delta, o_last + delta - 1, delta))
nxt = o_last + delta
ff2_data = [l for p, l, s, r in orf if p == 'FF2/DATA.BIN;1'][0]
if PATCH_FF2:
    st = [(l, s) for p, l, s, r in orf if p == 'FF2/START.UP;1'][0]
    off = 0x84c70
    lba = st[0] + off // 2048 + delta
    raw = cd.relocate(ORG.raw(lba - delta), lba)
    a, tbl = struct.unpack_from('<II', raw, 24 + off % 2048)      # {LBA do DATA.BIN, bytes do indice}
    assert a == ff2_data
    sectors[lba] = cd.patch_form1(raw, lba, off % 2048, struct.pack('<I', a + delta))
    # indice no inicio do DATA.BIN: a partir de 0x44, pares {LBA absoluto, tamanho} ate 'tbl'
    idx = bytearray(b''.join(ORG.data(ff2_data + i) for i in range((tbl + 2047) // 2048)))
    n = 0
    for o in range(0x44, tbl, 8):
        l = struct.unpack_from('<I', idx, o)[0]
        assert ff2_data <= l < ff2_data + 40000, (hex(o), l)
        struct.pack_into('<I', idx, o, l + delta); n += 1
    for i in range((tbl + 2047) // 2048):
        L = ff2_data + i + delta
        sectors[L] = cd.form1(L, bytes(idx[i * 2048:(i + 1) * 2048]), ORG.raw(ff2_data + i)[18])
    print('FF2: DATA.BIN %d -> %d, %d entradas do indice corrigidas' % (a, a + delta, n))

# ---- 5) ISO9660 no fim ----
def both32(v): return struct.pack('<I', v) + struct.pack('>I', v)
def both16(v): return struct.pack('<H', v) + struct.pack('>H', v)
XA_FILE = b'\x00\x00\x00\x00\x0d\x55\x58\x41\x00\x00\x00\x00\x00\x00'   # atributo XA padrao (form1)
XA_DIR = b'\x00\x00\x00\x00\x8d\x55\x58\x41\x00\x00\x00\x00\x00\x00'
DATE = bytes([126, 11, 4, 12, 0, 0, 36])


def record(name, lba, size, flags, xa, date=DATE):
    nb = name if isinstance(name, bytes) else name.encode()
    body = both32(lba) + both32(size) + date + bytes([flags, 0, 0]) + both16(1) + bytes([len(nb)]) + nb
    if len(nb) % 2 == 0: body += b'\0'
    body += xa
    return bytes([len(body) + 2, 0]) + body


def xa_of(rec):
    """area de sistema (XA) do registro original"""
    nl = rec[32]; o = 33 + nl + (0 if nl % 2 else 1)
    return rec[o:]


# arvore: dir -> lista de (nome, caminho completo)
dirs = {'': []}
for p, l, s, r in entries:
    if p.endswith('/'):
        dirs.setdefault(p, [])
        parent = p[:-1].rsplit('/', 1)[0] + '/' if '/' in p[:-1] else ''
        if p[:-1].split('/')[-1] not in [n for n, _ in dirs.setdefault(parent, [])]:
            dirs[parent].append((p[:-1].split('/')[-1], p))
    else:
        parent = p.rsplit('/', 1)[0] + '/' if '/' in p else ''
        dirs.setdefault(parent, []).append((p.split('/')[-1], p))
info = {p: (l, s, r) for p, l, s, r in entries}

order = sorted(dirs, key=lambda d: (d.count('/'), d))      # raiz primeiro, por nivel


def dir_size(d):
    recs = [record(b'\0', 0, 0, 2, XA_DIR), record(b'\1', 0, 0, 2, XA_DIR)]
    for n, p in sorted(dirs[d]):
        recs.append(record(n, 0, 0, 0, XA_FILE if not p.endswith('/') else XA_DIR))
    sec = 1; used = 0
    for r in recs:
        if used + len(r) > 2048: sec += 1; used = 0
        used += len(r)
    return sec


# path tables e diretorio raiz nos setores 18-20 (livres): o leitor de CD do emulador de NES do
# FF III so usa os 16 bits baixos do LBA do raiz. Os subdiretorios vao para o fim.
pt_l, pt_m = 18, 19
dlba = {'': 20}
assert dir_size('') == 1
for d in order:
    if d: dlba[d] = nxt; nxt += dir_size(d)
num = {d: i + 1 for i, d in enumerate(order)}


def dir_records(d):
    parent = (d[:-1].rsplit('/', 1)[0] + '/' if '/' in d[:-1] else '') if d else ''
    me = (dlba[d], dir_size(d) * 2048); pa = (dlba[parent], dir_size(parent) * 2048)
    recs = [record(b'\0', me[0], me[1], 2, XA_DIR), record(b'\1', pa[0], pa[1], 2, XA_DIR)]
    for n, p in sorted(dirs[d]):
        if p.endswith('/'):
            recs.append(record(n, dlba[p], dir_size(p) * 2048, 2, XA_DIR))
        else:
            l, s, r = info[p]
            xa = xa_of(r) if r else XA_FILE
            recs.append(record(n, l, s, 0, xa, r[18:25] if r else DATE))
    secs = []; cur = b''
    for r in recs:
        if len(cur) + len(r) > 2048: secs.append(cur); cur = b''
        cur += r
    secs.append(cur)
    return secs


for d in order:
    secs = dir_records(d)
    for i, s in enumerate(secs):
        sectors[dlba[d] + i] = cd.form1(dlba[d] + i, s, 0x89 if i == len(secs) - 1 else 0x08)

ptl = b''; ptm = b''
for d in order:
    name = d[:-1].split('/')[-1].encode() if d else b'\0'
    parent = (d[:-1].rsplit('/', 1)[0] + '/' if '/' in d[:-1] else '') if d else ''
    pad = b'\0' if len(name) % 2 else b''
    ptl += bytes([len(name), 0]) + struct.pack('<IH', dlba[d], num[parent]) + name + pad
    ptm += bytes([len(name), 0]) + struct.pack('>IH', dlba[d], num[parent]) + name + pad
assert len(ptl) <= 2048
sectors[pt_l] = cd.form1(pt_l, ptl, 0x89); sectors[pt_m] = cd.form1(pt_m, ptm, 0x89)

total = nxt + 150
pvd = bytearray(IV.data(16))
pvd[40:72] = b'FF_COLLECTION'.ljust(32)
pvd[80:88] = both32(total)
pvd[132:140] = both32(len(ptl))
pvd[140:144] = struct.pack('<I', pt_l); pvd[144:148] = b'\0' * 4
pvd[148:152] = struct.pack('>I', pt_m); pvd[152:156] = b'\0' * 4
root = record(b'\0', dlba[''], dir_size('') * 2048, 2, b'')
pvd[156:190] = root[:34]
sectors[16] = cd.form1(16, bytes(pvd), 0x89)

with open(out, 'wb') as f:
    empty = None
    for l in range(total):
        s = sectors.get(l)
        if s is None: s = empty or cd.form1(l, b'', 0x08)
        elif isinstance(s, tuple): s = s[0].raw(s[1]); s = s if s[12:15] == cd.msf(l) else cd.relocate(s, l)
        f.write(s)
open(os.path.splitext(out)[0] + '.cue', 'w').write('FILE "%s" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % os.path.basename(out))
print('total: %d setores = %.1f MB (CD de 700 MB = 359847 setores)' % (total, total * 2352 / 1e6))
