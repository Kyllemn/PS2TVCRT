"""teste da CONVERGENCIA pelo caminho real do controle (sem Pi): python3 teste_conv.py PASTA_DO_MENU SAIDA"""
import os, sys, shutil
app_dir, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
os.environ['RV_FAKEFB'] = os.path.join(out, 'x.png') + ':1920x240'
sys.path.insert(0, app_dir)
import rv_app
a = rv_app.App()
falhas = []
n = [0]
def tela(nome):
    a.fb.fake = os.path.join(out, '%02d_%s.png' % (n[0], nome)); n[0] += 1
    if hasattr(a, 'mostra_seguro'):
        a.mostra_seguro()
    else:
        a.fb.show(a.desenha(), a.conv)
def aperta(b):
    (a.acao_segura if hasattr(a, 'acao_segura') else a.acao)(b)
    if hasattr(a.leitora, 'quieta'):
        a.leitora.quieta = a.tela == 'conv'
try:
    for _ in range(3): aperta('R1')
    assert a.aba == 'OPCOES', a.aba
    while a.opcoes()[a.sel['OPCOES']] != 'CONVERGENCIA': aperta('BAIXO')
    tela('menu_opcoes')
    aperta('A')
    print('tela depois do A:', a.tela)
    tela('conv_entrou')
    if a.tela != 'conv': falhas.append('NAO ENTROU na convergencia (tela=%s)' % a.tela)
    if getattr(a.leitora, 'quieta', None) is not True: falhas.append('leitora nao ficou quieta')
    c0 = list(a.conv)
    for b, campo, sinal in (('DIR', 0, 1), ('ESQ', 0, -1), ('BAIXO', 1, 1), ('CIMA', 1, -1),
                            ('R1', 2, 1), ('L1', 2, -1), ('R2', 3, 1), ('L2', 3, -1)):
        antes = a.conv[campo]; aperta(b); depois = a.conv[campo]
        ok = (depois - antes) * sinal > 0
        print('%-5s %s: %s -> %s %s' % (b, 'XYWH'[campo], antes, depois, 'ok' if ok else 'FALHOU'))
        if not ok: falhas.append('botao %s nao mexeu' % b)
    aperta('DIR'); aperta('DIR'); aperta('R1')
    tela('conv_ajustado')
    aperta('A')
    cfg = rv_app.cfg_ler()
    print('salvo no cfg: x=%s largura=%s' % (cfg['video_posicao_x'], cfg['video_largura']))
    if int(cfg['video_posicao_x']) != a.conv[0]: falhas.append('A nao salvou')
    tela('conv_salvo')
    aperta('SELECT'); tela('conv_padrao')
    if a.conv != [0, 0, 1.0, 1.0]: falhas.append('SELECT nao voltou ao padrao: %s' % a.conv)
    aperta('B')
    print('tela depois do B:', a.tela, a.conv)
    if a.tela != 'menu': falhas.append('B nao voltou ao menu')
    if getattr(a.leitora, 'quieta', None) is not False: falhas.append('leitora nao voltou a funcionar')
    tela('menu_volta')
except Exception as ex:
    import traceback; traceback.print_exc()
    falhas.append('EXCECAO: %r' % ex)
print('RESULTADO:', 'PASSOU' if not falhas else 'FALHOU -> ' + '; '.join(falhas))
