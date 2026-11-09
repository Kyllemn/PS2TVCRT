#!/usr/bin/env python3
"""RETROVOLT Pi - R00: tela de teste da base do sistema.

Desenha direto no framebuffer (/dev/fb0), sem X nem Wayland, numa tela logica de
320x240 ampliada para o modo de video atual (1920x240 no tubo = 6x na horizontal).
Mostra versao, origem (pendrive/cartao), IP, Wi-Fi, temperatura, memoria e o modo
de video. Ao desenhar a primeira tela, grava /run/retrovolt/ok (o lancador usa
isso para saber que a versao funciona).

Teste fora do Pi:  RV_FAKEFB=saida.png:1920x240 python3 rv_app.py
"""
import fcntl
import mmap
import os
import socket
import subprocess
import sys
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

VERSAO = os.environ.get('RV_VERSAO', 'rv00')
ORIGEM = os.environ.get('RV_ORIGEM', 'CARTAO')
OK_FILE = '/run/retrovolt/ok'


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
    try:   # Pi OS trixie usa NetworkManager
        for l in subprocess.run(['nmcli', '-t', '-f', 'active,ssid', 'dev', 'wifi'], capture_output=True, text=True, timeout=3).stdout.splitlines():
            if l.startswith('yes:'):
                d['wifi'] = l[4:] or None
    except Exception:
        pass
    if not d['wifi']:
        try:
            d['wifi'] = subprocess.run(['iwgetid', '-r'], capture_output=True, text=True, timeout=2).stdout.strip() or None
        except Exception:
            pass
    try:
        d['modelo'] = open('/proc/device-tree/model').read().strip('\x00').upper().replace('RASPBERRY ', '')
    except Exception:
        d['modelo'] = 'PC (TESTE)'
    return d


def draw(fbw, fbh, d):
    c = Canvas()
    c.rect(0, 0, 6, H, ORANGE)
    c.rect(W - 6, 0, 6, H, ORANGE)
    c.text(16, 6, 'RETROVOLT', IVORY)
    c.rtext(W - 12, 6, time.strftime('%H:%M'), IVORY)
    c.text(16, 28, 'BASE DO SISTEMA', IVORY)
    c.rect(16, 38, 15 * 8, 2, ORANGE)
    c.rect(16, 48, W - 32, 1, (40, 50, 80))
    linhas = [
        ('VERSAO', '%s  (%s)' % (VERSAO.upper(), ORIGEM)),
        ('PLACA', (d['modelo'] or '?')[:22]),
        ('VIDEO', '%dX%d' % (fbw, fbh)),
        ('TEMP', '%dC' % d['temp'] if d['temp'] is not None else '?'),
        ('RAM', '%.1f/%.1fGB' % d['ram'] if d['ram'] else '?'),
        ('REDE', d['ip'] or 'SEM REDE'),
        ('WI-FI', (d['wifi'] or 'SEM WI-FI')[:18]),
    ]
    y = 60
    for k, v in linhas:
        c.text(24, y, k, SLATE)
        c.text(96, y, v, IVORY if v not in ('?', 'SEM REDE', 'SEM WI-FI') else RED)
        y += 16
    c.text(24, 182, 'SE VOCE ESTA VENDO ISTO,', GREEN)
    c.text(24, 194, 'A BASE FUNCIONOU.', GREEN)
    c.text(16, H - 12, 'LOG: RETROVOLT/LOG.TXT', SLATE)
    # barras laranja nos 4 cantos: mostram onde a imagem comeca e termina
    for (x, y) in ((0, 0), (W - 16, 0), (0, H - 4), (W - 16, H - 4)):
        c.rect(x, y, 16, 4, ORANGE)
    return c


class FB:
    """framebuffer real (/dev/fb0) ou falso (arquivo PNG para testar no PC)."""

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

    def pixel(self, c):
        r, g, b = c
        if self.bpp == 16:
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            return bytes((v & 0xff, v >> 8))
        return bytes((b, g, r, 0))

    def show(self, canvas):
        sx = max(1, self.w // W)
        sy = max(1, self.h // H)
        if self.w < self.h * 2.5:
            # LCD: mesma ampliacao nas duas direcoes. No tubo (super-resolucao
            # 1920x240/288/480i, tela muito larga) cada eixo amplia independente.
            sx = sy = min(sx, sy)
        ox = (self.w - W * sx) // 2
        oy = (self.h - H * sy) // 2
        bpp = self.bpp // 8
        cache = {}
        for y in range(H):
            row = bytearray()
            for x in range(W):
                c = canvas.px[y * W + x]
                p = cache.get(c)
                if p is None:
                    p = cache[c] = self.pixel(c) * sx
                row += p
            for k in range(sy):
                off = (oy + y * sy + k) * self.stride + ox * bpp
                self.mem[off:off + len(row)] = row
        if self.fake:
            from PIL import Image
            Image.frombuffer('RGBX', (self.w, self.h), bytes(self.mem), 'raw', 'BGRX', self.stride, 1).convert('RGB').save(self.fake)


def tty_graphics(on):
    """esconde o console de texto (KD_GRAPHICS) para ele nao escrever por cima"""
    try:
        fd = os.open('/dev/tty1', os.O_RDWR)
        fcntl.ioctl(fd, 0x4B3A, 1 if on else 0)
        os.close(fd)
    except Exception:
        pass


def main():
    fb = FB()
    if not fb.fake:
        tty_graphics(True)
    try:
        primeira = True
        while True:
            fb.show(draw(fb.w, fb.h, info()))
            if primeira:
                primeira = False
                if fb.fake:
                    return
                os.makedirs(os.path.dirname(OK_FILE), exist_ok=True)
                open(OK_FILE, 'w').write(VERSAO + '\n')
            time.sleep(5)
    finally:
        if not fb.fake:
            tty_graphics(False)


if __name__ == '__main__':
    main()
