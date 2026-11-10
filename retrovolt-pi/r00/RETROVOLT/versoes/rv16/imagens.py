"""RETROVOLT Pi - imagens do menu: arte do disco (CD) e capa do jogo.

Le PNG com a biblioteca padrao do Python (zlib): 8 bits, RGB, RGBA, paleta ou cinza.
Onde procurar (na pasta de jogos), so PNG:
  RETROVOLT/imagens/discos/<sistema>/<codigo>.png   arte do CD  (ex.: ps1/SLUS-00892.png)
  RETROVOLT/imagens/capas/<sistema>/<codigo>.png    capa do jogo
  ...ou <nome do arquivo do jogo>.png no lugar do codigo.
Reduzir uma imagem grande em Python demora (1 s ou mais no Pi): o menu pede numa
thread e guarda a reduzida em RETROVOLT/imagens/.cache (a segunda vez e na hora).
"""
import hashlib
import os
import struct
import zlib

_cache = {}


def le_png(path):
    """(largura, altura, lista de (r, g, b, a)) ou None"""
    try:
        d = open(path, 'rb').read()
    except OSError:
        return None
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        return None
    p, idat, pal, trns = 8, b'', None, None
    w = h = prof = tipo = entrel = 0
    while p + 8 <= len(d):
        n, t = struct.unpack('>I4s', d[p:p + 8])
        c = d[p + 8:p + 8 + n]
        if t == b'IHDR':
            w, h, prof, tipo, _, _, entrel = struct.unpack('>IIBBBBB', c)
        elif t == b'PLTE':
            pal = [tuple(c[i:i + 3]) for i in range(0, len(c), 3)]
        elif t == b'tRNS':
            trns = c
        elif t == b'IDAT':
            idat += c
        elif t == b'IEND':
            break
        p += 12 + n
    if prof != 8 or entrel or tipo not in (0, 2, 3, 4, 6) or w * h > 1024 * 1024:
        return None
    canais = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[tipo]
    try:
        raw = zlib.decompress(idat)
    except zlib.error:
        return None
    lin = w * canais
    out, ant = bytearray(), bytearray(lin)
    for y in range(h):
        f = raw[y * (lin + 1)]
        cur = bytearray(raw[y * (lin + 1) + 1:(y + 1) * (lin + 1)])
        for i in range(lin):
            a = cur[i - canais] if i >= canais else 0
            b = ant[i]
            c = ant[i - canais] if i >= canais else 0
            if f == 1:
                cur[i] = (cur[i] + a) & 255
            elif f == 2:
                cur[i] = (cur[i] + b) & 255
            elif f == 3:
                cur[i] = (cur[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                cur[i] = (cur[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += cur
        ant = cur
    px = []
    for i in range(w * h):
        v = out[i * canais:(i + 1) * canais]
        if tipo == 2:
            px.append((v[0], v[1], v[2], 255))
        elif tipo == 6:
            px.append((v[0], v[1], v[2], v[3]))
        elif tipo == 0:
            px.append((v[0], v[0], v[0], 255))
        elif tipo == 4:
            px.append((v[0], v[0], v[0], v[1]))
        else:
            r, g, b = pal[v[0]] if pal and v[0] < len(pal) else (0, 0, 0)
            a = trns[v[0]] if trns and v[0] < len(trns) else 255
            px.append((r, g, b, a))
    return w, h, px


def arte_disco(base, sistema, codigo=None, arquivo=None, tipo='discos'):
    """caminho da arte do disco (tipo='discos') ou da capa (tipo='capas'), se existir"""
    pasta = os.path.join(base, 'imagens', tipo, sistema or '')
    nomes = []
    if codigo:
        nomes += [codigo, codigo.replace('-', ''), codigo.replace('-', '_')]
    if arquivo:
        nomes.append(os.path.splitext(os.path.basename(arquivo))[0])
    for n in nomes:
        for ext in ('.png', '.PNG'):
            p = os.path.join(pasta, n + ext)
            if os.path.exists(p):
                return p
    return None


def disco_redondo(path, diametro):
    """arte reduzida para um circulo (cache): lista de (dx, dy, (r, g, b)) a partir do centro"""
    chave = (path, diametro)
    if chave in _cache:
        return _cache[chave]
    img = le_png(path)
    pts = None
    if img:
        w, h, px = img
        r = diametro / 2.0
        pts = []
        for y in range(diametro):
            for x in range(diametro):
                dx, dy = x - r + 0.5, y - r + 0.5
                if dx * dx + dy * dy > r * r:
                    continue
                cr, cg, cb, ca = px[(y * h // diametro) * w + x * w // diametro]
                if ca < 128:
                    continue
                pts.append((x - diametro // 2, y - diametro // 2, (cr, cg, cb)))
    if len(_cache) > 40:
        _cache.clear()
    _cache[chave] = pts
    return pts


def reduz(path, bw, bh, cache_dir=None):
    """imagem reduzida (media dos pontos) cabendo em bw x bh, mantendo a proporcao:
    (w, h, bytes RGB) ou None. Usa/grava o cache em disco."""
    try:
        st = os.stat(path)
    except OSError:
        return None
    nome = hashlib.md5(('%s|%d|%d' % (path, st.st_size, int(st.st_mtime))).encode()).hexdigest()
    arq = os.path.join(cache_dir, '%s_%dx%d.rgb' % (nome, bw, bh)) if cache_dir else None
    if arq and os.path.exists(arq):
        try:
            d = open(arq, 'rb').read()
            w, h = d[0] | d[1] << 8, d[2] | d[3] << 8
            if len(d) == 4 + w * h * 3:
                return w, h, d[4:]
        except OSError:
            pass
    img = le_png(path)
    if not img:
        return None
    sw, sh, px = img
    esc = min(bw / sw, bh / sh)
    w, h = max(1, int(sw * esc)), max(1, int(sh * esc))
    out = bytearray(w * h * 3)
    for y in range(h):
        y0, y1 = y * sh // h, max(y * sh // h + 1, (y + 1) * sh // h)
        for x in range(w):
            x0, x1 = x * sw // w, max(x * sw // w + 1, (x + 1) * sw // w)
            r = g = b = n = 0
            for yy in range(y0, y1, max(1, (y1 - y0) // 3)):
                base_i = yy * sw
                for xx in range(x0, x1, max(1, (x1 - x0) // 3)):
                    pr, pg, pb, pa = px[base_i + xx]
                    if pa < 128:            # transparente vira o fundo do menu
                        pr, pg, pb = 12, 18, 40
                    r += pr
                    g += pg
                    b += pb
                    n += 1
            o = (y * w + x) * 3
            out[o], out[o + 1], out[o + 2] = r // n, g // n, b // n
    if arq:
        try:
            os.makedirs(cache_dir, exist_ok=True)
            with open(arq, 'wb') as f:
                f.write(bytes((w & 255, w >> 8, h & 255, h >> 8)) + bytes(out))
        except OSError:
            pass
    return w, h, bytes(out)


# ------------------------------------------------------------------ fundo do menu
EXT_FUNDO = ('.png', '.jpg', '.jpeg')
ULTIMO_FUNDO = '/var/lib/retrovolt/fundo_ultimo'


def fundos(base):
    """imagens da pasta RETROVOLT/imagens/wallpapers (qualquer nome .png/.jpg;
    wallpaper_1 ... wallpaper_10 e o jeito sugerido)"""
    pasta = os.path.join(base, 'imagens', 'wallpapers')
    try:
        nomes = sorted(os.listdir(pasta))
    except OSError:
        return []
    return [os.path.join(pasta, n) for n in nomes
            if not n.startswith('.') and os.path.splitext(n)[1].lower() in EXT_FUNDO]


def fundo_menu(base, w, h, exe, cache_dir, brilho=45):
    """sorteia um fundo (sem repetir o da ultima vez quando ha mais de um), reduzido
    para a tela logica w x h e escurecido (brilho em %, para o texto aparecer).
    Devolve (lista de (r, g, b) com w*h pontos, caminho) ou (None, None)."""
    import random
    import subprocess
    lista = fundos(base)
    if not lista:
        return None, None
    try:
        ultimo = open(ULTIMO_FUNDO).read().strip()
    except OSError:
        ultimo = ''
    opcoes = [p for p in lista if os.path.basename(p) != ultimo] or lista
    random.shuffle(opcoes)
    for path in opcoes:           # se uma imagem estiver ruim, tenta a proxima
        try:
            st = os.stat(path)
        except OSError:
            continue
        nome = hashlib.md5(('%s|%d|%d' % (path, st.st_size, int(st.st_mtime))).encode()).hexdigest()
        arq = os.path.join(cache_dir, 'fundo_%s_%dx%d.rgb' % (nome, w, h))
        if not os.path.exists(arq):
            try:
                os.makedirs(cache_dir, exist_ok=True)
                r = subprocess.run([exe, '--fundo', path, arq, str(w), str(h)],
                                   capture_output=True, text=True, timeout=30)
                if r.returncode:
                    print('fundo: %s nao converteu (%s)' % (path, r.stderr.strip()), flush=True)
                    continue
            except (OSError, subprocess.TimeoutExpired) as ex:
                print('fundo: conversor falhou:', ex, flush=True)
                return None, None
        try:
            d = open(arq, 'rb').read()
        except OSError:
            continue
        if len(d) != 4 + w * h * 3 or (d[0] | d[1] << 8) != w or (d[2] | d[3] << 8) != h:
            continue
        k = max(0, min(100, brilho)) / 100.0
        tab = bytes(int(v * k) for v in range(256))
        rgb = d[4:].translate(tab)
        px = list(zip(rgb[0::3], rgb[1::3], rgb[2::3]))
        try:
            os.makedirs(os.path.dirname(ULTIMO_FUNDO), exist_ok=True)
            with open(ULTIMO_FUNDO, 'w') as f:
                f.write(os.path.basename(path))
        except OSError:
            pass
        return px, path
    return None, None
