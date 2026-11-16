"""RETROVOLT - onde fica cada coisa (R29, desenho do autor).

SISTEMA (sempre no cartao microSD, /var/lib/retrovolt/RETROVOLT):
    config/  (retrovolt.cfg fica em /var/lib/retrovolt/config, opcoes dos nucleos, recentes,
             favoritos), saves/, states/, logs/
ARMAZENAMENTO (jogos e midias: cartao, pendrive USB ou M.2 NVMe; OPCOES > ARMAZENAMENTO):
    roms/ (opcional na copia: pode ser enorme), bios/ e tv/ (sempre copiados ao mudar),
    imagens/ (capas, discos, wallpapers), capturas/  (opcionais, junto com as roms)
O pendrive tambem e o lugar das atualizacoes (RETROVOLT/update), em qualquer escolha.

Mudar de armazenamento nunca apaga nada da origem: copia sem sobrescrever o que ja existe no
destino. BIOS e listas de TV vao sempre; roms/imagens/capturas so com "COPIAR TUDO".
"""
import os
import shutil
import subprocess

DADOS = '/var/lib/retrovolt'
CARTAO = os.path.join(DADOS, 'RETROVOLT')
USB = os.environ.get('RV_USB', '/media/rvusb')
M2 = os.environ.get('RV_M2', '/media/rvm2')
LUGARES = ('cartao', 'pendrive', 'm2')
NOMES = {'cartao': 'CARTAO (MICRO SD)', 'pendrive': 'PENDRIVE (USB)', 'm2': 'M.2 (NVME)'}
PASTAS_SISTEMA = ('config', 'config/nucleos', 'saves', 'states', 'logs')
PASTAS_MIDIA = ('bios', 'roms', 'tv', 'imagens/consoles', 'imagens/wallpapers', 'imagens/capas',
                'imagens/discos', 'capturas')
OBRIGATORIAS = ('bios', 'tv')
OPCIONAIS = ('roms', 'imagens', 'capturas')


def raiz(lugar):
    """pasta RETROVOLT do lugar, ou None se o dispositivo nao estiver ligado"""
    if lugar == 'pendrive':
        if os.path.isdir(os.path.join(USB, 'RETROVOLT')) or monta(USB, 'sd'):
            return os.path.join(USB, 'RETROVOLT')
        return None
    if lugar == 'm2':
        return os.path.join(M2, 'RETROVOLT') if monta(M2, 'nvme') else None
    return CARTAO


def monta_m2():
    return monta(M2, 'nvme')


def monta(ponto, prefixo):
    """monta a 1a particao utilizavel (vfat, exfat ou ext4) de um pendrive ('sd') ou SSD M.2
    ('nvme') em 'ponto'. O lancador so monta pendrive que ja tem a pasta RETROVOLT; aqui vale
    qualquer um (a pasta e criada quando ele vira o armazenamento)."""
    if os.path.ismount(ponto):
        return True
    try:
        nomes = os.listdir('/dev')
    except OSError:
        return False
    if prefixo == 'nvme':
        devs = sorted(d for d in nomes if d.startswith('nvme') and 'p' in d[4:]) + \
            sorted(d for d in nomes if d.startswith('nvme') and d.endswith('n1'))
    else:
        devs = sorted(d for d in nomes if len(d) == 4 and d.startswith('sd') and d[3].isdigit()) + \
            sorted(d for d in nomes if len(d) == 3 and d.startswith('sd'))
    for d in devs:
        dev = '/dev/' + d
        try:
            tipo = subprocess.run(['blkid', '-o', 'value', '-s', 'TYPE', dev], capture_output=True, text=True,
                                  timeout=10).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            continue
        opts = {'vfat': 'rw,noatime,flush,uid=0,gid=0,umask=000', 'exfat': 'rw,noatime,uid=0,gid=0,umask=000',
                'ext4': 'rw,noatime'}.get(tipo)
        if not opts:
            continue
        os.makedirs(ponto, exist_ok=True)
        if subprocess.run(['mount', '-o', opts, dev, ponto], capture_output=True).returncode == 0:
            print('armazenamento: %s (%s) montado em %s' % (dev, tipo, ponto), flush=True)
            return True
    return False


def prepara(base, sistema=False):
    """cria as pastas que faltam (nunca apaga nada)"""
    for p in (PASTAS_SISTEMA if sistema else PASTAS_MIDIA):
        os.makedirs(os.path.join(base, p), exist_ok=True)


def tamanho(pasta):
    t = 0
    for r, _, arqs in os.walk(pasta):
        for a in arqs:
            try:
                t += os.path.getsize(os.path.join(r, a))
            except OSError:
                pass
    return t


def livre(pasta):
    try:
        return shutil.disk_usage(pasta).free
    except OSError:
        return 0


def _arquivos(origem, subs):
    out = []
    for s in subs:
        for r, dirs, arqs in os.walk(os.path.join(origem, s)):
            dirs[:] = [d for d in dirs if not d.startswith('.')]
            for a in arqs:
                if not a.startswith('.'):
                    out.append(os.path.relpath(os.path.join(r, a), origem))
    return out


def copia(origem, destino, tudo, progresso=lambda *a: None, cancelado=lambda: False):
    """copia BIOS e TV (sempre) e, com tudo=True, roms/imagens/capturas. Nao sobrescreve.
    Devolve (copiados, ja_existiam, erros)."""
    prepara(destino)
    lista = _arquivos(origem, OBRIGATORIAS + (OPCIONAIS if tudo else ()))
    total = sum(os.path.getsize(os.path.join(origem, r)) for r in lista if os.path.exists(os.path.join(origem, r))) or 1
    feito = cop = exist = err = 0
    for rel in lista:
        if cancelado():
            break
        a, b = os.path.join(origem, rel), os.path.join(destino, rel)
        tam = os.path.getsize(a) if os.path.exists(a) else 0
        progresso(feito * 100 // total, rel)
        if os.path.exists(b) and os.path.getsize(b) == tam:
            exist += 1
        else:
            try:
                os.makedirs(os.path.dirname(b), exist_ok=True)
                shutil.copyfile(a, b + '.copiando')
                os.replace(b + '.copiando', b)
                cop += 1
            except OSError as ex:
                err += 1
                print('armazenamento: nao copiou %s: %s' % (rel, ex), flush=True)
        feito += tam
    subprocess.run(['sync'])
    progresso(100, '')
    return cop, exist, err


def migra_sistema(de_base, sis):
    """1a vez no R29: config/saves/states que estavam no pendrive (versoes antigas) vem para o
    cartao. Arquivo que ja existe no cartao fica. Devolve quantos copiou."""
    n = 0
    if not de_base or os.path.abspath(de_base) == os.path.abspath(sis):
        return 0
    for sub in ('saves', 'states', 'config'):
        r0 = os.path.join(de_base, sub)
        for r, _, arqs in os.walk(r0):
            dest = os.path.join(sis, sub, os.path.relpath(r, r0))
            os.makedirs(dest, exist_ok=True)
            for a in arqs:
                if a == 'retrovolt.cfg':
                    continue
                d = os.path.join(dest, a)
                if not os.path.exists(d):
                    try:
                        shutil.copy2(os.path.join(r, a), d)
                        n += 1
                    except OSError:
                        pass
    return n
