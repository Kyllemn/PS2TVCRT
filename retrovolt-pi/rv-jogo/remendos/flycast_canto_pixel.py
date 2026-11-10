#!/usr/bin/env python3
"""Flycast GLES na GPU do Pi (V3D): jogos 2D montados com blocos (Gunbird 2, 320x240)
saem com um ponto mais escuro a cada 16 pixels, no canto onde 4 blocos se encontram:
o pixel do canto e pintado duas vezes (o desempate de vertice exatamente no centro do
pixel). Remendo: desloca TODOS os vertices pela mesma fracao minima de pixel (os lados
compartilhados continuam iguais, sem frestas), para nenhum canto cair no centro exato.
uso: flycast_canto_pixel.py pasta_do_flycast"""
import os
import sys

MARCA = '/* RETROVOLT: canto fora do centro do pixel */'
NOVO = ('\tgl_Position = vpos;\n'
        '\t' + MARCA + '\n'
        '\tgl_Position.xy += vec2(1.0e-4, 1.3e-4) * gl_Position.w;\n')
n = 0
for arq in ('core/rend/gles/gles.cpp', 'core/rend/gles/naomi2.cpp'):
    p = os.path.join(sys.argv[1], arq)
    s = open(p).read()
    if MARCA in s:
        n += 1
        continue
    velho = '\tgl_Position = vpos;\n'
    if s.count(velho) != 1:
        sys.exit('flycast: nao achei o gl_Position do vertex shader em %s (mudou?)' % arq)
    open(p, 'w').write(s.replace(velho, NOVO))
    n += 1
print('flycast: vertices deslocados 1/60 de pixel (%d shaders)' % n)
