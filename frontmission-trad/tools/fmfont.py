from PIL import Image
F=open('/root/fm1/pak/51.raw','rb').read()
PAL=[0,255,170,100]
def tile(t,im,ox):
    o=t*32
    for y in range(16):
        b0=F[o+2*y]; b1=F[o+2*y+1]
        for x in range(8):
            v=((b0>>(7-x))&1)*2+((b1>>(7-x))&1)
            if v: im.putpixel((ox+x,y),PAL[v])
def glyph(i):
    im=Image.new('L',(16,16),0)
    tile(2*i,im,0); tile(2*i+1,im,8)
    return im
NGLYPH=len(F)//64
