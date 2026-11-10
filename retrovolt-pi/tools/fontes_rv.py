#!/usr/bin/env python3
"""Gera versoes/rvNN/fontes.py com TODAS as fontes do RetroVolt (fontes em tools/fontes_src).
uso: python3 tools/fontes_rv.py versoes/rvNN/fontes.py
Depois: tools/mkfonteh.py versoes/rvNN rv-jogo/fonte.h (fontes do menu de dentro do jogo).

Licencas (textos em fontes_src/LICENCA-*): X11 misc-fixed = dominio publico;
Spleen = BSD-2 (c) Frederic Cambus; Tamzen = licenca livre da Tamsyn (c) Scott Fial /
Suraj N. Kurapati; GG 8X15 = conversao para bitmap da fonte Ithaca (c) 2025 GGBotNet,
SIL Open Font License 1.1 (o nome reservado "Ithaca" nao e usado na versao convertida)."""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, AQUI)
from mkfonte import glifos  # noqa: E402

SRC = os.path.join(AQUI, 'fontes_src')


def negrito(b):
    """cada ponto ganha o vizinho da direita (a celula fica 1 ponto mais larga)"""
    return bytes((v | (v << 1)) & 0xff for v in b)


def completa(b, ch, reserva):
    """glifo vazio (fonte sem acentos) pega o da fonte reserva, mesma altura"""
    out = bytearray(b)
    for i in range(224):
        if not any(b[i * ch:(i + 1) * ch]) and i != 0:
            out[i * ch:(i + 1) * ch] = reserva[i * ch:(i + 1) * ch]
    return bytes(out)


def ttf(path, px, cw, ch, topo=0):
    """fonte TTF pixel desenhada no tamanho nativo (px por em), cada glifo centralizado
    numa celula cw x ch (o menu e de largura fixa)"""
    f = ImageFont.truetype(path, px)
    out = bytearray()
    for code in range(32, 256):
        im = Image.new('1', (cw + 8, ch + 4), 0)
        ImageDraw.Draw(im).text((4, -topo), chr(code), font=f, fill=1)
        bb = im.getbbox()
        cel = [0] * ch
        if bb:
            larg = bb[2] - bb[0]
            dx = bb[0] - max(0, (cw - larg) // 2)
            for y in range(min(ch, im.height)):
                for x in range(bb[0], bb[2]):
                    if im.getpixel((x, y)) and 0 <= x - dx < cw:
                        cel[y] |= 1 << (x - dx)
        out += bytes(cel)
    return bytes(out)


def m(n):
    return os.path.join(SRC, n)


def lista():
    misc58 = glifos(m('misc-5x8.pcf.gz'), 5, 8)
    f = []
    # as que ja existiam (mesma ordem; 5X7 e a padrao do autor)
    f.append(('5x7', 5, 7, glifos(m('misc-5x7.pcf.gz'), 5, 7)))
    f.append(('5x7 negrito', 6, 7, negrito(glifos(m('misc-5x7.pcf.gz'), 5, 7))))
    f.append(('4x6', 4, 6, glifos(m('misc-4x6.pcf.gz'), 4, 6)))
    f.append(('5x8', 5, 8, misc58))
    f.append(('spleen 5x8', 5, 8, completa(glifos(m('spleen-5x8.bdf'), 5, 8), 8, misc58)))  # sem acentos: completa
    f.append(('6x9', 6, 9, glifos(m('misc-6x9.pcf.gz'), 6, 9)))
    f.append(('6x10', 6, 10, glifos(m('misc-6x10.pcf.gz'), 6, 10)))
    f.append(('6x12', 6, 12, glifos(m('misc-6x12.pcf.gz'), 6, 12)))
    f.append(('spleen 6x12', 6, 12, completa(glifos(m('spleen-6x12.bdf'), 6, 12), 12, glifos(m('misc-6x12.pcf.gz'), 6, 12))))
    f.append(('6x13', 6, 13, glifos(m('misc-6x13.pcf.gz'), 6, 13)))
    # novas
    f.append(('5x8 negrito', 6, 8, negrito(misc58)))
    f.append(('6x10 negrito', 7, 10, negrito(glifos(m('misc-6x10.pcf.gz'), 6, 10))))
    f.append(('6x13 negrito', 7, 13, negrito(glifos(m('misc-6x13.pcf.gz'), 6, 13))))
    f.append(('tamzen 5x9', 5, 9, glifos(m('Tamzen5x9r.bdf'), 5, 9)))
    f.append(('tamzen 6x12', 6, 12, glifos(m('Tamzen6x12r.bdf'), 6, 12)))
    f.append(('tamzen 7x13', 7, 13, glifos(m('Tamzen7x13r.bdf'), 7, 13)))
    f.append(('tamzen 7x13 negrito', 7, 13, glifos(m('Tamzen7x13b.bdf'), 7, 13)))
    f.append(('tamzen 8x15', 8, 15, glifos(m('Tamzen8x15r.bdf'), 8, 15)))
    f.append(('tamzen 8x15 negrito', 8, 15, glifos(m('Tamzen8x15b.bdf'), 8, 15)))
    f.append(('spleen 8x16', 8, 16, completa(glifos(m('spleen-8x16.bdf'), 8, 16), 16,
                                              glifos(m('Tamzen8x15r.bdf'), 8, 16))))
    f.append(('gg 8x15', 8, 15, ttf(m('gg-8x15-ithaca.ttf'), 16, 8, 15)))
    return f


def main():
    saida = sys.argv[1]
    linhas = ['# gerado por tools/fontes_rv.py - fontes bitmap (Latin-1, codigos 32..255)',
              '# X11 misc-fixed: dominio publico. Spleen: BSD-2 (c) Frederic Cambus. Tamzen: livre',
              '# (c) Scott Fial / Suraj N. Kurapati. GG 8X15: da fonte Ithaca (c) 2025 GGBotNet,',
              '# SIL Open Font License 1.1. Licencas completas em licencas/.',
              'FONTES = {']
    for nome, cw, ch, b in lista():
        assert len(b) == 224 * ch, nome
        linhas.append("    '%s': (%d, %d, '%s')," % (nome, cw, ch, b.hex()))
    linhas.append('}')
    open(saida, 'w').write('\n'.join(linhas) + '\n')
    print('%d fontes em %s' % (len(linhas) - 6, saida))


if __name__ == '__main__':
    main()
