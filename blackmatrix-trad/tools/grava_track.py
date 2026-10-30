"""Grava arquivos novos no track03 (mesmo lugar e tamanho em setores)."""
import sys, os, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import cdsec
track, lista, saida = sys.argv[1:4]; trocas=sys.argv[4:]   # NOME=arquivo
BASE=45000
pos={}
for line in open(lista):
    p=line.split()
    if len(p)==3 and p[0].startswith('/'): pos[p[0][1:]]=(int(p[1]),int(p[2]))
shutil.copyfile(track,saida)
f=open(saida,'r+b')
for t in trocas:
    nome,arq=t.split('='); lba,size=pos[nome]; data=open(arq,'rb').read()
    assert (len(data)+2047)//2048 <= (size+2047)//2048, 'nao cabe no lugar'
    assert len(data)==size, 'tamanho mudou: precisa atualizar o diretorio'
    data+=b'\0'*((-len(data))%2048)
    for i in range(len(data)//2048):
        f.seek((lba-BASE+i)*2352); f.write(cdsec.mode1(lba+i,data[i*2048:(i+1)*2048]))
    print(nome,'gravado em',lba)
f.close()
