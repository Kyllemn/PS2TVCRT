"""RETROVOLT Pi - imagens dos discos (arte do CD) para o menu.

Le PNG com a biblioteca padrao do Python (zlib): 8 bits, RGB, RGBA, paleta ou cinza.
Onde procurar a arte de um jogo (na pasta de jogos):
  RETROVOLT/imagens/discos/<sistema>/<codigo>.png   ex.: ps1/SLUS-00892.png
  RETROVOLT/imagens/discos/<sistema>/<nome do arquivo do jogo>.png
"""
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


def arte_disco(base, sistema, codigo=None, arquivo=None):
    """caminho da arte do disco, se existir"""
    pasta = os.path.join(base, 'imagens', 'discos', sistema or '')
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
