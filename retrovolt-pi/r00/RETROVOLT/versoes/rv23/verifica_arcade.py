"""RETROVOLT - OPCOES > VERIFICAR ARCADE (R28m).

Confere os .zip de roms/arcade, roms/neogeo e da pasta bios contra a lista do FBNeo que vem com
o nucleo (dados/fbneo_roms.txt.gz, gerado do DAT oficial por tools/mkfbneo_roms.py). So le o
indice de cada .zip (nome e CRC de cada arquivo): rapido, nao descompacta nada.

Para cada jogo diz: OK, FALTAM N (quais arquivos, pelo CRC), SEM BIOS, DESCONHECIDO (nome que o
FBNeo nao conhece) ou NAO CONFERE (.7z). O relatorio completo vai para
RETROVOLT/verificacao_arcade.txt (inclui o que tem dentro de cada .zip com problema, para
saber de que versao ele e).
"""
import gzip
import os
import time
import zipfile

AQUI = os.path.dirname(os.path.abspath(__file__))
_DB = None


def banco():
    global _DB
    if _DB is None:
        db, atual = {}, None
        try:
            with gzip.open(os.path.join(AQUI, 'dados', 'fbneo_roms.txt.gz'), 'rt') as f:
                for l in f:
                    p = l.rstrip('\n').split('\t')
                    if p[0] == 'G':
                        atual = db[p[1]] = {'pai': p[2], 'bios': p[3], 'eh_bios': p[4] == '1', 'roms': []}
                    elif p[0] == 'R' and atual is not None:
                        atual['roms'].append((p[1], p[2], int(p[3] or 0), p[4]))
        except OSError:
            pass
        _DB = db
    return _DB


def conteudo_zip(path):
    """{crc: nome} dos arquivos do .zip, ou None se nao der para ler"""
    try:
        with zipfile.ZipFile(path) as z:
            return {'%08x' % i.CRC: i.filename for i in z.infolist() if not i.is_dir()}
    except (OSError, zipfile.BadZipFile, ValueError):
        return None


def acha_zips(base):
    """{nome curto: caminho} de roms/arcade, roms/neogeo e bios (a primeira ocorrencia vale)"""
    achados = {}
    for pasta in ('roms/arcade', 'roms/neogeo', 'bios/neogeo', 'bios/arcade', 'bios'):
        raiz = os.path.join(base, pasta)
        for dirpath, dirs, arqs in os.walk(raiz):
            if pasta == 'bios' and dirpath != raiz:
                dirs[:] = []
            for a in sorted(arqs):
                n, ext = os.path.splitext(a)
                if ext.lower() in ('.zip', '.7z') and not a.startswith('.'):
                    achados.setdefault(n.lower(), os.path.join(dirpath, a))
    return achados


NEOGEO_OBRIGATORIOS = ('sm1.sm1', 'sfix.sfix', '000-lo.lo')


def confere_bios(nome, g, tem):
    """(faltando, aviso) de um .zip de BIOS"""
    if nome == 'neogeo':
        falta = [r[0] for r in g['roms'] if r[0] in NEOGEO_OBRIGATORIOS and r[1] not in tem]
        principais = [r for r in g['roms'] if r[0] not in NEOGEO_OBRIGATORIOS]
        if not any(r[1] in tem for r in principais):
            falta.append('(nenhuma BIOS principal: sp-s2.sp1, sp-s3.sp1, uni-bios...)')
        return falta
    return [r[0] for r in g['roms'] if r[1] not in tem]


def verifica(base, progresso=lambda *a: None, cancelado=lambda: False):
    """lista de (nome curto, caminho, status, detalhes) e o caminho do relatorio"""
    db = banco()
    zips = acha_zips(base)
    nomes = sorted(zips)
    res = []
    cache = {}

    def tem_de(n):
        if n not in cache:
            p = zips.get(n)
            cache[n] = (conteudo_zip(p) or {}) if p and p.lower().endswith('.zip') else {}
        return cache[n]

    for k, n in enumerate(nomes):
        if cancelado():
            break
        progresso(k, len(nomes), n)
        p = zips[n]
        if n not in db:
            if n in ('hiscore', 'qsound'):
                continue
            res.append((n, p, 'DESCONHECIDO', ['o FBNeo nao tem jogo com esse nome']))
            continue
        if p.lower().endswith('.7z'):
            res.append((n, p, 'NAO CONFERE', ['.7z: o verificador so le .zip (o FBNeo abre .7z)']))
            continue
        tem = tem_de(n)
        if not tem and conteudo_zip(p) is None:
            res.append((n, p, 'ZIP COM DEFEITO', ['o arquivo .zip nao abre']))
            continue
        g = db[n]
        if g['eh_bios']:
            falta = confere_bios(n, g, tem)
        else:
            pai = tem_de(g['pai']) if g['pai'] else {}
            falta = [r[0] for r in g['roms'] if r[3] != 'B' and r[1] not in tem and not (r[3] == 'P' and r[1] in pai)]
        det = []
        if falta:
            det.append('FALTAM: ' + ', '.join(falta))
        if not g['eh_bios'] and g['bios'] and g['bios'] not in zips:
            det.append('SEM BIOS: %s.zip nao encontrado' % g['bios'])
        if not g['eh_bios'] and g['pai'] and g['pai'] not in zips and any(r[3] == 'P' and r[1] not in tem for r in g['roms']):
            det.append('CLONE: precisa tambem do %s.zip (jogo pai)' % g['pai'])
        status = 'OK' if not det else ('FALTAM %d' % len(falta) if falta else det[0].split(':')[0])
        if det:
            det.append('TEM NO ZIP: ' + ', '.join('%s (%s)' % (nm, c) for c, nm in sorted(tem.items(), key=lambda x: x[1])))
        res.append((n, p, status, det))
    progresso(len(nomes), len(nomes), '')
    rel = os.path.join(base, 'verificacao_arcade.txt')
    try:
        with open(rel, 'w') as f:
            f.write('RETROVOLT - verificacao do arcade (FBNeo) em %s\n' % time.strftime('%d/%m/%Y %H:%M'))
            ok = sum(1 for r in res if r[2] == 'OK')
            f.write('%d .zip conferidos: %d OK, %d com problema\n\n' % (len(res), ok, len(res) - ok))
            for n, p, st, det in sorted(res, key=lambda r: (r[2] == 'OK', r[0])):
                f.write('%-12s %-16s %s\n' % (n, st, p))
                for d in det:
                    f.write('             %s\n' % d)
    except OSError:
        rel = None
    return res, rel
