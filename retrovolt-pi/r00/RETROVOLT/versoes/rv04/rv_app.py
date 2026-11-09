#!/usr/bin/env python3
"""RETROVOLT Pi - R04: menu com abas (DISCO / JOGOS / TV / OPCOES), leitora de CD
e o primeiro nucleo (PlayStation, pcsx_rearmed). Os jogos rodam no rv-jogo (C).

Desenha direto no framebuffer (/dev/fb0) uma tela logica de 320x240. No tubo
(super-resolucao 1920x240) a imagem logica e colocada num retangulo ajustavel
(posicao X/Y e largura) = CONVERGENCIA; os valores ficam em
RETROVOLT/config/retrovolt.cfg (pendrive) e numa copia no cartao.

Telas: MENU (CONVERGENCIA, TESTE DO CONTROLE, INFO), CONVERGENCIA, CONTROLE, INFO.
Controle: qualquer controle/teclado pelo evdev (8BitDo no modo Xbox: A=confirma,
B=volta, L1/R1, START, SELECT, direcional ou analogico).

Teste fora do Pi:  RV_FAKEFB=saida.png:1920x240 RV_TELA=conv python3 rv_app.py
"""
import fcntl
import mmap
import os
import queue
import subprocess
import sys
import threading
import time

import disco

# fonte 8x8 (font8x8_basic, dominio publico), 95 glifos ASCII 32..126
FONT_HEX = (
    '0000000000000000183c3c1818001800363600000000000036367f367f3636000c3e031e301f'
    '0c00006333180c6663001c361c6e3b336e000606030000000000180c0606060c1800060c1818'
    '180c060000663cff3c660000000c0c3f0c0c000000000000001818300000003f000000000000'
    '0000001818006030180c060301003e63737b6f673e000c0e0c0c0c0c3f001e33301c06333f00'
    '1e33301c30331e00383c36337f3030003f031f3030331e001c06031f33331e003f3330180c0c'
    '0c001e33331e33331e001e33333e30180e000018180000181800001818000018183030180c06'
    '0c18300000003f003f000000060c1830180c06001e3330180c000c003e637b7b7b031e000c1e'
    '33333f3333003f66663e66663f003c66030303663c001f36666666361f007f46161e16467f00'
    '7f46161e16060f003c66030373667c003333333f333333001e0c0c0c0c0c1e00783030303333'
    '1e006766361e366667000f06060646667f0063777f7f6b63630063676f7b736363001c366363'
    '63361c003f66663e06060f001e3333333b1e38003f66663e366667001e33070e38331e003f2c'
    '0c0c0c0c1e003333333333333f0033333333331e0c006363636b7f7763006363361c1c366300'
    '3333331e0c0c1e007f6331184c667f001e06060606061e0003060c18306040001e1818181818'
    '1e00081c36630000000000000000000000ff0c0c18000000000000001e303e336e000706063e'
    '66663b0000001e3303331e003830303e33336e0000001e333f031e001c36060f06060f000000'
    '6e33333e301f0706366e666667000c000e0c0c0c1e00300030303033331e070666361e366700'
    '0e0c0c0c0c0c1e000000337f7f6b630000001f333333330000001e3333331e0000003b66663e'
    '060f00006e33333e307800003b6e66060f0000003e031e301f00080c3e0c0c2c180000003333'
    '33336e0000003333331e0c000000636b7f7f3600000063361c36630000003333333e301f0000'
    '3f190c263f00380c0c070c0c38001818180018181800070c0c380c0c07006e3b000000000000'
)
FONT = bytes.fromhex(FONT_HEX)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fontes import FONTES  # noqa: E402

# fontes disponiveis: (nome na tela, largura, altura, glifos 32..255)
FONTES_LISTA = [('8X8', 8, 8, FONT + bytes(8 * 129))] + [
    (n.upper(), cw, ch, bytes.fromhex(hx)) for n, (cw, ch, hx) in FONTES.items()]


class Fonte:
    """fonte em uso (troca em OPCOES > FONTE); todo o desenho usa F.cw / F.ch"""
    def usa(self, nome):
        for n, cw, ch, b in FONTES_LISTA:
            if n == nome.upper():
                self.nome, self.cw, self.ch, self.b = n, cw, ch, b
                return
        self.usa('8X8')


F = Fonte()
F.usa('8X8')

W, H = 320, 240
NAV = (12, 18, 40)
ORANGE = (255, 140, 40)
IVORY = (240, 236, 225)
SLATE = (120, 130, 160)
GREEN = (90, 220, 140)
RED = (230, 110, 90)
LINE = (40, 50, 80)

VERSAO = os.environ.get('RV_VERSAO', 'rv04')
ORIGEM = os.environ.get('RV_ORIGEM', 'CARTAO')
USB = os.environ.get('RV_USB', '/media/rvusb')
OK_FILE = '/run/retrovolt/ok'
DADOS = '/var/lib/retrovolt'

# ---------------------------------------------------------------- configuracao
CFG_PADRAO = [
    ('VIDEO', None, None),
    ('video_posicao_x', '0', 'posicao horizontal da imagem no tubo (pontos da tela larga; 6 = 1 pixel do jogo)'),
    ('video_posicao_y', '0', 'posicao vertical (linhas)'),
    ('video_largura', '1.000', 'largura da imagem (1.000 = tela toda; 0.900 = 90%)'),
    ('video_altura', '1.000', 'altura da imagem (1.000 = todas as linhas; 0.950 = 95%)'),
    ('SOM', None, None),
    ('volume', '80', 'volume geral dos jogos, 0 a 100'),
    ('LEITORA', None, None),
    ('leitora_velocidade', '4', 'velocidade da leitora nos jogos (2, 4, 8...; 0 = maxima, mais barulho)'),
    ('MENU', None, None),
    ('fonte', '6X10', 'fonte do menu: ' + ', '.join(f[0] for f in FONTES_LISTA)),
]


def cfg_caminhos():
    p = []
    if os.path.isdir(os.path.join(USB, 'RETROVOLT')):
        p.append(os.path.join(USB, 'RETROVOLT', 'config', 'retrovolt.cfg'))
    p.append(os.path.join(DADOS, 'config', 'retrovolt.cfg'))
    return p


def cfg_ler():
    c = {k: v for k, v, _ in CFG_PADRAO if v is not None}
    for path in cfg_caminhos():
        try:
            for linha in open(path, encoding='utf-8', errors='replace'):
                linha = linha.split('#', 1)[0].strip()
                if '=' not in linha:
                    continue
                k, v = linha.split('=', 1)
                k = k.strip()
                v = v.strip().strip('"')
                if k in c:
                    c[k] = v
            print('config lida de', path, flush=True)
            break
        except OSError:
            continue
    return c


def cfg_gravar(c):
    txt = ['# RETROVOLT Pi - configuracao. Pode editar no PC; valor errado volta ao padrao.', '']
    for k, v, com in CFG_PADRAO:
        if v is None:
            txt.append('# ---------- %s ----------' % k)
            continue
        txt.append('%s = "%s"%s' % (k, c.get(k, v), '    # ' + com if com else ''))
    data = '\n'.join(txt) + '\n'
    for path in cfg_caminhos():
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            tmp = path + '.tmp'
            with open(tmp, 'w', encoding='utf-8') as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, path)
            print('config gravada em', path, flush=True)
        except OSError as e:
            print('ERRO ao gravar config em', path, e, flush=True)


def num(c, k, tipo, lo, hi):
    try:
        v = tipo(c[k])
    except (KeyError, ValueError):
        v = tipo(dict((a, b) for a, b, _ in CFG_PADRAO if b)[k])
    return max(lo, min(hi, v))


# ---------------------------------------------------------------- desenho
class Canvas:
    def __init__(self):
        self.px = [NAV] * (W * H)

    def rect(self, x, y, w, h, c):
        for j in range(max(0, y), min(H, y + h)):
            row = j * W
            for i in range(max(0, x), min(W, x + w)):
                self.px[row + i] = c

    def text(self, x, y, s, c):
        for n, ch in enumerate(s):
            o = ord(ch) - 32
            if o < 0 or o > 223:
                continue
            for r in range(F.ch):
                b = F.b[o * F.ch + r]
                yy = y + r
                if not 0 <= yy < H:
                    continue
                for k in range(F.cw):
                    if b >> k & 1:
                        xx = x + n * F.cw + k
                        if 0 <= xx < W:
                            self.px[yy * W + xx] = c

    def rtext(self, x, y, s, c):
        self.text(x - len(s) * F.cw, y, s, c)

    def ctext(self, y, s, c):
        self.text((W - len(s) * F.cw) // 2, y, s, c)


def moldura(c, titulo, rodape_d):
    c.rect(0, 0, 6, H, ORANGE)
    c.rect(W - 6, 0, 6, H, ORANGE)
    c.text(16, 6, 'RETRO', IVORY)
    c.text(16 + 5 * F.cw, 6, 'VOLT', ORANGE)
    c.rtext(W - 12, 6, time.strftime('%H:%M'), IVORY)
    c.text(16, 28, titulo, IVORY)
    c.rect(16, 28 + F.ch + 2, len(titulo) * F.cw, 2, ORANGE)
    c.rect(16, 48, W - 32, 1, LINE)
    c.text(16, H - 12, VERSAO.upper(), SLATE)
    c.rtext(W - 12, H - 12, rodape_d, SLATE)


def item(c, x, y, s, on, valor=None):
    c.text(x, y, s, IVORY if on else SLATE)
    if on:
        c.rect(x - 8, y - 1, 3, F.ch + 2, ORANGE)
        c.rect(x, y + F.ch + 2, len(s) * F.cw, 1, ORANGE)
        c.text(x + (len(s) + 1) * F.cw, y, '->', ORANGE)
    if valor:
        c.rtext(W - 14, y, valor, ORANGE if on else (170, 150, 120))


# ---------------------------------------------------------------- framebuffer
class FB:
    def __init__(self):
        fake = os.environ.get('RV_FAKEFB')
        if fake:
            path, size = fake.split(':')
            self.w, self.h = map(int, size.split('x'))
            self.bpp, self.stride, self.fake = 32, self.w * 4, path
            self.mem = bytearray(self.stride * self.h)
            return
        self.fake = None
        base = '/sys/class/graphics/fb0/'
        self.w, self.h = map(int, open(base + 'virtual_size').read().split(','))
        self.bpp = int(open(base + 'bits_per_pixel').read())
        self.stride = int(open(base + 'stride').read())
        self.fd = os.open('/dev/fb0', os.O_RDWR)
        self.mem = mmap.mmap(self.fd, self.stride * self.h)

    @property
    def tubo(self):
        return self.w >= self.h * 2.5      # super-resolucao (1920x240/288/480i)

    def pixel(self, c):
        r, g, b = c
        if self.bpp == 16:
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            return bytes((v & 0xff, v >> 8))
        return bytes((b, g, r, 0))

    def show(self, canvas, conv):
        """conv = (posicao_x, posicao_y, largura, altura). Fora da imagem: preto.
        A tela inteira e montada fora do framebuffer e copiada de uma vez (sem piscar)."""
        bpp = self.bpp // 8
        preto = self.pixel((0, 0, 0))
        if self.tubo:
            linhas_por = max(1, self.h // H)              # 480i: 2 linhas por linha logica
            cw = max(W, min(self.w, round(self.w * conv[2])))
            ch = max(H // 2, round(H * linhas_por * conv[3]))
            x0 = (self.w - cw) // 2 + conv[0]
            y0 = (self.h - ch) // 2 + conv[1] * linhas_por
        else:
            s = max(1, min(self.w // W, self.h // H))
            cw, ch = W * s, H * s
            x0, y0 = (self.w - cw) // 2, (self.h - ch) // 2
        cont = [((i + 1) * cw) // W - (i * cw) // W for i in range(W)]
        cache = {}
        linhas = {}
        corte_e = max(0, -x0) * bpp
        dest_x = max(0, x0)
        linha_preta = preto * (self.stride // bpp)
        frame = bytearray(linha_preta * self.h)
        for j in range(ch):
            yy = y0 + j
            if not 0 <= yy < self.h:
                continue
            y = (2 * j + 1) * H // (2 * ch)                 # linha logica (centro): 1a e ultima sempre aparecem
            chave = tuple(canvas.px[y * W:(y + 1) * W])
            row = linhas.get(chave)
            if row is None:
                parts = []
                for x in range(W):
                    k = (chave[x], cont[x])
                    p = cache.get(k)
                    if p is None:
                        p = cache[k] = self.pixel(chave[x]) * cont[x]
                    parts.append(p)
                row = b''.join(parts)[corte_e:]
                row = row[:max(0, (self.w - dest_x) * bpp)]
                linhas[chave] = row
            off = yy * self.stride + dest_x * bpp
            frame[off:off + len(row)] = row
        self.espera_vsync()
        self.mem[0:len(frame)] = frame
        if self.fake:
            from PIL import Image
            Image.frombuffer('RGBX', (self.w, self.h), bytes(self.mem), 'raw', 'BGRX', self.stride, 1).convert('RGB').save(self.fake)

    def espera_vsync(self):
        """espera o retraco vertical (FBIO_WAITFORVSYNC), se o driver suportar"""
        if self.fake or getattr(self, 'sem_vsync', False):
            return
        try:
            fcntl.ioctl(self.fd, 0x40044620, b'\0\0\0\0')
        except OSError:
            self.sem_vsync = True
            print('aviso: framebuffer sem FBIO_WAITFORVSYNC', flush=True)


def tty_graphics(on):
    try:
        fd = os.open('/dev/tty1', os.O_RDWR)
        fcntl.ioctl(fd, 0x4B3A, 1 if on else 0)
        os.close(fd)
    except Exception:
        pass


# ---------------------------------------------------------------- controle (evdev)
class Entrada(threading.Thread):
    """Le todos os controles e teclados. Gera acoes: CIMA BAIXO ESQ DIR A B X Y L1 R1 START SELECT.
    Tambem guarda o ultimo evento cru (para a tela de teste do controle)."""
    REPETE_INI, REPETE = 0.30, 0.07

    def __init__(self, fila):
        super().__init__(daemon=True)
        self.fila = fila
        self.devs = {}
        self.ultimo = ''
        self.nomes = []
        self.segurando = {}
        try:
            import evdev
            self.evdev = evdev
            self.e = evdev.ecodes
        except ImportError:
            self.evdev = None
            print('AVISO: python3-evdev ausente: sem controle', flush=True)

    def mapa(self):
        e = self.e
        return {
            e.KEY_UP: 'CIMA', e.KEY_DOWN: 'BAIXO', e.KEY_LEFT: 'ESQ', e.KEY_RIGHT: 'DIR',
            e.KEY_ENTER: 'A', e.KEY_SPACE: 'A', e.KEY_ESC: 'B', e.KEY_BACKSPACE: 'B',
            e.KEY_PAGEUP: 'L1', e.KEY_PAGEDOWN: 'R1', e.KEY_Q: 'L1', e.KEY_E: 'R1',
            e.KEY_S: 'START', e.KEY_TAB: 'SELECT',
            e.BTN_SOUTH: 'A', e.BTN_EAST: 'B', e.BTN_NORTH: 'X', e.BTN_WEST: 'Y',
            e.BTN_TL: 'L1', e.BTN_TR: 'R1', e.BTN_START: 'START', e.BTN_SELECT: 'SELECT',
            e.BTN_TL2: 'L2', e.BTN_TR2: 'R2', e.KEY_HOME: 'L2', e.KEY_END: 'R2',
            e.BTN_DPAD_UP: 'CIMA', e.BTN_DPAD_DOWN: 'BAIXO', e.BTN_DPAD_LEFT: 'ESQ', e.BTN_DPAD_RIGHT: 'DIR',
        }

    def procura(self):
        ev = self.evdev
        for p in ev.list_devices():
            if p in self.devs:
                continue
            try:
                d = ev.InputDevice(p)
                caps = d.capabilities()
                if self.e.EV_KEY not in caps:
                    d.close()
                    continue
                self.devs[p] = d
                print('entrada: %s (%s)' % (d.name, p), flush=True)
            except OSError:
                pass
        self.nomes = [d.name for d in self.devs.values()]

    def solta(self, nome):
        self.segurando.pop(nome, None)

    def aperta(self, nome):
        self.fila.put(nome)
        if nome in ('CIMA', 'BAIXO', 'ESQ', 'DIR', 'L1', 'R1', 'L2', 'R2'):
            self.segurando[nome] = time.monotonic() + self.REPETE_INI

    def _pendentes(self):
        out = []
        try:
            while True:
                out.append(self.fila.get_nowait())
        except queue.Empty:
            pass
        return out

    def run(self):
        if not self.evdev:
            return
        import select
        mp = self.mapa()
        e = self.e
        eixo = {}
        prox_busca = 0
        while True:
            agora = time.monotonic()
            if agora >= prox_busca:
                self.procura()
                prox_busca = agora + 2
            for nome, quando in list(self.segurando.items()):
                if agora >= quando:
                    self.fila.put(nome)
                    self.segurando[nome] = agora + self.REPETE
            fds = {d.fd: (p, d) for p, d in self.devs.items()}
            try:
                r, _, _ = select.select(list(fds), [], [], 0.03)
            except (OSError, ValueError):
                r = []
            for fd in r:
                p, d = fds[fd]
                try:
                    evs = list(d.read())
                except OSError:
                    print('entrada removida: %s' % d.name, flush=True)
                    self.devs.pop(p, None)
                    continue
                for ev in evs:
                    if ev.type == e.EV_KEY:
                        nome = mp.get(ev.code)
                        nm = e.BTN.get(ev.code) or e.KEY.get(ev.code) or ev.code
                        if isinstance(nm, (list, tuple)):
                            nm = nm[0]
                        self.ultimo = '%s %s %s' % (d.name[:14], nm, ev.value)
                        if nome:
                            if ev.value == 1:
                                self.aperta(nome)
                            elif ev.value == 0:
                                self.solta(nome)
                        self.fila.put('_raw')
                    elif ev.type == e.EV_ABS:
                        # direcional (HAT) e analogico como setas
                        if ev.code in (e.ABS_Z, e.ABS_RZ, e.ABS_BRAKE, e.ABS_GAS):
                            # gatilhos analogicos (8BitDo/Xbox: LT = ABS_Z, RT = ABS_RZ)
                            try:
                                info = d.absinfo(ev.code)
                                v = (ev.value - info.min) / max(1, info.max - info.min)
                            except Exception:
                                v = 0
                            nome = 'L2' if ev.code in (e.ABS_Z, e.ABS_BRAKE) else 'R2'
                            estado = nome if v > 0.5 else None
                            antes = eixo.get((p, ev.code))
                            if estado != antes:
                                if antes:
                                    self.solta(antes)
                                if estado:
                                    self.aperta(estado)
                                eixo[(p, ev.code)] = estado
                                self.ultimo = '%s %s %d' % (d.name[:14], e.ABS.get(ev.code, ev.code), ev.value)
                                self.fila.put('_raw')
                        elif ev.code in (e.ABS_HAT0X, e.ABS_HAT0Y, e.ABS_X, e.ABS_Y):
                            try:
                                info = d.absinfo(ev.code)
                                meio = (info.max + info.min) / 2
                                faixa = max(1, (info.max - info.min) / 2)
                                v = (ev.value - meio) / faixa
                            except Exception:
                                v = ev.value
                            dirs = ('ESQ', 'DIR') if ev.code in (e.ABS_HAT0X, e.ABS_X) else ('CIMA', 'BAIXO')
                            estado = dirs[0] if v < -0.5 else (dirs[1] if v > 0.5 else None)
                            antes = eixo.get((p, ev.code))
                            if estado != antes:
                                if antes:
                                    self.solta(antes)
                                if estado:
                                    self.aperta(estado)
                                eixo[(p, ev.code)] = estado
                                self.ultimo = '%s %s %d' % (d.name[:14], e.ABS.get(ev.code, ev.code), ev.value)
                                self.fila.put('_raw')


# ---------------------------------------------------------------- informacoes
def info():
    d = {}
    try:
        d['temp'] = int(open('/sys/class/thermal/thermal_zone0/temp').read()) // 1000
    except Exception:
        d['temp'] = None
    try:
        m = {l.split(':')[0]: int(l.split()[1]) for l in open('/proc/meminfo')}
        d['ram'] = (m['MemTotal'] - m['MemAvailable']) / 1048576, m['MemTotal'] / 1048576
    except Exception:
        d['ram'] = None
    try:
        out = subprocess.run(['hostname', '-I'], capture_output=True, text=True, timeout=2).stdout.split()
        d['ip'] = out[0] if out else None
    except Exception:
        d['ip'] = None
    d['wifi'] = None
    try:
        for l in subprocess.run(['nmcli', '-t', '-f', 'active,ssid', 'dev', 'wifi'], capture_output=True, text=True, timeout=3).stdout.splitlines():
            if l.startswith('yes:'):
                d['wifi'] = l[4:] or None
    except Exception:
        pass
    return d


def edid_real():
    """Le a EDID verdadeira pelo fio de dados do HDMI (DDC/I2C), so para o log:
    a EDID do RetroVolt substitui a do adaptador, e precisamos da real para a
    deteccao TUBO/LCD."""
    try:
        subprocess.run(['modprobe', 'i2c-dev'], timeout=5)
        for b in sorted(os.listdir('/sys/bus/i2c/devices')):
            if not b.startswith('i2c-'):
                continue
            try:
                nome = open('/sys/bus/i2c/devices/%s/name' % b).read().strip()
            except OSError:
                continue
            print('i2c: %s = %s' % (b, nome), flush=True)
            if 'ddc' not in nome.lower() and 'hdmi' not in nome.lower():
                continue
            n = b.split('-')[1]
            r = subprocess.run(['i2ctransfer', '-y', n, 'w1@0x50', '0x00', 'r128'],
                               capture_output=True, text=True, timeout=5)
            print('edid real %s: %s %s' % (b, r.stdout.strip(), r.stderr.strip()), flush=True)
    except Exception as ex:
        print('edid real: erro', ex, flush=True)


# ---------------------------------------------------------------- sistemas e pastas
PASTA = os.path.dirname(os.path.abspath(__file__))
# pasta em roms/ -> (nome na tela, nucleo, tem CD)
SISTEMAS = {
    'ps1': ('PLAYSTATION', 'pcsx_rearmed_libretro.so', True),
    'segacd': ('SEGA CD', None, True),
    'saturn': ('SATURN', None, True),
    'pcenginecd': ('PC ENGINE CD', None, True),
    'neogeocd': ('NEO GEO CD', None, True),
    '3do': ('3DO', None, True),
    'atari2600': ('ATARI 2600', None, False), 'nes': ('NES', None, False),
    'snes': ('SUPER NINTENDO', None, False), 'n64': ('NINTENDO 64', None, False),
    'gb': ('GAME BOY', None, False), 'gbc': ('GAME BOY COLOR', None, False),
    'gba': ('GAME BOY ADVANCE', None, False), 'mastersystem': ('MASTER SYSTEM', None, False),
    'megadrive': ('MEGA DRIVE', None, False), '32x': ('32X', None, False),
    'dreamcast': ('DREAMCAST', None, False), 'pcengine': ('PC ENGINE', None, False),
    'neogeo': ('NEO GEO', None, False), 'arcade': ('ARCADE', None, False),
}
EXT_JOGO = {
    'ps1': ('.cue', '.chd', '.pbp', '.m3u', '.iso', '.img', '.bin', '.exe'),
}
NOME_SISTEMA = {k: v[0] for k, v in SISTEMAS.items()}
NOME_SISTEMA['audio'] = 'CD DE AUDIO'


def base():
    """pasta RETROVOLT (pendrive) ou a do cartao; cria a estrutura se faltar (nunca apaga nada)"""
    b = os.environ.get('RV_BASE')
    if not b:
        b = os.path.join(USB, 'RETROVOLT') if os.path.isdir(os.path.join(USB, 'RETROVOLT')) else os.path.join(DADOS, 'RETROVOLT')
    for p in ('config', 'bios', 'roms', 'saves', 'states', 'tv', 'imagens/consoles', 'capturas', 'logs'):
        os.makedirs(os.path.join(b, p), exist_ok=True)
    for s, (_, nucleo, _) in SISTEMAS.items():
        if nucleo:
            os.makedirs(os.path.join(b, 'roms', s), exist_ok=True)
    return b


def nome_limpo(arq):
    import re
    n = os.path.splitext(os.path.basename(arq))[0]   # tira (USA), [!] etc., mas deixa (Disc 1)
    disc = re.search(r'\((?:Disc|Disco|CD)\s*(\d+)\)', n, re.I)
    n = re.sub(r'\s*[\(\[][^\)\]]*[\)\]]', '', n).strip()
    if disc:
        n += ' (DISCO %s)' % disc.group(1)
    return n.upper().replace('_', ' ')


def lista_jogos(b, sistema):
    raiz = os.path.join(b, 'roms', sistema)
    exts = EXT_JOGO.get(sistema, ())
    achados = []
    try:
        nomes = sorted(os.listdir(raiz))
    except OSError:
        return []
    for n in nomes:
        p = os.path.join(raiz, n)
        if os.path.isdir(p):           # um nivel: pasta de jogo com varios discos
            try:
                for m in sorted(os.listdir(p)):
                    achados.append(os.path.join(p, m))
            except OSError:
                pass
        else:
            achados.append(p)
    achados = [p for p in achados if os.path.splitext(p)[1].lower() in exts and not os.path.basename(p).startswith('.')]
    # .bin com .cue na mesma pasta: mostra so o .cue; com .m3u, so o .m3u
    pastas_cue = {os.path.dirname(p) for p in achados if p.lower().endswith('.cue')}
    pastas_m3u = {os.path.dirname(p) for p in achados if p.lower().endswith('.m3u')}
    out = []
    for p in achados:
        d, ext = os.path.dirname(p), os.path.splitext(p)[1].lower()
        if ext == '.bin' and d in pastas_cue:
            continue
        if ext != '.m3u' and d in pastas_m3u and d != raiz:
            continue
        out.append(p)
    return sorted(out, key=lambda p: nome_limpo(p))


def copia_log():
    """copia o LOG.TXT do cartao para o pendrive (o lancador so copia no inicio)"""
    src = os.path.join(DADOS, 'logs', 'LOG.TXT')
    dst = os.path.join(USB, 'RETROVOLT', 'LOG.TXT')
    if not os.path.isdir(os.path.dirname(dst)) or not os.path.exists(src):
        return
    try:
        sys.stdout.flush()
        with open(src, 'rb') as a, open(dst + '.tmp', 'wb') as b:
            b.write(a.read())
            b.flush()
            os.fsync(b.fileno())
        os.replace(dst + '.tmp', dst)
    except OSError as ex:
        print('log: nao copiou para o pendrive:', ex, flush=True)


# ---------------------------------------------------------------- leitora (thread)
class Leitora(threading.Thread):
    """vigia a leitora: avisa quando entra/sai um disco (ja identificado)"""

    def __init__(self, fila):
        super().__init__(daemon=True)
        self.fila = fila
        self.dev = None
        self.estado = 'procurando'
        self.info = None
        self.pausa = threading.Event()

    def run(self):
        fake = os.environ.get('RV_CD')
        while True:
            if self.pausa.is_set():
                time.sleep(0.5)
                continue
            if fake:
                if self.estado != 'ok':
                    self.dev = fake
                    self.info = disco.identifica_arquivo(fake)
                    self.estado = 'ok'
                    self.fila.put('_disco_novo')
                time.sleep(1)
                continue
            devs = disco.leitoras()
            if not devs:
                novo = 'sem_leitora'
            else:
                self.dev = devs[0]
                novo = disco.estado(self.dev)
            if novo == 'ok' and self.estado != 'ok':
                self.estado = 'lendo'
                self.fila.put('_disco')
                try:
                    self.info = disco.identifica_disco(self.dev)
                except Exception as ex:
                    print('leitora: erro ao ler o disco:', ex, flush=True)
                    self.info = {'sistema': None, 'nome': None, 'codigo': None, 'rotulo': None}
                print('leitora: disco', self.info, flush=True)
                self.estado = 'ok'
                self.fila.put('_disco_novo')
            elif novo != 'ok' and novo != self.estado and not (novo == 'lendo' and self.estado == 'lendo'):
                if self.estado == 'ok':
                    print('leitora: disco saiu', flush=True)
                self.estado = novo
                self.info = None
                self.fila.put('_disco')
            time.sleep(1.5)


# ---------------------------------------------------------------- telas
ABAS = ['DISCO', 'JOGOS', 'TV', 'OPCOES']
OPCOES = ['VOLUME', 'CONVERGENCIA', 'FONTE', 'TESTE DO CONTROLE', 'INFO', 'REINICIAR', 'DESLIGAR']


def cabecalho(c, aba, wifi, rodape_d):
    c.rect(0, 0, 6, H, ORANGE)
    c.rect(W - 6, 0, 6, H, ORANGE)
    c.text(16, 6, 'RETRO', IVORY)
    c.text(16 + 5 * F.cw, 6, 'VOLT', ORANGE)
    hora = time.strftime('%H:%M')
    c.rtext(W - 12, 6, hora, IVORY)
    c.rtext(W - 12 - (len(hora) + 2) * F.cw, 6, (wifi or 'SEM WI-FI')[:14].upper(), GREEN if wifi else SLATE)
    x = 16
    for a in ABAS:
        on = a == aba
        c.text(x, 26, a, IVORY if on else SLATE)
        if on:
            c.rect(x, 26 + F.ch + 2, len(a) * F.cw, 2, ORANGE)
        x += (len(a) + 3) * F.cw
    c.rect(16, 26 + F.ch + 8, W - 32, 1, LINE)
    c.text(16, H - 4 - F.ch, 'L1/R1 ABAS', SLATE)
    c.rtext(W - 12, H - 4 - F.ch, rodape_d, SLATE)


def desenha_cd(c, cx, cy, r):
    """CD liso: aneis iridescentes, brilho e furo"""
    import math
    cores = [(170, 180, 200), (190, 170, 220), (160, 200, 230), (200, 220, 190), (230, 200, 170), (210, 180, 210)]
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            d = math.hypot(x - cx, (y - cy))
            if d > r:
                continue
            if d < r * 0.13:
                continue                                  # furo
            if d < r * 0.30:
                c.px[y * W + x] = (200, 205, 215) if d > r * 0.16 else (120, 125, 140)
                continue
            ang = math.atan2(y - cy, x - cx)
            k = (ang / (2 * math.pi) + 0.5 + d / r * 0.35) % 1.0
            i = int(k * len(cores))
            a, b = cores[i], cores[(i + 1) % len(cores)]
            f = k * len(cores) - i
            col = [int(a[j] + (b[j] - a[j]) * f) for j in range(3)]
            brilho = max(0.0, math.cos(ang + 0.8)) ** 6 * 70
            col = tuple(min(255, int(v + brilho)) for v in col)
            if d > r - 1.2:
                col = tuple(int(v * 0.6) for v in col)
            c.px[y * W + x] = col


class App:
    def __init__(self):
        self.fb = FB()
        self.cfg = cfg_ler()
        self.conv = [num(self.cfg, 'video_posicao_x', int, -300, 300),
                     num(self.cfg, 'video_posicao_y', int, -20, 20),
                     num(self.cfg, 'video_largura', float, 0.70, 1.10),
                     num(self.cfg, 'video_altura', float, 0.80, 1.10)]
        F.usa(self.cfg.get('fonte', '6X10'))
        self.fila = queue.Queue()
        self.ent = Entrada(self.fila)
        self.leitora = Leitora(self.fila)
        self.base = base()
        self.aba = os.environ.get('RV_ABA', 'DISCO')
        self.tela = os.environ.get('RV_TELA', 'menu')   # menu | conv | fonte | controle | info | aviso
        self.sel = {'JOGOS': 0, 'OPCOES': 0, 'SIS': 0}
        self.sistema = os.environ.get('RV_SISTEMA')       # dentro de uma pasta de JOGOS
        self.jogos = lista_jogos(self.base, self.sistema) if self.sistema else []
        self.info = info()
        self.wifi = self.info.get('wifi')
        self.salvo = 0
        self.bs = 0
        self.ultima_acao = ''
        self.aviso = None
        self.confirma = None
        self.contagem = None      # (REINICIAR|DESLIGAR, hora final)
        self.vol_sujo = 0

    # ---------------------------------------------------------- jogos
    def sistemas_com_jogos(self):
        out = []
        cd = self.leitora.info if self.leitora.estado == 'ok' else None
        for s in SISTEMAS:
            tem = bool(lista_jogos(self.base, s)) if s in EXT_JOGO else False
            if not tem and s in EXT_JOGO:
                tem = bool(cd and cd.get('sistema') == s)
            if not tem and s not in EXT_JOGO:
                try:
                    tem = any(not n.startswith('.') for n in os.listdir(os.path.join(self.base, 'roms', s)))
                except OSError:
                    tem = False
            if tem:
                out.append(s)
        return out

    def itens_pasta(self):
        """lista da pasta: INICIAR CD, 2 linhas vazias, jogos"""
        itens = []
        if SISTEMAS[self.sistema][2]:
            itens.append(('cd', 'INICIAR CD'))
            itens.append(('vazio', ''))
            itens.append(('vazio', ''))
        for p in self.jogos:
            itens.append(('arq', p))
        return itens

    def jogar(self, sistema, arquivo=None, cd=False):
        nucleo = SISTEMAS.get(sistema, (None, None))[1]
        if not nucleo:
            self.mostra_aviso('%s: NUCLEO AINDA NAO INSTALADO' % NOME_SISTEMA.get(sistema, '?'))
            return
        exe = os.path.join(PASTA, 'rv-jogo')
        so = os.path.join(PASTA, 'nucleos', nucleo)
        if not os.access(exe, os.X_OK):
            try:          # pendrive sem permissao de execucao: roda uma copia
                os.makedirs('/run/retrovolt/bin', exist_ok=True)
                subprocess.run(['cp', exe, '/run/retrovolt/bin/rv-jogo'])
                os.chmod('/run/retrovolt/bin/rv-jogo', 0o755)
                exe = '/run/retrovolt/bin/rv-jogo'
            except OSError as ex:
                print('jogo: rv-jogo sem permissao:', ex, flush=True)
        if cd:
            i = self.leitora.info or {}
            nome = i.get('nome') or i.get('rotulo') or 'DISCO'
            save = i.get('codigo') or i.get('rotulo') or 'DISCO'
        else:
            i = disco.identifica_arquivo(arquivo) or {}
            nome = nome_limpo(arquivo)
            save = i.get('codigo') or os.path.splitext(os.path.basename(arquivo))[0]
        save = ''.join(ch if ch.isalnum() or ch in '-_ ' else '_' for ch in save).strip() or 'jogo'
        saves = os.path.join(self.base, 'saves', sistema)
        os.makedirs(saves, exist_ok=True)
        cmd = [exe, '--nucleo', so, '--bios', os.path.join(self.base, 'bios'), '--saves', saves,
               '--nome', nome[:60], '--save', save, '--fonte', F.nome, '--volume', str(self.volume()),
               '--conv', '%d,%d,%.3f,%.3f' % tuple(self.conv),
               '--opcoes', os.path.join(self.base, 'config', 'nucleos', nucleo.replace('_libretro.so', '.opt'))]
        cmd += ['--cd', self.leitora.dev, '--cd-vel', self.cfg.get('leitora_velocidade', '4')] if cd else ['--jogo', arquivo]
        if self.cfg.get('audio_dispositivo'):
            cmd += ['--audio', self.cfg['audio_dispositivo']]
        self.carregando(nome)
        print('jogo: %s' % ' '.join(cmd), flush=True)
        copia_log()
        self.leitora.pausa.set()
        try:
            os.remove('/run/retrovolt/volume')
        except OSError:
            pass
        t0 = time.monotonic()
        try:
            r = subprocess.run(cmd).returncode
        except OSError as ex:
            r = -1
            print('jogo: nao rodou:', ex, flush=True)
        self.leitora.pausa.clear()
        try:   # volume mudado no menu do jogo (START+SELECT)
            v = int(open('/run/retrovolt/volume').read())
            self.cfg['volume'] = str(max(0, min(100, v)))
            cfg_gravar(self.cfg)
        except (OSError, ValueError):
            pass
        print('jogo: terminou (codigo %s, %.0f s)' % (r, time.monotonic() - t0), flush=True)
        self.ent.segurando.clear()
        self._pendentes()
        copia_log()
        if r != 0:
            self.mostra_aviso('O JOGO NAO ABRIU (CODIGO %s). VEJA O LOG.TXT' % r)

    def carregando(self, nome):
        c = Canvas()
        cabecalho(c, self.aba, self.wifi, '')
        c.ctext(100, 'CARREGANDO', ORANGE)
        c.ctext(100 + F.ch + 8, nome[:40], IVORY)
        self.fb.show(c, self.conv)

    def mostra_aviso(self, txt):
        self.aviso = (txt, time.monotonic() + 4)

    # ---------------------------------------------------------- desenho
    def desenha(self):
        c = Canvas()
        t = self.tela
        if t == 'menu':
            if self.aba == 'DISCO':
                self.desenha_disco(c)
            elif self.aba == 'JOGOS':
                self.desenha_jogos(c)
            elif self.aba == 'TV':
                cabecalho(c, 'TV', self.wifi, '')
                c.text(24, 70, 'RETROTV', IVORY)
                c.text(24, 70 + F.ch + 8, 'CHEGA NA PROXIMA VERSAO (R05).', SLATE)
            else:
                cabecalho(c, 'OPCOES', self.wifi, 'A ENTRA')
                y = 58
                for n, s in enumerate(OPCOES):
                    on = n == self.sel['OPCOES']
                    if s == 'VOLUME':
                        v = self.volume()
                        item(c, 24, y, s, on)
                        bx, bw = 24 + 14 * F.cw, 100
                        c.rect(bx, y + F.ch // 2 - 1, bw, 3, LINE)
                        c.rect(bx, y + F.ch // 2 - 1, bw * v // 100, 3, ORANGE if on else SLATE)
                        c.text(bx + bw + 8, y, ('< %d >' if on else '%d') % v, ORANGE if on else SLATE)
                        y += F.ch + 9
                        continue
                    if s == 'DESLIGAR':
                        c.text(24, y, s, RED if on else (150, 80, 70))
                        if on:
                            c.rect(16, y - 1, 3, F.ch + 2, RED)
                            c.rect(24, y + F.ch + 2, len(s) * F.cw, 1, RED)
                            c.text(24 + (len(s) + 1) * F.cw, y, '->', RED)
                    else:
                        item(c, 24, y, s, on)
                    y += F.ch + 9
            if self.confirma:
                self.desenha_confirma(c)
            if self.contagem:
                self.desenha_contagem(c)
        elif t == 'conv':
            self.desenha_conv(c)
        elif t == 'fonte':
            moldura(c, 'FONTE', 'A SALVA  B VOLTA')
            nomes = [f[0] for f in FONTES_LISTA]
            c.text(24, 60, 'FONTE %d DE %d: %s' % (nomes.index(F.nome) + 1, len(nomes), F.nome), ORANGE)
            c.text(24, 60 + F.ch + 4, '<- ->  TROCA A FONTE', SLATE)
            y = 96
            for n, s in enumerate(['DISCO', 'JOGOS', 'OPCOES']):
                item(c, 24, y, s, n == 0)
                y += F.ch + 10
            c.text(24, 160, 'PLAYSTATION  NINTENDO 64', IVORY)
            c.text(24, 160 + F.ch + 4, 'ABCDEFGHIJKLMNOPQRSTUVWXYZ', IVORY)
            c.text(24, 160 + 2 * (F.ch + 4), '0123456789  12:45', IVORY)
            if time.monotonic() < self.salvo:
                c.rtext(W - 14, 60, 'SALVO!', GREEN)
        elif t == 'controle':
            moldura(c, 'TESTE DO CONTROLE', 'B 3X VOLTA')
            c.text(24, 64, 'APERTE OS BOTOES:', SLATE)
            c.text(24, 84, (self.ent.ultimo or '-').upper()[:34], GREEN)
            c.text(24, 110, 'ACOES (MAPEADAS):', SLATE)
            c.text(24, 122, (self.ultima_acao or '-'), ORANGE)
            c.text(24, 150, 'CONTROLES:', SLATE)
            for k, nm in enumerate((self.ent.nomes or ['NENHUM'])[:3]):
                c.text(24, 162 + k * (F.ch + 2), nm.upper()[:40], IVORY)
            c.text(24, 206, 'SAIR: APERTE B 3 VEZES', SLATE)
        elif t == 'info':
            moldura(c, 'INFO', 'B VOLTA')
            d = self.info
            bios = sorted(n for n in os.listdir(os.path.join(self.base, 'bios')) if not n.startswith('.')) if os.path.isdir(os.path.join(self.base, 'bios')) else []
            linhas = [
                ('VERSAO', '%s (%s)' % (VERSAO.upper(), ORIGEM)),
                ('VIDEO', '%dX%d %s' % (self.fb.w, self.fb.h, 'TUBO' if self.fb.tubo else 'LCD')),
                ('TEMP', '%dC' % d['temp'] if d['temp'] is not None else '?'),
                ('RAM', '%.1f/%.1fGB' % d['ram'] if d['ram'] else '?'),
                ('REDE', d['ip'] or 'SEM REDE'),
                ('WI-FI', (d['wifi'] or 'SEM WI-FI')[:18]),
                ('JOGOS', 'PENDRIVE' if self.base.startswith(USB) else 'CARTAO'),
                ('LEITORA', {'procurando': '...', 'ok': 'COM DISCO', 'vazia': 'VAZIA', 'aberta': 'ABERTA', 'lendo': 'LENDO',
                             'sem_leitora': 'NAO ENCONTRADA'}.get(self.leitora.estado, '?')),
                ('BIOS', ('%d: ' % len(bios) + ' '.join(bios))[:34] if bios else 'NENHUMA'),
            ]
            y = 60
            for k, v in linhas:
                c.text(24, y, k, SLATE)
                c.text(96, y, v.upper(), IVORY)
                y += F.ch + 6
        if self.aviso and time.monotonic() < self.aviso[1]:
            txt = self.aviso[0][:48]
            w = len(txt) * F.cw + 16
            c.rect((W - w) // 2, 190, w, F.ch + 10, (60, 30, 20))
            c.rect((W - w) // 2, 190, w, 1, ORANGE)
            c.ctext(195, txt, IVORY)
        return c

    def desenha_confirma(self, c):
        txt = '%s? A SIM  B NAO' % self.confirma
        w = len(txt) * F.cw + 20
        c.rect((W - w) // 2, 100, w, F.ch + 20, (40, 20, 20))
        c.rect((W - w) // 2, 100, w, 2, RED)
        c.ctext(110, txt, IVORY)

    def desenha_disco(self, c):
        st = self.leitora.estado
        i = self.leitora.info if st == 'ok' else None
        pode = bool(i and SISTEMAS.get(i.get('sistema'), (0, None))[1])
        cabecalho(c, 'DISCO', self.wifi, 'A JOGAR' if pode else '')
        if st == 'ok':
            desenha_cd(c, 88, 128, 56)
        else:
            for r, cor in ((56, (30, 38, 66)), (20, NAV)):
                for y in range(128 - r, 128 + r + 1):
                    for x in range(88 - r, 88 + r + 1):
                        if (x - 88) ** 2 + (y - 128) ** 2 <= r * r:
                            c.px[y * W + x] = cor
        x, y = 160, 66
        if st == 'procurando':
            c.text(x, y, 'PROCURANDO A', SLATE)
            c.text(x, y + F.ch + 4, 'LEITORA...', SLATE)
            return
        if st == 'sem_leitora':
            c.text(x, y, 'LEITORA NAO', SLATE)
            c.text(x, y + F.ch + 4, 'ENCONTRADA', SLATE)
            return
        if st != 'ok':
            c.text(x, y, {'vazia': 'LEITORA VAZIA', 'aberta': 'LEITORA ABERTA',
                          'lendo': 'LENDO O DISCO...'}.get(st, 'LEITORA'), SLATE)
            if st != 'lendo':
                c.text(x, y + F.ch + 6, 'COLOQUE UM DISCO', SLATE)
            return
        c.text(x, y, 'DISCO NA LEITORA', SLATE)
        sis = i.get('sistema')
        c.text(x, y + F.ch + 8, NOME_SISTEMA.get(sis, 'DESCONHECIDO'), ORANGE)
        nome = i.get('nome') or i.get('rotulo') or ''
        yy = y + 2 * F.ch + 16
        palavras, linha = nome.upper().split(), ''
        lim = (W - 16 - x) // F.cw
        linhas = []
        for p in palavras:
            if len(linha) + len(p) + (1 if linha else 0) > lim:
                linhas.append(linha)
                linha = p
            else:
                linha = (linha + ' ' + p).strip()
        if linha:
            linhas.append(linha)
        for l in linhas[:3]:
            c.text(x, yy, l, IVORY)
            yy += F.ch + 2
        if i.get('codigo'):
            c.text(x, yy + 4, i['codigo'], SLATE)
            yy += F.ch + 6
        if sis in SISTEMAS:
            c.text(x, yy + 4, 'NUCLEO: %s' % (SISTEMAS[sis][1] or 'AINDA NAO').replace('_libretro.so', '').upper(), SLATE)
        if pode:
            c.text(x, 188, 'A JOGAR', IVORY)
            c.rect(x, 188 + F.ch + 2, 7 * F.cw, 1, ORANGE)
            c.text(x + 8 * F.cw, 188, '->', ORANGE)

    def desenha_jogos(self, c):
        if not self.sistema:
            cabecalho(c, 'JOGOS', self.wifi, 'A ENTRA')
            sis = self.sistemas_com_jogos()
            if not sis:
                c.text(24, 64, 'NENHUM JOGO ENCONTRADO.', SLATE)
                c.text(24, 64 + F.ch + 6, 'COLOQUE OS JOGOS EM', SLATE)
                c.text(24, 64 + 2 * (F.ch + 6), 'RETROVOLT/ROMS/PS1/', IVORY)
                return
            self.sel['SIS'] = min(self.sel['SIS'], len(sis) - 1)
            y = 58
            for n, s in enumerate(sis):
                item(c, 24, y, NOME_SISTEMA[s], n == self.sel['SIS'])
                y += F.ch + 9
            return
        itens = self.itens_pasta()
        cabecalho(c, 'JOGOS', self.wifi, 'A JOGA  B VOLTA')
        titulo = NOME_SISTEMA[self.sistema]
        n_jogos = len(self.jogos)
        sel = self.sel['JOGOS']
        c.text(16, 50, titulo, ORANGE)
        pos_jogo = sum(1 for k, _ in itens[:sel + 1] if k == 'arq')
        if itens[sel][0] == 'arq':
            c.rtext(W - 12, 50, '%d/%d' % (pos_jogo, n_jogos), SLATE)
        passo = F.ch + 7
        y0 = 50 + F.ch + 10
        cabem = (H - 8 - F.ch - y0) // passo
        ini = max(0, min(sel - cabem // 2, len(itens) - cabem))
        cd = self.leitora.info if self.leitora.estado == 'ok' else None
        y = y0
        for n in range(ini, min(len(itens), ini + cabem)):
            k, v = itens[n]
            if k == 'cd':
                ok = bool(cd and cd.get('sistema') == self.sistema)
                valor = None
                if not ok:
                    valor = {'lendo': 'LENDO...', 'sem_leitora': 'SEM LEITORA', 'procurando': '...'}.get(self.leitora.estado, 'SEM DISCO')
                    if cd:
                        valor = NOME_SISTEMA.get(cd.get('sistema'), 'OUTRO DISCO')
                item(c, 24, y, v, n == sel, valor)
                if not ok and n != sel:
                    pass
            elif k == 'arq':
                lim = (W - 40) // F.cw - 3
                item(c, 24, y, nome_limpo(v)[:lim], n == sel)
            y += passo
        if not self.jogos:
            c.text(24, y0 + 3 * passo, 'SEM JOGOS NA PASTA ROMS/%s' % self.sistema.upper(), SLATE)

    # ---------------------------------------------------------- acoes
    def acao_menu(self, a):
        if self.contagem:
            if a == 'B':
                print('opcoes: %s cancelado' % self.contagem[0], flush=True)
                self.contagem = None
            return
        if self.confirma:
            if a == 'A':
                self.contagem = (self.confirma, time.monotonic() + 5)
                print('opcoes: %s em 5 s' % self.confirma, flush=True)
                threading.Thread(target=self.prepara_desligar, daemon=True).start()
            if a in ('A', 'B'):
                self.confirma = None
            return
        if a in ('L1', 'R1'):
            i = ABAS.index(self.aba)
            self.aba = ABAS[(i + (1 if a == 'R1' else -1)) % len(ABAS)]
            return
        if self.aba == 'DISCO':
            i = self.leitora.info if self.leitora.estado == 'ok' else None
            if a == 'A' and i:
                self.jogar(i.get('sistema'), cd=True) if i.get('sistema') in SISTEMAS else \
                    self.mostra_aviso('DISCO NAO RECONHECIDO')
        elif self.aba == 'JOGOS':
            if not self.sistema:
                sis = self.sistemas_com_jogos()
                if not sis:
                    return
                if a == 'CIMA':
                    self.sel['SIS'] = (self.sel['SIS'] - 1) % len(sis)
                elif a == 'BAIXO':
                    self.sel['SIS'] = (self.sel['SIS'] + 1) % len(sis)
                elif a == 'A':
                    self.sistema = sis[min(self.sel['SIS'], len(sis) - 1)]
                    self.jogos = lista_jogos(self.base, self.sistema)
                    self.sel['JOGOS'] = 0
                return
            itens = self.itens_pasta()
            sel = self.sel['JOGOS']
            if a in ('CIMA', 'BAIXO', 'ESQ', 'DIR'):
                d = -1 if a in ('CIMA', 'ESQ') else 1
                passos = 1 if a in ('CIMA', 'BAIXO') else 10
                for _ in range(passos):
                    n = sel + d
                    while 0 <= n < len(itens) and itens[n][0] == 'vazio':
                        n += d
                    if 0 <= n < len(itens):
                        sel = n
                self.sel['JOGOS'] = sel
            elif a == 'B':
                self.sistema = None
            elif a == 'A' and itens:
                k, v = itens[sel]
                if k == 'cd':
                    cd = self.leitora.info if self.leitora.estado == 'ok' else None
                    if cd and cd.get('sistema') == self.sistema:
                        self.jogar(self.sistema, cd=True)
                    elif cd:
                        self.mostra_aviso('O DISCO NA LEITORA E DE %s' % NOME_SISTEMA.get(cd.get('sistema'), 'OUTRO SISTEMA'))
                    else:
                        self.mostra_aviso('COLOQUE UM DISCO NA LEITORA')
                elif k == 'arq':
                    self.jogar(self.sistema, arquivo=v)
        elif self.aba == 'OPCOES':
            if OPCOES[self.sel['OPCOES']] == 'VOLUME' and a in ('ESQ', 'DIR'):
                v = max(0, min(100, self.volume() + (5 if a == 'DIR' else -5)))
                self.cfg['volume'] = str(v)
                self.vol_sujo = time.monotonic()
                return
            if a == 'CIMA':
                self.sel['OPCOES'] = (self.sel['OPCOES'] - 1) % len(OPCOES)
            elif a == 'BAIXO':
                self.sel['OPCOES'] = (self.sel['OPCOES'] + 1) % len(OPCOES)
            elif a == 'A':
                o = OPCOES[self.sel['OPCOES']]
                if o == 'VOLUME':
                    return
                if o in ('REINICIAR', 'DESLIGAR'):
                    self.confirma = o
                    return
                self.tela = {'CONVERGENCIA': 'conv', 'FONTE': 'fonte', 'TESTE DO CONTROLE': 'controle', 'INFO': 'info'}[o]
                if self.tela == 'conv':
                    self.conv_antes = list(self.conv)
                if self.tela == 'fonte':
                    self.fonte_antes = F.nome
                if self.tela == 'info':
                    self.info = info()

    def acao(self, a):
        t = self.tela
        if t == 'menu':
            self.acao_menu(a)
        elif t == 'conv':
            x, y, w, h = self.conv
            passo = 6 if self.fb.tubo else 0
            if a == 'ESQ':
                x -= passo
            elif a == 'DIR':
                x += passo
            elif a == 'CIMA':
                y -= 1
            elif a == 'BAIXO':
                y += 1
            elif a == 'L1':
                w -= 0.005
            elif a == 'R1':
                w += 0.005
            elif a == 'L2':
                h -= 0.005
            elif a == 'R2':
                h += 0.005
            elif a == 'SELECT':
                x, y, w, h = 0, 0, 1.0, 1.0
            elif a == 'A':
                self.cfg.update(video_posicao_x=str(x), video_posicao_y=str(y),
                                video_largura='%.3f' % w, video_altura='%.3f' % h)
                cfg_gravar(self.cfg)
                self.conv_antes = [x, y, w, h]
                self.salvo = time.monotonic() + 2
            elif a == 'B':
                x, y, w, h = self.conv_antes
                self.tela = 'menu'
            self.conv = [max(-300, min(300, x)), max(-20, min(20, y)),
                         max(0.70, min(1.10, round(w, 3))), max(0.80, min(1.10, round(h, 3)))]
        elif t == 'fonte':
            nomes = [f[0] for f in FONTES_LISTA]
            i = nomes.index(F.nome)
            if a in ('ESQ', 'CIMA', 'L1'):
                F.usa(nomes[(i - 1) % len(nomes)])
            elif a in ('DIR', 'BAIXO', 'R1'):
                F.usa(nomes[(i + 1) % len(nomes)])
            elif a == 'A':
                self.cfg['fonte'] = F.nome
                cfg_gravar(self.cfg)
                self.fonte_antes = F.nome
                self.salvo = time.monotonic() + 2
            elif a == 'B':
                F.usa(self.fonte_antes)
                self.tela = 'menu'
        elif t == 'controle':
            self.ultima_acao = a
            self.bs = self.bs + 1 if a == 'B' else 0
            if self.bs >= 3:
                self.bs = 0
                self.tela = 'menu'
        elif t == 'info':
            if a == 'B':
                self.tela = 'menu'

    def volume(self):
        try:
            return max(0, min(100, int(self.cfg.get('volume', '80'))))
        except ValueError:
            return 80

    def prepara_desligar(self):
        """durante a contagem: grava configuracao e log e esvazia os caches de disco"""
        try:
            cfg_gravar(self.cfg)
        except Exception as ex:
            print('desligar: cfg', ex, flush=True)
        copia_log()
        os.sync()

    def fim_contagem(self):
        acao = self.contagem[0]
        self.contagem = None
        c = Canvas()
        cabecalho(c, self.aba, self.wifi, '')
        c.ctext(110, 'DESLIGANDO...' if acao == 'DESLIGAR' else 'REINICIANDO...', ORANGE)
        self.fb.show(c, self.conv)
        copia_log()
        os.sync()
        cmd = ['systemctl', 'poweroff' if acao == 'DESLIGAR' else 'reboot']
        print('opcoes: %s' % ' '.join(cmd), flush=True)
        if not self.fb.fake:
            subprocess.Popen(cmd)

    def desenha_contagem(self, c):
        acao, fim = self.contagem
        n = max(1, int(fim - time.monotonic() + 0.999))
        txt = '%s EM %d' % ('DESLIGANDO' if acao == 'DESLIGAR' else 'REINICIANDO', n)
        w = max(len(txt), 14) * F.cw + 30
        c.rect((W - w) // 2, 92, w, 2 * F.ch + 30, (40, 20, 20))
        c.rect((W - w) // 2, 92, w, 2, RED if acao == 'DESLIGAR' else ORANGE)
        c.ctext(104, txt, IVORY)
        c.ctext(104 + F.ch + 8, 'B CANCELA', SLATE)

    def disco_novo(self):
        """disco entrou: se estiver no menu, abre o jogo na hora"""
        i = self.leitora.info or {}
        sis = i.get('sistema')
        print('disco novo: %s' % i, flush=True)
        if self.tela != 'menu' or self.confirma:
            self.mostra_aviso('DISCO: %s' % (i.get('nome') or NOME_SISTEMA.get(sis, '?')))
            return
        self.aba = 'DISCO'
        if sis in SISTEMAS and SISTEMAS[sis][1]:
            self.jogar(sis, cd=True)

    def _pendentes(self):
        out = []
        try:
            while True:
                out.append(self.fila.get_nowait())
        except queue.Empty:
            pass
        return out

    def _wifi(self):
        while True:
            time.sleep(30)
            try:
                self.wifi = info().get('wifi')
            except Exception:
                pass

    def run(self):
        fake = self.fb.fake
        if not fake:
            tty_graphics(True)
            self.ent.start()
            threading.Thread(target=edid_real, daemon=True).start()
            threading.Thread(target=self._wifi, daemon=True).start()
        if not fake or os.environ.get('RV_CD'):
            self.leitora.start()
        try:
            if fake:
                if os.environ.get('RV_CD'):
                    time.sleep(1.5)
                    self._pendentes()
                self.fb.show(self.desenha(), self.conv)
                return
            self.fb.show(self.desenha(), self.conv)
            os.makedirs(os.path.dirname(OK_FILE), exist_ok=True)
            open(OK_FILE, 'w').write(VERSAO + '\n')
            ultimo_desenho = time.monotonic()
            while True:
                try:
                    a = self.fila.get(timeout=0.25 if self.contagem else 1.0)
                except queue.Empty:
                    a = None
                mudou = bool(self.contagem)
                for b in [a] + self._pendentes():
                    if b is None:
                        continue
                    if b == '_raw':
                        mudou = mudou or self.tela == 'controle'
                    elif b == '_disco':
                        mudou = True
                    elif b == '_disco_novo':
                        self.disco_novo()
                        mudou = True
                    else:
                        self.acao(b)
                        mudou = True
                if self.vol_sujo and time.monotonic() - self.vol_sujo > 1.5:
                    self.vol_sujo = 0
                    cfg_gravar(self.cfg)
                if self.contagem and time.monotonic() >= self.contagem[1]:
                    self.fim_contagem()
                    continue
                if self.aviso and time.monotonic() > self.aviso[1]:
                    self.aviso = None
                    mudou = True
                if mudou or time.monotonic() - ultimo_desenho > 60:
                    if time.monotonic() - ultimo_desenho > 60:
                        copia_log()
                    self.fb.show(self.desenha(), self.conv)
                    ultimo_desenho = time.monotonic()
        finally:
            if not fake:
                tty_graphics(False)


if __name__ == '__main__':
    App().run()
