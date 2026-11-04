/*
 * FM-LOADER: menu de boot para o CD que junta Front Mission 1st e Front Mission 2.
 * Tela 640x480 entrelacada. As duas telas (uma por jogo selecionado) vem prontas do
 * MENU.DAT, geradas a partir do mockup; o PS1 so desenha o disco do jogo
 * selecionado girando por cima. Ao confirmar, aplica a configuracao do kernel do
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

#define SCR_W 640
#define SCR_H 480
#define SCR_BYTES (SCR_W * SCR_H * 2)
#define DAT_SECTORS 626               /* MENU.DAT = 2048 de cabecalho + 2 telas + 2 discos */

typedef struct {
	char     magic[4];
	uint16_t disc_x[2], disc_y[2];    /* intercalado no arquivo: x0,y0,x1,y1 */
	uint16_t disc_size;
} Header;

static uint8_t dat[DAT_SECTORS * 2048];
static uint16_t *screen[2];
static uint16_t *disc_tex[2];
static int disc_x[2], disc_y[2], dsize;

static uint16_t restore_buf[128 * 128];
static uint8_t pad_buf[2][34];

static DISPENV disp;
static DRAWENV draw;
static uint32_t ot[2];
static POLY_FT4 quad;

typedef struct {
	const char *path;
	int evcb, tcb;
	uint32_t stack;
} Game;

/* valores tirados do SYSTEM.CNF de cada jogo */
static const Game games[2] = {
	{ "cdrom:\\SLPM_873.17;1", 16, 4, 0x801fff00 },  /* Front Mission 1st */
	{ "cdrom:\\SLPM_873.31;1", 24, 8, 0x801ffff0 },  /* Front Mission 2 */
};

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

static void upload_screen(int sel) {
	RECT r = { 0, 0, SCR_W, SCR_H };
	LoadImage(&r, (const uint32_t *)screen[sel]);
	DrawSync(0);
}

/* repoe o pedaco da tela onde o disco gira */
static void restore_disc_area(int sel) {
	int x = disc_x[sel], y = disc_y[sel];
	uint16_t *src = screen[sel] + y * SCR_W + x;
	for (int j = 0; j < dsize; j++)
		memcpy(restore_buf + j * dsize, src + j * SCR_W, dsize * 2);
	RECT r = { x, y, dsize, dsize };
	LoadImage(&r, (const uint32_t *)restore_buf);
	DrawSync(0);
}

/* desenha o disco do jogo selecionado girado em 'ang' (4096 = volta inteira) */
static void draw_disc(int sel, int ang) {
	int h = dsize / 2;
	int cx = disc_x[sel] + h, cy = disc_y[sel] + h;
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
	int u = sel ? 128 : 0;
	setUV4(&quad, u, 0, u + dsize - 1, 0, u, dsize - 1, u + dsize - 1, dsize - 1);
	quad.tpage = getTPage(2, 0, 640, 0);
	quad.clut = 0;
	setRGB0(&quad, 128, 128, 128);
	termPrim(&quad);
	DrawPrim(&quad);
	DrawSync(0);
}

static void load_data(void) {
	CdlFILE f;
	if (!CdSearchFile(&f, "\\FMMENU.DAT;1"))
		for (;;) VSync(0);
	CdControl(CdlSetloc, &f.pos, 0);
	CdRead(DAT_SECTORS, (uint32_t *)dat, CdlModeSpeed);
	CdReadSync(0, 0);

	const uint16_t *h = (const uint16_t *)(dat + 4);
	disc_x[0] = h[0]; disc_y[0] = h[1];
	disc_x[1] = h[2]; disc_y[1] = h[3];
	dsize = h[4];
	screen[0] = (uint16_t *)(dat + 2048);
	screen[1] = (uint16_t *)(dat + 2048 + SCR_BYTES);
	disc_tex[0] = (uint16_t *)(dat + 2048 + 2 * SCR_BYTES);
	disc_tex[1] = disc_tex[0] + dsize * dsize;

	RECT r0 = { 640, 0, dsize, dsize }, r1 = { 768, 0, dsize, dsize };
	LoadImage(&r0, (const uint32_t *)disc_tex[0]);
	LoadImage(&r1, (const uint32_t *)disc_tex[1]);
	DrawSync(0);
}

static void launch(int g) {
	const Game *gm = &games[g];

	/* tela preta antes de sair */
	VSync(0);
	clear_screen();

	StopPAD();
	StopCallback();
	EnterCriticalSection();
	SetConf(gm->evcb, gm->tcb, gm->stack);
	ExitCriticalSection();
	_96_remove();
	_96_init();
	/* LoadExec da BIOS: (arquivo, base da pilha, deslocamento) */
	LoadExec(gm->path, (int)gm->stack, (const char **)0);
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
	upload_screen(sel);

	for (;;) {
		VSync(0);
		PADTYPE *pad = (PADTYPE *)pad_buf[0];
		uint16_t btn = 0xffff;
		if (pad->stat == 0 && (pad->type == 0x4 || pad->type == 0x5 || pad->type == 0x7))
			btn = pad->btn;
		uint16_t press = prev & ~btn;     /* botoes ativos em 0 */
		prev = btn;

		if (press & (PAD_LEFT | PAD_RIGHT)) {
			sel ^= 1;
			upload_screen(sel);
		}
		if (press & (PAD_CROSS | PAD_CIRCLE | PAD_START))
			launch(sel);

		restore_disc_area(sel);
		draw_disc(sel, ang);
		ang = (ang + 12) & 4095;          /* ~5,7 s por volta */
	}
}
