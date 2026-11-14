#!/usr/bin/env python3
"""gera versoes/rvNN/dados/fbneo_roms.txt.gz a partir do DAT do FBNeo (so metadados: nome, CRC e
tamanho de cada arquivo de cada jogo, pai/clone e BIOS). Usado por OPCOES > VERIFICAR ARCADE.

Formato (texto, uma linha por registro):
  G <tab> jogo <tab> pai (cloneof) <tab> bios (romof quando e BIOS) <tab> 1 se for BIOS
  R <tab> nome <tab> crc <tab> tamanho <tab> P se vem do pai (merge), B se vem da BIOS
uso: python3 -B tools/mkfbneo_roms.py "<DAT Arcade only>" <pasta da versao>
"""
import gzip
import os
import sys
import xml.etree.ElementTree as ET


def main():
    dat, versao = sys.argv[1], sys.argv[2]
    raiz = ET.parse(dat).getroot()
    jogos = {g.get('name'): g for g in raiz.findall('game')}
    bios = {n for n, g in jogos.items() if g.get('isbios') == 'yes'}
    linhas = []
    for n, g in jogos.items():
        pai = g.get('cloneof') or ''
        romof = g.get('romof') or ''
        b = romof if romof in bios else (jogos[pai].get('romof') if pai in jogos and jogos[pai].get('romof') in bios else '')
        linhas.append('G\t%s\t%s\t%s\t%s' % (n, pai, b, '1' if n in bios else ''))
        roms_bios = {(r.get('crc') or '').lower() for r in jogos[b].findall('rom')} if b in jogos else set()
        roms_pai = {(r.get('crc') or '').lower() for r in jogos[pai].findall('rom')} if pai in jogos else set()
        for r in g.findall('rom'):
            crc = (r.get('crc') or '').lower()
            if not crc or r.get('status') == 'nodump':
                continue
            origem = ''
            if r.get('merge'):
                origem = 'B' if crc in roms_bios and crc not in roms_pai else 'P'
            linhas.append('R\t%s\t%s\t%s\t%s' % (r.get('name'), crc, r.get('size') or '0', origem))
    arq = os.path.join(versao, 'dados', 'fbneo_roms.txt.gz')
    with gzip.open(arq, 'wt') as f:
        f.write('\n'.join(linhas) + '\n')
    print('%d jogos (%d BIOS) -> %s (%d KB)' % (len(jogos), len(bios), arq, os.path.getsize(arq) // 1024))


if __name__ == '__main__':
    main()
