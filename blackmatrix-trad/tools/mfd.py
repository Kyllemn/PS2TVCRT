"""Glifos da fonte MFD do Black/Matrix AD: 32x16, 2 bits/pixel, textura
'twiddled' em dois blocos 16x16 lado a lado; 1o pixel = bits 7-6."""
import numpy as np
def _m(x,y):
    r=0
    for i in range(4): r|=((y>>i)&1)<<(2*i)|((x>>i)&1)<<(2*i+1)
    return r
POS=np.array([[(x//16)*256+_m(x%16,y) for x in range(32)] for y in range(16)])
def decode(b):
    b=np.frombuffer(b,dtype=np.uint8)
    p=np.stack([(b>>s)&3 for s in (6,4,2,0)],1).reshape(-1)
    return p[POS]
def encode(a):
    p=np.zeros(512,np.uint8); p[POS]=a
    p=p.reshape(-1,4)
    return bytes(((p[:,0]<<6)|(p[:,1]<<4)|(p[:,2]<<2)|p[:,3]).astype(np.uint8))
