import re, sys, math, colorsys
from PIL import Image, ImageDraw, ImageFilter
exec(open(sys.argv[2]).read().split("# ===================== TEMA A")[0])
OUT=sys.argv[1]; SC=3

def disc(size, tint=0.0):
    """CD iridescente suave (supersample 4x)."""
    S=size*4; im=Image.new('RGBA',(S,S),(0,0,0,0)); p=im.load()
    c=S/2; R=S/2-2
    for y in range(S):
        for x in range(S):
            dx,dy=x-c,y-c; d=math.hypot(dx,dy)
            if d>R: continue
            r=d/R
            if r<0.12: continue                      # furo
            a=math.atan2(dy,dx)
            if r<0.30:                               # anel transparente / hub
                v=200 if r>0.27 or r<0.15 else 230
                p[x,y]=(v,v,v+10,140 if 0.15<r<0.27 else 255); continue
            # arco-iris conico + leve espiral
            h=(a/(2*math.pi)+0.25*r+tint)%1.0
            s=0.45+0.15*math.sin(a*2)
            l=0.62+0.12*math.cos(a*2+r*3)
            rr,gg,bb=colorsys.hls_to_rgb(h,l,s)
            # brilho especular em faixa diagonal
            spec=max(0,math.cos(a-0.8))**18+max(0,math.cos(a+math.pi-0.8))**18
            k=min(1,spec*0.9)
            rr,gg,bb=[v+(1-v)*k for v in (rr,gg,bb)]
            if r>0.96: rr,gg,bb=0.88,0.9,0.95        # borda
            p[x,y]=(int(rr*255),int(gg*255),int(bb*255),255)
    # sombra
    im=im.resize((size,size),Image.LANCZOS)
    sh=Image.new('RGBA',(size+12,size+12),(0,0,0,0))
    ImageDraw.Draw(sh).ellipse((8,10,size+6,size+8),fill=(0,0,0,110))
    sh=sh.filter(ImageFilter.GaussianBlur(5)); sh.alpha_composite(im,(2,2))
    return sh

def up(c): return c.img.resize((W*SC,H*SC),Image.NEAREST).convert('RGBA')
def put_disc(img,cx,cy,r,tint=0.0):
    d=disc(r*2*SC,tint); img.alpha_composite(d,(cx*SC-r*SC-2,cy*SC-r*SC-2))
def pair(a,b,name,gap=24,bg=(0,0,0)):
    out=Image.new('RGB',(a.width*2+gap,a.height),bg); out.paste(a,(0,0)); out.paste(b,(a.width+gap,0)); out.save(OUT+name)
def rbox(c,x,y,w,h,col,rad=3):
    for j in range(h):
        for i in range(w):
            dx=min(i,w-1-i); dy=min(j,h-1-j)
            if dx<rad and dy<rad and math.hypot(rad-dx-0.5,rad-dy-0.5)>rad: continue
            c.p(x+i,y+j,col)

# ===================== D: VIDRO (azul petroleo, paineis arredondados) =====================
MINT=(90,240,200); TEAL=(8,40,48); WT=(235,250,250); MD=(120,170,175)
def glassD(tab):
    c=C((0,0,0))
    for j in range(H):
        t=j/H; c.rect(0,j,W,1,(int(4+10*t),int(30+20*(1-t)),int(40+25*(1-t))))
    c.txt(12,8,"RETROVOLT",WT); c.rtxt(W-12,8,"12:34",MD)
    rbox(c,60,22,200,18,(10,60,70),8)
    for n,t in enumerate(TABS):
        x=62+n*66
        if n==tab: rbox(c,x,24,64,14,MINT,6); c.txt(x+(64-len(t)*8)//2,27,t,TEAL)
        else: c.txt(x+(64-len(t)*8)//2,27,t,MD)
    c.txt(38,27,"L1",MD); c.txt(266,27,"R1",MD)
    return c
a=glassD(0)
rbox(a,24,48,272,170,(12,58,68),8); rbox(a,25,49,270,168,(16,70,80),8)
a.ctxt(150,"PLAYSTATION",MINT,None,2)
a.ctxt(172,"CASTLEVANIA SYMPHONY OF THE NIGHT",WT)
a.ctxt(184,"PCSX REARMED  -  240P",MD)
rbox(a,118,198,84,14,MINT,6); a.ctxt(201,"X  JOGAR",TEAL)
A=up(a); put_disc(A,160,96,42)
b=glassD(1)
rbox(b,24,48,272,170,(12,58,68),8); rbox(b,25,49,270,168,(16,70,80),8)
y=56
for n,s in enumerate(SYSTEMS):
    if n==7: rbox(b,32,y-3,256,14,MINT,6); b.txt(44,y,s,TEAL)
    else: b.txt(44,y,s,WT)
    y+=18
B=up(b); pair(A,B,'_temaD_vidro.png')

# ===================== E: ARCADE (vermelho/preto/amarelo) =====================
RED=(220,30,40); YL=(255,215,0); BK=(14,10,10)
def arcadeE(tab):
    c=C(BK)
    for i in range(-H,W,16):                         # listras diagonais
        for j in range(0,30):
            for k in range(8): c.p(i+j+k,j,(40,10,12))
    c.rect(0,30,W,3,YL); c.rect(0,33,W,2,RED)
    c.txt(10,10,"RETROVOLT",YL,(120,0,0),1); c.rtxt(W-10,10,"CREDIT 00",(255,120,120))
    # abas como botoes de fliperama
    xs=[54,160,266]
    for n,t in enumerate(TABS):
        x=xs[n]; col=[RED,(40,160,255),(60,200,90)][n]
        on=(n==tab)
        for j in range(-9,10):
            for i in range(-9,10):
                if i*i+j*j<=81: c.p(x+i,52+j,col if on else tuple(v//3 for v in col))
        if on:
            for j in range(-9,10):
                for i in range(-9,10):
                    if i*i+j*j<=81 and i*i+j*j>=64: c.p(x+i,52+j,YL)
        c.ctxt(66,t,YL if on else (110,90,60),None,1,x)
    c.txt(8,48,"L1",(150,120,60)); c.rtxt(W-8,48,"R1",(150,120,60))
    c.rect(0,H-14,W,14,RED); c.rect(0,H-15,W,1,YL)
    return c
a=arcadeE(0)
a.ctxt(168,"PLAYSTATION",YL,(140,0,0),2)
a.ctxt(190,"CASTLEVANIA SOTN",(255,255,255))
a.ctxt(H-11,"INSIRA FICHA... OU APERTE X",YL)
A=up(a); put_disc(A,160,120,36,0.5)
b=arcadeE(1)
y=84
for n,s in enumerate(SYSTEMS):
    if n==7: b.rect(0,y-3,W,14,YL); b.txt(40,y,"> "+s+" <",BK)
    else: b.txt(40,y,"  "+s,(255,230,200) if n%2==0 else (255,170,150))
    y+=15
b.ctxt(H-11,"X ABRIR    O VOLTAR",YL)
B=up(b); pair(A,B,'_temaE_arcade.png',bg=(40,0,0))

# ===================== F: EDITORIAL (azul marinho, numeros grandes) =====================
NAV=(12,18,40); OR=(255,140,40); IV=(240,236,225); SL=(120,130,160)
def editF(tab):
    c=C(NAV)
    c.rect(0,0,6,H,OR)
    c.txt(16,8,"RETROVOLT",IV); c.rtxt(W-10,8,"12:34",SL)
    x=16
    for n,t in enumerate(TABS):
        num="0%d"%(n+1); on=(n==tab)
        c.txt(x,24,num,OR if on else SL,None,2)
        c.txt(x+36,28,t,IV if on else SL)
        if on: c.rect(x,42,36+len(t)*8,2,OR)
        x+=36+len(t)*8+14
    c.rect(16,48,W-26,1,(40,50,80))
    c.txt(16,H-12,"L1/R1 ABAS",SL)
    return c
a=editF(0)
a.txt(134,66,"DISCO NA LEITORA",OR)
a.txt(134,82,"PLAYSTATION",IV,None,2)
a.txt(134,104,"CASTLEVANIA",IV); a.txt(134,114,"SYMPHONY OF",IV); a.txt(134,124,"THE NIGHT",IV)
a.txt(134,142,"SLUS-00067",SL); a.txt(134,152,"PCSX REARMED",SL)
a.rect(134,172,120,18,OR); a.txt(142,177,"X  JOGAR  ->",NAV)
a.rtxt(W-10,H-12,"TUBO 15KHZ 240P",SL)
A=up(a); put_disc(A,68,128,52,0.15)
b=editF(1)
y=60
for n,s in enumerate(SYSTEMS):
    on=(n==7)
    b.txt(20,y,"%02d"%(n+1),OR if on else (60,70,100))
    b.txt(44,y,s,IV if on else SL)
    if on: b.rect(44,y+10,len(s)*8,1,OR); b.rtxt(W-14,y,"->",OR)
    y+=18
b.rtxt(W-10,H-12,"X ABRIR  O VOLTAR",SL)
B=up(b); pair(A,B,'_temaF_editorial.png',bg=(4,6,16))
