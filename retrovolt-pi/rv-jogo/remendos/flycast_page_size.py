#!/usr/bin/env python3
"""Flycast: o CMakeLists pega o tamanho de pagina da maquina que COMPILA (getconf PAGESIZE)
e passa -DPAGE_SIZE=... para o codigo. Compilando no PC (4 KB) para o Pi 5 (kernel com
paginas de 16 KB) o mprotect da memoria emulada falha e o nucleo para (SIGTRAP).
Remendo: so pergunta ao getconf se nao vier -DPAGE_SIZE=... na linha do cmake.
uso: flycast_page_size.py pasta_do_flycast"""
import os
import sys

p = os.path.join(sys.argv[1], 'CMakeLists.txt')
s = open(p).read()
velho = '\texecute_process(COMMAND getconf PAGESIZE OUTPUT_VARIABLE PAGE_SIZE OUTPUT_STRIP_TRAILING_WHITESPACE)\n'
novo = ('\tif(NOT PAGE_SIZE)\n'
        '\t\texecute_process(COMMAND getconf PAGESIZE OUTPUT_VARIABLE PAGE_SIZE OUTPUT_STRIP_TRAILING_WHITESPACE)\n'
        '\tendif()\n')
if novo in s:
    print('flycast: remendo do PAGE_SIZE ja aplicado')
elif s.count(velho) == 1:
    open(p, 'w').write(s.replace(velho, novo))
    print('flycast: PAGE_SIZE vem da linha do cmake')
else:
    sys.exit('flycast: nao achei o getconf PAGESIZE no CMakeLists.txt (mudou?)')
