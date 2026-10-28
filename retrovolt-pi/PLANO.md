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
