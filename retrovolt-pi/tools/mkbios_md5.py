#!/usr/bin/env python3
"""gera versoes/rvNN/dados/bios_md5.json a partir do System.dat do libretro (lista publica de
nome/tamanho/MD5 das BIOS; nenhuma BIOS dentro). O menu reconhece a BIOS pelo conteudo (MD5),
nao pelo nome do arquivo, e entrega ao nucleo com o nome que ele procura.
uso: python3 -B mkbios_md5.py System.dat saida.json
System.dat: https://raw.githubusercontent.com/libretro/libretro-database/master/dat/System.dat"""
import json
import re
import sys

# grupo do System.dat -> sistema do RetroVolt; nomes que o nucleo usa (o 1o da lista ganha
# quando o mesmo conteudo aparece com varios nomes); descricao curta para a tela BIOS
GRUPOS = {
    'Sony - PlayStation': 'ps1',
    'Sega - Mega CD - Sega CD': 'segacd',
    'Sega - Mega Drive - Genesis': 'megadrive',
    'Sega - Master System - Mark III': 'mastersystem',
    'Sega - Game Gear': 'gamegear',
    'NEC - PC Engine - TurboGrafx 16 - SuperGrafx': 'pcengine',
    'Sega - Saturn': 'saturn',
    'Sega - Dreamcast': 'dreamcast',
    'SNK - NeoGeo CD': 'neogeocd',
    'Nintendo - Game Boy Advance': 'gba',
    'Nintendo - Gameboy': 'gb',
    'Nintendo - Gameboy Color': 'gbc',
    'Nintendo - Nintendo DS': 'nds',
    'Nintendo - Famicom Disk System': 'nes',
}
PREFERE = ['scph5501.bin', 'scph1001.bin', 'scph7001.bin', 'scph101.bin', 'scph5500.bin', 'scph5502.bin',
           'sega_101.bin', 'mpr-17933.bin', 'saturn_bios.bin', 'dc_boot.bin', 'dc_flash.bin', 'syscard3.pce',
           'gb_bios.bin', 'gbc_bios.bin', 'bios7.bin', 'bios9.bin', 'firmware.bin', 'neocd_f.rom', 'neocd_sf.rom',
           'neocd_z.rom', 'neocd_sz.rom', 'neocd_t.rom', 'neocd_st.rom', 'uni-bioscd.rom', 'bios.sms']
TROCA = {'flash.bin': 'dc_flash.bin', 'boot.bin': 'dc_boot.bin', 'dmg_boot.bin': 'gb_bios.bin', 'dmg_rom.bin': 'gb_bios.bin',
         'cgb_boot.bin': 'gbc_bios.bin', 'cgb_bios.bin': 'gbc_bios.bin', 'nds7.bin': 'bios7.bin', 'nds9.bin': 'bios9.bin'}
DESC = {'scph1001.bin': 'PS1 EUA', 'scph5501.bin': 'PS1 EUA', 'scph7001.bin': 'PS1 EUA', 'scph1000.bin': 'PS1 JAPAO',
        'scph5500.bin': 'PS1 JAPAO', 'scph5502.bin': 'PS1 EUROPA', 'scph1002.bin': 'PS1 EUROPA', 'scph101.bin': 'PSONE EUA',
        'sega_101.bin': 'SATURN JAPAO', 'mpr-17933.bin': 'SATURN EUA/EUROPA', 'bios_CD_U.bin': 'SEGA CD EUA',
        'bios_CD_E.bin': 'MEGA CD EUROPA', 'bios_CD_J.bin': 'MEGA CD JAPAO', 'syscard3.pce': 'SUPER CD-ROM2 3.0',
        'dc_boot.bin': 'DREAMCAST', 'dc_flash.bin': 'DREAMCAST FLASH', 'gba_bios.bin': 'GBA', 'bios7.bin': 'DS ARM7',
        'bios9.bin': 'DS ARM9', 'firmware.bin': 'DS FIRMWARE', 'disksys.rom': 'FAMICOM DISK SYSTEM'}
PULA = ('.zip', '.db', '.nes')     # zips (neogeo, kronos) e listas nao sao BIOS de 1 arquivo


def main(dat, saida):
    com, tab = None, {}
    for l in open(dat, encoding='utf-8', errors='replace'):
        m = re.search(r'comment "([^"]*)"', l)
        if m:
            com = m.group(1)
        m = re.search(r'rom \( name "?(.+?)"? size (\d+) .*?md5 ([0-9a-fA-F]{32})', l)
        if not (m and com in GRUPOS):
            continue
        nome = m.group(1).rsplit('/', 1)[-1]
        if nome.lower().endswith(PULA) or 'kronos' in m.group(1):
            continue
        nome = TROCA.get(nome, nome)
        md5, tam, sis = m.group(3).lower(), int(m.group(2)), GRUPOS[com]
        atual = tab.get(md5)
        pref = lambda n: PREFERE.index(n) if n in PREFERE else 99
        if atual is None or pref(nome) < pref(atual[1]):
            tab[md5] = [sis, nome, DESC.get(nome, nome.upper()), tam]
    json.dump(tab, open(saida, 'w'), indent=0, sort_keys=True)
    print('%d BIOS conhecidas -> %s' % (len(tab), saida))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
