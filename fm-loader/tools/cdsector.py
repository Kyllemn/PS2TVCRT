"""Setores de CD do PS1 (MODE2/2352): leitura de imagens .bin, codificacao Mode 2 Form 1 (EDC/ECC)
e listagem ISO9660 com os registros de diretorio originais (para copiar atributos XA)."""
import struct

SYNC = b'\x00' + b'\xff' * 10 + b'\x00'

# ---------- EDC / ECC (ECMA-130) ----------
_edc = []
for i in range(256):
    e = i
    for _ in range(8): e = (e >> 1) ^ (0xD8018001 if e & 1 else 0)
    _edc.append(e)
_f = [0] * 256; _b = [0] * 256
for i in range(256):
    j = (i << 1) ^ (0x11D if i & 0x80 else 0); _f[i] = j; _b[i ^ j] = i


def edc(data):
    e = 0
    for c in data: e = (e >> 8) ^ _edc[(e ^ c) & 0xff]
    return e


def _ecc_pq(sec, major, minor, mmul, minc, out_off):
    """calcula P (major=86,minor=24) ou Q (52,43) sobre sec[12:], grava em sec[out_off:]"""
    size = major * minor
    for ma in range(major):
        idx = (ma >> 1) * mmul + (ma & 1)
        a = b = 0
        for mi in range(minor):
            t = sec[12 + idx]
            idx += minc
            if idx >= size: idx -= size
            a ^= t; b ^= t; a = _f[a]
        a = _b[_f[a] ^ b]
        sec[out_off + ma] = a
        sec[out_off + ma + major] = a ^ b


def bcd(v): return ((v // 10) << 4) | (v % 10)


def msf(lba):
    l = lba + 150
    return bytes([bcd(l // 4500), bcd((l // 75) % 60), bcd(l % 75)])


def form1(lba, data, sub=0x08):
    """setor Mode 2 Form 1 completo (2352) com 2048 bytes de dados; sub = flags do subcabecalho"""
    data = data.ljust(2048, b'\0')
    s = bytearray(2352)
    s[0:12] = SYNC; s[12:15] = msf(lba); s[15] = 2
    sh = bytes([0, 0, sub, 0]); s[16:24] = sh + sh
    s[24:2072] = data
    s[2072:2076] = struct.pack('<I', edc(s[16:2072]))
    hdr = bytes(s[12:16]); s[12:16] = b'\0\0\0\0'      # Mode 2: ECC com o cabecalho zerado
    _ecc_pq(s, 86, 24, 2, 86, 2076)
    _ecc_pq(s, 52, 43, 86, 88, 2076 + 172)
    s[12:16] = hdr
    return bytes(s)


def relocate(raw, lba):
    """setor bruto copiado para outra posicao: so o endereco do cabecalho muda (ECC/EDC do Mode 2
    nao dependem dele)"""
    s = bytearray(raw); s[12:15] = msf(lba); return bytes(s)


def patch_form1(raw, lba, off, new):
    """altera bytes dentro dos dados de um setor Form 1 e recalcula EDC/ECC"""
    d = bytearray(raw[24:2072]); d[off:off + len(new)] = new
    return form1(lba, bytes(d), raw[18])


# ---------- leitura de imagem e ISO9660 ----------
class Bin:
    def __init__(self, fn):
        self.f = open(fn, 'rb')
        self.f.seek(0, 2); self.nsec = self.f.tell() // 2352

    def raw(self, lba, n=1):
        self.f.seek(lba * 2352); return self.f.read(2352 * n)

    def data(self, lba):
        self.f.seek(lba * 2352 + 24); return self.f.read(2048)

    def files(self):
        """[(caminho, lba, tamanho, registro_de_diretorio_original)]; diretorios terminam em '/'"""
        out = []
        pvd = self.data(16); r = pvd[156:190]
        self._walk(struct.unpack_from('<I', r, 2)[0], struct.unpack_from('<I', r, 10)[0], '', out)
        return out

    def _walk(self, lba, size, path, out):
        data = b''.join(self.data(lba + i) for i in range((size + 2047) // 2048)); o = 0
        while o < len(data):
            n = data[o]
            if n == 0: o = (o // 2048 + 1) * 2048; continue
            r = data[o:o + n]
            l = struct.unpack_from('<I', r, 2)[0]; sz = struct.unpack_from('<I', r, 10)[0]; nm = r[33:33 + r[32]]
            if nm not in (b'\0', b'\1'):
                name = nm.decode('ascii')
                if r[25] & 2:
                    out.append((path + name + '/', l, sz, bytes(r)))
                    self._walk(l, sz, path + name + '/', out)
                else:
                    out.append((path + name, l, sz, bytes(r)))
            o += n
