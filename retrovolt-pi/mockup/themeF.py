import sys
exec(open(sys.argv[2]).read().split("# ===================== D: VIDRO")[0].replace("exec(open(sys.argv[2]).read()","exec(open(sys.argv[3]).read()"))
OUT=sys.argv[1]
NAV=(12,18,40); OR=(255,140,40); IV=(240,236,225); SL=(120,130,160)
def editF(tab, foot_r):
    c=C(NAV)
    c.rect(0,0,6,H,OR); c.rect(W-6,0,6,H,OR)
    c.txt(16,6,"RETROVOLT",IV); c.rtxt(W-12,6,"12:34",IV)
    c.rtxt(W-12,17,"47C  CPU 12%",(95,105,140))
    c.rtxt(W-12,27,"RAM 1.2/4GB",(95,105,140))
    c.rtxt(W-12,37,"USB 41% USADO",(95,105,140))
    x=16
    for n,t in enumerate(TABS):
        on=(n==tab)
        c.txt(x,28,t,IV if on else SL)
        if on: c.rect(x,38,len(t)*8,2,OR)
        x+=len(t)*8+20
    c.rect(16,48,W-32,1,(40,50,80))
    c.txt(16,H-12,"L1/R1 ABAS",SL); c.rtxt(W-12,H-12,foot_r,SL)
    return c
def save(img,name): img.convert('RGB').save(OUT+name)
# DISCO
a=editF(0,"TUBO 15KHZ 240P")
a.txt(134,66,"DISCO NA LEITORA",OR)
a.txt(134,82,"PLAYSTATION",IV,None,2)
a.txt(134,104,"CASTLEVANIA",IV); a.txt(134,114,"SYMPHONY OF",IV); a.txt(134,124,"THE NIGHT",IV)
a.txt(134,142,"SLUS-00067",SL); a.txt(134,152,"PCSX REARMED",SL)
a.rect(134,172,120,18,OR); a.txt(142,177,"X  JOGAR  ->",NAV)
A=up(a); put_disc(A,68,128,52,0.15); save(A,'_F1_disco.png')
# JOGOS
b=editF(1,"X ABRIR  O VOLTAR")
y=60
for n,s in enumerate(SYSTEMS):
    on=(n==7)
    b.txt(24,y,s,IV if on else SL)
    if on: b.rect(16,y-1,3,10,OR); b.rect(24,y+10,len(s)*8,1,OR); b.txt(24+len(s)*8+8,y,"->",OR)
    y+=18
save(up(b),'_F2_jogos.png')
# PASTA
p=editF(1,"X JOGAR  O VOLTAR")
p.txt(24,56,"PLAYSTATION",OR)
roms=["APE ESCAPE","CASTLEVANIA SOTN","CRASH BANDICOOT","FINAL FANTASY VII","GRAN TURISMO 2","METAL GEAR SOLID","RESIDENT EVIL 2","SILENT HILL","TEKKEN 3"]
y=74
for n,s in enumerate(roms):
    on=(n==1)
    p.txt(24,y,s,IV if on else SL)
    if on: p.rect(16,y-1,3,10,OR); p.rect(24,y+10,len(s)*8,1,OR); p.txt(24+len(s)*8+8,y,"->",OR)
    y+=16
p.txt(W-60,56,"2/23",SL)
save(up(p),'_F3_pasta.png')
# OPCOES
o=editF(2,"X ENTRAR  O VOLTAR")
opts=[("VOLUME GERAL","8/10"),("VIDEO","TUBO 15KHZ"),("CONVERGENCIA",""),("CONTROLES",""),("WI-FI","CASA_5G"),("LEITORA","ASUS 08D1S"),("PASTAS DE JOGOS","USB"),("SISTEMA",""),("REINICIAR",""),("DESLIGAR","")]
y=60
for n,(k,v) in enumerate(opts):
    on=(n==2)
    col=IV if on else (SL if n<8 else (230,110,90))
    o.txt(24,y,k,col)
    if v: o.rtxt(W-14,y,v,OR if on else (170,150,120))
    if on: o.rect(16,y-1,3,10,OR); o.rect(24,y+10,len(k)*8,1,OR); o.txt(24+len(k)*8+8,y,"->",OR)
    y+=16
save(up(o),'_F4_opcoes.png')
