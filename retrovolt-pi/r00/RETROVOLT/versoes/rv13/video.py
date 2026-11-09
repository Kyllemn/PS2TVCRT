"""RETROVOLT Pi - deteccao automatica TUBO / LCD.

O boot do Pi decide a saida de video pelo cmdline.txt:
  TUBO: EDID propria do RetroVolt (240p/288p/480i, 15 kHz) + video=1920x240.
  LCD : sem EDID propria; video= no modo escolhido para o menu (perto de 720p).
Ao ligar, o RetroVolt le a EDID VERDADEIRA do aparelho ligado:
  - no modo LCD: pelo proprio kernel (/sys/class/drm/.../edid);
  - no modo TUBO (a EDID do kernel e a nossa): pelo fio DDC do HDMI (i2c).
Se o aparelho nao combina com o modo atual, troca o cmdline.txt e reinicia.
Tubo = adaptador AGO (fabricante AGO, produto 0001). Sem EDID legivel: nao mexe.
"""
import os
import subprocess
import time

CMDLINE = '/boot/firmware/cmdline.txt'
EDID_TUBO = 'drm.edid_firmware=HDMI-A-1:edid/retrovolt-crt.bin,HDMI-A-2:edid/retrovolt-crt.bin'
VIDEO_TUBO = 'video=HDMI-A-1:1920x240@60e'
TROCAS = '/var/lib/retrovolt/video_trocas'

VIC = {1: (640, 480, 60), 2: (720, 480, 60), 3: (720, 480, 60), 4: (1280, 720, 60), 16: (1920, 1080, 60),
       17: (720, 576, 50), 18: (720, 576, 50), 19: (1280, 720, 50), 31: (1920, 1080, 50),
       34: (1920, 1080, 30), 63: (1920, 1080, 120), 97: (3840, 2160, 60), 95: (3840, 2160, 30)}


# ------------------------------------------------------------------ leitura da EDID
def _dtd(b):
    pclk = (b[0] | b[1] << 8) * 10000
    if not pclk:
        return None
    ha = b[2] | (b[4] & 0xF0) << 4
    hb = b[3] | (b[4] & 0x0F) << 8
    va = b[5] | (b[7] & 0xF0) << 4
    vb = b[6] | (b[7] & 0x0F) << 8
    entrel = bool(b[17] & 0x80)
    hz = pclk / ((ha + hb) * (va + vb)) if (ha + hb) and (va + vb) else 0
    if entrel:
        hz *= 2
    return (ha, va * (2 if entrel else 1), round(hz, 2), entrel)


def analisa(e):
    """dict com fabricante, produto, nome, preferido e lista de modos (w, h, hz)"""
    if not e or len(e) < 128 or e[:8] != b'\x00\xff\xff\xff\xff\xff\xff\x00':
        return None
    if sum(e[:128]) % 256:          # soma de verificacao errada: leitura ruim, nao confia
        return None
    m = e[8] << 8 | e[9]
    fab = ''.join(chr(((m >> s) & 31) + 64) for s in (10, 5, 0))
    prod = e[10] | e[11] << 8
    nome = None
    modos, pref = [], None
    for off in (54, 72, 90, 108):
        b = e[off:off + 18]
        if b[0] or b[1]:
            d = _dtd(b)
            if d and not d[3]:
                modos.append(d[:3])
                if pref is None:
                    pref = d[:3]
        elif b[3] == 0xFC:
            nome = b[5:18].split(b'\n')[0].decode('ascii', 'replace').strip() or None
    asp = {0: (16, 10), 1: (4, 3), 2: (5, 4), 3: (16, 9)}
    for i in range(38, 54, 2):
        if e[i] == 1 and e[i + 1] == 1:
            continue
        w = (e[i] + 31) * 8
        a, b2 = asp[e[i + 1] >> 6]
        modos.append((w, w * b2 // a, (e[i + 1] & 0x3F) + 60))
    est = [(35, 5, (640, 480, 60)), (35, 0, (800, 600, 60)), (36, 3, (1024, 768, 60))]
    for byte, bit, md in est:
        if e[byte] >> bit & 1:
            modos.append(md)
    for k in range(1, e[126] + 1):          # extensoes CEA
        x = e[128 * k:128 * (k + 1)]
        if len(x) < 128 or x[0] != 2:
            continue
        fim = x[2]
        i = 4
        while i < fim:
            tag, n = x[i] >> 5, x[i] & 31
            if tag == 2:
                for v in x[i + 1:i + 1 + n]:
                    if (v & 0x7F) in VIC:
                        modos.append(VIC[v & 0x7F])
            i += n + 1
        o = fim
        while o + 18 <= 127 and (x[o] or x[o + 1]):
            d = _dtd(x[o:o + 18])
            if d and not d[3]:
                modos.append(d[:3])
            o += 18
    vistos, uniq = set(), []
    for md in modos:
        k = (md[0], md[1], round(md[2]))
        if k not in vistos:
            vistos.add(k)
            uniq.append(md)
    return {'fab': fab, 'prod': prod, 'nome': nome, 'pref': pref, 'modos': uniq}


def modo_atual():
    try:
        return 'tubo' if 'retrovolt-crt.bin' in open('/proc/cmdline').read() else 'lcd'
    except OSError:
        return 'tubo'


def _conector():
    for c in sorted(os.listdir('/sys/class/drm')):
        if '-HDMI-A-' in c:
            try:
                if open('/sys/class/drm/%s/status' % c).read().strip() == 'connected':
                    return c
            except OSError:
                pass
    return None


def _ddc(conector):
    """EDID pelo fio DDC: HDMI-A-1 = controlador ...508200, HDMI-A-2 = ...508280"""
    alvo = '508280' if conector and conector.endswith('HDMI-A-2') else '508200'
    subprocess.run(['modprobe', 'i2c-dev'], timeout=5)
    for b in sorted(os.listdir('/sys/bus/i2c/devices')):
        if not b.startswith('i2c-'):
            continue
        try:
            nome = open('/sys/bus/i2c/devices/%s/name' % b).read()
        except OSError:
            continue
        if alvo not in nome:
            continue
        n = b.split('-')[1]
        out = b''
        for off in ('0x00', '0x80'):
            r = subprocess.run(['i2ctransfer', '-y', n, 'w1@0x50', off, 'r128'],
                               capture_output=True, text=True, timeout=5)
            if r.returncode:
                break
            out += bytes(int(v, 16) for v in r.stdout.split())
            if off == '0x00' and (len(out) < 128 or out[126] == 0):
                break
        if len(out) >= 128:
            return out
    return None


def edid_verdadeira():
    """(bytes, de_onde) ou (None, motivo)"""
    con = _conector()
    if modo_atual() == 'lcd' and con:
        try:
            e = open('/sys/class/drm/%s/edid' % con, 'rb').read()
            if len(e) >= 128:
                return e, 'kernel'
        except OSError:
            pass
    try:
        e = _ddc(con)
        if e:
            return e, 'ddc'
    except Exception as ex:
        return None, 'ddc erro %s' % ex
    return None, 'sem edid'


def classifica(info):
    if not info:
        return None
    if info['fab'] == 'AGO' and info['prod'] == 0x0001:
        return 'tubo'
    if info['fab'] == 'RTV':           # a nossa propria (DDC devolveu a do kernel?): nao decide
        return None
    return 'lcd'


def escolhe_menu(info):
    """modo do LCD: a resolucao NATIVA do monitor (o modo preferido da EDID), que e a que
    o monitor mostra sem esticar nem deslocar (1280x720 num monitor 1440x900 saiu cortado).
    Reserva: 1280x720; depois o modo mais perto de 720 linhas."""
    pref = info.get('pref')
    if pref and 600 <= pref[1] <= 1200 and pref[0] >= pref[1] and 48 <= pref[2] <= 76:
        return pref[:2]
    modos = [m for m in info['modos'] if 55 <= m[2] <= 76]
    for w, h, hz in modos:
        if (w, h) == (1280, 720):
            return (1280, 720)
    cand = [m for m in modos if 576 <= m[1] <= 900 and m[0] >= m[1]]
    if cand:
        cand.sort(key=lambda m: (abs(m[1] - 720), -(m[0] / m[1]), abs(m[2] - 60)))
        return cand[0][:2]
    if pref:
        return pref[:2]
    return (1280, 720)


# ------------------------------------------------------------------ troca do boot
def cmdline_para(saida, modo_lcd=None):
    cmd = open(CMDLINE).read().strip().split()
    cmd = [t for t in cmd if not t.startswith('drm.edid_firmware=') and not t.startswith('video=HDMI-A-1:')]
    if saida == 'tubo':
        cmd += [EDID_TUBO, VIDEO_TUBO]
    else:
        w, h = modo_lcd or (1280, 720)
        cmd += ['video=HDMI-A-1:%dx%d@60' % (w, h)]
    return ' '.join(cmd)


def pode_trocar():
    """no maximo 2 trocas em 10 minutos (evita ficar reiniciando sem parar)"""
    agora = time.time()
    try:
        ts = [float(x) for x in open(TROCAS).read().split()]
    except (OSError, ValueError):
        ts = []
    ts = [t for t in ts if agora - t < 600]
    return len(ts) < 2, ts


def troca(saida, modo_lcd=None):
    if not precisa_trocar(saida, modo_lcd):
        print('video: boot ja esta em %s: nada a trocar' % saida.upper(), flush=True)
        return False
    ok, ts = pode_trocar()
    if not ok:
        print('video: trocas demais em pouco tempo: nao troca agora', flush=True)
        return False
    if not precisa_trocar(saida, modo_lcd):
        return False
    novo = cmdline_para(saida, modo_lcd)
    antes = open(CMDLINE).read()
    with open(CMDLINE + '.rv-anterior', 'w') as f:
        f.write(antes)
    with open(CMDLINE + '.tmp', 'w') as f:
        f.write(novo + '\n')
        f.flush()
        os.fsync(f.fileno())
    os.replace(CMDLINE + '.tmp', CMDLINE)
    os.makedirs(os.path.dirname(TROCAS), exist_ok=True)
    with open(TROCAS, 'w') as f:
        f.write(' '.join(str(t) for t in ts + [time.time()]))
    os.sync()
    print('video: boot trocado para %s: %s' % (saida.upper(), novo), flush=True)
    return True


def _le_confirmada():
    """le a EDID ate 4 vezes (1 s entre elas) e so aceita com 2 leituras validas iguais"""
    leituras = []
    onde = 'sem edid'
    for _ in range(4):
        e, onde = edid_verdadeira()
        info = analisa(e)
        if info:
            chave = (info['fab'], info['prod'], info['nome'], info['pref'])
            if any(k == chave for k, _ in leituras):
                return e, onde + ' (confirmada)'
            leituras.append((chave, e))
        time.sleep(1)
    return None, onde + ' (sem 2 leituras iguais)'


def _tokens_video():
    try:
        t = open(CMDLINE).read().split()
    except OSError:
        return set()
    return {x for x in t if x.startswith('drm.edid_firmware=') or x.startswith('video=HDMI-A-1:')}


def precisa_trocar(saida, modo_lcd=None):
    """compara so os parametros de video do boot (a ordem nao importa)"""
    queria = {x for x in cmdline_para(saida, modo_lcd).split()
              if x.startswith('drm.edid_firmware=') or x.startswith('video=HDMI-A-1:')}
    return queria != _tokens_video()


TESTE = '/var/lib/retrovolt/video_teste'      # resolucao nova esperando confirmacao


def modos_lcd():
    """resolucoes que o monitor aceita (para OPCOES > VIDEO > RESOLUCAO), da maior para
    a menor: progressivas, 48-76 Hz, de 480 a 1200 linhas"""
    e, _ = edid_verdadeira()
    info = analisa(e)
    if not info:
        return []
    vistos, out = set(), []
    for w, h, hz in info['modos']:
        if 480 <= h <= 1080 and w >= h and 48 <= hz <= 76 and (w, h) not in vistos:
            vistos.add((w, h))
            out.append((w, h))
    return sorted(out, key=lambda m: (-m[1], -m[0]))


def testa_modo(modo, nome='AUTO'):
    """troca para a resolucao escolhida guardando a anterior: se ninguem confirmar
    depois do boot (monitor sem imagem), o RetroVolt volta sozinho. A escolha so vira
    definitiva (configuracao) quando a pessoa confirma: nome fica guardado no teste."""
    antes = open(CMDLINE).read()
    if not precisa_trocar('lcd', modo):
        return False
    os.makedirs(os.path.dirname(TESTE), exist_ok=True)
    with open(TESTE, 'w') as f:
        f.write('#RVMODO %s\n' % nome)
        f.write(antes)
        f.flush()
        os.fsync(f.fileno())
    try:
        os.remove(TROCAS)
    except OSError:
        pass
    return troca('lcd', modo)


def confirma_teste():
    """apaga o teste e devolve a resolucao escolhida ('AUTO' ou 'LxA') para gravar"""
    nome = 'AUTO'
    try:
        l = open(TESTE).readline()
        if l.startswith('#RVMODO '):
            nome = l.split(None, 1)[1].strip() or 'AUTO'
        os.remove(TESTE)
    except OSError:
        pass
    os.sync()
    return nome


def desfaz_teste():
    """volta o cmdline de antes do teste (o reinicio fica por conta de quem chamou)"""
    try:
        antes = open(TESTE).read()
    except OSError:
        return False
    if antes.startswith('#RVMODO '):
        antes = antes.split('\n', 1)[1] if '\n' in antes else ''
    if 'root=' not in antes:            # teste estragado: nao mexe no boot
        os.remove(TESTE)
        return False
    with open(CMDLINE + '.tmp', 'w') as f:
        f.write(antes)
        f.flush()
        os.fsync(f.fileno())
    os.replace(CMDLINE + '.tmp', CMDLINE)
    os.remove(TESTE)
    os.sync()
    print('video: resolucao nao confirmada: voltando a anterior', flush=True)
    return True


def decide(forcado='AUTO', modo_escolhido=None):
    """(saida_desejada, modo_lcd, motivo). forcado: AUTO / TUBO / LCD.
    modo_escolhido: resolucao do LCD escolhida em OPCOES > VIDEO (senao a nativa)"""
    e, onde = _le_confirmada()
    info = analisa(e)
    if info:
        print('video: EDID (%s) %s %04X "%s" preferido %s; modos %s' % (
            onde, info['fab'], info['prod'], info['nome'] or '', info['pref'],
            ' '.join('%dx%d@%g' % m for m in info['modos'][:16])), flush=True)
    else:
        print('video: EDID nao lida (%s)' % onde, flush=True)
    if forcado in ('TUBO', 'LCD'):
        saida = forcado.lower()
        motivo = 'escolhido em OPCOES > VIDEO'
    else:
        saida = classifica(info)
        motivo = 'detectado'
    if saida is None:
        return None, None, 'sem EDID: fica como esta'
    modo = escolhe_menu(info) if (saida == 'lcd' and info) else None
    if saida == 'lcd' and modo_escolhido:
        modo = modo_escolhido
    return saida, modo, motivo
