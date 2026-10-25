# EMBERUSB

Abre o **Ember** (emulador de PS1 para PS2, de Gageformer) com o CD de PS1 na
leitora de CD USB externa. O Ember não é modificado: ele lê `.cue`/`.bin`, e o
`vcd.irx` faz o disco da leitora aparecer como esses arquivos.

- `iop/vcd.irx`: dispositivo `vcd:`. `vcd:/X` = `mass:/X`, menos
  `<pasta do jogo>/<jogo>.cue` (gerado pela TOC) e `<jogo>.bin` (disco inteiro,
  2352 bytes por setor, lido da leitora com cache e leitura adiantada).
- `ee/EMBERUSB.ELF`: sobe os drivers (leitora + pendrive), acha o Ember e o
  disco, sobe o `vcd.irx` e abre `vcd:/Ember/ember.elf` **sem reiniciar o IOP**.

Pendrive: `mass:/Ember/ember.elf` + `bios.bin` (sua). Os saves ficam em
`mass:/Ember/games/<codigo do jogo>/`. Logs: `mass:/EMBERUSB.TXT` e
`VCD.TXT` na pasta do jogo.

Usa o `cdusb.irx` (../cdusb), que exporta a biblioteca `cdusb` para o IOP.
