#include <string.h>
#include "nes.h"
#include "apu.h"
static uint8_t irq_map;
static inline void irq_update(void) { cpu.irq = irq_map | apu_irq; }

Ppu ppu;
uint32_t ppu_rstate;   /* muda quando ctrl/mask/rolagem/bancos/espelhamento mudam */
uint8_t ppu_vram_dirty; /* nametable, paleta ou CHR escritas */
Cart cart;
uint8_t nes_ram[0x800];
uint8_t nes_screen[240][256];
uint8_t pad_state;
int chr_dirty;
static uint8_t pad_shift, pad_strobe;
static uint8_t chr_ram_buf[0x2000];

/* ---------------- mapeamento ---------------- */
static void map_prg8(int slot, int bank) {               /* slot 0..3 = $8000,$A000,$C000,$E000 */
	int n = cart.prg_size / 0x2000;
	bank = ((bank % n) + n) % n;
	uint8_t *base = cart.prg + bank * 0x2000 - (0x8000 + slot * 0x2000);
	for (int i = 0; i < 4; i++) cpu.rmap[16 + slot * 4 + i] = base;
}
static void map_chr1(int slot, int bank) {
	int n = cart.chr_size / 0x400;
	ppu_rstate++;
	ppu.chrmap[slot] = cart.chr + (bank % n) * 0x400;
}
static void set_mirror(int vertical) {
	ppu_rstate++;
	if (cart.four_screen) { for (int i = 0; i < 4; i++) ppu.ntmap[i] = ppu.nt + i * 0x400; return; }
	if (vertical) { ppu.ntmap[0] = ppu.ntmap[2] = ppu.nt; ppu.ntmap[1] = ppu.ntmap[3] = ppu.nt + 0x400; }
	else { ppu.ntmap[0] = ppu.ntmap[1] = ppu.nt; ppu.ntmap[2] = ppu.ntmap[3] = ppu.nt + 0x400; }
}
static void mmc3_update(void) {
	if (cart.prg_mode) { map_prg8(0, -2); map_prg8(2, cart.regs[6]); }
	else { map_prg8(0, cart.regs[6]); map_prg8(2, -2); }
	map_prg8(1, cart.regs[7]); map_prg8(3, -1);
	int b = cart.chr_inv ? 4 : 0, s = cart.chr_inv ? 0 : 4;
	map_chr1(b + 0, cart.regs[0] & 0xfe); map_chr1(b + 1, cart.regs[0] | 1);
	map_chr1(b + 2, cart.regs[1] & 0xfe); map_chr1(b + 3, cart.regs[1] | 1);
	for (int i = 0; i < 4; i++) map_chr1(s + i, cart.regs[2 + i]);
}
static void mmc3_write(uint16_t a, uint8_t v) {
	switch (a & 0xe001) {
	case 0x8000: cart.bank_sel = v & 7; cart.prg_mode = (v >> 6) & 1; cart.chr_inv = v >> 7; mmc3_update(); break;
	case 0x8001: cart.regs[cart.bank_sel] = v; mmc3_update(); break;
	case 0xa000: if (!cart.four_screen) set_mirror(!(v & 1)); break;
	case 0xa001: break;
	case 0xc000: cart.irq_latch = v; break;
	case 0xc001: cart.irq_counter = 0; cart.irq_reload = 1; break;
	case 0xe000: cart.irq_enable = 0; irq_map = 0; irq_update(); break;
	case 0xe001: cart.irq_enable = 1; break;
	}
}
static void mmc3_scanline(void) {
	if (cart.irq_counter == 0 || cart.irq_reload) { cart.irq_counter = cart.irq_latch; cart.irq_reload = 0; }
	else cart.irq_counter--;
	if (cart.irq_counter == 0 && cart.irq_enable) { irq_map = 1; irq_update(); }
}

/* ---------------- PPU: memoria ---------------- */
static inline uint8_t ppu_peek(uint16_t a) {
	a &= 0x3fff;
	if (a < 0x2000) return ppu.chrmap[a >> 10][a & 0x3ff];
	if (a < 0x3f00) return ppu.ntmap[(a >> 10) & 3][a & 0x3ff];
	a &= 0x1f; if ((a & 0x13) == 0x10) a &= 0x0f;
	return ppu.pal[a];
}
static inline void ppu_poke(uint16_t a, uint8_t v) {
	a &= 0x3fff;
	if (a < 0x2000) { if (cart.chr_ram) { uint8_t *p = ppu.chrmap[a >> 10]; p[a & 0x3ff] = v; chr_dirty |= 1 << ((p - cart.chr) >> 10); ppu_vram_dirty = 1; } return; }
	if (a < 0x3f00) { uint8_t *q = &ppu.ntmap[(a >> 10) & 3][a & 0x3ff]; if (*q != v) { *q = v; ppu_vram_dirty = 1; } return; }
	a &= 0x1f; if ((a & 0x13) == 0x10) a &= 0x0f;
	if (ppu.pal[a] != (v & 0x3f)) { ppu.pal[a] = v & 0x3f; ppu_vram_dirty = 1; }
}

/* ---------------- barramento da CPU ---------------- */
uint8_t bus_read(uint16_t a) {
	if (a < 0x4000) {
		switch (a & 7) {
		case 2: { uint8_t r = (ppu.status & 0xe0) | (ppu.openbus & 0x1f);
			ppu.status &= 0x7f; ppu.w = 0; return r; }
		case 4: return ppu.oam[ppu.oamaddr];
		case 7: { uint8_t r;
			if ((ppu.v & 0x3fff) >= 0x3f00) { r = ppu_peek(ppu.v); ppu.readbuf = ppu_peek(ppu.v - 0x1000); }
			else { r = ppu.readbuf; ppu.readbuf = ppu_peek(ppu.v); }
			ppu.v += (ppu.ctrl & 4) ? 32 : 1; return r; }
		}
		return ppu.openbus;
	}
	if (a == 0x4016) { uint8_t r = pad_strobe ? (pad_state & 1) : (pad_shift & 1); if (!pad_strobe) pad_shift = (pad_shift >> 1) | 0x80; return 0x40 | r; }
	if (a == 0x4017) return 0x40;
	if (a == 0x4015) { uint8_t r = apu_read_status(); irq_update(); return r; }
	return a >> 8;
}
void bus_write(uint16_t a, uint8_t v) {
	if (a < 0x4000) {
		ppu.openbus = v;
		if ((a & 7) <= 1 || (a & 7) == 5 || (a & 7) == 6) ppu_rstate++;
		switch (a & 7) {
		case 0:
			if ((v & 0x80) && !(ppu.ctrl & 0x80) && (ppu.status & 0x80)) cpu.nmi = 1;
			ppu.ctrl = v; ppu.t = (ppu.t & 0xf3ff) | ((v & 3) << 10);
			break;
		case 1: ppu.mask = v; break;
		case 3: ppu.oamaddr = v; break;
		case 4: ppu.oam[ppu.oamaddr++] = v; break;
		case 5: if (!ppu.w) { ppu.t = (ppu.t & 0xffe0) | (v >> 3); ppu.x = v & 7; }
			else ppu.t = (ppu.t & 0x8c1f) | ((v & 7) << 12) | ((v & 0xf8) << 2);
			ppu.w ^= 1; break;
		case 6: if (!ppu.w) ppu.t = (ppu.t & 0x00ff) | ((v & 0x3f) << 8);
			else { ppu.t = (ppu.t & 0xff00) | v; ppu.v = ppu.t; }
			ppu.w ^= 1; break;
		case 7: ppu_poke(ppu.v, v); ppu.v += (ppu.ctrl & 4) ? 32 : 1; break;
		}
		return;
	}
	if (a == 0x4014) {                                   /* DMA de sprites: 513 ciclos */
		uint8_t *pg = cpu.rmap[(v << 8) >> 11];
		for (int i = 0; i < 256; i++) { uint16_t a2 = (v << 8) | i; ppu.oam[(ppu.oamaddr + i) & 0xff] = pg ? pg[a2] : bus_read(a2); }
		cpu.cycles += 513 + (cpu.cycles & 1); return;
	}
	if (a == 0x4016) { pad_strobe = v & 1; if (pad_strobe) pad_shift = pad_state; else pad_shift = pad_state; return; }
	if (a < 0x4018) { apu_write(a, v); irq_update(); return; }
	if (a < 0x4020) return;
	if (a < 0x8000) return;
	if (cart.mapper == 4) mmc3_write(a, v);
}

/* ---------------- PPU: render de uma linha ---------------- */
#ifndef NES_GPU
static void render_line(int y) {
	uint8_t *out = nes_screen[y];
	uint8_t bgpix[256 + 16];
	int show_bg = ppu.mask & 8, show_sp = ppu.mask & 16;
	if (!show_bg) memset(bgpix, 0, sizeof bgpix);
	else {
		uint16_t v = ppu.v;
		uint16_t base = (ppu.ctrl & 0x10) ? 0x1000 : 0;
		int fy = (v >> 12) & 7;
		for (int tile = 0; tile < 33; tile++) {
			uint8_t *ntp = ppu.ntmap[(v >> 10) & 3];
			uint8_t idx = ntp[v & 0x3ff];
			uint8_t at = ntp[0x3c0 | ((v >> 4) & 0x38) | ((v >> 2) & 7)];
			int sh = ((v >> 4) & 4) | (v & 2);
			uint8_t paln = ((at >> sh) & 3) << 2;
			uint16_t pa = base + idx * 16 + fy;
			uint8_t lo = ppu.chrmap[pa >> 10][pa & 0x3ff], hi = ppu.chrmap[(pa + 8) >> 10][(pa + 8) & 0x3ff];
			uint8_t *d = bgpix + tile * 8;
			for (int b = 0; b < 8; b++) {
				int c = ((lo >> (7 - b)) & 1) | (((hi >> (7 - b)) & 1) << 1);
				d[b] = c ? (paln | c) : 0;
			}
			/* incremento horizontal de v */
			if ((v & 0x1f) == 31) { v &= ~0x1f; v ^= 0x400; } else v++;
		}
		if (!(ppu.mask & 2)) memset(bgpix + ppu.x, 0, 8);
	}
	uint8_t *bg = bgpix + ppu.x;
	for (int i = 0; i < 256; i++) out[i] = ppu.pal[bg[i]];
	/* sprites: os 8 primeiros da linha, prioridade pelo indice */
	if (show_sp) {
		int h = (ppu.ctrl & 0x20) ? 16 : 8, n = 0;
		int list[8];
		for (int s = 0; s < 64 && n < 8; s++) {
			int sy = ppu.oam[s * 4] + 1;
			if (y >= sy && y < sy + h) list[n++] = s;
		}
		uint8_t done[256]; memset(done, 0, sizeof done);
		for (int k = 0; k < n; k++) {
			int s = list[k]; uint8_t *o = ppu.oam + s * 4;
			int row = y - (o[0] + 1), attr = o[2], sx = o[3];
			if (attr & 0x80) row = h - 1 - row;
			uint16_t pa;
			if (h == 16) pa = ((o[1] & 1) << 12) | ((o[1] & 0xfe) << 4) | ((row & 8) << 1) | (row & 7);
			else pa = ((ppu.ctrl & 8) ? 0x1000 : 0) | (o[1] << 4) | row;
			uint8_t lo = ppu.chrmap[pa >> 10][pa & 0x3ff], hi = ppu.chrmap[(pa + 8) >> 10][(pa + 8) & 0x3ff];
			for (int b = 0; b < 8; b++) {
				int px = sx + b; if (px > 255) break;
				int bit = (attr & 0x40) ? b : 7 - b;
				int c = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
				if (!c || done[px]) continue;
				if (px < 8 && !(ppu.mask & 4)) continue;
				done[px] = 1;
				if (s == 0 && bg[px] && px < 255 && ppu.sprite0_line == 255) ppu.sprite0_line = y;
				if (!(attr & 0x20) || !bg[px]) out[px] = ppu.pal[0x10 | ((attr & 3) << 2) | c];
			}
		}
	}
	if (ppu.mask & 1) for (int i = 0; i < 256; i++) out[i] &= 0x30;
}
#endif

static inline int rendering(void) { return ppu.mask & 0x18; }
#ifdef NES_GPU
void gpu_line(int y);        /* grava o estado da linha (psx/video.c) */
void gpu_frame_end(void);    /* monta a lista de desenho da frame */
static void sprite0_approx(void) {
	/* sem render por software: o sprite 0 "bate" na primeira linha opaca dele */
	ppu.sprite0_line = 255;
	if ((ppu.mask & 0x18) != 0x18) return;
	int h = (ppu.ctrl & 0x20) ? 16 : 8, y0 = ppu.oam[0] + 1;
	for (int r = 0; r < h; r++) {
		int row = (ppu.oam[2] & 0x80) ? h - 1 - r : r;
		uint16_t pa = (h == 16) ? (((ppu.oam[1] & 1) << 12) | ((ppu.oam[1] & 0xfe) << 4) | ((row & 8) << 1) | (row & 7))
		                        : (((ppu.ctrl & 8) ? 0x1000 : 0) | (ppu.oam[1] << 4) | row);
		if (ppu.chrmap[pa >> 10][pa & 0x3ff] | ppu.chrmap[(pa + 8) >> 10][(pa + 8) & 0x3ff]) {
			if (y0 + r < 239) ppu.sprite0_line = y0 + r;
			return;
		}
	}
}
#endif
static void inc_y(void) {
	uint16_t v = ppu.v;
	if ((v & 0x7000) != 0x7000) v += 0x1000;
	else { v &= ~0x7000; int y = (v >> 5) & 31;
		if (y == 29) { y = 0; v ^= 0x800; } else if (y == 31) y = 0; else y++;
		v = (v & ~0x3e0) | (y << 5); }
	ppu.v = v;
}

/* ---------------- frame ---------------- */
static int32_t dot_acc;   /* ciclos de PPU acumulados (3 por ciclo de CPU) */
#ifdef NES_PROF
#include <hwregs_c.h>
volatile uint32_t prof_cpu, prof_apu, prof_line;
#define PT() TIMER_VALUE(2)
static void run_dots(int dots) { uint16_t a = PT(); dot_acc += dots; cpu_run(dot_acc / 3); uint16_t b = PT(); apu_run(cpu.cycles); irq_update(); prof_cpu += (uint16_t)(b - a); prof_apu += (uint16_t)(PT() - b); }
#else
static void run_dots(int dots) { dot_acc += dots; cpu_run(dot_acc / 3); apu_run(cpu.cycles); irq_update(); }
#endif

void nes_frame(void) {
	for (int y = 0; y < 262; y++) {
		ppu.line = y;
		if (y < 240) {
			if (rendering()) { ppu.v = (ppu.v & ~0x41f) | (ppu.t & 0x41f); }
#ifdef NES_GPU
			if (y == 0) sprite0_approx();
#ifdef NES_PROF
			{ uint16_t a = PT(); gpu_line(y); prof_line += (uint16_t)(PT() - a); }
#else
			gpu_line(y);
#endif
#else
			if (rendering()) render_line(y); else memset(nes_screen[y], ppu.pal[0], 256);
#endif
			if (ppu.sprite0_line == y) ppu.status |= 0x40;
			run_dots(260);
			if (rendering()) { inc_y(); if (cart.mapper == 4) mmc3_scanline(); }
			run_dots(81);
		} else if (y == 240) {
#ifdef NES_GPU
			gpu_frame_end();
#endif
			run_dots(341);
		} else if (y == 241) {
			ppu.status |= 0x80; ppu.frame++;
			if (ppu.ctrl & 0x80) cpu.nmi = 1;
			run_dots(341);
		} else if (y == 261) {
			ppu.status &= ~0xe0;
#ifndef NES_GPU
			ppu.sprite0_line = 255;
#endif
			run_dots(260);
			if (rendering()) { if (cart.mapper == 4) mmc3_scanline(); ppu.v = (ppu.v & 0x41f) | (ppu.t & ~0x41f); }
			run_dots(81);
		} else run_dots(341);
	}
}

/* ---------------- carga ---------------- */
int nes_load(uint8_t *rom, uint32_t size) {
	if (size < 16 || memcmp(rom, "NES\x1a", 4)) return -1;
	memset(&cart, 0, sizeof cart); memset(&ppu, 0, sizeof ppu); memset(&cpu, 0, sizeof cpu);
	cart.prg_size = rom[4] * 0x4000; cart.chr_size = rom[5] * 0x2000;
	cart.mapper = (rom[6] >> 4) | (rom[7] & 0xf0);
	cart.battery = (rom[6] >> 1) & 1; cart.mirror_v = rom[6] & 1; cart.four_screen = (rom[6] >> 3) & 1;
	uint8_t *p = rom + 16 + ((rom[6] & 4) ? 512 : 0);
	cart.prg = p;
	if (cart.chr_size) cart.chr = p + cart.prg_size;
	else { cart.chr = chr_ram_buf; cart.chr_size = 0x2000; cart.chr_ram = 1; memset(chr_ram_buf, 0, sizeof chr_ram_buf); }
	if (cart.mapper != 0 && cart.mapper != 4) return -2;
	/* RAM interna e PRG-RAM direto pela tabela de paginas */
	for (int i = 0; i < 4; i++) { cpu.rmap[i] = cpu.wmap[i] = nes_ram - i * 0x800; }
	for (int i = 0; i < 4; i++) { cpu.rmap[12 + i] = cpu.wmap[12 + i] = cart.prgram - 0x6000; }
	set_mirror(cart.mirror_v);
	if (cart.mapper == 0) {
		map_prg8(0, 0); map_prg8(1, 1); map_prg8(2, cart.prg_size > 0x4000 ? 2 : 0); map_prg8(3, cart.prg_size > 0x4000 ? 3 : 1);
		for (int i = 0; i < 8; i++) map_chr1(i, i);
	} else {
		cart.regs[6] = 0; cart.regs[7] = 1; mmc3_update();
	}
	ppu.sprite0_line = 255;
	return 0;
}
void nes_reset(void) { apu_reset(); irq_map = 0; dot_acc = 0; cpu.cycles = 0; cpu_reset(); dot_acc = cpu.cycles * 3; }
