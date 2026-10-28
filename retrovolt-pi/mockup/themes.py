import re, sys, math
from PIL import Image
src = open('/home/user/PS2TVCRT/pceusb/src/font8x8.h').read()
FONT = bytes(int(h,16) for h in re.findall(r'\\x([0-9A-Fa-f]{2})', src))
W,H = 320,240
OUT=sys.argv[1]

class C:
    def __init__(s,bg):
        s.img=Image.new('RGB',(W,H),bg); s.px=s.img.load()
    def p(s,x,y,c):
        if 0<=x<W and 0<=y<H: s.px[x,y]=c
    def rect(s,x,y,w,h,c):
        for j in range(y,y+h):
            for i in range(x,x+w): s.p(i,j,c)
    def box(s,x,y,w,h,c):
        s.rect(x,y,w,1,c); s.rect(x,y+h-1,w,1,c); s.rect(x,y,1,h,c); s.rect(x+w-1,y,1,h,c)
    def ch(s,x,y,c,col,sc=1):
        o=ord(c)-32
        if o<0 or o>94: return
        for r in range(8):
            b=FONT[o*8+r]
            for k in range(8):
                if b>>k&1: s.rect(x+k*sc,y+r*sc,sc,sc,col)
    def txt(s,x,y,t,col,sh=None,sc=1):
        for i,c in enumerate(t):
            if sh: s.ch(x+i*8*sc+sc,y+sc,c,sh,sc)
            s.ch(x+i*8*sc,y,c,col,sc)
    def ctxt(s,y,t,col,sh=None,sc=1,cx=W//2):
        s.txt(cx-len(t)*4*sc,y,t,col,sh,sc)
    def rtxt(s,x,y,t,col): s.txt(x-len(t)*8,y,t,col)
    def icon(s,x,y,rows,col,sc=1):
        for r,row in enumerate(rows):
            for k,v in enumerate(row):
                if v=='#': s.rect(x+k*sc,y+r*sc,sc,sc,col)

DISC=["...####...","..#....#..",".#.####.#.","#.#....#.#","#.#.##.#.#","#.#.##.#.#","#.#....#.#",".#.####.#.","..#....#..","...####..."]
SYSTEMS=["ATARI 2600","NES","SNES","MASTER SYSTEM","MEGA DRIVE","PC ENGINE","GAME BOY","PLAYSTATION","NEO GEO CD"]
TABS=["DISCO","JOGOS","OPCOES"]

def pair(a,b,name,sc=3,gap=8,bg=(0,0,0)):
    out=Image.new('RGB',(W*2+gap,H),bg); out.paste(a.img,(0,0)); out.paste(b.img,(W+gap,0))
    out.resize((out.width*sc,out.height*sc),Image.NEAREST).save(OUT+name)

# ===================== TEMA A: TERMINAL FOSFORO (verde) =====================
G=(60,255,120); GD=(20,110,50); GB=(4,20,8)
def termA(tab):
    c=C((0,8,2))
    for j in range(0,H,2): c.rect(0,j,W,1,(0,14,5))
    c.txt(8,6,"RETROVOLT OS v1.0",G); c.rtxt(W-8,6,"[12:34]",GD)
    x=8
    c.txt(x,22,"L1",GD); x+=24
    for n,t in enumerate(TABS):
        s="[ %s ]"%t if n==tab else "  %s  "%t
        if n==tab: c.rect(x-2,20,len(s)*8+4,12,G); c.txt(x,22,s,(0,8,2))
        else: c.txt(x,22,s,GD)
        x+=len(s)*8+8
    c.txt(x,22,"R1",GD)
    c.rect(8,36,W-16,1,GD)
    c.rect(0,H-13,W,1,GD)
    return c
a=termA(0)
a.txt(16,48,"> LEITORA: ASUS SDRW-08D1S-U ... OK",G)
a.txt(16,60,"> DISCO   : PLAYSTATION (CD)",G)
a.txt(16,72,"> JOGO    : CASTLEVANIA SOTN",G)
a.txt(16,84,"> CODIGO  : SLUS-00067",G)
a.txt(16,96,"> NUCLEO  : PCSX REARMED",G)
a.txt(16,108,"> VIDEO   : 320X240 @ 59.94HZ",G)
a.box(60,132,200,64,G); a.box(62,134,196,60,GD)
a.ctxt(146,"CASTLEVANIA",G,None,2)
a.ctxt(172,"PRESSIONE X PARA JOGAR_",G)
a.txt(6,H-10,"TUBO 15KHZ",GD); a.rtxt(W-6,H-10,"X JOGAR",G)
b=termA(1)
b.txt(16,46,"/JOGOS/",G)
y=60
for n,s in enumerate(SYSTEMS):
    if n==7: b.rect(14,y-2,W-28,12,G); b.txt(16,y,"> /%s/"%s,(0,8,2))
    else: b.txt(16,y,"  /%s/"%s,G)
    y+=14
b.txt(6,H-10,"9 PASTAS",GD); b.rtxt(W-6,H-10,"X ABRIR O VOLTAR",G)
pair(a,b,'_temaA_terminal.png',bg=(20,20,20))

# ===================== TEMA B: SUNSET (rosa/ciano, grade) =====================
PK=(255,80,170); CY=(60,230,255); NV=(20,8,40)
def sunsetB(tab):
    c=C(NV)
    for j in range(H):
        t=j/H
        col=(int(30+90*t),int(10+20*t),int(70-20*t)) if j<150 else (16,4,36)
        c.rect(0,j,W,1,col)
    # sol
    for j in range(70,150):
        for i in range(110,210):
            if math.hypot(i-160,j-150)<52 and not (j>110 and (j//4)%2):
                t=(j-98)/52; c.p(i,j,(255,int(200-140*t),int(60+80*t)))
    # grade
    for j in range(150,H):
        if (j-150) in (0,3,7,12,18,26,36,48,62,78): c.rect(0,j,W,1,PK)
    for k in range(-10,11):
        x0=160+k*16; 
        for j in range(150,H):
            x=int(160+(x0-160)*(1+(j-150)/12)); c.p(x,j,(180,40,140))
    # abas: pilulas centralizadas
    c.ctxt(6,"RETROVOLT",CY,(80,0,80),1)
    tw=72; x0=(W-3*tw-16)//2
    c.txt(x0-22,24,"L1",PK); c.txt(x0+3*tw+16+6,24,"R1",PK)
    for n,t in enumerate(TABS):
        x=x0+n*(tw+8)
        if n==tab: c.rect(x+2,20,tw-4,16,CY); c.rect(x,22,tw,12,CY); c.txt(x+(tw-len(t)*8)//2,24,t,NV)
        else: c.rect(x+2,20,tw-4,16,(70,30,110)); c.rect(x,22,tw,12,(70,30,110)); c.txt(x+(tw-len(t)*8)//2,24,t,(200,160,230))
    return c
a=sunsetB(0)
a.rect(40,46,240,150,(14,6,30)); a.box(40,46,240,150,CY); a.box(42,48,236,146,PK)
a.icon(130,58,DISC,CY,6)
a.ctxt(124,"PLAYSTATION",PK,(60,0,40),2)
a.ctxt(146,"CASTLEVANIA SOTN",(255,255,255))
a.ctxt(158,"PCSX REARMED - 240P",(200,160,230))
a.rect(116,174,88,14,PK); a.ctxt(177,"X JOGAR",(255,255,255))
b=sunsetB(1)
b.rect(40,46,240,166,(14,6,30)); b.box(40,46,240,166,CY)
y=54
for n,s in enumerate(SYSTEMS):
    if n==7: b.rect(44,y-3,232,14,PK); b.txt(52,y,"* "+s,(255,255,255))
    else: b.txt(52,y,"  "+s,CY)
    y+=17
pair(a,b,'_temaB_sunset.png',bg=(10,0,20))

# ===================== TEMA C: CARTUCHO CLARO (split lista + detalhe) =====================
BG=(214,210,200); DK=(48,44,60); RD=(200,40,50); PU=(110,80,170); LT=(236,233,225)
def cartC(tab):
    c=C(BG)
    c.rect(0,0,W,34,DK)
    c.txt(8,5,"RETROVOLT",LT); c.rtxt(W-8,5,"12:34",(160,160,170))
    # abas com sublinhado
    x=8; c.txt(x,20,"L1",(160,160,170)); x=32
    for n,t in enumerate(TABS):
        col=LT if n==tab else (130,126,140)
        c.txt(x,20,t,col)
        if n==tab: c.rect(x,30,len(t)*8,3,RD)
        x+=len(t)*8+20
    c.txt(x,20,"R1",(160,160,170))
    # rodape
    c.rect(0,H-14,W,14,DK)
    for i,(k,cc) in enumerate([("X",RD),("O",PU)]): pass
    return c
def btn(c,x,y,k,col,label):
    c.rect(x,y,10,10,col); c.txt(x+1,y+1,k,LT); c.txt(x+14,y+1,label,LT)
a=cartC(0)
a.rect(12,44,140,170,LT); a.box(12,44,140,170,DK)
a.rect(20,52,124,124,DK)
a.icon(42,74,DISC,RD,8)
a.rect(160,44,148,170,LT); a.box(160,44,148,170,DK)
a.txt(168,54,"SISTEMA",PU); a.txt(168,66,"PLAYSTATION",DK)
a.txt(168,84,"JOGO",PU); a.txt(168,96,"CASTLEVANIA",DK); a.txt(168,106,"SOTN",DK)
a.txt(168,124,"CODIGO",PU); a.txt(168,136,"SLUS-00067",DK)
a.txt(168,154,"NUCLEO",PU); a.txt(168,166,"PCSX REARMED",DK)
a.rect(168,186,132,18,RD); a.txt(168+(132-7*8)//2,191,"> JOGAR",LT)
a.txt(20,186,"LEITORA OK",DK); a.txt(20,198,"CD - 240P",DK)
btn(a,6,H-12,"X",RD,"JOGAR"); a.rtxt(W-6,H-11,"TUBO 15KHZ",(160,160,170))
b=cartC(1)
b.rect(12,44,180,170,LT); b.box(12,44,180,170,DK)
y=52
for n,s in enumerate(SYSTEMS):
    if n==7: b.rect(14,y-3,176,14,DK); b.rect(14,y-3,3,14,RD); b.txt(22,y,s,LT)
    else: b.txt(22,y,s,DK)
    y+=18
b.rect(200,44,108,170,DK)
b.icon(226,70,DISC,(255,200,60),5)
b.ctxt(140,"SONY",LT,None,1,254); b.ctxt(152,"PLAYSTATION",(255,200,60),None,1,254)
b.ctxt(170,"1994",(160,160,170),None,1,254)
btn(b,6,H-12,"X",RD,"ABRIR"); btn(b,90,H-12,"O",PU,"VOLTAR")
pair(a,b,'_temaC_cartucho.png',bg=(60,60,70))
