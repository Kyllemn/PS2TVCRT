/*
 * Loader de coletanea (2 a 4 jogos num CD): menu de boot em 640x480 entrelacado.
 * O MENU.DAT (tools/menu_dat.py) traz a lista de jogos, a tela base e, para cada jogo
 * selecionado, so os retangulos que mudam; o PS1 desenha o disco do jogo selecionado
 * girando por cima. Ao confirmar, aplica a configuracao do kernel do
 * jogo (a do SYSTEM.CNF dele) e chama o LoadExec da BIOS.
 */
#include <stdint.h>
#include <string.h>
#include <psxapi.h>
#include <psxetc.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <psxcd.h>
#include <hwregs_c.h>

#define SCR_W 640
#define SCR_H 480
#define MAX_GAMES 4
#define DAT_MAX (1400 * 1024)         /* MENU.DAT inteiro na RAM */

/* MENU.DAT 'MNU2' (gerado por tools/menu_dat.py) */
typedef struct {
	char     exe[40];
	uint16_t tcb, event;
	uint32_t stack;
	uint16_t disc_x, disc_y;
	uint32_t patch_off, rects_off;
	uint16_t nrects, pad;
} GameEnt;
typedef struct {
	char     magic[4];
	uint16_t n, dsize;
	uint32_t base_off, disc_off;
	GameEnt  g[MAX_GAMES];
} Header;

static uint8_t dat[DAT_MAX] __attribute__((aligned(4)));
static Header *hd;
static uint16_t *base;
static int ngames, dsize;

static uint16_t tmp_buf[SCR_W * 16];
static uint8_t pad_buf[2][34];

static DISPENV disp;
static DRAWENV draw;
static POLY_FT4 quad;
static uint32_t ot[1];

static void clear_screen(void) {
	FILL f;
	setFill(&f);
	setXY0(&f, 0, 0);
	setWH(&f, SCR_W, SCR_H);
	setRGB0(&f, 0, 0, 0);
	termPrim(&f);
	DrawPrim(&f);
	DrawSync(0);
}

/* envia um retangulo da tela base (linhas de 640) para a VRAM */
static void load_from_base(int x, int y, int w, int h) {
	for (int j = 0; j < h; j++) memcpy(tmp_buf + j * w, base + (y + j) * SCR_W + x, w * 2);
	RECT r = { x, y, w, h };
	LoadImage(&r, (const uint32_t *)tmp_buf);
	DrawSync(0);
}
/* troca a tela: volta os retangulos do jogo anterior para a base e aplica os do novo */
static void show(int from, int to) {
	if (from >= 0) {
		const uint8_t *p = dat + hd->g[from].rects_off;
		for (int i = 0; i < hd->g[from].nrects; i++) {
			const uint16_t *r = (const uint16_t *)p;
			load_from_base(r[0], r[1], r[2], r[3]);
			p += 8 + r[2] * r[3] * 2;
		}
	}
	const uint8_t *p = dat + hd->g[to].rects_off;
	for (int i = 0; i < hd->g[to].nrects; i++) {
		const uint16_t *r = (const uint16_t *)p;
		RECT rc = { r[0], r[1], r[2], r[3] };
		LoadImage(&rc, (const uint32_t *)(p + 8));
		DrawSync(0);
		p += 8 + r[2] * r[3] * 2;
	}
}

/* repoe o pedaco da tela onde o disco gira */
static void restore_disc_area(int sel) {
	RECT r = { hd->g[sel].disc_x, hd->g[sel].disc_y, dsize, dsize };
	LoadImage(&r, (const uint32_t *)(dat + hd->g[sel].patch_off));
	DrawSync(0);
}

/* desenha o disco do jogo selecionado girado em 'ang' (4096 = volta inteira) */
static void draw_disc(int sel, int ang) {
	int h = dsize / 2;
	int cx = hd->g[sel].disc_x + h, cy = hd->g[sel].disc_y + h;
	int s = isin(ang), c = icos(ang);
	int px[4] = { -h, h, -h, h }, py[4] = { -h, -h, h, h };
	int rx[4], ry[4];
	for (int i = 0; i < 4; i++) {
		rx[i] = cx + ((px[i] * c - py[i] * s) >> 12);
		ry[i] = cy + ((px[i] * s + py[i] * c) >> 12);
	}
	setPolyFT4(&quad);
	setShadeTex(&quad, 1);
	setXY4(&quad, rx[0], ry[0], rx[1], ry[1], rx[2], ry[2], rx[3], ry[3]);
	/* discos na VRAM a partir de x=640: 128 px por disco (2 por pagina de textura de 64 KB) */
	int u = (sel & 1) * 128;
	setUV4(&quad, u, 0, u + dsize - 1, 0, u, dsize - 1, u + dsize - 1, dsize - 1);
	quad.tpage = getTPage(2, 0, 640 + (sel >> 1) * 256, 0);
	quad.clut = 0;
	setRGB0(&quad, 128, 128, 128);
	/* pela tabela de ordenacao (DMA em lista ligada), o caminho padrao da GPU */
	ClearOTagR(ot, 1);
	addPrim(ot, &quad);
	DrawOTag(ot);
	DrawSync(0);
}

static void load_data(void) {
	CdlFILE f;
	if (!CdSearchFile(&f, "\\MENU.DAT;1"))
		for (;;) VSync(0);
	int sectors = (f.size + 2047) / 2048;
	if (sectors * 2048 > DAT_MAX) for (;;) VSync(0);
	CdControl(CdlSetloc, &f.pos, 0);
	CdRead(sectors, (uint32_t *)dat, CdlModeSpeed);
	CdReadSync(0, 0);

	hd = (Header *)dat;
	ngames = hd->n; dsize = hd->dsize;
	base = (uint16_t *)(dat + hd->base_off);
	for (int i = 0; i < ngames; i++) {
		RECT r = { 640 + (i >> 1) * 256 + (i & 1) * 128, 0, dsize, dsize };
		LoadImage(&r, (const uint32_t *)(dat + hd->disc_off + i * dsize * dsize * 2));
	}
	RECT r = { 0, 0, SCR_W, SCR_H };
	LoadImage(&r, (const uint32_t *)base);
	DrawSync(0);
}

static void launch(int g) {
	const GameEnt *gm = &hd->g[g];
	static char path[41];
	memcpy(path, gm->exe, 40); path[40] = 0;

	/* tela preta antes de sair */
	VSync(0);
	clear_screen();

	StopPAD();
	StopCallback();
	EnterCriticalSection();
	SetConf(gm->event, gm->tcb, gm->stack);
	ExitCriticalSection();
	_96_remove();
	_96_init();
	/* LoadExec da BIOS: (arquivo, base da pilha, deslocamento) */
	LoadExec(path, (int)gm->stack, (const char **)0);
	for (;;);
}

int main(void) {
	ResetGraph(0);
	SetDefDispEnv(&disp, 0, 0, SCR_W, SCR_H);
	disp.isinter = 1;
	SetDefDrawEnv(&draw, 0, 0, SCR_W, SCR_H);
	draw.dfe = 1;
	draw.isbg = 0;
	PutDispEnv(&disp);
	PutDrawEnv(&draw);

	clear_screen();
	SetDispMask(1);

	InitPAD(pad_buf[0], 34, pad_buf[1], 34);
	StartPAD();
	ChangeClearPAD(0);

	CdInit();
	load_data();

	int sel = 0, ang = 0;
	uint16_t prev = 0xffff;
	show(-1, sel);

	for (;;) {
		VSync(0);
		PADTYPE *pad = (PADTYPE *)pad_buf[0];
		uint16_t btn = 0xffff;
		if (pad->stat == 0 && (pad->type == 0x4 || pad->type == 0x5 || pad->type == 0x7))
			btn = pad->btn;
		uint16_t press = prev & ~btn;     /* botoes ativos em 0 */
		prev = btn;

		if (press & (PAD_LEFT | PAD_RIGHT)) {
			int nsel = (press & PAD_LEFT) ? (sel + ngames - 1) % ngames : (sel + 1) % ngames;
			show(sel, nsel);
			sel = nsel;
		}
		if (press & (PAD_CROSS | PAD_CIRCLE | PAD_START))
			launch(sel);

		restore_disc_area(sel);
		draw_disc(sel, ang);
		ang = (ang + 12) & 4095;          /* ~5,7 s por volta */
	}
}
