"""Gera MENU.DAT para o loader: 2 telas 640x480 (15 bits, com dithering) sem o disco do jogo
selecionado, e 2 texturas de disco 112x112 (15 bits, fundo transparente = 0) para girar no PS1.
Layout identico ao mockup (fm-loader/mockup/mock.py)."""
import sys, struct
sys.path.insert(0, '/home/user/PS2TVCRT/fm-loader/mockup')
from PIL import Image, ImageDraw, ImageEnhance
import numpy as np
import mock

W, H = 640, 480
DISC = 112                      # tamanho da textura do disco (selecionado)

def layout(sel):
    """repete a geometria do mockup; devolve a tela sem o disco selecionado e a posicao dele"""
    c = mock.bg().convert('RGBA'); d = ImageDraw.Draw(c)
    nomes = ['FRONT MISSION 1ST', 'FRONT MISSION 2']
    capas = ['43.jpg', '44.jpg']; discos = ['45.jpg', '42.jpg']
    t = nomes[sel]; tw = d.textlength(t, font=mock.F)
    d.text(((W-tw)/2+2, 52), t, font=mock.F, fill=(0, 0, 0, 200)); d.text(((W-tw)/2, 50), t, font=mock.F, fill=(235, 240, 245, 255))
    d.line(((W-tw)/2, 86, (W+tw)/2, 86), fill=(80, 200, 230, 255), width=2)
    pos = None
    for k, cx in enumerate((W*0.28, W*0.72)):
        on = k == sel
        cw = 200 if on else 168
        cv = mock.CAPAS.setdefault(capas[k], mock.recorta(capas[k])); ch = int(cv.height*cw/cv.width); cv = cv.resize((cw, ch), Image.LANCZOS)
        if not on:
            a_ = cv.split()[-1]; cv = ImageEnhance.Brightness(cv).enhance(0.45); cv.putalpha(a_)
        x = int(cx-cw/2); y = 110+(0 if on else 14)
        c.alpha_composite(mock.shadow(cv), (x-16, y-16))
        if on:
            from PIL import ImageFilter
            g = cv.split()[-1].filter(ImageFilter.MaxFilter(7)).filter(ImageFilter.GaussianBlur(5))
            gl = Image.new('RGBA', cv.size, (80, 200, 230, 0)); gl.putalpha(g.point(lambda v: int(v*0.8)))
            c.alpha_composite(gl, (x, y))
        c.alpha_composite(cv, (x, y))
        ds = DISC if on else 90
        dk = mock.disc(discos[k], ds, 0)
        dy = y+ch+14; dx = int(cx-ds/2)
        c.alpha_composite(mock.shadow(dk, 6, 4), (dx-12, dy-12))
        if on:
            pos = (dx, dy)                     # o PS1 desenha este disco girando
        else:
            dk = Image.composite(ImageEnhance.Brightness(dk).enhance(0.45), Image.new('RGBA', dk.size, (0, 0, 0, 0)), dk.split()[-1])
            c.alpha_composite(dk, (dx, dy))
    hint = '←  →  ESCOLHER        ✕  JOGAR'
    hw = d.textlength(hint, font=mock.F2); d.text(((W-hw)/2, H-28), hint, font=mock.F2, fill=(200, 210, 220, 255))
    return c.convert('RGB'), pos

BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]])/16.0 - 0.5

def to15(img, alpha=None):
    """RGB888 -> 15 bits do PS1 (bit15 = opaco p/ textura), com dithering ordenado"""
    a = np.asarray(img, dtype=np.float32)
    h, w, _ = a.shape
    th = np.tile(BAYER, (h//4+1, w//4+1))[:h, :w][..., None]*8
    q = np.clip((a+th)/8.0+0.0, 0, 31).round().astype(np.uint16)
    v = q[..., 0] | (q[..., 1] << 5) | (q[..., 2] << 10)
    if alpha is not None:
        al = np.asarray(alpha) >= 128
        v = np.where(al, v | 0x8000, 0).astype(np.uint16)
    return v.astype('<u2').tobytes()

def disc_tex(path):
    d = mock.disc(path, DISC, 0)
    return to15(d.convert('RGB'), d.split()[-1])

if __name__ == '__main__':
    out = sys.argv[1]
    hdr = []; blob = b''
    for sel in (0, 1):
        img, pos = layout(sel)
        img.save(out + '.tela%d.png' % sel)
        hdr.append(pos)
        blob += to15(img)
    blob += disc_tex('45.jpg') + disc_tex('42.jpg')
    # cabecalho: magic, posicao do disco de cada tela (x,y), tamanho do disco
    head = struct.pack('<4s5H', b'FMLD', hdr[0][0], hdr[0][1], hdr[1][0], hdr[1][1], DISC)
    head = head.ljust(2048, b'\0')
    open(out, 'wb').write(head + blob)
    print('MENU.DAT', len(head+blob), 'bytes; discos em', hdr)
