#!/usr/bin/env python3
"""gera nomes_fbneo.json (vai em versoes/rvNN/, fora do git) a partir do DAT oficial do FBNeo:
{"nomes": {zip: descricao}, "bios": [zips que sao BIOS]}.
uso: python3 -B mknomes_fbneo.py "FBNeo/dats/FinalBurn Neo (ClrMame Pro XML, Arcade only).dat" saida.json
(o hiscore.dat vai em versoes/rvNN/fbneo/, copiado de FBNeo/metadata/hiscore.dat)"""
import json
import sys
import xml.etree.ElementTree as ET


def main(dat, saida):
    raiz = ET.parse(dat).getroot()
    nomes, bios = {}, []
    for g in raiz.iter('game'):
        n = g.get('name')
        d = (g.findtext('description') or n).strip()
        nomes[n] = d
        if g.get('isbios') == 'yes':
            bios.append(n)
    with open(saida, 'w') as f:
        json.dump({'nomes': nomes, 'bios': sorted(bios)}, f, ensure_ascii=False, separators=(',', ':'))
    print('%d nomes, %d bios -> %s' % (len(nomes), len(bios), saida))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
