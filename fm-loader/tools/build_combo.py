"""Monta o CD combinado: menu + Front Mission 1st + Front Mission 2.
uso: build_combo.py <fm1.bin> <fm2.bin> <dir_saida>
Os .bin devem ser as imagens ja traduzidas (MODE2/2352, 1 faixa). Precisa do PSn00bSDK
(dumpsxiso/mkpsxiso) em /opt/psx/sdk/bin e de build/FMMENU.EXE e build/MENU.DAT."""
import sys, os, re, shutil, subprocess
SDK = '/opt/psx/sdk/bin/'
HERE = os.path.dirname(os.path.abspath(__file__)) + '/..'
fm1, fm2, out = map(os.path.abspath, sys.argv[1:4])
os.makedirs(out, exist_ok=True); os.chdir(out)
for src, x in ((fm1, 'x1'), (fm2, 'x2')):
    subprocess.run([SDK + 'dumpsxiso', '-y', '-x', x, '-s', x + '.xml', src], check=True, stdout=subprocess.DEVNULL)
open('SYSTEM.CNF', 'w', newline='').write('BOOT = cdrom:\\FMMENU.EXE;1\r\nTCB = 4\r\nEVENT = 16\r\nSTACK = 801FFFF0\r\n')
shutil.copy(HERE + '/build/FMMENU.EXE', 'FMMENU.EXE'); shutil.copy(HERE + '/build/MENU.DAT', 'MENU.DAT')
x1 = open('x1.xml').read(); x2 = open('x2.xml').read()
# FM1st: so arquivos na raiz; fica um DUMMY.BIN (o do FM2) e o SYSTEM.CNF do menu
f1 = [l.strip() for l in x1.splitlines() if l.strip().startswith('<file ') and 'SYSTEM.CNF' not in l and 'DUMMY.BIN' not in l]
tree2 = x2.split('<directory_tree gmt_offs="0">')[1].split('<dummy sectors="150"')[0]
tree2 = '\n'.join(l for l in tree2.splitlines() if 'SYSTEM.CNF' not in l)
xml = '''<iso_project image_name="fm_combo.bin" cue_sheet="fm_combo.cue">
    <track type="data" xa_edc="true" new_type="false">
        <identifiers system="PLAYSTATION" application="PLAYSTATION" volume="FRONT_MISSION_COLLECTION" copyright="SQUAREENIX"/>
        <license file="x2/license_data.dat"/>
        <default_attributes gmt_offs="36" xa_attrib="32" xa_perm="1365" xa_gid="0" xa_uid="0"/>
        <directory_tree gmt_offs="0">
            <file name="SYSTEM.CNF" source="SYSTEM.CNF" type="data"/>
            <file name="FMMENU.EXE" source="FMMENU.EXE" type="data"/>
            <file name="FMMENU.DAT" source="MENU.DAT" type="data"/>
''' + '\n'.join('            ' + l for l in f1) + '\n' + tree2 + '''
            <dummy sectors="150" type="0"/>
        </directory_tree>
    </track>
</iso_project>
'''
open('combo.xml', 'w').write(xml)
subprocess.run([SDK + 'mkpsxiso', '-y', 'combo.xml'], check=True)
