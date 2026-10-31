"""Gera um CDI (DiscJuggler) audio/dados com boot automatico para Dreamcast (CD-R, MIL-CD).
Layout igual ao do cdi4dc (img4dc, SiZiOUS, GPL), reescrito em Python:
sessao 1 = faixa de audio de 302 setores (silencio); sessao 2 = faixa de dados Modo 2 Forma 1
no LBA 11702, setores de 2336 bytes (subcabecalho + dados + EDC/ECC).
Tambem tem o embaralhador do 1ST_READ.BIN (mesmo algoritmo do scramble.c do KallistiOS)
e o montador de ISO9660 com base no LBA 11702."""
import struct, numpy as np

MSINFO = 11702

# ---------- embaralhador do 1ST_READ (boot por CD) ----------
def scramble(data):
    out = bytearray(); pos = 0; seed = len(data) & 0xffff
    def rnd():
        nonlocal seed
        seed = (seed * 2109 + 9273) & 0x7fff
        return (seed + 0xc000) & 0xffff
    rest = len(data); chunk = 2048 * 1024
    while chunk >= 32:
        while rest >= chunk:
            sz = chunk // 32; idx = list(range(sz))
            for i in range(sz - 1, -1, -1):
                x = (rnd() * i) >> 16
                idx[i], idx[x] = idx[x], idx[i]
                out += data[pos + 32 * idx[i]: pos + 32 * idx[i] + 32]
            rest -= chunk; pos += chunk
        chunk >>= 1
    out += data[pos:]
    return bytes(out)

def descramble(data):
    out = bytearray(len(data)); pos = 0; rp = 0; seed = len(data) & 0xffff
    def rnd():
        nonlocal seed
        seed = (seed * 2109 + 9273) & 0x7fff
        return (seed + 0xc000) & 0xffff
    rest = len(data); chunk = 2048 * 1024
    while chunk >= 32:
        while rest >= chunk:
            sz = chunk // 32; idx = list(range(sz))
            for i in range(sz - 1, -1, -1):
                x = (rnd() * i) >> 16
                idx[i], idx[x] = idx[x], idx[i]
                out[pos + 32 * idx[i]: pos + 32 * idx[i] + 32] = data[rp:rp + 32]; rp += 32
            rest -= chunk; pos += chunk
        chunk >>= 1
    out[pos:] = data[rp:]
    return bytes(out)

# ---------- ISO9660 ----------
def both32(v): return struct.pack('<I', v) + struct.pack('>I', v)
def both16(v): return struct.pack('<H', v) + struct.pack('>H', v)
def dirrec(name, lba, size, date, flags):
    n = len(name); r = struct.pack('<BB', 0, 0) + both32(lba) + both32(size) + date + bytes([flags, 0, 0]) + both16(1) + bytes([n]) + name
    if len(r) % 2: r += b'\0'
    return bytes([len(r)]) + r[1:]

def monta_iso(ipbin, pvd_orig, arquivos, data_dir, MSINFO=MSINFO):
    """arquivos: lista ordenada (nome, bytes) na ordem do disco. Devolve a imagem (setores de 2048)."""
    assert len(ipbin) == 32768
    nomes = sorted(arquivos, key=lambda a: a[0])
    def empacota(recs):
        out = bytearray(); cur = bytearray()
        for r in recs:
            if len(cur) + len(r) > 2048: out += cur + b'\0' * (2048 - len(cur)); cur = bytearray()
            cur += r
        return bytes(out + cur + b'\0' * (2048 - len(cur)))
    def registros(root, nsec, pos, MSINFO=MSINFO):
        r = [dirrec(b'\0', MSINFO + root, nsec * 2048, data_dir, 2), dirrec(b'\1', MSINFO + root, nsec * 2048, data_dir, 2)]
        return r + [dirrec(nm.encode() + b';1', MSINFO + pos.get(nm, 0), len(d), data_dir, 0) for nm, d in nomes]
    ROOT = 20
    nsec_dir = len(empacota(registros(ROOT, 1, {}))) // 2048
    prox = ROOT + nsec_dir; pos = {}
    for nm, d in arquivos:
        pos[nm] = prox; prox += (len(d) + 2047) // 2048
    total = prox
    def bloco_dir():
        return empacota(registros(ROOT, nsec_dir, pos))
    img = bytearray(total * 2048)
    img[0:32768] = ipbin
    pvd = bytearray(pvd_orig)
    pvd[80:88] = both32(MSINFO + total)
    pvd[132:140] = both32(10)                                      # tamanho da tabela de caminhos
    pvd[140:144] = struct.pack('<I', MSINFO + 18); pvd[144:148] = b'\0' * 4
    pvd[148:152] = struct.pack('>I', MSINFO + 19); pvd[152:156] = b'\0' * 4
    pvd[156:190] = dirrec(b'\0', MSINFO + ROOT, nsec_dir * 2048, data_dir, 2)
    img[16 * 2048:17 * 2048] = pvd
    img[17 * 2048:17 * 2048 + 7] = b'\xffCD001\x01'
    img[18 * 2048:18 * 2048 + 10] = bytes([1, 0]) + struct.pack('<I', MSINFO + ROOT) + struct.pack('<H', 1) + b'\0\0'
    img[19 * 2048:19 * 2048 + 10] = bytes([1, 0]) + struct.pack('>I', MSINFO + ROOT) + struct.pack('>H', 1) + b'\0\0'
    img[ROOT * 2048:(ROOT + nsec_dir) * 2048] = bloco_dir()
    for nm, d in arquivos:
        img[pos[nm] * 2048:pos[nm] * 2048 + len(d)] = d
    return bytes(img), {nm: MSINFO + p for nm, p in pos.items()}

# ---------- EDC/ECC vetorizados (Modo 2 Forma 1) ----------
_F = np.zeros(256, np.uint8); _B = np.zeros(256, np.uint8); _E = np.zeros(256, np.uint32)
for i in range(256):
    j = ((i << 1) ^ (0x11D if i & 0x80 else 0)) & 0xff
    _F[i] = j; _B[i ^ j] = i
    e = i
    for _ in range(8): e = (e >> 1) ^ (0xD8018001 if e & 1 else 0)
    _E[i] = e
def _idx(major_count, minor_count, major_mult, minor_inc):
    size = major_count * minor_count; t = np.zeros((major_count, minor_count), np.int64)
    for major in range(major_count):
        index = (major >> 1) * major_mult + (major & 1)
        for m in range(minor_count):
            t[major, m] = index; index += minor_inc
            if index >= size: index -= size
    return t
_P = _idx(86, 24, 2, 86); _Q = _idx(52, 43, 86, 88)
def _ecc(S, tab, dest):
    v = S[:, 0xC + tab]                         # (N, majors, minors)
    a = np.zeros(v.shape[:2], np.uint8); b = np.zeros_like(a)
    for m in range(v.shape[2]):
        t = v[:, :, m]; a ^= t; b ^= t; a = _F[a]
    a = _B[_F[a] ^ b]; n = tab.shape[0]
    S[:, dest:dest + n] = a; S[:, dest + n:dest + 2 * n] = a ^ b
def setores_m2f1(dados):
    """dados: array (N, 2048) -> array (N, 2336) = subcab(8 zeros) + dados + EDC + ECC P/Q."""
    N = dados.shape[0]; S = np.zeros((N, 2352), np.uint8)
    S[:, 24:2072] = dados
    e = np.zeros(N, np.uint32)
    for j in range(16, 2072):
        e = (e >> 8) ^ _E[(e ^ S[:, j]) & 0xff]
    S[:, 2072:2076] = e.view(np.uint8).reshape(N, 4)
    _ecc(S, _P, 0x81C); _ecc(S, _Q, 0x81C + 0xAC)
    return S[:, 16:2352]

# ---------- CDI ----------
def _fill(size, pares):
    b = bytearray(size)
    for o, v in pares: b[o] = v
    return b
GAP1 = _fill(2336, [(0x002, 0x20), (0x006, 0x20), (0x91c, 0x3f), (0x91d, 0x13), (0x91e, 0xb0), (0x91f, 0xbe)])
GAP2 = _fill(2336, [(0x00008, 0x54), (0x00009, 0x44), (0x0000a, 0x49), (0x0000b, 0x01), (0x0000c, 0x50), (0x0000d, 0x01), (0x0000e, 0x02),
    (0x0000f, 0x02), (0x00010, 0x02), (0x00011, 0x80), (0x00012, 0xff), (0x00013, 0xff), (0x00014, 0xff), (0x00808, 0x78),
    (0x00809, 0x62), (0x0080a, 0x21), (0x0080b, 0x6d), (0x00818, 0x93), (0x00819, 0x78), (0x0081a, 0x85), (0x0081b, 0xf5),
    (0x0081c, 0x60), (0x0081d, 0xf5), (0x0081e, 0xf7), (0x0081f, 0xf7), (0x00820, 0xf7), (0x00821, 0x0b), (0x00822, 0xaa),
    (0x00823, 0xaa), (0x00824, 0xaa), (0x0085e, 0x88), (0x0085f, 0xa6), (0x00860, 0x63), (0x00861, 0xb7), (0x0086e, 0xc7),
    (0x0086f, 0x3c), (0x00870, 0xcc), (0x00871, 0xf4), (0x00872, 0x30), (0x00873, 0xf4), (0x00874, 0xf5), (0x00875, 0xf5),
    (0x00876, 0xf5), (0x00877, 0x8b), (0x00878, 0x55), (0x00879, 0x55), (0x0087a, 0x55), (0x008b4, 0xf0), (0x008b5, 0xc4),
    (0x008b6, 0x42), (0x008b7, 0xda), (0x008c6, 0x63), (0x008c7, 0xb7), (0x008c8, 0xd0), (0x008c9, 0xf7), (0x008ca, 0x59),
    (0x008cb, 0x26), (0x008cc, 0xea), (0x008cd, 0x66), (0x008d0, 0xd1), (0x008d2, 0xf3), (0x008d3, 0x15), (0x008d4, 0x4d),
    (0x008d5, 0xf5), (0x008d6, 0xf8), (0x008d7, 0x31), (0x008d8, 0x7e), (0x008d9, 0x2f), (0x008da, 0x6b), (0x008db, 0xcc),
    (0x008dc, 0x41), (0x008dd, 0x80), (0x008de, 0xe0), (0x008df, 0xf2), (0x008e0, 0x23), (0x008e1, 0x40), (0x008fa, 0x42),
    (0x008fb, 0xda), (0x008fc, 0xcb), (0x008fd, 0x22), (0x008fe, 0x93), (0x008ff, 0x5a), (0x00900, 0x1a), (0x00901, 0xa2),
    (0x00904, 0x7b), (0x00906, 0x0c), (0x00907, 0xbf), (0x00908, 0x10), (0x00909, 0xab), (0x0090a, 0x05), (0x0090b, 0xb2),
    (0x0090c, 0xe9), (0x0090d, 0xaf), (0x0090e, 0xdc), (0x0090f, 0xcf), (0x00910, 0x4e), (0x00911, 0x0d), (0x00912, 0x6e),
    (0x00913, 0xcf), (0x00914, 0x77), (0x00915, 0x04)])
SECTOR1 = [(0x00000, 0x02), (0x00002, 0x96), (0x00006, 0x2e), (0x00007, 0x01), (0x00024, 0xc4), (0x00025, 0x01), (0x00038, 0x02),
    (0x00041, 0xc4), (0x00042, 0x01), (0x0005a, 0xff), (0x0005b, 0xff), (0x0005c, 0xff), (0x0005d, 0xff), (0x0005e, 0xff),
    (0x0005f, 0xff), (0x00060, 0xff), (0x00061, 0xff), (0x00062, 0x01), (0x00066, 0x80), (0x0006a, 0x02), (0x0006e, 0x10),
    (0x00072, 0x44), (0x00073, 0xac), (0x000a0, 0xff), (0x000a1, 0xff), (0x000a2, 0xff), (0x000a3, 0xff), (0x000bd, 0x01)]
SECTOR2 = [(0x00000, 0x02), (0x00002, 0x96), (0x00006, 0x9c), (0x00007, 0x04), (0x00010, 0x02), (0x00018, 0x01), (0x00020, 0xb6),
    (0x00021, 0x2d), (0x00038, 0x01), (0x0003c, 0x04), (0x0005a, 0xff), (0x0005b, 0xff), (0x0005c, 0xff), (0x0005d, 0xff),
    (0x0005e, 0xff), (0x0005f, 0xff), (0x00060, 0xff), (0x00061, 0xff), (0x00062, 0x01), (0x00066, 0x80), (0x0006a, 0x02),
    (0x0006e, 0x10), (0x00072, 0x44), (0x00073, 0xac), (0x000a0, 0xff), (0x000a1, 0xff), (0x000a2, 0xff), (0x000a3, 0xff),
    (0x000b0, 0x02), (0x000b8, 0xb6), (0x000b9, 0x2d)]
DD_SECTOR1 = [(0x00000, 0x02), (0x00002, 0x96), (0x00006, 0x9c), (0x00007, 0x04), (0x00010, 0x02), (0x00038, 0x01), (0x0003c, 0x04),
    (0x0005a, 0xff), (0x0005b, 0xff), (0x0005c, 0xff), (0x0005d, 0xff), (0x0005e, 0xff), (0x0005f, 0xff), (0x00060, 0xff),
    (0x00061, 0xff), (0x00062, 0x01), (0x00066, 0x80), (0x0006a, 0x02), (0x0006e, 0x10), (0x00072, 0x44), (0x00073, 0xac),
    (0x000a0, 0xff), (0x000a1, 0xff), (0x000a2, 0xff), (0x000a3, 0xff), (0x000b0, 0x02), (0x000bd, 0x01)]
DD_SECTOR2 = [(0x00000, 0x02), (0x00002, 0x96), (0x00006, 0x2e), (0x00007, 0x01), (0x00010, 0x02), (0x00018, 0x01), (0x00024, 0xc4),
    (0x00025, 0x01), (0x00038, 0x01), (0x0003c, 0x04), (0x00041, 0xc4), (0x00042, 0x01), (0x0005a, 0xff), (0x0005b, 0xff),
    (0x0005c, 0xff), (0x0005d, 0xff), (0x0005e, 0xff), (0x0005f, 0xff), (0x00060, 0xff), (0x00061, 0xff), (0x00062, 0x01),
    (0x00066, 0x80), (0x0006a, 0x02), (0x0006e, 0x10), (0x00072, 0x44), (0x00073, 0xac), (0x000a0, 0xff), (0x000a1, 0xff),
    (0x000a2, 0xff), (0x000a3, 0xff), (0x000b0, 0x02)]
HEAD_NEXT = [(0x0000b, 0x02), (0x00016, 0x80), (0x00017, 0x40), (0x00018, 0x7e), (0x00019, 0x05), (0x0001d, 0x98)]
HEAD_END = [(0x00001, 0x01), (0x00005, 0x01), (0x00026, 0x06), (0x00029, 0x80)]
MARK = bytes([0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF])

def _inicio_cab(nome):
    return MARK * 2 + struct.pack('<HH', 0x00AB, 0x0210) + bytes([len(nome)]) + nome + _fill(31, HEAD_NEXT)

def grava_cdi(iso, saida, nome_cdi=b'C:\\BlackMatrixAD.cdi', progresso=None):
    assert len(iso) % 2048 == 0 and iso[0:32].startswith(b'SEGA SEGAKATANA SEGA ENTERPRISES')
    vol = iso[0x8028:0x8048]
    n = len(iso) // 2048
    f = open(saida, 'wb')
    f.write(b'\0' * 352800)                       # pre-gap da faixa de audio
    f.write(b'\0' * 710304)                       # faixa de audio: 302 setores de silencio
    for _ in range(75): f.write(GAP1)             # pre-gap da faixa de dados (150 setores)
    for _ in range(75): f.write(GAP2)
    arr = np.frombuffer(iso, np.uint8).reshape(n, 2048)
    L = 4096
    for i in range(0, n, L):
        f.write(setores_m2f1(arr[i:i + L]).tobytes())
        if progresso: progresso(min(n, i + L), n)
    blocos = n + 2
    while blocos < 302:
        f.write(b'\0' * 2336); blocos += 1
    f.write(GAP1); f.write(GAP1)                  # 2 setores de fim de faixa
    pos_cab = f.tell()
    usado = (301 + MSINFO + (blocos - 1)) - 150
    cab = bytearray(struct.pack('<HHI', 2, 1, 0))
    cab += _inicio_cab(nome_cdi) + _fill(195, SECTOR1)
    s2 = _fill(195, SECTOR2)
    s2[0x06:0x0a] = struct.pack('<I', blocos)
    s2[0x24:0x28] = struct.pack('<I', blocos + 150)
    s2[0x41:0x45] = struct.pack('<I', blocos + 150)
    cab += _inicio_cab(nome_cdi) + s2
    cab += _inicio_cab(nome_cdi) + struct.pack('<I', usado) + bytes([len(vol)]) + vol + _fill(42, HEAD_END)
    cab += struct.pack('<I', len(cab) + 4)
    f.write(cab); f.close()
    return blocos

def grava_cdi_dd(iso, saida, nome_cdi=b'C:\\BlackMatrixAD.cdi', progresso=None):
    """Modo dados/dados (cdi4dc -d): sessao 1 = ISO inteiro a partir do LBA 0 (ISO montado com base 0);
    sessao 2 = faixa de boot de 302 setores com os 18 primeiros setores do ISO (IP.BIN + descritores),
    no LBA (setores da faixa 1 + 11400)."""
    assert len(iso) % 2048 == 0 and iso[0:32].startswith(b'SEGA SEGAKATANA SEGA ENTERPRISES')
    vol = iso[0x8028:0x8048]
    n = len(iso) // 2048
    f = open(saida, 'wb')
    for _ in range(150): f.write(GAP1)            # pre-gap da faixa 1
    arr = np.frombuffer(iso, np.uint8).reshape(n, 2048)
    L = 4096
    for i in range(0, n, L):
        f.write(setores_m2f1(arr[i:i + L]).tobytes())
        if progresso: progresso(min(n, i + L), n)
    blocos = n + 2
    while blocos < 302:
        f.write(b'\0' * 2336); blocos += 1
    f.write(GAP1); f.write(GAP1)
    for _ in range(75): f.write(GAP1)             # pre-gap da faixa 2
    for _ in range(75): f.write(GAP2)
    f.write(setores_m2f1(arr[0:18]).tobytes())    # faixa de boot: IP.BIN + descritores
    f.write(b'\0' * 2336 * (300 - 18))
    f.write(GAP1); f.write(GAP1)
    pos_cab = f.tell()
    usado = (301 + MSINFO + (blocos - 1)) - 150
    cab = bytearray(struct.pack('<HHI', 2, 1, 0))
    s1 = _fill(195, DD_SECTOR1)
    s1[0x06:0x0a] = struct.pack('<I', blocos)
    s1[0x24:0x28] = struct.pack('<I', blocos + 150)
    s1[0x41:0x45] = struct.pack('<I', blocos + 150)
    cab += _inicio_cab(nome_cdi) + s1
    s2 = _fill(195, DD_SECTOR2); lba2 = blocos + 11400
    s2[0x20:0x24] = struct.pack('<I', lba2); s2[0xb8:0xbc] = struct.pack('<I', lba2)
    cab += _inicio_cab(nome_cdi) + s2
    cab += _inicio_cab(nome_cdi) + struct.pack('<I', usado) + bytes([len(vol)]) + vol + _fill(42, HEAD_END)
    cab += struct.pack('<I', len(cab) + 4)
    f.write(cab); f.close()
    return blocos, lba2
