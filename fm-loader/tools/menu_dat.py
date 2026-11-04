"""Gera MENU.DAT (formato 'MNU2') para o loader a partir de um config JSON com N jogos (2 a 4).

config.json:
{
  "fundo": "/caminho/fundo.jpg",            # qualquer tamanho; e reduzido/cortado para 640x480 (4:3)
  "rodape": "Leonardo Torres",              # texto no canto inferior esquerdo (opcional)
  "escurecer": 0.8,                         # brilho do fundo (opcional)
  "jogos": [
    {"nome": "FINAL FANTASY III", "capa": "capa.jpg", "disco": "disco.jpg",
     "exe": "cdrom:\\\\NES.EXE;1", "tcb": 4, "event": 16, "stack": "801fff00"}, ...
  ]
}
uso: menu_dat.py config.json saida/MENU.DAT   (tambem grava saida/MENU.DAT.telaN.png)

Formato (little endian), cabecalho de 2048 bytes:
  'MNU2', u16 n, u16 dsize, u32 base_off, u32 disc_off
  por jogo (64 bytes): char exe[40], u16 tcb, u16 event, u32 stack, u16 disc_x, u16 disc_y,
                       u32 patch_off, u32 rects_off, u16 nrects, u16 pad
Dados: tela base (tela do jogo 0, sem o disco dele) 640x480 15 bits; texturas dos discos
(dsize x dsize, 0 = transparente); por jogo: pedaco do fundo sob o disco (dsize x dsize) e a lista
de retangulos que diferem da base: u16 x,y,w,h + pixels.
"""
import sys, json, struct
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageEnhance
import numpy as np

W, H = 640, 480
DISC = 112
F = ImageFont.truetype('/usr/share/fonts/opentype/inter/InterDisplay-Bold.otf', 24)
F2 = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 12)


def fundo(cfg):
    b = Image.open(cfg['fundo']).convert('RGB')
    # cobre 640x480 mantendo a proporcao (corta o excesso no centro)
    s = max(W / b.width, H / b.height)
    b = b.resize((round(b.width * s), round(b.height * s)), Image.LANCZOS)
    x, y = (b.width - W) // 2, (b.height - H) // 2
    b = b.crop((x, y, x + W, y + H))
    return ImageEnhance.Brightness(b).enhance(cfg.get('escurecer', 0.8))


def disco(path, size, ang=0):
    d = Image.open(path).convert('RGBA').resize((size, size), Image.LANCZOS).rotate(ang)
    m = Image.new('L', (size, size), 0); dr = ImageDraw.Draw(m)
    dr.ellipse((1, 1, size - 2, size - 2), fill=255)
    r = size * 0.11; dr.ellipse((size / 2 - r, size / 2 - r, size / 2 + r, size / 2 + r), fill=0)
    d.putalpha(m); return d


def sombra(im, r=8, o=6):
    a = im.split()[-1]
    s = Image.new('RGBA', (im.width + 4 * r, im.height + 4 * r), (0, 0, 0, 0))
    s.paste((0, 0, 0, 190), (2 * r + o, 2 * r + o), a)
    return s.filter(ImageFilter.GaussianBlur(r))


def tela(cfg, sel, com_disco_sel=False):
    """monta a tela com o jogo 'sel' escolhido; devolve (imagem, posicao do disco escolhido)"""
    jogos = cfg['jogos']; n = len(jogos)
    c = fundo(cfg).convert('RGBA'); d = ImageDraw.Draw(c)
    t = jogos[sel]['nome']; tw = d.textlength(t, font=F)
    d.text(((W - tw) / 2 + 2, 44), t, font=F, fill=(0, 0, 0, 220))
    d.text(((W - tw) / 2, 42), t, font=F, fill=(235, 240, 250, 255))
    d.line(((W - tw) / 2, 76, (W + tw) / 2, 76), fill=(90, 210, 240, 255), width=2)
    big, small = (168, 136) if n >= 3 else (200, 168)
    pos = None
    for k, g in enumerate(jogos):
        cx = W * (k + 0.5) / n
        on = k == sel; cw = big if on else small
        cv = Image.open(g['capa']).convert('RGBA'); ch = int(cv.height * cw / cv.width)
        cv = cv.resize((cw, ch), Image.LANCZOS)
        if not on: cv = ImageEnhance.Brightness(cv).enhance(0.45)
        x = int(cx - cw / 2); y = 100 + (0 if on else 16)
        c.alpha_composite(sombra(cv), (x - 16, y - 16))
        if on:
            gl = Image.new('RGBA', (cw + 12, ch + 12), (90, 210, 240, 0)); m = Image.new('L', gl.size, 0)
            ImageDraw.Draw(m).rectangle((0, 0, gl.width - 1, gl.height - 1), fill=200)
            gl.putalpha(m.filter(ImageFilter.GaussianBlur(4))); c.alpha_composite(gl, (x - 6, y - 6))
        c.alpha_composite(cv, (x, y))
        ds = DISC if on else 84
        dk = disco(g['disco'], ds)
        dx, dy = int(cx - ds / 2), y + ch + 12
        c.alpha_composite(sombra(dk, 6, 4), (dx - 12, dy - 12))
        if on:
            pos = (dx, dy)
            if com_disco_sel: c.alpha_composite(dk, (dx, dy))
        else:
            a_ = dk.split()[-1]; dk = ImageEnhance.Brightness(dk).enhance(0.45); dk.putalpha(a_)
            c.alpha_composite(dk, (dx, dy))
    dica = '←  →  ESCOLHER        ✕  JOGAR'
    hw = d.textlength(dica, font=F2); d.text(((W - hw) / 2, H - 26), dica, font=F2, fill=(210, 220, 235, 255))
    if cfg.get('rodape'): d.text((20, H - 26), cfg['rodape'], font=F2, fill=(170, 185, 205, 255))
    return c.convert('RGB'), pos


BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16.0 - 0.5


def to15(img, alpha=None):
    a = np.asarray(img, dtype=np.float32); h, w, _ = a.shape
    th = np.tile(BAYER, (h // 4 + 1, w // 4 + 1))[:h, :w][..., None] * 8
    q = np.clip((a + th) / 8.0, 0, 31).round().astype(np.uint16)
    v = q[..., 0] | (q[..., 1] << 5) | (q[..., 2] << 10)
    if alpha is not None:
        v = np.where(np.asarray(alpha) >= 128, v | 0x8000, 0)
    return v.astype('<u2')


def retangulos(base, tela_, t=16):
    """faixas de 16 linhas; em cada faixa, junta blocos 16x16 vizinhos que diferem da base"""
    out = []
    for y in range(0, H, t):
        dif = [(base[y:y + t, x:x + t] != tela_[y:y + t, x:x + t]).any() for x in range(0, W, t)]
        x = 0
        while x < len(dif):
            if not dif[x]: x += 1; continue
            x0 = x
            while x < len(dif) and dif[x]: x += 1
            out.append((x0 * t, y, (x - x0) * t, min(t, H - y)))
    return out


def main():
    cfg = json.load(open(sys.argv[1])); out = sys.argv[2]
    n = len(cfg['jogos']); assert 2 <= n <= 4
    telas, pos = [], []
    for s in range(n):
        img, p = tela(cfg, s); img.save(out + '.tela%d.png' % s)
        tela(cfg, s, True)[0].save(out + '.preview%d.png' % s)
        telas.append(to15(img)); pos.append(p)
    base = telas[0]
    head_sz = 2048
    data = bytearray()
    base_off = head_sz; data += base.tobytes()
    disc_off = head_sz + len(data)
    for g in cfg['jogos']:
        dk = disco(g['disco'], DISC); data += to15(dk.convert('RGB'), dk.split()[-1]).tobytes()
    entries = []
    for s in range(n):
        x, y = pos[s]
        patch_off = head_sz + len(data); data += telas[s][y:y + DISC, x:x + DISC].tobytes()
        rects = retangulos(base, telas[s])
        rects_off = head_sz + len(data)
        for (rx, ry, rw, rh) in rects:
            data += struct.pack('<4H', rx, ry, rw, rh) + telas[s][ry:ry + rh, rx:rx + rw].tobytes()
        g = cfg['jogos'][s]
        entries.append(struct.pack('<40sHHIHHIIHH', g['exe'].encode(), g['tcb'], g['event'], int(g['stack'], 16),
                                   x, y, patch_off, rects_off, len(rects), 0))
        print('jogo %d: %d retangulos, disco em %s' % (s, len(rects), pos[s]))
    head = struct.pack('<4sHHII', b'MNU2', n, DISC, base_off, disc_off) + b''.join(entries)
    blob = head.ljust(head_sz, b'\0') + bytes(data)
    open(out, 'wb').write(blob)
    print('MENU.DAT', len(blob), 'bytes')


if __name__ == '__main__':
    main()
