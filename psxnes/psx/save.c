/* psxnes - bateria do cartucho (PRG-RAM 8 KB) no memory card do slot 1.
 * - ao iniciar: le o save, se existir (se nao existir, cria o arquivo ja nesse momento);
 * - salvamento automatico: a cada 15 frames compara a PRG-RAM com a ultima copia; quando o jogo
 *   reescreve um bloco grande de uma vez, grava no cartao assim que a copia termina;
 * - a gravacao e assincrona e so dos setores (128 bytes) que mudaram: um setor por vez, com o
 *   jogo rodando (antes a gravacao parava o jogo 1-2 s em toda troca de mapa no FF3, que usa a
 *   PRG-RAM tambem como memoria de trabalho);
 * - L1+R1+SELECT forca a gravacao. */
#include <stdint.h>
#include <string.h>
#include <psxapi.h>
#include "../core/nes.h"

#define FREAD 1
#define FWRITE 2
#define FCREAT 0x200
#define FASYNC_ 0x8000
#define BLOCKS 2

static char path[32];
static uint8_t file[BLOCKS * 8192];
static uint8_t snap[0x2000];
static uint8_t card[0x2000];      /* PRG-RAM como esta gravada no cartao */
static int pending, tick;
static int afd = -1, cur = -1, timeout;   /* gravacao assincrona em andamento */
static int ev[4];
volatile int dbg_save[12];
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

static void events_init(void) {
	static const uint32_t spec[4] = { EvSpIOE, EvSpERROR, EvSpTIMOUT, EvSpNEW };
	EnterCriticalSection();
	for (int i = 0; i < 4; i++) { ev[i] = OpenEvent(SwCARD, spec[i], EvMdNOINTR, 0); EnableEvent(ev[i]); }
	ExitCriticalSection();
}
static void events_clear(void) { for (int i = 0; i < 4; i++) TestEvent(ev[i]); }
/* espera um evento do cartao: 0 = ok, 1 = erro, 2 = tempo esgotado, 3 = cartao novo, -1 = nada */
static int events_wait(void) {
	for (int n = 0; n < 200000; n++) for (int i = 0; i < 4; i++) if (TestEvent(ev[i])) return i;
	return -1;
}
/* primeiro acesso: a BIOS sinaliza "cartao novo"; reconhece e carrega o diretorio */
static int card_ready(void) {
	for (int t = 0; t < 2; t++) {
		events_clear(); _card_info(0); int r = events_wait(); dbg_save[4 + t] = r;
		if (r == 3) { _new_card(); continue; }
		if (r != 0) continue;
		events_clear(); _card_load(0); r = events_wait(); dbg_save[8 + t] = r;
		if (r == 0) return 1;
	}
	return 0;
}

void save_init(uint32_t crc) {
	static const char hx[] = "0123456789ABCDEF";
	strcpy(path, "bu00:BINES");
	for (int i = 0; i < 8; i++) path[10 + i] = hx[(crc >> (28 - 4 * i)) & 15];
	path[18] = 0;
	save_enabled = cart.battery;
	if (!save_enabled) return;
	events_init();
	/* sem cartao respondendo: desliga o salvamento (cada tentativa falha travava o jogo ~1 s) */
	if (!card_ready()) { save_enabled = 0; return; }
	int fd = open(path, FREAD), ok = 0; dbg_save[3] = fd;
	if (fd >= 0) {
		if (read(fd, file, sizeof file) == sizeof file && file[0] == 'S' && file[1] == 'C') {
			memcpy(cart.prgram, file + 0x100, 0x2000); ok = 1;
		}
		close(fd);
	}
	if (!ok) {   /* cria o arquivo agora (sincrono, so uma vez, antes do jogo comecar) */
		build_header("NES SAVE");
		memcpy(file + 0x100, cart.prgram, 0x2000);
		fd = open(path, FCREAT | (BLOCKS << 16)); dbg_save[0] = fd;
		if (fd >= 0) close(fd);
		fd = open(path, FWRITE); dbg_save[1] = fd;
		if (fd >= 0) { dbg_save[2] = write(fd, file, sizeof file); close(fd); }
	}
	memcpy(card, cart.prgram, 0x2000);
	memcpy(snap, cart.prgram, 0x2000);
}

volatile int save_writes;
/* comeca a gravar o proximo setor diferente; fecha o arquivo quando nao ha mais nenhum */
static void next_sector(void) {
	const uint8_t *d = file + 0x100;
	for (cur++; cur < 64; cur++) if (memcmp(d + cur * 128, card + cur * 128, 128)) break;
	if (cur >= 64 || lseek(afd, 0x100 + cur * 128, SEEK_SET) < 0) { close(afd); afd = -1; return; }
	events_clear();
	if (write(afd, d + cur * 128, 128) < 0) { close(afd); afd = -1; return; }
	timeout = 0; save_writes++;
}
static void start_write(void) {
	if (afd >= 0 || !memcmp(cart.prgram, card, 0x2000)) return;
	memcpy(file + 0x100, cart.prgram, 0x2000);       /* copia estavel durante a gravacao */
	afd = open(path, FWRITE | FASYNC_);
	if (afd < 0) return;
	cur = -1; next_sector();
}
static void poll_write(void) {
	if (TestEvent(ev[0])) {
		memcpy(card + cur * 128, file + 0x100 + cur * 128, 128);
		next_sector();
	} else if (TestEvent(ev[1]) || TestEvent(ev[2]) || TestEvent(ev[3]) || ++timeout > 180) {
		close(afd); afd = -1; pending = 1;            /* falhou: tenta de novo mais tarde */
	}
}

/* chamado uma vez por frame; force = combinacao de botoes */
void save_frame(int force) {
	if (!save_enabled) return;
	if (afd >= 0) { poll_write(); return; }
	if (force) { start_write(); memcpy(snap, cart.prgram, 0x2000); pending = 0; return; }
	if (++tick < 15) return;
	tick = 0;
	const uint32_t *a = (const uint32_t *)cart.prgram, *b = (const uint32_t *)snap;
	int diff = 0;
	for (int i = 0; i < 0x800; i++) diff += a[i] != b[i];
	memcpy(snap, cart.prgram, 0x2000);
	if (diff >= 64) pending = 1;               /* bloco grande reescrito (~256 bytes ou mais) */
	else if (pending) { start_write(); pending = 0; }
}
