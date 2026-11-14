#!/usr/bin/env python3
"""gera versoes/rvNN/dados/discos_<sistema>.txt.gz (codigo, setores da faixa de dados e nome
de cada disco) a partir dos DATs do Redump no libretro-database (so metadados: nomes, codigos
e tamanhos; nenhum arquivo de jogo). O disco.py usa para dar nome aos discos que nao trazem o
nome gravado (PC Engine CD, Neo Geo CD) e nomes mais bonitos para Sega CD/Saturn/Dreamcast.

uso: python3 -B tools/mkdiscos.py <pasta da versao> [pasta com os .dat ja baixados]
"""
import gzip
import os
import re
import sys
import urllib.parse
import urllib.request

URL = 'https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/redump/%s.dat'
DATS = {
    'pcenginecd': 'NEC - PC Engine CD - TurboGrafx-CD',
    'neogeocd': 'SNK - Neo Geo CD',
    'segacd': 'Sega - Mega-CD - Sega CD',
    'saturn': 'Sega - Saturn',
    'dreamcast': 'Sega - Dreamcast',
}


def le_dat(nome, pasta=None):
    if pasta and os.path.exists(os.path.join(pasta, nome + '.dat')):
        return open(os.path.join(pasta, nome + '.dat'), errors='replace').read()
    req = urllib.request.Request(URL % urllib.parse.quote(nome), headers={'User-Agent': 'RetroVolt/1.0'})
    return urllib.request.urlopen(req, timeout=60).read().decode('utf-8', 'replace')


def main():
    destino = os.path.join(sys.argv[1], 'dados')
    pasta = sys.argv[2] if len(sys.argv) > 2 else None
    os.makedirs(destino, exist_ok=True)
    for sis, nome in DATS.items():
        txt = le_dat(nome, pasta)
        linhas = set()
        for g in re.findall(r'game \((.*?)\n\)', txt, re.S):
            n = re.search(r'^\s*name "([^"]*)"', g, re.M)
            sz = re.search(r'\brom \(.*?size (\d+)', g)
            if not n or not sz:
                continue
            ser = re.search(r'^\s*serial "([^"]*)"', g, re.M)
            setores = int(sz.group(1)) // 2352
            linhas.add('%s\t%d\t%s' % (ser.group(1) if ser else '', setores, n.group(1)))
        arq = os.path.join(destino, 'discos_%s.txt.gz' % sis)
        with gzip.open(arq, 'wt') as f:
            f.write('\n'.join(sorted(linhas)) + '\n')
        print('%s: %d discos -> %s (%d KB)' % (sis, len(linhas), arq, os.path.getsize(arq) // 1024))


if __name__ == '__main__':
    main()
