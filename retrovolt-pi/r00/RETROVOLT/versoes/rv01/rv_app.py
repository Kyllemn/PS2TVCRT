#!/usr/bin/env python3
"""RETROVOLT Pi - R01: convergencia, controle e arquivo de configuracao.

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
import threading
import time

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

W, H = 320, 240
NAV = (12, 18, 40)
ORANGE = (255, 140, 40)
IVORY = (240, 236, 225)
SLATE = (120, 130, 160)
GREEN = (90, 220, 140)
RED = (230, 110, 90)
LINE = (40, 50, 80)

VERSAO = os.environ.get('RV_VERSAO', 'rv01')
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
            if o < 0 or o > 94:
                continue
            for r in range(8):
                b = FONT[o * 8 + r]
                yy = y + r
                if not 0 <= yy < H:
                    continue
                for k in range(8):
                    if b >> k & 1:
                        xx = x + n * 8 + k
                        if 0 <= xx < W:
                            self.px[yy * W + xx] = c

    def rtext(self, x, y, s, c):
        self.text(x - len(s) * 8, y, s, c)

    def ctext(self, y, s, c):
        self.text((W - len(s) * 8) // 2, y, s, c)


def moldura(c, titulo, rodape_d):
    c.rect(0, 0, 6, H, ORANGE)
    c.rect(W - 6, 0, 6, H, ORANGE)
    c.text(16, 6, 'RETROVOLT', IVORY)
    c.rtext(W - 12, 6, time.strftime('%H:%M'), IVORY)
    c.text(16, 28, titulo, IVORY)
    c.rect(16, 38, len(titulo) * 8, 2, ORANGE)
    c.rect(16, 48, W - 32, 1, LINE)
    c.text(16, H - 12, VERSAO.upper(), SLATE)
    c.rtext(W - 12, H - 12, rodape_d, SLATE)


def item(c, x, y, s, on, valor=None):
    c.text(x, y, s, IVORY if on else SLATE)
    if on:
        c.rect(x - 8, y - 1, 3, 10, ORANGE)
        c.rect(x, y + 10, len(s) * 8, 1, ORANGE)
        c.text(x + len(s) * 8 + 8, y, '->', ORANGE)
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
        """conv = (posicao_x, posicao_y, largura). Fora do retangulo da imagem: preto."""
        bpp = self.bpp // 8
        preto = self.pixel((0, 0, 0))
        if self.tubo:
            sy = max(1, self.h // H)
            cw = max(W, min(self.w, round(self.w * conv[2])))
            x0 = (self.w - cw) // 2 + conv[0]
            y0 = (self.h - H * sy) // 2 + conv[1] * sy
        else:
            s = max(1, min(self.w // W, self.h // H))
            cw, sy = W * s, s
            x0, y0 = (self.w - cw) // 2, (self.h - H * sy) // 2
        # quantos pontos da tela cada pixel logico ocupa (largura nao inteira)
        cont = [((i + 1) * cw) // W - (i * cw) // W for i in range(W)]
        cache = {}
        linhas = {}
        # recorte horizontal (imagem saindo da tela)
        corte_e = max(0, -x0) * bpp
        dest_x = max(0, x0)
        vazio = preto * self.w
        for y in range(self.h):
            off = y * self.stride
            self.mem[off:off + self.w * bpp] = vazio
        for y in range(H):
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
            for k in range(sy):
                yy = y0 + y * sy + k
                if 0 <= yy < self.h:
                    off = yy * self.stride + dest_x * bpp
                    self.mem[off:off + len(row)] = row
        if self.fake:
            from PIL import Image
            Image.frombuffer('RGBX', (self.w, self.h), bytes(self.mem), 'raw', 'BGRX', self.stride, 1).convert('RGB').save(self.fake)


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
        if nome in ('CIMA', 'BAIXO', 'ESQ', 'DIR', 'L1', 'R1'):
            self.segurando[nome] = time.monotonic() + self.REPETE_INI

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
                        if ev.code in (e.ABS_HAT0X, e.ABS_HAT0Y, e.ABS_X, e.ABS_Y):
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


# ---------------------------------------------------------------- telas
MENU = ['CONVERGENCIA', 'TESTE DO CONTROLE', 'INFO']


class App:
    def __init__(self):
        self.fb = FB()
        self.cfg = cfg_ler()
        self.conv = [num(self.cfg, 'video_posicao_x', int, -300, 300),
                     num(self.cfg, 'video_posicao_y', int, -20, 20),
                     num(self.cfg, 'video_largura', float, 0.70, 1.10)]
        self.fila = queue.Queue()
        self.ent = Entrada(self.fila)
        self.tela = os.environ.get('RV_TELA', 'menu')
        self.sel = 0
        self.info = info()
        self.salvo = 0
        self.bs = 0
        self.ultima_acao = ''

    def desenha(self):
        c = Canvas()
        t = self.tela
        if t == 'menu':
            moldura(c, 'TESTE R01', 'A ENTRAR')
            y = 64
            for n, s in enumerate(MENU):
                item(c, 24, y, s, n == self.sel)
                y += 20
            c.text(24, 150, 'CONTROLE:', SLATE)
            nomes = self.ent.nomes or ['NENHUM ENCONTRADO']
            for k, nm in enumerate(nomes[:3]):
                c.text(24, 162 + k * 10, nm.upper()[:34], IVORY if self.ent.nomes else RED)
        elif t == 'conv':
            self.desenha_conv(c)
        elif t == 'controle':
            moldura(c, 'TESTE DO CONTROLE', 'B 3X VOLTA')
            c.text(24, 64, 'APERTE OS BOTOES:', SLATE)
            c.text(24, 84, (self.ent.ultimo or '-').upper()[:34], GREEN)
            c.text(24, 110, 'ACOES (MAPEADAS):', SLATE)
            c.text(24, 122, (self.ultima_acao or '-'), ORANGE)
            c.text(24, 150, 'SAIR: APERTE B 3 VEZES', SLATE)
        elif t == 'info':
            moldura(c, 'INFO', 'B VOLTA')
            d = self.info
            linhas = [
                ('VERSAO', '%s (%s)' % (VERSAO.upper(), ORIGEM)),
                ('VIDEO', '%dX%d %s' % (self.fb.w, self.fb.h, 'TUBO' if self.fb.tubo else 'LCD')),
                ('TEMP', '%dC' % d['temp'] if d['temp'] is not None else '?'),
                ('RAM', '%.1f/%.1fGB' % d['ram'] if d['ram'] else '?'),
                ('REDE', d['ip'] or 'SEM REDE'),
                ('WI-FI', (d['wifi'] or 'SEM WI-FI')[:18]),
            ]
            y = 64
            for k, v in linhas:
                c.text(24, y, k, SLATE)
                c.text(96, y, v, IVORY)
                y += 16
        return c

    def desenha_conv(self, c):
        # moldura de 1 pixel em toda a borda + barras grossas nos 4 cantos
        c.rect(0, 0, W, 1, ORANGE); c.rect(0, H - 1, W, 1, ORANGE)
        c.rect(0, 0, 1, H, ORANGE); c.rect(W - 1, 0, 1, H, ORANGE)
        for x, y in ((0, 0), (W - 24, 0), (0, H - 6), (W - 24, H - 6)):
            c.rect(x, y, 24, 6, ORANGE)
        for x, y in ((0, 0), (W - 6, 0), (0, H - 24), (W - 6, H - 24)):
            c.rect(x, y, 6, 24, ORANGE)
        # cruz no centro
        c.rect(W // 2 - 10, H // 2, 21, 1, IVORY); c.rect(W // 2, H // 2 - 10, 1, 21, IVORY)
        c.ctext(40, 'CONVERGENCIA', IVORY)
        c.ctext(56, 'AJUSTE ATE VER OS 4 CANTOS', SLATE)
        c.ctext(66, 'LARANJA E A BORDA INTEIRA', SLATE)
        x, y, w = self.conv
        c.ctext(150, 'X %+d   Y %+d   LARGURA %d%%' % (x, y, round(w * 100)), ORANGE)
        c.ctext(170, 'SETAS: POSICAO', SLATE)
        c.ctext(180, 'L1/R1: LARGURA', SLATE)
        c.ctext(190, 'SELECT: PADRAO', SLATE)
        c.ctext(204, 'A: SALVAR   B: VOLTAR', IVORY)
        if time.monotonic() < self.salvo:
            c.ctext(120, 'SALVO!', GREEN)

    def acao(self, a):
        t = self.tela
        if t == 'menu':
            if a == 'CIMA':
                self.sel = (self.sel - 1) % len(MENU)
            elif a == 'BAIXO':
                self.sel = (self.sel + 1) % len(MENU)
            elif a == 'A':
                self.tela = ['conv', 'controle', 'info'][self.sel]
                if self.tela == 'conv':
                    self.conv_antes = list(self.conv)
                if self.tela == 'info':
                    self.info = info()
        elif t == 'conv':
            x, y, w = self.conv
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
            elif a == 'SELECT':
                x, y, w = 0, 0, 1.0
            elif a == 'A':
                self.cfg.update(video_posicao_x=str(x), video_posicao_y=str(y), video_largura='%.3f' % w)
                cfg_gravar(self.cfg)
                self.conv_antes = [x, y, w]      # B passa a voltar para o que foi salvo
                self.salvo = time.monotonic() + 2
            elif a == 'B':
                x, y, w = self.conv_antes
                self.tela = 'menu'
            self.conv = [max(-300, min(300, x)), max(-20, min(20, y)), max(0.70, min(1.10, round(w, 3)))]
        elif t == 'controle':
            self.ultima_acao = a
            self.bs = self.bs + 1 if a == 'B' else 0
            if self.bs >= 3:
                self.bs = 0
                self.tela = 'menu'
        elif t == 'info':
            if a == 'B':
                self.tela = 'menu'

    def run(self):
        fake = self.fb.fake
        if not fake:
            tty_graphics(True)
            self.ent.start()
            threading.Thread(target=edid_real, daemon=True).start()
        try:
            self.fb.show(self.desenha(), self.conv)
            if fake:
                return
            os.makedirs(os.path.dirname(OK_FILE), exist_ok=True)
            open(OK_FILE, 'w').write(VERSAO + '\n')
            while True:
                try:
                    a = self.fila.get(timeout=1.0)
                except queue.Empty:
                    a = None
                if a and a != '_raw':
                    self.acao(a)
                # junta eventos acumulados antes de redesenhar (desenho leva ~0,1 s)
                try:
                    while True:
                        b = self.fila.get_nowait()
                        if b != '_raw':
                            self.acao(b)
                except queue.Empty:
                    pass
                self.fb.show(self.desenha(), self.conv)
        finally:
            if not fake:
                tty_graphics(False)


if __name__ == '__main__':
    App().run()
