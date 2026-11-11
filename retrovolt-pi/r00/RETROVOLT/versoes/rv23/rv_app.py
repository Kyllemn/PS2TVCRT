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

T0 = time.monotonic()


def tempo(etapa):
    """registra no log quanto tempo cada etapa da entrada levou"""
    print('tempo: %.2f s %s' % (time.monotonic() - T0, etapa), flush=True)


import disco
import shutil
import imagens
import tv
import video
import wifi

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
# o autor quer no maximo 5x8 (os negritos de 5 tem 6 de largura): as maiores ficam no
# fontes.py (e no rv-jogo) mas nao aparecem para escolher
FONTE_MAX_ALT, FONTE_MAX_LARG = 8, 6
FONTES_LISTA = [(n.upper(), cw, ch, bytes.fromhex(hx)) for n, (cw, ch, hx) in FONTES.items()
                if ch <= FONTE_MAX_ALT and cw <= FONTE_MAX_LARG]


class Fonte:
    """fonte em uso (troca em OPCOES > FONTE); todo o desenho usa F.cw / F.ch"""
    def usa(self, nome):
        for n, cw, ch, b in FONTES_LISTA:
            if n == nome.upper():
                self.nome, self.cw, self.ch, self.b = n, cw, ch, b
                return
        self.usa('5X7')


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

VERSAO = os.environ.get('RV_VERSAO', 'rv23')
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
    ('leitora_cache_dreamcast', '256', 'Dreamcast: MB de memoria guardando o que ja foi lido do disco (menos picotes)'),
    ('tubo_480i', 'SIM', 'tubo: jogo em 480i sai em 480i de verdade (SIM) ou sempre 240p (NAO)'),
    ('dreamcast_gpu', 'VULKAN', 'Dreamcast: VULKAN (padrao) ou GLES (reserva, se o Vulkan der problema)'),
    ('tv_brilho', '0', 'RetroTV: brilho da imagem, -50 a 50 (LT/RT na tela cheia)'),
    ('tv_troca_rapida', 'SIM', 'RetroTV: SIM = tela cheia abre por tras da previa (sem tela preta); NAO = jeito antigo'),
    ('LEITORA', None, None),
    ('leitora_velocidade', '4', 'velocidade da leitora nos jogos (0 = maxima; a leitora pode ter minimo proprio, ex.: 10x)'),
    ('leitora_acordar', '10', 'segundos sem leitura ate o RetroVolt acordar a leitora (2 a 60)'),
    ('JOGOS', None, None),
    ('pasta_jogos', 'pendrive', 'onde ficam jogos, BIOS e saves: pendrive ou cartao'),
    ('VIDEO', None, None),
    ('video_saida', 'AUTO', 'AUTO detecta pela EDID; TUBO ou LCD forca (so na tela em que foi escolhido)'),
    ('video_saida_tela', '', 'tela onde TUBO/LCD foi escolhido (preenchido sozinho; outra tela = detecta)'),
    ('video_modo', 'AUTO', 'resolucao do LCD: AUTO (nativa do monitor) ou LARGURAXALTURA, ex.: 1280X720'),
    ('video_filtro', 'NENHUM', 'so no LCD: NENHUM, SCANLINES, SCANLINES LEVES, CRT, SUAVE, SCALE2X, SCALE3X ou XBR (padrao; cada nucleo pode ter o seu)'),
    ('MENU', None, None),
    ('fonte', '5X7', 'fonte do menu: ' + ', '.join(f[0] for f in FONTES_LISTA)),
    ('menu_desce', '18', 'quanto o menu (nome RETROVOLT, relogio, abas) desce, em pontos do menu (0 a 60)'),
    ('menu_logo_sobe', '16', 'quanto so a linha RETROVOLT/Wi-Fi/relogio sobe de volta, em pontos (0 a 60)'),
    ('menu_fundo_brilho', '45', 'brilho da imagem de fundo do menu, 0 a 100 (imagens em imagens/wallpapers)'),
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
FUNDO = None   # imagem de fundo do menu (lista de W*H_TELA cores), sorteada no inicio
MOSTRA_TARJAS = False   # tarjas laranja nas laterais: tiradas a pedido do autor (True volta)
# o menu desce TOPO pontos (menu_desce no retrovolt.cfg): o desenho usa W x H e a tela tem
# W x H_TELA; as TOPO linhas de cima mostram so o fundo. O rodape fica no mesmo lugar.
TOPO = 0
H_TELA = 240
LOGO_SOBE = 0   # menu_logo_sobe: quanto a linha RETROVOLT/Wi-Fi/relogio sobe (pontos)


class Canvas:
    def __init__(self, liso=False):
        self.topo = 0 if H + TOPO > H_TELA else TOPO    # tela inteira (convergencia): sem descer
        self.ht = H + self.topo                          # linhas da tela de verdade
        if FUNDO and not liso and len(FUNDO) == W * self.ht:
            self.acima = FUNDO[:self.topo * W]
            self.px = FUNDO[self.topo * W:]
        else:
            self.acima = [NAV] * (self.topo * W)
            self.px = [NAV] * (W * H)
        self.camada = None
        self.extras = []    # imagens desenhadas na resolucao do monitor (LCD), por cima

    def camada_topo(self):
        """camada da tela inteira (W x ht), transparente (None), so para a linha de cima"""
        if self.camada is None:
            self.camada = CamadaTopo(self.ht)
        return self.camada

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

    def text_esc(self, x, y, s, c, esc):
        """texto ampliado (esc x esc pontos por pixel da fonte)"""
        for n, ch in enumerate(s):
            o = ord(ch) - 32
            if o < 0 or o > 223:
                continue
            for r in range(F.ch):
                b = F.b[o * F.ch + r]
                for k in range(F.cw):
                    if b >> k & 1:
                        self.rect(x + (n * F.cw + k) * esc, y + r * esc, esc, esc, c)

    def desenho(self, x, y, linhas, c):
        for j, l in enumerate(linhas):
            for i, ch in enumerate(l):
                if ch == '#' and 0 <= x + i < W and 0 <= y + j < H:
                    self.px[(y + j) * W + x + i] = c

    def text_cortado(self, x, y, s, c, xa, xb):
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
                        if xa <= xx < xb and 0 <= xx < W:
                            self.px[yy * W + xx] = c

    def rtext(self, x, y, s, c):
        self.text(x - len(s) * F.cw, y, s, c)

    def ctext(self, y, s, c):
        self.text((W - len(s) * F.cw) // 2, y, s, c)


RAIO = ['.....####', '....####.', '....###..', '...####..', '...###...', '..#######',
        '..######.', '.....###.', '....###..', '....##...', '...##....', '...#.....', '..#......']
ICONE_WIFI = ['...#####...', '.##.....##.', '#...###...#', '..##...##..', '....###....', '.....#.....']
AMARELO = (255, 196, 40)


def logo(c, x, y):
    """RETRO (raio) VOLT, em tamanho maior: e a marca"""
    esc = 2 if F.ch <= 10 else 1
    c.text_esc(x, y, 'RETRO', IVORY, esc)
    xr = x + 5 * F.cw * esc - 3                    # raio colado no O ...
    for j, l in enumerate(RAIO):                    # raio ampliado na altura do texto
        for i, ch in enumerate(l):
            if ch == '#':
                c.rect(xr + i, y + j * F.ch * esc // len(RAIO) + 1, 1, max(1, F.ch * esc // len(RAIO) + 1), AMARELO)
    c.text_esc(xr + 10, y, 'VOLT', ORANGE, esc)       # ... e no V


def topo_direita(c, wifi, y0=3):
    hora = time.strftime('%H:%M')
    yy = y0 + (F.ch * (2 if F.ch <= 10 else 1) - F.ch) // 2
    c.rtext(W - 12, yy, hora, IVORY)
    if wifi is False:        # tela sem wi-fi no topo
        return
    nome = (wifi or 'SEM WI-FI')[:14].upper()
    xn = W - 12 - (len(hora) + 2) * F.cw - len(nome) * F.cw
    c.text(xn, yy, nome, GREEN if wifi else SLATE)
    c.desenho(xn - 14, yy + (F.ch - 6) // 2, ICONE_WIFI, GREEN if wifi else SLATE)


def linha_topo(c, wifi):
    """RETROVOLT + Wi-Fi + relogio. Com o menu descido (menu_desce), essa linha pode
    subir sozinha (menu_logo_sobe, em pontos): e desenhada numa camada da tela inteira,
    por cima do fundo, enquanto as abas e o resto ficam onde estao."""
    if c.topo and LOGO_SOBE:
        y = max(2, c.topo + 3 - LOGO_SOBE)
        alvo = c.camada_topo()
    else:
        y, alvo = 3, c
    logo(alvo, 16, y)
    topo_direita(alvo, wifi, y)


def moldura(c, titulo, rodape_d):
    if MOSTRA_TARJAS:
        c.rect(0, 0, 6, H, ORANGE)
        c.rect(W - 6, 0, 6, H, ORANGE)
    linha_topo(c, False)
    c.text(16, 30, titulo, IVORY)
    c.rect(16, 30 + F.ch + 2, len(titulo) * F.cw, 2, ORANGE)
    c.rect(16, 30 + F.ch + 8, W - 32, 1, LINE)
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
class CamadaTopo(Canvas):
    """mesmos desenhos do Canvas, em coordenadas da tela inteira; None = transparente"""
    def __init__(self, ht):
        self.px = [None] * (W * ht)
        self.extras = []
        self.topo, self.ht, self.camada = 0, ht, None


class FB:
    trava = threading.Lock()
    geo = None

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
        HT, topo = canvas.ht, canvas.topo                 # linhas da tela (com o topo que desce)
        if self.tubo:
            linhas_por = max(1, self.h // HT)             # 480i: 2 linhas por linha logica
            cw = max(W, min(self.w, round(self.w * conv[2])))
            # altura menor que 100%: o menu ja vem desenhado com essas linhas (ajusta_altura),
            # uma linha do desenho = uma linha do tubo. Antes eram puladas linhas e as letras
            # perdiam um traco ("SEGA CD" virava "SFGA CN").
            ch = HT * linhas_por if conv[3] < 1 else round(HT * linhas_por * conv[3])
            x0 = (self.w - cw) // 2 + conv[0]
            y0 = (self.h - ch) // 2 + conv[1] * linhas_por
        else:
            s = max(1, min(self.w // W, self.h // HT))
            cw, ch = W * s, HT * s
            x0, y0 = (self.w - cw) // 2, (self.h - ch) // 2
        cont = [((i + 1) * cw) // W - (i * cw) // W for i in range(W)]
        cache = {}
        linhas = {}
        por_y = {}
        corte_e = max(0, -x0) * bpp
        dest_x = max(0, x0)
        linha_preta = preto * (self.stride // bpp)
        frame = bytearray(linha_preta * self.h)
        for j in range(ch):
            yy = y0 + j
            if not 0 <= yy < self.h:
                continue
            y = (2 * j + 1) * HT // (2 * ch)                # linha logica (centro): 1a e ultima sempre aparecem
            row = por_y.get(y)
            if row is not None:
                off = yy * self.stride + dest_x * bpp
                frame[off:off + len(row)] = row
                continue
            linha = canvas.acima[y * W:(y + 1) * W] if y < topo else canvas.px[(y - topo) * W:(y - topo + 1) * W]
            cam = canvas.camada.px[y * W:(y + 1) * W] if canvas.camada is not None else None
            if cam and any(v is not None for v in cam):
                linha = [a if a is not None else b for a, b in zip(cam, linha)]
            chave = tuple(linha)
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
            por_y[y] = row
            off = yy * self.stride + dest_x * bpp
            frame[off:off + len(row)] = row
        if not self.tubo:
            for e in canvas.extras:
                self._imagem(frame, e, x0, y0 + topo * s, s)
        # geometria desta tela (a previa da TV desenha direto na caixa, no lugar certo)
        if self.tubo:
            self.geo = (x0, y0, cw / float(W), ch / float(HT), topo)
        else:
            self.geo = (x0, y0, float(s), float(s), topo)
        self.espera_vsync()
        with self.trava:
            self.mem[0:len(frame)] = frame
        if self.fake:
            from PIL import Image
            Image.frombuffer('RGBX', (self.w, self.h), bytes(self.mem), 'raw', 'BGRX', self.stride, 1).convert('RGB').save(self.fake)

    _linhas_img = {}

    def _imagem(self, frame, e, x0, y0, s):
        """imagem ja reduzida para o tamanho em pontos do monitor, centralizada na caixa
        (lx, ly, lw, lh) da tela logica; circulo=True recorta em disco com furo"""
        lx, ly, lw, lh, m, circulo = e
        w, h, rgb = m
        bpp = self.bpp // 8
        chave = (id(m), circulo, self.bpp)
        linhas = self._linhas_img.get(chave)
        if linhas is None:
            # cada linha: lista de trechos (x, bytes); fora do disco e o furo ficam
            # sem desenhar (aparece o fundo do menu)
            aro = self.pixel((200, 205, 215))
            r2 = (min(w, h) / 2.0) ** 2
            f2 = (min(w, h) / 9.0) ** 2
            f2b = (min(w, h) / 9.0 - 2) ** 2
            linhas = []
            for y in range(h):
                trechos, partes, x_ini = [], [], None
                for x in range(w):
                    p = None
                    if circulo:
                        dx, dy = x - w / 2.0 + 0.5, y - h / 2.0 + 0.5
                        d2 = dx * dx + dy * dy
                        if d2 > r2 or d2 <= f2b:
                            p = False
                        elif d2 <= f2:
                            p = aro
                    if p is None:
                        o = (y * w + x) * 3
                        p = self.pixel((rgb[o], rgb[o + 1], rgb[o + 2]))
                    if p is False:
                        if partes:
                            trechos.append((x_ini, b''.join(partes)))
                            partes = []
                        continue
                    if not partes:
                        x_ini = x
                    partes.append(p)
                if partes:
                    trechos.append((x_ini, b''.join(partes)))
                linhas.append(trechos)
            if len(self._linhas_img) > 12:
                self._linhas_img.clear()
            self._linhas_img[chave] = linhas
        ox = x0 + lx * s + (lw * s - w) // 2
        oy = y0 + ly * s + (lh * s - h) // 2
        for y, trechos in enumerate(linhas):
            yy = oy + y
            if 0 <= yy < self.h and 0 <= ox and ox + w <= self.w:
                for x, row in trechos:
                    off = yy * self.stride + (ox + x) * bpp
                    frame[off:off + len(row)] = row

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
        self.modo_texto = False     # teclado USB digitando (senha do Wi-Fi)
        self.ja_procurou = False
        self.select_desde = 0
        self.shift = False
        self.caps = False
        self.layout = 'br'
        try:
            for l in open('/etc/default/keyboard'):
                if l.startswith('XKBLAYOUT='):
                    self.layout = l.split('=', 1)[1].strip().strip('"').split(',')[0] or 'br'
        except OSError:
            pass
        try:
            import evdev
            self.evdev = evdev
            self.e = evdev.ecodes
        except ImportError:
            self.evdev = None
            print('AVISO: python3-evdev ausente: sem controle', flush=True)

    def tecla_texto(self, code):
        """caractere da tecla (layout br ABNT2 ou us), ou None"""
        e = self.e
        letras = 'QWERTYUIOPASDFGHJKLZXCVBNM'
        for ch in letras:
            if code == getattr(e, 'KEY_' + ch):
                maius = self.shift != self.caps
                return ch if maius else ch.lower()
        dig = '1234567890'
        sh_us = '!@#$%^&*()'
        sh_br = '!@#$%\u00a8&*()'
        for i, ch in enumerate(dig):
            if code == getattr(e, 'KEY_' + ch):
                return (sh_br if self.layout == 'br' else sh_us)[i] if self.shift else ch
        if code == e.KEY_SPACE:
            return ' '
        if self.layout == 'br':
            t = {e.KEY_GRAVE: "'\"", e.KEY_MINUS: '-_', e.KEY_EQUAL: '=+', e.KEY_LEFTBRACE: '`^',
                 e.KEY_RIGHTBRACE: '[{', e.KEY_SEMICOLON: '\u00e7\u00c7', e.KEY_APOSTROPHE: '~^',
                 e.KEY_BACKSLASH: ']}', e.KEY_102ND: '\\|', e.KEY_COMMA: ',<', e.KEY_DOT: '.>',
                 e.KEY_SLASH: ';:', e.KEY_RO: '/?'}
        else:
            t = {e.KEY_GRAVE: '`~', e.KEY_MINUS: '-_', e.KEY_EQUAL: '=+', e.KEY_LEFTBRACE: '[{',
                 e.KEY_RIGHTBRACE: ']}', e.KEY_SEMICOLON: ';:', e.KEY_APOSTROPHE: "'\"",
                 e.KEY_BACKSLASH: '\\|', e.KEY_COMMA: ',<', e.KEY_DOT: '.>', e.KEY_SLASH: '/?'}
        if code in t:
            return t[code][1 if self.shift else 0]
        return None

    def texto(self, ev):
        """modo texto: teclado vira digitacao. True = evento usado"""
        e = self.e
        if ev.code in (e.KEY_LEFTSHIFT, e.KEY_RIGHTSHIFT):
            self.shift = ev.value != 0
            return True
        if ev.value != 1 and ev.value != 2:
            return ev.code in (e.KEY_ENTER, e.KEY_BACKSPACE, e.KEY_ESC) or self.tecla_texto(ev.code) is not None
        if ev.code == e.KEY_CAPSLOCK:
            self.caps = not self.caps
            return True
        if ev.code in (e.KEY_ENTER, e.KEY_KPENTER):
            self.fila.put('_ok')
            return True
        if ev.code == e.KEY_BACKSPACE:
            self.fila.put('_apaga')
            return True
        if ev.code == e.KEY_ESC:
            self.fila.put('_sai')
            return True
        ch = self.tecla_texto(ev.code)
        if ch is not None:
            self.fila.put('_txt:' + ch)
            return True
        return False

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
                keys = caps.get(self.e.EV_KEY, [])
                if self.ja_procurou and (self.e.BTN_SOUTH in keys or self.e.BTN_GAMEPAD in keys):
                    self.fila.put('_controle:' + d.name)
            except OSError:
                pass
        self.nomes = [d.name for d in self.devs.values()]
        self.ja_procurou = True

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
                        if self.modo_texto and ev.code < 0x100 and self.texto(ev):
                            continue
                        nome = mp.get(ev.code)
                        if nome == 'SELECT':
                            self.select_desde = time.monotonic() if ev.value else 0
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
    d['mascara'] = d['gateway'] = None
    try:
        for l in subprocess.run(['ip', '-o', '-4', 'addr', 'show', 'scope', 'global'], capture_output=True, text=True, timeout=2).stdout.splitlines():
            pre = int(l.split()[3].split('/')[1])
            m = (0xffffffff << (32 - pre)) & 0xffffffff
            d['mascara'] = '.'.join(str(m >> s & 255) for s in (24, 16, 8, 0))
            break
        r = subprocess.run(['ip', 'route', 'show', 'default'], capture_output=True, text=True, timeout=2).stdout.split()
        if 'via' in r:
            d['gateway'] = r[r.index('via') + 1]
    except Exception:
        pass
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
# pasta em roms/ -> (nome na tela, nucleo, tem CD). A ordem e a ordem do menu.
# Um nucleo que roda varios consoles (Genesis Plus GX: Mega Drive, Master System, Game Gear,
# SG-1000, Sega CD) tem um sistema para cada console, cada um com a sua pasta. Sistema de
# cartucho so aparece com jogos na pasta; sistema de CD com nucleo aparece sempre (INICIAR CD).
SISTEMAS = {
    'ps1': ('PLAYSTATION', 'pcsx_rearmed_libretro.so', True),
    'segacd': ('SEGA CD', 'genesis_plus_gx_libretro.so', True),
    'neogeocd': ('NEO GEO CD', 'neocd_libretro.so', True),
    'pcengine': ('PC ENGINE', 'mednafen_pce_fast_libretro.so', True),
    'pcenginecd': ('PC ENGINE CD', 'mednafen_pce_fast_libretro.so', True),
    'megadrive': ('MEGA DRIVE', 'genesis_plus_gx_libretro.so', False),
    '32x': ('32X', 'picodrive_libretro.so', False),
    'mastersystem': ('MASTER SYSTEM', 'genesis_plus_gx_libretro.so', False),
    'gamegear': ('GAME GEAR', 'genesis_plus_gx_libretro.so', False),
    'sg1000': ('SG-1000', 'genesis_plus_gx_libretro.so', False),
    'snes': ('SUPER NINTENDO', 'snes9x_libretro.so', False),
    # Dreamcast: Flycast precisa de placa de video (OpenGL/Vulkan) -> fase GPU do rv-jogo.
    # Disco: CD-R gravado (selfboot, 2 sessoes) a leitora le; GD-ROM original nao.
    'dreamcast': ('DREAMCAST', 'flycast_libretro.so', True),
    'saturn': ('SATURN', None, True),
    '3do': ('3DO', None, True),
    'atari2600': ('ATARI 2600', None, False), 'nes': ('NES', None, False),
    'n64': ('NINTENDO 64', None, False),
    'gb': ('GAME BOY', None, False), 'gbc': ('GAME BOY COLOR', None, False),
    'gba': ('GAME BOY ADVANCE', None, False),
    'neogeo': ('NEO GEO', None, False), 'arcade': ('ARCADE', None, False),
}
EXT_JOGO = {
    'ps1': ('.cue', '.chd', '.pbp', '.m3u', '.iso', '.img', '.bin', '.exe', '.mdf', '.toc', '.cbn'),
    'pcengine': ('.pce', '.cue', '.chd', '.ccd', '.toc', '.m3u'),
    'segacd': ('.cue', '.chd', '.iso', '.m3u'),
    'neogeocd': ('.cue', '.chd'),
    'megadrive': ('.md', '.gen', '.smd', '.bin', '.68k', '.sgd', '.mdx', '.bms'),
    '32x': ('.32x', '.bin'),
    'mastersystem': ('.sms',),
    'gamegear': ('.gg',),
    'sg1000': ('.sg',),
    'snes': ('.sfc', '.smc', '.fig', '.swc', '.bs'),
    'dreamcast': ('.cdi', '.gdi', '.chd', '.cue', '.m3u'),
}
# PC Engine: uma lista so (cartoes e CDs). "pcenginecd" e o nome do disco na leitora e
# da pasta antiga roms/pcenginecd, que entra na mesma lista.
APELIDO = {'pcenginecd': 'pcengine'}
PASTAS_EXTRAS = {'pcengine': ['pcenginecd']}
EXT_CD = ('.cue', '.chd', '.ccd', '.m3u', '.iso', '.img', '.bin', '.pbp')
# BIOS que o sistema precisa (sem ela o jogo nem abre): nome do arquivo em RETROVOLT/bios/
BIOS_OBRIGATORIA = {'pcengine': 'syscard3.pce'}   # so para jogos de CD
# opcoes de nucleo que valem desde o primeiro jogo (so se o arquivo .opt ainda nao existe;
# depois manda o que a pessoa escolher em OPCOES DO NUCLEO)
OPCOES_PADRAO = {
    # Neo Geo CD: regiao USA (textos em ingles; Japan e o padrao do nucleo) e o atalho da
    # BIOS que encurta a espera de leitura
    'neocd_libretro.so': {'neocd_region': 'USA', 'neocd_cdspeedhack': 'On'},
    # Dreamcast: resolucao nativa (640x480); no tubo cada linha vai para a TV
    'flycast_libretro.so': {'reicast_internal_resolution': '640x480'},
}
# Sega CD: uma BIOS por regiao do disco (o nucleo escolhe pela regiao gravada no disco)
BIOS_SEGACD = {'U': ('bios_CD_U.bin', 'AMERICANO'), 'E': ('bios_CD_E.bin', 'EUROPEU'),
               'J': ('bios_CD_J.bin', 'JAPONES')}


# BIOS: RETROVOLT/bios/<sistema>/ (o mesmo nome da pasta em roms/). Arquivo solto em
# RETROVOLT/bios/ ainda vale (versoes antigas), mas a subpasta do sistema tem preferencia.
# Nomes exatos que os nucleos procuram (no cartao a caixa conta; no pendrive nao):
BIOS_NOMES = ['bios_CD_U.bin', 'bios_CD_E.bin', 'bios_CD_J.bin', 'bios_MD.bin', 'bios_U.sms', 'bios_E.sms',
              'bios_J.sms', 'bios.gg', 'syscard3.pce', 'syscard2.pce', 'syscard1.pce', 'gexpress.pce',
              'scph1001.bin', 'scph5500.bin', 'scph5501.bin', 'scph5502.bin', 'scph7001.bin', 'scph101.bin']
PASTA_BIOS_ANTIGA = {'neogeocd': 'neocd'}       # a R19 criou bios/neocd


def bios_arquivos(base_rv, sistema):
    """{nome: caminho} das BIOS deste sistema (subpasta > pasta antiga > solta em bios/)"""
    raiz = os.path.join(base_rv, 'bios')
    achados = {}
    pastas = [raiz]
    if sistema in PASTA_BIOS_ANTIGA:
        pastas.append(os.path.join(raiz, PASTA_BIOS_ANTIGA[sistema]))
    pastas.append(os.path.join(raiz, sistema))
    for pasta in pastas:                   # a ultima (subpasta do sistema) ganha
        try:
            for n in os.listdir(pasta):
                p = os.path.join(pasta, n)
                if not n.startswith('.') and os.path.isfile(p):
                    achados[n] = p
        except OSError:
            pass
    return achados


def bios_tem(arqs, nome):
    return any(n.lower() == nome.lower() for n in arqs)


def bios_pasta_nucleo(base_rv, sistema):
    """pasta de sistema do nucleo: /run/retrovolt/bios/<sistema> com atalhos para as BIOS
    (com o nome exato que o nucleo procura). Neo Geo CD: o NeoCD procura em neocd/."""
    destino = os.path.join('/run/retrovolt/bios', sistema)
    arqs = bios_arquivos(base_rv, sistema)
    try:
        shutil.rmtree(destino, ignore_errors=True)
        os.makedirs(destino)
        if sistema == 'dreamcast':
            # Flycast: BIOS em dc/ e ele GRAVA ali a memoria do console (dc_nvmem.bin: data,
            # idioma). dc/ aponta direto para RETROVOLT/bios/dreamcast no pendrive, para nao
            # se perder ao desligar (no pendrive a caixa das letras nao conta)
            pasta = os.path.join(base_rv, 'bios', 'dreamcast')
            os.makedirs(pasta, exist_ok=True)
            os.symlink(pasta, os.path.join(destino, 'dc'))
            return destino
        alvo = os.path.join(destino, 'neocd') if sistema == 'neogeocd' else destino
        os.makedirs(alvo, exist_ok=True)
        exatos = {n.lower(): n for n in BIOS_NOMES}
        for n, p in arqs.items():
            os.symlink(p, os.path.join(alvo, n))
            certo = exatos.get(n.lower())
            if certo and certo != n:
                os.symlink(p, os.path.join(alvo, certo))
        return destino
    except OSError as ex:
        print('bios: nao montou %s (%s); usando a pasta do pendrive' % (destino, ex), flush=True)
        return os.path.join(base_rv, 'bios', sistema)


def bios_falta(base_rv, sistema, info, e_cd):
    """mensagem se falta a BIOS deste jogo, ou None"""
    arqs = bios_arquivos(base_rv, sistema)
    onde = 'RETROVOLT/BIOS/%s' % sistema.upper()
    if sistema == 'segacd':
        reg = (info or {}).get('regiao')
        if reg in BIOS_SEGACD:
            nome, regiao = BIOS_SEGACD[reg]
            if not bios_tem(arqs, nome):
                return 'FALTA A BIOS %s (SEGA CD %s) EM %s' % (nome.upper(), regiao, onde)
            return None
        # regiao desconhecida (ex.: .chd): basta uma delas
        if not any(bios_tem(arqs, n) for n, _ in BIOS_SEGACD.values()):
            return 'FALTA A BIOS DO SEGA CD (BIOS_CD_U.BIN, _E OU _J) EM %s' % onde
        return None
    precisa = BIOS_OBRIGATORIA.get(sistema)
    if precisa and e_cd and not bios_tem(arqs, precisa):
        return 'FALTA A BIOS %s EM %s' % (precisa.upper(), onde)
    return None


NOME_SISTEMA = {k: v[0] for k, v in SISTEMAS.items()}
NOME_SISTEMA['audio'] = 'CD DE AUDIO'


BASE_CARTAO = os.path.join(DADOS, 'RETROVOLT')
AUDIO_PADRAO = 'hdmi:CARD=vc4hdmi0,DEV=0'      # o mesmo do rv-jogo


def tem_pendrive():
    return os.path.isdir(os.path.join(USB, 'RETROVOLT'))


def base(onde='pendrive'):
    """pasta RETROVOLT do dispositivo escolhido (OPCOES > PASTAS DE JOGOS: pendrive ou
    cartao); sem pendrive usa o cartao. Cria a estrutura se faltar (nunca apaga nada)"""
    b = os.environ.get('RV_BASE')
    if not b:
        b = os.path.join(USB, 'RETROVOLT') if onde != 'cartao' and tem_pendrive() else BASE_CARTAO
    for p in ('config', 'bios', 'roms', 'saves', 'states', 'tv', 'imagens/consoles', 'imagens/wallpapers',
              'capturas', 'logs'):
        os.makedirs(os.path.join(b, p), exist_ok=True)
    for s, (_, nucleo, _) in SISTEMAS.items():
        if s not in APELIDO:          # uma pasta de BIOS por sistema (pcenginecd usa a do pcengine)
            os.makedirs(os.path.join(b, 'bios', s), exist_ok=True)
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


GUARDA_DIR = '/usr/local/lib/retrovolt'
GUARDA_UNIDADE = '/etc/systemd/system/retrovolt-video.service'
GUARDA_TEXTO_UNIDADE = """[Unit]
Description=RETROVOLT: confere a tela (tubo/LCD) antes do menu
After=local-fs.target systemd-modules-load.service
Before=retrovolt.service

[Service]
Type=oneshot
ExecStart=/usr/bin/python3 /usr/local/lib/retrovolt/rv_video_guarda.py
TimeoutStartSec=45

[Install]
WantedBy=multi-user.target
"""


def instala_guarda_video():
    """instala/atualiza no cartao a guarda de video do boot (independe do pendrive e da
    versao do menu: roda antes do menu e corrige tubo/LCD). So grava o que mudou."""
    try:
        reg = '/var/lib/retrovolt/logs/video_guarda.txt'
        if os.path.exists(reg):
            for l in open(reg).read().splitlines()[-3:]:
                print(l, flush=True)
        mudou = False
        os.makedirs(GUARDA_DIR, exist_ok=True)
        for nome in ('video.py', 'rv_video_guarda.py'):
            novo = open(os.path.join(PASTA, nome), 'rb').read()
            dest = os.path.join(GUARDA_DIR, nome)
            try:
                if open(dest, 'rb').read() == novo:
                    continue
            except OSError:
                pass
            with open(dest + '.tmp', 'wb') as f:
                f.write(novo)
            os.replace(dest + '.tmp', dest)
            mudou = True
        try:
            unidade_ok = open(GUARDA_UNIDADE).read() == GUARDA_TEXTO_UNIDADE
        except OSError:
            unidade_ok = False
        if not unidade_ok:
            with open(GUARDA_UNIDADE, 'w') as f:
                f.write(GUARDA_TEXTO_UNIDADE)
            mudou = True
        habilitada = subprocess.run(['systemctl', 'is-enabled', 'retrovolt-video.service'],
                                    capture_output=True, text=True).stdout.strip() == 'enabled'
        if mudou or not habilitada:
            subprocess.run(['systemctl', 'daemon-reload'], capture_output=True)
            subprocess.run(['systemctl', 'enable', 'retrovolt-video.service'], capture_output=True)
            os.sync()
            print('video: guarda de video do boot instalada/atualizada no cartao', flush=True)
    except Exception as ex:
        print('video: nao instalou a guarda do boot: %s' % ex, flush=True)


def exe_rv_jogo():
    """caminho do rv-jogo pronto para rodar (pendrive sem permissao de execucao: copia)"""
    exe = os.path.join(PASTA, 'rv-jogo')
    if os.access(exe, os.X_OK):
        return exe
    try:
        os.makedirs('/run/retrovolt/bin', exist_ok=True)
        subprocess.run(['cp', exe, '/run/retrovolt/bin/rv-jogo'])
        os.chmod('/run/retrovolt/bin/rv-jogo', 0o755)
        return '/run/retrovolt/bin/rv-jogo'
    except OSError as ex:
        print('jogo: rv-jogo sem permissao:', ex, flush=True)
        return exe


def lista_jogos(b, sistema):
    out = []
    for pasta in [sistema] + PASTAS_EXTRAS.get(sistema, []):
        out += _lista_pasta(b, pasta, EXT_JOGO.get(sistema, ()))
    return sorted(out, key=lambda p: nome_limpo(p))


def _lista_pasta(b, pasta, exts):
    raiz = os.path.join(b, 'roms', pasta)
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


def espaco(pasta):
    try:
        u = shutil.disk_usage(pasta)
        return u.used // 1048576, u.total // 1048576
    except OSError:
        return None


def copia_sem_sobrescrever(de, para):
    """copia saves/states/config de uma base para outra; arquivo que ja existe fica"""
    n = 0
    for sub in ('saves', 'states', 'config'):
        raiz = os.path.join(de, sub)
        for pasta, _, arqs in os.walk(raiz):
            dest = os.path.join(para, sub, os.path.relpath(pasta, raiz))
            os.makedirs(dest, exist_ok=True)
            for a in arqs:
                d = os.path.join(dest, a)
                if not os.path.exists(d):
                    try:
                        shutil.copy2(os.path.join(pasta, a), d)
                        n += 1
                    except OSError as ex:
                        print('pastas: nao copiou', a, ex, flush=True)
    os.sync()
    return n


class Miniaturas(threading.Thread):
    """reduz capas/artes numa thread (no Pi leva ~1 s por imagem) e guarda em cache"""

    def __init__(self, fila, cache):
        super().__init__(daemon=True)
        self.fila, self.cache = fila, cache
        self.pronto, self.pedidos = {}, set()
        self.q = queue.Queue()

    def pega(self, path, bw, bh):
        k = (path, bw, bh)
        if k in self.pronto:
            return self.pronto[k]
        if k not in self.pedidos:
            self.pedidos.add(k)
            self.q.put(k)
        return None

    def run(self):
        while True:
            k = self.q.get()
            try:
                self.pronto[k] = imagens.reduz(k[0], k[1], k[2], self.cache)
            except Exception as ex:
                print('imagens: erro em %s: %s' % (k[0], ex), flush=True)
                self.pronto[k] = None
            if len(self.pronto) > 80:
                self.pronto.clear()
                self.pedidos.clear()
            self.fila.put('_img')


def reativa_dongle_8bitdo():
    """O receptor 8BitDo ligado desde o boot as vezes nao vira controle (precisa tirar e
    pôr de novo). Se ele estiver na USB e nenhum controle dele aparecer, faz um "tirar e
    pôr" por software (authorized 0 -> 1). Uma vez por boot."""
    # no modo XInput o receptor se apresenta como controle de Xbox (Microsoft, 045e)
    marca = '/run/retrovolt/dongle_reativado'
    if os.path.exists(marca):
        return
    time.sleep(8)
    fabs = (0x2dc8, 0x045e)
    try:
        import evdev
        tem_controle = False
        for p in evdev.list_devices():
            try:
                d = evdev.InputDevice(p)
                k = d.capabilities().get(evdev.ecodes.EV_KEY, [])
                if d.info.vendor in fabs and (evdev.ecodes.BTN_SOUTH in k or evdev.ecodes.BTN_GAMEPAD in k):
                    tem_controle = True
                d.close()
            except OSError:
                pass
        raiz = '/sys/bus/usb/devices'
        for n in sorted(os.listdir(raiz)):
            if ':' in n:
                continue
            try:
                ven = open(os.path.join(raiz, n, 'idVendor')).read().strip()
                pro = open(os.path.join(raiz, n, 'idProduct')).read().strip()
                nome = open(os.path.join(raiz, n, 'product')).read().strip() if os.path.exists(os.path.join(raiz, n, 'product')) else ''
                print('usb: %s %s:%s %s' % (n, ven, pro, nome), flush=True)
            except OSError:
                pass
        print('usb: controle 8BitDo/Xbox %s' % ('encontrado' if tem_controle else 'NAO encontrado'), flush=True)
        if tem_controle:
            return
        for n in os.listdir(raiz):
            if ':' in n:
                continue
            try:
                if int(open(os.path.join(raiz, n, 'idVendor')).read().strip(), 16) not in fabs:
                    continue
                prod = open(os.path.join(raiz, n, 'product')).read().strip() if os.path.exists(os.path.join(raiz, n, 'product')) else '?'
                print('usb: receptor 8BitDo (%s, %s) sem controle: religando por software' % (n, prod), flush=True)
                try:
                    open(os.path.join(raiz, n, 'power', 'control'), 'w').write('on')
                except OSError:
                    pass
                open(os.path.join(raiz, n, 'authorized'), 'w').write('0')
                time.sleep(1.5)
                open(os.path.join(raiz, n, 'authorized'), 'w').write('1')
            except OSError as ex:
                print('usb: nao religou %s: %s' % (n, ex), flush=True)
        os.makedirs('/run/retrovolt', exist_ok=True)
        open(marca, 'w').write('1')
    except Exception as ex:
        print('usb: erro', ex, flush=True)


# ---------------------------------------------------------------- leitora (thread)
class Leitora(threading.Thread):
    """vigia a leitora em segundo plano. Identifica o disco (pode girar), mas NUNCA
    abre jogo sozinho: so avisa a tela. Comeca 4 s depois do menu aparecer, para nao
    atrasar a entrada. O disco so "saiu" com 2 leituras seguidas sem disco (leitora
    dormindo as vezes responde "sem disco" e isso nao pode virar disco novo)."""

    def __init__(self, fila):
        super().__init__(daemon=True)
        self.fila = fila
        self.dev = None
        self.estado = 'procurando'
        self.info = None
        self.pausa = threading.Event()
        self.quieta = False       # True enquanto a tela de CONVERGENCIA esta aberta
        self.sem_disco = 0

    def run(self):
        fake = os.environ.get('RV_CD')
        if not fake:
            time.sleep(4)
        while True:
            if self.pausa.is_set() or self.quieta:
                time.sleep(0.5)
                continue
            if fake:
                if self.estado != 'ok':
                    self.dev = fake
                    self.info = disco.identifica_arquivo(fake)
                    self.estado = 'ok'
                    self.fila.put('_disco')
                time.sleep(1)
                continue
            devs = disco.leitoras()
            if not devs:
                novo = 'sem_leitora'
            else:
                self.dev = devs[0]
                novo = disco.estado(self.dev)
            if self.estado == 'ok' and novo != 'ok':
                if novo == 'lendo':                 # acordando: e o mesmo disco
                    novo = 'ok'
                else:
                    self.sem_disco += 1
                    if self.sem_disco < 2:          # confirma antes de dizer que saiu
                        time.sleep(1.5)
                        continue
            if novo == 'ok':
                self.sem_disco = 0
            if novo == 'ok' and self.estado != 'ok':
                self.estado = 'lendo'
                self.fila.put('_disco')
                t0 = time.monotonic()
                try:
                    self.info = disco.identifica_disco(self.dev)
                except Exception as ex:
                    print('leitora: erro ao ler o disco:', ex, flush=True)
                    self.info = {'sistema': None, 'nome': None, 'codigo': None, 'rotulo': None}
                print('leitora: disco %s (%.1f s)' % (self.info, time.monotonic() - t0), flush=True)
                self.estado = 'ok'
                self.fila.put('_disco')
            elif novo != 'ok' and novo != self.estado and not (novo == 'lendo' and self.estado == 'lendo'):
                if self.estado == 'ok':
                    print('leitora: disco saiu', flush=True)
                self.estado = novo
                self.info = None
                self.fila.put('_disco')
            time.sleep(1.5)


# ---------------------------------------------------------------- telas
ABAS = ['DISCO', 'JOGOS', 'TV', 'OPCOES']
OPCOES = ['VOLUME', 'VIDEO', 'CONVERGENCIA', 'FONTE', 'WI-FI', 'PASTAS DE JOGOS', 'TESTE DO CONTROLE', 'INFO', 'REINICIAR', 'DESLIGAR']
TECLAS = {False: ['1234567890', 'qwertyuiop', 'asdfghjkl@', 'zxcvbnm.-_'],
          True: ['!#$%&*()+=', 'QWERTYUIOP', 'ASDFGHJKL?', 'ZXCVBNM,:/']}
TECLAS_FIM = ['ABC', 'ESPACO', 'APAGAR', 'OK']


def cabecalho(c, aba, wifi, rodape_d):
    if MOSTRA_TARJAS:
        c.rect(0, 0, 6, H, ORANGE)
        c.rect(W - 6, 0, 6, H, ORANGE)
    linha_topo(c, wifi)
    x = 16
    for a in ABAS:
        on = a == aba
        c.text(x, 30, a, IVORY if on else SLATE)
        if on:
            c.rect(x, 30 + F.ch + 2, len(a) * F.cw, 2, ORANGE)
        x += (len(a) + 3) * F.cw
    c.rect(16, 30 + F.ch + 8, W - 32, 1, LINE)
    c.text(16, H - 4 - F.ch, 'L1/R1 ABAS', SLATE)
    c.rtext(W - 12, H - 4 - F.ch, rodape_d, SLATE)


AZUL_CAIXA = (24, 44, 110)


def caixa(c, linhas, cores=None):
    """popup de confirmacao no meio da tela: fundo azul com contorno branco"""
    w = max(len(l) for l in linhas) * F.cw + 24
    h = len(linhas) * (F.ch + 6) + 14
    x0, y0 = (W - w) // 2, (H - h) // 2
    c.rect(x0, y0, w, h, (255, 255, 255))
    c.rect(x0 + 1, y0 + 1, w - 2, h - 2, AZUL_CAIXA)
    for k, l in enumerate(linhas):
        c.ctext(y0 + 8 + k * (F.ch + 6), l, (cores or [IVORY] * len(linhas))[k])


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
        tempo('programa carregado')
        self.fb = FB()
        global W, H
        self.esc = None
        if not self.fb.tubo:
            # LCD (qualquer um): amplia um numero inteiro de vezes (nitido) e a tela logica
            # enche o monitor: 1280x720 -> 426x240 x3; 1440x900 -> 480x300 x3;
            # 1920x1080 -> 480x270 x4; 1024x768 -> 341x256 x3; 800x600 -> 400x300 x2;
            # 640x480 -> 320x240 x2 (CRT 31 kHz fica igual ao tubo)
            esc = max(1, self.fb.h // 240)
            while esc > 1 and self.fb.w // esc < 320:
                esc -= 1
            self.esc = esc
            H = self.fb.h // esc
            W = self.fb.w // esc
            print('tela: %dx%d, menu %dx%d ampliado %dx' % (self.fb.w, self.fb.h, W, H, esc), flush=True)
        self.cfg = cfg_ler()
        self.conv = [num(self.cfg, 'video_posicao_x', int, -300, 300),
                     num(self.cfg, 'video_posicao_y', int, -20, 20),
                     num(self.cfg, 'video_largura', float, 0.70, 1.10),
                     num(self.cfg, 'video_altura', float, 0.80, 1.10)]
        F.usa(self.cfg.get('fonte', '6X10'))
        global TOPO, H_TELA
        self.h_base = H
        H_TELA = H
        TOPO = num(self.cfg, 'menu_desce', int, 0, 60)
        global LOGO_SOBE
        LOGO_SOBE = num(self.cfg, 'menu_logo_sobe', int, 0, 60)
        H = H_TELA - TOPO
        self.ajusta_altura(fundo=False)
        self.video_modo_ok = self.cfg.get('video_modo', 'AUTO').upper()   # ultima confirmada
        self.fila = queue.Queue()
        self.ent = Entrada(self.fila)
        self.leitora = Leitora(self.fila)
        self.mini = None
        self.base = base(self.cfg.get('pasta_jogos', 'pendrive'))
        self.carrega_fundo()
        self.mini = Miniaturas(self.fila, os.path.join(self.base, 'imagens', '.cache'))
        self.mini.start()
        self.aba = os.environ.get('RV_ABA', 'DISCO')
        self.tela = os.environ.get('RV_TELA', 'menu')   # menu | conv | fonte | controle | info | aviso
        self.sel = {'JOGOS': 0, 'OPCOES': 0, 'SIS': 0}
        self.sistema = os.environ.get('RV_SISTEMA')       # dentro de uma pasta de JOGOS
        self.jogos = lista_jogos(self.base, self.sistema) if self.sistema else []
        self.info = {'temp': None, 'ram': None, 'ip': None, 'wifi': None, 'mascara': None, 'gateway': None}
        self.wifi = None
        self.salvo = 0
        self.bs = 0
        self.ultima_acao = ''
        self.aviso = None
        self.confirma = None
        self.contagem = None      # (REINICIAR|DESLIGAR, hora final)
        self.vol_sujo = 0
        self.pergunta_disco = None    # disco reconhecido: INICIAR? A SIM / B NAO
        self.wf = {'rede': None, 'ip': None, 'lista': None, 'sel': 0, 'msg': '', 'ocupado': False}
        self.senha = ''
        self.tec = {'x': 0, 'y': 0, 'maius': False, 'rede': None}
        self.pastas_sel = 0
        self.pastas_msg = ''

    # ---------------------------------------------------------- jogos
    def sistemas_com_jogos(self):
        out = []
        cd = self.leitora.info if self.leitora.estado == 'ok' else None
        for s in SISTEMAS:
            if s in APELIDO:                 # pcenginecd entra na lista do PC Engine
                continue
            if SISTEMAS[s][1] and SISTEMAS[s][2] and os.path.exists(os.path.join(PASTA, 'nucleos', SISTEMAS[s][1])):
                out.append(s)                # sistema de CD com nucleo no pacote: sempre (INICIAR CD)
                continue
            tem = bool(lista_jogos(self.base, s)) if s in EXT_JOGO else False
            if not tem and s in EXT_JOGO:
                tem = bool(cd and APELIDO.get(cd.get('sistema'), cd.get('sistema')) == s)
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
        sistema = APELIDO.get(sistema, sistema)
        self.ultimo_jogo = (sistema, arquivo, cd)
        nucleo = SISTEMAS.get(sistema, (None, None))[1]
        if not nucleo or not os.path.exists(os.path.join(PASTA, 'nucleos', nucleo)):
            self.mostra_aviso('%s: NUCLEO AINDA NAO INSTALADO' % NOME_SISTEMA.get(sistema, '?'))
            return
        e_cd = cd or (arquivo and os.path.splitext(arquivo)[1].lower() in EXT_CD)
        info_bios = (self.leitora.info or {}) if cd else \
            (disco.identifica_arquivo(arquivo) if sistema == 'segacd' else None)
        falta = bios_falta(self.base, sistema, info_bios, e_cd)
        if falta:
            self.mostra_aviso(falta)
            return
        exe = exe_rv_jogo()
        so = os.path.join(PASTA, 'nucleos', nucleo)
        if cd:
            i = self.leitora.info or {}
            nome = i.get('nome') or i.get('rotulo') or 'DISCO'
            save = i.get('codigo') or i.get('rotulo') or i.get('digital') or 'DISCO'
        else:
            i = disco.identifica_arquivo(arquivo) or {}
            nome = nome_limpo(arquivo)
            save = i.get('codigo') or os.path.splitext(os.path.basename(arquivo))[0]
        save = ''.join(ch if ch.isalnum() or ch in '-_ ' else '_' for ch in save).strip() or 'jogo'
        saves = os.path.join(self.base, 'saves', sistema)
        os.makedirs(saves, exist_ok=True)
        opt = os.path.join(self.base, 'config', 'nucleos', nucleo.replace('_libretro.so', '.opt'))
        if nucleo in OPCOES_PADRAO and not os.path.exists(opt):
            try:
                os.makedirs(os.path.dirname(opt), exist_ok=True)
                with open(opt, 'w') as f:
                    f.write('# RETROVOLT: opcoes iniciais deste nucleo (mude no jogo: START+SELECT > OPCOES DO NUCLEO)\n')
                    for k, v in OPCOES_PADRAO[nucleo].items():
                        f.write('%s = "%s"\n' % (k, v))
            except OSError as ex:
                print('jogo: nao gravou %s: %s' % (opt, ex), flush=True)
        cmd = [exe, '--nucleo', so, '--bios', bios_pasta_nucleo(self.base, sistema), '--saves', saves,
               '--nome', nome[:60], '--save', save, '--fonte', F.nome, '--volume', str(self.volume()),
               '--conv', '%d,%d,%.3f,%.3f' % tuple(self.conv),
               '--opcoes', os.path.join(self.base, 'config', 'nucleos', nucleo.replace('_libretro.so', '.opt')),
               '--botoes', os.path.join(self.base, 'config', 'nucleos', nucleo.replace('_libretro.so', '.botoes')),
               '--conv-arq', os.path.join(self.base, 'config', 'nucleos', nucleo.replace('_libretro.so', '.conv')),
               '--states', os.path.join(self.base, 'states', sistema, save),
               '--filtro', str(self.FILTROS.index(self.cfg.get('video_filtro', 'NENHUM').upper())
                               if self.cfg.get('video_filtro', 'NENHUM').upper() in self.FILTROS else 0),
               '--filtro-arq', os.path.join(self.base, 'config', 'nucleos', nucleo.replace('_libretro.so', '.filtro')),
               '--imagem-arq', os.path.join(self.base, 'config', 'nucleos', nucleo.replace('_libretro.so', '.imagem'))]
        cmd += ['--cd', self.leitora.dev, '--cd-vel', self.cfg.get('leitora_velocidade', '4'),
                '--cd-acorda', str(num(self.cfg, 'leitora_acordar', int, 2, 60))] if cd else ['--jogo', arquivo]
        cmd += ['--tubo-480i', '0' if self.cfg.get('tubo_480i', 'SIM').upper() == 'NAO' else '1']
        if sistema == 'dreamcast':
            cmd += ['--gpu', 'gles' if self.cfg.get('dreamcast_gpu', 'VULKAN').upper() == 'GLES' else 'vulkan']
            # opcoes por jogo (ex.: Gunbird 2 em 1280x960 sem pontinhos, 3D em 640x480): o que
            # mudar no menu do jogo vale so para ele; o resto vem do flycast.opt
            cmd += ['--opcoes-jogo', os.path.join(self.base, 'config', 'nucleos', 'flycast', save + '.opt')]
        if cd and sistema == 'dreamcast':
            # CD-R em 2 sessoes: uma faixa por arquivo. Cache grande: o Dreamcast volta toda hora
            # aos mesmos arquivos e cada busca nova na leitora para o jogo ~100 ms
            cmd += ['--cd-faixas', '1', '--cd-cache', str(num(self.cfg, 'leitora_cache_dreamcast', int, 2, 600))]
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
        if r == 10:            # VOLTAR PARA A LISTA: lista deste sistema, cursor no jogo
            self.tela = 'menu'
            self.aba = 'JOGOS'
            self.sistema = sistema
            self.jogos = lista_jogos(self.base, sistema)
            itens = self.itens_pasta()
            self.sel['JOGOS'] = 0
            for n, (k, v) in enumerate(itens):
                if (cd and k == 'cd') or (not cd and k == 'arq' and v == arquivo):
                    self.sel['JOGOS'] = n
                    break
        elif r == 0:           # SAIR DO JOGO: menu principal
            self.tela = 'menu'
            self.aba = 'DISCO'
            self.sistema = None
        else:
            self.mostra_aviso('O JOGO NAO ABRIU (CODIGO %s). VEJA O LOG.TXT' % r)

    def carregando(self, nome):
        c = Canvas()
        cabecalho(c, self.aba, self.wifi, '')
        c.ctext(100, 'CARREGANDO', ORANGE)
        c.ctext(100 + F.ch + 8, nome[:40], IVORY)
        self.fb.show(c, self.conv)

    def mostra_aviso(self, txt):
        cabe = (W - 44) // F.cw
        extra = (len(txt) - cabe) / 8.0 + 1.5 if len(txt) > cabe else 0
        agora = time.monotonic()
        self.aviso = (txt, agora + 4 + extra, agora)

    # ---------------------------------------------------------- desenho
    def altura_tubo(self, h=None):
        """linhas do menu no tubo: a altura da convergencia abaixo de 100% vira menos linhas
        desenhadas (nenhuma linha pulada); acima de 100% as linhas se repetem"""
        h = self.conv[3] if h is None else h
        if not self.fb.tubo or h >= 1:
            return self.h_base
        return max(self.h_base // 2, int(round(self.h_base * h)))

    def ajusta_altura(self, fundo=True):
        global H_TELA, H
        novo = self.altura_tubo()
        if novo == H_TELA:
            return
        H_TELA = novo
        H = H_TELA - TOPO
        print('tela: menu com %d linhas (altura da convergencia %.3f)' % (H_TELA, self.conv[3]), flush=True)
        if fundo:
            self.carrega_fundo()

    def carrega_fundo(self):
        global FUNDO
        t0 = time.monotonic()
        FUNDO, qual = imagens.fundo_menu(self.base, W, H_TELA, exe_rv_jogo(), os.path.join(self.base, 'imagens', '.cache'),
                                         num(self.cfg, 'menu_fundo_brilho', int, 0, 100))
        print('fundo: %s %dx%d (%.1f s)' % (os.path.basename(qual) if qual else 'nenhum (cor lisa)', W, H_TELA,
                                           time.monotonic() - t0), flush=True)

    def desenha(self):
        t = self.tela
        if t == 'conv':     # teste de convergencia: a tela inteira, sem descer e sem fundo
            global H
            h_menu = H
            H = self.altura_tubo()          # ao vivo: a altura que esta sendo ajustada
            try:
                c = Canvas(liso=True)
                self.desenha_conv(c)
            finally:
                H = h_menu
            return c
        c = Canvas()
        if t == 'menu':
            if self.aba == 'DISCO':
                self.desenha_disco(c)
            elif self.aba == 'JOGOS':
                self.desenha_jogos(c)
            elif self.aba == 'TV':
                self.desenha_tv(c)
            else:
                cabecalho(c, 'OPCOES', self.wifi, 'A ENTRA')
                y = max(56, 30 + F.ch + 14)
                lista = self.opcoes()
                espaco = H - 14 - F.ch - y
                passo_op = max(F.ch + 3, min(F.ch + 9, espaco // len(lista)))
                cabem = max(1, espaco // passo_op)
                ini = 0                       # fonte alta no tubo: a lista rola com a selecao
                if cabem < len(lista):
                    ini = max(0, min(len(lista) - cabem, self.sel['OPCOES'] - cabem // 2))
                    if ini > 0:
                        c.rtext(W - 14, y - F.ch - 2, '^', SLATE)
                    if ini + cabem < len(lista):
                        c.rtext(W - 14, y + cabem * passo_op - 4, 'v', SLATE)
                for n, s in enumerate(lista):
                    if not ini <= n < ini + cabem:
                        continue
                    on = n == self.sel['OPCOES']
                    if s == 'VOLUME':
                        v = self.volume()
                        item(c, 24, y, s, on)
                        bx, bw = 24 + 14 * F.cw, 100
                        c.rect(bx, y + F.ch // 2 - 1, bw, 3, LINE)
                        c.rect(bx, y + F.ch // 2 - 1, bw * v // 100, 3, ORANGE if on else SLATE)
                        c.text(bx + bw + 8, y, ('< %d >' if on else '%d') % v, ORANGE if on else SLATE)
                        y += passo_op
                        continue
                    if s == 'DESLIGAR':
                        c.text(24, y, s, RED if on else (150, 80, 70))
                        if on:
                            c.rect(16, y - 1, 3, F.ch + 2, RED)
                            c.rect(24, y + F.ch + 2, len(s) * F.cw, 1, RED)
                            c.text(24 + (len(s) + 1) * F.cw, y, '->', RED)
                    else:
                        item(c, 24, y, s, on)
                    y += passo_op
            if self.confirma:
                self.desenha_confirma(c)
            if self.contagem:
                self.desenha_contagem(c)
            if self.pergunta_disco:
                self.desenha_pergunta_disco(c)
        elif t == 'video':
            self.desenha_video(c)
        elif t == 'teste_video':
            moldura(c, 'VIDEO', '')
            n = max(0, int(self.teste_ate - time.monotonic() + 0.999))
            c.ctext(H // 2 - 30, 'RESOLUCAO NOVA: %dX%d' % (self.fb.w, self.fb.h), GREEN)
            c.ctext(H // 2 - 10, 'MANTER ESTA RESOLUCAO?', IVORY)
            c.ctext(H // 2 + 10, 'A OU START: SIM   (VOLTA SOZINHA EM %d)' % n, ORANGE)
        elif t == 'wifi':
            self.desenha_wifi(c)
        elif t == 'teclado':
            self.desenha_teclado(c)
        elif t == 'pastas':
            self.desenha_pastas(c)
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
            bios = ['%s %d' % (s.upper(), len(bios_arquivos(self.base, s))) for s in SISTEMAS
                    if s not in APELIDO and SISTEMAS[s][1] and bios_arquivos(self.base, s)]
            linhas = [
                ('VERSAO', '%s (%s)' % (VERSAO.upper(), ORIGEM)),
                ('VIDEO', '%dX%d %s' % (self.fb.w, self.fb.h, 'TUBO' if self.fb.tubo else 'LCD')),
                ('TEMP', '%dC' % d['temp'] if d['temp'] is not None else '?'),
                ('RAM', '%.1f/%.1fGB' % d['ram'] if d['ram'] else '?'),
                ('WI-FI', (d['wifi'] or 'SEM WI-FI')[:18]),
                ('IP', d.get('ip') or 'SEM REDE'),
                ('MASCARA', d.get('mascara') or '-'),
                ('GATEWAY', d.get('gateway') or '-'),
                ('JOGOS', ('PENDRIVE' if self.base.startswith(USB) else 'CARTAO') +
                 (' %d/%d MB' % espaco(self.base) if espaco(self.base) else '')),
                ('LEITORA', {'procurando': '...', 'ok': 'COM DISCO', 'vazia': 'VAZIA', 'aberta': 'ABERTA', 'lendo': 'LENDO',
                             'sem_leitora': 'NAO ENCONTRADA'}.get(self.leitora.estado, '?')),
                ('BIOS', ('%d: ' % len(bios) + ' '.join(bios))[:34] if bios else 'NENHUMA'),
            ]
            y = 54
            for k, v in linhas:
                c.text(24, y, k, SLATE)
                c.text(24 + 9 * F.cw, y, v.upper(), IVORY)
                y += F.ch + 3
        if self.aviso and time.monotonic() < self.aviso[1]:
            # aviso: canto inferior esquerdo, fundo laranja, letra azul, cantos arredondados
            txt, _, ini = self.aviso
            cabe = (W - 44) // F.cw
            vis = min(len(txt), cabe)
            w = vis * F.cw + 12
            x0, y0, h = 14, H - 24 - 2 * F.ch, F.ch + 6
            c.rect(x0 + 1, y0, w - 2, h, ORANGE)
            c.rect(x0, y0 + 1, w, h - 2, ORANGE)
            desl = 0
            if len(txt) > cabe:      # letreiro: espera 1 s, rola 8 letras/s, para no fim
                desl = min(int(max(0.0, time.monotonic() - ini - 1.0) * 8 * F.cw), (len(txt) - cabe) * F.cw)
            c.text_cortado(x0 + 6 - desl, y0 + 3, txt, NAV, x0 + 6, x0 + 6 + vis * F.cw)
        return c

    def desenha_pergunta_disco(self, c):
        i = self.pergunta_disco
        sis = i.get('sistema')
        tem = bool(SISTEMAS.get(sis, (0, None))[1])
        x0, y0, w, h = 30, 62, W - 60, 120
        c.rect(x0, y0, w, h, (20, 26, 52))
        c.rect(x0, y0, w, 2, ORANGE)
        c.text(x0 + 10, y0 + 10, 'DISCO RECONHECIDO', SLATE)
        c.text(x0 + 10, y0 + 14 + F.ch, NOME_SISTEMA.get(sis, 'DESCONHECIDO'), ORANGE)
        nome = (i.get('nome') or i.get('rotulo') or '').upper()
        lim = (w - 20) // F.cw
        yy = y0 + 20 + 2 * F.ch
        for k in range(0, min(len(nome), lim * 2), lim):
            c.text(x0 + 10, yy, nome[k:k + lim], IVORY)
            yy += F.ch + 3
        if tem:
            c.text(x0 + 10, y0 + h - F.ch - 10, 'INICIAR?  A SIM   B NAO', IVORY)
        else:
            c.text(x0 + 10, y0 + h - 2 * F.ch - 14, 'NUCLEO AINDA NAO INSTALADO', RED)
            c.text(x0 + 10, y0 + h - F.ch - 10, 'B VOLTA', SLATE)

    def itens_wifi(self):
        out = [('acao', 'BUSCAR REDES', 0, False)]
        for nome, sinal, prot in (self.wf['lista'] or []):
            out.append(('rede', nome, sinal, prot))
        if self.wf['rede']:
            out.append(('esquecer', 'ESQUECER %s' % self.wf['rede'][:18].upper(), 0, False))
        return out

    def desenha_wifi(self, c):
        moldura(c, 'WI-FI', 'A ESCOLHE  B VOLTA')
        wf = self.wf
        if wf['rede']:
            c.text(24, 56, 'CONECTADO: %s' % wf['rede'][:24].upper(), GREEN)
            c.text(24, 58 + F.ch, 'IP %s' % (wf['ip'] or '-'), SLATE)
        else:
            c.text(24, 56, 'SEM WI-FI', SLATE)
        itens = self.itens_wifi()
        passo = F.ch + 6
        y0 = 66 + 2 * F.ch
        cabem = max(1, (H - 34 - 2 * F.ch - y0) // passo)
        sel = min(wf['sel'], len(itens) - 1)
        ini = max(0, min(sel - cabem // 2, len(itens) - cabem))
        y = y0
        for n in range(ini, min(len(itens), ini + cabem)):
            k, nome, sinal, prot = itens[n]
            if k == 'rede':
                item(c, 24, y, nome[:22] + (' *' if prot else ''), n == sel, '%d%%' % sinal)
            else:
                item(c, 24, y, nome, n == sel)
            y += passo
        if wf['ocupado'] or wf['msg']:
            txt = wf['msg'] or 'AGUARDE...'
            c.rect(16, H - 30 - F.ch, W - 32, F.ch + 8, (40, 30, 20))
            c.text(24, H - 26 - F.ch, txt[:44], ORANGE)

    def desenha_teclado(self, c):
        moldura(c, 'SENHA', 'START CONECTA  B APAGA')
        c.text(24, 54, (self.tec['rede'] or '')[:30], ORANGE)
        c.rect(24, 58 + F.ch, W - 48, F.ch + 6, (20, 26, 52))
        c.text(28, 61 + F.ch, self.senha[-36:] + '_', IVORY)
        linhas = TECLAS[self.tec['maius']]
        cw = (W - 48) // 10
        y = 74 + 2 * F.ch
        for j, l in enumerate(linhas):
            for i, ch in enumerate(l):
                on = self.tec['y'] == j and self.tec['x'] == i
                x = 24 + i * cw
                c.rect(x, y - 3, cw - 2, F.ch + 6, ORANGE if on else (30, 38, 66))
                c.text(x + (cw - 2 - F.cw) // 2, y, ch, NAV if on else IVORY)
            y += F.ch + 9
        xs = 24
        for i, nome in enumerate(TECLAS_FIM):       # ABC(2) ESPACO(3) APAGAR(3) OK(2)
            larg = cw * (3 if i in (1, 2) else 2)
            on = self.tec['y'] == len(linhas) and self.tec['x'] == i
            if i == 0:
                nome = 'abc' if self.tec['maius'] else 'ABC'
            c.rect(xs, y - 3, larg - 2, F.ch + 6, ORANGE if on else (30, 38, 66))
            c.text(xs + (larg - 2 - len(nome) * F.cw) // 2, y, nome, NAV if on else IVORY)
            xs += larg
        if self.wf['msg']:
            c.text(24, H - 18 - 2 * F.ch, self.wf['msg'][:44], ORANGE)
        else:
            c.text(24, H - 18 - 2 * F.ch, 'TECLADO USB TAMBEM FUNCIONA', SLATE)

    def desenha_pastas(self, c):
        moldura(c, 'PASTAS DE JOGOS', 'A ESCOLHE  B VOLTA')
        atual = 'pendrive' if self.base.startswith(USB) else 'cartao'
        c.text(24, 56, 'JOGOS, BIOS E SAVES FICAM EM:', SLATE)
        y = 66 + F.ch
        for n, (k, nome) in enumerate((('pendrive', 'PENDRIVE'), ('cartao', 'CARTAO (MICRO SD)'))):
            pasta = os.path.join(USB, 'RETROVOLT') if k == 'pendrive' else BASE_CARTAO
            disp = tem_pendrive() if k == 'pendrive' else True
            esp = espaco(pasta if os.path.exists(pasta) else os.path.dirname(pasta)) if disp else None
            valor = ('EM USO' if k == atual else None) if disp else 'NAO ENCONTRADO'
            item(c, 24, y, nome, n == self.pastas_sel, valor)
            if esp:
                c.text(40, y + F.ch + 5, '%d MB USADOS DE %d MB' % esp, SLATE)
            y += 2 * F.ch + 16
        c.text(24, y + 4, 'A PASTA RETROVOLT E CRIADA SE FALTAR.', SLATE)
        c.text(24, y + 8 + F.ch, 'NADA E APAGADO.', SLATE)
        if self.pastas_msg:
            c.text(24, H - 26 - F.ch, self.pastas_msg[:44], ORANGE)
        if self.confirma == 'COPIAR':
            caixa(c, ['COPIAR SAVES E CONFIGURACAO', 'PARA O NOVO? A SIM  B NAO'])

    def desenha_confirma(self, c):
        caixa(c, ['%s? A SIM  B NAO' % self.confirma])

    def desenha_disco(self, c):
        st = self.leitora.estado
        i = self.leitora.info if st == 'ok' else None
        pode = bool(i and SISTEMAS.get(i.get('sistema'), (0, None))[1])
        cabecalho(c, 'DISCO', self.wifi, 'A JOGAR' if pode else '')
        raio = min(56 + (H - 240) // 2, (W - 200) // 3)
        cx, cy = 32 + raio, (H + 16) // 2
        if st == 'ok':
            arte = imagens.arte_disco(self.base, i.get('sistema'), i.get('codigo')) if i else None
            if not (arte and self.desenha_arte(c, arte, cx, cy, 2 * raio)):
                desenha_cd(c, cx, cy, raio)
        else:
            furo = raio * 5 // 14
            for y in range(cy - raio, cy + raio + 1):
                for x in range(cx - raio, cx + raio + 1):
                    if furo * furo < (x - cx) ** 2 + (y - cy) ** 2 <= raio * raio:
                        c.px[y * W + x] = (30, 38, 66)
        x, y = cx + raio + 16, 66
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
            yj = H - 52
            c.text(x, yj, 'A JOGAR', IVORY)
            c.rect(x, yj + F.ch + 2, 7 * F.cw, 1, ORANGE)
            c.text(x + 8 * F.cw, yj, '->', ORANGE)

    def desenha_arte(self, c, arte, cx, cy, diam):
        """arte do disco num circulo, com o furo do meio; False se ainda nao ha imagem"""
        if self.esc:            # LCD: imagem com os pontos do monitor (nitida)
            m = self.mini.pega(arte, diam * self.esc, diam * self.esc)
            if not m:
                return False
            c.extras.append((cx - diam // 2, cy - diam // 2, diam, diam, m, True))
            return True
        m = self.mini.pega(arte, diam, diam)
        if not m:
            return False
        w, h, rgb = m
        r = diam / 2.0
        for y in range(h):
            for x in range(w):
                dx, dy = x - w / 2.0 + 0.5, y - h / 2.0 + 0.5
                if dx * dx + dy * dy > r * r:
                    continue
                X, Y = cx - w // 2 + x, cy - h // 2 + y
                if 0 <= X < W and 0 <= Y < H:
                    o = (y * w + x) * 3
                    c.px[Y * W + X] = (rgb[o], rgb[o + 1], rgb[o + 2])
        f = max(4, diam // 9)
        for y in range(-f, f + 1):
            for x in range(-f, f + 1):
                if x * x + y * y <= f * f:
                    c.px[(cy + y) * W + cx + x] = NAV if x * x + y * y <= (f - 2) ** 2 else (200, 205, 215)
        return True

    def desenha_capa(self, c, arte, x0, y0, bw, bh):
        """capa do jogo centralizada na caixa; moldura vazia enquanto carrega/sem capa"""
        if self.esc and arte:   # LCD: capa com os pontos do monitor (nitida)
            m = self.mini.pega(arte, bw * self.esc, bh * self.esc)
            if m:
                c.extras.append((x0, y0, bw, bh, m, False))
                return
        m = self.mini.pega(arte, bw, bh) if (arte and not self.esc) else None
        if not m:
            c.rect(x0, y0, bw, bh, (20, 26, 52))
            c.rect(x0, y0, bw, 1, LINE); c.rect(x0, y0 + bh - 1, bw, 1, LINE)
            c.rect(x0, y0, 1, bh, LINE); c.rect(x0 + bw - 1, y0, 1, bh, LINE)
            txt = '...' if arte else 'SEM CAPA'
            c.text(x0 + (bw - len(txt) * F.cw) // 2, y0 + (bh - F.ch) // 2, txt, SLATE)
            return
        w, h, rgb = m
        ox, oy = x0 + (bw - w) // 2, y0 + (bh - h) // 2
        for y in range(h):
            Y = oy + y
            if not 0 <= Y < H:
                continue
            for x in range(w):
                X = ox + x
                if 0 <= X < W:
                    o = (y * w + x) * 3
                    c.px[Y * W + X] = (rgb[o], rgb[o + 1], rgb[o + 2])

    def arte_jogo(self, arquivo):
        """arte do disco de um jogo da lista (pelo codigo lido da imagem, ou pelo nome)"""
        if not hasattr(self, '_codigos'):
            self._codigos = {}
        if arquivo not in self._codigos:
            i = disco.identifica_arquivo(arquivo) or {}
            self._codigos[arquivo] = i.get('codigo')
        return imagens.arte_disco(self.base, self.sistema, self._codigos[arquivo], arquivo, tipo='capas')

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
        bw = bh = max(96, min(H - 120, (W - 40) // 3))      # caixa da capa
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
                ok = bool(cd and APELIDO.get(cd.get('sistema'), cd.get('sistema')) == self.sistema)
                valor = None
                if not ok:
                    valor = {'lendo': 'LENDO...', 'sem_leitora': 'SEM LEITORA', 'procurando': '...'}.get(self.leitora.estado, 'SEM DISCO')
                    if cd:
                        valor = NOME_SISTEMA.get(cd.get('sistema'), 'OUTRO DISCO')
                item(c, 24, y, v, n == sel, valor)
                if not ok and n != sel:
                    pass
            elif k == 'arq':
                lim = (W - 40 - bw - 4) // F.cw - 3
                item(c, 24, y, nome_limpo(v)[:lim], n == sel)
            y += passo
        if not self.jogos:
            c.text(24, y0 + 3 * passo, 'SEM JOGOS NA PASTA ROMS/%s' % self.sistema.upper(), SLATE)
        # a direita da lista: capa do jogo (arquivos) ou arte do CD (INICIAR CD)
        k, v = itens[sel]
        x0, y0c = W - 14 - bw, 50 + F.ch + 8 + (H - (50 + F.ch + 8) - 24 - bh) // 2
        if k == 'arq':
            self.desenha_capa(c, self.arte_jogo(v), x0, y0c, bw, bh)
        elif k == 'cd':
            arte = imagens.arte_disco(self.base, cd.get('sistema'), cd.get('codigo')) if cd else None
            if not (arte and self.desenha_arte(c, arte, x0 + bw // 2, y0c + bh // 2, bh)):
                desenha_cd(c, x0 + bw // 2, y0c + bh // 2, bh // 2)

    # ---------------------------------------------------------- acoes
    def acao_menu(self, a):
        if self.pergunta_disco:
            i = self.pergunta_disco
            if a == 'A' and SISTEMAS.get(i.get('sistema'), (0, None))[1]:
                self.pergunta_disco = None
                self.jogar(i.get('sistema'), cd=True)
            elif a == 'B':
                self.pergunta_disco = None
            return
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
            self.tv_para_previa()
            if self.aba == 'TV':
                self.tv_carrega()
            return
        if self.aba == 'TV':
            self.acao_tv(a)
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
                    if cd and APELIDO.get(cd.get('sistema'), cd.get('sistema')) == self.sistema:
                        self.jogar(self.sistema, cd=True)
                    elif cd:
                        self.mostra_aviso('O DISCO NA LEITORA E DE %s' % NOME_SISTEMA.get(cd.get('sistema'), 'OUTRO SISTEMA'))
                    else:
                        self.mostra_aviso('COLOQUE UM DISCO NA LEITORA')
                elif k == 'arq':
                    self.jogar(self.sistema, arquivo=v)
        elif self.aba == 'OPCOES':
            if self.opcoes()[self.sel['OPCOES']] == 'VOLUME' and a in ('ESQ', 'DIR'):
                v = max(0, min(100, self.volume() + (5 if a == 'DIR' else -5)))
                self.cfg['volume'] = str(v)
                self.vol_sujo = time.monotonic()
                return
            if a == 'CIMA':
                self.sel['OPCOES'] = (self.sel['OPCOES'] - 1) % len(self.opcoes())
            elif a == 'BAIXO':
                self.sel['OPCOES'] = (self.sel['OPCOES'] + 1) % len(self.opcoes())
            elif a == 'A':
                o = self.opcoes()[self.sel['OPCOES']]
                if o == 'VOLUME':
                    return
                if o in ('REINICIAR', 'DESLIGAR'):
                    self.confirma = o
                    return
                self.tela = {'CONVERGENCIA': 'conv', 'FONTE': 'fonte', 'TESTE DO CONTROLE': 'controle', 'INFO': 'info',
                             'WI-FI': 'wifi', 'PASTAS DE JOGOS': 'pastas', 'VIDEO': 'video'}[o]
                if self.tela == 'video':
                    self.video_sel = 0
                if self.tela == 'wifi':
                    self.wf.update(sel=0, msg='')
                    self.wifi_tarefa(lambda: None, 'LENDO...')
                if self.tela == 'pastas':
                    self.pastas_sel = 0 if self.base.startswith(USB) else 1
                    self.pastas_msg = ''
                if self.tela == 'conv':
                    self.conv_antes = list(self.conv)
                if self.tela == 'fonte':
                    self.fonte_antes = F.nome
                if self.tela == 'info':
                    threading.Thread(target=self._info_fundo, daemon=True).start()

    # ---------------------------------------------------------- video
    SAIDAS = ['AUTO', 'TUBO', 'LCD']
    FILTROS = ['NENHUM', 'SCANLINES', 'SCANLINES LEVES', 'CRT', 'SUAVE', 'SCALE2X', 'SCALE3X', 'XBR']

    def itens_video(self):
        """(chave, nome, valor) dos itens de OPCOES > VIDEO"""
        itens = [('saida', 'SAIDA', self.cfg.get('video_saida', 'AUTO').upper())]
        if not self.fb.tubo:
            itens.append(('modo', 'RESOLUCAO', self.cfg.get('video_modo', 'AUTO').upper()))
        if not self.fb.tubo:                      # no tubo nao ha efeitos
            itens.append(('filtro', 'FILTRO', self.cfg.get('video_filtro', 'NENHUM').upper()))
        return itens

    # ---------------------------------------------------------- RetroTV
    def tv_carrega(self, espera=False):
        """le as listas em segundo plano (podem vir da internet); o menu mostra
        LENDO AS LISTAS... e redesenha quando terminar"""
        self.tv_progs = tv.tem_programas()
        self.__dict__.setdefault('tv_st', {'lista': None, 'sel_lista': 0, 'sel': 0})
        if not hasattr(self, 'tv_listas'):
            self.tv_listas = []
        if getattr(self, 'tv_carregando', False):
            return
        self.tv_carregando = True

        def tarefa():
            try:
                self._tv_carrega()
            finally:
                self.tv_carregando = False
                self.fila.put('_tv')
        if espera:
            tarefa()
        else:
            threading.Thread(target=tarefa, daemon=True).start()

    def _tv_carrega(self):
        listas = tv.listas(self.base)
        self.tv_listas = listas
        st = self.tv_st
        if st['lista'] is not None and st['lista'] >= len(self.tv_listas):
            st['lista'] = None
        if len(self.tv_listas) == 1:          # uma lista so: vai direto aos canais
            st['lista'] = 0
        print('tv: %d listas (%s)' % (len(self.tv_listas), ', '.join(
            '%s: %d canais' % (n, len(cs)) for n, _, cs in self.tv_listas)), flush=True)

    def tv_canais(self):
        st = self.tv_st
        if st['lista'] is None:
            return []
        return self.tv_listas[st['lista']][2]

    def tv_caixa(self):
        """caixa da previa (pontos do menu), do tamanho da previa do save state (4:3)"""
        bw = max(120, min((W - 40) // 2, (H - 110) * 4 // 3))
        bh = bw * 3 // 4
        x0 = W - 14 - bw
        y0 = 50 + F.ch + 8 + max(0, (H - (50 + F.ch + 8) - 24 - bh) // 2)
        return x0, y0, bw, bh

    def desenha_tv(self, c):
        if not hasattr(self, 'tv_st'):
            self.tv_carrega()
        st = self.tv_st
        cabecalho(c, 'TV', self.wifi, '<- -> 10  LT/RT GRUPO  A VER' if st['lista'] is not None else 'A ENTRA')
        if not self.tv_progs['ffmpeg']:
            c.text(24, 64, 'FALTA O FFMPEG NO SISTEMA. NO PUTTY:', ORANGE)
            c.text(24, 64 + F.ch + 6, 'SUDO APT INSTALL -Y FFMPEG MPV', IVORY)
            return
        if not self.tv_listas and getattr(self, 'tv_carregando', False):
            c.text(24, 64, 'LENDO AS LISTAS DE CANAIS...', SLATE)
            return
        if not self.tv_listas:
            c.text(24, 64, 'NENHUMA LISTA DE CANAIS.', SLATE)
            c.text(24, 64 + F.ch + 6, 'COLOQUE A LISTA (.M3U .M3U8 .PLS', SLATE)
            c.text(24, 64 + 2 * (F.ch + 6), '.XSPF .TXT) EM RETROVOLT/TV/', IVORY)
            return
        if st['lista'] is None:               # varias listas: escolhe uma
            st['sel_lista'] = min(st['sel_lista'], len(self.tv_listas) - 1)
            y = 58
            for n, (nome, _, cs) in enumerate(self.tv_listas):
                item(c, 24, y, nome.upper()[:30], n == st['sel_lista'], '%d' % len(cs))
                y += F.ch + 9
            return
        nome_l, _, canais = self.tv_listas[st['lista']]
        sel = st['sel'] = min(st['sel'], len(canais) - 1)
        grupo = canais[sel].get('grupo', '')
        titulo = nome_l.upper() + ('  > ' + grupo.upper() if grupo else '')
        c.text(16, 50, titulo[:(W - 80) // F.cw], ORANGE)
        c.rtext(W - 12, 50, '%d/%d' % (sel + 1, len(canais)), SLATE)
        x0, y0c, bw, bh = self.tv_caixa()
        passo = F.ch + 7
        y0 = 50 + F.ch + 10
        cabem = max(1, (H - 8 - F.ch - y0) // passo)
        ini = max(0, min(sel - cabem // 2, len(canais) - cabem))
        lim = (W - 40 - bw - 4) // F.cw - 3
        y = y0
        larg = max(len(cs.get('num', '')) for cs in canais)
        for n in range(ini, min(len(canais), ini + cabem)):
            num = canais[n].get('num', str(n + 1)).rjust(larg)
            item(c, 24, y, ('%s %s' % (num, canais[n]['nome'].upper()))[:lim], n == sel)
            y += passo
        # caixa da previa: preta com contorno; o video entra por cima (direto na tela)
        c.rect(x0 - 1, y0c - 1, bw + 2, bh + 2, (200, 205, 215))
        c.rect(x0, y0c, bw, bh, (0, 0, 0))
        pv = getattr(self, 'tv_previa', None)
        ch = canais[sel]
        if pv and pv.erro:
            c.text(x0 + 6, y0c + bh // 2 - F.ch, 'CANAL FORA DO AR', RED)
        elif pv:
            if pv.quadros == 0:
                c.text(x0 + 6, y0c + bh // 2 - F.ch // 2, 'SINTONIZANDO...', SLATE)
        else:
            if ch.get('grupo'):
                c.text(x0 + 6, y0c + 6, ch['grupo'].upper()[:bw // F.cw - 2], SLATE)
            c.text(x0 + 6, y0c + bh // 2 - F.ch // 2, 'A: ASSISTIR', SLATE)
        c.text(x0, y0c + bh + 6, ('%s %s' % (ch.get('num', ''), ch['nome'].upper())).strip()[:bw // F.cw], IVORY)

    def tv_brilho(self):
        return num(self.cfg, 'tv_brilho', int, -50, 50)

    def tv_para_previa(self):
        if getattr(self, 'tv_pre', 0):        # tela cheia abrindo por tras: cancela
            self.tv_pre = 0
            if getattr(self, 'tv_cheia', None):
                self.tv_cheia.fecha()
            self.leitora.pausa.clear()
        pv = getattr(self, 'tv_previa', None)
        if pv:
            pv.fecha()
            self.tv_previa = None

    def tv_inicia_previa(self):
        self.tv_para_previa()
        canais = self.tv_canais()
        if not canais or self.fb.fake:
            return
        ch = canais[self.tv_st['sel']]
        self.fb.show(self.desenha(), self.conv)     # caixa preta e geometria atual
        x0, y0, sx, sy, topo = self.fb.geo
        bx, by, bw, bh = self.tv_caixa()
        px, py = int(x0 + bx * sx), int(y0 + (by + topo) * sy)
        pw, ph = int(bw * sx) // 2 * 2, int(bh * sy) // 2 * 2
        if self.fb.tubo:     # encaixa em pontos quadrados (bw x bh) e estica para a tela larga
            aw, ah = bw // 2 * 2, bh // 2 * 2
        else:
            aw, ah = pw, ph
        bpp = self.fb.bpp // 8
        fb = self.fb

        def desenha(q):
            if self.tela != 'menu' or self.aba != 'TV':
                return
            linha = pw * bpp
            with fb.trava:
                for y in range(ph):
                    o = (py + y) * fb.stride + px * bpp
                    fb.mem[o:o + linha] = q[y * linha:(y + 1) * linha]
            if pv.quadros == 1:
                self.fila.put('_tv')                # tira o "SINTONIZANDO..."

        pv = tv.Previa(ch['url'], pw, ph, self.fb.bpp, desenha, self.cfg.get('audio_dispositivo') or AUDIO_PADRAO,
                       self.volume(), aw, ah, self.tv_brilho())
        self.tv_previa = pv
        self.tv_previa_ini = time.monotonic()
        print('tv: previa %s (%s)' % (ch['nome'], ch['url']), flush=True)
        pv.start()

    def tv_tela_cheia(self, cedo=False):
        """cedo: o mpv abre por tras com a previa ainda rodando e so toma a tela quando ja
        tem imagem (sem os 3-4 s de preto); a previa fecha nessa hora (tv_vigia)"""
        canais = self.tv_canais()
        if not canais:
            return
        ch = canais[self.tv_st['sel']]
        if not cedo:
            self.tv_para_previa()
        if not getattr(self, 'tv_cheia', None):
            self.tv_cheia = tv.TelaCheia(self.fb.tubo, self.fb.w, self.fb.h, self.fb.bpp,
                                         self.cfg.get('audio_dispositivo') or AUDIO_PADRAO, self.volume())
        self.leitora.pausa.set()
        if cedo:
            self.tv_pre = time.monotonic()
        else:
            self.tela = 'tv_cheia'
        print('tv: tela cheia %s%s' % (ch['nome'], ' (abrindo por tras da previa)' if cedo else ''), flush=True)
        self.tv_cheia.volume = self.volume()
        self.tv_cheia.brilho = self.tv_brilho()
        self.tv_cheia.abre(ch['url'], '%s  %s' % (ch.get('num', ''), ch['nome']),
                           '%s  %s\nA: LISTA DE CANAIS   <- ->: VOLUME   LT/RT: BRILHO   B: VOLTAR'
                           % (ch.get('num', ''), ch['nome']), sem_audio=cedo)

    def tv_sai_tela_cheia(self, aviso=None):
        if getattr(self, 'tv_cheia', None):
            self.tv_cheia.fecha()
        self.leitora.pausa.clear()
        self.tela = 'menu'
        if aviso:
            self.mostra_aviso(aviso)

    def tv_vigia(self):
        """chamado pelo laco do menu: previa -> tela cheia em 5 s; mpv fechou sozinho"""
        pre = getattr(self, 'tv_pre', 0)
        if pre and self.tela == 'menu':
            tc = self.tv_cheia
            if tc.rodando() and tc.mostrando():
                self.tv_pre = 0
                pv = getattr(self, 'tv_previa', None)
                if pv:
                    pv.fecha()
                    self.tv_previa = None
                self.tela = 'tv_cheia'
                tc.liga_audio()                     # a previa soltou a saida de som
                print('tv: tela cheia no ar (%.1f s depois de abrir)' % (time.monotonic() - pre), flush=True)
                return False
            if not tc.rodando() or time.monotonic() - pre > 8:
                # nao abriu junto com a previa (servidor que so aceita 1 conexao?): do jeito antigo
                print('tv: tela cheia nao abriu junto com a previa: %s' % (tc.erro()[:200] if not tc.rodando()
                                                                          else 'demorou'), flush=True)
                self.tv_pre = 0
                self.tv_tela_cheia()
                return True
            return False
        pv = getattr(self, 'tv_previa', None)
        if pv and self.tela == 'menu':
            if pv.erro:
                print('tv: canal fora do ar: %s' % pv.erro[:200], flush=True)
                self.tv_previa = None
                return True
            rapida = self.cfg.get('tv_troca_rapida', 'SIM').upper() != 'NAO'
            if pv.quadros and time.monotonic() - self.tv_previa_ini >= (3 if rapida else 5):
                self.tv_tela_cheia(cedo=rapida)     # rapida: toma a tela uns 2 s depois, ja com imagem
                return False
        if self.tela == 'tv_cheia' and not self.tv_cheia.rodando():
            err = self.tv_cheia.erro()
            print('tv: reproducao terminou %s' % err[:200], flush=True)
            self.tv_sai_tela_cheia('CANAL ENCERROU OU SAIU DO AR')
            return True
        return False

    def acao_tv(self, a):
        if not hasattr(self, 'tv_st'):
            self.tv_carrega()
            return
        st = self.tv_st
        if st['lista'] is None:
            n = len(self.tv_listas)
            if not n:
                return
            if a in ('CIMA', 'BAIXO'):
                st['sel_lista'] = (st['sel_lista'] + (1 if a == 'BAIXO' else -1)) % n
            elif a == 'A':
                st['lista'], st['sel'] = st['sel_lista'], 0
            return
        canais = self.tv_canais()
        if a in ('CIMA', 'BAIXO', 'ESQ', 'DIR', 'L2', 'R2'):
            self.tv_para_previa()
            if a in ('L2', 'R2'):
                # LT/RT: comeco do grupo seguinte/anterior (lista com grupos); <- -> = 10 em 10
                inicios = [i for i, c in enumerate(canais) if i == 0 or c.get('grupo') != canais[i - 1].get('grupo')]
                atual = max(i for i in inicios if i <= st['sel'])
                if a == 'R2':
                    st['sel'] = next((i for i in inicios if i > atual), st['sel'])
                else:
                    st['sel'] = atual if st['sel'] != atual else max([i for i in inicios if i < atual] or [0])
            else:
                d = (1 if a in ('BAIXO', 'DIR') else -1) * (10 if a in ('ESQ', 'DIR') else 1)
                st['sel'] = max(0, min(len(canais) - 1, st['sel'] + d))
        elif a == 'A':
            if getattr(self, 'tv_pre', 0):
                pass                                 # tela cheia ja abrindo por tras
            elif getattr(self, 'tv_previa', None) and self.tv_previa.quadros:
                # A de novo: tela cheia sem esperar
                self.tv_tela_cheia(cedo=self.cfg.get('tv_troca_rapida', 'SIM').upper() != 'NAO')
            else:
                self.tv_inicia_previa()
        elif a == 'B':
            if getattr(self, 'tv_previa', None):
                self.tv_para_previa()
            elif len(self.tv_listas) > 1:
                st['lista'] = None

    def tv_guia_mostra(self):
        """guia por cima do video: 9 canais em volta do cursor (o video continua)"""
        canais = self.tv_canais()
        g = self.tv_guia
        linhas = []
        for i in range(g - 4, g + 5):
            if 0 <= i < len(canais):
                c = canais[i]
                marca = '>> ' if i == g else '    '
                atual = '  (NO AR)' if i == self.tv_st['sel'] else ''
                linhas.append('%s%s  %s%s' % (marca, c.get('num', i + 1), c['nome'], atual))
        grupo = canais[g].get('grupo', '')
        topo = (grupo.upper() + '\n') if grupo else ''
        self.tv_cheia.texto(topo + '\n'.join(linhas) + '\n\nA: ASSISTIR   B: FECHAR   <- ->: VOLUME', 6000)
        self.tv_guia_ate = time.monotonic() + 6

    def acao_tv_cheia(self, a):
        """tela cheia. Sem guia: CIMA/BAIXO troca de canal, A abre o guia, B/START volta a
        lista. Com guia: CIMA/BAIXO anda no guia (o video continua), A troca, B fecha.
        ESQ/DIR (ou L1/R1): volume. LT/RT: brilho (tv_brilho no retrovolt.cfg)."""
        canais = self.tv_canais()
        st = self.tv_st
        guia = time.monotonic() < getattr(self, 'tv_guia_ate', 0)
        if a in ('L2', 'R2'):
            b = max(-50, min(50, self.tv_brilho() + (5 if a == 'R2' else -5)))
            self.cfg['tv_brilho'] = str(b)
            self.vol_sujo = time.monotonic()            # grava no retrovolt.cfg em 1,5 s
            self.tv_cheia.muda_brilho(b)
            return
        if a in ('ESQ', 'DIR', 'L1', 'R1'):
            v = max(0, min(100, self.volume() + (5 if a in ('DIR', 'R1') else -5)))
            self.cfg['volume'] = str(v)
            self.vol_sujo = time.monotonic()            # grava no retrovolt.cfg em 1,5 s
            self.tv_cheia.muda_volume(v)
            return
        if not canais:
            self.tv_sai_tela_cheia()
            return
        if guia:
            if a in ('CIMA', 'BAIXO'):
                self.tv_guia = (self.tv_guia + (1 if a == 'BAIXO' else -1)) % len(canais)
                self.tv_guia_mostra()
            elif a == 'A':
                self.tv_guia_ate = 0
                if self.tv_guia != st['sel']:
                    st['sel'] = self.tv_guia
                    ch = canais[st['sel']]
                    print('tv: canal %s (guia)' % ch['nome'], flush=True)
                    self.tv_cheia.troca(ch['url'], '%s  %s' % (ch.get('num', ''), ch['nome']))
                else:
                    self.tv_cheia.texto('', 1)
            elif a in ('B', 'SELECT'):
                self.tv_guia_ate = 0
                self.tv_cheia.texto('', 1)
            elif a == 'START':
                self.tv_sai_tela_cheia()
            return
        if a in ('B', 'START', 'SELECT'):
            self.tv_sai_tela_cheia()
        elif a == 'A':
            self.tv_guia = st['sel']
            self.tv_guia_mostra()
        elif a in ('CIMA', 'BAIXO'):      # troca de canal, como na TV
            st['sel'] = (st['sel'] + (1 if a == 'CIMA' else -1)) % len(canais)
            ch = canais[st['sel']]
            print('tv: canal %s' % ch['nome'], flush=True)
            self.tv_cheia.troca(ch['url'], '%s  %s' % (ch.get('num', ''), ch['nome']))

    def desenha_conv(self, c):
        """teste de convergencia: borda inteira, cantos e cruz no centro (sumiu na rv04)"""
        c.rect(0, 0, W, 2, ORANGE); c.rect(0, H - 2, W, 2, ORANGE)
        c.rect(0, 0, 1, H, ORANGE); c.rect(W - 1, 0, 1, H, ORANGE)
        for x, y in ((0, 0), (W - 24, 0), (0, H - 6), (W - 24, H - 6)):
            c.rect(x, y, 24, 6, ORANGE)
        for x, y in ((0, 0), (W - 6, 0), (0, H - 24), (W - 6, H - 24)):
            c.rect(x, y, 6, 24, ORANGE)
        c.rect(W // 2 - 10, H // 2, 21, 1, IVORY); c.rect(W // 2, H // 2 - 10, 1, 21, IVORY)
        c.ctext(40, 'CONVERGENCIA', IVORY)
        c.ctext(56, 'AJUSTE ATE VER OS 4 CANTOS', SLATE)
        c.ctext(66, 'LARANJA E A BORDA INTEIRA', SLATE)
        x, y, w, h = self.conv
        c.ctext(140, 'X %+d  Y %+d' % (x, y), ORANGE)
        c.ctext(152, 'LARGURA %d%%  ALTURA %d%%' % (round(w * 100), round(h * 100)), ORANGE)
        c.ctext(168, 'SETAS: POSICAO', SLATE)
        c.ctext(178, 'LB/RB: LARGURA  LT/RT: ALTURA', SLATE)
        c.ctext(188, 'SELECT: PADRAO', SLATE)
        c.ctext(204, 'A: SALVAR   B: VOLTAR', IVORY)
        if time.monotonic() < getattr(self, 'salvo', 0):
            c.ctext(86, 'SALVO!', GREEN)

    def desenha_video(self, c):
        moldura(c, 'VIDEO', '<- -> MUDA  A APLICA  B VOLTA')
        atual = 'TUBO 240P' if self.fb.tubo else 'LCD %dX%d' % (self.fb.w, self.fb.h)
        c.text(24, 56, 'AGORA: %s' % atual, GREEN)
        y = 66 + F.ch
        for n, (k, nome, v) in enumerate(self.itens_video()):
            on = n == getattr(self, 'video_sel', 0)
            if k == 'modo' and v == 'AUTO':
                v = 'AUTO (NATIVA)'
            item(c, 24, y, nome, on, ('< %s >' if on else '%s') % v)
            y += F.ch + 10
        ajuda = ['AUTO: O PI VE SE E TUBO OU LCD', 'SOZINHO E REINICIA SE PRECISAR.',
                 'SEM IMAGEM? SEGURE SELECT 5 S', 'PARA TROCAR TUBO/LCD.']
        if not self.fb.tubo:
            ajuda += ['RESOLUCAO NOVA SEM IMAGEM? ESPERE', '30 S QUE ELA VOLTA SOZINHA.']
        passo = min(F.ch + 3, (H - 16 - (y + 4)) // len(ajuda))
        for n, l in enumerate(ajuda):
            c.text(24, y + 4 + n * passo, l, SLATE)
        if self.confirma == 'VIDEO':
            caixa(c, ['REINICIAR AGORA? A SIM  B NAO'])

    def acao_video(self, a):
        if self.confirma == 'VIDEO':
            if a == 'A':
                self.confirma = None
                k = self.itens_video()[getattr(self, 'video_sel', 0)][0]
                if k == 'modo':
                    self.aplica_modo()
                else:
                    self.aplica_video(self.cfg.get('video_saida', 'AUTO').upper(), manual=True)
            elif a == 'B':
                self.confirma = None
            return
        itens = self.itens_video()
        sel = min(getattr(self, 'video_sel', 0), len(itens) - 1)
        k = itens[sel][0]
        if a in ('CIMA', 'BAIXO'):
            self.video_sel = (sel + (1 if a == 'BAIXO' else -1)) % len(itens)
        elif a == 'B':
            self.cfg['video_modo'] = self.video_modo_ok      # resolucao mexida e nao aplicada: desfaz
            self.tela = 'menu'
        elif a in ('ESQ', 'DIR'):
            d = 1 if a == 'DIR' else -1
            if k == 'saida':
                lst, ck = self.SAIDAS, 'video_saida'
            elif k == 'filtro':
                lst, ck = self.FILTROS, 'video_filtro'
            else:
                if not getattr(self, '_modos', None):
                    ms = video.modos_lcd()
                    if not ms:      # monitor nao informou (adaptador, cabo): lista comum
                        print('video: monitor nao informou resolucoes; usando a lista comum', flush=True)
                        ms = [(1920, 1080), (1680, 1050), (1600, 900), (1440, 900), (1366, 768), (1280, 1024),
                              (1280, 960), (1280, 800), (1280, 720), (1024, 768), (800, 600), (640, 480)]
                    self._modos = ['AUTO'] + ['%dX%d' % m for m in ms]
                    print('video: resolucoes oferecidas: %s' % ' '.join(self._modos), flush=True)
                lst, ck = self._modos, 'video_modo'
            v = self.cfg.get(ck, lst[0]).upper()
            v = lst[(lst.index(v) + d) % len(lst)] if v in lst else lst[0]
            self.cfg[ck] = v
            if k != 'modo':
                cfg_gravar(self.cfg)
        elif a == 'A' and k in ('saida', 'modo'):
            self.confirma = 'VIDEO'

    def modo_cfg(self):
        v = self.cfg.get('video_modo', 'AUTO').upper()
        try:
            w, h = (int(x) for x in v.split('X'))
            return (w, h)
        except ValueError:
            return None

    def aplica_modo(self):
        """troca a resolucao do LCD; no proximo boot pede confirmacao (15 s).
        A configuracao continua com a resolucao anterior ate a pessoa confirmar."""
        nome = self.cfg.get('video_modo', 'AUTO').upper()
        modo = self.modo_cfg()
        self.cfg['video_modo'] = self.video_modo_ok     # definitiva so depois do "MANTER? A"
        cfg_gravar(self.cfg)
        if modo is None:      # AUTO: a nativa do monitor (boot sem resolucao forcada)
            _, modo, _ = video.decide('LCD')
        print('video: resolucao escolhida %s (%s), aguardando confirmacao' % (modo, nome), flush=True)
        if video.testa_modo(modo, nome):
            c = Canvas()
            c.ctext(H // 2 - 10, ('MUDANDO PARA %dX%d...' % modo) if modo else 'MUDANDO PARA A NATIVA...', ORANGE)
            c.ctext(H // 2 + F.ch, 'O PI VAI REINICIAR', IVORY)
            self.fb.show(c, self.conv)
            copia_log()
            os.sync()
            time.sleep(2)
            subprocess.Popen(['systemctl', 'reboot'])
        else:
            ok, _ = video.pode_trocar()
            print('video: troca nao feita (%s)' % ('ja estava nela' if ok else 'trocas demais'), flush=True)
            self.mostra_aviso('JA ESTA NESSA RESOLUCAO' if ok else 'ESPERE 10 MINUTOS PARA TROCAR DE NOVO')

    def aplica_video(self, forcado, manual=False):
        """decide a saida (AUTO = pela EDID) e, se mudar, troca o boot e reinicia"""
        if self.fb.fake:
            return
        try:
            tela = None if manual else self.cfg.get('video_saida_tela') or None
            saida, modo, motivo = video.decide(forcado, self.modo_cfg(), tela)
            print('video: %s -> %s %s (%s)' % (video.modo_atual(), saida, modo or '', motivo), flush=True)
            # escolha feita a mao agora: vale para ESTA tela (identidade guardada)
            if manual:
                novo = video.ULTIMA_TELA if forcado in ('TUBO', 'LCD') and video.ULTIMA_TELA else ''
                if self.cfg.get('video_saida_tela', '') != novo:
                    self.cfg['video_saida_tela'] = novo
                    cfg_gravar(self.cfg)
            if saida and video.troca(saida, modo):
                c = Canvas()
                c.ctext(110, 'MUDANDO PARA %s...' % saida.upper(), ORANGE)
                c.ctext(110 + F.ch + 6, 'O PI VAI REINICIAR', IVORY)
                self.fb.show(c, self.conv)
                copia_log()
                os.sync()
                time.sleep(2)
                subprocess.Popen(['systemctl', 'reboot'])
            elif saida is None and manual:
                self.mostra_aviso('NAO CONSEGUI LER O APARELHO (EDID)')
        except Exception as ex:
            print('video: erro', ex, flush=True)

    def _vigia_tela(self):
        """tela trocada com o Pi ligado (ex.: tirou o cabo da TV e ligou no LCD): percebe pela
        EDID (2 leituras iguais) e chama a mesma decisao do boot (troca e reinicia se precisar).
        So no menu; em jogo e durante teste de resolucao nao mexe."""
        time.sleep(20)                       # o boot ja decidiu; comeca depois
        ultima = None
        while True:
            time.sleep(5)
            if self.tela != 'menu' or self.leitora.pausa.is_set() or os.path.exists(video.TESTE):
                continue
            try:
                ident = video.identidade(video.analisa(video.edid_verdadeira()[0]))
            except Exception:
                continue
            if not ident:
                continue
            if ultima is None or ident == ultima:
                ultima = ident
                continue
            time.sleep(1)
            try:
                if video.identidade(video.analisa(video.edid_verdadeira()[0])) != ident:
                    continue
            except Exception:
                continue
            print('video: tela trocada com o Pi ligado: %s -> %s' % (ultima, ident), flush=True)
            ultima = ident
            self.fila.put('_tela_trocou')

    def troca_cega(self):
        """SELECT segurado 5 s: inverte TUBO/LCD (para quando a tela nao mostra nada)"""
        novo = 'LCD' if self.fb.tubo else 'TUBO'
        print('video: SELECT segurado: forcando %s' % novo, flush=True)
        self.cfg['video_saida'] = novo
        cfg_gravar(self.cfg)
        try:
            os.remove(video.TROCAS)       # pedido da pessoa: sem limite de trocas
        except OSError:
            pass
        self.aplica_video(novo, manual=True)

    # ---------------------------------------------------------- wi-fi
    def wifi_tarefa(self, func, msg):
        """roda func numa thread (nmcli demora); a tela mostra AGUARDE"""
        if self.wf['ocupado']:
            return
        self.wf['ocupado'] = True
        self.wf['msg'] = msg

        def roda():
            try:
                func()
            except Exception as ex:
                print('wifi: erro', ex, flush=True)
                self.wf['msg'] = 'ERRO NO WI-FI'
            self.wf['ocupado'] = False
            self.wf['rede'], self.wf['ip'] = wifi.conectada()
            self.wifi = self.wf['rede']
            self.fila.put('_wifi')
        threading.Thread(target=roda, daemon=True).start()

    def wifi_busca(self):
        def f():
            self.wf['lista'] = wifi.busca()
            self.wf['msg'] = '%d REDES ENCONTRADAS' % len(self.wf['lista'])
        self.wifi_tarefa(f, 'BUSCANDO REDES...')

    def wifi_conecta(self, nome, senha):
        def f():
            ok, msg = wifi.conecta(nome, senha)
            print('wifi: conectar %s: %s' % (nome, msg), flush=True)   # nunca a senha
            self.wf['msg'] = msg
            if ok:
                self.tela = 'wifi'
                self.ent.modo_texto = False
        self.wifi_tarefa(f, 'CONECTANDO...')

    def acao_wifi(self, a):
        if self.wf['ocupado']:
            return
        itens = self.itens_wifi()
        if a == 'CIMA':
            self.wf['sel'] = (self.wf['sel'] - 1) % len(itens)
        elif a == 'BAIXO':
            self.wf['sel'] = (self.wf['sel'] + 1) % len(itens)
        elif a == 'B':
            self.tela = 'menu'
        elif a == 'A':
            k, nome, _, prot = itens[min(self.wf['sel'], len(itens) - 1)]
            self.wf['msg'] = ''
            if k == 'acao':
                self.wifi_busca()
            elif k == 'esquecer':
                rede = self.wf['rede']
                self.wifi_tarefa(lambda: wifi.esquece(rede), 'ESQUECENDO...')
            elif not prot:
                self.wifi_conecta(nome, None)
            else:
                self.senha = ''
                self.tec.update(x=0, y=1, maius=False, rede=nome)
                self.tela = 'teclado'
                self.ent.modo_texto = True

    def acao_teclado(self, a):
        if self.wf['ocupado']:
            return
        linhas = TECLAS[self.tec['maius']]
        t = self.tec
        if a.startswith('_txt:'):
            self.senha += a[5:]
            return
        if a in ('_apaga', 'B') or a == 'X':
            if self.senha:
                self.senha = self.senha[:-1]
            elif a == 'B':
                self.sai_teclado()
            return
        if a == '_sai':
            self.sai_teclado()
            return
        if a in ('_ok', 'START'):
            if self.senha:
                self.wifi_conecta(t['rede'], self.senha)
            return
        if a == 'Y':
            t['maius'] = not t['maius']
            return
        n_lin = len(linhas) + 1
        if a == 'CIMA':
            t['y'] = (t['y'] - 1) % n_lin
        elif a == 'BAIXO':
            t['y'] = (t['y'] + 1) % n_lin
        elif a in ('ESQ', 'DIR'):
            n = len(TECLAS_FIM) if t['y'] == len(linhas) else 10
            t['x'] = (t['x'] + (1 if a == 'DIR' else -1)) % n
        elif a == 'A':
            if t['y'] < len(linhas):
                self.senha += linhas[t['y']][min(t['x'], 9)]
            else:
                k = TECLAS_FIM[min(t['x'], len(TECLAS_FIM) - 1)]
                if k == 'ABC':
                    t['maius'] = not t['maius']
                elif k == 'ESPACO':
                    self.senha += ' '
                elif k == 'APAGAR':
                    self.senha = self.senha[:-1]
                elif k == 'OK' and self.senha:
                    self.wifi_conecta(t['rede'], self.senha)
        if t['y'] == len(linhas):
            t['x'] = min(t['x'], len(TECLAS_FIM) - 1)

    def sai_teclado(self):
        self.senha = ''
        self.ent.modo_texto = False
        self.tela = 'wifi'

    # ---------------------------------------------------------- pastas de jogos
    def acao_pastas(self, a):
        if self.confirma == 'COPIAR':
            if a in ('A', 'B'):
                self.confirma = None
                self.muda_pasta(self.pastas_destino, copiar=(a == 'A'))
            return
        if a in ('CIMA', 'BAIXO'):
            self.pastas_sel = 1 - self.pastas_sel
        elif a == 'B':
            self.tela = 'menu'
            self.pastas_msg = ''
        elif a == 'A':
            destino = ('pendrive', 'cartao')[self.pastas_sel]
            atual = 'pendrive' if self.base.startswith(USB) else 'cartao'
            if destino == atual:
                self.pastas_msg = 'JA ESTA EM USO'
                return
            if destino == 'pendrive' and not tem_pendrive():
                self.pastas_msg = 'PENDRIVE NAO ENCONTRADO'
                return
            self.pastas_destino = destino
            self.confirma = 'COPIAR'

    def muda_pasta(self, destino, copiar):
        antiga = self.base
        self.cfg['pasta_jogos'] = destino
        self.base = base(destino)
        cfg_gravar(self.cfg)
        n = copia_sem_sobrescrever(antiga, self.base) if copiar else 0
        print('pastas: %s -> %s (%d arquivos copiados)' % (antiga, self.base, n), flush=True)
        self.pastas_msg = 'PRONTO' + (': %d ARQUIVOS COPIADOS' % n if copiar else '')
        self.sistema = None

    def falha_tela(self, onde):
        """erro numa tela: registra no LOG e volta ao menu (o menu nunca cai por isso)"""
        import traceback
        print('ERRO na tela %s (%s):\n%s' % (self.tela, onde, traceback.format_exc()), flush=True)
        nome = self.tela.upper()
        try:                                  # TV: fecha previa/tela cheia
            self.tv_para_previa()
            if getattr(self, 'tv_cheia', None):
                self.tv_cheia.fecha()
            self.leitora.pausa.clear()
        except Exception:
            pass
        self.tela = 'menu'
        self.confirma = None
        self.mostra_aviso('ERRO NA TELA %s: VOLTEI AO MENU (VEJA O LOG)' % nome)

    def acao_segura(self, a):
        try:
            self.acao(a)
        except Exception:
            self.falha_tela('botao %s' % a)

    def mostra_seguro(self):
        try:
            self.fb.show(self.desenha(), self.conv)
        except Exception:
            self.falha_tela('desenho')
            self.fb.show(self.desenha(), self.conv)

    def acao(self, a):
        t = self.tela
        if t == 'tv_cheia':
            self.acao_tv_cheia(a)
            return
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
            if self.tela == 'menu':
                self.ajusta_altura()
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
        elif t == 'video':
            self.acao_video(a)
        elif t == 'teste_video':
            if a in ('A', 'START'):
                nome = video.confirma_teste()
                self.cfg['video_modo'] = nome
                self.video_modo_ok = nome
                cfg_gravar(self.cfg)
                print('video: resolucao confirmada: %s' % nome, flush=True)
                self.tela = 'menu'
                self.mostra_aviso('RESOLUCAO MANTIDA')
        elif t == 'wifi':
            self.acao_wifi(a)
        elif t == 'teclado':
            self.acao_teclado(a)
        elif t == 'pastas':
            self.acao_pastas(a)

    def _info_fundo(self):
        try:
            self.info = info()
            if self.info.get('wifi'):
                self.wifi = self.info.get('wifi')
        except Exception:
            pass
        self.fila.put('_wifi')

    def opcoes(self):
        """itens de OPCOES: CONVERGENCIA so no tubo (no LCD a imagem ja sai exata)"""
        return [o for o in OPCOES if o != 'CONVERGENCIA' or self.fb.tubo]

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
        caixa(c, [txt, 'B CANCELA'], [IVORY, SLATE])

    def disco_novo(self):
        """disco entrou: no menu, pergunta se quer iniciar"""
        i = self.leitora.info or {}
        sis = i.get('sistema')
        print('disco novo: %s' % i, flush=True)
        if self.tela != 'menu' or self.confirma:
            self.mostra_aviso('DISCO: %s' % (i.get('nome') or NOME_SISTEMA.get(sis, '?')))
            return
        self.aba = 'DISCO'
        if sis in SISTEMAS:      # pergunta antes de abrir (A SIM / B NAO)
            self.pergunta_disco = i
        elif sis == 'audio':
            self.mostra_aviso('CD DE AUDIO')

    def _pendentes(self):
        out = []
        try:
            while True:
                out.append(self.fila.get_nowait())
        except queue.Empty:
            pass
        return out

    def _wifi(self):
        """nome do Wi-Fi no topo: a rede pode subir depois do menu; procura a cada 5 s ate
        achar, depois a cada 30 s; uma consulta que falha nao apaga o nome (so 3 seguidas)"""
        falhas = 0
        while True:
            time.sleep(30 if self.wifi else 5)
            try:
                nome = info().get('wifi')
            except Exception:
                nome = None
            if nome:
                falhas = 0
                if nome != self.wifi:
                    self.wifi = nome
                    self.fila.put('_wifi')
            else:
                falhas += 1
                if falhas >= 3 and self.wifi:
                    self.wifi = None
                    self.fila.put('_wifi')

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
                self.mostra_seguro()
                return
            tempo('preparado (configuracao, pastas, controles)')
            self.mostra_seguro()
            tempo('primeira tela desenhada')
            os.makedirs(os.path.dirname(OK_FILE), exist_ok=True)
            open(OK_FILE, 'w').write(VERSAO + '\n')
            if os.path.exists(video.TESTE):        # resolucao nova: confirma em 15 s
                self.tela = 'teste_video'
                self.teste_ate = time.monotonic() + 30
            else:
                # tubo ou LCD? (le a EDID verdadeira; se nao combinar com o boot, troca e reinicia)
                threading.Thread(target=self.aplica_video, args=(self.cfg.get('video_saida', 'AUTO').upper(),),
                                 daemon=True).start()
            threading.Thread(target=self._info_fundo, daemon=True).start()
            threading.Thread(target=reativa_dongle_8bitdo, daemon=True).start()
            threading.Thread(target=instala_guarda_video, daemon=True).start()
            threading.Thread(target=self._vigia_tela, daemon=True).start()
            ultimo_desenho = time.monotonic()
            while True:
                try:
                    rolando = self.aviso and len(self.aviso[0]) > (W - 44) // F.cw
                    curto = (self.contagem or self.tela in ('teste_video', 'tv_cheia') or getattr(self, 'tv_previa', None)
                             or getattr(self, 'tv_pre', 0))
                    a = self.fila.get(timeout=0.25 if curto else 0.1 if rolando else 1.0)
                except queue.Empty:
                    a = None
                mudou = bool(self.contagem) or bool(self.aviso and len(self.aviso[0]) > (W - 44) // F.cw)
                for b in [a] + self._pendentes():
                    if b is None:
                        continue
                    if b == '_raw':
                        mudou = mudou or self.tela == 'controle'
                    elif b == '_disco':
                        if self.leitora.estado != 'ok':
                            self.pergunta_disco = None
                        mudou = True
                    elif b in ('_wifi', '_img', '_tv'):
                        mudou = True
                    elif b.startswith('_controle:'):
                        self.mostra_aviso('CONTROLE CONECTADO: ' + b[10:].upper())
                        mudou = True
                    elif b.startswith('_txt:') or b in ('_ok', '_apaga', '_sai'):
                        if self.tela == 'teclado':
                            self.acao_teclado(b)
                        mudou = True
                    elif b == '_tela_trocou':        # cabo trocado com o Pi ligado
                        threading.Thread(target=self.aplica_video, args=(self.cfg.get('video_saida', 'AUTO').upper(),),
                                         daemon=True).start()
                    elif b == '_disco_novo':
                        self.disco_novo()
                        mudou = True
                    else:
                        self.acao_segura(b)
                        mudou = True
                self.leitora.quieta = self.tela == 'conv'     # convergencia: nada de leitora
                try:
                    mudou = self.tv_vigia() or mudou
                except Exception:
                    self.falha_tela('tv')
                if self.tela == 'teste_video':
                    mudou = True
                    if time.monotonic() > self.teste_ate:
                        if video.desfaz_teste():
                            copia_log()
                            subprocess.Popen(['systemctl', 'reboot'])
                        self.tela = 'menu'
                if self.ent.select_desde and time.monotonic() - self.ent.select_desde > 5:
                    self.ent.select_desde = 0
                    self.troca_cega()
                if self.vol_sujo and time.monotonic() - self.vol_sujo > 1.5:
                    self.vol_sujo = 0
                    cfg_gravar(self.cfg)
                if self.contagem and time.monotonic() >= self.contagem[1]:
                    self.fim_contagem()
                    continue
                if self.aviso and time.monotonic() > self.aviso[1]:
                    self.aviso = None
                    mudou = True
                if self.tela == 'tv_cheia':               # o mpv e dono da tela
                    continue
                if mudou or time.monotonic() - ultimo_desenho > 60:
                    if time.monotonic() - ultimo_desenho > 60:
                        copia_log()
                    self.mostra_seguro()
                    ultimo_desenho = time.monotonic()
        finally:
            if not fake:
                tty_graphics(False)


if __name__ == '__main__':
    App().run()
