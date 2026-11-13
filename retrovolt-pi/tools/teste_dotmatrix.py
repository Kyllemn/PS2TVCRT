#!/usr/bin/env python3
"""teste do DOT MATRIX: repete as contas de desenha_base (rv-jogo.c) e confere se todos os
pontos do jogo viram blocos iguais. uso: python3 -B tools/teste_dotmatrix.py"""
# repete as contas de desenha_base (rv-jogo.c) para o DOT MATRIX no LCD (nao compila nada)
def geo(w, h, fw, fh, aspecto=4/3, novo=True, tela_cheia=False):
    alta = h > 300
    eh = h
    gh = eh // 2 if alta else eh
    lp = max(1, fh // gh)
    dh = gh * lp
    dot_int = novo and not alta and lp >= 2
    if tela_cheia and not dot_int:
        dh = fh
    cw = round(dh * aspecto)          # lround
    if dot_int:
        kx = max(1, round(cw / w))
        while kx > 1 and kx * w > fw:
            kx -= 1
        cw = kx * w
    cw = min(cw, fw)
    # larguras de cada ponto do jogo (colunas com o mesmo xmap) e alturas de cada linha
    xmap = [i * w // cw for i in range(cw)]
    larg = {}
    for x in xmap:
        larg[x] = larg.get(x, 0) + 1
    alts = [(j + 1) * dh // eh - j * dh // eh for j in range(eh)]
    return cw, dh, set(larg.values()), set(alts)

monitores = [(1440, 900), (1920, 1080), (1280, 720), (1280, 1024), (1366, 768), (1024, 768), (1680, 1050),
             (1600, 900), (1280, 960), (800, 600), (640, 480), (1280, 800)]
larguras = [256, 288, 320, 336, 352, 384, 512]   # 224 linhas: MD H32/H40, SNES (e 512), PCE, CPS, Neo Geo
ruins_antes = ruins = 0
for fw, fh in monitores:
    for w in larguras:
        for tc in (False, True):
            _, _, la, al = geo(w, 224, fw, fh, novo=False, tela_cheia=tc)
            if len(la) > 1 or len(al) > 1:
                ruins_antes += 1
            cw, dh, la, al = geo(w, 224, fw, fh, novo=True, tela_cheia=tc)
            if len(la) > 1 or len(al) > 1 or cw > fw or dh > fh:
                ruins += 1
                print('DESIGUAL', fw, fh, w, tc, la, al)
print('combinacoes:', len(monitores) * len(larguras) * 2, '| desalinhadas antes:', ruins_antes, '| depois:', ruins)
for fw, fh in ((1440, 900), (1920, 1080), (1280, 720)):
    print(fw, fh, ' '.join('%dx224->%dx%d(%dx%d)' % (w, *geo(w, 224, fw, fh)[:2], list(geo(w, 224, fw, fh)[2])[0],
                                                     list(geo(w, 224, fw, fh)[3])[0]) for w in (256, 320, 384, 512)))
