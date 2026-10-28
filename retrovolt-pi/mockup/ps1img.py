import sys
from PIL import Image, ImageDraw, ImageFilter
K=4; Wd,Hd=360,408
S=Image.new('RGBA',(Wd*K,Hd*K),(0,0,0,0)); d=ImageDraw.Draw(S)
def rr(b,r,f,o=None,w=0): d.rounded_rectangle([v*K for v in b],r*K,fill=f,outline=o,width=w*K)
def el(b,f,o=None,w=0): d.ellipse([v*K for v in b],fill=f,outline=o,width=w*K)
# ---------------- console (vista de cima, levemente inclinada)
rr((14,22,346,228),16,(120,120,128))            # sombra/base
rr((10,14,350,214),16,(206,206,212))            # tampo
rr((10,186,350,226),14,(160,160,168))           # frente
rr((10,180,350,192),6,(186,186,194))
# ranhuras do tampo (lado direito)
for i in range(8): rr((250,40+i*9,334,43+i*9),2,(190,190,198))
# tampa do CD
el((40,30,230,206),(176,176,184))
el((46,36,224,200),(196,196,204))
el((60,50,210,186),(204,204,212),(182,182,190),2)
el((120,110,150,140),(188,188,196))
# botoes
el((262,128,292,158),(150,150,158)); el((264,130,290,156),(186,186,194))     # reset
el((302,128,332,158),(150,150,158)); el((304,130,330,156),(186,186,194))     # power
el((306,164,314,172),(60,220,90))                                           # led
rr((238,160,296,176),6,(150,150,158)); rr((240,162,294,174),5,(186,186,194)) # open
# portas da frente
for x in (70,200):
    rr((x,195,x+70,218),4,(70,70,78)); rr((x+6,200,x+64,213),3,(30,30,34))
    rr((x+10,188,x+60,193),2,(110,110,118))      # slot do memory card
# ---------------- controle
cy=262
el((40,cy+40,140,cy+138),(176,176,184)); el((220,cy+40,320,cy+138),(176,176,184))   # punhos
rr((60,cy,300,cy+92),40,(196,196,204))
el((44,cy+44,136,cy+132),(196,196,204)); el((224,cy+44,316,cy+132),(196,196,204))
rr((70,cy-8,120,cy+6),6,(150,150,158)); rr((240,cy-8,290,cy+6),6,(150,150,158))     # L/R
# direcional
el((70,cy+16,140,cy+86),(184,184,192))
rr((98,cy+26,112,cy+76),3,(80,80,88)); rr((80,cy+44,130,cy+58),3,(80,80,88))
# botoes de acao (cores, sem simbolos)
el((220,cy+16,290,cy+86),(184,184,192))
for (x,y,c) in ((255,cy+30,(70,200,140)),(275,cy+51,(230,90,90)),(255,cy+72,(110,150,230)),(235,cy+51,(220,140,200))):
    el((x-9,y-9,x+9,y+9),(70,70,78)); el((x-7,y-7,x+7,y+7),c)
# start/select
rr((160,cy+50,176,cy+56),3,(90,90,98)); rr((186,cy+50,202,cy+56),3,(90,90,98))
# cabo
d.line([(180*K,cy*K),(180*K,(cy-12)*K)],fill=(60,60,66),width=4*K)
img=S.resize((Wd,Hd),Image.LANCZOS)
# sombra suave
sh=Image.new('RGBA',(Wd+20,Hd+20),(0,0,0,0)); sh.paste((0,0,0,120),(10,16),img)
sh=sh.filter(ImageFilter.GaussianBlur(8)); sh.alpha_composite(img,(6,4))
sh.save(sys.argv[1])
