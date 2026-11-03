import sys; sys.path.insert(0,'/root/fm1')
from fmfont import glyph
from PIL import Image
def codes(seg, rule):
    out=[]; i=0
    while i<len(seg):
        b=seg[i]
        if b>=0xf0: out.append(('C',b)); i+=1; continue
        if 0xc0<=b<=0xcf and rule:
            out.append(('G',rule(b,seg[i+1]))); i+=2; continue
        out.append(('G',b)); i+=1
    return out
def render_lines(seg, rule, fn, scale=2):
    lines=[[]]
    for t,v in codes(seg,rule):
        if t=='C':
            if v in (0xfe,0xff): lines.append([])
            else: lines[-1].append(None)
        else: lines[-1].append(v)
    lines=[l for l in lines if l][:20]
    W=max(len(l) for l in lines)*17+2
    im=Image.new('L',(W,len(lines)*18+2),30)
    for y,l in enumerate(lines):
        for x,c in enumerate(l):
            if c is not None and c<2880: im.paste(glyph(c),(x*17+1,y*18+1))
            elif c is None: im.paste(80,(x*17+1,y*18+1,x*17+17,y*18+17))
    im.resize((im.width*scale,im.height*scale),Image.NEAREST).save(fn)
