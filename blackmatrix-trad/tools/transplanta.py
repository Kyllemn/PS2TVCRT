"""Leva as traducoes de um arquivo do disco 1 para o mesmo arquivo do disco 2, por conteudo:
cada trecho alterado (original_d1 -> traduzido_d1) e procurado no original do disco 2 com uma margem de contexto;
se o trecho original aparece no disco 2, e trocado pelo traduzido (todas as ocorrencias).
uso: transplanta.py orig_d1 trad_d1 orig_d2 saida_d2 [relatorio]"""
import sys, numpy as np
o1, t1, o2, out = sys.argv[1:5]; rel = sys.argv[5] if len(sys.argv) > 5 else None
A = open(o1, 'rb').read(); T = open(t1, 'rb').read(); B = bytearray(open(o2, 'rb').read())
assert len(A) == len(T)
a = np.frombuffer(A, np.uint8); t = np.frombuffer(T, np.uint8)
dif = np.flatnonzero(a != t)
spans = []
if len(dif):
    s = p = int(dif[0])
    for x in dif[1:]:
        x = int(x)
        if x - p > 512: spans.append((s, p + 1)); s = x
        p = x
    spans.append((s, p + 1))
CTX = 64; ok = 0; falta = []; linhas = []
for s, e in spans:
    lo = max(0, s - CTX); hi = min(len(A), e + CTX)
    chave = A[lo:hi]; achou = 0; pos = B.find(chave)
    while pos >= 0:
        B[pos:pos + (hi - lo)] = T[lo:hi]; achou += 1
        pos = B.find(chave, pos + 1)
    linhas.append('%x-%x (%d bytes): %d no disco 2' % (s, e, e - s, achou))
    if achou: ok += 1
    else: falta.append((s, e))
open(out, 'wb').write(bytes(B))
print('trechos traduzidos no d1:', len(spans), '| levados ao d2:', ok, '| sem par no d2:', len(falta))
if rel: open(rel, 'w').write('\n'.join(linhas) + '\n')
