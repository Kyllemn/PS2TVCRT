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
import json
import shutil
import imagens
import tv
import youtube
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
    ('run_ahead', 'SIM', 'SNES, arcades, NES, Game Boy, GBA e Neo Geo Pocket: SIM = 1 quadro a menos de atraso no controle'),
    ('som_ganho_gba', '160', 'GBA: volume extra em %, 100 = sem ganho (o som do GBA e baixo de fabrica)'),
    ('ds_duas_telas', 'SIM', 'Nintendo DS com 2 monitores ligados: SIM = tela de baixo no 2o HDMI; NAO = uma tela so'),
    ('leitora_cache_saturn', '512', 'Saturno: MB de memoria com o disco (lido pelo jogo e copiado em 2o plano)'),
    ('leitora_cache_dreamcast', '512', 'Dreamcast: MB de memoria com o disco (lido pelo jogo e copiado em 2o plano; menos picotes)'),
    ('tubo_480i', 'SIM', 'tubo: jogo em 480i sai em 480i de verdade (SIM) ou sempre 240p (NAO)'),
    ('dreamcast_gpu', 'VULKAN', 'Dreamcast: VULKAN (padrao) ou GLES (reserva, se o Vulkan der problema)'),
    ('botoes_icones', 'AUTO', 'icones dos botoes no menu: AUTO (pelo controle), XBOX, PLAYSTATION ou NINTENDO (so o desenho)'),
    ('menu_efeito', 'NENHUM', 'so no LCD: efeito no menu principal: NENHUM, SCANLINES LEVE, SCANLINES FORTE, CRT ou DOT MATRIX'),
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
    ('video_filtro', 'NENHUM', 'so no LCD: NENHUM, SCANLINES, SCANLINES LEVES, CRT, SUAVE, SCALE2X, SCALE3X, XBR ou DOT MATRIX (padrao; cada nucleo pode ter o seu)'),
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


# ---------------------------------------------------------------- icones dos botoes
# So o DESENHO muda: a acao e a mesma (o botao de baixo do controle e o "A" do menu e aparece
# como A verde no Xbox, X azul no PlayStation e B no Nintendo; o da direita como B, O e A...).
# Formas vetoriais com borda suavizada (varias amostras por ponto). No LCD o icone e desenhado
# nos pontos do monitor (3x mais detalhe, letras vetoriais); no tubo, no proprio menu, com a
# letra na fonte do menu (mais nitida em 11 pontos).
ICONES_ESC = None          # LCD: pontos do monitor por ponto do menu (App)
ICONES_LAYOUT = 'xbox'     # xbox | playstation | nintendo (App, a cada desenho)
_BT = ('A', 'B', 'X', 'Y', 'L1', 'R1', 'L2', 'R2', 'LB', 'RB', 'LT', 'RT', 'START', 'SELECT')
_BT_APELIDO = {'LB': 'L1', 'RB': 'R1', 'LT': 'L2', 'RT': 'R2'}

# letras em tracos (caixa de altura 1; largura, lista de linhas)
_GLIFOS = {
    'A': (0.84, [[(0, 1), (0.42, 0), (0.84, 1)], [(0.17, 0.66), (0.67, 0.66)]]),
    'B': (0.72, [[(0, 0), (0, 1), (0.46, 1), (0.63, 0.95), (0.72, 0.82), (0.72, 0.68), (0.63, 0.55),
                  (0.46, 0.49), (0, 0.49)],
                 [(0, 0), (0.41, 0), (0.57, 0.05), (0.65, 0.16), (0.65, 0.3), (0.57, 0.43), (0.44, 0.49)]]),
    'X': (0.82, [[(0, 0), (0.82, 1)], [(0.82, 0), (0, 1)]]),
    'Y': (0.82, [[(0, 0), (0.41, 0.52), (0.82, 0)], [(0.41, 0.52), (0.41, 1)]]),
    'L': (0.6, [[(0, 0), (0, 1), (0.6, 1)]]),
    'R': (0.74, [[(0, 1), (0, 0), (0.43, 0), (0.6, 0.06), (0.69, 0.19), (0.69, 0.31), (0.6, 0.44),
                  (0.43, 0.5), (0, 0.5)], [(0.38, 0.5), (0.74, 1)]]),
    'T': (0.72, [[(0, 0), (0.72, 0)], [(0.36, 0), (0.36, 1)]]),
    'Z': (0.72, [[(0, 0), (0.72, 0), (0, 1), (0.72, 1)]]),
    '1': (0.52, [[(0.06, 0.22), (0.32, 0), (0.32, 1)], [(0.04, 1), (0.56, 1)]]),
    '2': (0.68, [[(0.02, 0.24), (0.12, 0.07), (0.34, 0), (0.56, 0.07), (0.66, 0.23), (0.62, 0.42),
                  (0.0, 1), (0.68, 1)]]),
}

_CINZA_ARO, _CINZA_FUNDO, _BRANCO = (128, 133, 150), (52, 56, 68), (240, 241, 245)


def _poli(pts, w, cor, fechado=False):
    if fechado:
        pts = list(pts) + [pts[0]]
    return [(cor, ('s', pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], w)) for i in range(len(pts) - 1)]


def _rotulo_prims(txt, wu, cor, alt=0.44, sw=0.11):
    """letras em tracos, centralizadas (unidades: altura do icone = 1)"""
    esp = 0.3 * alt                     # espaco entre letras
    larg = sum(_GLIFOS[ch][0] for ch in txt) * alt + esp * (len(txt) - 1)
    x = (wu - larg) / 2.0
    y0 = 0.5 - alt / 2.0
    out = []
    for ch in txt:
        gw, linhas = _GLIFOS[ch]
        for l in linhas:
            out += _poli([(x + px * alt, y0 + py * alt) for px, py in l], sw, cor)
        x += gw * alt + esp
    return out


def _pxr(x0, y0, x1, y1, alt, cor):
    """retangulo alinhado aos pontos (icone pequeno do tubo: traco de 1 ponto nitido)"""
    return (cor, ('rr', x0 / float(alt), y0 / float(alt), x1 / float(alt), y1 / float(alt), 0))


def _miudo(lay, nome, wu, alt, prims):
    """START/SELECT do Xbox e do PlayStation com 11 pontos: os tracos finos viram mancha;
    aqui viram pontos inteiros (como pixel art)"""
    base = prims[:2]                   # aro e fundo
    w = int(round(wu * alt))
    cx = w // 2
    l3 = [int(round(alt * f)) for f in (.3, .5, .7)]
    if nome == 'START':                # tres tracos
        return base + [_pxr(cx - 2, y, cx + 3 - (w % 2 == 0), y + 1, alt, _BRANCO) for y in l3]
    if lay == 'xbox':                  # VIEW: quadrado vazado atras e cheio na frente
        out = base + [_pxr(cx - 3, l3[0], cx + 1, l3[0] + 1, alt, _BRANCO), _pxr(cx - 3, l3[0], cx - 2, l3[1] + 1, alt, _BRANCO),
                      _pxr(cx - 1, l3[1] - 1, cx + 3, l3[2] + 1, alt, _BRANCO)]
        return out
    a = l3[0]                          # PlayStation SHARE: seta para cima
    return base + [_pxr(cx, a, cx + 1, a + 1, alt, _BRANCO), _pxr(cx - 1, a + 1, cx + 2, a + 2, alt, _BRANCO),
                   _pxr(cx - 2, a + 2, cx + 3, a + 3, alt, _BRANCO), _pxr(cx, a + 3, cx + 1, l3[2] + 1, alt, _BRANCO)]


def _icone_def(lay, nome, alt=None):
    """(largura em unidades, formas, rotulo, cor do rotulo); formas = [(cor, forma)]"""
    face = {'A': 0, 'B': 1, 'X': 2, 'Y': 3}           # baixo, direita, esquerda, cima
    if nome in face:
        k = face[nome]
        if lay == 'playstation':
            prims = [(_CINZA_ARO, ('c', .5, .5, .5)), ((26, 28, 36), ('c', .5, .5, .43))]
            if k == 0:      # X azul
                prims += _poli([(.33, .33), (.67, .67)], .1, (120, 176, 242))
                prims += _poli([(.67, .33), (.33, .67)], .1, (120, 176, 242))
            elif k == 1:    # O vermelho
                prims.append(((242, 98, 98), ('an', .5, .5, .19, .095)))
            elif k == 2:    # quadrado rosa
                prims += _poli([(.33, .33), (.67, .33), (.67, .67), (.33, .67)], .09, (228, 142, 218), True)
            else:           # triangulo verde
                prims += _poli([(.5, .29), (.71, .66), (.29, .66)], .09, (82, 216, 166), True)
            return 1.0, prims, '', None
        if lay == 'nintendo':
            letra = 'BAYX'[k]
            return 1.0, [(_CINZA_ARO, ('c', .5, .5, .5)), (_CINZA_FUNDO, ('c', .5, .5, .43))], letra, _BRANCO
        cor = [(86, 176, 62), (218, 64, 52), (56, 124, 226), (236, 184, 32)][k]
        aro = tuple(int(v * .62) for v in cor)
        return 1.0, [(aro, ('c', .5, .5, .5)), (cor, ('c', .5, .5, .43))], 'ABXY'[k], \
            ((60, 44, 0) if k == 3 else _BRANCO)
    if nome in ('L1', 'R1', 'L2', 'R2'):
        rot = {'xbox': {'L1': 'LB', 'R1': 'RB', 'L2': 'LT', 'R2': 'RT'},
               'nintendo': {'L1': 'L', 'R1': 'R', 'L2': 'ZL', 'R2': 'ZR'}}.get(lay, {}).get(nome, nome)
        wu = 0.7 + 0.44 * len(rot)
        if nome in ('L1', 'R1'):     # botao de ombro: pilula
            prims = [(_CINZA_ARO, ('rr', 0, .06, wu, .94, .44)), (_CINZA_FUNDO, ('rr', .07, .13, wu - .07, .87, .37))]
        else:                        # gatilho: mais quadrado e mais alto
            prims = [(_CINZA_ARO, ('rr', 0, 0, wu, 1, .26)), (_CINZA_FUNDO, ('rr', .07, .07, wu - .07, .93, .2))]
        return wu, prims, rot, _BRANCO
    # START / SELECT
    if lay == 'playstation':
        wu = 1.15
        prims = [(_CINZA_ARO, ('rr', .02, .14, wu - .02, .86, .36)), (_CINZA_FUNDO, ('rr', .09, .21, wu - .09, .79, .29))]
        if nome == 'START':          # OPTIONS: tres tracos
            for yy in (.38, .5, .62):
                prims += _poli([(.37, yy), (.78, yy)], .065, _BRANCO)
        else:                        # SHARE/CREATE: seta para cima
            prims += _poli([(.575, .68), (.575, .34)], .075, _BRANCO)
            prims += _poli([(.43, .48), (.575, .33), (.72, .48)], .075, _BRANCO)
        if alt and alt < 16:
            prims = _miudo(lay, nome, wu, alt, prims)
        return wu, prims, '', None
    prims = [(_CINZA_ARO, ('c', .5, .5, .5)), (_CINZA_FUNDO, ('c', .5, .5, .43))]
    if lay == 'nintendo':
        prims += _poli([(.3, .5), (.7, .5)], .1, _BRANCO)
        if nome == 'START':
            prims += _poli([(.5, .3), (.5, .7)], .1, _BRANCO)
    elif nome == 'START':            # Xbox MENU: tres tracos
        for yy in (.37, .5, .63):
            prims += _poli([(.32, yy), (.68, yy)], .07, _BRANCO)
    else:                            # Xbox VIEW: dois quadrados
        prims += _poli([(.31, .31), (.58, .31), (.58, .58), (.31, .58)], .06, _BRANCO, True)
        prims.append((_BRANCO, ('rr', .42, .42, .7, .7, .04)))
    if lay == 'xbox' and alt and alt < 16:
        prims = _miudo(lay, nome, 1.0, alt, prims)
    return 1.0, prims, '', None


def _caixa_forma(f):
    t = f[0]
    if t == 'c':
        return f[1] - f[3], f[2] - f[3], f[1] + f[3], f[2] + f[3]
    if t == 'an':
        r = f[3] + f[4] / 2
        return f[1] - r, f[2] - r, f[1] + r, f[2] + r
    if t == 'rr':
        return f[1], f[2], f[3], f[4]
    w = f[5] / 2
    return min(f[1], f[3]) - w, min(f[2], f[4]) - w, max(f[1], f[3]) + w, max(f[2], f[4]) + w


def _dentro(f, x, y):
    t = f[0]
    if t == 'c':
        return (x - f[1]) ** 2 + (y - f[2]) ** 2 <= f[3] * f[3]
    if t == 'an':
        d2 = (x - f[1]) ** 2 + (y - f[2]) ** 2
        return (f[3] - f[4] / 2) ** 2 <= d2 <= (f[3] + f[4] / 2) ** 2
    if t == 'rr':
        _, x0, y0, x1, y1, r = f
        if x < x0 or x > x1 or y < y0 or y > y1:
            return False
        dx = max(x0 + r - x, 0, x - (x1 - r))
        dy = max(y0 + r - y, 0, y - (y1 - r))
        return dx * dx + dy * dy <= r * r
    _, ax, ay, bx, by, w = f                 # traco com pontas redondas
    vx, vy = bx - ax, by - ay
    l2 = vx * vx + vy * vy
    u = 0 if l2 == 0 else max(0.0, min(1.0, ((x - ax) * vx + (y - ay) * vy) / l2))
    dx, dy = x - (ax + u * vx), y - (ay + u * vy)
    return dx * dx + dy * dy <= w * w / 4


_ICONES_CACHE = {}


def icone_img(lay, nome, alt, com_rotulo):
    """(largura, altura, [None | (r, g, b, a)]) em pontos; a de 0 a 1"""
    chave = (lay, nome, alt, com_rotulo)
    img = _ICONES_CACHE.get(chave)
    if img is not None:
        return img
    wu, prims, rot, cor_rot = _icone_def(lay, nome, alt)
    if com_rotulo and rot:
        prims = prims + (_rotulo_prims(rot, wu, cor_rot) if len(rot) == 1 else
                         _rotulo_prims(rot, wu, cor_rot, alt=0.38, sw=0.085))
    if alt < 16:                       # tubo (11 pontos): tracos 1,5x mais grossos, senao somem
        grosso = []
        for cor, f in prims:
            if f[0] == 's':
                f = f[:5] + (f[5] * 1.5,)
            elif f[0] == 'an':
                f = f[:4] + (f[4] * 1.5,)
            grosso.append((cor, f))
        prims = grosso
    w = max(1, int(round(wu * alt)))
    n = 4 if alt < 24 else 3
    offs = [(i + 0.5) / n for i in range(n)]
    inv = 1.0 / alt
    caixas = []
    for cor, f in prims:
        x0, y0, x1, y1 = _caixa_forma(f)
        caixas.append((cor, f, x0 * alt - 1, y0 * alt - 1, x1 * alt + 1, y1 * alt + 1))
    px = []
    for py in range(alt):
        for qx in range(w):
            cand = [(cor, f) for cor, f, a, b, c2, d in caixas if a <= qx + 1 and c2 >= qx and b <= py + 1 and d >= py]
            if not cand:
                px.append(None)
                continue
            r = g = b = 0
            hits = 0
            for sy in offs:
                uy = (py + sy) * inv
                for sx in offs:
                    ux = (qx + sx) * inv
                    cor = None
                    for c3, f in cand:
                        if _dentro(f, ux, uy):
                            cor = c3
                    if cor:
                        r += cor[0]
                        g += cor[1]
                        b += cor[2]
                        hits += 1
            px.append((r // hits, g // hits, b // hits, hits / float(n * n)) if hits else None)
    img = (w, alt, px)
    if len(_ICONES_CACHE) > 200:
        _ICONES_CACHE.clear()
    _ICONES_CACHE[chave] = img
    return img


def icone_largura(nome):
    """largura do icone em pontos do menu"""
    return max(1, int(round(_icone_def(ICONES_LAYOUT, nome)[0] * (F.ch + 3))))


def icone(c, x, y, nome):
    """desenha o icone do botao com o topo em y (altura F.ch+3); devolve a largura"""
    alt = F.ch + 3
    wl = icone_largura(nome)
    if ICONES_ESC:                  # LCD: nos pontos do monitor, por cima do menu
        c.extras.append((x, y, wl, alt, icone_img(ICONES_LAYOUT, nome, alt * ICONES_ESC, True), False, 'icone'))
        return wl
    w, h, px = icone_img(ICONES_LAYOUT, nome, alt, False)
    for j in range(h):
        yy = y + j
        if not 0 <= yy < H:
            continue
        for i in range(w):
            p = px[j * w + i]
            xx = x + i
            if p is None or not 0 <= xx < W:
                continue
            k = yy * W + xx
            a = p[3]
            if a >= 0.999:
                c.px[k] = p[:3]
            else:
                o = c.px[k]
                c.px[k] = (int(o[0] + (p[0] - o[0]) * a), int(o[1] + (p[1] - o[1]) * a), int(o[2] + (p[2] - o[2]) * a))
    _, _, rot, cor_rot = _icone_def(ICONES_LAYOUT, nome)
    if rot:                         # tubo: letra na fonte do menu (nitida)
        c.text(x + (w - len(rot) * F.cw + 1) // 2, y + (alt - F.ch + 1) // 2, rot, cor_rot)
    return w


def _bt_partes(s, ini=0, so=None):
    """texto -> [('t', texto) | ('b', botao)]; so as palavras a partir de ini viram icone"""
    import re
    partes = [('t', s[:ini])] if ini else []
    for pal in re.split(r'( +)', s[ini:]):
        if not pal:
            continue
        nucleo = pal[:-1] if pal.endswith(':') and len(pal) > 1 else pal
        sep = '/' if '/' in nucleo else ('+' if '+' in nucleo else None)
        pecas = nucleo.split(sep) if sep else [nucleo]
        if pal.strip() and all(p in _BT and (so is None or p in so) for p in pecas):
            for k, p in enumerate(pecas):
                if k and sep == '+':
                    partes.append(('t', '+'))
                partes.append(('b', _BT_APELIDO.get(p, p)))
        else:
            partes.append(('t', pal))
    return partes


def bt_largura(s, ini=0, so=None):
    larg, ant = 0, None
    for k, v in _bt_partes(s, ini, so):
        if k == 't':
            larg += len(v) * F.cw
        else:
            larg += icone_largura(v) + (1 if ant == 'b' else 0)
        ant = k
    return larg


def bt_text(c, x, y, s, cor, ini=0, so=None):
    """texto com os botoes (A, B, L1/R1, START...) trocados pelos icones do controle"""
    ant = None
    for k, v in _bt_partes(s, ini, so):
        if k == 't':
            c.text(x, y, v, cor)
            x += len(v) * F.cw
        else:
            if ant == 'b':
                x += 1
            x += icone(c, x, y - 2, v)
        ant = k
    return x


def bt_rtext(c, x, y, s, cor, ini=0, so=None):
    bt_text(c, x - bt_largura(s, ini, so), y, s, cor, ini, so)


def bt_ctext(c, y, s, cor, ini=0, so=None):
    bt_text(c, (W - bt_largura(s, ini, so)) // 2, y, s, cor, ini, so)


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
    c.rtext(W - 12, yy + F.ch + 2, 'LTB', ORANGE)    # iniciais do autor, fixas abaixo da hora
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
    bt_rtext(c, W - 12, H - 12, rodape_d, SLATE)


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

    # efeitos do menu no LCD (OPCOES > VIDEO > EFEITO DO MENU): (luz da ultima linha de cada
    # bloco, luz da ultima coluna). O menu e ampliado s vezes: cada ponto vira um bloco s x s.
    # Luz em 1/8: 7 = 87,5%, 6 = 75%, 5 = 62,5%, 4 = 50% (contas de deslocar bits, linha inteira).
    EFEITOS_MENU = {'NENHUM': (8, 8), 'SCANLINES LEVE': (7, 8), 'SCANLINES FORTE': (5, 8),
                    'CRT': (6, 7), 'DOT MATRIX': (5, 5)}
    efeito = 'NENHUM'

    def _mascaras(self, n):
        """mascaras do tamanho de uma linha (n bytes) para escurecer todos os pontos de uma vez"""
        bpp = self.bpp // 8
        if bpp == 2:
            m1, m2, m3, cheio = 0x7BEF, 0x39E7, 0x18E3, 0xFFFF
        else:
            m1, m2, m3, cheio = 0x7F7F7F, 0x3F3F3F, 0x1F1F1F, 0xFFFFFFFF
        k = n // bpp

        def rep(v):
            return int.from_bytes(v.to_bytes(bpp, 'little') * k, 'little')
        return rep(m1), rep(m2), rep(m3), rep(cheio)

    @staticmethod
    def _escurece(v, f, M):
        m1, m2, m3, _ = M
        if f == 7:
            return v - ((v >> 3) & m3)
        if f == 6:
            return v - ((v >> 2) & m2)
        if f == 5:
            return ((v >> 1) & m1) + ((v >> 3) & m3)
        if f == 4:
            return (v >> 1) & m1
        return v

    def show(self, canvas, conv):
        """conv = (posicao_x, posicao_y, largura, altura). Fora da imagem: preto.
        A tela inteira e montada fora do framebuffer e copiada de uma vez (sem piscar).
        As linhas prontas ficam guardadas entre um desenho e outro: apertar um botao so
        refaz as linhas que mudaram (com imagem de fundo cada linha custa caro em Python)."""
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
        fl, fc = (8, 8) if self.tubo or s < 2 else self.EFEITOS_MENU.get(self.efeito, (8, 8))
        corte_e = max(0, -x0) * bpp
        dest_x = max(0, x0)
        geo = (W, cw, x0, self.w, self.bpp, fl, fc)
        if getattr(self, '_geo_cache', None) != geo or len(self._linhas_cache) > 3000 or len(self._cor_cache) > 200000:
            self._geo_cache = geo
            self._linhas_cache = {}      # linha logica (cores) -> bytes da linha da tela
            self._efeito_cache = {}      # (cores, escura) -> bytes com o efeito
            self._cor_cache = {}
            self._masc = None
        linhas, cache, efc = self._linhas_cache, self._cor_cache, self._efeito_cache
        por_y = {}
        linha_preta = preto * (self.stride // bpp)
        frame = bytearray(linha_preta * self.h)
        for j in range(ch):
            yy = y0 + j
            if not 0 <= yy < self.h:
                continue
            y = (2 * j + 1) * HT // (2 * ch)                # linha logica (centro): 1a e ultima sempre aparecem
            escura = fl < 8 and j % s == s - 1                # ultima linha do bloco (efeito)
            row = por_y.get((y, escura))
            if row is None:
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
                if escura or fc < 8:
                    ke = (chave, escura)
                    r2 = efc.get(ke)
                    if r2 is None:
                        if self._masc is None or self._masc[0] != len(row):
                            M = self._mascaras(len(row))
                            sel = 0
                            if fc < 8:   # pontos da ultima coluna de cada bloco s
                                um = (1 << (8 * bpp)) - 1
                                for i in range(s - 1, len(row) // bpp, s):
                                    sel |= um << (8 * bpp * i)
                            self._masc = (len(row), M, sel)
                        _, M, sel = self._masc
                        v = int.from_bytes(row, 'little')
                        if escura:
                            v = self._escurece(v, fl, M)
                        if fc < 8:
                            v = (v & ~sel) | (self._escurece(v, fc, M) & sel)
                        r2 = efc[ke] = v.to_bytes(len(row), 'little')
                    row = r2
                por_y[(y, escura)] = row
            off = yy * self.stride + dest_x * bpp
            frame[off:off + len(row)] = row
        if not self.tubo:
            for e in canvas.extras:
                if len(e) > 6:                   # icone de botao (com borda suave)
                    self._icone(frame, e, x0, y0 + topo * s, s)
                else:
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

    def _icone(self, frame, e, x0, y0, s):
        """icone (pontos do monitor, com transparencia) misturado com o que ja esta na tela"""
        lx, ly, lw, lh, m = e[:5]
        w, h, px = m
        bpp = self.bpp // 8
        ox = x0 + lx * s
        oy = y0 + ly * s + (lh * s - h) // 2
        for j in range(h):
            yy = oy + j
            if not 0 <= yy < self.h:
                continue
            base = yy * self.stride
            for i in range(w):
                p = px[j * w + i]
                xx = ox + i
                if p is None or not 0 <= xx < self.w:
                    continue
                off = base + xx * bpp
                a = p[3]
                if a < 0.999:
                    if bpp == 2:
                        v = frame[off] | frame[off + 1] << 8
                        r0, g0, b0 = (v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31
                    else:
                        b0, g0, r0 = frame[off], frame[off + 1], frame[off + 2]
                    cor = (int(r0 + (p[0] - r0) * a), int(g0 + (p[1] - g0) * a), int(b0 + (p[2] - b0) * a))
                else:
                    cor = p[:3]
                frame[off:off + bpp] = self.pixel(cor)

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
        self.layout_ctl = 'xbox'    # desenho dos icones: o do ultimo controle usado
        self._lay_dev = {}
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
                eh_pad = self.e.BTN_SOUTH in keys or self.e.BTN_GAMEPAD in keys
                if eh_pad and not self._lay_dev.get('_primeiro'):
                    self._lay_dev['_primeiro'] = p
                    self.nota_controle(p, d)
                if self.ja_procurou and eh_pad:
                    self.fila.put('_controle:' + d.name)
            except OSError:
                pass
        self.nomes = [d.name for d in self.devs.values()]
        self.ja_procurou = True

    @staticmethod
    def layout_de(nome, vendor):
        """desenho dos botoes pelo fabricante/nome do controle (so visual)"""
        n = (nome or '').lower()
        if vendor == 0x054c or any(k in n for k in ('playstation', 'dualsense', 'dualshock', 'ps3', 'ps4', 'ps5', 'sony')):
            return 'playstation'
        if vendor == 0x057e or any(k in n for k in ('nintendo', 'pro controller', 'joy-con', 'switch')):
            return 'nintendo'
        if 'xbox' in n or 'x-box' in n or vendor == 0x045e:
            return 'xbox'
        if vendor == 0x2dc8 or '8bitdo' in n:   # 8BitDo: letras no padrao Nintendo (B embaixo)
            return 'nintendo'
        return 'xbox'

    def nota_controle(self, p, d):
        """controle usado: se o desenho dos botoes for outro, o menu redesenha"""
        lay = self._lay_dev.get(p)
        if lay is None:
            try:
                keys = d.capabilities().get(self.e.EV_KEY, [])
                eh_pad = self.e.BTN_SOUTH in keys or self.e.BTN_GAMEPAD in keys
                lay = self.layout_de(d.name, d.info.vendor) if eh_pad else ''
            except (OSError, AttributeError):
                lay = ''
            self._lay_dev[p] = lay
        if lay and lay != self.layout_ctl:
            self.layout_ctl = lay
            print('controle: icones %s (%s)' % (lay, d.name), flush=True)
            self.fila.put('_icones')

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
                    self._lay_dev.pop(p, None)   # outro controle pode vir no mesmo caminho
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
                                self.nota_controle(p, d)
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
    'pcengine': ('PC ENGINE', 'mednafen_pce_fast_libretro.so', False),
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
    # Saturno: Beetle Saturn v1.32 (o mesmo do ReplayOS; desenho por CPU, JIT aarch64 nos DSPs)
    'saturn': ('SATURN', 'mednafen_saturn_libretro.so', True),
    '3do': ('3DO', None, True),
    'atari2600': ('ATARI 2600', None, False),
    'n64': ('NINTENDO 64', None, False),
    # portateis e NES: Mesen (NES/Famicom Disk System), Gambatte (GB/GBC), mGBA (GBA),
    # Beetle NeoPop (Neo Geo Pocket/Color), melonDS DS (DS; 2 monitores = 2 telas)
    'nes': ('NES', 'mesen_libretro.so', False),
    'gb': ('GAME BOY', 'gambatte_libretro.so', False), 'gbc': ('GAME BOY COLOR', 'gambatte_libretro.so', False),
    'gba': ('GAME BOY ADVANCE', 'mgba_libretro.so', False),
    'ngp': ('NEO GEO POCKET', 'mednafen_ngp_libretro.so', False),
    'nds': ('NINTENDO DS', 'melondsds_libretro.so', False),
    # FBNeo (FinalBurn Neo), jogos em .zip no romset do FBNeo: NEO GEO (MVS/AES) e ARCADE
    # (CPS1, CPS2, CPS3 e o resto do FBNeo). BIOS neogeo.zip em bios/neogeo (ou bios/arcade)
    'neogeo': ('NEO GEO', 'fbneo_libretro.so', False), 'arcade': ('ARCADE', 'fbneo_libretro.so', False),
}
EXT_JOGO = {
    'ps1': ('.cue', '.chd', '.pbp', '.m3u', '.iso', '.img', '.bin', '.exe', '.mdf', '.toc', '.cbn'),
    'pcengine': ('.pce',),
    'pcenginecd': ('.cue', '.chd', '.ccd', '.toc', '.m3u'),
    'segacd': ('.cue', '.chd', '.iso', '.m3u'),
    'neogeocd': ('.cue', '.chd'),
    'megadrive': ('.md', '.gen', '.smd', '.bin', '.68k', '.sgd', '.mdx', '.bms'),
    '32x': ('.32x', '.bin'),
    'mastersystem': ('.sms',),
    'gamegear': ('.gg',),
    'sg1000': ('.sg',),
    'snes': ('.sfc', '.smc', '.fig', '.swc', '.bs'),
    'dreamcast': ('.cdi', '.gdi', '.chd', '.cue', '.m3u'),
    'saturn': ('.cue', '.chd', '.iso', '.ccd', '.mds', '.m3u'),
    'arcade': ('.zip', '.7z'),
    'neogeo': ('.zip', '.7z'),
    'nes': ('.nes', '.fds', '.unf', '.unif', '.zip'),
    'gb': ('.gb', '.dmg', '.zip'),
    'gbc': ('.gbc', '.gb', '.zip'),
    'gba': ('.gba', '.zip'),
    'ngp': ('.ngp', '.ngc', '.ngpc', '.npc', '.zip'),
    'nds': ('.nds', '.dsi', '.ids'),
}
# cartuchos pequenos em .zip: o jogo de dentro vai para a memoria (/run) e abre de la
# (os nucleos destes nao abrem .zip). DS fica de fora: jogo de ate 512 MB nao cabe na RAM.
ZIP_EXTRAI = ('nes', 'gb', 'gbc', 'gba', 'ngp')
ROM_RAM = '/run/retrovolt/rom'
# run-ahead (1 quadro a menos de atraso; desliga com run_ahead = NAO). DS fica de fora: o
# estado do DS e grande e o JIT recompila tudo a cada volta
RUN_AHEAD = ('snes', 'arcade', 'neogeo', 'nes', 'gb', 'gbc', 'gba', 'ngp')
# FBNeo: as duas listas usam a mesma BIOS (um jogo de Neo Geo pode estar em ARCADE)
FBNEO = ('arcade', 'neogeo')
# Saturno: o nucleo procura saturn_bios.bin, sega_101.bin (japonesa) ou mpr-17933.bin
# (americana/europeia); qualquer uma serve
BIOS_SATURNO = ('saturn_bios.bin', 'sega_101.bin', 'mpr-17933.bin')
# PC Engine CD: lista propria no menu (roms/pcenginecd + INICIAR CD); PC Engine so cartuchos
# (.pce). No jogo o CD usa o sistema pcengine (mesma BIOS, saves e states de antes).
APELIDO = {'pcenginecd': 'pcengine'}
PASTAS_EXTRAS = {}
# ordem do menu JOGOS (de cima para baixo); o que nao estiver aqui vem depois, se tiver jogos
ORDEM_JOGOS = ['ps1', 'dreamcast', 'saturn', 'segacd', 'neogeocd', 'pcenginecd', 'pcengine', 'snes',
               'megadrive', 'mastersystem', 'neogeo', 'arcade', 'nds', 'gba', 'gbc', 'gb', 'nes', 'ngp']
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
    'flycast_libretro.so': {'reicast_internal_resolution': '640x480', 'reicast_texupscale': '1',
                            'reicast_anisotropic_filtering': 'off'},
    # Saturno (Beetle): sincronia Fast 8 (~15%), JIT nos 2 DSPs, sem entrada no meio do quadro
    # (custa CPU), entrelacado em Bob (480i real no tubo; sem pente no LCD)
    # FBNeo: som em 48 kHz (a saida do rv-jogo; sem reamostrar), 32 bits de cor, recordes,
    # cartao de memoria por jogo (AES salva nele), Neo Geo pela BIOS do dipswitch (MVS; troca
    # para AES/UNIBIOS no menu do jogo), sem frameskip
    'fbneo_libretro.so': {'fbneo-samplerate': '48000', 'fbneo-allow-depth-32': 'enabled',
                          'fbneo-hiscores': 'enabled', 'fbneo-memcard-mode': 'per-game',
                          'fbneo-neogeo-mode': 'DIPSWITCH', 'fbneo-frameskip-type': 'disabled',
                          'fbneo-force-60hz': 'disabled'},
    # DS com 1 monitor: uma tela por vez em tela cheia, R3 troca (cima/baixo); toque pelo
    # analogico direito segurando R2 (com 2 monitores o rv-jogo forca cima+baixo e divide)
    'melondsds_libretro.so': {'melonds_number_of_screen_layouts': '2', 'melonds_screen_layout1': 'top',
                              'melonds_screen_layout2': 'bottom', 'melonds_touch_mode': 'joystick',
                              'melonds_show_cursor': 'timeout', 'melonds_render_mode': 'software',
                              'melonds_threaded_renderer': 'enabled', 'melonds_jit_enable': 'enabled',
                              'melonds_network_mode': 'disabled'},   # sem Wi-Fi do DS (libpcap)
    # GBA (mGBA): filtro passa-baixa do proprio nucleo a 40% (~9,6 kHz; o padrao 60% abafa):
    # tira o chiado agudo do som de 65 kHz sem perder clareza
    'mgba_libretro.so': {'mgba_audio_low_pass_filter': 'enabled', 'mgba_audio_low_pass_range': '40'},
    'mednafen_saturn_libretro.so': {'beetle_saturn_sh2_interleave': '8', 'beetle_saturn_cpucache_emumode': 'auto',
                                    'beetle_saturn_jit_scu': 'enabled', 'beetle_saturn_jit_scsp': 'enabled',
                                    'beetle_saturn_midsync': 'disabled', 'beetle_saturn_deinterlacer': 'bob'},
}
# Sega CD: uma BIOS por regiao do disco (o nucleo escolhe pela regiao gravada no disco)
BIOS_SEGACD = {'U': ('bios_CD_U.bin', 'AMERICANO'), 'E': ('bios_CD_E.bin', 'EUROPEU'),
               'J': ('bios_CD_J.bin', 'JAPONES')}


# BIOS: RETROVOLT/bios/<sistema>/ (o mesmo nome da pasta em roms/). Arquivo solto em
# RETROVOLT/bios/ ainda vale (versoes antigas), mas a subpasta do sistema tem preferencia.
# Nomes exatos que os nucleos procuram (no cartao a caixa conta; no pendrive nao):
BIOS_NOMES = ['bios_CD_U.bin', 'bios_CD_E.bin', 'bios_CD_J.bin', 'bios_MD.bin', 'bios_U.sms', 'bios_E.sms',
              'bios_J.sms', 'bios.gg', 'syscard3.pce', 'syscard2.pce', 'syscard1.pce', 'gexpress.pce',
              'scph1001.bin', 'scph5500.bin', 'scph5501.bin', 'scph5502.bin', 'scph7001.bin', 'scph101.bin',
              'saturn_bios.bin', 'sega_101.bin', 'mpr-17933.bin', 'gba_bios.bin', 'gb_bios.bin',
              'gbc_bios.bin', 'bios7.bin', 'bios9.bin', 'disksys.rom']
PASTA_BIOS_ANTIGA = {'neogeocd': 'neocd'}       # a R19 criou bios/neocd


def bios_arquivos(base_rv, sistema):
    """{nome: caminho} das BIOS deste sistema (subpasta > pasta antiga > solta em bios/)"""
    raiz = os.path.join(base_rv, 'bios')
    achados = {}
    pastas = [raiz]
    if sistema in PASTA_BIOS_ANTIGA:
        pastas.append(os.path.join(raiz, PASTA_BIOS_ANTIGA[sistema]))
    if sistema in FBNEO:
        pastas += [os.path.join(raiz, s) for s in FBNEO if s != sistema]
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
        if sistema in FBNEO:
            # system/fbneo/: hiscore.dat e os recordes (.hi) que o FBNeo grava; fica no
            # pendrive (RETROVOLT/bios/fbneo) para os recordes nao sumirem ao desligar
            pasta = os.path.join(base_rv, 'bios', 'fbneo')
            os.makedirs(pasta, exist_ok=True)
            hs = os.path.join(PASTA, 'fbneo', 'hiscore.dat')
            if os.path.exists(hs) and not os.path.exists(os.path.join(pasta, 'hiscore.dat')):
                shutil.copy(hs, os.path.join(pasta, 'hiscore.dat'))
            if not os.path.lexists(os.path.join(alvo, 'fbneo')):
                os.symlink(pasta, os.path.join(alvo, 'fbneo'))
        if sistema == 'saturn':
            # o Beetle escolhe a BIOS pelo NOME da regiao do jogo (sega_101 = Japao,
            # mpr-17933 = EUA/Europa) e nao confere o conteudo: com uma so, as outras viram
            # atalho para ela
            tem = [os.path.join(alvo, n) for n in BIOS_SATURNO if os.path.lexists(os.path.join(alvo, n))]
            if tem:
                for n in BIOS_SATURNO:
                    if not os.path.lexists(os.path.join(alvo, n)):
                        os.symlink(os.path.realpath(tem[0]), os.path.join(alvo, n))
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
    if sistema == 'neogeo':
        if not bios_tem(arqs, 'neogeo.zip') and not os.path.exists(os.path.join(base_rv, 'roms', 'neogeo', 'neogeo.zip')):
            return 'FALTA A BIOS NEOGEO.ZIP EM %s' % onde
        return None
    if sistema == 'saturn':
        if not any(bios_tem(arqs, n) for n in BIOS_SATURNO):
            return 'FALTA A BIOS DO SATURNO (SEGA_101.BIN OU MPR-17933.BIN) EM %s' % onde
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


def extrai_zip(arq, sistema):
    """jogo de dentro do .zip (cartucho pequeno) copiado para a memoria: caminho, ou None"""
    import zipfile
    exts = tuple(e for e in EXT_JOGO.get(sistema, ()) if e != '.zip')
    try:
        with zipfile.ZipFile(arq) as z:
            membros = [m for m in z.infolist()
                       if not m.is_dir() and os.path.splitext(m.filename)[1].lower() in exts]
            if not membros:
                print('jogo: %s sem %s dentro' % (arq, '/'.join(exts)), flush=True)
                return None
            m = max(membros, key=lambda m: m.file_size)   # varios: o maior
            if m.file_size > 64 * 1048576:
                print('jogo: %s grande demais para a memoria (%d MB)' % (m.filename, m.file_size >> 20), flush=True)
                return None
            shutil.rmtree(ROM_RAM, ignore_errors=True)
            os.makedirs(ROM_RAM, exist_ok=True)
            nome = os.path.basename(m.filename.replace('\\', '/')) or ('jogo' + exts[0])
            destino = os.path.join(ROM_RAM, nome)
            with z.open(m) as src, open(destino, 'wb') as dst:
                shutil.copyfileobj(src, dst, 1048576)
            return destino
    except (OSError, RuntimeError, NotImplementedError, EOFError, ValueError, zipfile.BadZipFile) as ex:
        print('jogo: nao abriu o zip %s: %s' % (arq, ex), flush=True)
        shutil.rmtree(ROM_RAM, ignore_errors=True)
        return None


def nome_limpo(arq):
    import re
    n = os.path.splitext(os.path.basename(arq))[0]   # tira (USA), [!] etc., mas deixa (Disc 1)
    partes = arq.replace('\\', '/').lower().split('/')
    if arq.lower().endswith(('.zip', '.7z')) and any(p in FBNEO for p in partes[:-1]):
        # arcade: "sf2ce" -> "STREET FIGHTER II': CHAMPION EDITION (WORLD 920513)" (a versao fica:
        # clones tem o mesmo nome e so a versao diferencia)
        d = fbneo_dados().get('nomes', {}).get(n.lower())
        if d:
            return d.upper()
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
    if sistema in FBNEO:      # BIOS (neogeo.zip etc.) fica na pasta mas nao e jogo
        bios = set(fbneo_dados().get('bios', [])) | {'neogeo', 'qsound', 'hiscore'}
        out = [p for p in out if os.path.splitext(os.path.basename(p))[0].lower() not in bios]
    return sorted(out, key=lambda p: nome_limpo(p))


_FBNEO = None


def fbneo_dados():
    """nomes dos jogos do FBNeo (nome curto do .zip -> nome completo) e quais sao BIOS;
    gerado do DAT oficial do FBNeo junto com o nucleo (nomes_fbneo.json)"""
    global _FBNEO
    if _FBNEO is None:
        try:
            with open(os.path.join(PASTA, 'nomes_fbneo.json')) as f:
                _FBNEO = json.load(f)
        except (OSError, ValueError):
            _FBNEO = {}
    return _FBNEO


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
ABAS = ['DISCO', 'JOGOS', 'TV', 'APPS', 'OPCOES']
APPS = ['YOUTUBE', 'KODI']
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
    # legenda no centro da tela; se a dica da direita for comprida (TV), no centro do espaco
    # que sobra a esquerda dela
    leg = 'L1/R1 ABAS  START FUNDO'
    lw = bt_largura(leg)
    lim = W - 12 - bt_largura(rodape_d) - 2 * F.cw if rodape_d else W - 16
    x = (W - lw) // 2
    if x + lw > lim:
        x = max(16, (16 + lim - lw) // 2)
    bt_text(c, x, H - 4 - F.ch, leg, SLATE)
    bt_rtext(c, W - 12, H - 4 - F.ch, rodape_d, SLATE)


AZUL_CAIXA = (24, 44, 110)


def caixa(c, linhas, cores=None):
    """popup de confirmacao no meio da tela: fundo azul com contorno branco"""
    # icones so na parte de botoes: depois do "?" ("REINICIAR? A SIM  B NAO") ou na linha que
    # comeca com um botao ("B CANCELA"); o resto e texto (o "A" de "VOLTA A ..." fica letra)
    inis = []
    for l in linhas:
        q = l.rfind('? ')
        inis.append(q + 1 if q >= 0 else (0 if l.split(' ')[0] in _BT else len(l)))
    w = max(bt_largura(l, i) for l, i in zip(linhas, inis)) + 24
    h = len(linhas) * (F.ch + 6) + 14
    x0, y0 = (W - w) // 2, (H - h) // 2
    c.rect(x0, y0, w, h, (255, 255, 255))
    c.rect(x0 + 1, y0 + 1, w - 2, h - 2, AZUL_CAIXA)
    for k, l in enumerate(linhas):
        bt_ctext(c, y0 + 8 + k * (F.ch + 6), l, (cores or [IVORY] * len(linhas))[k], inis[k])


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
        global W, H, ICONES_ESC
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
            ICONES_ESC = esc if esc > 1 else None   # icones nos pontos do monitor
            H = self.fb.h // esc
            W = self.fb.w // esc
            print('tela: %dx%d, menu %dx%d ampliado %dx' % (self.fb.w, self.fb.h, W, H, esc), flush=True)
        self.cfg = cfg_ler()
        self.fb.efeito = self.cfg.get('menu_efeito', 'NENHUM').upper()
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
        for s in ORDEM_JOGOS + [x for x in SISTEMAS if x not in ORDEM_JOGOS]:
            if SISTEMAS[s][1] and SISTEMAS[s][2] and os.path.exists(os.path.join(PASTA, 'nucleos', SISTEMAS[s][1])):
                out.append(s)                # sistema de CD com nucleo no pacote: sempre (INICIAR CD)
                continue
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
        lista_sis = sistema if sistema in SISTEMAS else APELIDO.get(sistema, sistema)   # lista do menu JOGOS
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
        if nucleo in OPCOES_PADRAO:
            # arquivo novo: todas as opcoes iniciais; arquivo que ja existe: so as que faltam
            # (opcao nova de uma versao nova; o que a pessoa escolheu fica)
            try:
                tem = set()
                if os.path.exists(opt):
                    for l in open(opt, errors='replace'):
                        if '=' in l and not l.lstrip().startswith('#'):
                            tem.add(l.split('=', 1)[0].strip())
                faltam = [(k, v) for k, v in OPCOES_PADRAO[nucleo].items() if k not in tem]
                if faltam:
                    os.makedirs(os.path.dirname(opt), exist_ok=True)
                    novo = not os.path.exists(opt)
                    with open(opt, 'a') as f:
                        if novo:
                            f.write('# RETROVOLT: opcoes iniciais deste nucleo (mude no jogo: START+SELECT > OPCOES DO NUCLEO)\n')
                        for k, v in faltam:
                            f.write('%s = "%s"\n' % (k, v))
                    print('jogo: opcoes iniciais em %s: %s' % (opt, ' '.join(k for k, _ in faltam)), flush=True)
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
        rom = arquivo
        if not cd and sistema in ZIP_EXTRAI and arquivo.lower().endswith('.zip'):
            rom = extrai_zip(arquivo, sistema)
            if not rom:
                self.mostra_aviso('O .ZIP NAO TEM JOGO DE %s' % NOME_SISTEMA.get(sistema, '?'))
                return
        cmd += ['--cd', self.leitora.dev, '--cd-vel', self.cfg.get('leitora_velocidade', '4'),
                '--cd-acorda', str(num(self.cfg, 'leitora_acordar', int, 2, 60))] if cd else ['--jogo', rom]
        cmd += ['--tubo-480i', '0' if self.cfg.get('tubo_480i', 'SIM').upper() == 'NAO' else '1']
        if sistema == 'dreamcast':
            cmd += ['--gpu', 'gles' if self.cfg.get('dreamcast_gpu', 'VULKAN').upper() == 'GLES' else 'vulkan']
            # opcoes por jogo (ex.: Gunbird 2 em 1280x960 sem pontinhos, 3D em 640x480): o que
            # mudar no menu do jogo vale so para ele; o resto vem do flycast.opt
            cmd += ['--opcoes-jogo', os.path.join(self.base, 'config', 'nucleos', 'flycast', save + '.opt')]
        if sistema in RUN_AHEAD and self.cfg.get('run_ahead', 'SIM').upper() != 'NAO':
            cmd += ['--runahead', '1']        # 1 quadro a menos de atraso no controle
        if sistema == 'gba':      # o som do GBA e baixo de fabrica: ganho (com limitador suave)
            cmd += ['--som-ganho', str(num(self.cfg, 'som_ganho_gba', int, 50, 300))]
        if sistema == 'nds' and self.cfg.get('ds_duas_telas', 'SIM').upper() != 'NAO':
            cmd += ['--ds-duas-telas', '1']   # so liga se tiver 2 monitores ligados
        if sistema in FBNEO:      # opcoes por jogo (ex.: AES so num jogo)
            cmd += ['--opcoes-jogo', os.path.join(self.base, 'config', 'nucleos', 'fbneo', save + '.opt')]
        if sistema == 'saturn':   # opcoes por jogo, como no Dreamcast
            cmd += ['--opcoes-jogo', os.path.join(self.base, 'config', 'nucleos', nucleo.replace('_libretro.so', ''),
                                                  save + '.opt')]
        if cd and sistema == 'saturn':
            # disco comum (1 sessao). Cache + copia para a memoria em 2o plano, como no Dreamcast
            cmd += ['--cd-cache', str(num(self.cfg, 'leitora_cache_saturn', int, 2, 1024))]
        if cd and sistema == 'dreamcast':
            # CD-R em 2 sessoes: uma faixa por arquivo. Cache grande: o Dreamcast volta toda hora
            # aos mesmos arquivos e cada busca nova na leitora para o jogo ~100 ms
            mb = num(self.cfg, 'leitora_cache_dreamcast', int, 2, 1024)
            if mb == 256:  # padrao antigo gravado no cfg: sobe para o novo
                mb = 512
            cmd += ['--cd-faixas', '1', '--cd-cache', str(mb)]
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
        if rom != arquivo:   # jogo tirado do .zip: sai da memoria
            shutil.rmtree(ROM_RAM, ignore_errors=True)
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
        if r in (0, 10):       # SAIR / VOLTAR PARA A LISTA: lista deste sistema, cursor no jogo
            self.tela = 'menu'
            self.aba = 'JOGOS'
            self.sistema = lista_sis
            ordem = self.sistemas_com_jogos()
            if lista_sis in ordem:
                self.sel['SIS'] = ordem.index(lista_sis)
            self.jogos = lista_jogos(self.base, lista_sis)
            itens = self.itens_pasta()
            self.sel['JOGOS'] = 0
            for n, (k, v) in enumerate(itens):
                if (cd and k == 'cd') or (not cd and k == 'arq' and v == arquivo):
                    self.sel['JOGOS'] = n
                    break
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

    ICONES = ['AUTO', 'XBOX', 'PLAYSTATION', 'NINTENDO']

    def layout_icones(self):
        """xbox | playstation | nintendo: o escolhido em OPCOES > VIDEO ou o do controle"""
        v = self.cfg.get('botoes_icones', 'AUTO').upper()
        if v in ('XBOX', 'PLAYSTATION', 'NINTENDO'):
            return v.lower()
        return getattr(self.ent, 'layout_ctl', 'xbox')

    def desenha(self):
        global ICONES_LAYOUT
        ICONES_LAYOUT = self.layout_icones()
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
            elif self.aba == 'APPS':
                cabecalho(c, 'APPS', self.wifi, 'A ENTRA')
                y = max(56, 30 + F.ch + 14)
                for n, nome in enumerate(APPS):
                    item(c, 24, y, nome, n == self.sel.get('APPS', 0))
                    y += F.ch + 9
            else:
                cabecalho(c, 'OPCOES', self.wifi, 'A ENTRA')
                y = max(56, 30 + F.ch + 14)
                lista = self.opcoes()
                sobra = H - 14 - F.ch - y      # (nao chamar de espaco: e a funcao do INFO)
                passo_op = max(F.ch + 3, min(F.ch + 9, sobra // len(lista)))
                cabem = max(1, sobra // passo_op)
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
            bt_ctext(c, H // 2 + 10, 'A OU START: SIM   (VOLTA SOZINHA EM %d)' % n, ORANGE)
        elif t == 'wifi':
            self.desenha_wifi(c)
        elif t == 'teclado':
            self.desenha_teclado(c)
        elif t == 'youtube':
            self.desenha_youtube(c)
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
            bt_text(c, 24, 206, 'SAIR: APERTE B 3 VEZES', SLATE)
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
            c.text(24, y + 6, 'CRIADO POR LEONARDO TORRES', ORANGE)
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
            bt_text(c, x0 + 10, y0 + h - F.ch - 10, 'INICIAR?  A SIM   B NAO', IVORY, 9)
        else:
            c.text(x0 + 10, y0 + h - 2 * F.ch - 14, 'NUCLEO AINDA NAO INSTALADO', RED)
            bt_text(c, x0 + 10, y0 + h - F.ch - 10, 'B VOLTA', SLATE)

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
        busca = self.tec.get('modo') == 'youtube'
        moldura(c, 'BUSCA' if busca else 'SENHA', 'START BUSCA  B APAGA' if busca else 'START CONECTA  B APAGA')
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
            xf = bt_text(c, x, yj, 'A JOGAR', IVORY)
            c.rect(x, yj + F.ch + 2, xf - x, 1, ORANGE)
            c.text(xf + F.cw, yj, '->', ORANGE)

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
            # lista como sempre foi (x 24, y 58, um nome a cada F.ch+9); se nao couber acima da
            # legenda de baixo (muitos sistemas), rola com o cursor e mostra ^ v a esquerda
            passo = F.ch + 9
            y0 = 58
            y_fim = H - 4 - F.ch - 6             # topo da legenda menos uma folga
            cabem = max(1, (y_fim - y0 + 9) // passo)
            sel = self.sel['SIS']
            vis = min(len(sis), cabem)
            ini = max(0, min(sel - vis // 2, len(sis) - vis))
            for n in range(ini, ini + vis):
                item(c, 24, y0 + (n - ini) * passo, NOME_SISTEMA[sis[n]], n == sel)
            if ini > 0:
                c.text(7, y0, '^', SLATE)
            if ini + vis < len(sis):
                c.text(7, y0 + (vis - 1) * passo, 'v', SLATE)
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
        if a == 'START' and not self.confirma and len(imagens.fundos(self.base)) > 1:
            self.carrega_fundo()           # START no menu: sorteia outro fundo
            self.mostra_aviso('FUNDO TROCADO')
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
            if self.aba == 'JOGOS' and self.sistema:
                # saindo de JOGOS de dentro de um console: na volta, lista de consoles com o
                # cursor nele
                ordem = self.sistemas_com_jogos()
                if self.sistema in ordem:
                    self.sel['SIS'] = ordem.index(self.sistema)
                self.sistema = None
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
                for i in range(passos):
                    n = sel + d
                    while 0 <= n < len(itens) and itens[n][0] == 'vazio':
                        n += d
                    if 0 <= n < len(itens):
                        sel = n
                    elif i == 0 and itens:     # passou do fim/comeco: da a volta
                        n = 0 if d > 0 else len(itens) - 1
                        while itens[n][0] == 'vazio':
                            n += d
                        sel = n
                        break
                    else:
                        break
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
        elif self.aba == 'APPS':
            n = self.sel.get('APPS', 0)
            if a in ('CIMA', 'BAIXO'):
                self.sel['APPS'] = (n + (1 if a == 'BAIXO' else -1)) % len(APPS)
            elif a == 'A' and APPS[n] == 'YOUTUBE':
                self.tela = 'youtube'
                self.__dict__.setdefault('yt', {'termo': '', 'res': [], 'sel': 0, 'msg': '', 'ocupado': False})
            elif a == 'A' and APPS[n] == 'KODI':
                self.abre_kodi()
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
    FILTROS = ['NENHUM', 'SCANLINES', 'SCANLINES LEVES', 'CRT', 'SUAVE', 'SCALE2X', 'SCALE3X', 'XBR', 'DOT MATRIX']

    def itens_video(self):
        """(chave, nome, valor) dos itens de OPCOES > VIDEO"""
        itens = [('saida', 'SAIDA', self.cfg.get('video_saida', 'AUTO').upper())]
        if not self.fb.tubo:
            itens.append(('modo', 'RESOLUCAO', self.cfg.get('video_modo', 'AUTO').upper()))
        if not self.fb.tubo:                      # no tubo nao ha efeitos
            itens.append(('filtro', 'FILTRO', self.cfg.get('video_filtro', 'NENHUM').upper()))
            itens.append(('mefeito', 'EFEITO DO MENU', self.cfg.get('menu_efeito', 'NENHUM').upper()))
        itens.append(('gpu', 'GPU DREAMCAST', 'GLES' if self.cfg.get('dreamcast_gpu', 'VULKAN').upper() == 'GLES' else 'VULKAN'))
        v = self.cfg.get('botoes_icones', 'AUTO').upper()
        if v == 'AUTO':
            v = 'AUTO (%s)' % getattr(self.ent, 'layout_ctl', 'xbox').upper()
        itens.append(('icones', 'ICONES DOS BOTOES', v))
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
            bt_text(c, x0 + 6, y0c + bh // 2 - F.ch // 2, 'A: ASSISTIR', SLATE)
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
                self.tv_som = (time.monotonic() + 0.4, 10)   # confere (e tenta de novo) por ~4 s
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
        som = getattr(self, 'tv_som', None)
        if som and self.tela == 'tv_cheia' and time.monotonic() >= som[0]:
            # o HDMI pode demorar um instante para ser solto pelo ffmpeg da previa: tenta de novo
            tc = self.tv_cheia
            if tc.liga_audio():
                self.tv_som = None
                print('tv: som no HDMI (%s)' % tc.consulta('current-ao'), flush=True)
            elif som[1] <= 1:
                self.tv_som = None
                print('tv: som NAO abriu no HDMI (saida ocupada?)', flush=True)
            else:
                self.tv_som = (time.monotonic() + 0.4, som[1] - 1)
        elif som and self.tela != 'tv_cheia':
            self.tv_som = None
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
        if self.tela == 'yt_cheia' and getattr(self, 'yt_barra_viva', False):
            if time.monotonic() < getattr(self, 'yt_barra_ate', 0):
                self.yt_barra(mostrar=False)      # segue o tempo enquanto aparece
            else:
                self.yt_tela.esconde_barra()
                self.yt_barra_viva = False
        if self.tela == 'yt_cheia' and not self.yt_tela.rodando():
            err = self.yt_tela.erro()
            print('youtube: video terminou %s' % err[:200], flush=True)
            self.yt_sai('VIDEO TERMINOU' if not err.strip() else 'O VIDEO NAO ABRIU (ATUALIZE O YT-DLP)')
            return True
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
                if not canais:
                    pass
                elif abs(d) == 1 or st['sel'] in (0, len(canais) - 1) and not 0 <= st['sel'] + d < len(canais):
                    st['sel'] = (st['sel'] + d) % len(canais)     # da a volta na lista
                else:
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
        c.ctext(166, 'SETAS: POSICAO', SLATE)
        bt_ctext(c, 179, 'LB/RB: LARGURA  LT/RT: ALTURA', SLATE)    # icones: 2 pontos acima e abaixo
        bt_ctext(c, 193, 'SELECT: PADRAO', SLATE)
        bt_ctext(c, 207, 'A: SALVAR   B: VOLTAR', IVORY)
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
        if self.itens_video()[min(getattr(self, 'video_sel', 0), len(self.itens_video()) - 1)][0] == 'gpu':
            ajuda = ['GPU DREAMCAST: VULKAN OU GLES.', 'VALE NO PROXIMO JOGO QUE ABRIR',
                     '(O JOGO COMECA DO ZERO COM A', 'GPU NOVA: SALVE ANTES DE SAIR).']
        if self.itens_video()[min(getattr(self, 'video_sel', 0), len(self.itens_video()) - 1)][0] == 'icones':
            ajuda = ['ICONES DOS BOTOES NO MENU.', 'SO O DESENHO MUDA: O BOTAO', 'CONTINUA O MESMO.',
                     'AUTO: PELO CONTROLE EM USO.']
        passo = min(F.ch + 3, (H - 16 - (y + 4)) // len(ajuda))
        for n, l in enumerate(ajuda):
            bt_text(c, 24, y + 4 + n * passo, l, SLATE, so=('SELECT',))
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
            elif k == 'gpu':
                lst, ck = ['VULKAN', 'GLES'], 'dreamcast_gpu'
            elif k == 'mefeito':
                lst, ck = list(self.fb.EFEITOS_MENU), 'menu_efeito'
            elif k == 'icones':
                lst, ck = self.ICONES, 'botoes_icones'
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
            if k == 'mefeito':
                self.fb.efeito = v
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
                self.tec.update(x=0, y=1, maius=False, rede=nome, modo='wifi')
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
                self.teclado_ok()
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
                    self.teclado_ok()
        if t['y'] == len(linhas):
            t['x'] = min(t['x'], len(TECLAS_FIM) - 1)

    def sai_teclado(self):
        self.senha = ''
        self.ent.modo_texto = False
        self.tela = 'youtube' if self.tec.get('modo') == 'youtube' else 'wifi'

    def teclado_ok(self):
        if self.tec.get('modo') == 'youtube':
            termo = self.senha.strip()
            self.senha = ''
            self.ent.modo_texto = False
            self.tela = 'youtube'
            if termo:
                self.yt_busca(termo)
        else:
            self.wifi_conecta(self.tec['rede'], self.senha)

    # ---------------------------------------------------------- APPS > KODI
    KODI_SENHA = 'retrovolt'      # do controle pelo celular (app Kore), so na rede de casa

    @staticmethod
    def usuario_kodi():
        """o usuario normal do Pi (uid 1000, ex.: rv): o Kodi roda como ele, igual pelo PuTTY.
        Como root ele nao abria (outra pasta de configuracao, sem a sessao do usuario)."""
        import pwd
        try:
            u = pwd.getpwuid(1000)
            return u.pw_name, u.pw_dir, u.pw_uid, u.pw_gid
        except KeyError:
            return None

    # mapa pronto do 8BitDo para o Kodi (driver "linux" = /dev/input/js*, ordem do xpad:
    # botoes 0 A 1 B 2 X 3 Y 4 LB 5 RB 6 SELECT 7 START 8 HOME 9 L3 10 R3; eixos 0/1 analogico
    # esquerdo, 2 LT, 3/4 analogico direito, 5 RT, 6/7 direcional). Perfil "Default", que e o
    # que os menus do Kodi usam (no perfil Xbox 360 os menus ficam sem resposta).
    KODI_MAPA = '''<?xml version="1.0" ?>
<!-- RETROVOLT: mapa do 8BitDo (gravado pelo RetroVolt; apague para configurar no Kodi) -->
<buttonmap>
    <device name="8BitDo Ultimate Wireless / Pro 2 Wired Controller" provider="linux" buttoncount="11" axiscount="8">
        <configuration>
            <axis index="2" center="-1" range="2" />
            <axis index="5" center="-1" range="2" />
        </configuration>
        <controller id="game.controller.default">
            <feature name="a" button="0" />
            <feature name="b" button="1" />
            <feature name="x" button="2" />
            <feature name="y" button="3" />
            <feature name="leftbumper" button="4" />
            <feature name="rightbumper" button="5" />
            <feature name="back" button="6" />
            <feature name="start" button="7" />
            <feature name="guide" button="8" />
            <feature name="leftthumb" button="9" />
            <feature name="rightthumb" button="10" />
            <feature name="lefttrigger" axis="+2" />
            <feature name="righttrigger" axis="+5" />
            <feature name="up" axis="-7" />
            <feature name="down" axis="+7" />
            <feature name="left" axis="-6" />
            <feature name="right" axis="+6" />
            <feature name="leftstick">
                <up axis="-1" />
                <down axis="+1" />
                <right axis="+0" />
                <left axis="-0" />
            </feature>
            <feature name="rightstick">
                <up axis="-4" />
                <down axis="+4" />
                <right axis="+3" />
                <left axis="-3" />
            </feature>
        </controller>
    </device>
</buttonmap>
'''

    def prepara_kodi(self, casa_usuario=None):
        """antes de abrir: (1) liga os addons de controle e de streaming (o Kodi do apt deixa
        desligado o que veio do sistema); (2) liga o controle pelo celular (servidor web +
        controle remoto de outros aparelhos), para usar o app Kore como controle/teclado"""
        import glob
        import sqlite3
        import xml.etree.ElementTree as ET
        casa = os.path.join(casa_usuario or os.path.expanduser('~'), '.kodi', 'userdata')
        for db in glob.glob(os.path.join(casa, 'Database', 'Addons*.db')):
            try:
                con = sqlite3.connect(db, timeout=3)
                n = con.execute("UPDATE installed SET enabled=1 WHERE enabled=0 AND addonID IN "
                                "('peripheral.joystick','inputstream.adaptive','inputstream.ffmpegdirect')").rowcount
                con.commit()
                con.close()
                if n:
                    print('kodi: %d addon(s) de controle/streaming ligados' % n, flush=True)
            except Exception as ex:
                print('kodi: addons nao ligados (%s)' % ex, flush=True)
        # mapa do 8BitDo: grava o nosso; um mapa feito no Kodi para esse controle (ex.: no perfil
        # Xbox 360, com conflito) fica guardado como .bak
        pasta = os.path.join(casa, 'addon_data', 'peripheral.joystick', 'resources', 'buttonmaps', 'xml', 'linux')
        try:
            os.makedirs(pasta, exist_ok=True)
            nosso = os.path.join(pasta, '8BitDo_Ultimate_Wireless___Pro_2_Wired_Controller_11b_8a.xml')
            for n in os.listdir(pasta):
                c = os.path.join(pasta, n)
                if n.endswith('.xml') and c != nosso:
                    try:
                        txt = open(c, errors='replace').read()
                    except OSError:
                        continue
                    if '8BitDo Ultimate Wireless / Pro 2 Wired Controller' in txt and 'RETROVOLT' not in txt:
                        os.replace(c, c + '.bak')
                        print('kodi: mapa antigo do 8BitDo guardado como %s.bak' % n, flush=True)
            atual = open(nosso, errors='replace').read() if os.path.exists(nosso) else ''
            if atual and 'RETROVOLT' not in atual:     # feito no Kodi com o mesmo nome
                os.replace(nosso, nosso + '.bak')
                print('kodi: mapa antigo do 8BitDo guardado como .bak', flush=True)
                atual = ''
            if True:
                if atual != self.KODI_MAPA:
                    with open(nosso, 'w') as f:
                        f.write(self.KODI_MAPA)
                    print('kodi: mapa do 8BitDo gravado', flush=True)
        except OSError as ex:
            print('kodi: mapa do controle nao gravado (%s)' % ex, flush=True)
        quer = {'services.webserver': 'true', 'services.webserverport': '8080',
                'services.webserverauthentication': 'true', 'services.webserverusername': 'kodi',
                'services.webserverpassword': self.KODI_SENHA, 'services.esenabled': 'true',
                'services.esallinterfaces': 'true', 'services.zeroconf': 'true'}
        arq = os.path.join(casa, 'guisettings.xml')
        try:
            if os.path.exists(arq):
                arv = ET.parse(arq)
                raiz = arv.getroot()
            else:
                os.makedirs(casa, exist_ok=True)
                raiz = ET.Element('settings', version='2')
                arv = ET.ElementTree(raiz)
            mudou = False
            atuais = {e.get('id'): e for e in raiz.findall('setting')}
            for k, v in quer.items():
                e = atuais.get(k)
                if e is None:
                    e = ET.SubElement(raiz, 'setting', id=k)
                if e.text != v or 'default' in e.attrib:
                    e.text = v
                    e.attrib.pop('default', None)
                    mudou = True
            if mudou:
                arv.write(arq, encoding='utf-8', xml_declaration=False)
                print('kodi: controle pelo celular ligado (app Kore)', flush=True)
        except Exception as ex:
            print('kodi: guisettings nao ajustado (%s)' % ex, flush=True)

    def abre_kodi(self):
        """Kodi direto na tela (GBM, sem area de trabalho), como um jogo: o menu espera ele
        fechar (no Kodi: menu Desligar > Sair) e volta para APPS"""
        exe = shutil.which('kodi-standalone') or shutil.which('kodi')
        if not exe:
            self.mostra_aviso('FALTA O KODI. NO PUTTY: SUDO APT INSTALL KODI')
            return
        c = Canvas()
        cabecalho(c, 'APPS', self.wifi, '')
        c.ctext(H // 2 - F.ch, 'ABRINDO O KODI...', ORANGE)
        c.ctext(H // 2 + 6, 'PARA VOLTAR: DESLIGAR > SAIR', SLATE)
        self.fb.show(c, self.conv)
        cmd = [exe] if exe.endswith('standalone') else [exe, '--standalone', '--windowing=gbm']
        env = None
        u = self.usuario_kodi() if os.geteuid() == 0 else None
        if u:
            nome, casa, uid, gid = u
            self.prepara_kodi(casa)
            cmd = ['runuser', '-u', nome, '--'] + cmd
            env = dict(os.environ, HOME=casa, USER=nome, LOGNAME=nome,
                       XDG_RUNTIME_DIR='/run/user/%d' % uid)
            os.makedirs('/run/user/%d' % uid, exist_ok=True)
            try:
                os.chown('/run/user/%d' % uid, uid, gid)
                os.chmod('/run/user/%d' % uid, 0o700)
            except OSError:
                pass
            for k in ('.kodi',):                  # o que o root criou antes passa para o usuario
                p0 = os.path.join(casa, k)
                if os.path.isdir(p0):
                    subprocess.run(['chown', '-R', '%d:%d' % (uid, gid), p0], capture_output=True)
        else:
            self.prepara_kodi()
        print('kodi: %s' % ' '.join(cmd), flush=True)
        copia_log()
        self.leitora.pausa.set()
        t0 = time.monotonic()
        try:
            p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
            r = p.returncode
            if r not in (0, 64, 65, 66) or time.monotonic() - t0 < 10:
                # fechou logo: o motivo vai para o LOG (e o fim do log do proprio Kodi)
                for l in (p.stdout or '').strip().splitlines()[-12:]:
                    print('kodi: %s' % l[:200], flush=True)
                for k in ([os.path.join(u[1], '.kodi/temp/kodi.log')] if u else []) + \
                        ['/root/.kodi/temp/kodi.log', os.path.expanduser('~/.kodi/temp/kodi.log')]:
                    try:
                        for l in open(k, errors='replace').read().splitlines()[-25:]:
                            if 'ERROR' in l or 'FATAL' in l or 'error' in l:
                                print('kodi.log: %s' % l[:200], flush=True)
                        break
                    except OSError:
                        pass
        except OSError as ex:
            r = -1
            print('kodi: nao abriu:', ex, flush=True)
        self.leitora.pausa.clear()
        print('kodi: fechou (codigo %s, %.0f s)' % (r, time.monotonic() - t0), flush=True)
        # o que o Kodi achou de controle (addon, driver, botoes): vai sempre para o LOG
        import re
        for k in ([os.path.join(u[1], '.kodi/temp/kodi.log')] if u else []) + [os.path.expanduser('~/.kodi/temp/kodi.log')]:
            try:
                ls = open(k, errors='replace').read().splitlines()
            except OSError:
                continue
            achou = [l for l in ls if re.search(r'joystick|peripheral|controller|8bitdo|buttonmap|udev|evdev|game\.controller',
                                                 l, re.I)]
            print('kodi.log: %d linhas sobre controle (ultimas abaixo)' % len(achou), flush=True)
            for l in achou[-40:]:
                print('kodi.log: %s' % l[:220], flush=True)
            break
        self.ent.segurando.clear()
        self._pendentes()                  # teclas apertadas dentro do Kodi nao valem aqui
        self.tela = 'menu'
        self.aba = 'APPS'
        if time.monotonic() - t0 < 10:
            self.mostra_aviso('O KODI NAO ABRIU (CODIGO %s). VEJA O LOG.TXT' % r)

    # ---------------------------------------------------------- APPS > YOUTUBE
    def yt_itens(self):
        return [('buscar', 'BUSCAR')] + [('video', v) for v in self.yt['res']] + [('atualiza', 'ATUALIZAR YT-DLP')]

    def yt_busca(self, termo):
        yt = self.yt
        yt.update(termo=termo, msg='BUSCANDO...', ocupado=True)
        print('youtube: busca', flush=True)       # o termo nao vai para o LOG

        def tarefa():
            try:
                yt['res'] = youtube.busca(termo)
                yt['sel'] = 1 if yt['res'] else 0
                yt['msg'] = '%d VIDEOS' % len(yt['res']) if yt['res'] else 'NADA ENCONTRADO'
            except RuntimeError as ex:
                yt['msg'] = str(ex)
            finally:
                yt['ocupado'] = False
                self.fila.put('_wifi')             # redesenha
        threading.Thread(target=tarefa, daemon=True).start()

    def yt_atualiza(self):
        yt = self.yt
        yt.update(msg='ATUALIZANDO O YT-DLP...', ocupado=True)

        def tarefa():
            try:
                yt['msg'] = youtube.atualiza()[1]
            finally:
                yt['ocupado'] = False
                self.fila.put('_wifi')
        threading.Thread(target=tarefa, daemon=True).start()

    def desenha_youtube(self, c):
        yt = self.yt
        moldura(c, 'YOUTUBE', 'A ESCOLHE  B VOLTA')
        if not youtube.ytdlp():
            c.text(24, 56, 'FALTA O YT-DLP. RODE O INSTALAR.SH', ORANGE)
            c.text(24, 60 + F.ch, '(PRECISA DE INTERNET)', SLATE)
            return
        itens = self.yt_itens()
        sel = min(yt['sel'], len(itens) - 1)
        x0, by0, bw, bh = self.tv_caixa()            # capa no lugar da previa da TV
        y0, passo = 54, F.ch + 6
        cabem = max(1, (H - 22 - 2 * F.ch - y0) // passo)
        ini = max(0, min(sel - cabem // 2, len(itens) - cabem))
        y = y0
        larg = max(8, (x0 - 24 - 12) // F.cw - 3)       # titulo ate a capa (o -> ocupa 3)
        for n in range(ini, min(len(itens), ini + cabem)):
            k, v = itens[n]
            on = n == sel
            if k == 'video':
                item(c, 24, y, v[1].upper()[:larg], on)
            elif k == 'buscar':
                item(c, 24, y, 'BUSCAR' + ((': ' + yt['termo'].upper())[:larg - 8] if yt['termo'] else ''), on)
            else:
                item(c, 24, y, v, on)
            y += passo
        if yt['msg']:
            c.text(24, H - 18 - 2 * F.ch, yt['msg'][:44], ORANGE if yt['ocupado'] else SLATE)
        k, v = itens[sel]
        if k == 'video':
            self.yt_desenha_capa(c, v[0], x0, by0, bw, bh)
            cabe = bw // F.cw
            if v[3]:
                c.text(x0, by0 + bh + 6, v[3], ORANGE)
            if v[2]:
                c.text(x0, by0 + bh + 10 + F.ch, v[2].upper()[:cabe], SLATE)

    def yt_desenha_capa(self, c, vid, x0, y0, bw, bh):
        """capa baixada em 2o plano (so a do video marcado); moldura vazia enquanto chega"""
        caps = self.__dict__.setdefault('yt_capas', {})
        e = max(1, self.esc or 1)
        m = caps.get(vid)
        if m is None and vid not in caps:
            caps[vid] = None                         # pedido feito
            if len(caps) > 40:                       # na memoria do menu: no maximo 40 capas
                caps.clear()
                caps[vid] = None

            def tarefa():
                caps[vid] = youtube.capa(vid, bw * e, bh * e, exe_rv_jogo()) or False
                self.fila.put('_img')
            threading.Thread(target=tarefa, daemon=True).start()
        if m and self.esc:
            c.extras.append((x0, y0, bw, bh, m, False))
        elif m and not self.esc:                     # tubo: desenha os pontos direto
            w, h, rgb = m
            for yy in range(min(h, bh)):
                for xx in range(min(w, bw)):
                    o = (yy * w + xx) * 3
                    c.px[(y0 + yy) * W + x0 + xx] = (rgb[o], rgb[o + 1], rgb[o + 2])
        else:
            c.rect(x0, y0, bw, bh, (20, 26, 52))
            txt = 'CARREGANDO...' if m is None else 'SEM CAPA'
            c.text(x0 + (bw - len(txt) * F.cw) // 2, y0 + bh // 2 - F.ch // 2, txt, SLATE)

    def acao_youtube(self, a):
        yt = self.yt
        if a == 'B':
            self.tela = 'menu'
            self.__dict__.pop('yt_capas', None)        # saiu do YouTube: capas fora da RAM
            youtube.limpa_capas(tudo=True)
            return
        if yt['ocupado'] or not youtube.ytdlp():
            return
        itens = self.yt_itens()
        sel = min(yt['sel'], len(itens) - 1)
        if a in ('CIMA', 'BAIXO'):
            yt['sel'] = (sel + (1 if a == 'BAIXO' else -1)) % len(itens)
        elif a in ('ESQ', 'DIR'):
            yt['sel'] = max(0, min(len(itens) - 1, sel + (10 if a == 'DIR' else -10)))
        elif a == 'A':
            k, v = itens[sel]
            if k == 'buscar':
                self.senha = yt['termo']
                self.tec.update(x=0, y=1, maius=False, rede='BUSCAR NO YOUTUBE', modo='youtube')
                self.tela = 'teclado'
                self.ent.modo_texto = True
            elif k == 'atualiza':
                self.yt_atualiza()
            else:
                self.yt_toca(v)

    def yt_toca(self, v):
        vid, titulo = v[0], v[1]
        if not getattr(self, 'yt_tela', None):
            self.yt_tela = tv.TelaCheia(self.fb.tubo, self.fb.w, self.fb.h, self.fb.bpp,
                                        self.cfg.get('audio_dispositivo') or AUDIO_PADRAO, self.volume())
        self.leitora.pausa.set()
        self.tela = 'yt_cheia'
        self.yt_tela.volume = self.volume()
        print('youtube: tocando um video', flush=True)
        self.yt_tela.abre(youtube.url(vid), titulo,
                          '%s\nA: PAUSA   <- ->: 10 S   LT/RT: 1 MIN   SELECT: TEMPO   B: VOLTAR' % titulo,
                          extra=['--ytdl=yes', '--ytdl-format=' + youtube.formato(self.fb.tubo),
                                 # barra e tempo grandes e legiveis (tambem no tubo); a mensagem
                                 # do seek mostra "tempo / total (porcentagem)"
                                 '--osd-bar-h=4', '--osd-bar-w=90', '--osd-font-size=%d' % (40 if self.fb.tubo else 46),
                                 '--osd-border-size=3', '--osd-color=#FFFFFF', '--osd-border-color=#000000',
                                 '--osd-on-seek=no',
                                 # avancar sem esperar: guarda bem mais do video (para frente e
                                 # para tras) e pula para o quadro-chave mais perto (sem decodificar
                                 # ate o ponto exato, que no Pi leva segundos)
                                 '--demuxer-max-bytes=150MiB', '--demuxer-max-back-bytes=50MiB',
                                 '--demuxer-readahead-secs=60', '--cache-secs=120', '--hr-seek=no',
                                 '--script-opts=ytdl_hook-ytdl_path=' + youtube.ytdlp()])

    def yt_sai(self, aviso=None):
        if getattr(self, 'yt_tela', None):
            self.yt_tela.fecha()
        self.leitora.pausa.clear()
        self.tela = 'youtube'
        if aviso:
            self.yt['msg'] = aviso

    def yt_barra(self, mostrar=True):
        """barra estilo YouTube por cima do video; some 5 s depois do ultimo botao"""
        if mostrar:
            self.yt_barra_ate = time.monotonic() + 5
        tc = self.yt_tela
        pos, dur = tc.consulta('time-pos'), tc.consulta('duration')
        tc.barra(pos, dur, bool(tc.consulta('pause')))
        self.yt_barra_viva = True

    def acao_yt_cheia(self, a):
        tc = self.yt_tela
        if a in ('B', 'START'):
            self.yt_sai()
        elif a == 'A':                      # pausa/continua
            tc.linha('no-osd cycle pause')
            self.yt_barra()
        elif a == 'SELECT':                 # so mostra onde esta
            self.yt_barra()
        elif a in ('ESQ', 'DIR', 'L2', 'R2'):
            passo = {'ESQ': -10, 'DIR': 10, 'L2': -60, 'R2': 60}[a]
            tc.linha('no-osd seek %d' % passo)
            time.sleep(0.05)                # o mpv atualiza a posicao
            self.yt_barra()
        elif a in ('CIMA', 'BAIXO'):
            v = max(0, min(100, self.volume() + (5 if a == 'CIMA' else -5)))
            self.cfg['volume'] = str(v)
            self.vol_sujo = time.monotonic()
            tc._comando(['set_property', 'volume', v])
            tc._comando(['show-text', 'VOLUME %d' % v, 1500])

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
        if t == 'yt_cheia':
            self.acao_yt_cheia(a)
            return
        if t == 'youtube':
            self.acao_youtube(a)
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
        if sis in SISTEMAS:      # pergunta antes de abrir (por cima da aba atual) (A SIM / B NAO)
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
                    curto = (self.contagem or self.tela in ('teste_video', 'tv_cheia', 'yt_cheia') or getattr(self, 'tv_previa', None)
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
                    elif b in ('_wifi', '_img', '_tv', '_icones'):
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
                if self.tela in ('tv_cheia', 'yt_cheia'):   # o mpv e dono da tela
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
