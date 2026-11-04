/* psxnes - sistema NES: barramento, PPU (referencia, linha a linha), MMC3, controle */
#ifndef PSXNES_NES_H
#define PSXNES_NES_H
#include <stdint.h>
#include "cpu.h"

typedef struct {
	/* registradores */
	uint8_t ctrl, mask, status, oamaddr;
	uint16_t v, t; uint8_t x, w;
	uint8_t readbuf, openbus;
	uint8_t oam[256];
	uint8_t pal[32];
	uint8_t nt[0x1000];          /* 4 KB: suporta quatro telas */
	uint8_t *ntmap[4];           /* espelhamento */
	uint8_t *chrmap[8];          /* bancos de 1 KB do padrao (CHR) */
	int line;                    /* linha atual 0..261 */
	uint32_t frame;
	uint8_t sprite0_line;        /* linha em que o sprite 0 bate (255 = nao bate) */
} Ppu;

typedef struct {
	uint8_t *prg; uint32_t prg_size;
	uint8_t *chr; uint32_t chr_size; int chr_ram;
	uint8_t prgram[0x2000];
	int mapper, battery, mirror_v, four_screen;
	/* MMC3 */
	uint8_t bank_sel, regs[8], prg_mode, chr_inv, mirroring;
	uint8_t irq_latch, irq_counter, irq_reload, irq_enable;
} Cart;

extern Ppu ppu;
extern Cart cart;
extern uint8_t nes_ram[0x800];
extern uint8_t nes_screen[240][256];   /* indices de cor 0..63 */
extern uint8_t pad_state;               /* botoes: A B Sel Start Cima Baixo Esq Dir (bit0 = A) */
extern int chr_dirty;
extern uint32_t ppu_rstate;
extern uint8_t ppu_vram_dirty;                   /* o jogo escreveu na CHR-RAM */

int nes_load(uint8_t *rom, uint32_t size);
void nes_reset(void);
void nes_frame(void);                   /* emula uma frame inteira (262 linhas) */
#endif
