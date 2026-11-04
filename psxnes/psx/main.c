/* psxnes - frontend do PS1: carrega GAME.NES do CD, roda 1 frame do NES por VSync. */
#include <stdint.h>
#include <string.h>
#include <psxapi.h>
#include <psxetc.h>
#include <psxgpu.h>
#include <psxpad.h>
#include <psxcd.h>
#include "../core/nes.h"
#include <hwregs_c.h>
/* perfil (leitura pelo depurador): tempos medios por frame em unidades de 8 ciclos */
volatile uint32_t prof_emu, prof_snd, prof_wait, prof_frames, prof_late;
static inline uint16_t tnow(void) { return TIMER_VALUE(2); }

void video_init(void);
void video_present(void);
void video_set_skip(int skip);
void sound_init(void);
void sound_update(void);
void save_init(uint32_t crc);
void save_frame(int force);
static uint16_t raw_btn;

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
	uint16_t b = ~p->btn; raw_btn = b;
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
	InitCARD(1);
	StartCARD();
	_bu_init();
	InitPAD(pad_buf[0], 34, pad_buf[1], 34);
	StartPAD();
	ChangeClearPAD(0);
	sound_init();
	CdInit();
	uint32_t size = load_rom();
	if (nes_load(rom, size)) die();
	uint32_t crc = 0xffffffff;
	for (uint32_t i = 16; i < size; i++) { crc ^= rom[i]; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320 & -(crc & 1)); }
	save_init(~crc);
	nes_reset();

	TIMER_CTRL(2) = 0x0200;   /* contador 2: clock do sistema / 8 */
	int last = VSync(-1);
	int prev_btn_combo = 0;
	for (;;) {
		pad_state = read_pad();
		uint16_t t0 = tnow();
		nes_frame();
		uint16_t t1 = tnow();
		sound_update();
		save_frame((raw_btn & (PAD_L1 | PAD_R1 | PAD_SELECT)) == (PAD_L1 | PAD_R1 | PAD_SELECT) && !(prev_btn_combo));
		prev_btn_combo = (raw_btn & (PAD_L1 | PAD_R1 | PAD_SELECT)) == (PAD_L1 | PAD_R1 | PAD_SELECT);
		/* ritmo de 60 frames/s com recuperacao: 'last' e o vblank em que esta frame deveria terminar.
		 * Se a emulacao atrasou, a proxima frame nao e desenhada (so emulada) ate alcancar o relogio;
		 * assim o jogo e o som mantem a velocidade certa mesmo com picos de carga. */
		int now = VSync(-1);
		int late = now > last + 1;
		uint16_t t2 = tnow();
		if (now > last + 4) last = now - 1;        /* muito atras (ex.: leitura do cartao): ressincroniza */
		while (VSync(-1) <= last) ;
		prof_emu += (uint16_t)(t1 - t0); prof_snd += (uint16_t)(t2 - t1); prof_wait += (uint16_t)(tnow() - t2); prof_frames++; prof_late += late;
		video_present();
		video_set_skip(late);
		last++;
	}
}
