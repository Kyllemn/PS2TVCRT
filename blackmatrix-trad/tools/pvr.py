"""Le/grava texturas PVRT do Dreamcast (ARGB1555=0, RGB565=1, ARGB4444=2; twiddled=1, retangular=9)."""
import struct, numpy as np
def _tw(n):
    t=np.zeros(n,dtype=np.int64)
    for i in range(n):
        r=0
        for b in range(12): r|=((i>>b)&1)<<(2*b)
        t[i]=r
    return t
def idx(w,h,df):
    ys,xs=np.mgrid[0:h,0:w]
    if df==9: return ys*w+xs
    m=min(w,h); tw=_tw(max(w,h))
    blk=(xs//m)*(m*m) if w>h else (ys//m)*(m*m)
    return blk+(tw[ys%m]|(tw[xs%m]<<1))
def head(b,o):
    sz,pf,df,_,w,h=struct.unpack('<IBBHHH',b[o+4:o+16]); return pf,df,w,h
def decode(b,o):
    pf,df,w,h=head(b,o)
    px=np.frombuffer(b[o+16:o+16+w*h*2],dtype='<u2')[idx(w,h,df)].astype(np.int64)
    if pf==0: a=(px>>15)*255; r=((px>>10)&31)*255//31; g=((px>>5)&31)*255//31; bl=(px&31)*255//31
    elif pf==1: a=np.full_like(px,255); r=((px>>11)&31)*255//31; g=((px>>5)&63)*255//63; bl=(px&31)*255//31
    else: a=((px>>12)&15)*17; r=((px>>8)&15)*17; g=((px>>4)&15)*17; bl=(px&15)*17
    return np.dstack([r,g,bl,a]).astype(np.uint8)
def encode(img,pf,df):
    h,w,_=img.shape; im=img.astype(np.int64); r,g,bl,a=[im[...,i] for i in range(4)]
    if pf==0: px=((a>=128)<<15)|((r*31+127)//255<<10)|((g*31+127)//255<<5)|((bl*31+127)//255)
    elif pf==1: px=((r*31+127)//255<<11)|((g*63+127)//255<<5)|((bl*31+127)//255)
    else: px=((a*15+127)//255<<12)|((r*15+127)//255<<8)|((g*15+127)//255<<4)|((bl*15+127)//255)
    out=np.zeros(w*h,dtype='<u2'); out[idx(w,h,df).ravel()]=px.ravel(); return out.tobytes()
