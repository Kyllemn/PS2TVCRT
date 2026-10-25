# PROMPT — Retomar o EMBERUSB (Ember lendo o CD de PS1 pela leitora USB)

> Cole este texto numa sessão nova junto com o zip `emberusb_retomada.zip`.
> Projeto pausado em 25/10/2026 na versão **e05**, esperando o Ember (beta)
> amadurecer.

## O que é
O **Ember** (Gageformer, https://github.com/Gageformer/Ember) é um emulador de PS1
para PS2, **código fechado e em beta**. Ele lê `.cue/.bin` e usa os drivers de
armazenamento que o lançador deixou no IOP (não reinicia o IOP; acha a própria
pasta pelo `argv[0]`). A licença permite fazer lançador e proíbe modificar ou
desmontar o Ember.

O **EMBERUSB** faz o Ember jogar o **CD original de PS1 na leitora USB externa**
sem mexer nele: um dispositivo de arquivos virtual no IOP (`vcd:`) mostra o disco
como `<jogo>.cue` + `<jogo>.bin`.

**Status:** funciona. Jogos abrem, imagem e som OK, controle OK. Ficam congeladas
curtas (100–600 ms nos pulos de posição do disco) e algumas de ~2,4 s.

## Peças (no zip)
```
cdusb/iop/cdusb.c       IRX: comandos SCSI/MMC pela USB (bulk-only), protocolo com o EE
                         (RPC 0x52564344) e biblioteca do IOP "cdusb" (exports.tab):
                           cdusb_iop_find_cd(int *sub), cdusb_iop_scsi(...)
cdusb/common/           cdusb_rpc.h (RPC), cdusb_iop.h (biblioteca do IOP)
cdusb/ee/               cdusb_ee.c (cliente RPC), diag.c/main.c = CDUSB.ELF (testes)
emberusb/iop/vcd.c      vcd.irx: dispositivo vcd: (iomanX). vcd:/X = mass:/X, menos
                         <dir>/<nome>.cue (gerado pela TOC) e <dir>/<nome>.bin (disco
                         inteiro, 2352 B/setor, lido da leitora). Cache em blocos de 16
                         setores, leitura adiantada, manter girando, recuperação, log.
                         Args: root=mass: dir=/Ember/games/SLUS-00933 name=SLUS-00933 blocks=16
emberusb/ee/main.c      EMBERUSB.ELF: sobe iomanX, fileXio, usbd, cdusb (+FINALIZE),
                         bdm/bdmfs_fatfs/usbmass_bd; acha o Ember; lê a TOC e o SYSTEM.CNF
                         (código do jogo = pasta dos saves); sobe o vcd.irx; abre
                         vcd:/Ember/ember.elf com argv[1] = "<COD>/<COD>.cue" SEM reset.
emberusb/loader/        mini-loader (do elf-loader da ps2sdk) com o reset do IOP desligado;
                         a ps2sdk instalada não tem LoadELFFromFileWithPartitionNoReset.
binarios/               EMBERUSB_e05.ELF e CDUSB_c08.ELF já compilados
```
Compilar (ps2dev): `make -C cdusb/iop && make -C emberusb/iop && make -C emberusb/loader && make -C emberusb/ee`

## Lições que custaram caro (não repetir)
1. **Ordem dos módulos:** `cdusb.irx` sobe antes do `usbmass_bd` e faz FINALIZE (fica só
   com leitores tipo 5; o pendrive vai para o usbmass). Leitora plugada depois do
   FINALIZE também é aceita (subclasse 02/05).
2. **Controle:** carregar `rom0:XSIO2MAN` + `rom0:XPADMAN` (como o wLaunchELF). Com
   `SIO2MAN/PADMAN` antigos o controle do Ember não sobe (e01).
3. **Vídeo:** na TV de tubo o usuário usa `display: 240` no `settings.txt` do Ember;
   no padrão (480) a imagem saiu dividida/espremida (não investigado: pode ser estado
   do GS deixado pela tela de texto do lançador).
4. **Formatação no IOP:** o `vsprintf` da ROM saiu vazio nesse PS2. O vcd.c tem
   formatador próprio (`fmt/fmtx`). Testar no PC antes de mudar.
5. **Leitora ASUS SDRW-08D1S-U (13FD:1640), cabo Y no carregador** (sem isso ela cai
   da USB em busca longa):
   - dados crus por READ CD (tipo qualquer, 0xF8) 2352; música por READ CD CD-DA;
     a pausa de 150 setores antes da música lê como música;
   - **sempre 16 setores por comando** (com 8 ela trava 1,5–2 s);
   - velocidade travada (pede 4x, ela fica em 10x): 730 KB/s crus (PS1 precisa 344);
   - busca 100–300 ms; troca dados↔música ~350 ms (a 1ª ~1,5 s);
   - o motor às vezes desacelera (~3 s para voltar);
   - o disco SLUS-00933 (67 min) falha/trava lendo em sequência perto da borda de fora.
6. **e04 foi ruim e foi revertida:** ler adiantado em VÁRIOS trechos faz a leitora ir e
   voltar entre os lados do disco a cada bloco (380–580 ms por bloco). Em leitor óptico,
   leitura adiantada só no trecho atual. Comandos de energia (START STOP UNIT ativo,
   página 1Ah) não ajudaram.

## O que o log mostrou (VCD.TXT, SLUS-00933, e03/e04)
- O Ember lê de 16 KB em 16 KB (às vezes 9,5 KB), quase sempre em sequência, com
  pulos entre arquivos; relê os mesmos lugares (ex.: 263241, 124681, 247868, 251, 354).
- ~89% dos blocos vinham da memória/leitura adiantada; as esperas >100 ms são pulos
  (busca da leitora) — o Ember espera a leitura (síncrona).
- Congeladas de **~2,4 s** em pulos longos (0→91728, 267696, 277024) e depois de
  idas e voltas entre os extremos do disco. Causa não confirmada: disco nas regiões de
  fora ou a leitora mudando rotação. **Teste pendente:** e05 com o Castlevania SOTN
  (SLPM/DRACULAX, disco que passou em todos os testes) vs. SLUS-00933.

## Próximos passos quando retomar
1. Ver o que mudou no Ember (README novo, se lê de forma assíncrona, se o código abriu).
2. Rodar a e05 com 2 discos e comparar os `VCD.TXT` (separar disco vs. leitora).
3. Ideias para as esperas: cache maior/mais esperto **só com dados do trecho atual**
   e releituras (nunca ler adiantado em outro trecho); testar outra leitora (de mesa,
   com fonte própria).

## Regras de trabalho com o usuário
- pt-BR, curto e direto. Textos de tela e log sem acentos.
- **Só compilar com `#compilar`**. Versões numeradas (e06...). Sempre dizer o tamanho
  do ELF em bytes.
- Mudar só o necessário; não quebrar o que funciona (a e04 piorou e foi reprovada).
- Nunca colocar BIOS no git, não baixar BIOS, não publicar logs.
