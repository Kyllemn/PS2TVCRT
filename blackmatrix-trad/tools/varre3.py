import sys,re,os
sys.path.insert(0,'bm_tools'); import pvr
COM=set('のをにはがたてしますでといるかなっれりんこアイクスラトンーッルリ')
FW=rb'(?:[\x81-\x84\x88-\x9f\xe0-\xea][\x40-\x7e\x80-\xfc])+'
def ok(t):
    n=len(t)
    if n<3: return False
    kk=sum('ぁ'<=c<='ヿ' for c in t); kj=sum('一'<=c<='鿿' for c in t)
    cm=sum(c in COM for c in t)
    return cm>=2 and (kk+kj)/n>=0.7 and kk/n>=0.3
def limpa(d):
    d=bytearray(d)
    for m in re.finditer(b'PVRT',d):
        o=m.start()
        try: pf,df,w,h=pvr.head(d,o)
        except: continue
        if w in (8,16,32,64,128,256,512,1024) and h in (8,16,32,64,128,256,512,1024):
            d[o+16:o+16+w*h*2]=b'\0'*min(w*h*2,len(d)-o-16)
    return bytes(d)
for f in sys.argv[1:]:
    d=limpa(open(f,'rb').read()); b=os.path.basename(f)
    for al in (0,1):
        e=d[al:]; e=e[:len(e)&~1]; s=bytearray(len(e)); s[0::2]=e[1::2]; s[1::2]=e[0::2]
        for modo,buf in (('BE',e),('LE',bytes(s))):
            for m in re.finditer(FW,buf):
                if m.start()%2: continue
                try: t=m.group().decode('shift_jis')
                except: continue
                if ok(t): print('%s\t%s\t%x\t%s'%(b,modo,al+m.start(),t))
    # meia largura: >=5 seguidos, terminando em 0 ou espaco
    for m in re.finditer(rb'[\xa6-\xdf]{5,}(?=[\x00\x20\x81])',d):
        t=m.group().decode('shift_jis','replace')
        if sum(c in 'ｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾗﾘﾙﾚﾛﾝｰﾞ' for c in t)>=len(t)*0.8: print('%s\tHW\t%x\t%s'%(b,m.start(),t))
