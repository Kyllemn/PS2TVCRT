#!/usr/bin/env python3
"""Gera a lista codigo -> nome dos jogos (para o RetroVolt mostrar o nome do disco).
Fonte: libretro-database (metadat/redump). Saida: texto 'CODIGO<TAB>NOME' compactado (gzip).
uso: mknomes.py arquivo.dat saida.txt.gz"""
import gzip, re, sys, unicodedata
dat, saida = sys.argv[1], sys.argv[2]
txt = open(dat, encoding='utf-8', errors='replace').read()
nomes = {}
for g in re.finditer(r'game \(\s*name "([^"]+)"(.*?)\n\)', txt, re.S):
    nome, corpo = g.group(1), g.group(2)
    m = re.search(r'^\s*serial "([^"]+)"', corpo, re.M)
    if not m:
        continue
    disco = re.search(r'\(Disc (\d+)\)', nome)
    base = re.sub(r'\s*\([^)]*\)', '', nome).strip()
    base = unicodedata.normalize('NFKD', base).encode('ascii', 'ignore').decode().upper()
    if disco:
        base += ' (DISCO %s)' % disco.group(1)
    for s in re.split(r'[,\s]+', m.group(1)):
        s = s.strip().upper().replace('_', '-').encode('ascii', 'ignore').decode()
        if s and s not in nomes:
            nomes[s] = base
with gzip.open(saida, 'wt', encoding='ascii') as f:
    for s in sorted(nomes):
        f.write('%s\t%s\n' % (s, nomes[s]))
print(len(nomes), 'codigos')
