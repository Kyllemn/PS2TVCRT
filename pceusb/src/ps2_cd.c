/*
 * ps2_cd.c - disco do leitor USB para o Mednafen PCE (PS2).
 * Criado por Leonardo Torres.
 *
 * Mesmo desenho do RETROVOLT (360, x360_cd.c), com o cdcache por baixo:
 *  - dados: o leitor entrega 2048 bytes; o setor cru de 2352 e montado aqui
 *    (sincronismo + cabecalho; sem EDC/ECC: o nucleo nao confere no PS2);
 *  - musica: SEM esperar (o que nao chegou vira silencio: a emulacao nunca
 *    para por I/O);
 *  - subcanal Q gerado pela TOC;
 *  - CDIF_HintReadSector (busca do jogo) -> cdcd_seek: o leitor ja comeca a
 *    ler antes do "toca".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "cdrom/CDAccess.h"
#include "cdrom/CDUtility.h"
#include "cdcache.h"
#include "diag.h"

#define DATA_TIMEOUT_MS	12000

static TOC disc_toc;
static uint8_t dbuf[2048] __attribute__((aligned(64)));
static unsigned st_data_miss, st_audio_miss;

static uint8_t bcd(int v)
{
	return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static void make_subq(int32_t lba, uint8_t *pw)
{
	uint8_t q[12];
	int t = cdcd_track_of(lba), i;
	int32_t rel = lba - disc_toc.tracks[t].lba;
	uint32_t r = rel < 0 ? (uint32_t)(-rel - 1) : (uint32_t)rel;
	uint32_t a = (uint32_t)(lba + 150);

	memset(q, 0, sizeof(q));
	q[0] = 0x01 | (disc_toc.tracks[t].control << 4);
	q[1] = U8_to_BCD(t);
	q[2] = U8_to_BCD(rel < 0 ? 0 : 1);
	q[3] = U8_to_BCD(r / 75 / 60);
	q[4] = U8_to_BCD((r / 75) % 60);
	q[5] = U8_to_BCD(r % 75);
	q[7] = U8_to_BCD(a / 75 / 60);
	q[8] = U8_to_BCD((a / 75) % 60);
	q[9] = U8_to_BCD(a % 75);
	subq_generate_checksum(q);

	memset(pw, 0, 96);
	for (i = 0; i < 96; i++)
		pw[i] = ((q[i >> 3] >> (7 - (i & 7))) & 1) ? 0x40 : 0x00;
}

static bool x_Read_Raw_Sector(struct CDAccess *self, uint8_t *buf, int32_t lba)
{
	int t;
	(void)self;

	if (lba < 0 || lba >= disc_toc.tracks[100].lba) {
		synth_leadout_sector_lba(0x01, &disc_toc, lba, buf);
		return true;
	}
	t = cdcd_track_of(lba);
	if (cdcd_is_data(t)) {
		int a = lba + 150, ok = cdcd_read(lba, 1, dbuf, DATA_TIMEOUT_MS) == 1;
		memset(buf, 0, 2352);
		memset(buf + 1, 0xff, 10);
		buf[12] = bcd(a / 75 / 60);
		buf[13] = bcd((a / 75) % 60);
		buf[14] = bcd(a % 75);
		buf[15] = 1;				/* modo 1 */
		if (ok)
			memcpy(buf + 16, dbuf, 2048);
		else if (st_data_miss++ < 20)
			L("[CD] setor de dados %d nao chegou", (int)lba);
	} else if (cdcd_read_nb(lba, 1, buf) != 1) {
		memset(buf, 0, 2352);			/* silencio */
		st_audio_miss++;
	}
	make_subq(lba, buf + 2352);
	return true;
}

static bool x_Fast_Read_Raw_PW_TSRE(struct CDAccess *self, uint8_t *pwbuf, int32_t lba)
{
	(void)self;
	if (lba >= disc_toc.tracks[100].lba)
		subpw_synth_leadout_lba(&disc_toc, lba, pwbuf);
	else
		make_subq(lba, pwbuf);
	return true;
}

static bool x_Read_TOC(struct CDAccess *self, TOC *toc)
{
	(void)self;
	*toc = disc_toc;
	return true;
}

static void x_destroy(struct CDAccess *self)
{
	free(self);
}

/* gancho do nucleo (CDAccess.c): caminho "ps2disc:" */
CDAccess *CDAccess_PS2_New(const char *path)
{
	const cdcd_toc *c = cdcd_get_toc();
	CDAccess *a;
	int t;

	(void)path;
	memset(&disc_toc, 0, sizeof(disc_toc));
	disc_toc.first_track = c->first;
	disc_toc.last_track = c->last;
	disc_toc.disc_type = DISC_TYPE_CDDA_OR_M1;
	for (t = c->first; t <= c->last; t++) {
		disc_toc.tracks[t].adr = 1;
		disc_toc.tracks[t].control = c->ctrl[t];
		disc_toc.tracks[t].lba = c->lba[t];
		disc_toc.tracks[t].valid = true;
	}
	disc_toc.tracks[100].adr = 1;
	disc_toc.tracks[100].control = disc_toc.tracks[c->last].control & 0x4;
	disc_toc.tracks[100].lba = c->lba[100];
	disc_toc.tracks[100].valid = true;

	a = (CDAccess *)calloc(1, sizeof(*a));
	if (!a)
		return NULL;
	a->Read_Raw_Sector = x_Read_Raw_Sector;
	a->Fast_Read_Raw_PW_TSRE = x_Fast_Read_Raw_PW_TSRE;
	a->Read_TOC = x_Read_TOC;
	a->destroy = x_destroy;
	L("[CD] leitor USB ligado ao Mednafen: faixas %d a %d, fim %d", c->first, c->last, c->lba[100]);
	return a;
}

/* busca do jogo (CDIF_HintReadSector): o leitor ja comeca a ler ali */
void ps2_cd_hint(int lba)
{
	cdcd_seek(lba);
}

void ps2_cd_stats(unsigned *data_miss, unsigned *audio_miss)
{
	*data_miss = st_data_miss;
	*audio_miss = st_audio_miss;
}
