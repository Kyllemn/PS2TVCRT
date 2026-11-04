# psxnes

Emulador de NES para PlayStation, feito do zero para rodar o Final Fantasy III (MMC3) em velocidade cheia.

- `core/cpu.c`: 6502 interpretado (flags preguiçosas, leitura por páginas de 2 KB). Validado contra o `nestest.log`: 8991 instruções iguais, incluindo ciclos e opcodes não oficiais.
- `pc/nestest.c`: teste no PC (`gcc -O2 pc/nestest.c core/cpu.c`).

ROMs (jogos e de teste) não vão para o git.

- `core/nes.c`: barramento, PPU de referência (linha a linha), MMC3 e controle.
- `pc/runrom.c`: roda uma ROM no PC com roteiro de botões e grava quadros.
- `pc/refrun.py`: roda a mesma ROM e roteiro no FCEUmm (libretro) para comparar quadro a quadro.
  No FF III, os quadros batem com a referência (100% na maioria; diferenças só em cursor piscando e esmaecimentos, por 1 quadro de defasagem).
