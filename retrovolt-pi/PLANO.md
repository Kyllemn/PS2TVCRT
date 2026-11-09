# RETROVOLT Pi — plano tecnico (decisoes aprovadas)

## Hardware do autor
- Raspberry Pi 5 com **2 GB** de RAM. Orcamento previsto: sistema enxuto
  ~250 MB, nucleo + emulacao ~100-300 MB, video (GPU/CMA) ~64-256 MB.
  Cache de disco: ate ~1 GB, adaptativo (sempre deixar >= 400 MB livres;
  se faltar memoria, descarta o que foi usado ha mais tempo).
- Um disco inteiro (ate ~700 MB) cabe; guardar so o disco ATUAL (ao trocar
  de disco em jogo de varios CDs, o anterior sai da memoria).

## Leitora (fase R03)
- Qualquer leitora USB de CD/DVD (deteccao pelo TIPO, nunca pela marca;
  usb-storage/uas + sr do Linux). Correcoes por modelo em arquivo de config.
- Camada de disco no frontend (VFS libretro): os nucleos veem uma imagem
  (.cue/.bin etc.) montada SOB DEMANDA, setor por setor. Nada e gravado;
  o cache fica so na RAM e some ao tirar o disco/desligar.
- Leitura numa thread separada, lendo adiantado: a emulacao nunca espera a
  leitora no meio de um quadro.
- Motor sempre girando (as duas juntas):
  1. comando padrao para a leitora nao entrar em repouso (power condition /
     standby desligado);
  2. garantia: se ficar ~2 s sem uso, le 1 setor (keep-alive, como no PS2).
- (Nos modos SILENCIOSA/RAPIDA) copia inteira pensada primeiro para Sega CD,
  Neo Geo CD e PC Engine CD. Musica sempre com buffer de alguns segundos.
- PS1: SEM copia inteira de inicio (discos variados: so dados, dados + faixas
  de musica, XA/FMV intercalado na trilha de dados, ate ~650 MB). Usa leitura
  adiantada agressiva + cache crescente (tudo que foi lido fica na RAM) e, como
  opcao, copia em segundo plano com prioridade baixa.
- Velocidade: travada na mais baixa aceita (SET CD SPEED + SET STREAMING,
  reaplicados apos qualquer reset).
- Modo padrao da leitora: **AO VIVO com janela de 32 MB** (se der ruim, aumentar):
  a leitora acompanha o jogo em tempo real (LED pisca nos loadings); a cada
  leitura do jogo ela copia 32 MB adiante e descansa (keep-alive, girando
  devagar); volta a ler perto do fim da janela ou num salto do jogo.
  Recentes guardados ate 100 MB (descarta o mais antigo).
  Janela ajustavel nas opcoes: 8 / 32 / 100 MB.
- Outros modos: SILENCIOSA (copia o disco inteiro devagar e para o motor) e
  RAPIDA (copia inteiro rapido e para).
- Opcao por nucleo: imitar o tempo de loading original (desligada por padrao).
- Teste da leitora em OPCOES > LEITORA (modelo, dados, musica, leitura crua,
  velocidade, busca, tempo do motor).

## Como os nucleos recebem o disco (estudo inicial, codigo-fonte de 28/10)
Interface padrao: a libretro-common tem leitora fisica pronta (HAVE_CDROM):
o nucleo abre "cdrom://drive1.cue", recebe um .cue montado pela TOC e le as
faixas "drive1-trackNN.bin" com comandos SCSI diretos. Problema: na maioria
dos nucleos essa leitura e SINCRONA, na thread da emulacao (engasgo em busca).

Nosso caminho: o frontend fornece a interface de arquivos (VFS,
RETRO_ENVIRONMENT_GET_VFS_INTERFACE) e responde ele mesmo esses caminhos,
a partir do cache/thread de leitura. O nucleo nao muda; quem fala com a
leitora e so a nossa camada.

| Nucleo | Console | VFS do frontend | Leitora (HAVE_CDROM) | Obs. |
|---|---|---|---|---|
| pcsx_rearmed | PS1 | sim | sim | ja tem leitura ASSINCRONA com buffer (cdrom-async) |
| beetle-psx / swanstation | PS1 | sim | sim / nao | alternativas |
| Genesis Plus GX | Sega CD | sim | sim | |
| beetle-pce-fast | PC Engine CD | sim | sim | o mesmo nucleo do PCEUSB (PS2) |
| neocd_libretro | Neo Geo CD | sim | sim | cue/chd |
| opera | 3DO | sim | sim | |
| yabause | Saturn | NAO | so libchdr | usa E/S propria: precisa de ajuste ou montagem |

Proximo passo deste estudo: ler, nucleo por nucleo, como cada um le setores
crus, subcanal e faixas de musica (CDDA) e onde a leitura acontece (thread).

## Video por nucleo (aprovado)
- Ao iniciar um nucleo (e sempre que o jogo mudar de resolucao), entregar a
  resolucao/frequencia NATIVA do console (DynaRes/SwitchRes).
- Aviso no rodape DIREITO por ~5 s e some: a resolucao entregue, ex.
  "320X240P 59.94HZ", "640X480I 59.94HZ". So informativo.
- Dreamcast: nativo e 480i (tremida inerente ao entrelacado na TV de tubo,
  pior em linhas finas). Opcao no nucleo: "480I NATIVO" (padrao) ou
  "240P ESTAVEL" (metade da resolucao vertical, sem tremer).

## Cadeia de video do autor
HDMI (Pi 5) > adaptador HDMI-VGA > conversor VGA-componente > TV de tubo
(15 kHz, 240p). Ja funciona com o ReplayOS. O Pi OS padrao liga em 31 kHz+
(TV nao mostra): preparar tudo no PC, EDID propria so com modos de 15 kHz
(boot ja em 240p), instalador automatico no 1o boot, SSH/Wi-Fi para log.
Primeiro teste: tela que alterna sozinha entre variantes de 240p (com o nome
de cada uma na tela e no log), para achar o sinal certo num boot so.

## TV: RetroTV (aprovado)
- Nucleo libretro RetroTV (github.com/chris-skitch/retrotv-libretro, licenca MIT),
  o mesmo usado no ReplayOS: listas IPTV M3U (MPEG-TS/HLS), guia XMLTV,
  troca de canal, estatica, NO SIGNAL, 4:3 / 240p / 50 Hz para tubo.
- Integracao: o frontend abre o nucleo com a "grade" escolhida como conteudo
  (arquivo .txt com o link M3U e opcional `epg=`, ou um .m3u). Pasta das grades:
  `RETROVOLT/TV/` no pendrive (ou na pasta de dados). As partes de instalacao
  do ReplayOS (install.sh, .cfg do ReplayOS) sao trocadas pelo nosso lancador.
- Dependencia: o RetroTV carrega o FFmpeg do sistema por dlopen
  (`libavcodec.so.61` = FFmpeg 7): usar Raspberry Pi OS baseado no Debian
  Trixie (o Bookworm traz FFmpeg 5 e nao serve).
- Decodificacao: o Pi 5 NAO tem decodificador H.264 por hardware (so HEVC);
  H.264 ate 1080p roda por software nos 4 nucleos A76, com folga para 720p.
- Memoria (Pi de 2 GB): a pausa do RetroTV guarda ate ~15 min na RAM
  (~1 GB a 8-10 Mbit/s). Limitar a ~3 min (~250 MB) nesta placa.

## Deteccao TUBO x LCD pela EDID (dados reais do autor, 09/11)
EDID lida no ReplayOS, porta HDMI-A-1, cadeia HDMI>VGA>componente>tubo:
fabricante "AGO", produto 0001, serial "10NC", 2013, EDID 1.3, entrada digital,
tela 30x23 cm (4:3), SEM nome de monitor, preferida 1024x768@60 (48,4 kHz),
outras 1920x1080, 1360x768, 1280x1024; nenhum modo de 15 kHz. E a EDID fixa do
adaptador HDMI>VGA (a TV de tubo nao manda nada).
Regras, em ordem:
1. Lista de adaptadores conhecidos (fabricante+produto): AGO/0001 = conversor > TUBO.
2. "Cara de conversor": sem nome de monitor (descritor 0xFC) + preferida <= 1024x768
   + tela 4:3 > TUBO.
3. Tem nome, preferida >= 1280x720 ou 16:9/16:10 > LCD (resolucao/proporcao dele).
4. Duvida > pergunta unica TUBO/LCD (lembrada). Troca manual: video_saida no
   retrovolt.cfg ou OPCOES > VIDEO; modo seguro com SELECT ao ligar.
Limite conhecido: o mesmo adaptador num monitor VGA de PC parece igual (regra 2
erra); a troca manual resolve. O LOG.TXT registra a EDID para ampliar a lista.

## Fases (proposta de 09/11)
| Fase | Conteudo | O autor testa |
|---|---|---|
| R00 | Base: instalador, boot direto, versoes pelo pendrive, volta automatica, LOG.TXT, cartao protegido | Liga e mostra a nossa tela |
| R01 | Video: deteccao TUBO/LCD (EDID), 240p, tela de teste com barras laranja da convergencia | Imagem certa no tubo |
| R02 | Menu: abas, controle, OPCOES, Wi-Fi, criacao das pastas, retrovolt.cfg | Navegar no menu |
| R03 | Primeiro nucleo: PS1 (pcsx_rearmed) pela pasta roms/ps1; popup START+SELECT; save states | Jogar |
| R04 | Leitora USB (modo AO VIVO) com PS1 | Jogar pelo CD |
| R05 | TV: RetroTV | Assistir canais |
| R06 | Demais nucleos (cartucho e CD) | Jogar |
| R07 | Acabamento: imagens dos consoles, BIOS, INFO, imagem propria do cartao | Tudo |
Primeira instalacao (R00): internet por cabo de rede ou Wi-Fi so desta vez no
Raspberry Pi Imager (o instalador baixa os pacotes). Depois, Wi-Fi so pelo menu.
