from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageEnhance
I='/tmp/claude-0/-home-user-PS2TVCRT/3556a9de-1f69-5db9-a91d-fdfcea6c5b46/images/'
W,H=640,480
F=ImageFont.truetype('/usr/share/fonts/opentype/inter/InterDisplay-Bold.otf',26)
F2=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',12)
def bg():
    b=Image.open(I+'41.jpg').convert('RGB')
    h=H; w=int(b.width*h/b.height); b=b.resize((w,h),Image.LANCZOS)
    x=(w-W)//2; b=b.crop((x,0,x+W,H))
    b=ImageEnhance.Brightness(b).enhance(0.75)
    # vinheta
    v=Image.new('L',(W,H),0); d=ImageDraw.Draw(v)
    for i in range(60): d.rectangle((i*2,i*2,W-i*2,H-i*2),fill=int(255*i/60))
    v=v.filter(ImageFilter.GaussianBlur(30))
    return Image.composite(b,Image.new('RGB',(W,H),(0,0,0)),v)
def disc(path,size,ang):
    d=Image.open(I+path).convert('RGBA').resize((size,size),Image.LANCZOS).rotate(ang)
    m=Image.new('L',(size,size),0); ImageDraw.Draw(m).ellipse((1,1,size-2,size-2),fill=255)
    d.putalpha(m); return d
def shadow(im,r=8,o=6):
    a=im.split()[-1] if im.mode=='RGBA' else Image.new('L',im.size,255)
    s=Image.new('RGBA',(im.width+4*r,im.height+4*r),(0,0,0,0))
    s.paste((0,0,0,180),(2*r+o,2*r+o),a); return s.filter(ImageFilter.GaussianBlur(r))
def recorta(path,tol=28):
    if path.endswith('.png'): return Image.open(I+path).convert('RGBA')
    im=Image.open(I+path).convert('RGB'); W0,H0=im.size
    from collections import deque
    px=im.load(); m=Image.new('L',im.size,255); mp=m.load()
    q=deque([(0,0),(W0-1,0),(0,H0-1),(W0-1,H0-1)])
    for x in range(0,W0,8): q+=[(x,0),(x,H0-1)]
    for y in range(0,H0,8): q+=[(0,y),(W0-1,y)]
    while q:
        x,y=q.pop()
        if x<0 or y<0 or x>=W0 or y>=H0 or mp[x,y]==0: continue
        r,g,b=px[x,y]
        if min(r,g,b)<255-tol or max(r,g,b)-min(r,g,b)>20: continue
        mp[x,y]=0; q+=[(x+1,y),(x-1,y),(x,y+1),(x,y-1)]
    m=m.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(0.8))
    o=im.convert('RGBA'); o.putalpha(m); return o
CAPAS={}
def tela(sel):
    c=bg().convert('RGBA'); d=ImageDraw.Draw(c)
    nomes=['FRONT MISSION 1ST','FRONT MISSION 2']
    capas=['43.jpg','44.jpg']; discos=['45.jpg','42.jpg']
    # titulo
    t=nomes[sel]; tw=d.textlength(t,font=F)
    d.text(((W-tw)/2+2,52),t,font=F,fill=(0,0,0,200)); d.text(((W-tw)/2,50),t,font=F,fill=(235,240,245,255))
    d.line(((W-tw)/2,86,(W+tw)/2,86),fill=(80,200,230,255),width=2)
    for k,cx in enumerate((W*0.28,W*0.72)):
        on=k==sel
        cw=200 if on else 168
        cv=CAPAS.setdefault(capas[k],recorta(capas[k])); ch=int(cv.height*cw/cv.width); cv=cv.resize((cw,ch),Image.LANCZOS)
        if not on:
            a_=cv.split()[-1]; cv=ImageEnhance.Brightness(cv).enhance(0.45); cv.putalpha(a_)
        x=int(cx-cw/2); y=110+(0 if on else 14)
        s=shadow(cv); c.alpha_composite(s,(x-16,y-16))
        if on: pass
        c_later=(cv,x,y)
        if on:
            g=cv.split()[-1].filter(ImageFilter.MaxFilter(7)).filter(ImageFilter.GaussianBlur(5))
            gl=Image.new('RGBA',cv.size,(80,200,230,0)); gl.putalpha(g.point(lambda v:int(v*0.8)))
            c.alpha_composite(gl,(x,y))
        c.alpha_composite(*[c_later[0]],(c_later[1],c_later[2]))
        ds=110 if on else 90
        dk=disc(discos[k],ds,-35 if on else 0)
        if not on: dk=Image.composite(ImageEnhance.Brightness(dk).enhance(0.45),Image.new('RGBA',dk.size,(0,0,0,0)),dk.split()[-1])
        dy=y+ch+14
        c.alpha_composite(shadow(dk,6,4),(int(cx-ds/2)-12,dy-12)); c.alpha_composite(dk,(int(cx-ds/2),dy))
    hint='←  →  ESCOLHER        ✕  JOGAR'
    hw=d.textlength(hint,font=F2); d.text(((W-hw)/2,H-28),hint,font=F2,fill=(200,210,220,255))
    return c.convert('RGB')
a=tela(0); b=tela(1)
a.save('tela_fm1.png'); b.save('tela_fm2.png')
m=Image.new('RGB',(W*2+20,H),(20,20,20)); m.paste(a,(0,0)); m.paste(b,(W+20,0)); m.save('mockup.png')
