/* psxnes - bateria do cartucho (PRG-RAM 8 KB) no memory card do slot 1.
 * - ao iniciar: le o save, se existir;
 * - salvamento automatico: a cada 15 frames compara a PRG-RAM com a ultima copia; quando o jogo
 *   reescreve um bloco grande de uma vez (o que acontece ao salvar), grava no cartao assim que a
 *   copia termina;
 * - L1+R1+SELECT forca a gravacao. */
#include <stdint.h>
#include <string.h>
#include <psxapi.h>
#include "../core/nes.h"

#define FREAD 1
#define FWRITE 2
#define FCREAT 0x200
#define BLOCKS 2

static char path[32];
static uint8_t file[BLOCKS * 8192];
static uint8_t snap[0x2000];
static int pending, tick;
int save_enabled;

static void sjis_title(uint8_t *dst, const char *s) {
	int o = 0;
	for (; *s && o < 62; s++) {
		uint16_t c;
		if (*s == ' ') c = 0x8140;
		else if (*s >= '0' && *s <= '9') c = 0x824f + (*s - '0');
		else if (*s >= 'A' && *s <= 'Z') c = 0x8260 + (*s - 'A');
		else if (*s >= 'a' && *s <= 'z') c = 0x8281 + (*s - 'a');
		else if (*s == '-') c = 0x817c;
		else c = 0x8140;
		dst[o++] = c >> 8; dst[o++] = c & 0xff;
	}
}

static void build_header(const char *title) {
	memset(file, 0, 0x100);
	file[0] = 'S'; file[1] = 'C'; file[2] = 0x11; file[3] = BLOCKS;
	sjis_title(file + 4, title);
	/* paleta do icone (15 bits) e um icone 16x16 simples: cartucho estilizado */
	uint16_t *clut = (uint16_t *)(file + 0x60);
	clut[0] = 0; clut[1] = 0x8000 | (6 << 10) | (6 << 5) | 6; clut[2] = 0x8000 | (20 << 10) | (20 << 5) | 20; clut[3] = 0x8000 | (4 << 10) | (8 << 5) | 28;
	uint8_t *ic = file + 0x80;
	for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x += 2) {
		int c[2];
		for (int k = 0; k < 2; k++) {
			int xx = x + k;
			c[k] = (xx >= 2 && xx < 14 && y >= 1 && y < 15) ? 1 : 0;
			if (c[k] && xx >= 4 && xx < 12 && y >= 3 && y < 9) c[k] = 2;
			if (c[k] && y >= 11 && y < 13 && xx >= 5 && xx < 11) c[k] = 3;
		}
		ic[y * 8 + x / 2] = c[0] | (c[1] << 4);
	}
}

void save_init(uint32_t crc) {
	static const char hx[] = "0123456789ABCDEF";
	strcpy(path, "bu00:BINES");
	for (int i = 0; i < 8; i++) path[10 + i] = hx[(crc >> (28 - 4 * i)) & 15];
	path[18] = 0;
	save_enabled = cart.battery;
	if (!save_enabled) return;
	int fd = open(path, FREAD);
	if (fd >= 0) {
		if (read(fd, file, sizeof file) == sizeof file && file[0] == 'S' && file[1] == 'C')
			memcpy(cart.prgram, file + 0x100, 0x2000);
		close(fd);
	}
	memcpy(snap, cart.prgram, 0x2000);
}

static void write_card(void) {
	build_header("NES SAVE");
	memcpy(file + 0x100, cart.prgram, 0x2000);
	int fd = open(path, FWRITE);
	if (fd < 0) {
		fd = open(path, FCREAT | (BLOCKS << 16));
		if (fd >= 0) close(fd);
		fd = open(path, FWRITE);
	}
	if (fd >= 0) { write(fd, file, sizeof file); close(fd); }
}

/* chamado uma vez por frame; force = combinacao de botoes */
void save_frame(int force) {
	if (!save_enabled) return;
	if (force) { write_card(); memcpy(snap, cart.prgram, 0x2000); pending = 0; return; }
	if (++tick < 15) return;
	tick = 0;
	const uint32_t *a = (const uint32_t *)cart.prgram, *b = (const uint32_t *)snap;
	int diff = 0;
	for (int i = 0; i < 0x800; i++) diff += a[i] != b[i];
	memcpy(snap, cart.prgram, 0x2000);
	if (diff >= 64) pending = 1;               /* bloco grande reescrito (~256 bytes ou mais) */
	else if (pending) { write_card(); pending = 0; }
}
