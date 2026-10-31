"""Disco 2 (EVENT.DAT, telas SCL de fundo preto): fins de capitulo 7-11 e as falas do final do jogo
(texto vertical em japones -> texto horizontal em portugues, branco com contorno, centralizado).
Cada mestre tem a sua versao da fala (Domina, Prica, Clage, Michette, Praha, Zero).
uso: img_fim_d2.py tools EVENT_entrada EVENT_saida [pasta_previa]"""
import sys, os, struct, numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
sys.path.insert(0, sys.argv[1]); import scl
src, out = sys.argv[2:4]; prev = sys.argv[4] if len(sys.argv) > 4 else None
SERIF = '/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf'
SANS = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
CAP = {380: ('Capítulo 7', 'Além da Colina de Narcis Tezetta...'),
       402: ('Capítulo 8', 'Duas igualdades'),
       431: ('Capítulo 9', 'Noé, terra dos escravos'),
       488: ('Capítulo 10', 'A alma que renasce'),
       499: ('Capítulo 11', 'Rumo ao fim...')}
FALA = {
    542: 'N-na...|Na verdade...',
    549: 'Tudo bem...|Eu ouvi|tudo...',
    550: 'Pra mim...|era difícil demais,|não entendi direito,|mas...|ouvi tudo...',
    551: 'Sim...|Eu estava sem sentidos,|mas ouvi tudo...',
    552: 'Tudo bem...|Eu ouvi|tudo...',
    553: 'Tudo bem...|Ouvi tudo...',
    554: 'Eu sei...|Não estava bem consciente,|mas ouvi a conversa|toda...!',
    555: 'Mais importante,|olhe aquilo!!',
    556: 'M-mais importante,|maninho,|olhe aquilo!!',
    557: '...Tudo|acabou, não é?|E...|o que restou foi...',
    558: 'M-mais importante...|olhe aquilo...!!',
    559: 'M-mais importante,|o-olhe aquilo...!!',
    560: 'Nós...|perdemos|tudo, né...',
    563: '...Aquilo é',
    564: 'o "Jardim do Éden"...|que estava no fundo do mar|desde o Grande Dilúvio...',
    565: 'o "Jardim do Éden"...|que afundou no mar|há muito, muito tempo...',
    566: 'o "Jardim do Éden"...|que estava no fundo do mar|desde o Grande Dilúvio...',
    567: 'o "Jardim do Éden"...|que estava no fundo do mar|desde o Grande Dilúvio...',
    568: 'o "Jardim do Éden"...|que estava no fundo do mar|desde o Grande Dilúvio...',
    569: 'Em troca de tudo,|a única coisa que restou|foi aquele "Jardim do Éden"...',
    576: 'Quando eu era pequena...|minha mãe me contou|uma coisa...',
    577: 'Quando eu era pequenininha...|a mamãe me contou|uma coisa...',
    578: 'Quando eu era pequena...|minha mãe me contou|uma coisa...',
    579: 'Quando eu era pequena...|minha mãe me contou|uma coisa...',
    580: 'Quando eu era pequena...|minha mãe me contou|uma coisa...',
    581: 'Há muito tempo...|minha mãe me contou|uma coisa...',
    582: '......!?',
    583: 'Dizem que "Satanás"...|quer dizer "aquele que é|o mais amado por Deus"...',
    584: 'Dizem que "Satanás"...|quer dizer "aquele que é|o mais amado por Deus"...',
    585: 'Dizem que "Satanás", aquele que|levaria o mundo à destruição...|quer dizer "o mais|amado por Deus"...',
    586: 'Dizem que "Satanás"...|quer dizer "aquele que é|o mais amado por Deus"...',
    587: 'Dizem que "Satanás"...|quer dizer "aquele que é|o mais amado por Deus"...',
    588: 'Dizem que "Satanás", aquele que|levaria o mundo à destruição...|quer dizer "o mais|amado por Deus"...',
    589: 'Parece que quer dizer|"aquele que é o mais|amado por Deus"...',
    590: 'Mas...|além de nós...|não sobrou|mais ninguém...',
    593: 'Mesmo assim,|com você ao meu lado,|acho que vamos dar um jeito!',
    594: 'Mesmo assim...|com o maninho do lado,|acho que dá pra dar um jeito...',
    595: 'Se a gente|unir as forças...|mesmo assim acho que|vamos dar um jeito!!',
    596: '......|Mesmo assim...|com você,|acho que daremos um jeito...',
    597: '......Mas...|mesmo assim...|com você,|acho que dá pra dar um jeito...',
    598: 'Mas se formos nós...|até isso parece|possível, não acha...!?',
    599: 'Para isso,|nós...|precisamos ter|muitos bebês!!',
    600: 'Para isso,|nós...|temos que ter|muuuitos bebês!!',
    601: 'Para isso,|nós...|temos que ter|muitos bebês!!',
    602: 'Para isso,|nós...|precisaremos ter|muitos bebês!!',
    603: 'Para isso,|nós...|teremos que ter|muitos bebês...!!',
    604: 'Hm...|Viver só|nós dois...|também não é nada mal!',
    609: 'E ela se tornou|a segunda "Eva"!!',
    610: 'E assim, a jornada|sem fim dos dois...|está apenas começando!!',
    619: 'Por quê!?|Por que, pai!?',
    620: 'Irmão......',
    621: 'Irmão Caaain!!'}

def contorno(im, xy, t, f):
    m = Image.new('L', im.size, 0); ImageDraw.Draw(m).text(xy, t, font=f, fill=255)
    im.paste(Image.new('RGB', im.size, (24, 24, 24)), (0, 0), m.filter(ImageFilter.MaxFilter(5)))
    im.paste(Image.new('RGB', im.size, (240, 240, 240)), (0, 0), m)

def fala(t):
    im = Image.new('RGB', (640, 480), (0, 0, 0)); dr = ImageDraw.Draw(im)
    linhas = t.split('|'); sz = 34
    while True:
        f = ImageFont.truetype(SANS, sz)
        if max(dr.textlength(l, font=f) for l in linhas) <= 590 and len(linhas) * sz * 1.35 <= 440: break
        sz -= 1
    lh = int(sz * 1.35); y = 240 - lh * len(linhas) // 2
    for l in linhas:
        w = dr.textlength(l, font=f); contorno(im, (320 - w / 2, y), l, f); y += lh
    return im

def capitulo(c, t):
    im = Image.new('RGB', (640, 480), (0, 0, 0)); dr = ImageDraw.Draw(im)
    f = ImageFont.truetype(SERIF, 28); dr.text((51, 28), c, font=f, fill=(255, 255, 255))
    sz = 36; f = ImageFont.truetype(SERIF, sz); tt = '“%s”' % t
    while dr.textlength(tt, font=f) > 600: sz -= 1; f = ImageFont.truetype(SERIF, sz)
    dr.text((320 - dr.textlength(tt, font=f) / 2, 206), tt, font=f, fill=(255, 255, 255))
    f = ImageFont.truetype(SERIF, 30); dr.text((611 - dr.textlength('Fim', font=f), 412), 'Fim', font=f, fill=(255, 255, 255))
    return im

d = bytearray(open(src, 'rb').read())
for k in list(CAP) + list(FALA):
    s, ln = struct.unpack('<II', d[16 + k * 8:24 + k * 8]); blk = bytes(d[s * 2048:(s + ln) * 2048]); assert blk[:4] == b'SCL ', k
    offs, texs, tab = scl.ler(blk)
    orig = scl.monta(texs, tab)
    assert (orig[..., :3].reshape(-1, 3).max(1) < 40).mean() > 0.6, ('esperava tela preta', k)
    im = capitulo(*CAP[k]) if k in CAP else fala(FALA[k])
    a = np.array(im).astype(np.int32); meia = ((a[0::2] + a[1::2]) // 2).astype(np.uint8)
    img = np.dstack([meia, np.full(meia.shape[:2], 255, np.uint8)])
    nt, ntab, n = scl.reconstroi(img, texs, tab); novo = scl.grava(blk, offs, nt, ntab); d[s * 2048:s * 2048 + len(novo)] = novo
    if prev: Image.fromarray(np.repeat(meia, 2, 0)).save(os.path.join(prev, 'fim_%d.png' % k))
open(out, 'wb').write(bytes(d)); print('telas pretas traduzidas:', len(CAP) + len(FALA))
