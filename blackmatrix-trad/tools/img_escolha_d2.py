"""Disco 2: tela de escolha do mestre (SYSTEM.DAT bloco 3, SCL de fundo branco e texto preto grosso).
uso: img_escolha_d2.py tools SYSTEM_entrada SYSTEM_saida [previa.png]"""
import sys, struct, numpy as np
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, sys.argv[1]); import scl
src, out = sys.argv[2:4]; prev = sys.argv[4] if len(sys.argv) > 4 else None
K = 3
d = bytearray(open(src, 'rb').read())
s, ln = struct.unpack('<II', d[16 + K * 8:24 + K * 8]); blk = bytes(d[s * 2048:(s + ln) * 2048])
offs, texs, tab = scl.ler(blk); a = np.repeat(scl.monta(texs, tab)[..., :3], 2, 0)
im = Image.fromarray(a); dr = ImageDraw.Draw(im)
dr.rectangle((0, 0, 639, 140), fill=(255, 255, 255)); dr.rectangle((0, 300, 639, 479), fill=(255, 255, 255))
F = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
def linha(t, yc, sz):
    f = ImageFont.truetype(F, sz)
    while dr.textlength(t, font=f) > 600: sz -= 2; f = ImageFont.truetype(F, sz)
    bb = dr.textbbox((0, 0), t, font=f); w = bb[2] - bb[0]; h = bb[3] - bb[1]
    dr.text((320 - w / 2 - bb[0], yc - h / 2 - bb[1]), t, font=f, fill=(0, 0, 0))
linha('Escolha seu', 72, 84); linha('mestre!', 376, 96)
a = np.array(im).astype(np.int32); meia = ((a[0::2] + a[1::2]) // 2).astype(np.uint8)
img = np.dstack([meia, np.full(meia.shape[:2], 255, np.uint8)])
nt, ntab, n = scl.reconstroi(img, texs, tab); novo = scl.grava(blk, offs, nt, ntab); d[s * 2048:s * 2048 + len(novo)] = novo
if prev: Image.fromarray(np.repeat(meia, 2, 0)).save(prev)
open(out, 'wb').write(bytes(d)); print('escolha d2 ok, blocos', n)
