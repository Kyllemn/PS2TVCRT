#!/usr/bin/env python3
"""Converte fontes bitmap (PCF/BDF) para o formato do RetroVolt Pi.

Saida: fontes.py com FONTES = {nome: (largura, altura, hex)}; cada glifo (codigos
32..255, Latin-1) e uma sequencia de 'altura' linhas, 1 byte por linha, bit 0 =
pixel da esquerda (mesmo formato da font8x8 antiga).
uso: mkfonte.py saida.py nome=arquivo.pcf.gz:LxA[:negrito] ...
"""
import gzip, io, sys
from PIL import BdfFontFile, PcfFontFile, Image, ImageDraw

def carrega(path):
    data = open(path, 'rb').read()
    if path.endswith('.gz'):
        data = gzip.decompress(data)
    ff = (BdfFontFile.BdfFontFile if path.endswith(('.bdf', '.bdf.gz')) else PcfFontFile.PcfFontFile)(io.BytesIO(data))
    return ff

def glifos(path, cw, chh):
    ff = carrega(path)
    # glyph[i] = (dx, dy), dst bbox (x0,y0,x1,y1) relativo a baseline, src, im
    asc = max(-g[1][1] for g in ff.glyph if g)  # altura acima da baseline
    out = bytearray()
    for code in range(32, 256):
        g = ff.glyph[code] if code < len(ff.glyph) else None
        cel = [0] * chh
        if g:
            (dx, dy), (x0, y0, x1, y1), src, im = g
            gi = im.crop(src)
            for yy in range(gi.height):
                for xx in range(gi.width):
                    if gi.getpixel((xx, yy)):
                        X = x0 + xx; Y = asc + y0 + yy
                        if 0 <= X < cw and 0 <= Y < chh:
                            cel[Y] |= 1 << X
        out += bytes(cel)
    return out

def main():
    saida = sys.argv[1]
    linhas = ['# gerado por tools/mkfonte.py - fontes bitmap (Latin-1, codigos 32..255)',
              '# X11 misc-fixed: dominio publico. Spleen: BSD-2 (c) Frederic Cambus.',
              'FONTES = {']
    for arg in sys.argv[2:]:
        nome, resto = arg.split('=', 1)
        negrito = resto.endswith(':negrito')       # nome=arquivo:LxA:negrito -> L+1 de largura
        if negrito:
            resto = resto[:-len(':negrito')]
        path, tam = resto.rsplit(':', 1)
        cw, chh = map(int, tam.split('x'))
        b = glifos(path, cw, chh)
        if negrito:                                 # cada ponto ganha o vizinho da direita
            b = bytes((v | (v << 1)) & 0xff for v in b)
            cw += 1
        linhas.append("    '%s': (%d, %d, '%s')," % (nome, cw, chh, b.hex()))
    linhas.append('}')
    open(saida, 'w').write('\n'.join(linhas) + '\n')

main()
