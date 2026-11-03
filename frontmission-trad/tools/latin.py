"""Letras latinas 8x16 no estilo da fonte do PS1: traco (2) de 1 px com contorno (3)."""
from PIL import Image, ImageFont, ImageDraw
UF=ImageFont.truetype('/usr/share/fonts/opentype/unifont/unifont_jp.otf',16)
def stroke(ch):
    im=Image.new('L',(16,16),0); ImageDraw.Draw(im).text((0,0),ch,font=UF,fill=255)
    px=[[1 if im.getpixel((x,y))>127 else 0 for x in range(16)] for y in range(16)]
    cols=[x for x in range(16) if any(px[y][x] for y in range(16))]
    return px,cols
def cell(ch):
    """matriz 16x8 com valores 0/2/3"""
    g=[[0]*8 for _ in range(16)]
    if ch==' ': return g
    px,cols=stroke(ch)
    if not cols: return g
    w=cols[-1]-cols[0]+1
    x0=max(1,(8-w)//2) if w<=6 else 1
    sh=cols[0]-x0
    for y in range(16):
        for x in range(8):
            sx=x+sh
            if 0<=sx<16 and px[y][sx]: g[y][x]=2
    for y in range(16):
        for x in range(8):
            if g[y][x]==0 and any(0<=y+dy<16 and 0<=x+dx<8 and g[y+dy][x+dx]==2 for dy in (-1,0,1) for dx in (-1,0,1)):
                g[y][x]=3
    return g
