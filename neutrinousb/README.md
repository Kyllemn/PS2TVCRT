# NEUTRINOUSB

Jogos de PS2 (DVD ou CD) direto da leitora de CD/DVD USB externa, pelo
**Neutrino**, sem mudar o Neutrino.

- `iop/usbcd_bd.irx`: a leitora como dispositivo de bloco do BDM (setores de
  512 bytes: 1 setor do disco = 4). O Neutrino lê o disco inteiro como ISO:
  `neutrino.elf -bsd=usbcd -bsdfs=bd -dvd=bdfs:usbcd0p0`
- `config/bsd-usbcd.toml`: configuração do driver para o Neutrino
  (`usbd_mini.irx` + `usbcd_bd.irx`, nos dois ambientes).
- `ee/NEUTRINOUSB.ELF`: lançador de teste. Acha o Neutrino no pendrive,
  instala o driver e a configuração nele, confere o disco e abre o Neutrino
  com os argumentos acima, **sem reiniciar o IOP** (o Neutrino lê a
  configuração dele pelo pendrive antes de reiniciar o IOP).

Para usar no seu lançador: copie `usbcd_bd.irx` para `modules/` e
`bsd-usbcd.toml` para `config/` do Neutrino e chame com os argumentos acima.
Argumentos do driver (no .toml, `args = [...]`): `speed=<KB/s>` (0 = não
travar; padrão CD 706, DVD não trava), `keep=<ms>` (manter girando, padrão 2000).

Testes que deram as regras (CDUSB c07–c09, ASUS SDRW-08D1S-U): ver
`../cdusb` e o histórico.
