#!/usr/bin/env python3
"""RETROVOLT Pi - guarda de video do boot (retrovolt-video.service, antes do menu).

Nunca ficar sem imagem: le a tela de verdade (EDID pelo DDC/HDMI) e, se o boot estiver no
modo errado (sinal de tubo num LCD, ou de LCD no tubo), corrige o cmdline e reinicia na hora.
Independe do pendrive e da versao do menu: fica em /usr/local/lib/retrovolt/ (o menu instala
e atualiza sozinho). Respeita OPCOES > VIDEO (AUTO; TUBO/LCD so na tela em que foi escolhido).
Registro em /var/lib/retrovolt/logs/video_guarda.txt (o menu copia para o LOG.TXT).
"""
import os
import subprocess
import sys
import time

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, AQUI)
REG = '/var/lib/retrovolt/logs/video_guarda.txt'
CFG = '/var/lib/retrovolt/config/retrovolt.cfg'


def log(msg):
    linha = '%s guarda de video: %s' % (time.strftime('%Y-%m-%d %H:%M:%S'), msg)
    print(linha, flush=True)
    try:
        os.makedirs(os.path.dirname(REG), exist_ok=True)
        antigas = open(REG).read().splitlines()[-200:] if os.path.exists(REG) else []
        with open(REG, 'w') as f:
            f.write('\n'.join(antigas + [linha]) + '\n')
    except OSError:
        pass


def le_cfg():
    c = {}
    try:
        for l in open(CFG, encoding='utf-8', errors='replace'):
            l = l.split('#', 1)[0].strip()
            if '=' in l:
                k, v = l.split('=', 1)
                c[k.strip()] = v.strip().strip('"')
    except OSError:
        pass
    return c


def main():
    import video
    c = le_cfg()
    if os.path.exists(video.TESTE):
        log('resolucao nova esperando confirmacao: o menu cuida (volta sozinha em 30 s)')
        return
    forcado = c.get('video_saida', 'AUTO').upper()
    modo = None
    v = c.get('video_modo', 'AUTO').upper()
    if 'X' in v:
        try:
            modo = tuple(int(x) for x in v.split('X'))
        except ValueError:
            modo = None
    try:
        saida, modo, motivo = video.decide(forcado, modo, c.get('video_saida_tela') or None)
    except Exception as ex:
        log('nao leu a tela (%s): fica como esta' % ex)
        return
    agora = video.modo_atual()
    if not saida:
        log('tela nao identificada (%s): fica em %s' % (motivo, agora))
        return
    if not video.precisa_trocar(saida, modo):
        log('tela %s, boot em %s: certo (%s)' % (video.ULTIMA_TELA, agora, motivo))
        return
    ok, _ = video.pode_trocar()
    if not ok:
        log('tela pede %s, boot em %s, mas ja trocou 2 vezes em 10 min: espera (SELECT 5 s troca)'
            % (saida.upper(), agora))
        return
    if video.troca(saida, modo):
        log('tela %s pede %s e o boot estava em %s: corrigido, reiniciando' % (video.ULTIMA_TELA, saida.upper(), agora))
        os.sync()
        subprocess.run(['systemctl', 'reboot'])


if __name__ == '__main__':
    try:
        main()
    except Exception as ex:      # a guarda nunca pode impedir o menu de subir
        log('erro: %s' % ex)
