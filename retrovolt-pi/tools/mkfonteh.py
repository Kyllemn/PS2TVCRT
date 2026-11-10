#!/usr/bin/env python3
"""Gera rv-jogo/fonte.h a partir das fontes do menu (versoes/rvNN/fontes.py + 8x8).
uso: mkfonteh.py pasta_da_versao saida.h"""
import re, sys
ident = lambda n: re.sub(r'\W', '_', n)   # 'SPLEEN 6X12' -> SPLEEN_6X12 (nome em C)
pasta, saida = sys.argv[1], sys.argv[2]
sys.path.insert(0, pasta)
from fontes import FONTES
src = open(pasta + '/rv_app.py').read()
hx = ''.join(re.findall(r"'([0-9a-f]+)'", src.split('FONT_HEX = (')[1].split(')')[0]))
lista = [('8X8', 8, 8, bytes.fromhex(hx) + bytes(8 * 129))]
lista += [(n.upper(), cw, ch, bytes.fromhex(h)) for n, (cw, ch, h) in FONTES.items()]
out = ['/* gerado por tools/mkfonteh.py: fontes do menu, Latin-1 32..255, 1 byte por linha, bit0 = esquerda */',
       'typedef struct { const char *nome; int cw, ch; const unsigned char *b; } Fonte;']
for n, cw, ch, b in lista:
    out.append('static const unsigned char F_%s[] = {%s};' % (ident(n), ','.join(str(x) for x in b)))
out.append('static const Fonte FONTES[] = {%s};' % ', '.join('{"%s", %d, %d, F_%s}' % (n, cw, ch, ident(n)) for n, cw, ch, _ in lista))
out.append('#define N_FONTES %d' % len(lista))
open(saida, 'w').write('\n'.join(out) + '\n')
