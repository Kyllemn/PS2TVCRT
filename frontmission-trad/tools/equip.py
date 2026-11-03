import json,re,sys,unicodedata,difflib
sys.path.insert(0,'/root/fm1'); from romaji import rom
DS=[n.replace('`n\\','II') for n in json.load(open('/root/fm1/ds_atnames.json')) if re.fullmatch(r"[A-Z0-9][A-Z0-9 .'\-/&II]{1,23}",n.replace('`n\\','II'))]
def z2h(s): return unicodedata.normalize('NFKC',s)
TYPES=[('バズーカ',' BAZOOKA'),('ライフル',' RIFLE'),('グレネード',' GRENADE'),('Ｍランチャー',' M.LAUNCHER'),('Ｒランチャー',' R.LAUNCHER'),('ランチャー',' LAUNCHER'),('シールド',' SHIELD'),('バルカン',' VULCAN')]
def key(s):
    s=s.upper(); return re.sub(r'[^A-Z0-9]','',s.replace('L','R').replace('V','B'))
def jp_key(j):
    out=''
    for ch in j:
        c=z2h(ch)
        if re.match(r'[A-Za-z0-9]',c): out+=c.upper()
        elif ch=='Ⅱ': out+='II'
        elif ch=='Ⅲ': out+='III'
        else: out+=rom(ch).upper()
    return re.sub(r'[^A-Z0-9]','',out.replace('L','R').replace('V','B'))
DK={d:key(d) for d in DS}
def best(jpname):
    k=jp_key(jpname)
    sc=[(difflib.SequenceMatcher(None,k,v).ratio(),d) for d,v in DK.items()]
    sc.sort(reverse=True); return sc[0]
