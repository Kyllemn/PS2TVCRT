"""RETROVOLT Pi - leitora de CD e identificacao de discos/imagens.

Identifica o sistema pelo conteudo (nada de nome de arquivo):
  PS1 (PVD "PLAYSTATION" + SYSTEM.CNF), Sega CD ("SEGADISCSYSTEM"),
  Saturn ("SEGA SEGASATURN"), PC Engine CD ("PC Engine CD-ROM SYSTEM" na faixa de
  dados), 3DO (cabecalho Opera), Neo Geo CD (IPL.TXT), CD de audio.
Nunca manda comando de velocidade para a leitora.
"""
import fcntl
import gzip
import os
import struct

CDROMREADTOCHDR = 0x5305
CDROMREADTOCENTRY = 0x5306
CDROMREADRAW = 0x5314
CDROM_DRIVE_STATUS = 0x5326
CDS_NO_DISC, CDS_TRAY_OPEN, CDS_DRIVE_NOT_READY, CDS_DISC_OK = 1, 2, 3, 4
CDROM_LBA, CDROM_LEADOUT, CDSL_CURRENT = 1, 0xAA, 0x7fffffff

AQUI = os.path.dirname(os.path.abspath(__file__))


def leitoras():
    return [p for p in ('/dev/sr0', '/dev/sr1', '/dev/sr2') if os.path.exists(p)]


def estado(dev):
    """'ok', 'vazia', 'aberta', 'lendo' ou 'erro'"""
    try:
        fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK)
    except OSError:
        return 'erro'
    try:
        st = fcntl.ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT)
    except OSError:
        st = -1
    finally:
        os.close(fd)
    return {CDS_DISC_OK: 'ok', CDS_NO_DISC: 'vazia', CDS_TRAY_OPEN: 'aberta',
            CDS_DRIVE_NOT_READY: 'lendo'}.get(st, 'vazia')


# ------------------------------------------------------------------ fontes de setores
class Leitora:
    """setores de um disco na leitora"""

    def __init__(self, dev):
        self.fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK)
        self.faixas = []   # (numero, lba, dados?)
        hdr = bytearray(2)
        fcntl.ioctl(self.fd, CDROMREADTOCHDR, hdr)
        for t in list(range(hdr[0], hdr[1] + 1)) + [CDROM_LEADOUT]:
            e = bytearray(struct.pack('BBBxiB3x', t, 0, CDROM_LBA, 0, 0))
            fcntl.ioctl(self.fd, CDROMREADTOCENTRY, e)
            ctrl = e[1] >> 4
            lba = struct.unpack_from('i', e, 4)[0]
            self.faixas.append((t, lba, bool(ctrl & 4)))
        self.fim = self.faixas.pop()[1]

    def raw(self, lba):
        a = lba + 150
        b = bytearray(2352)
        b[0:3] = bytes((a // 4500, a // 75 % 60, a % 75))
        fcntl.ioctl(self.fd, CDROMREADRAW, b)
        return bytes(b)

    def ler(self, lba):
        """2048 bytes de dados do setor (modo 1 ou modo 2 forma 1)"""
        try:
            return os.pread(self.fd, 2048, lba * 2048)
        except OSError:
            r = self.raw(lba)
            return r[24:24 + 2048] if r[15] == 2 else r[16:16 + 2048]

    def fechar(self):
        os.close(self.fd)


class Imagem:
    """setores de uma imagem (.iso 2048, .bin/.img 2352 ou .cue)"""

    def __init__(self, path):
        self.faixas = [(1, 0, True)]
        ext = os.path.splitext(path)[1].lower()
        if ext == '.cue':
            path, self.faixas = self._cue(path)
        self.f = open(path, 'rb')
        tam = os.fstat(self.f.fileno()).st_size
        cab = self.f.read(16)
        self.setor = 2352 if cab[:12] == b'\x00' + b'\xff' * 10 + b'\x00' else (2048 if ext == '.iso' else 2352)
        if not self.faixas[0][2]:          # cue com audio primeiro: setor 2352
            self.setor = 2352
        self.fim = tam // self.setor

    @staticmethod
    def _cue(path):
        pasta = os.path.dirname(path)
        arq, faixas, num, dados = None, [], 0, True
        for l in open(path, errors='replace'):
            p = l.strip().split()
            if not p:
                continue
            if p[0].upper() == 'FILE' and arq is None:
                arq = os.path.join(pasta, l.split('"')[1] if '"' in l else p[1])
            elif p[0].upper() == 'TRACK':
                num, dados = int(p[1]), p[2].upper() != 'AUDIO'
            elif p[0].upper() == 'INDEX' and p[1] == '01':
                m, s, f = map(int, p[2].split(':'))
                faixas.append((num, (m * 60 + s) * 75 + f, dados))
        return arq, faixas or [(1, 0, True)]

    def ler(self, lba):
        self.f.seek(lba * self.setor)
        r = self.f.read(self.setor)
        if self.setor == 2048:
            return r
        return r[24:24 + 2048] if r[15:16] == b'\x02' else r[16:16 + 2048]

    def fechar(self):
        self.f.close()


# ------------------------------------------------------------------ ISO9660
def _iso_raiz(src, base=0):
    pvd = src.ler(base + 16)
    if pvd[0:6] != b'\x01CD001':
        return None
    lba, tam = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
    return pvd, lba, tam


def _iso_lista(src, lba, tam):
    out = {}
    for k in range((tam + 2047) // 2048):
        d = src.ler(lba + k)
        i = 0
        while i < 2048 and d[i]:
            n = d[i]
            nl = d[i + 32]
            nome = d[i + 33:i + 33 + nl].decode('ascii', 'replace').split(';')[0].upper()
            out[nome] = (struct.unpack_from('<I', d, i + 2)[0], struct.unpack_from('<I', d, i + 10)[0])
            i += n
    return out


def _codigo_ps1(boot):
    # 'cdrom:\SLUS_008.92;1' -> 'SLUS-00892'
    b = boot.replace('\\', '/').split(':')[-1].split('/')[-1].split(';')[0].upper()
    return b.replace('_', '-').replace('.', '')


_nomes = {}


def nome_jogo(sistema, codigo):
    if not codigo:
        return None
    if sistema not in _nomes:
        _nomes[sistema] = {}
        try:
            with gzip.open(os.path.join(AQUI, 'dados', '%s_nomes.txt.gz' % sistema), 'rt') as f:
                for l in f:
                    k, _, v = l.rstrip('\n').partition('\t')
                    _nomes[sistema][k] = v
        except OSError:
            pass
    return _nomes[sistema].get(codigo)


def identifica(src):
    """{'sistema', 'nome', 'codigo', 'rotulo', 'digital'}; sistema None = desconhecido.
    digital = "impressao digital" das faixas (igual em todo disco igual): nome de save de
    reserva para disco sem codigo nem rotulo (cada jogo com os seus saves e estados)"""
    import zlib
    r = {'sistema': None, 'nome': None, 'codigo': None, 'rotulo': None}
    toc = ','.join('%d' % lba for _, lba, _ in src.faixas) + ',%d' % getattr(src, 'fim', 0)
    r['digital'] = 'CD-%08X' % (zlib.crc32(toc.encode()) & 0xffffffff)
    faixas = src.faixas
    if faixas and not any(d for _, _, d in faixas):
        r['sistema'] = 'audio'
        r['nome'] = 'CD DE AUDIO (%d FAIXAS)' % len(faixas)
        return r
    dados = [lba for _, lba, d in faixas if d]
    base = dados[0] if dados else 0
    try:
        s0 = src.ler(base)
    except OSError:
        s0 = b''
    if s0[:14] == b'SEGADISCSYSTEM':
        r['sistema'] = 'segacd'
        r['nome'] = s0[0x120:0x150].decode('ascii', 'replace').strip() or None
        r['codigo'] = s0[0x180:0x18e].decode('ascii', 'replace').strip() or None
        return r
    if s0[:15] == b'SEGA SEGASATURN':
        r['sistema'] = 'saturn'
        r['codigo'] = s0[0x20:0x2a].decode('ascii', 'replace').strip() or None
        r['nome'] = s0[0x60:0xd0].decode('ascii', 'replace').strip() or None
        return r
    if s0[:7] == b'\x01ZZZZZ\x01':
        r['sistema'] = '3do'
        r['rotulo'] = s0[0x28:0x48].split(b'\0')[0].decode('ascii', 'replace').strip() or None
        return r
    if len(dados) and faixas and not faixas[0][2]:
        try:
            if src.ler(base + 1)[0x20:0x37] == b'PC Engine CD-ROM SYSTEM':
                r['sistema'] = 'pcenginecd'
                # o CD do PC Engine nao tem codigo: usa a "impressao digital" das faixas
                # (quantidade e tamanho), que e a mesma em todo disco igual -> nome do save
                import zlib
                toc = ','.join('%d' % lba for _, lba, _ in faixas) + ',%d' % getattr(src, 'fim', 0)
                r['codigo'] = 'PCECD-%08X' % (zlib.crc32(toc.encode()) & 0xffffffff)
                return r
        except OSError:
            pass
    raiz = _iso_raiz(src, base)
    if raiz:
        pvd, lba, tam = raiz
        sysid = pvd[8:40].decode('ascii', 'replace').strip()
        r['rotulo'] = pvd[40:72].decode('ascii', 'replace').strip() or None
        try:
            arqs = _iso_lista(src, lba, tam)
        except Exception:
            arqs = {}
        if 'IPL.TXT' in arqs:
            r['sistema'] = 'neogeocd'
            return r
        if sysid == 'PLAYSTATION' or 'SYSTEM.CNF' in arqs or 'PSX.EXE' in arqs:
            r['sistema'] = 'ps1'
            if 'SYSTEM.CNF' in arqs:
                l, t = arqs['SYSTEM.CNF']
                cnf = src.ler(l)[:min(t, 2048)].decode('ascii', 'replace')
                for linha in cnf.splitlines():
                    if linha.strip().upper().startswith('BOOT'):
                        r['codigo'] = _codigo_ps1(linha.split('=', 1)[1].strip())
            r['nome'] = nome_jogo('ps1', r['codigo'])
            return r
    return r


def identifica_disco(dev):
    src = Leitora(dev)
    try:
        return identifica(src)
    finally:
        src.fechar()


def identifica_arquivo(path):
    if os.path.splitext(path)[1].lower() not in ('.cue', '.iso', '.bin', '.img'):
        return None
    try:
        src = Imagem(path)
    except Exception:
        return None
    try:
        return identifica(src)
    except Exception:
        return None
    finally:
        src.fechar()
