"""Leitura de mensagens PS1 (japones) e DS (ingles) em tokens comparaveis."""
import struct,re
OCR={}
for l in open('/root/fm1/ocr2.tsv',encoding='utf-8'):
    i,c,s=l.rstrip('\n').split('\t'); OCR[int(i)]=c
OCR.update({38:'？',34:'！',28:'。',33:'、',27:'・',32:'：',9:'ー'})
ARG1={0xfc,0xf8,0xfa,0xfb,0xf9,0xf7,0xf6,0xf5,0xf4,0xf3,0xf2,0xf1,0xf0,0xfd}
def ps1_tokens(d,i):
    """le mensagem japonesa em d[i:] ate FF; devolve (lista de tokens, fim)"""
    t=[]
    while i<len(d):
        b=d[i]
        if b==0xff: return t,i+1
        if b==0xfe: t.append(('NL',)); i+=1; continue
        if b>=0xf0: t.append(('C',b,d[i+1] if i+1<len(d) else 0)); i+=2; continue
        if 0xc0<=b<=0xcf: t.append(('G',0xc0+(b-0xc0)*0xc0+d[i+1])); i+=2; continue
        t.append(('G',b)); i+=1
    return t,i
def ps1_str(t):
    s=''
    for x in t:
        if x[0]=='NL': s+='\n'
        elif x[0]=='C': s+='[%02X%02X]'%(x[1],x[2])
        else: s+=OCR.get(x[1],'〓')
    return s
DSM={0x50:' ',0x57:"'",0x5c:',',0x5e:'.',0x4f:'?',0x51:'!',0x4a:':'}
def ds_tokens(d,i):
    t=[]
    while i<len(d):
        b=d[i]
        if b==0xff: return t,i+1
        if b==0xfe: t.append(('NL',)); i+=1; continue
        if b>=0xf0: t.append(('C',b,d[i+1])); i+=2; continue
        t.append(('G',b)); i+=1
    return t,i
def ds_char(b):
    if b<=0x19: return chr(b+65)
    if b<=0x33: return chr(b+71)
    return DSM.get(b,'[%02X]'%b)
def ds_str(t):
    s=''
    for x in t:
        if x[0]=='NL': s+='\n'
        elif x[0]=='C': s+='[%02X%02X]'%(x[1],x[2])
        else: s+=ds_char(x[1])
    return s
