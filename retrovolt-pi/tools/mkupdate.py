#!/usr/bin/env python3
"""gera RETROVOLT/update/rvup_Vxx.bin (OPCOES > ATUALIZAR, R29).

uso: python3 -B tools/mkupdate.py <numero> <pasta da versao pronta> <saida.bin> [nota ...]
  <pasta da versao pronta> = a pasta rvNN COMPLETA (rv_app.py, rv-jogo, nucleos/, dados/,
  nomes_fbneo.json, fbneo/...), como vai para /opt/retrovolt/versoes/rvXX.
Tambem leva o lancador e o servico de r00/RETROVOLT/sistema (sempre: sao pequenos e assim o
cartao fica igual ao repositorio). Zip normal (deflate), cada arquivo com SHA-256 no
manifest.json; o versao.txt e escrito pelo atualizador no Pi.
"""
import hashlib
import json
import os
import sys
import time
import zipfile

AQUI = os.path.dirname(os.path.abspath(__file__))
SISTEMA = os.path.join(AQUI, '..', 'r00', 'RETROVOLT', 'sistema')
IGNORA = ('__pycache__', '.cache')


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def main():
    num, pasta, saida = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    notas = sys.argv[4:]
    for obrig in ('rv_app.py', 'rv-jogo', 'nucleos'):
        if not os.path.exists(os.path.join(pasta, obrig)):
            sys.exit('a pasta da versao nao tem %s: pacote incompleto' % obrig)
    entradas = []
    for raiz, dirs, arqs in os.walk(pasta):
        dirs[:] = sorted(d for d in dirs if d not in IGNORA)
        for a in sorted(arqs):
            if a.endswith('.pyc') or a == 'versao.txt':
                continue
            p = os.path.join(raiz, a)
            entradas.append(('versao/' + os.path.relpath(p, pasta).replace(os.sep, '/'), p))
    for a in ('rv-lancador', 'retrovolt.service'):
        entradas.append(('sistema/' + a, os.path.join(SISTEMA, a)))
    man = {'formato': 1, 'versao': num, 'nome': 'R%d' % num, 'data': time.strftime('%d/%m/%Y'),
           'notas': notas, 'arquivos': {}, 'tamanho': 0}
    for nome, p in entradas:
        man['arquivos'][nome] = sha(p)
        man['tamanho'] += os.path.getsize(p)
    os.makedirs(os.path.dirname(os.path.abspath(saida)), exist_ok=True)
    with zipfile.ZipFile(saida + '.tmp', 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('manifest.json', json.dumps(man, indent=1, ensure_ascii=True))
        for nome, p in entradas:
            z.write(p, nome)
    os.replace(saida + '.tmp', saida)
    print('%s: R%d, %d arquivos, %d MB (instalado %d MB)' % (saida, num, len(entradas),
          os.path.getsize(saida) // 1048576, man['tamanho'] // 1048576))


if __name__ == '__main__':
    main()
