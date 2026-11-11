"""RETROVOLT - YouTube pelo yt-dlp (busca) e mpv (video, com o ytdl_hook dele).

Sem conta e sem chave: o yt-dlp le o site como um navegador. O YouTube muda de vez em
quando e o yt-dlp para de funcionar ate ser atualizado (APPS > YOUTUBE > ATUALIZAR YT-DLP;
a versao instalada pelo instalar.sh e o binario oficial, que se atualiza sozinho com -U).
"""
import json
import os
import shutil
import subprocess

# binario oficial (atualiza com -U); o do apt costuma estar velho demais para o YouTube
YTDLP_LOCAL = '/usr/local/bin/yt-dlp'


def ytdlp():
    """caminho do yt-dlp ou None"""
    if os.path.exists(YTDLP_LOCAL):
        return YTDLP_LOCAL
    return shutil.which('yt-dlp')


def duracao(seg):
    try:
        seg = int(seg)
    except (TypeError, ValueError):
        return ''
    h, m, s = seg // 3600, seg // 60 % 60, seg % 60
    return '%d:%02d:%02d' % (h, m, s) if h else '%d:%02d' % (m, s)


def busca(termo, n=30, timeout=40):
    """[(id, titulo, canal, duracao)] ou levanta RuntimeError com a mensagem"""
    exe = ytdlp()
    if not exe:
        raise RuntimeError('FALTA O YT-DLP (RODE O INSTALAR.SH)')
    try:
        r = subprocess.run([exe, '--flat-playlist', '--dump-single-json', '--no-warnings',
                            '--ignore-config', 'ytsearch%d:%s' % (n, termo)],
                           capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise RuntimeError('A BUSCA DEMOROU DEMAIS (INTERNET?)')
    if r.returncode:
        err = (r.stderr or '').strip().splitlines()
        print('youtube: busca falhou: %s' % (err[-1][:200] if err else r.returncode), flush=True)
        raise RuntimeError('A BUSCA FALHOU (INTERNET? ATUALIZE O YT-DLP)')
    try:
        d = json.loads(r.stdout)
    except ValueError:
        raise RuntimeError('RESPOSTA ESTRANHA DO YOUTUBE')
    out = []
    for e in d.get('entries') or []:
        vid = e.get('id')
        if not vid or e.get('ie_key', 'Youtube') not in ('Youtube', None):
            continue
        if e.get('live_status') == 'is_upcoming':
            continue
        out.append((vid, e.get('title') or vid, e.get('channel') or e.get('uploader') or '',
                    'AO VIVO' if e.get('live_status') == 'is_live' else duracao(e.get('duration'))))
    return out


def url(vid):
    return 'https://www.youtube.com/watch?v=' + vid


def formato(tubo):
    """H.264 (o Pi decodifica rapido) ate 1080p no LCD e 480p no tubo; se nao tiver, o melhor
    ate essa altura"""
    alt = 480 if tubo else 1080
    return ('bv*[height<=%d][vcodec^=avc1]+ba/b[height<=%d][vcodec^=avc1]/bv*[height<=%d]+ba/b[height<=%d]/b'
            % (alt, alt, alt, alt))


def atualiza(timeout=120):
    """(ok, mensagem)"""
    exe = ytdlp()
    if not exe:
        return False, 'FALTA O YT-DLP (RODE O INSTALAR.SH)'
    try:
        r = subprocess.run([exe, '-U'], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, 'DEMOROU DEMAIS'
    txt = ((r.stdout or '') + (r.stderr or '')).strip().splitlines()
    ult = txt[-1] if txt else ''
    print('youtube: yt-dlp -U: %s' % ult[:200], flush=True)
    if r.returncode:
        return False, 'NAO ATUALIZOU (INTERNET?)'
    return True, 'YT-DLP EM DIA' if 'up to date' in ult.lower() else 'YT-DLP ATUALIZADO'


# capas na RAM (/run e tmpfs): nada no cartao/pendrive; apagadas ao sair do YouTube e no
# desligar. Teto de 24 MB: passou, apaga as mais antigas (dia inteiro no YouTube nao enche)
CAPAS = '/run/retrovolt/yt'
CAPAS_MAX = 24 * 1048576


def limpa_capas(tudo=False):
    try:
        arqs = [os.path.join(CAPAS, n) for n in os.listdir(CAPAS)]
    except OSError:
        return
    if tudo:
        for a in arqs:
            try:
                os.remove(a)
            except OSError:
                pass
        return
    info = []
    for a in arqs:
        try:
            st = os.stat(a)
            info.append((st.st_mtime, st.st_size, a))
        except OSError:
            pass
    total = sum(t for _, t, _ in info)
    for _, t, a in sorted(info):          # mais antigas primeiro
        if total <= CAPAS_MAX * 3 // 4:   # desce ate 75% do teto
            break
        try:
            os.remove(a)
            total -= t
        except OSError:
            pass


def _tamanho_capas():
    try:
        return sum(os.path.getsize(os.path.join(CAPAS, n)) for n in os.listdir(CAPAS))
    except OSError:
        return 0


def capa(vid, w, h, exe, cache_dir=CAPAS, timeout=15):
    """capa do video (hqdefault 480x360, 4:3) reduzida para w x h pelo conversor do rv-jogo:
    (w, h, bytes RGB) ou None. Fica em /run (RAM), com teto."""
    import urllib.request
    cache_dir = CAPAS
    os.makedirs(cache_dir, exist_ok=True)
    if _tamanho_capas() > CAPAS_MAX:
        limpa_capas()
    jpg = os.path.join(cache_dir, 'yt_%s.jpg' % vid)
    rgb = os.path.join(cache_dir, 'yt_%s_%dx%d.rgb' % (vid, w, h))
    if not os.path.exists(rgb):
        if not os.path.exists(jpg):
            try:
                req = urllib.request.Request('https://i.ytimg.com/vi/%s/hqdefault.jpg' % vid,
                                             headers={'User-Agent': 'Mozilla/5.0'})
                d = urllib.request.urlopen(req, timeout=timeout).read()
            except Exception:
                return None
            with open(jpg + '.tmp', 'wb') as f:
                f.write(d)
            os.replace(jpg + '.tmp', jpg)
        try:
            r = subprocess.run([exe, '--fundo', jpg, rgb, str(w), str(h)], capture_output=True, timeout=20)
        except (OSError, subprocess.TimeoutExpired):
            return None
        if r.returncode:
            return None
    try:
        d = open(rgb, 'rb').read()
    except OSError:
        return None
    ww, hh = d[0] | d[1] << 8, d[2] | d[3] << 8
    if len(d) != 4 + ww * hh * 3:
        return None
    return ww, hh, d[4:]
