"""RETROVOLT Pi - Wi-Fi pelo NetworkManager (nmcli).

A senha fica guardada pelo proprio NetworkManager, em arquivo do sistema que so o
root le (/etc/NetworkManager/system-connections/), nunca em texto aberto no pendrive.
"""
import subprocess


def _nmcli(args, tempo=20):
    try:
        r = subprocess.run(['nmcli'] + args, capture_output=True, text=True, timeout=tempo)
        return r.returncode, r.stdout, r.stderr.strip()
    except (OSError, subprocess.TimeoutExpired) as ex:
        return -1, '', str(ex)


def _campos(linha):
    """separa uma linha do 'nmcli -t' (':' separa, '\\:' e um ':' de verdade)"""
    out, cur, i = [], '', 0
    while i < len(linha):
        ch = linha[i]
        if ch == '\\' and i + 1 < len(linha):
            cur += linha[i + 1]
            i += 2
            continue
        if ch == ':':
            out.append(cur)
            cur = ''
        else:
            cur += ch
        i += 1
    out.append(cur)
    return out


def conectada():
    """(nome da rede, IP) ou (None, None)"""
    rede = None
    _, out, _ = _nmcli(['-t', '-f', 'ACTIVE,SSID', 'dev', 'wifi'], 5)
    for l in out.splitlines():
        c = _campos(l)
        if len(c) >= 2 and c[0] == 'yes':
            rede = c[1] or None
    ip = None
    if rede:
        _, out, _ = _nmcli(['-t', '-f', 'IP4.ADDRESS', 'dev', 'show', 'wlan0'], 5)
        for l in out.splitlines():
            c = _campos(l)
            if len(c) >= 2 and c[1]:
                ip = c[1].split('/')[0]
                break
    return rede, ip


def busca():
    """lista de (nome, sinal 0-100, protegida?) sem repetir nome, do sinal mais forte"""
    _, out, _ = _nmcli(['-t', '-f', 'SSID,SIGNAL,SECURITY', 'dev', 'wifi', 'list', '--rescan', 'yes'], 25)
    redes = {}
    for l in out.splitlines():
        c = _campos(l)
        if len(c) < 3 or not c[0]:
            continue
        try:
            sinal = int(c[1])
        except ValueError:
            sinal = 0
        protegida = c[2] not in ('', '--')
        if c[0] not in redes or redes[c[0]][0] < sinal:
            redes[c[0]] = (sinal, protegida)
    return sorted(((n, s, p) for n, (s, p) in redes.items()), key=lambda r: -r[1])


def conecta(nome, senha=None):
    """(ok, mensagem)"""
    args = ['dev', 'wifi', 'connect', nome]
    if senha:
        args += ['password', senha]
    cod, out, err = _nmcli(args, 45)
    if cod == 0:
        return True, 'CONECTADO'
    texto = (err or out).strip()
    if senha:
        texto = texto.replace(senha, '***')     # a senha nunca vai para o log
    print('wifi: nmcli recusou (%d): %s' % (cod, texto[:200]), flush=True)
    msg = texto.lower()
    if 'secrets' in msg or 'password' in msg or 'auth' in msg:
        return False, 'SENHA ERRADA'
    if 'no network' in msg or 'not found' in msg:
        return False, 'REDE NAO ENCONTRADA'
    return False, 'NAO CONECTOU'


def esquece(nome):
    cod, _, _ = _nmcli(['connection', 'delete', 'id', nome], 15)
    return cod == 0
