# psxnes

Emulador de NES para PlayStation, feito do zero para rodar o Final Fantasy III (MMC3) em velocidade cheia.

- `core/cpu.c`: 6502 interpretado (flags preguiçosas, leitura por páginas de 2 KB). Validado contra o `nestest.log`: 8991 instruções iguais, incluindo ciclos e opcodes não oficiais.
- `pc/nestest.c`: teste no PC (`gcc -O2 pc/nestest.c core/cpu.c`).

ROMs (jogos e de teste) não vão para o git.
