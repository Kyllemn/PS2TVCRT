"""RETROVOLT - OPCOES > ATUALIZAR (R29).

Arquivo de atualizacao: RETROVOLT/update/rvup_Vxx.bin no pendrive (feito por
tools/mkupdate.py). E um .zip com:
  manifest.json  {"formato": 1, "versao": 29, "nome": "R29", "data": "...", "notas": [...],
                  "arquivos": {"versao/rv_app.py": "<sha256>", ...}, "tamanho": bytes}
  versao/...     o RetroVolt inteiro dessa versao (vai para /opt/retrovolt/versoes/rvXX)
  sistema/...    opcional: rv-lancador e retrovolt.service

Ordem ao aplicar (nada da versao em uso e apagado):
  1. confere o manifesto e o SHA-256 de cada arquivo (arquivo corrompido = nada muda)
  2. confere o espaco livre no cartao
  3. extrai em /opt/retrovolt/versoes/.rvXX.novo e so no fim troca o nome para rvXX
  4. lancador/servico: grava .novo e troca, guardando o anterior (.anterior)
  5. tira rvXX da lista de versoes ruins, apaga versoes antigas (fica a nova + 2 anteriores)
  6. sync. Quem chama reinicia o Pi.
Se a nova nao chegar na tela inicial 2 vezes, o lancador volta sozinho para a anterior.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import zipfile

AQUI = os.path.dirname(os.path.abspath(__file__))
CARTAO = '/opt/retrovolt/versoes'
DADOS = '/var/lib/retrovolt'
SISTEMA_DESTINO = {
    'sistema/rv-lancador': ('/usr/local/bin/rv-lancador', 0o755),
    'sistema/retrovolt.service': ('/etc/systemd/system/retrovolt.service', 0o644),
}
MANTER = 3                       # versoes no cartao: a nova + 2 anteriores


class ErroAtualizacao(Exception):
    pass


def versao_atual():
    """(numero, nome) da versao rodando agora (versao.txt da pasta; sem ele, R28)"""
    try:
        with open(os.path.join(AQUI, 'versao.txt')) as f:
            n, nome = f.read().split()[:2]
            return int(n), nome
    except (OSError, ValueError):
        return 28, 'R28'


def procura(usb):
    """caminho do rvup_Vxx.bin de numero mais alto em <usb>/RETROVOLT/update, ou None"""
    pasta = os.path.join(usb, 'RETROVOLT', 'update')
    melhores = []
    try:
        for n in os.listdir(pasta):
            m = re.match(r'^rvup_v(\d+)\.bin$', n, re.I)
            if m:
                melhores.append((int(m.group(1)), os.path.join(pasta, n)))
    except OSError:
        return None
    return max(melhores)[1] if melhores else None


def le_manifesto(arq):
    try:
        with zipfile.ZipFile(arq) as z:
            m = json.loads(z.read('manifest.json').decode('utf-8'))
    except (OSError, KeyError, ValueError, zipfile.BadZipFile) as ex:
        raise ErroAtualizacao('ARQUIVO INVALIDO (%s)' % type(ex).__name__)
    if m.get('formato') != 1 or not isinstance(m.get('versao'), int) or not m.get('arquivos'):
        raise ErroAtualizacao('ARQUIVO DE OUTRO FORMATO')
    return m


def aplica(arq, progresso=lambda pct, txt: None):
    """instala o rvup (ver o comeco do arquivo). Devolve a pasta nova."""
    m = le_manifesto(arq)
    pasta_nova = os.path.join(CARTAO, 'rv%d' % m['versao'])
    tmp = os.path.join(CARTAO, '.rv%d.novo' % m['versao'])
    with zipfile.ZipFile(arq) as z:
        nomes = set(z.namelist())
        arqs = m['arquivos']
        # 1. integridade: todos os arquivos do manifesto existem e batem o SHA-256
        total = sum(z.getinfo(n).file_size for n in arqs if n in nomes) or 1
        lido = 0
        for n, sha in arqs.items():
            if n not in nomes:
                raise ErroAtualizacao('FALTA NO PACOTE: %s' % n)
            h = hashlib.sha256()
            with z.open(n) as f:
                for bloco in iter(lambda: f.read(1 << 20), b''):
                    h.update(bloco)
                    lido += len(bloco)
                    progresso(40 * lido // total, 'CONFERINDO O ARQUIVO')
            if h.hexdigest() != sha:
                raise ErroAtualizacao('ARQUIVO CORROMPIDO: %s' % os.path.basename(n))
        # 2. espaco
        os.makedirs(CARTAO, exist_ok=True)
        livre = shutil.disk_usage(CARTAO).free
        if livre < total * 1.2 + 50 * 1048576:
            raise ErroAtualizacao('POUCO ESPACO NO CARTAO: PRECISA %d MB, TEM %d MB'
                                  % (total * 1.2 // 1048576 + 50, livre // 1048576))
        # 3. extrai a versao ao lado (a atual continua intacta)
        shutil.rmtree(tmp, ignore_errors=True)
        feito = 0
        for n in arqs:
            if not n.startswith('versao/'):
                continue
            destino = os.path.join(tmp, n[len('versao/'):])
            os.makedirs(os.path.dirname(destino), exist_ok=True)
            with z.open(n) as src, open(destino, 'wb') as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
            feito += z.getinfo(n).file_size
            progresso(40 + 45 * feito // total, 'INSTALANDO')
        rvj = os.path.join(tmp, 'rv-jogo')
        if os.path.exists(rvj):
            os.chmod(rvj, 0o755)
        with open(os.path.join(tmp, 'versao.txt'), 'w') as f:
            f.write('%d %s\n' % (m['versao'], m.get('nome') or 'R%d' % m['versao']))
        subprocess.run(['sync'])
        if os.path.exists(pasta_nova):              # reinstalar a mesma versao
            velha = pasta_nova + '.velha'
            shutil.rmtree(velha, ignore_errors=True)
            os.rename(pasta_nova, velha)
            os.rename(tmp, pasta_nova)
            shutil.rmtree(velha, ignore_errors=True)
        else:
            os.rename(tmp, pasta_nova)
        progresso(90, 'ATUALIZANDO O SISTEMA')
        # 4. lancador / servico (so se vierem no pacote)
        for n, (destino, modo) in SISTEMA_DESTINO.items():
            if n not in arqs:
                continue
            novo = destino + '.novo'
            with z.open(n) as src, open(novo, 'wb') as dst:
                shutil.copyfileobj(src, dst)
            os.chmod(novo, modo)
            if os.path.exists(destino):
                shutil.copy2(destino, destino + '.anterior')
            os.replace(novo, destino)
        if any(n in arqs for n in SISTEMA_DESTINO):
            subprocess.run(['systemctl', 'daemon-reload'])
    # 5. a nova nao fica marcada como ruim; versoes velhas demais saem
    ruins = os.path.join(DADOS, 'ruins')
    try:
        linhas = [l for l in open(ruins).read().splitlines() if l.strip() != 'cartao:rv%d' % m['versao']]
        with open(ruins, 'w') as f:
            f.write(''.join(l + '\n' for l in linhas))
    except OSError:
        pass
    try:
        os.remove(os.path.join(DADOS, 'tentativas', 'cartao:rv%d' % m['versao']))
    except OSError:
        pass
    versoes = sorted((d for d in os.listdir(CARTAO) if re.match(r'^rv\d+$', d)), key=lambda d: int(d[2:]))
    em_uso = os.path.basename(AQUI)
    for d in versoes[:-MANTER]:
        if d != em_uso:
            shutil.rmtree(os.path.join(CARTAO, d), ignore_errors=True)
    subprocess.run(['sync'])
    progresso(100, 'PRONTO')
    return pasta_nova
