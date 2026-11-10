#!/usr/bin/env python3
"""Remendo do NeoCD (BIOS embutida/HLE): nome de arquivo do disco sem diferenciar
maiusculas. O IPL.TXT de jogos de verdade pede 'g046.fix' e o disco (ISO 9660) tem
'G046.FIX'; a BIOS original nao diferencia, a HLE diferenciava e dava tela preta.
uso: neocd_nomes_sem_caixa.py pasta_do_neocd"""
import sys
p = sys.argv[1] + '/src/hlebios.cpp'
s = open(p).read()
velho = '        if (entry == name)\n'
novo = ('        if ((entry.size() == name.size()) && std::equal(entry.begin(), entry.end(), name.begin(),\n'
        '                [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) ==\n'
        '                                            std::toupper(static_cast<unsigned char>(b)); }))\n')
if novo in s:
    print('remendo ja aplicado')
elif s.count(velho) == 1:
    s = s.replace(velho, novo)
    if '#include <algorithm>' not in s:
        s = '#include <algorithm>\n#include <cctype>\n' + s
    open(p, 'w').write(s)
    print('remendo aplicado: nomes sem diferenciar maiusculas')
else:
    sys.exit('remendo: trecho nao encontrado (o NeoCD mudou?)')
