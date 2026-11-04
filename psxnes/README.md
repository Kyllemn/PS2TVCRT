# psxnes

Emulador de NES para PlayStation, feito do zero para rodar o Final Fantasy III (MMC3) em velocidade cheia.

- `core/cpu.c`: 6502 interpretado (flags preguiçosas, leitura por páginas de 2 KB). Validado contra o `nestest.log`: 8991 instruções iguais, incluindo ciclos e opcodes não oficiais.
- `pc/nestest.c`: teste no PC (`gcc -O2 pc/nestest.c core/cpu.c`).

ROMs (jogos e de teste) não vão para o git.

- `core/nes.c`: barramento, PPU de referência (linha a linha), MMC3 e controle.
- `pc/runrom.c`: roda uma ROM no PC com roteiro de botões e grava quadros.
- `pc/refrun.py`: roda a mesma ROM e roteiro no FCEUmm (libretro) para comparar quadro a quadro.
  No FF III, os quadros batem com a referência (100% na maioria; diferenças só em cursor piscando e esmaecimentos, por 1 quadro de defasagem).

## Estado (em andamento)
- Dynarec 6502 -> MIPS (`psx/dynarec.c`, `psx/dyn.S`) e CPU em trechos longos por eventos (`core/nes.c`):
  no Beetle o FF3 fica em ~60 qps na abertura, no mapa e nas batalhas (antes: 21-46).
- Pendente: o memory card não responde (BIOS `_card_info` dá tempo esgotado com o tratador de
  interrupções do PSn00bSDK). Sem cartão, o salvamento é desligado no boot para não travar o jogo.
  No console, a teste1 travou no fim da 2ª batalha (provável gravação no cartão) e mostrou o
  inimigo com letras/números no lugar do desenho: investigar.
- `pc/prof6502.c`: perfil do código 6502 no PC (compilar com -DPROFILE).
