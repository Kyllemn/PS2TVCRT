# RETROVOLT Pi — plano tecnico (decisoes aprovadas)

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
- Copia da trilha de dados inteira para a RAM ao inserir o disco:
  LIGADA para Sega CD, Neo Geo CD e PC Engine CD (dados pequenos + faixas
  de musica). Musica tocada com buffer de alguns segundos.
- PS1: SEM copia inteira de inicio (discos variados: so dados, dados + faixas
  de musica, XA/FMV intercalado na trilha de dados, ate ~650 MB). Usa leitura
  adiantada agressiva + cache crescente (tudo que foi lido fica na RAM) e, como
  opcao, copia em segundo plano com prioridade baixa.
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
