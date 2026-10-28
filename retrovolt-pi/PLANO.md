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
