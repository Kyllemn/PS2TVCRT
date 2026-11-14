"""RETROVOLT - capas automaticas (R28, pedido do autor: "so colocar a rom e quando iniciar
ele baixar").

Ao ligar (e depois a cada 30 min), com internet, numa thread de baixa prioridade:
- para cada jogo das pastas roms/<sistema> que ainda nao tem capa em imagens/capas/<sistema>/,
- PlayStation: tenta primeiro pelo codigo do disco (SLUS-01158) no psx-covers (GitHub);
- todos: cruza o nome do arquivo com a lista de capas do libretro-thumbnails (a lista de
  nomes de cada console fica em imagens/.cache/capas/ e e baixada de novo a cada 30 dias)
  e baixa a capa do nome que bate;
- a capa e reduzida pelo ffmpeg (no maximo 600 linhas, PNG RGB, o que o menu le rapido)
  e gravada com o nome da ROM (ou o codigo do disco). O menu ja procura por esses nomes.
Nome que nao bate com seguranca nao recebe capa (melhor sem capa do que capa errada); fica
anotado e so e tentado de novo depois de 30 dias. Nada e baixado com jogo rodando.
"""
import difflib
import os
import re
import subprocess
import threading
import time
import urllib.parse
import urllib.request

LIBRETRO = 'https://thumbnails.libretro.com/'
LIBRETRO_GH = 'https://raw.githubusercontent.com/libretro-thumbnails/%s/master/Named_Boxarts/%s'
PSX_COVERS = 'https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/default/%s.jpg'
AGENTE = {'User-Agent': 'RetroVolt/1.0'}
DIAS = 30 * 86400
ALTURA = 600

# pasta do RetroVolt (roms/<chave>) -> pastas do libretro-thumbnails (na ordem de busca)
SISTEMAS_LIBRETRO = {
    'ps1': ['Sony - PlayStation'],
    'segacd': ['Sega - Mega-CD - Sega CD'],
    'neogeocd': ['SNK - Neo Geo CD'],
    'pcengine': ['NEC - PC Engine - TurboGrafx 16'],
    'pcenginecd': ['NEC - PC Engine CD - TurboGrafx-CD'],
    'megadrive': ['Sega - Mega Drive - Genesis'],
    '32x': ['Sega - 32X'],
    'mastersystem': ['Sega - Master System - Mark III'],
    'gamegear': ['Sega - Game Gear'],
    'sg1000': ['Sega - SG-1000'],
    'snes': ['Nintendo - Super Nintendo Entertainment System'],
    'dreamcast': ['Sega - Dreamcast'],
    'saturn': ['Sega - Saturn'],
    'nes': ['Nintendo - Nintendo Entertainment System'],
    'gb': ['Nintendo - Game Boy'],
    'gbc': ['Nintendo - Game Boy Color'],
    'gba': ['Nintendo - Game Boy Advance'],
    'ngp': ['SNK - Neo Geo Pocket Color', 'SNK - Neo Geo Pocket'],
    'nds': ['Nintendo - Nintendo DS'],
    'neogeo': ['FBNeo - Arcade Games', 'SNK - Neo Geo'],
    'arcade': ['FBNeo - Arcade Games'],
}
# regiao preferida quando o nome da ROM nao diz (Brasil primeiro: o autor e daqui)
REGIOES = ['brazil', 'usa', 'world', 'europe', 'japan']


def log(msg):
    print('capas: ' + msg, flush=True)


def troca_proibidos(nome):
    """os nomes do libretro trocam & * / : ` < > ? \\ | por _"""
    return re.sub(r'[&*/:`<>?\\|"]', '_', nome)


def _tags(nome):
    return [t.strip().lower() for g in re.findall(r'\(([^)]*)\)', nome) for t in g.split(',')]


def titulo(nome):
    """chave de comparacao: so o titulo, sem (regiao), [tags], pontuacao e 'The' no fim"""
    s = re.sub(r'\([^)]*\)|\[[^\]]*\]', ' ', nome)
    s = s.lower().replace('&', ' and ').replace('_', ' ')
    s = re.sub(r',\s*the\b', ' ', s)
    s = re.sub(r'^the\s+', ' ', s)
    s = re.sub(r'\b(disc|disk|cd)\s*\d+\b', ' ', s)
    s = re.sub(r'[^a-z0-9]+', ' ', s)
    return ' '.join(s.split())


class Lista:
    """nomes das capas de um console no libretro (cache em disco, renovado a cada 30 dias)"""

    def __init__(self, cache_dir, pasta_libretro):
        self.pasta = pasta_libretro
        self.arq = os.path.join(cache_dir, 'lista_' + re.sub(r'[^A-Za-z0-9]+', '_', pasta_libretro) + '.txt')
        self.nomes = None
        self.por_titulo = {}

    def carrega(self):
        if self.nomes is not None:
            return True
        velho = True
        try:
            velho = time.time() - os.path.getmtime(self.arq) > DIAS
        except OSError:
            pass
        if velho:
            try:
                url = LIBRETRO + urllib.parse.quote(self.pasta) + '/Named_Boxarts/'
                html = baixa(url).decode('utf-8', 'replace')
                nomes = [urllib.parse.unquote(h)[:-4] for h in re.findall(r'href="([^"?/][^"]*\.png)"', html)]
                if nomes:
                    os.makedirs(os.path.dirname(self.arq), exist_ok=True)
                    with open(self.arq + '.tmp', 'w') as f:
                        f.write('\n'.join(nomes) + '\n')
                    os.replace(self.arq + '.tmp', self.arq)
                    log('lista %s: %d capas' % (self.pasta, len(nomes)))
            except Exception as ex:
                log('lista %s: sem internet? (%s)' % (self.pasta, ex))
        try:
            with open(self.arq) as f:
                self.nomes = [l.rstrip('\n') for l in f if l.strip()]
        except OSError:
            return False
        for n in self.nomes:
            self.por_titulo.setdefault(titulo(n), []).append(n)
        return True

    def acha(self, nome_rom):
        """nome da capa que corresponde ao arquivo, ou None se nao tiver certeza"""
        if not self.carrega():
            return None
        exato = troca_proibidos(nome_rom)
        if exato in self.por_titulo.get(titulo(exato), []):
            return exato
        t = titulo(nome_rom)
        cands = self.por_titulo.get(t)
        if not cands and len(t) >= 4:
            perto = difflib.get_close_matches(t, self.por_titulo.keys(), n=1, cutoff=0.9)
            cands = self.por_titulo.get(perto[0]) if perto else None
        if not cands and len(t.split()) >= 2:
            # nome curto sem uma palavra do oficial ("pokemon emerald" -> "Pokemon - Emerald
            # Version"): todas as palavras batem e o oficial so tem UMA a mais; vale se for o unico
            meu = set(t.split())
            achados = [k for k in self.por_titulo
                       if meu <= set(k.split()) and len(k.split()) - len(t.split()) == 1]
            if len(achados) == 1:
                cands = self.por_titulo[achados[0]]
        if not cands:
            return None
        meus = set(_tags(nome_rom))

        def nota(c):
            tc = set(_tags(c))
            pontos = 10 * len(meus & tc)
            for i, r in enumerate(REGIOES):
                if r in tc:
                    pontos += len(REGIOES) - i
                    break
            return (pontos, -len(c))
        return max(cands, key=nota)


def baixa(url, timeout=20):
    req = urllib.request.Request(url, headers=AGENTE)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def grava_capa(dados, destino, tmp_dir):
    """reduz com o ffmpeg (PNG RGB de ate 600 linhas); sem ffmpeg, grava o PNG como veio"""
    os.makedirs(os.path.dirname(destino), exist_ok=True)
    os.makedirs(tmp_dir, exist_ok=True)
    ent = os.path.join(tmp_dir, 'capa_baixada')
    with open(ent, 'wb') as f:
        f.write(dados)
    try:
        r = subprocess.run(['nice', '-n', '19', 'ffmpeg', '-v', 'error', '-y', '-i', ent, '-vf',
                            "scale=-2:'min(%d,ih)'" % ALTURA, '-frames:v', '1', '-pix_fmt', 'rgb24',
                            destino + '.tmp.png'], timeout=60)
        if r.returncode == 0 and os.path.getsize(destino + '.tmp.png') > 0:
            os.replace(destino + '.tmp.png', destino)
            return True
    except (OSError, subprocess.SubprocessError):
        pass
    try:
        os.remove(destino + '.tmp.png')
    except OSError:
        pass
    if dados[:8] == b'\x89PNG\r\n\x1a\n':
        os.replace(ent, destino)
        return True
    return False


class Capas(threading.Thread):
    """procura e baixa as capas que faltam (ver o comeco do arquivo)"""

    def __init__(self, base, lista_jogos, identifica, avisa, pausa, nomes_fbneo=None):
        super().__init__(daemon=True)
        self.base = base
        self.lista_jogos = lista_jogos      # (base, sistema) -> caminhos dos jogos
        self.identifica = identifica        # caminho -> {'codigo': ...} (PS1)
        self.avisa = avisa                  # chamado quando uma capa nova chega (redesenha)
        self.pausa = pausa                  # Event: jogo rodando (nao baixa nada)
        self.nomes_fbneo = nomes_fbneo or (lambda: {})   # nome curto do .zip -> nome completo
        self.cache = os.path.join(base, 'imagens', '.cache', 'capas')
        self.listas = {}
        self.falhas_arq = os.path.join(self.cache, 'sem_capa.txt')

    def _falhas(self):
        f = {}
        try:
            with open(self.falhas_arq) as a:
                for l in a:
                    t, _, k = l.rstrip('\n').partition(' ')
                    if k and time.time() - float(t) < DIAS:
                        f[k] = t
        except (OSError, ValueError):
            pass
        return f

    def _grava_falhas(self, f):
        try:
            os.makedirs(self.cache, exist_ok=True)
            with open(self.falhas_arq, 'w') as a:
                a.write(''.join('%s %s\n' % (t, k) for k, t in f.items()))
        except OSError:
            pass

    def _espera_jogo(self):
        while self.pausa.is_set():
            time.sleep(5)

    def tem_capa(self, sis, jogo, codigo=None):
        pasta = os.path.join(self.base, 'imagens', 'capas', sis)
        nomes = [os.path.splitext(os.path.basename(jogo))[0]]
        if codigo:
            nomes += [codigo, codigo.replace('-', ''), codigo.replace('-', '_')]
        return any(os.path.exists(os.path.join(pasta, n + e)) for n in nomes for e in ('.png', '.PNG'))

    def nome_para_busca(self, sis, jogo):
        n = os.path.splitext(os.path.basename(jogo))[0]
        if sis in ('neogeo', 'arcade'):       # romset do FBNeo: nome curto -> nome completo
            try:
                n = self.nomes_fbneo().get(n.lower(), n)
            except Exception:
                pass
        return re.sub(r'\s*\((track|faixa)\s*\d+\)', '', n, flags=re.I)

    def uma_volta(self):
        falhas = self._falhas()
        novas = 0
        for sis, pastas in SISTEMAS_LIBRETRO.items():
            try:
                jogos = self.lista_jogos(self.base, sis)
            except Exception:
                continue
            for jogo in jogos:
                self._espera_jogo()
                chave = sis + '/' + os.path.basename(jogo)
                if chave in falhas:
                    continue
                codigo = None
                if sis == 'ps1':
                    try:
                        codigo = (self.identifica(jogo) or {}).get('codigo')
                    except Exception:
                        codigo = None
                if self.tem_capa(sis, jogo, codigo):
                    continue
                destino_dir = os.path.join(self.base, 'imagens', 'capas', sis)
                ok = False
                if codigo:
                    try:
                        ok = grava_capa(baixa(PSX_COVERS % codigo), os.path.join(destino_dir, codigo + '.png'),
                                        self.cache)
                        if ok:
                            log('%s: %s (codigo %s, psx-covers)' % (sis, os.path.basename(jogo), codigo))
                    except Exception:
                        ok = False
                if not ok:
                    busca = self.nome_para_busca(sis, jogo)
                    for p in pastas:
                        lst = self.listas.get(p)
                        if lst is None:
                            lst = self.listas[p] = Lista(self.cache, p)
                        achado = lst.acha(busca)
                        if not achado:
                            continue
                        destino = os.path.join(destino_dir, os.path.splitext(os.path.basename(jogo))[0] + '.png')
                        nome_url = urllib.parse.quote(achado + '.png')
                        for url in (LIBRETRO + urllib.parse.quote(p) + '/Named_Boxarts/' + nome_url,
                                    LIBRETRO_GH % (urllib.parse.quote(p.replace(' ', '_')), nome_url)):
                            try:
                                ok = grava_capa(baixa(url), destino, self.cache)
                            except Exception:
                                ok = False
                            if ok:
                                break
                        if ok:
                            log('%s: %s -> %s' % (sis, os.path.basename(jogo), achado))
                            break
                if ok:
                    novas += 1
                    self.avisa()
                else:
                    falhas[chave] = '%d' % time.time()
                    log('%s: sem capa para %s' % (sis, os.path.basename(jogo)))
                time.sleep(0.3)
        self._grava_falhas(falhas)
        return novas

    def run(self):
        time.sleep(20)                       # deixa o boot e o Wi-Fi terminarem
        while True:
            try:
                baixa(LIBRETRO, timeout=10)  # tem internet?
                n = self.uma_volta()
                if n:
                    log('%d capas novas' % n)
                espera = 1800
            except Exception:
                espera = 300                 # sem internet: tenta de novo em 5 min
            time.sleep(espera)
