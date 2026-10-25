/*
 * diag.c - testes do leitor de CD USB (fase 1).
 * Criado por Leonardo Torres.
 *
 * Mesmas licoes do leitor USB do RETROVOLT (Xbox 360):
 *  - TOC: primeiro o cabecalho, depois o tamanho exato;
 *  - dados por READ(10) de 2048; musica por READ CD (0xBE) de 2352;
 *  - num CD misto os 150 setores antes da 1a faixa de musica nao sao lidos
 *    como dados;
 *  - nunca muda a velocidade do leitor.
 */
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <libpad.h>
#include <debug.h>

#include "cdusb_ee.h"
#include "diag.h"

#define TMO_CMD		5000
#define TMO_READ	10000
#define EE_HZ_MS	294912u
#define BENCH_MS	4000
#define MOTOR_WAIT_MS	15000
#define PACE_SECS	15

static int dev, sub;
static diag_result *res;
static cdusb_scsi_resp R;
static u8 buf[CDUSB_DATA_MAX] __attribute__((aligned(64)));

static struct {
	int lba, ctrl;
} trk[101];			/* 1..99 = faixas, 100 = fim do disco */
static int t_first, t_last;

/* ------------------------------------------------------------------ comandos */
static int cdb_len(u8 op)
{
	if (sub == 0x02)
		return 12;	/* subclasse ATAPI: sempre 12 */
	if (op < 0x20)
		return 6;
	if (op < 0x60)
		return 10;
	return 12;
}

static int cmd(const u8 *cdb, int dir, int len, int tmo)
{
	int r = cdusb_scsi(dev, cdb, cdb_len(cdb[0]), dir, buf, len, tmo, &R);
	if (r == CDUSB_E_EE_TIMEOUT)
		res->stuck = 1;
	if (r == CDUSB_E_NODEV)
		res->gone = 1;
	return r;
}

static const char *rtxt(int r)
{
	switch (r) {
	case CDUSB_OK: return "OK";
	case CDUSB_CHECK: return "RECUSADO";
	case CDUSB_PHASE: return "ERRO DE FASE";
	case CDUSB_E_USB: return "ERRO USB";
	case CDUSB_E_TIMEOUT: return "SEM RESPOSTA";
	case CDUSB_E_NODEV: return "SEM LEITOR";
	case CDUSB_E_EE_TIMEOUT: return "IOP PAROU";
	default: return "ERRO";
	}
}

static void lcmd(const char *what, int r)
{
	L("[CD] %s: %s (sense %06X, resto %d, usb %d, %d ms)", what, rtxt(r), R.sense, R.residue,
	  R.usb_rc, R.us / 1000);
}

/* o leitor ainda esta la? */
static int alive(void)
{
	cdusb_status st;

	if (res->stuck || res->gone)
		return 0;
	if (cdusb_get_status(&st) < 0) {
		res->stuck = cdusb_stuck();
		return 0;
	}
	if (st.dev[dev].state != CDUSB_DEV_READY) {
		res->gone = 1;
		L("[CD] leitor DESCONECTOU (estado %d, %d desconexoes)", st.dev[dev].state, st.disconnects);
		return 0;
	}
	return 1;
}

static void put_lba(u8 *c, int lba)
{
	c[2] = lba >> 24; c[3] = lba >> 16; c[4] = lba >> 8; c[5] = lba;
}

static int read10(int lba, int n)
{
	u8 c[12] = {0x28, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	put_lba(c, lba);
	c[7] = n >> 8; c[8] = n;
	return cmd(c, 1, n * 2048, TMO_READ);
}

/* READ CD: tipo 1 = musica (CD-DA), 0 = qualquer; flags 0x10 = so a parte do usuario,
 * 0xF8 = setor cru inteiro; subq 0 = sem subcanal, 1 = P-W cru (96), 2 = Q (16) */
static int readcd(int lba, int n, int type, int flags, int subq)
{
	u8 c[12] = {0xBE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	int extra = subq == 1 ? 96 : subq == 2 ? 16 : 0;
	c[1] = type << 2;
	put_lba(c, lba);
	c[6] = n >> 16; c[7] = n >> 8; c[8] = n;
	c[9] = flags;
	c[10] = subq;
	return cmd(c, 1, n * (2352 + extra), TMO_READ);
}

/* ------------------------------------------------------------------ TOC */
static int track_of(int lba)
{
	int t;
	for (t = t_last; t > t_first; t--)
		if (lba >= trk[t].lba)
			return t;
	return t_first;
}

static int is_data_t(int t)
{
	return (trk[t].ctrl & 4) != 0;
}

static int track_end(int t)
{
	return t < t_last ? trk[t + 1].lba : trk[100].lba;
}

/* fim seguro para ler como dados (pausa antes da musica fica de fora) */
static int safe_end(int t)
{
	int e = track_end(t);
	if (is_data_t(t) && t < t_last && !is_data_t(t + 1) && e - 150 > trk[t].lba)
		e -= 150;
	return e;
}

static int first_of(int data)
{
	int t;
	for (t = t_first; t <= t_last; t++)
		if (is_data_t(t) == data)
			return t;
	return -1;
}

static int data_raw;		/* 1 = dados lidos crus (teste PS1: modo 2 nem sempre aceita READ(10)) */

/* 1 setor, com o comando certo para o tipo da faixa */
static int read_one(int lba)
{
	int t = track_of(lba);
	if (is_data_t(t)) {
		if (lba >= safe_end(t))
			lba = safe_end(t) - 1;
		return data_raw ? readcd(lba, 1, 0, 0xF8, 0) : read10(lba, 1);
	}
	return readcd(lba, 1, 1, 0x10, 0);
}

static int read_toc(void)
{
	u8 c[12] = {0x43, 0, 0, 0, 0, 0, 1, 0, 4, 0, 0, 0};
	int r, len, i, have_end = 0;
	char hx[3 * 24 + 1];

	/* cabecalho primeiro: adaptadores USB se confundem com pedido maior */
	memset(buf, 0, 4);
	r = cmd(c, 1, 4, TMO_CMD);
	lcmd("READ TOC (cabecalho)", r);
	if (r != CDUSB_OK)
		return 0;
	len = (buf[0] << 8 | buf[1]) + 2;
	if (len < 12 || len > 4 + 100 * 8)
		return 0;
	c[7] = len >> 8;
	c[8] = len;
	memset(buf, 0, len);
	r = cmd(c, 1, len, TMO_CMD);
	lcmd("READ TOC", r);
	if (r != CDUSB_OK)
		return 0;
	if (R.residue > 0 && R.residue < len)
		len -= R.residue;
	for (i = 0; i < 24 && i < len; i++)
		snprintf(hx + i * 3, 4, "%02X ", buf[i]);
	hx[i * 3] = 0;
	L("[CD] TOC (%d bytes): %s", len, hx);

	memset(trk, 0, sizeof(trk));
	t_first = buf[2];
	t_last = buf[3];
	for (i = 4; i + 8 <= len; i += 8) {
		int t = buf[i + 2];
		int idx = (t == 0xAA) ? 100 : t;
		if (idx < 1 || idx > 100)
			continue;
		trk[idx].lba = buf[i + 4] << 24 | buf[i + 5] << 16 | buf[i + 6] << 8 | buf[i + 7];
		trk[idx].ctrl = buf[i + 1] & 0x0f;
		if (idx == 100)
			have_end = 1;
	}
	if (t_first < 1 || t_last < t_first || t_last > 99 || !have_end ||
	    trk[100].lba <= trk[t_last].lba || trk[100].lba > 450000) {
		L("[CD] TOC invalida: faixas %d a %d, fim %d", t_first, t_last, trk[100].lba);
		return 0;
	}
	res->first = t_first;
	res->last = t_last;
	res->leadout = trk[100].lba;
	for (i = t_first; i <= t_last; i++) {
		int l = trk[i].lba + 150;
		if (is_data_t(i))
			res->data_tracks++;
		else
			res->audio_tracks++;
		L("[CD] faixa %2d: setor %6d (%02d:%02d:%02d) %s", i, trk[i].lba, l / 4500, (l / 75) % 60,
		  l % 75, is_data_t(i) ? "DADOS" : "MUSICA");
	}
	L("[CD] fim do disco: setor %d (%d min)", trk[100].lba, trk[100].lba / 4500);
	return 1;
}

/* ------------------------------------------------------------------ disco pronto */
static int wait_ready(void)
{
	u8 c[12] = {0};
	int r, last = 0x7fffffff, code, shown = 0, errors = 0, prep = 0;

	for (;;) {
		if (pad_new() & PAD_CIRCLE) {
			res->aborted = 1;
			L("[CD] usuario saiu esperando o disco");
			return 0;
		}
		r = cmd(c, 0, 0, TMO_CMD);
		code = (r == CDUSB_CHECK) ? R.sense : r;
		if (code != last) {
			lcmd("TEST UNIT READY", r);
			last = code;
		}
		if (r == CDUSB_OK)
			return 1;
		if (r < 0) {
			if (!alive() || ++errors > 10)
				return 0;
			ee_wait_ms(500);
			continue;
		}
		if (r == CDUSB_CHECK) {
			int key = (R.sense >> 16) & 0x0f, asc = (R.sense >> 8) & 0xff;
			if (key == 2 && asc == 0x3A) {
				if (shown != 1)
					S("   SEM DISCO: COLOQUE UM DISCO NO LEITOR  (O = SAIR)");
				shown = 1;
				prep = 0;
			} else if (key == 2 && asc == 0x04) {
				if (shown != 2)
					S("   PREPARANDO O DISCO...");
				shown = 2;
				if (++prep > 200) {		/* ~60 s sem ficar pronto */
					L("[CD] o disco nao ficou pronto em 60 s");
					return 0;
				}
			} else if (key == 6) {
				continue;			/* aviso de troca de disco: tenta de novo ja */
			} else if (++errors > 20) {
				return 0;
			}
		}
		ee_wait_ms(300);
	}
}

/* ------------------------------------------------------------------ informacoes */
static const char *profile_txt(int p)
{
	switch (p) {
	case 0x00: return "sem disco";
	case 0x08: return "CD-ROM";
	case 0x09: return "CD-R";
	case 0x0A: return "CD-RW";
	case 0x10: return "DVD-ROM";
	case 0x11: return "DVD-R";
	case 0x12: return "DVD-RAM";
	case 0x13: case 0x14: return "DVD-RW";
	case 0x1A: return "DVD+RW";
	case 0x1B: return "DVD+R";
	default: return "outro";
	}
}

static void get_profile(void)
{
	u8 c[12] = {0x46, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0};
	int r;

	memset(buf, 0, 8);
	r = cmd(c, 1, 8, TMO_CMD);
	lcmd("GET CONFIGURATION", r);
	res->profile = -1;
	if (r == CDUSB_OK) {
		res->profile = buf[6] << 8 | buf[7];
		L("[CD] tipo de disco: %04X (%s)", res->profile, profile_txt(res->profile));
	}
}

/* pagina 2A: velocidades e o que o leitor sabe fazer (so le, nao muda nada) */
static void get_caps(void)
{
	u8 c[12] = {0x5A, 0x08, 0x2A, 0, 0, 0, 0, 0, 8, 0, 0, 0};
	char hx[3 * 24 + 1];
	int r, n, off, i;

	res->max_kbs = res->cur_kbs = -1;
	res->caps = -1;
	memset(buf, 0, 64);
	r = cmd(c, 1, 8, TMO_CMD);
	lcmd("MODE SENSE 2A (cabecalho)", r);
	if (r != CDUSB_OK)
		return;
	n = (buf[0] << 8 | buf[1]) + 2;
	if (n > 64)
		n = 64;
	if (n < 16)
		return;
	c[8] = n;
	memset(buf, 0, 64);
	r = cmd(c, 1, n, TMO_CMD);
	lcmd("MODE SENSE 2A", r);
	if (r != CDUSB_OK)
		return;
	off = 8 + (buf[6] << 8 | buf[7]);
	if (off + 16 > n)
		return;
	for (i = 0; i < 24 && off + i < n; i++)
		snprintf(hx + i * 3, 4, "%02X ", buf[off + i]);
	hx[i * 3] = 0;
	L("[CD] pagina 2A: %s", hx);
	res->caps = buf[off + 5];
	res->max_kbs = buf[off + 8] << 8 | buf[off + 9];
	res->cur_kbs = buf[off + 14] << 8 | buf[off + 15];
	L("[CD] velocidade maxima %d KB/s, atual %d KB/s; musica por comando %s, musica exata %s, "
	  "subcanal %s",
	  res->max_kbs, res->cur_kbs, (res->caps & 1) ? "SIM" : "NAO", (res->caps & 2) ? "SIM" : "NAO",
	  (res->caps & 4) ? "SIM" : "NAO");
}

/* ------------------------------------------------------------------ sistema do disco */
static int find_str(const u8 *b, int n, const char *s)
{
	int l = strlen(s), i;
	for (i = 0; i + l <= n; i++)
		if (b[i] == (u8)s[0] && memcmp(b + i, s, l) == 0)
			return i;
	return -1;
}

static void hex16(const char *what, const u8 *b)
{
	L("[CD] %s: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
	  what, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
	  b[14], b[15]);
}

static void identify(void)
{
	int fd = first_of(1), start, r, k, root, size;

	strcpy(res->system, res->data_tracks ? "CD COM DADOS (SISTEMA DESCONHECIDO)" : "CD DE AUDIO");
	if (fd < 0)
		return;
	start = trk[fd].lba;

	r = read10(start, 1);
	lcmd("READ(10) 1o setor de dados", r);
	if (r != CDUSB_OK)
		return;
	res->data_ok = 1;
	hex16("1o setor de dados", buf);
	if (find_str(buf, 32, "SEGADISCSYSTEM") >= 0) {
		strcpy(res->system, "SEGA CD");
		return;
	}
	if (find_str(buf, 32, "SEGA SEGASATURN") >= 0) {
		strcpy(res->system, "SEGA SATURN");
		return;
	}
	for (k = 0; k < 3 && start + k < safe_end(fd); k++) {
		if (k && read10(start + k, 1) != CDUSB_OK)
			break;
		if (find_str(buf, 2048, "PC Engine CD-ROM SYSTEM") >= 0) {
			strcpy(res->system, "PC ENGINE CD");
			return;
		}
	}
	/* ISO9660: setor 16 */
	if (start + 16 >= safe_end(fd) || read10(start + 16, 1) != CDUSB_OK)
		return;
	if (memcmp(buf + 1, "CD001", 5) != 0) {
		L("[CD] sem ISO9660 no setor 16");
		return;
	}
	L("[CD] ISO9660: sistema \"%.32s\" volume \"%.32s\"", buf + 8, buf + 40);
	if (find_str(buf + 8, 32, "PLAYSTATION") >= 0) {
		strcpy(res->system, "PLAYSTATION");
		return;
	}
	root = buf[158] | buf[159] << 8 | buf[160] << 16 | buf[161] << 24;
	size = buf[166] | buf[167] << 8 | buf[168] << 16 | buf[169] << 24;
	L("[CD] pasta raiz: setor %d, %d bytes", root, size);
	for (k = 0; k < 4 && k * 2048 < size; k++) {
		if (read10(root + k, 1) != CDUSB_OK)
			break;
		if (find_str(buf, 2048, "IPL.TXT") >= 0) {
			strcpy(res->system, "NEO GEO CD");
			return;
		}
	}
}

/* ------------------------------------------------------------------ leituras */
static void test_reads(void)
{
	int fd = first_of(1), fa = first_of(0), r, lba, i, nz;

	if (fd >= 0) {
		r = readcd(trk[fd].lba, 1, 0, 0xF8, 0);
		lcmd("READ CD cru (dados)", r);
		if (r == CDUSB_OK) {
			res->raw_ok = buf[0] == 0 && buf[1] == 0xFF && buf[10] == 0xFF && buf[11] == 0;
			hex16("setor cru", buf);
			L("[CD] setor cru: sincronismo %s, modo %d", res->raw_ok ? "OK" : "ERRADO", buf[15]);
		}
	}
	if (fa < 0)
		return;
	lba = trk[fa].lba + 75 * 10;
	if (lba >= track_end(fa))
		lba = (trk[fa].lba + track_end(fa)) / 2;
	r = readcd(lba, 1, 1, 0x10, 0);
	lcmd("READ CD musica", r);
	if (r == CDUSB_OK) {
		res->audio_ok = 1;
		for (i = nz = 0; i < 2352; i++)
			if (buf[i])
				nz++;
		res->audio_silent = nz == 0;
		hex16("musica", buf);
		L("[CD] musica no setor %d: %d de 2352 bytes diferentes de zero", lba, nz);
	}
	r = readcd(lba, 1, 1, 0x10, 2);
	lcmd("READ CD musica + subcanal Q", r);
	if (r == CDUSB_OK) {
		const u8 *q = buf + 2352;
		hex16("subcanal Q", q);
		res->subq_ok = (q[0] & 0x0f) == 1;
		L("[CD] Q: controle %X, faixa %02X, indice %02X, absoluto %02X:%02X:%02X", q[0] >> 4, q[1],
		  q[2], q[7], q[8], q[9]);
	}
	r = readcd(lba, 1, 1, 0x10, 1);
	lcmd("READ CD musica + subcanal P-W", r);
	res->subraw_ok = r == CDUSB_OK;
}

/* le 'per' setores do jeito do tipo: 0 = dados READ(10), 1 = musica, 2 = dados crus (2352) */
static int bench_read(int kind, int lba, int per)
{
	if (kind == 1)
		return readcd(lba, per, 1, 0x10, 0);
	if (kind == 2)
		return readcd(lba, per, 0, 0xF8, 0);
	return read10(lba, per);
}

/* leitura em sequencia por ms; devolve bytes por segundo (-1 = falhou).
 * Comandos acima de 300 ms vao para o log (setor e tempo).
 * audio: 0 = dados (2048), 1 = musica, 2 = dados crus (2352, como o .bin do PS1) */
static int bench(int audio, int t, int per, int ms, int *ms_max, int *slow)
{
	int start = trk[t].lba, end = audio == 1 ? track_end(t) : safe_end(t);
	int lba, r, n = 0, mx = 0, mn = 1 << 30, c, wraps = 0;
	int bytes_per = audio ? 2352 : 2048;
	u64 t0, c0, bytes = 0;
	u32 total_ms;
	char what[48];

	snprintf(what, sizeof(what), "%s, %d setores por comando",
		 audio == 1 ? "musica" : audio == 2 ? "dados crus" : "dados", per);
	if (end - start < per * 8) {
		L("[BENCH] %s: faixa curta demais", what);
		return -1;
	}
	/* primeira leitura fora da conta (busca + motor) */
	lba = start;
	c0 = ee_now_us();
	r = bench_read(audio, lba, per);
	L("[BENCH] %s: primeira leitura (posiciona) %d ms", what, (int)((ee_now_us() - c0) / 1000));
	if (r != CDUSB_OK) {
		lcmd(what, r);
		return -1;
	}
	lba += per;
	t0 = ee_now_us();
	while (ee_now_us() - t0 < (u64)ms * 1000) {
		if (lba + per > end) {
			lba = start;
			wraps++;
		}
		c0 = ee_now_us();
		r = bench_read(audio, lba, per);
		c = (int)((ee_now_us() - c0) / 1000);
		if (r != CDUSB_OK) {
			lcmd(what, r);
			L("[BENCH] %s: falhou no setor %d depois de %d comandos", what, lba, n);
			return -1;
		}
		if (c > 300) {
			if (slow)
				(*slow)++;
			L("[BENCH] %s: LENTO setor %d em %d ms (comando %d)", what, lba, c, n);
		}
		bytes += (u64)per * bytes_per;
		n++;
		if (c > mx) mx = c;
		if (c < mn) mn = c;
		lba += per;
	}
	total_ms = (u32)((ee_now_us() - t0) / 1000);
	{
		int bps = (int)(bytes * 1000 / (total_ms ? total_ms : 1));
		L("[BENCH] %s: %d comandos, %d KB em %d ms = %d KB/s; comando min %d ms, media %d ms, pior %d ms; "
		  "voltas %d",
		  what, n, (int)(bytes / 1024), (int)total_ms, bps / 1024, mn, (int)(total_ms / (n ? n : 1)), mx,
		  wraps);
		if (ms_max)
			*ms_max = mx;
		return bps;
	}
}

/* maior faixa de musica (-1 = nenhuma) */
static int longest_audio(void)
{
	int t, best = -1;
	for (t = t_first; t <= t_last; t++)
		if (!is_data_t(t) && (best < 0 || track_end(t) - trk[t].lba > track_end(best) - trk[best].lba))
			best = t;
	return best;
}

/*
 * Musica em tempo real: o emulador consome 75 setores por segundo (1x) e a
 * leitura antecipada tenta manter 1 s (75 setores) a frente, lendo 16 por vez.
 * Mede a menor folga e quantas vezes o buffer teria esvaziado (= corte).
 */
#define PACE_AHEAD	75
static void pace(int t, int secs, speed_result *s)
{
	int start = trk[t].lba + 150, end = track_end(t), lba, r, c, cmds = 0;
	int produced = 0, before, consumed, ahead, min_ahead = 1 << 30, in_cut = 0;
	u64 t0, now, c0;

	s->pace_min_ms = s->pace_cuts = s->pace_max_ms = -1;
	if (end - start < 75 * secs + 160) {
		L("[TEMPO REAL] faixa curta demais");
		return;
	}
	/* enche o buffer antes de "tocar" */
	lba = start;
	while (produced < PACE_AHEAD) {
		r = readcd(lba, 16, 1, 0x10, 0);
		if (r != CDUSB_OK) {
			lcmd("TEMPO REAL (enchendo)", r);
			return;
		}
		lba += 16;
		produced += 16;
	}
	s->pace_cuts = 0;
	s->pace_max_ms = 0;
	t0 = ee_now_us();
	for (;;) {
		now = ee_now_us();
		if (now - t0 >= (u64)secs * 1000000)
			break;
		consumed = (int)((now - t0) * 75 / 1000000);
		if (produced - consumed >= PACE_AHEAD) {
			ee_wait_ms(2);
			continue;
		}
		before = produced;
		c0 = ee_now_us();
		r = readcd(lba, 16, 1, 0x10, 0);
		now = ee_now_us();
		c = (int)((now - c0) / 1000);
		if (r != CDUSB_OK) {
			lcmd("TEMPO REAL", r);
			s->pace_cuts = -1;
			return;
		}
		cmds++;
		lba += 16;
		produced += 16;
		/* folga no pior momento: logo antes deste comando terminar */
		ahead = before - (int)((now - t0) * 75 / 1000000);
		if (ahead < min_ahead)
			min_ahead = ahead;
		if (ahead < 0) {
			if (!in_cut)
				s->pace_cuts++;
			in_cut = 1;
		} else
			in_cut = 0;
		if (c > s->pace_max_ms)
			s->pace_max_ms = c;
		if (c > 300)
			L("[TEMPO REAL] LENTO setor %d em %d ms (folga %d ms)", lba - 16, c, ahead * 1000 / 75);
	}
	s->pace_min_ms = min_ahead * 1000 / 75;
	L("[TEMPO REAL] %d s, %d comandos: menor folga %d ms (de 1000), cortes %d, pior comando %d ms", secs, cmds,
	  s->pace_min_ms, s->pace_cuts, s->pace_max_ms);
}

static int pick_far(void)
{
	int lba = trk[100].lba - 600;
	return lba > trk[t_first].lba ? lba : -1;
}

static int pick_near(void)
{
	return trk[t_first].lba + 300;
}

static void test_seek(speed_result *s, int shift)
{
	int near = pick_near() + shift, far = pick_far() - shift, i, r, ms, sum = 0, cnt = 0;
	u64 t0;

	s->seek_avg = s->seek_max = -1;
	if (far < 0 || far - near < 20000) {
		L("[SEEK] disco curto demais para medir a busca");
		return;
	}
	for (i = 0; i < 7; i++) {
		int lba = (i & 1) ? far : near;
		lba += i * 8;			/* setores novos: nada no cache do leitor */
		t0 = ee_now_us();
		r = read_one(lba);
		ms = (int)((ee_now_us() - t0) / 1000);
		L("[SEEK] setor %d: %s em %d ms", lba, rtxt(r), ms);
		if (r != CDUSB_OK)
			return;
		if (i == 0)
			continue;		/* a primeira so posiciona */
		sum += ms;
		cnt++;
		if (ms > s->seek_max)
			s->seek_max = ms;
	}
	s->seek_avg = sum / cnt;
	L("[SEEK] busca media %d ms, pior %d ms", s->seek_avg, s->seek_max);
}

/*
 * Velocidade do leitor USB (autorizado pelo usuario, SO no leitor USB).
 * kbs: KB/s do padrao MMC (1x = 176); 0xFFFF = maxima. Manda SET CD SPEED e
 * SET STREAMING (leitores novos so obedecem a este). restore = volta ao padrao.
 */
static void set_speed(int kbs, int restore, speed_result *s)
{
	u8 c[12], pd[28];
	int r, end = trk[100].lba > 0 ? trk[100].lba - 1 : 0;

	memset(c, 0, sizeof(c));
	c[0] = 0xBB;
	c[2] = kbs >> 8; c[3] = kbs;
	c[4] = 0xFF; c[5] = 0xFF;		/* gravacao: sem mudanca */
	r = cmd(c, 0, 0, TMO_CMD);
	lcmd(restore ? "SET CD SPEED (padrao)" : "SET CD SPEED", r);
	if (r == CDUSB_OK && s)
		s->set_ok |= 1;

	memset(pd, 0, sizeof(pd));
	if (restore) {
		pd[0] = 0x04;			/* RDD: volta ao padrao do leitor */
	} else {
		pd[8] = end >> 24; pd[9] = end >> 16; pd[10] = end >> 8; pd[11] = end;
		pd[14] = kbs >> 8; pd[15] = kbs;		/* KB de leitura */
		pd[18] = 1000 >> 8; pd[19] = 1000 & 0xff;	/* a cada 1000 ms */
		pd[22] = kbs >> 8; pd[23] = kbs;
		pd[26] = 1000 >> 8; pd[27] = 1000 & 0xff;
	}
	memset(c, 0, sizeof(c));
	c[0] = 0xB6;
	c[10] = sizeof(pd);
	r = cdusb_scsi(dev, c, 12, 2, pd, sizeof(pd), TMO_CMD, &R);
	if (r == CDUSB_E_EE_TIMEOUT)
		res->stuck = 1;
	lcmd(restore ? "SET STREAMING (padrao)" : "SET STREAMING", r);
	if (r == CDUSB_OK && s)
		s->set_ok |= 2;
	log_save();				/* se o leitor travar aqui, o log ja esta gravado */
}

static void test_motor(void)
{
	int lba = pick_near() + 2000, r, ms;
	u64 t0;

	res->spinup_ms = -1;
	if (lba >= trk[100].lba)
		return;
	S("   TESTE DO MOTOR: 15 S SEM LER NADA...  (O = PULAR)");
	t0 = ee_now_us();
	while (ee_now_us() - t0 < (u64)MOTOR_WAIT_MS * 1000) {
		if (pad_new() & PAD_CIRCLE) {
			L("[MOTOR] pulado pelo usuario");
			return;
		}
		ee_wait_ms(16);
	}
	t0 = ee_now_us();
	r = read_one(lba);
	ms = (int)((ee_now_us() - t0) / 1000);
	L("[MOTOR] depois de 15 s parado: setor %d %s em %d ms", lba, rtxt(r), ms);
	if (r == CDUSB_OK)
		res->spinup_ms = ms;
}

/* ------------------------------------------------------------------ roteiro */
#define STOP_IF_DEAD() do { log_save(); if (res->stuck || res->gone) return; } while (0)

static const struct {
	const char *name;
	int kbs;
} speeds[DIAG_SPEEDS] = {
	{"MAXIMA", 0}, {"8X", 1411}, {"4X", 706}, {"2X", 353},
};

void diag_run(int d, const cdusb_dev_info *info, diag_result *r)
{
	int fd, fa, i;

	dev = d;
	sub = info->sub;
	res = r;
	memset(r, 0, sizeof(*r));
	r->spinup_ms = -1;
	r->vid = info->vid;
	r->pid = info->pid;
	r->port = info->port;
	r->sub = info->sub;
	memcpy(r->vendor, info->inquiry + 8, 8);
	memcpy(r->product, info->inquiry + 16, 16);
	memcpy(r->rev, info->inquiry + 32, 4);

	screen_title("TESTANDO O LEITOR");
	S("   LEITOR: %s %s %s", r->vendor, r->product, r->rev);
	L("[CD] ---------------- novo teste: disp %d, %04X:%04X ----------------", d, r->vid, r->pid);

	S("   ESPERANDO O DISCO...");
	if (!wait_ready()) {
		STOP_IF_DEAD();
		return;
	}
	get_profile();
	get_caps();
	STOP_IF_DEAD();

	S("   LENDO O SUMARIO DO DISCO (TOC)...");
	if (!read_toc()) {
		S("   NAO CONSEGUI LER O SUMARIO DO DISCO");
		STOP_IF_DEAD();
		return;
	}
	r->disc = 1;
	STOP_IF_DEAD();

	S("   IDENTIFICANDO O DISCO...");
	identify();
	S("   DISCO: %s", r->system);
	STOP_IF_DEAD();

	S("   LENDO DADOS E MUSICA...");
	test_reads();
	STOP_IF_DEAD();

	fd = first_of(1);
	fa = longest_audio();
	for (i = 0; i < DIAG_SPEEDS; i++) {
		speed_result *s = &r->sp[i];

		memset(s, 0, sizeof(*s));
		s->name = speeds[i].name;
		s->kbs = speeds[i].kbs;
		s->data_bps = s->audio_bps = s->data_max_ms = s->audio_max_ms = -1;
		s->pace_min_ms = s->pace_cuts = s->pace_max_ms = s->seek_avg = s->seek_max = -1;
		r->nsp = i + 1;

		S("   VELOCIDADE %s: MEDINDO (~30 S)...", s->name);
		L("[VELOC] ================ %s ================", s->name);
		if (s->kbs)
			set_speed(s->kbs, 0, s);
		STOP_IF_DEAD();
		get_caps();
		s->cur_kbs = r->cur_kbs;
		STOP_IF_DEAD();

		if (fd >= 0 && r->data_ok) {
			s->data_bps = bench(0, fd, 16, BENCH_MS, &s->data_max_ms, &s->slow);
			STOP_IF_DEAD();
		}
		if (fa >= 0 && r->audio_ok) {
			s->audio_bps = bench(1, fa, 16, BENCH_MS, &s->audio_max_ms, &s->slow);
			STOP_IF_DEAD();
			pace(fa, PACE_SECS, s);
			STOP_IF_DEAD();
		}
		test_seek(s, i * 1000);
		STOP_IF_DEAD();
	}

	S("   VOLTANDO O LEITOR PARA A VELOCIDADE PADRAO...");
	set_speed(0xFFFF, 1, NULL);
	get_caps();
	STOP_IF_DEAD();

	test_motor();
	STOP_IF_DEAD();
	L("[CD] fim do teste");
}

/* ------------------------------------------------------------------ resultado */
/* bytes por segundo -> "x" em decimos (dados 1x = 153600 B/s, musica 1x = 176400 B/s) */
static int times10(int bps, int audio)
{
	return (int)((u64)bps * 10 / (audio ? 176400 : 153600));
}

void diag_summary(const diag_result *r, const char *log_file)
{
	int i, x;
	char d[16], a[16], p[24], k[16];

	screen_title("RESULTADO");
	S("   LEITOR:   %s %s %s", r->vendor, r->product, r->rev);
	S("             USB %04X:%04X, PORTA %d, SUBCLASSE %02X", r->vid, r->pid, r->port, r->sub);
	if (r->aborted)
		S("   TESTE INTERROMPIDO ANTES DO DISCO");
	if (r->gone) {
		S("   O LEITOR DESCONECTOU NO MEIO DO TESTE: FALTA DE ENERGIA?");
		S("   LIGUE O SEGUNDO PLUGUE DO CABO Y NUM CARREGADOR.");
	}
	if (r->stuck)
		S("   O LEITOR TRAVOU (SEM RESPOSTA). DESLIGUE E LIGUE O LEITOR.");
	if (!r->disc) {
		if (!r->aborted && !r->gone && !r->stuck)
			S("   NAO CONSEGUI LER O DISCO");
	} else {
		S("   DISCO:    %s", r->system);
		S("             %d FAIXAS: %d DE DADOS, %d DE MUSICA", r->last - r->first + 1, r->data_tracks,
		  r->audio_tracks);
		if (r->audio_tracks && !r->audio_ok)
			S("   MUSICA:   O LEITOR NAO ENTREGA A MUSICA PELO USB");
		else if (r->audio_tracks && r->audio_silent)
			S("   MUSICA:   VEIO SO SILENCIO (CONFERIR NO LOG)");
		S("   SUBCANAL: Q %s, P-W %s", r->subq_ok ? "OK" : "NAO", r->subraw_ok ? "OK" : "NAO");
		S(" ");
		S("   VELOC.  LEITOR   DADOS   MUSICA  TEMPO REAL 1X     BUSCA");
		for (i = 0; i < r->nsp; i++) {
			const speed_result *s = &r->sp[i];
			if (s->cur_kbs > 0)
				snprintf(k, sizeof(k), "%dX", (s->cur_kbs + 88) / 176);
			else
				snprintf(k, sizeof(k), "?");
			if (s->data_bps > 0) {
				x = times10(s->data_bps, 0);
				snprintf(d, sizeof(d), "%d,%dx", x / 10, x % 10);
			} else
				snprintf(d, sizeof(d), "-");
			if (s->audio_bps > 0) {
				x = times10(s->audio_bps, 1);
				snprintf(a, sizeof(a), "%d,%dx", x / 10, x % 10);
			} else
				snprintf(a, sizeof(a), "-");
			if (s->pace_cuts >= 0)
				snprintf(p, sizeof(p), "%d CORTES/%dMS", s->pace_cuts, s->pace_min_ms);
			else
				snprintf(p, sizeof(p), "-");
			S("   %-6s  %-7s  %-6s  %-6s  %-16s  %d/%d MS", s->name, k, d, a, p, s->seek_avg, s->seek_max);
		}
		S("   (TEMPO REAL: CORTES DE MUSICA / MENOR FOLGA DO BUFFER DE 1000 MS)");
		if (r->spinup_ms >= 0)
			S("   MOTOR:    DEPOIS DE 15 S PARADO, %d MS PARA VOLTAR A LER", r->spinup_ms);
	}
	S(" ");
	if (log_file)
		S("   LOG: %s", log_file);
	else
		S("   SEM PENDRIVE: LOG NAO GRAVADO");
}

/* ================================================================== PS1 (EMBER)
 * Teste para o plano do Ember: o disco do PS1 vira um .bin de 2352 bytes por
 * setor lido da leitora. Mede o que decide se da: leitura crua dos dados,
 * velocidade crua, tempo real a 2x com cache pequeno (o do IOP), troca
 * musica<->dados, busca, pausa antes da musica, subcanal Q (LibCrypt).
 */
#define PS1_RATE	150		/* PS1 a 2x: 150 setores por segundo */
#define PS1_AHEAD	96		/* cache do IOP: 96 setores crus = 220 KB */
#define PS1_PACE_SECS	20
#define PS1_SWITCHES	4

static ps1_result *pr;

/* consumo em tempo real de 'rate' setores/s, lendo 16 por vez para ficar
 * 'ahead' setores a frente. Devolve cortes (-1 = falhou). */
static int pace_gen(int kind, int start, int end, int rate, int ahead, int secs, int *min_ms, int *max_ms)
{
	int lba = start, produced = 0, before, consumed, a, min_a = 1 << 30, cuts = 0, in_cut = 0, r, c;
	u64 t0, now, c0;

	*min_ms = *max_ms = -1;
	if (end - start < rate * secs + ahead + 32)
		return -1;
	while (produced < ahead) {
		if (bench_read(kind, lba, 16) != CDUSB_OK)
			return -1;
		lba += 16;
		produced += 16;
	}
	*max_ms = 0;
	t0 = ee_now_us();
	for (;;) {
		now = ee_now_us();
		if (now - t0 >= (u64)secs * 1000000)
			break;
		consumed = (int)((now - t0) * rate / 1000000);
		if (produced - consumed >= ahead) {
			ee_wait_ms(2);
			continue;
		}
		before = produced;
		c0 = ee_now_us();
		r = bench_read(kind, lba, 16);
		now = ee_now_us();
		c = (int)((now - c0) / 1000);
		if (r != CDUSB_OK) {
			lcmd("TEMPO REAL PS1", r);
			return -1;
		}
		lba += 16;
		produced += 16;
		a = before - (int)((now - t0) * rate / 1000000);
		if (a < min_a)
			min_a = a;
		if (a < 0) {
			if (!in_cut)
				cuts++;
			in_cut = 1;
		} else
			in_cut = 0;
		if (c > *max_ms)
			*max_ms = c;
		if (c > 300)
			L("[PS1] LENTO setor %d em %d ms (folga %d ms)", lba - 16, c, a * 1000 / rate);
	}
	*min_ms = min_a * 1000 / rate;
	return cuts;
}

static int timed(int kind, int lba, int n)
{
	u64 t0 = ee_now_us();
	int r = bench_read(kind, lba, n);
	int ms = (int)((ee_now_us() - t0) / 1000);
	if (r != CDUSB_OK) {
		lcmd(kind == 1 ? "troca: musica" : "troca: dados", r);
		return -1;
	}
	return ms;
}

static void ps1_switch(int fd, int fa)
{
	int d = trk[fd].lba + 3000, a = trk[fa].lba + 750, i, ms;

	pr->da_avg = pr->da_max = pr->ad_avg = pr->ad_max = -1;
	if (d + PS1_SWITCHES * 600 >= safe_end(fd) || a + PS1_SWITCHES * 600 >= track_end(fa)) {
		L("[PS1] troca: faixas curtas demais");
		return;
	}
	if (timed(2, d, 16) < 0)
		return;
	pr->da_avg = pr->da_max = pr->ad_avg = pr->ad_max = 0;
	for (i = 0; i < PS1_SWITCHES; i++) {
		ms = timed(1, a + i * 600, 16);			/* dados -> musica */
		L("[PS1] troca %d: dados -> musica (setor %d) %d ms", i + 1, a + i * 600, ms);
		if (ms < 0)
			return;
		pr->da_avg += ms;
		if (ms > pr->da_max)
			pr->da_max = ms;
		ms = timed(2, d + i * 600, 16);			/* musica -> dados */
		L("[PS1] troca %d: musica -> dados (setor %d) %d ms", i + 1, d + i * 600, ms);
		if (ms < 0)
			return;
		pr->ad_avg += ms;
		if (ms > pr->ad_max)
			pr->ad_max = ms;
		log_save();
	}
	pr->da_avg /= PS1_SWITCHES;
	pr->ad_avg /= PS1_SWITCHES;
	L("[PS1] troca dados->musica: media %d ms, pior %d ms; musica->dados: media %d ms, pior %d ms",
	  pr->da_avg, pr->da_max, pr->ad_avg, pr->ad_max);
}

/* pausa de 150 setores antes da 1a musica depois dos dados: o que a leitora entrega? */
static void ps1_pregap(int fd)
{
	int t = fd + 1, p, i, r;
	static const int off[3] = {150, 75, 1};

	pr->pregap_raw = pr->pregap_audio = -1;
	if (t > t_last || is_data_t(t))
		return;
	pr->pregap_raw = pr->pregap_audio = 0;
	for (i = 0; i < 3; i++) {
		p = trk[t].lba - off[i];
		r = readcd(p, 1, 0, 0xF8, 0);
		L("[PS1] pausa: setor %d cru: %s (sense %06X, modo %d)", p, rtxt(r), R.sense, r == CDUSB_OK ? buf[15] : -1);
		if (r == CDUSB_OK)
			pr->pregap_raw++;
		r = readcd(p, 1, 1, 0x10, 0);
		L("[PS1] pausa: setor %d como musica: %s (sense %06X)", p, rtxt(r), R.sense);
		if (r == CDUSB_OK)
			pr->pregap_audio++;
	}
}

void ps1_run(int d, const cdusb_dev_info *info, ps1_result *p)
{
	static diag_result dr;
	speed_result s;
	int fd, fa, lba, r;

	dev = d;
	sub = info->sub;
	res = &dr;
	pr = p;
	memset(&dr, 0, sizeof(dr));
	memset(p, 0, sizeof(*p));
	data_raw = 0;
	p->raw_bps8 = p->raw_bps16 = p->cuts = p->pace_min_ms = p->pace_max_ms = -1;
	p->seek_avg = p->seek_max = p->da_avg = p->ad_avg = -1;
	p->pregap_raw = p->pregap_audio = p->spinup_ms = -1;
	memcpy(p->vendor, info->inquiry + 8, 8);
	memcpy(p->product, info->inquiry + 16, 16);

	screen_title("TESTE PS1 (EMBER)");
	S("   LEITOR: %s %s", p->vendor, p->product);
	L("[PS1] ---------------- teste PS1: disp %d, %04X:%04X ----------------", d, info->vid, info->pid);
	S("   ESPERANDO O DISCO...");
	if (!wait_ready())
		goto out;
	get_caps();
	if (!read_toc()) {
		S("   NAO CONSEGUI LER O SUMARIO DO DISCO");
		goto out;
	}
	p->disc = 1;
	p->data_tracks = dr.data_tracks;
	p->audio_tracks = dr.audio_tracks;
	identify();			/* pode falhar em modo 2 (READ(10)); o setor cru confere abaixo */
	strcpy(p->system, dr.system);
	S("   DISCO: %s, %d FAIXAS DE DADOS, %d DE MUSICA", p->system, p->data_tracks, p->audio_tracks);
	log_save();
	fd = first_of(1);
	if (fd < 0) {
		S("   SEM FAIXA DE DADOS: NAO E JOGO");
		goto out;
	}

	/* setor cru (o que vai no .bin) */
	r = readcd(trk[fd].lba + 16, 1, 0, 0xF8, 0);
	lcmd("READ CD cru (setor 16)", r);
	if (r == CDUSB_OK) {
		p->raw_ok = buf[0] == 0 && buf[1] == 0xFF && buf[10] == 0xFF && buf[11] == 0;
		p->raw_mode = buf[15];
		hex16("setor 16 cru", buf);
		L("[PS1] setor cru: sincronismo %s, modo %d, subcabecalho %02X %02X %02X %02X", p->raw_ok ? "OK" : "ERRADO",
		  buf[15], buf[16], buf[17], buf[18], buf[19]);
		/* ISO9660 dentro do setor cru (modo 2: dados em +24, modo 1: em +16) */
		{
			const u8 *u = buf + (buf[15] == 2 ? 24 : 16);
			if (memcmp(u + 1, "CD001", 5) == 0) {
				L("[PS1] ISO9660 (cru): sistema \"%.32s\" volume \"%.32s\"", u + 8, u + 40);
				if (find_str(u + 8, 32, "PLAYSTATION") >= 0)
					strcpy(p->system, "PLAYSTATION");
			}
		}
	}
	/* subcanal Q nos dados (LibCrypt depende dele) */
	lba = trk[fd].lba + 1000;
	r = readcd(lba, 1, 0, 0xF8, 2);
	lcmd("READ CD cru + Q (dados)", r);
	if (r == CDUSB_OK) {
		const u8 *q = buf + 2352;
		hex16("Q dos dados", q);
		p->subq_ok = (q[0] & 0x0f) == 1;
	}
	log_save();
	if (dr.stuck || dr.gone)
		goto out;

	S("   TRAVANDO A VELOCIDADE (PEDE 4X)...");
	memset(&s, 0, sizeof(s));
	set_speed(706, 0, &s);
	get_caps();
	p->cur_kbs = dr.cur_kbs;
	if (dr.stuck || dr.gone)
		goto out;

	if (p->raw_ok) {
		S("   VELOCIDADE DOS DADOS CRUS...");
		p->raw_bps16 = bench(2, fd, 16, BENCH_MS, &p->raw_max16, NULL);
		p->raw_bps8 = bench(2, fd, 8, BENCH_MS, &p->raw_max8, NULL);
		log_save();
		if (dr.stuck || dr.gone)
			goto out;
		S("   TEMPO REAL A 2X COM CACHE DE 220 KB (%d S)...", PS1_PACE_SECS);
		p->cuts = pace_gen(2, trk[fd].lba + 500, safe_end(fd), PS1_RATE, PS1_AHEAD, PS1_PACE_SECS,
				   &p->pace_min_ms, &p->pace_max_ms);
		L("[PS1] tempo real 2x: cortes %d, menor folga %d ms (de %d), pior comando %d ms", p->cuts,
		  p->pace_min_ms, PS1_AHEAD * 1000 / PS1_RATE, p->pace_max_ms);
		log_save();
	}
	fa = longest_audio();
	if (fa >= 0) {
		S("   TROCA MUSICA <-> DADOS (%d VEZES)...", PS1_SWITCHES);
		ps1_switch(fd, fa);
		ps1_pregap(fd);
		log_save();
		if (dr.stuck || dr.gone)
			goto out;
	}
	S("   BUSCA...");
	data_raw = p->raw_ok;
	test_seek(&s, 0);
	p->seek_avg = s.seek_avg;
	p->seek_max = s.seek_max;
	log_save();

	S("   VOLTANDO O LEITOR PARA A VELOCIDADE PADRAO...");
	set_speed(0xFFFF, 1, NULL);
	test_motor();
	p->spinup_ms = dr.spinup_ms;
out:
	data_raw = 0;
	p->stuck = dr.stuck;
	p->gone = dr.gone;
	p->aborted = dr.aborted;
	L("[PS1] fim do teste");
	log_save();
}

static const char *kbs_x(int bps, char *o)
{
	int x;
	if (bps <= 0) {
		strcpy(o, "-");
		return o;
	}
	x = (int)((u64)bps * 10 / 176400);		/* x de CD (2352 por setor) */
	sprintf(o, "%d KB/S (%d,%dX)", bps / 1024, x / 10, x % 10);
	return o;
}

void ps1_summary(const ps1_result *p, const char *log_file)
{
	char a[32], b[32];

	screen_title("RESULTADO PS1 (EMBER)");
	S("   LEITOR:   %s %s", p->vendor, p->product);
	if (p->gone)
		S("   O LEITOR DESCONECTOU NO MEIO DO TESTE: FALTA DE ENERGIA?");
	if (p->stuck)
		S("   O LEITOR TRAVOU (SEM RESPOSTA). DESLIGUE E LIGUE O LEITOR.");
	if (p->aborted)
		S("   TESTE INTERROMPIDO");
	if (p->disc) {
		S("   DISCO:    %s (%d DADOS, %d MUSICA)", p->system, p->data_tracks, p->audio_tracks);
		S("   SETOR CRU: %s, MODO %d   SUBCANAL Q: %s", p->raw_ok ? "OK" : "NAO", p->raw_mode,
		  p->subq_ok ? "OK" : "NAO");
		if (p->cur_kbs > 0)
			S("   LEITOR TRAVADO EM %dX", (p->cur_kbs + 88) / 176);
		S("   CRU 16 SETORES: %s, PIOR %d MS", kbs_x(p->raw_bps16, a), p->raw_max16);
		S("   CRU 8 SETORES:  %s, PIOR %d MS", kbs_x(p->raw_bps8, b), p->raw_max8);
		S("   (O PS1 PRECISA DE 2X = 344 KB/S)");
		if (p->cuts >= 0)
			S("   TEMPO REAL 2X: %d CORTES, MENOR FOLGA %d MS, PIOR %d MS", p->cuts, p->pace_min_ms,
			  p->pace_max_ms);
		if (p->da_avg >= 0)
			S("   TROCA DADOS->MUSICA %d MS (PIOR %d), MUSICA->DADOS %d MS (PIOR %d)", p->da_avg, p->da_max,
			  p->ad_avg, p->ad_max);
		if (p->pregap_raw >= 0)
			S("   PAUSA ANTES DA MUSICA: CRU %d/3, COMO MUSICA %d/3", p->pregap_raw, p->pregap_audio);
		if (p->seek_avg >= 0)
			S("   BUSCA: MEDIA %d MS, PIOR %d MS", p->seek_avg, p->seek_max);
		if (p->spinup_ms >= 0)
			S("   MOTOR: DEPOIS DE 15 S PARADO, %d MS PARA VOLTAR", p->spinup_ms);
	}
	S(" ");
	if (log_file)
		S("   LOG: %s", log_file);
	else
		S("   SEM PENDRIVE: LOG NAO GRAVADO");
}
