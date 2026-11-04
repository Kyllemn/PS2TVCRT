/* psxnes - frontend do PS1: carrega GAME.NES do CD, roda 1 frame do NES por VSync. */
#include <stdint.h>
#include <string.h>
#include <psxapi.h>
#include <psxetc.h>
#include <psxgpu.h>
#include <psxpad.h>
#include <psxcd.h>
#include "../core/nes.h"

void video_init(void);
void video_present(void);
void video_set_skip(int skip);

static uint8_t rom[0x80000 + 0x4000 + 0x800];   /* ate 512 KB PRG + cabecalho */
static uint8_t pad_buf[2][34];

static void die(void) { for (;;) VSync(0); }

static uint32_t load_rom(void) {
	CdlFILE f;
	if (!CdSearchFile(&f, "\\GAME.NES;1")) die();
	uint32_t size = f.size;
	if (size > sizeof rom) die();
	CdControl(CdlSetloc, &f.pos, 0);
	CdRead((size + 2047) / 2048, (uint32_t *)rom, CdlModeSpeed);
	CdReadSync(0, 0);
	return size;
}

static uint8_t read_pad(void) {
	PADTYPE *p = (PADTYPE *)pad_buf[0];
	if (p->stat != 0) return 0;
	uint16_t b = ~p->btn;
	uint8_t n = 0;
	if (b & (PAD_CROSS | PAD_CIRCLE)) n |= 0x01;     /* A */
	if (b & (PAD_SQUARE | PAD_TRIANGLE)) n |= 0x02;  /* B */
	if (b & PAD_SELECT) n |= 0x04;
	if (b & PAD_START) n |= 0x08;
	if (b & PAD_UP) n |= 0x10;
	if (b & PAD_DOWN) n |= 0x20;
	if (b & PAD_LEFT) n |= 0x40;
	if (b & PAD_RIGHT) n |= 0x80;
	return n;
}

int main(void) {
	ResetGraph(0);
	video_init();
	InitPAD(pad_buf[0], 34, pad_buf[1], 34);
	StartPAD();
	ChangeClearPAD(0);
	CdInit();
	uint32_t size = load_rom();
	if (nes_load(rom, size)) die();
	nes_reset();

	int last = VSync(-1);
	for (;;) {
		pad_state = read_pad();
		nes_frame();
		/* se a emulacao passou de 1 VSync, nao desenha a proxima frame (mantem a velocidade do jogo) */
		int now = VSync(-1);
		int late = (now - last) > 1;
		VSync(0);
		video_present();
		video_set_skip(late);
		last = VSync(-1);
	}
}
