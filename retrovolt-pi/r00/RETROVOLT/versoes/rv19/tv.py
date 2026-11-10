"""RETROVOLT Pi - RetroTV: listas de canais e reproducao.

Listas em RETROVOLT/tv/ (uma ou varias): .m3u / .m3u8 (IPTV, com #EXTINF, group-title,
tvg-logo), .pls (Winamp/Shoutcast), .xspf (VLC), .txt (uma URL por linha, ou
"nome, url"). Cada arquivo vira uma pasta na aba TV, como os sistemas na aba JOGOS.

Previa (5 s, na caixa do menu): o ffmpeg decodifica e entrega quadros pequenos ja no
formato do framebuffer; o menu copia direto para a caixa. Som da previa pelo HDMI.
Tela cheia: mpv (--vo=drm) com a resolucao da saida; no tubo a imagem e esticada para a
tela larga de 240 linhas (pontos 6x mais largos). Sem mpv: ffmpeg direto no framebuffer.
Precisa no Pi:  sudo apt install -y --no-install-recommends ffmpeg mpv
"""
import json
import os
import re
import shutil
import socket
import subprocess
import threading
import time
from html import unescape

EXT_LISTA = ('.m3u', '.m3u8', '.pls', '.xspf', '.txt')
SOCK = '/run/retrovolt/mpv.sock'


# ------------------------------------------------------------------ listas
def _m3u(txt):
    out, info = [], None
    for l in txt.splitlines():
        l = l.strip().lstrip('﻿')
        if not l:
            continue
        if l.upper().startswith('#EXTINF'):
            atr = dict(re.findall(r'([\w-]+)="([^"]*)"', l))
            nome = l.rsplit(',', 1)[1].strip() if ',' in l else ''
            info = {'nome': nome or atr.get('tvg-name', ''), 'grupo': atr.get('group-title', ''),
                    'logo': atr.get('tvg-logo', '')}
            continue
        if l.startswith('#'):
            continue
        c = info or {'nome': '', 'grupo': '', 'logo': ''}
        c['url'] = l
        c['nome'] = c['nome'] or os.path.basename(l.split('?')[0]) or l
        out.append(c)
        info = None
    return out


def _pls(txt):
    f, t = {}, {}
    for l in txt.splitlines():
        m = re.match(r'\s*(File|Title)(\d+)\s*=\s*(.*)', l, re.I)
        if m:
            (f if m.group(1).lower() == 'file' else t)[int(m.group(2))] = m.group(3).strip()
    return [{'nome': t.get(k) or f[k], 'url': f[k], 'grupo': '', 'logo': ''} for k in sorted(f)]


def _xspf(txt):
    out = []
    for bloco in re.findall(r'<track>(.*?)</track>', txt, re.S | re.I):
        loc = re.search(r'<location>(.*?)</location>', bloco, re.S | re.I)
        tit = re.search(r'<title>(.*?)</title>', bloco, re.S | re.I)
        if loc:
            url = unescape(loc.group(1).strip())
            out.append({'nome': unescape(tit.group(1).strip()) if tit else url, 'url': url, 'grupo': '', 'logo': ''})
    return out


def _txt(txt):
    out = []
    for l in txt.splitlines():
        l = l.strip()
        if not l or l.startswith('#'):
            continue
        if ',' in l and '://' in l.rsplit(',', 1)[1]:
            nome, url = (x.strip() for x in l.rsplit(',', 1))
        else:
            nome, url = os.path.basename(l.split('?')[0]) or l, l
        out.append({'nome': nome, 'url': url, 'grupo': '', 'logo': ''})
    return out


def _e_lista_de_canais(txt):
    """lista IPTV (varios canais com #EXTINF) e nao um canal HLS (#EXT-X-...)"""
    return '#EXTINF' in txt and '#EXT-X-' not in txt


def baixa_lista(url, pasta_cache, max_idade=6 * 3600):
    """baixa uma lista de canais da internet; guarda uma copia (usa a copia se a internet
    falhar ou se for recente). Devolve o texto ou None."""
    import hashlib
    import urllib.request
    os.makedirs(pasta_cache, exist_ok=True)
    arq = os.path.join(pasta_cache, hashlib.md5(url.encode()).hexdigest() + '.m3u')
    try:
        if time.time() - os.path.getmtime(arq) < max_idade:
            return open(arq, encoding='utf-8', errors='replace').read()
    except OSError:
        pass
    try:
        req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0 (RetroVolt)'})
        with urllib.request.urlopen(req, timeout=10) as r:
            dados = r.read(8 * 1024 * 1024)
        txt = dados.decode('utf-8', 'replace')
        if _e_lista_de_canais(txt):
            with open(arq + '.tmp', 'w', encoding='utf-8') as f:
                f.write(txt)
            os.replace(arq + '.tmp', arq)
        return txt
    except Exception as ex:
        print('tv: nao baixou %s (%s); usando a copia guardada' % (url[:80], ex), flush=True)
        try:
            return open(arq, encoding='utf-8', errors='replace').read()
        except OSError:
            return None


def _parece_lista(url):
    caminho = url.split('?')[0].lower()
    return caminho.endswith('.m3u') or caminho.endswith('.m3u8') or caminho.endswith('.pls')


def le_lista(path):
    try:
        txt = open(path, encoding='utf-8', errors='replace').read()
    except OSError:
        return []
    ext = os.path.splitext(path)[1].lower()
    if ext == '.pls' or txt.lstrip().lower().startswith('[playlist]'):
        canais = _pls(txt)
    elif ext == '.xspf' or '<playlist' in txt[:400].lower():
        canais = _xspf(txt)
    elif ext in ('.m3u', '.m3u8') or txt.lstrip().upper().startswith('#EXTM3U'):
        canais = _m3u(txt)
    else:
        canais = _txt(txt)
    # link de LISTA dentro do arquivo (ex.: .txt com o endereco de um .m3u de IPTV):
    # baixa e troca pelo conteudo. Canal HLS (.m3u8 de um canal so) fica como esta.
    pasta = os.path.dirname(path)
    expandidos = []
    for c in canais:
        if '://' in c['url'] and _parece_lista(c['url']) and ext == '.txt' or c.get('lista'):
            txt2 = baixa_lista(c['url'], os.path.join(pasta, '.cache'))
            if txt2 and _e_lista_de_canais(txt2):
                novos = _m3u(txt2)
                print('tv: %s -> %d canais' % (c['url'][:80], len(novos)), flush=True)
                expandidos += novos
                continue
            if txt2 and txt2.lstrip().lower().startswith('[playlist]'):
                expandidos += _pls(txt2)
                continue
        expandidos.append(c)
    canais = expandidos
    # caminho relativo (video no pendrive): relativo a pasta da lista
    for c in canais:
        if '://' not in c['url'] and not c['url'].startswith('/'):
            c['url'] = os.path.join(pasta, c['url'])
    return canais


def listas(base):
    """[(nome, caminho, canais)] dos arquivos de RETROVOLT/tv"""
    pasta = os.path.join(base, 'tv')
    try:
        nomes = sorted(os.listdir(pasta))
    except OSError:
        return []
    out = []
    for n in nomes:
        p = os.path.join(pasta, n)
        if n.startswith('.') or not os.path.isfile(p) or os.path.splitext(n)[1].lower() not in EXT_LISTA:
            continue
        canais = le_lista(p)
        if canais:
            out.append((os.path.splitext(n)[0], p, canais))
    return out


def tem_programas():
    return {'ffmpeg': bool(shutil.which('ffmpeg')), 'mpv': bool(shutil.which('mpv'))}


# ------------------------------------------------------------------ previa (ffmpeg -> caixa)
class Previa(threading.Thread):
    """decodifica o canal em quadros w x h no formato do framebuffer e chama
    desenha(bytes) a cada quadro. Som pelo ALSA (audio_dev)."""

    def __init__(self, url, w, h, bpp, desenha, audio_dev=None, volume=80, aw=None, ah=None):
        """w x h: pontos da caixa no framebuffer; aw x ah: a mesma caixa em pontos
        quadrados (no tubo os pontos sao 6x mais largos: encaixa em aw x ah e estica)"""
        super().__init__(daemon=True)
        self.url, self.w, self.h, self.bpp = url, w, h, bpp
        self.aw, self.ah = aw or w, ah or h
        self.desenha = desenha
        self.audio_dev, self.volume = audio_dev, volume
        self.proc = None
        self.parar = threading.Event()
        self.quadros = 0
        self.erro = None

    def run(self):
        fmt = 'rgb565le' if self.bpp == 16 else 'bgr0'
        vf = ('scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2,'
              'scale=%d:%d,fps=15' % (self.aw, self.ah, self.aw, self.ah, self.w, self.h))
        cmd = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-nostdin']
        if '://' in self.url:
            cmd += ['-rw_timeout', '10000000']  # canal fora do ar: desiste em 10 s
        else:
            cmd += ['-re']                      # video no pendrive: no ritmo real
        cmd += ['-i', self.url, '-map', '0:v:0', '-vf', vf, '-f', 'rawvideo', '-pix_fmt', fmt, 'pipe:1']
        if self.audio_dev:
            cmd += ['-map', '0:a:0?', '-af', 'volume=%.2f' % (self.volume / 100.0), '-ac', '2',
                    '-f', 'alsa', self.audio_dev]
        tam = self.w * self.h * (2 if self.bpp == 16 else 4)
        try:
            self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        except OSError as ex:
            self.erro = 'ffmpeg: %s' % ex
            return
        while not self.parar.is_set():
            q = self.proc.stdout.read(tam)
            if not q or len(q) < tam:
                break
            self.quadros += 1
            self.desenha(q)
        if not self.parar.is_set() and self.quadros == 0:
            try:
                self.erro = (self.proc.stderr.read(400) or b'').decode('utf-8', 'replace').strip() or 'sem imagem'
            except Exception:
                self.erro = 'sem imagem'
        self.fecha()

    def fecha(self):
        self.parar.set()
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(2)
            except subprocess.TimeoutExpired:
                self.proc.kill()


# ------------------------------------------------------------------ tela cheia
class TelaCheia:
    """mpv na saida inteira (troca de canal sem fechar, pelo socket de comando)"""

    def __init__(self, tubo, fb_w, fb_h, bpp, audio_dev=None, volume=80):
        self.tubo, self.audio_dev, self.volume = tubo, audio_dev, volume
        self.fb_w, self.fb_h, self.bpp = fb_w, fb_h, bpp
        self.proc = None
        self.com_mpv = bool(shutil.which('mpv'))

    def abre(self, url, nome=''):
        self.fecha()
        if self.com_mpv:
            try:
                os.remove(SOCK)
            except OSError:
                pass
            os.makedirs(os.path.dirname(SOCK), exist_ok=True)
            cmd = ['mpv', '--vo=drm', '--hwdec=auto-safe', '--fs', '--really-quiet', '--no-input-terminal',
                   '--no-osc', '--input-ipc-server=' + SOCK, '--volume=%d' % self.volume,
                   '--osd-level=1', '--osd-duration=3000', '--keep-open=no', '--cache=yes',
                   '--demuxer-max-bytes=32MiB', '--force-media-title=' + (nome or url)]
            if self.tubo:
                # tela larga de 240 linhas (pontos 6x mais largos): a imagem sai na proporcao
                # certa e com as linhas do tubo (abaixo de 480i)
                cmd += ['--monitorpixelaspect=%.5f' % ((4.0 / 3.0) / (self.fb_w / float(self.fb_h)))]
            if self.audio_dev:
                cmd += ['--audio-device=alsa/' + self.audio_dev]
            cmd += [url]
        else:
            # sem mpv: ffmpeg direto no framebuffer (tela inteira; no tubo, esticada)
            if self.tubo:
                vf = 'scale=%d:%d' % (self.fb_w, self.fb_h)
            else:
                vf = ('scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2'
                      % (self.fb_w, self.fb_h, self.fb_w, self.fb_h))
            cmd = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-nostdin']
            if '://' not in url:
                cmd += ['-re']
            cmd += ['-i', url, '-map', '0:v:0', '-vf', vf,
                    '-pix_fmt', 'rgb565le' if self.bpp == 16 else 'bgra', '-f', 'fbdev', '/dev/fb0']
            if self.audio_dev:
                cmd += ['-map', '0:a:0?', '-ac', '2', '-f', 'alsa', self.audio_dev]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        return self.proc

    def troca(self, url, nome=''):
        """troca de canal sem fechar o mpv (mais rapido); sem mpv, reabre"""
        if self.com_mpv and self.rodando():
            if self._comando(['loadfile', url, 'replace']):
                self._comando(['set_property', 'force-media-title', nome or url])
                self._comando(['show-text', nome or url, 3000])
                return
        self.abre(url, nome)

    def _comando(self, cmd):
        try:
            s = socket.socket(socket.AF_UNIX)
            s.settimeout(1)
            s.connect(SOCK)
            s.sendall((json.dumps({'command': cmd}) + '\n').encode())
            s.close()
            return True
        except OSError:
            return False

    def rodando(self):
        return self.proc is not None and self.proc.poll() is None

    def erro(self):
        try:
            return (self.proc.stderr.read(300) or b'').decode('utf-8', 'replace').strip()
        except Exception:
            return ''

    def fecha(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None
