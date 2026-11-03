import sys,re,collections,math
sys.path.insert(0,'/root/fm1'); from sem import jp_set,en_set
from romaji import namesim
sys.path.insert(0,'/root/fm1'); from fmlib import *
def feat_ps1(s):
    s=s.replace('[FD2B]','．').replace('[FD2E]','')
    lines=s.split('\n'); sp=None; body=s
    m=re.match(r'^([^\n：]{1,20})：\n',s)
    if m: sp=m.group(1); body=s[m.end():]
    bl=[l for l in body.split('\n') if l.strip()]
    nch=len(re.sub(r'\[[0-9A-F]{4}\]','',body))
    return dict(sem=jp_set(body),body=body,sp=sp,nl=len(bl),ex=body.count('！'),q=body.count('？'),dots=body.count('‥')+body.count('・'),n=nch,
                fc=re.findall(r'\[(F[C-F][0-9A-F]{2})\]',body))
def feat_ds(s):
    sp=None; body=s
    m=re.match(r'^([^\n:]{1,24}):\n',s)
    if m: sp=m.group(1); body=s[m.end():]
    bl=[l for l in body.split('\n') if l.strip()]
    nch=len(re.sub(r'\[[0-9A-F]{4}\]','',body))
    return dict(sem=en_set(body),body=body,sp=sp,nl=len(bl),ex=body.count('!'),q=body.count('?'),dots=body.count('...'),n=nch,
                fc=re.findall(r'\[(F[C-F][0-9A-F]{2})\]',body))
def sim(a,b,names):
    s=0.0
    if a['sp'] or b['sp']:
        if a['sp'] and b['sp']:
            if a['sp'].startswith('[FC') or b['sp'].startswith('[FC'):
                s+= 3 if a['sp']==b['sp'] else -2
            else:
                m=names.get(a['sp'])
                if m: s+= 3 if m==b['sp'] else -4
                else:
                    ns=namesim(a['sp'],b['sp'])
                    s+= 2.5 if ns>=0.75 else (0.3 if ns>=0.5 else -1.5)
        else: s-=2
    s+= 1.0 if a['nl']==b['nl'] else -0.3*abs(a['nl']-b['nl'])
    s+= 0.6 if (a['ex']>0)==(b['ex']>0) else -0.6
    s+= 0.6 if (a['q']>0)==(b['q']>0) else -0.6
    s+= 0.3 if (a['dots']>0)==(b['dots']>0) else -0.2
    for jp,en in names.items():
        if len(jp)>=2 and jp in a.get('body',''):
            s+= 1.2 if en.split()[0] in b.get('body','') else -0.3
    k=len(a['sem'] & b['sem'])
    s+= min(k,4)*0.9 - (0.6 if (k==0 and len(b['sem'])>=3 and len(a['sem'])>=3) else 0)
    if a['n'] and b['n']:
        r=b['n']/a['n']; s+= 0.8 if 1.2<=r<=3.5 else -0.8
    return s
def nw(P,D,names,gap=-1.0):
    n,m=len(P),len(D)
    S=[[0.0]*(m+1) for _ in range(n+1)]; Bk=[[0]*(m+1) for _ in range(n+1)]
    for i in range(1,n+1): S[i][0]=S[i-1][0]+gap; Bk[i][0]=1
    for j in range(1,m+1): S[0][j]=S[0][j-1]+gap; Bk[0][j]=2
    for i in range(1,n+1):
        for j in range(1,m+1):
            c=S[i-1][j-1]+sim(P[i-1],D[j-1],names)
            u=S[i-1][j]+gap; l=S[i][j-1]+gap
            if c>=u and c>=l: S[i][j]=c; Bk[i][j]=0
            elif u>=l: S[i][j]=u; Bk[i][j]=1
            else: S[i][j]=l; Bk[i][j]=2
    i,j=n,m; pairs=[]
    while i>0 or j>0:
        b=Bk[i][j]
        if i>0 and j>0 and b==0: pairs.append((i-1,j-1)); i-=1; j-=1
        elif i>0 and (j==0 or b==1): i-=1
        else: j-=1
    return pairs[::-1]
def learn(P,D,pairs):
    c=collections.defaultdict(collections.Counter)
    for i,j in pairs:
        a,b=P[i]['sp'],D[j]['sp']
        if a and b and not a.startswith('[') and not b.startswith('['): c[a][b]+=1
    return {k:v.most_common(1)[0][0] for k,v in c.items() if v.most_common(1)[0][1]>=2}
