/* psxnes - video do NES desenhado pela GPU do PS1.
 * - CHR (8 bancos fisicos de 1 KB) convertida para textura 4bpp em VRAM (320,0), 64x128 halfwords.
 * - 8 paletas do NES viram CLUTs de 16 cores em (16*i, 480); cor 0 transparente.
 * - Cada frame: o nucleo grava o estado de cada linha visivel (gpu_line); linhas com estado
 *   continuo viram um "segmento". Na linha 240 (gpu_frame_end) monta a lista: fundo, sprites
 *   atras, tiles de cada segmento (com area de desenho restrita as linhas dele), sprites na frente. */
#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include "../core/nes.h"

#define CHR_X 320
#define CLUT_Y 480

typedef struct {
	uint8_t y0, y1;               /* linhas [y0, y1) */
	uint16_t v;                   /* v da PPU na linha y0 */
	uint8_t x, ctrl, mask;
	uint8_t bank[8];              /* banco fisico de cada fatia de 1 KB do padrao */
	uint8_t nt[4];                /* qual KB de nametable em cada quadrante */
} Seg;

static Seg segs[240];
static int nseg;
static uint16_t pred_v;

/* buffers de primitivas (duplo) */
#define PRIMBUF 65536
static uint8_t primbuf[2][PRIMBUF];
static uint8_t *nextp;
static uint32_t ot[2][1];
static int db;
static DISPENV disp[2];
static DRAWENV draw[2];
static int skip_draw;

static const uint8_t nes_rgb[64][3] = {
{84,84,84},{0,30,116},{8,16,144},{48,0,136},{68,0,100},{92,0,48},{84,4,0},{60,24,0},{32,42,0},{8,58,0},{0,64,0},{0,60,0},{0,50,60},{0,0,0},{0,0,0},{0,0,0},
{152,150,152},{8,76,196},{48,50,236},{92,30,228},{136,20,176},{160,20,100},{152,34,32},{120,60,0},{84,90,0},{40,114,0},{8,124,0},{0,118,40},{0,102,120},{0,0,0},{0,0,0},{0,0,0},
{236,238,236},{76,154,236},{120,124,236},{176,98,236},{228,84,236},{236,88,180},{236,106,100},{212,136,32},{160,170,0},{116,196,0},{76,208,32},{56,204,108},{56,180,204},{60,60,60},{0,0,0},{0,0,0},
{236,238,236},{168,204,236},{188,188,236},{212,178,236},{236,174,236},{236,174,212},{236,180,176},{228,196,144},{204,210,120},{180,222,120},{168,226,144},{152,226,180},{160,214,228},{160,162,160},{0,0,0},{0,0,0}};
static uint16_t nes15[64];
static uint32_t spread[256];                 /* 8 bits -> 8 nibbles (bit7 = pixel 0) */
static uint8_t last_pal[32];
static uint32_t chr4[8][16 * 32];            /* banco convertido: 16 linhas x 32 words (32 tiles 4bpp por linha) */

void video_init(void) {
	for (int i = 0; i < 64; i++)
		nes15[i] = 0x8000 | (nes_rgb[i][0] >> 3) | ((nes_rgb[i][1] >> 3) << 5) | ((nes_rgb[i][2] >> 3) << 10);
	for (int b = 0; b < 256; b++) {
		uint32_t w = 0;
		for (int p = 0; p < 8; p++) if (b & (0x80 >> p)) w |= 1u << (p * 4);
		spread[b] = w;
	}
	SetDefDispEnv(&disp[0], 0, 0, 256, 240);
	SetDefDispEnv(&disp[1], 0, 240, 256, 240);
	SetDefDrawEnv(&draw[0], 0, 240, 256, 240);
	SetDefDrawEnv(&draw[1], 0, 0, 256, 240);
	for (int i = 0; i < 2; i++) { draw[i].isbg = 0; draw[i].dtd = 0; }
	memset(last_pal, 0xff, sizeof last_pal);
	chr_dirty = 0xff;
	db = 0;
	PutDispEnv(&disp[0]); PutDrawEnv(&draw[0]);
	SetDispMask(1);
}

static inline uint8_t bank_of(int slot) { return (uint8_t)((ppu.chrmap[slot] - cart.chr) >> 10); }
static inline uint8_t nt_of(int q) { return (uint8_t)((ppu.ntmap[q] - ppu.nt) >> 10); }

void gpu_line(int y) {
	int on = ppu.mask & 0x18;
	Seg *s = nseg ? &segs[nseg - 1] : 0;
	if (s && on && s->ctrl == ppu.ctrl && s->mask == ppu.mask && s->x == ppu.x && ppu.v == pred_v) {
		int same = 1;
		for (int i = 0; i < 8 && same; i++) same = s->bank[i] == bank_of(i);
		for (int i = 0; i < 4 && same; i++) same = s->nt[i] == nt_of(i);
		if (same) { s->y1 = y + 1; goto predict; }
	}
	if (s && !on && !(s->mask & 0x18)) { s->y1 = y + 1; return; }
	s = &segs[nseg++];
	s->y0 = y; s->y1 = y + 1; s->v = ppu.v; s->x = ppu.x; s->ctrl = ppu.ctrl; s->mask = ppu.mask;
	for (int i = 0; i < 8; i++) s->bank[i] = bank_of(i);
	for (int i = 0; i < 4; i++) s->nt[i] = nt_of(i);
predict: {
	uint16_t v = ppu.v;
	if ((v & 0x7000) != 0x7000) v += 0x1000;
	else { v &= ~0x7000; int cy = (v >> 5) & 31;
		if (cy == 29) { cy = 0; v ^= 0x800; } else if (cy == 31) cy = 0; else cy++;
		v = (v & ~0x3e0) | (cy << 5); }
	pred_v = (v & ~0x41f) | (ppu.t & 0x41f);
	}
}

static void upload_chr(void) {
	int nb = cart.chr_size >> 10; if (nb > 8) nb = 8;
	for (int b = 0; b < nb; b++) {
		if (!(chr_dirty & (1 << b))) continue;
		const uint8_t *src = cart.chr + b * 0x400;
		uint32_t *dst = chr4[b];
		/* layout: tiles 0..31 na linha de tiles 0, 32..63 na linha 1; cada linha de pixel = 32 tiles x 1 word... */
		for (int t = 0; t < 64; t++) {
			const uint8_t *tp = src + t * 16;
			int col = t & 31, row = t >> 5;
			for (int r = 0; r < 8; r++)
				dst[(row * 8 + r) * 32 + col] = spread[tp[r]] | (spread[tp[r + 8]] << 1);
		}
		RECT rc = { CHR_X, b * 16, 64, 16 };
		LoadImage(&rc, dst);
	}
	chr_dirty = 0;
}

static void upload_pal(void) {
	if (!memcmp(last_pal, ppu.pal, 32)) return;
	memcpy(last_pal, ppu.pal, 32);
	static uint16_t cl[8][16];
	for (int p = 0; p < 8; p++) {
		cl[p][0] = 0;
		for (int c = 1; c < 4; c++) cl[p][c] = nes15[ppu.pal[p * 4 + c] & 63];
		for (int c = 4; c < 16; c++) cl[p][c] = 0;
	}
	RECT rc = { 0, CLUT_Y, 128, 1 };
	LoadImage(&rc, (uint32_t *)cl);
}

#define ALLOC(T) ((T *)(nextp += sizeof(T), nextp - sizeof(T)))
static inline void link(void *p) { addPrim(ot[db], p); }

/* a OT de 1 entrada empilha em ordem inversa; montamos numa lista propria e ligamos no fim */
static void *plist[4096]; static int np;
static inline void push(void *p) { plist[np++] = p; }

static void add_fill(int x, int y, int w, int h, uint16_t c15) {
	FILL *f = ALLOC(FILL); setFill(f);
	setXY0(f, x, y + (db ? 0 : 240)); setWH(f, w, h);
	setRGB0(f, (c15 & 31) << 3, ((c15 >> 5) & 31) << 3, ((c15 >> 10) & 31) << 3);
	push(f);
}
static void add_area(int y0, int y1) {
	DR_AREA *a = ALLOC(DR_AREA);
	RECT r = { 0, y0 + (db ? 0 : 240), 256, y1 - y0 };
	setDrawArea(a, &r); push(a);
}
static void add_tpage(void) {
	DR_TPAGE *t = ALLOC(DR_TPAGE);
	setDrawTPage(t, 0, 1, getTPage(0, 0, CHR_X, 0)); push(t);
}

/* tile fisico -> u,v na textura */
static inline void tile_uv(int bank, int t, uint8_t *u, uint8_t *v) {
	*u = (t & 31) * 8; *v = bank * 16 + (t >> 5) * 8;
}

static void draw_sprites(int behind) {
	int h = (ppu.ctrl & 0x20) ? 16 : 8;
	for (int s = 63; s >= 0; s--) {
		uint8_t *o = ppu.oam + s * 4;
		if (((o[2] >> 5) & 1) != behind) continue;
		int y = o[0] + 1; if (y >= 240) continue;
		int x = o[3], attr = o[2];
		for (int part = 0; part < h / 8; part++) {
			uint16_t pa;
			int tilepart = (attr & 0x80) ? (h / 8 - 1 - part) : part;
			if (h == 16) pa = ((o[1] & 1) << 12) | (((o[1] & 0xfe) + tilepart) << 4);
			else pa = ((ppu.ctrl & 8) ? 0x1000 : 0) | (o[1] << 4);
			int bank = bank_of(pa >> 10), t = (pa >> 4) & 63;
			uint8_t u, v; tile_uv(bank, t, &u, &v);
			int yy = y + part * 8 + (db ? 0 : 0);
			POLY_FT4 *q = ALLOC(POLY_FT4); setPolyFT4(q); setShadeTex(q, 1);
			setXY4(q, x, yy, x + 8, yy, x, yy + 8, x + 8, yy + 8);
			int u0 = u, u1 = u + 8, v0 = v, v1 = v + 8;
			if (attr & 0x40) { u0 = u + 7; u1 = u - 1; }
			if (attr & 0x80) { v0 = v + 7; v1 = v - 1; }
			setUV4(q, u0, v0, u1, v0, u0, v1, u1, v1);
			q->clut = getClut(16 * (4 + (attr & 3)), CLUT_Y);
			q->tpage = getTPage(0, 0, CHR_X, 0);
			setRGB0(q, 128, 128, 128);
			push(q);
		}
	}
}

static void draw_segment(const Seg *s) {
	if (!(s->mask & 8)) return;
	int fy = (s->v >> 12) & 7, cy = (s->v >> 5) & 31, cx = s->v & 31, ntq = (s->v >> 10) & 3;
	int scroll_x = cx * 8 + s->x;
	/* y do topo da tela "virtual" para que a linha y0 mostre a linha (cy*8+fy) */
	int base_y = s->y0 - (cy * 8 + fy);
	add_area(s->y0, s->y1);
	int pbase = (s->ctrl & 0x10) ? 4 : 0;
	int rows = (s->y1 - s->y0 + fy + 7) / 8 + 1;
	for (int r = 0; r < rows; r++) {
		int ty = cy + r, q = ntq;
		/* vertical: 30 linhas de tiles por nametable */
		while (ty >= 30) { ty -= 30; q ^= 2; }
		int sy = base_y + (cy + r) * 8;
		if (sy + 8 <= s->y0 || sy >= s->y1) continue;
		for (int c = 0; c < 33; c++) {
			int tx = cx + c, qq = q;
			if (tx >= 32) { tx -= 32; qq ^= 1; }
			const uint8_t *nt = ppu.nt + s->nt[qq] * 0x400;
			uint8_t idx = nt[ty * 32 + tx];
			uint8_t at = nt[0x3c0 + (ty >> 2) * 8 + (tx >> 2)];
			int pal = (at >> (((ty & 2) << 1) | (tx & 2))) & 3;
			int bank = s->bank[pbase + (idx >> 6)], t = idx & 63;
			SPRT_8 *sp = ALLOC(SPRT_8); setSprt8(sp); setShadeTex(sp, 1);
			setXY0(sp, c * 8 - (s->x), sy + (db ? 0 : 0));
			uint8_t u, v; tile_uv(bank, t, &u, &v);
			setUV0(sp, u, v);
			sp->clut = getClut(16 * pal, CLUT_Y);
			setRGB0(sp, 128, 128, 128);
			push(sp);
		}
	}
	(void)scroll_x;
	if (!(s->mask & 2)) add_fill(0, s->y0, 8, s->y1 - s->y0, nes15[ppu.pal[0] & 63]);
}

void video_set_skip(int skip) { skip_draw = skip; }

void gpu_frame_end(void) {
	Seg *segs_end = segs + nseg;
	if (skip_draw) { nseg = 0; return; }
	upload_chr();
	upload_pal();
	nextp = primbuf[db]; np = 0;
	ClearOTagR(ot[db], 1);
	/* deslocamento de desenho: buffer de tras */
	add_area(0, 240);
	DR_OFFSET *off = ALLOC(DR_OFFSET); setDrawOffset(off, 0, db ? 0 : 240); push(off);
	add_tpage();
	add_fill(0, 0, 256, 240, nes15[ppu.pal[0] & 63]);
	int sp_on = ppu.mask & 0x10;
	if (sp_on) draw_sprites(1);
	for (Seg *s = segs; s < segs_end; s++) draw_segment(s);
	add_area(0, 240);
	if (sp_on) draw_sprites(0);
	if (sp_on && !(ppu.mask & 4)) add_fill(0, 0, 8, 240, nes15[ppu.pal[0] & 63]);
	/* encadeia na ordem em que foi montado */
	for (int i = np - 1; i >= 0; i--) addPrim(ot[db], plist[i]);
	nseg = 0;
}

void video_present(void) {
	DrawSync(0);
	if (!skip_draw) {
		PutDispEnv(&disp[db ? 0 : 1]);
		DrawOTag(ot[db]);
		db ^= 1;
	}
}
