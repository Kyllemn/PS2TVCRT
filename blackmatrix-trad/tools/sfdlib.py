import struct
def ts(b):
    return (((b[0]>>1)&7)<<30)|((((b[1]<<8)|b[2])>>1)<<15)|(((b[3]<<8)|b[4])>>1)
def packets(d):
    """lista de (scr, sid, cabecalho_bytes, payload, pts, dts, pos)"""
    o=0; scr=None; out=[]
    while o<len(d)-4 and d[o:o+3]==b'\0\0\1':
        sid=d[o+3]
        if sid==0xBA:
            scr=ts(d[o+4:o+9]); o+=12; continue
        if sid==0xB9: o+=4; break
        ln,=struct.unpack('>H',d[o+4:o+6]); body=d[o+6:o+6+ln]
        pts=dts=None; h=0
        if sid in (0xC0,0xE0) or 0xC0<=sid<=0xEF:
            while body[h]==0xff: h+=1
            if body[h]>>6==1: h+=2
            f=body[h]>>4
            if f==2: pts=ts(body[h:h+5]); h+=5
            elif f==3: pts=ts(body[h:h+5]); dts=ts(body[h+5:h+10]); h+=10
            else: h+=1
        out.append((scr,sid,body[:h],body[h:],pts,dts,o)); o+=6+ln
    return out
