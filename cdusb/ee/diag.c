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

static int q_mode;		/* c10: 1 = READ(10) com fila completa no USB (dir 3) */

static int read10(int lba, int n)
{
	u8 c[12] = {0x28, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	put_lba(c, lba);
	c[7] = n >> 8; c[8] = n;
	return cmd(c, q_mode ? 3 : 1, n * 2048, TMO_READ);
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
 * setor lido da leitora (c07). A c08 faz a fase toda de uma vez:
 *  1. setor cru, subcanal Q, velocidade crua (8 e 16 setores);
 *  2. travadas: le em sequencia, volta para tras e le de novo, em 4 pontos
 *     do disco, com 8 e 16 setores (a c07 travou 1,5-2 s depois de voltar);
 *  3. tempo real a 2x com cache de 220 KB: 60 s em sequencia e 60 s com
 *     pulos a cada 5 s (como o jogo carregando coisas);
 *  4. troca musica <-> dados e pausa antes da musica (se o disco tiver musica);
 *  5. busca em varias distancias;
 *  6. motor: parado 3, 6 e 15 s;
 * Se a leitora reiniciar no meio, espera ela voltar, trava a velocidade de
 * novo e repete a leitura (conta como recuperacao).
 */
#define PS1_RATE	150		/* PS1 a 2x: 150 setores por segundo */
#define PS1_AHEAD	96		/* cache do IOP: 96 setores crus = 220 KB */
#define PS1_SWITCHES	4
#define STALL_MS	300

static ps1_result *pr;
static unsigned rnd_state = 12345;
static int lock_kbs = 706;		/* velocidade travada de novo depois de recuperar */
static int recoveries;

static unsigned rnd(void)
{
	rnd_state = rnd_state * 1103515245u + 12345u;
	return rnd_state >> 8;
}

/* a leitora reiniciou ou parou de responder: espera voltar e trava de novo */
static int recover(const char *why)
{
	u64 t0 = ee_now_us();

	recoveries++;
	L("[REC] RECUPERANDO (%s)...", why);
	log_save();
	ee_wait_ms(500);
	if (res->stuck)
		return 0;
	res->gone = 0;
	while (ee_now_us() - t0 < 20000000ull) {
		cdusb_status st;
		if (cdusb_get_status(&st) == 0 && st.dev[dev].state == CDUSB_DEV_READY)
			break;
		ee_wait_ms(300);
	}
	if (!wait_ready())
		return 0;
	if (lock_kbs)
		set_speed(lock_kbs, 0, NULL);
	L("[REC] leitora de volta em %d ms", (int)((ee_now_us() - t0) / 1000));
	return 1;
}

/* leitura do teste: uma nova tentativa depois de recuperar */
static int pread(int kind, int lba, int n)
{
	int r = bench_read(kind, lba, n), key = (R.sense >> 16) & 0x0f;

	if (r == CDUSB_OK)
		return r;
	if (r == CDUSB_E_TIMEOUT || r == CDUSB_E_USB || r == CDUSB_PHASE || r == CDUSB_E_NODEV ||
	    (r == CDUSB_CHECK && (key == 6 || key == 2))) {
		lcmd("leitura PS1", r);
		if (!recover(rtxt(r)))
			return r;
		r = bench_read(kind, lba, n);
	}
	if (r != CDUSB_OK)
		lcmd("leitura PS1", r);
	return r;
}

static int timed(int kind, int lba, int n)
{
	u64 t0 = ee_now_us();
	int r = pread(kind, lba, n);
	int ms = (int)((ee_now_us() - t0) / 1000);
	return r == CDUSB_OK ? ms : -1;
}

static int dead(void)
{
	log_save();
	return res->stuck || res->gone;
}

/* ------------------------------------------------------------------ 2. travadas */
/* le 'count' setores de 'per' em 'per'; devolve o pior comando (-1 = falhou) */
static int seq_read(int lba, int count, int per, const char *what, int *stalls)
{
	int i, ms, worst = 0;

	for (i = 0; i < count; i += per) {
		ms = timed(2, lba + i, per);
		if (ms < 0)
			return -1;
		if (ms > STALL_MS) {
			(*stalls)++;
			L("[TRAVADA] %s: setor %d (comando %d) em %d ms", what, lba + i, i / per, ms);
		}
		if (ms > worst)
			worst = ms;
	}
	return worst;
}

static void ps1_stall(int fd)
{
	int lo = trk[fd].lba, hi = safe_end(fd), span = hi - lo, p, k, j, w;
	static const int pers[2] = {16, 8};
	char what[64];

	for (k = 0; k < 2; k++)
		pr->st_n[k] = pr->st_max[k] = 0;
	for (j = 0; j < 4; j++) {
		p = j == 0 ? lo : j == 1 ? lo + span / 4 : j == 2 ? lo + span / 2 : lo + span * 9 / 10;
		if (p + 800 > hi)
			continue;
		for (k = 0; k < 2; k++) {
			int per = pers[k];
			snprintf(what, sizeof(what), "%d setores, ponto %d, em frente", per, j + 1);
			w = seq_read(p, 400, per, what, &pr->st_n[k]);
			if (w < 0)
				return;
			if (w > pr->st_max[k]) pr->st_max[k] = w;
			snprintf(what, sizeof(what), "%d setores, ponto %d, volta 400", per, j + 1);
			w = seq_read(p, 400, per, what, &pr->st_n[k]);		/* volta para tras */
			if (w < 0)
				return;
			if (w > pr->st_max[k]) pr->st_max[k] = w;
			snprintf(what, sizeof(what), "%d setores, ponto %d, volta 200", per, j + 1);
			w = seq_read(p + 200, 200, per, what, &pr->st_n[k]);	/* volta curta */
			if (w < 0)
				return;
			if (w > pr->st_max[k]) pr->st_max[k] = w;
			L("[TRAVADA] %d setores, ponto %d (setor %d): ate aqui %d travadas, pior %d ms", per, j + 1, p,
			  pr->st_n[k], pr->st_max[k]);
			if (dead())
				return;
		}
	}
	pr->st_done = 1;
}

/* ------------------------------------------------------------------ 3. tempo real */
/* consome 'rate' setores/s com cache de 'ahead'; jump_s > 0: pula para um ponto
 * qualquer a cada jump_s segundos (o cache comeca vazio, o jogo espera o 1o bloco) */
static void pace2(int lo, int hi, int secs, int jump_s, ps1_pace *o)
{
	int lba = lo, produced = 0, before, a, min_a = 1 << 30, in_cut = 0, c, jw;
	u64 now = ee_now_us(), tend, tnext, base, c0;

	memset(o, 0, sizeof(*o));
	o->min_ms = o->max_ms = -1;
	o->cuts = -1;
	if (hi - lo < PS1_RATE * (jump_s ? jump_s : secs) + PS1_AHEAD + 64)
		return;
	while (produced < PS1_AHEAD) {
		if (pread(2, lba, 16) != CDUSB_OK)
			return;
		lba += 16;
		produced += 16;
	}
	o->cuts = 0;
	o->max_ms = 0;
	now = base = ee_now_us();
	tend = now + (u64)secs * 1000000;
	tnext = jump_s ? now + (u64)jump_s * 1000000 : ~0ull;
	while ((now = ee_now_us()) < tend) {
		if (now >= tnext) {
			int np = lo + (int)(rnd() % (unsigned)(hi - lo - PS1_RATE * jump_s - PS1_AHEAD - 64));
			jw = timed(2, np, 16);
			if (jw < 0) {
				o->cuts = -1;
				return;
			}
			o->jumps++;
			o->jwait_sum += jw;
			if (jw > o->jwait_max)
				o->jwait_max = jw;
			if (jw > STALL_MS)
				L("[TEMPO REAL] pulo para o setor %d: 1o bloco em %d ms", np, jw);
			lba = np + 16;
			produced = 16;
			in_cut = 0;
			base = ee_now_us();
			tnext = base + (u64)jump_s * 1000000;
			continue;
		}
		if (produced - (int)((now - base) * PS1_RATE / 1000000) >= PS1_AHEAD) {
			ee_wait_ms(2);
			continue;
		}
		if (lba + 16 > hi)
			lba = lo;
		before = produced;
		c0 = ee_now_us();
		if (pread(2, lba, 16) != CDUSB_OK) {
			o->cuts = -1;
			return;
		}
		now = ee_now_us();
		c = (int)((now - c0) / 1000);
		lba += 16;
		produced += 16;
		a = before - (int)((now - base) * PS1_RATE / 1000000);
		if (a < min_a)
			min_a = a;
		if (a < 0) {
			if (!in_cut) {
				o->cuts++;
				L("[TEMPO REAL] CORTE no setor %d (comando de %d ms)", lba - 16, c);
			}
			in_cut = 1;
		} else
			in_cut = 0;
		if (c > o->max_ms)
			o->max_ms = c;
		if (c > STALL_MS) {
			o->stalls++;
			L("[TEMPO REAL] LENTO setor %d em %d ms (folga %d ms)", lba - 16, c, a * 1000 / PS1_RATE);
		}
	}
	o->min_ms = min_a == (1 << 30) ? -1 : min_a * 1000 / PS1_RATE;
	L("[TEMPO REAL] %d s%s: cortes %d, travadas %d, menor folga %d ms, pior comando %d ms; pulos %d, 1o bloco "
	  "media %d ms, pior %d ms",
	  secs, jump_s ? " com pulos" : " em sequencia", o->cuts, o->stalls, o->min_ms, o->max_ms, o->jumps,
	  o->jumps ? o->jwait_sum / o->jumps : 0, o->jwait_max);
}

/* ------------------------------------------------------------------ 4. musica */
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
	/* musica em sequencia (o Ember toca a faixa lendo o .bin) */
	pr->audio_bps = bench(1, fa, 16, BENCH_MS, &pr->audio_max, NULL);
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

/* ------------------------------------------------------------------ 5. busca */
static void ps1_seek(int fd)
{
	int lo = trk[fd].lba, hi = safe_end(fd), base = lo + 1000, i, k, ms, sum;
	int dist[PS1_SEEKS];

	dist[0] = 2000;
	dist[1] = (hi - lo) / 10;
	dist[2] = (hi - lo) / 2;
	dist[3] = (hi - lo) * 9 / 10;
	dist[4] = hi - 600 - base;
	for (i = 0; i < PS1_SEEKS; i++) {
		pr->seek_ms[i] = -1;
		pr->seek_dist[i] = dist[i];
		if (base + dist[i] >= hi || dist[i] <= 0)
			continue;
		for (k = 0, sum = 0; k < 3; k++) {
			if (timed(2, base + k * 32, 1) < 0)	/* posiciona */
				return;
			ms = timed(2, base + dist[i] + k * 32, 1);
			if (ms < 0)
				return;
			sum += ms;
		}
		pr->seek_ms[i] = sum / 3;
		L("[BUSCA] pulo de %d setores (%d MB): media %d ms", dist[i], (int)((u64)dist[i] * 2352 >> 20),
		  pr->seek_ms[i]);
	}
}

/* ------------------------------------------------------------------ 6. motor */
static void ps1_motor(int fd)
{
	static const int waits[PS1_MOTORS] = {3, 6, 15};
	int lba = trk[fd].lba + 5000, i;
	u64 t0;

	for (i = 0; i < PS1_MOTORS; i++)
		pr->motor_ms[i] = -1;
	if (lba + 4000 >= safe_end(fd))
		return;
	for (i = 0; i < PS1_MOTORS; i++) {
		if (timed(2, lba, 1) < 0)
			return;
		S("   MOTOR: %d S SEM LER NADA...  (O = PULAR)", waits[i]);
		t0 = ee_now_us();
		while (ee_now_us() - t0 < (u64)waits[i] * 1000000) {
			if (pad_new() & PAD_CIRCLE) {
				L("[MOTOR] pulado pelo usuario");
				return;
			}
			ee_wait_ms(16);
		}
		pr->motor_ms[i] = timed(2, lba + 1000, 1);
		L("[MOTOR] depois de %d s parado: %d ms", waits[i], pr->motor_ms[i]);
		lba += 1000;
	}
}

/* ------------------------------------------------------------------ roteiro */
void ps1_run(int d, const cdusb_dev_info *info, ps1_result *p)
{
	static diag_result dr;
	speed_result s;
	int fd, fa, lba, r;
	u64 t_all = ee_now_us();

	dev = d;
	sub = info->sub;
	res = &dr;
	pr = p;
	memset(&dr, 0, sizeof(dr));
	memset(p, 0, sizeof(*p));
	data_raw = 0;
	lock_kbs = 706;
	recoveries = 0;
	p->raw_bps8 = p->raw_bps16 = p->da_avg = p->audio_bps = -1;
	p->pregap_raw = p->pregap_audio = -1;
	p->seq.cuts = p->jmp.cuts = -1;
	memcpy(p->vendor, info->inquiry + 8, 8);
	memcpy(p->product, info->inquiry + 16, 16);

	screen_title("TESTE PS1 (EMBER) - FASE COMPLETA, ~5 MIN");
	S("   LEITOR: %s %s", p->vendor, p->product);
	L("[PS1] ---------------- teste PS1 c08: disp %d, %04X:%04X ----------------", d, info->vid, info->pid);
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
	fd = first_of(1);
	if (fd < 0) {
		S("   SEM FAIXA DE DADOS: NAO E JOGO");
		goto out;
	}

	/* 1. setor cru (o que vai no .bin) e subcanal Q */
	S("   1/6 SETOR CRU, SUBCANAL E VELOCIDADE...");
	r = readcd(trk[fd].lba + 16, 1, 0, 0xF8, 0);
	lcmd("READ CD cru (setor 16)", r);
	if (r == CDUSB_OK) {
		const u8 *u;
		p->raw_ok = buf[0] == 0 && buf[1] == 0xFF && buf[10] == 0xFF && buf[11] == 0;
		p->raw_mode = buf[15];
		hex16("setor 16 cru", buf);
		L("[PS1] setor cru: sincronismo %s, modo %d, subcabecalho %02X %02X %02X %02X", p->raw_ok ? "OK" : "ERRADO",
		  buf[15], buf[16], buf[17], buf[18], buf[19]);
		u = buf + (buf[15] == 2 ? 24 : 16);
		if (memcmp(u + 1, "CD001", 5) == 0) {
			L("[PS1] ISO9660 (cru): sistema \"%.32s\" volume \"%.32s\"", u + 8, u + 40);
			if (find_str(u + 8, 32, "PLAYSTATION") >= 0)
				strcpy(p->system, "PLAYSTATION");
		}
	}
	S("   DISCO: %s, %d FAIXAS DE DADOS, %d DE MUSICA", p->system, p->data_tracks, p->audio_tracks);
	lba = trk[fd].lba + 1000;
	r = readcd(lba, 1, 0, 0xF8, 2);
	lcmd("READ CD cru + Q (dados)", r);
	if (r == CDUSB_OK) {
		const u8 *q = buf + 2352;
		hex16("Q dos dados", q);
		p->subq_ok = (q[0] & 0x0f) == 1;
	}
	if (dead() || !p->raw_ok)
		goto out;
	memset(&s, 0, sizeof(s));
	set_speed(706, 0, &s);
	get_caps();
	p->cur_kbs = dr.cur_kbs;
	data_raw = 1;
	p->raw_bps16 = bench(2, fd, 16, BENCH_MS, &p->raw_max16, NULL);
	p->raw_bps8 = bench(2, fd, 8, BENCH_MS, &p->raw_max8, NULL);
	if (dead())
		goto out;

	/* 2. travadas depois de voltar para tras */
	S("   2/6 TRAVADAS: FRENTE E VOLTA EM 4 PONTOS DO DISCO...");
	ps1_stall(fd);
	if (dead())
		goto out;

	/* 3. tempo real */
	S("   3/6 TEMPO REAL 2X, CACHE 220 KB: 60 S EM SEQUENCIA...");
	pace2(trk[fd].lba + 500, safe_end(fd), 60, 0, &p->seq);
	if (dead())
		goto out;
	S("   3/6 TEMPO REAL 2X: 60 S COM PULOS A CADA 5 S...");
	pace2(trk[fd].lba + 500, safe_end(fd), 60, 5, &p->jmp);
	if (dead())
		goto out;

	/* 4. musica */
	fa = longest_audio();
	if (fa >= 0) {
		S("   4/6 TROCA MUSICA <-> DADOS...");
		ps1_switch(fd, fa);
		ps1_pregap(fd);
		if (dead())
			goto out;
	} else
		S("   4/6 SEM FAIXAS DE MUSICA NESTE DISCO: PULADO");

	/* 5. busca */
	S("   5/6 BUSCA EM VARIAS DISTANCIAS...");
	ps1_seek(fd);
	if (dead())
		goto out;

	/* 6. motor (sem a trava: como fica no dia a dia, com a trava aplicada) */
	S("   6/6 MOTOR...");
	ps1_motor(fd);

	S("   VOLTANDO O LEITOR PARA A VELOCIDADE PADRAO...");
	set_speed(0xFFFF, 1, NULL);
out:
	data_raw = 0;
	p->recoveries = recoveries;
	p->stuck = dr.stuck;
	p->gone = dr.gone;
	p->aborted = dr.aborted;
	L("[PS1] fim do teste (%d s)", (int)((ee_now_us() - t_all) / 1000000));
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

static void pace_line(const char *what, const ps1_pace *o)
{
	if (o->cuts < 0) {
		S("   %s: -", what);
		return;
	}
	S("   %s: %d CORTES, %d TRAVADAS, FOLGA MIN %d MS, PIOR %d MS", what, o->cuts, o->stalls, o->min_ms,
	  o->max_ms);
	if (o->jumps)
		S("      %d PULOS: 1O BLOCO MEDIA %d MS, PIOR %d MS", o->jumps, o->jwait_sum / o->jumps, o->jwait_max);
}

void ps1_summary(const ps1_result *p, const char *log_file)
{
	char a[32], b[32];
	int i;

	screen_title("RESULTADO PS1 (EMBER)");
	S("   LEITOR: %s %s", p->vendor, p->product);
	if (p->gone)
		S("   O LEITOR DESCONECTOU NO MEIO DO TESTE: FALTA DE ENERGIA?");
	if (p->stuck)
		S("   O LEITOR TRAVOU (SEM RESPOSTA). DESLIGUE E LIGUE O LEITOR.");
	if (p->aborted)
		S("   TESTE INTERROMPIDO");
	if (p->recoveries)
		S("   LEITORA REINICIOU %d VEZ(ES) NO MEIO (RECUPERADA)", p->recoveries);
	if (p->disc) {
		S("   DISCO: %s (%d DADOS, %d MUSICA)", p->system, p->data_tracks, p->audio_tracks);
		S("   SETOR CRU: %s, MODO %d   SUBCANAL Q: %s   LEITOR EM %dX", p->raw_ok ? "OK" : "NAO", p->raw_mode,
		  p->subq_ok ? "OK" : "NAO", p->cur_kbs > 0 ? (p->cur_kbs + 88) / 176 : 0);
		S("   CRU 16: %s PIOR %d MS   CRU 8: %s PIOR %d MS", kbs_x(p->raw_bps16, a), p->raw_max16,
		  kbs_x(p->raw_bps8, b), p->raw_max8);
		if (p->st_done || p->st_n[0] || p->st_n[1])
			S("   TRAVADAS FRENTE/VOLTA: 16 SET. %d (PIOR %d MS), 8 SET. %d (PIOR %d MS)", p->st_n[0],
			  p->st_max[0], p->st_n[1], p->st_max[1]);
		pace_line("2X SEQUENCIA 60 S", &p->seq);
		pace_line("2X COM PULOS 60 S", &p->jmp);
		if (p->da_avg >= 0)
			S("   TROCA DADOS->MUSICA %d MS (PIOR %d), MUSICA->DADOS %d MS (PIOR %d)", p->da_avg, p->da_max,
			  p->ad_avg, p->ad_max);
		if (p->audio_bps > 0)
			S("   MUSICA EM SEQUENCIA: %s", kbs_x(p->audio_bps, a));
		if (p->pregap_raw >= 0)
			S("   PAUSA ANTES DA MUSICA: CRU %d/3, COMO MUSICA %d/3", p->pregap_raw, p->pregap_audio);
		for (i = 0; i < PS1_SEEKS; i++)
			if (p->seek_ms[i] >= 0)
				S("   BUSCA DE %d MB: %d MS", (int)((u64)p->seek_dist[i] * 2352 >> 20), p->seek_ms[i]);
		if (p->motor_ms[0] >= 0)
			S("   MOTOR PARADO 3/6/15 S: %d / %d / %d MS PARA VOLTAR", p->motor_ms[0], p->motor_ms[1],
			  p->motor_ms[2]);
	}
	S(" ");
	if (log_file)
		S("   LOG: %s", log_file);
	else
		S("   SEM PENDRIVE: LOG NAO GRAVADO");
}

/* ================================================================== PS2
 * c09: os mesmos testes, agora com DVD (ou CD azul) de PS2, para o driver de
 * bloco do Neutrino. Dados por READ(10) de 2048 bytes.
 *  1. disco: perfil, capacidade, camadas (DVD-9), SYSTEM.CNF (BOOT2);
 *  2. velocidade em sequencia com 8, 16 e 19 setores por comando (19 = o
 *     maximo do cdusb), na velocidade do leitor e travada (DVD 1x / CD 4x);
 *  3. leituras pequenas espalhadas (jogo carregando muitos arquivos);
 *  4. busca em varias distancias e troca de camada;
 *  5. travadas frente/volta, tempo real (300 e 600 KB/s, cache de 128 KB,
 *     com pulos) e motor, na velocidade travada;
 *  c10: a sequencia tambem com a fila completa no USB (comando + pedacos de
 *     4 KB + resposta de uma vez), para medir o ganho do NEUTRINOUSB n04.
 */
#define PS2_SEEKS	6
static ps2_result *qr;

static int read_capacity(int *last)
{
	u8 c[12] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	int r = cmd(c, 1, 8, TMO_CMD);
	if (r != CDUSB_OK)
		return 0;
	*last = buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3];
	return 1;
}

/* DVD: camadas e fim da camada 0 (READ DVD STRUCTURE, formato 0) */
static void dvd_layers(void)
{
	u8 c[12] = {0xAD, 0, 0, 0, 0, 0, 0, 0, 0, 36, 0, 0};
	int r = cmd(c, 1, 36, TMO_CMD), start, endl0;

	qr->layers = qr->l0_end = -1;
	lcmd("READ DVD STRUCTURE", r);
	if (r != CDUSB_OK)
		return;
	hex16("DVD fisico", buf + 4);
	qr->layers = ((buf[4 + 2] >> 5) & 3) + 1;
	start = buf[4 + 5] << 16 | buf[4 + 6] << 8 | buf[4 + 7];
	endl0 = buf[4 + 13] << 16 | buf[4 + 14] << 8 | buf[4 + 15];
	if (qr->layers > 1 && endl0 > start)
		qr->l0_end = endl0 - start;		/* ultimo setor da camada 0 (LBA) */
	L("[PS2] DVD: %d camada(s), %s, inicio PSN %X, fim da camada 0 PSN %X (LBA %d)", qr->layers,
	  (buf[4 + 2] & 0x10) ? "OTP" : "PTP", start, endl0, qr->l0_end);
}

/* SYSTEM.CNF pela ISO9660 (READ(10)) */
static void ps2_identify(int base)
{
	int root, rsize, k, off, lba = -1;
	char txt[2049], *p;

	strcpy(qr->system, "DESCONHECIDO");
	if (read10(base + 16, 1) != CDUSB_OK || memcmp(buf + 1, "CD001", 5) != 0) {
		L("[PS2] sem ISO9660 no setor 16");
		return;
	}
	L("[PS2] ISO9660: sistema \"%.32s\" volume \"%.32s\"", buf + 8, buf + 40);
	snprintf(qr->system, sizeof(qr->system), "%.11s", buf + 8);
	root = buf[158] | buf[159] << 8 | buf[160] << 16 | buf[161] << 24;
	rsize = buf[166] | buf[167] << 8 | buf[168] << 16 | buf[169] << 24;
	for (k = 0; k * 2048 < rsize && k < 8 && lba < 0; k++) {
		if (read10(base + root + k, 1) != CDUSB_OK)
			return;
		for (off = 0; off < 2048 && buf[off];) {
			if (buf[off + 32] >= 10 && memcmp(buf + off + 33, "SYSTEM.CNF", 10) == 0) {
				lba = buf[off + 2] | buf[off + 3] << 8 | buf[off + 4] << 16 | buf[off + 5] << 24;
				break;
			}
			off += buf[off];
		}
	}
	if (lba < 0 || read10(base + lba, 1) != CDUSB_OK) {
		L("[PS2] SYSTEM.CNF nao encontrado");
		return;
	}
	memcpy(txt, buf, 2048);
	txt[2048] = 0;
	L("[PS2] SYSTEM.CNF: %.160s", txt);
	p = strstr(txt, "BOOT2");
	qr->ps2 = p != NULL;
	if (p && (p = strstr(p, "cdrom0:")) != NULL) {
		int n = 0;
		p += 7;
		while (*p == '\\')
			p++;
		while (*p && *p != ';' && *p != '\r' && *p != '\n' && n < 15)
			qr->code[n++] = *p++;
		qr->code[n] = 0;
	}
}

/* em sequencia: 'per' setores por comando durante ms; KB/s (-1 = falhou) */
static int ps2_bench(int start, int per, int ms, int *worst)
{
	int lba = start, r, c, n = 0, end = trk[100].lba;
	u64 t0, c0, bytes = 0;

	*worst = 0;
	if (pread(0, lba, per) != CDUSB_OK)
		return -1;
	lba += per;
	t0 = ee_now_us();
	while (ee_now_us() - t0 < (u64)ms * 1000) {
		if (lba + per > end)
			lba = start;
		c0 = ee_now_us();
		r = pread(0, lba, per);
		c = (int)((ee_now_us() - c0) / 1000);
		if (r != CDUSB_OK)
			return -1;
		if (c > *worst)
			*worst = c;
		if (c > STALL_MS)
			L("[PS2] LENTO: setor %d x%d em %d ms", lba, per, c);
		bytes += per * 2048;
		lba += per;
		n++;
	}
	{
		u64 el = ee_now_us() - t0;
		c = (int)(bytes * 1000000 / (el ? el : 1) / 1024);
	}
	L("[PS2] em sequencia, %d setores por comando: %d KB/s (%d comandos, pior %d ms)", per, c, n, *worst);
	return c;
}

static void ps2_speed_block(int st_i, int start)
{
	static const int pers[3] = {8, 16, 19};
	int k, w;
	ps2_speed *v = &qr->sp[st_i];

	for (k = 0; k < 3; k++) {
		v->kbs[k] = ps2_bench(start, pers[k], BENCH_MS, &w);
		v->worst[k] = w;
		if (dead())
			return;
	}
	/* leituras pequenas espalhadas: 4 setores em pontos aleatorios */
	{
		int i, ms, sum = 0, mx = 0, cnt = 0, span = trk[100].lba - start - 64;
		for (i = 0; i < 20 && span > 0; i++) {
			int lba = start + (int)(rnd() % (unsigned)span);
			ms = timed(0, lba, 4);
			if (ms < 0)
				return;
			sum += ms;
			cnt++;
			if (ms > mx)
				mx = ms;
		}
		v->small_avg = cnt ? sum / cnt : -1;
		v->small_max = mx;
		L("[PS2] 20 leituras pequenas espalhadas: media %d ms, pior %d ms", v->small_avg, mx);
	}
	/* busca em varias distancias */
	{
		int span = trk[100].lba - start, base = start + 1000, i, k2, sm, ms;
		int dist[PS2_SEEKS];
		dist[0] = 2000;
		dist[1] = span / 10;
		dist[2] = span / 2;
		dist[3] = span * 9 / 10;
		dist[4] = span - 1000 - 600;
		dist[5] = qr->l0_end > 0 ? qr->l0_end + 200 - base : -1;	/* troca de camada */
		for (i = 0; i < PS2_SEEKS; i++) {
			v->seek[i] = -1;
			qr->seek_mb[i] = dist[i] > 0 ? (int)((u64)dist[i] * 2048 >> 20) : -1;
			if (dist[i] <= 0 || base + dist[i] >= trk[100].lba)
				continue;
			for (k2 = 0, sm = 0; k2 < 3; k2++) {
				if (timed(0, base + k2 * 32, 1) < 0)
					return;
				ms = timed(0, base + dist[i] + k2 * 32, 1);
				if (ms < 0)
					return;
				sm += ms;
			}
			v->seek[i] = sm / 3;
			L("[PS2] busca de %d MB%s: media %d ms", qr->seek_mb[i], i == 5 ? " (troca de camada)" : "",
			  v->seek[i]);
		}
	}
}

/* tempo real: consome rate_kbs com cache de cache_kb; pulo a cada jump_s (0 = sem) */
static void ps2_pace(int start, int rate_kbs, int cache_kb, int secs, int jump_s, ps1_pace *o)
{
	int per = 16, rate = rate_kbs / 2;		/* setores por segundo */
	int ahead = cache_kb / 2, lba = start, end = trk[100].lba, produced = 0, before, a, min_a = 1 << 30;
	int in_cut = 0, c, jw;
	u64 now, tend, tnext, base, c0;

	memset(o, 0, sizeof(*o));
	o->min_ms = o->max_ms = -1;
	o->cuts = -1;
	while (produced < ahead) {
		if (pread(0, lba, per) != CDUSB_OK)
			return;
		lba += per;
		produced += per;
	}
	o->cuts = 0;
	o->max_ms = 0;
	now = base = ee_now_us();
	tend = now + (u64)secs * 1000000;
	tnext = jump_s ? now + (u64)jump_s * 1000000 : ~0ull;
	while ((now = ee_now_us()) < tend) {
		if (now >= tnext) {
			int np = start + (int)(rnd() % (unsigned)(end - start - rate * jump_s - ahead - 64));
			jw = timed(0, np, per);
			if (jw < 0) {
				o->cuts = -1;
				return;
			}
			o->jumps++;
			o->jwait_sum += jw;
			if (jw > o->jwait_max)
				o->jwait_max = jw;
			lba = np + per;
			produced = per;
			in_cut = 0;
			base = ee_now_us();
			tnext = base + (u64)jump_s * 1000000;
			continue;
		}
		if (produced - (int)((now - base) * rate / 1000000) >= ahead) {
			ee_wait_ms(2);
			continue;
		}
		if (lba + per > end)
			lba = start;
		before = produced;
		c0 = ee_now_us();
		if (pread(0, lba, per) != CDUSB_OK) {
			o->cuts = -1;
			return;
		}
		now = ee_now_us();
		c = (int)((now - c0) / 1000);
		lba += per;
		produced += per;
		a = before - (int)((now - base) * rate / 1000000);
		if (a < min_a)
			min_a = a;
		if (a < 0) {
			if (!in_cut)
				o->cuts++;
			in_cut = 1;
		} else
			in_cut = 0;
		if (c > o->max_ms)
			o->max_ms = c;
		if (c > STALL_MS) {
			o->stalls++;
			L("[PS2] tempo real: LENTO setor %d em %d ms", lba - per, c);
		}
	}
	o->min_ms = min_a == (1 << 30) ? -1 : min_a * 1000 / rate;
	L("[PS2] tempo real %d KB/s, cache %d KB, %d s%s: cortes %d, travadas %d, folga min %d ms, pior %d ms; "
	  "pulos %d (1o bloco media %d ms, pior %d ms)",
	  rate_kbs, cache_kb, secs, jump_s ? " com pulos" : "", o->cuts, o->stalls, o->min_ms, o->max_ms, o->jumps,
	  o->jumps ? o->jwait_sum / o->jumps : 0, o->jwait_max);
}

static void ps2_stall(int start)
{
	int span = trk[100].lba - start, j, p, i, ms;

	qr->st_n = qr->st_max = 0;
	for (j = 0; j < 4; j++) {
		p = start + (j == 0 ? 0 : j == 1 ? span / 4 : j == 2 ? span / 2 : span * 9 / 10);
		if (p + 1000 > trk[100].lba)
			continue;
		for (i = 0; i < 3; i++) {		/* frente 800, volta 800, volta 400 */
			int a = i == 2 ? p + 400 : p, n = i == 2 ? 400 : 800, k;
			for (k = 0; k < n; k += 16) {
				ms = timed(0, a + k, 16);
				if (ms < 0)
					return;
				if (ms > STALL_MS) {
					qr->st_n++;
					L("[PS2] TRAVADA: ponto %d, passada %d, setor %d em %d ms", j + 1, i + 1, a + k, ms);
				}
				if (ms > qr->st_max)
					qr->st_max = ms;
			}
		}
		if (dead())
			return;
	}
	L("[PS2] travadas frente/volta: %d, pior %d ms", qr->st_n, qr->st_max);
}

void ps2_run(int d, const cdusb_dev_info *info, ps2_result *p)
{
	static diag_result dr;
	speed_result s;
	int last = -1, start, i;
	u64 t_all = ee_now_us();
	static const int waits[3] = {3, 6, 15};

	dev = d;
	sub = info->sub;
	res = &dr;
	qr = p;
	memset(&dr, 0, sizeof(dr));
	memset(p, 0, sizeof(*p));
	recoveries = 0;
	memcpy(p->vendor, info->inquiry + 8, 8);
	memcpy(p->product, info->inquiry + 16, 16);
	for (i = 0; i < 2; i++) {
		int k;
		for (k = 0; k < 3; k++)
			p->sp[i].kbs[k] = -1;
		p->sp[i].small_avg = -1;
		for (k = 0; k < PS2_SEEKS; k++)
			p->sp[i].seek[k] = -1;
	}
	p->seq300.cuts = p->jmp300.cuts = p->jmp600.cuts = -1;
	p->qkbs[0] = p->qkbs[1] = p->qkbs[2] = -1;
	q_mode = 0;
	for (i = 0; i < 3; i++)
		p->motor_ms[i] = -1;

	screen_title("TESTE PS2 (DVD/CD) - FASE COMPLETA, ~6 MIN");
	S("   LEITOR: %s %s", p->vendor, p->product);
	L("[PS2] ---------------- teste PS2 c09: disp %d, %04X:%04X ----------------", d, info->vid, info->pid);
	S("   ESPERANDO O DISCO...");
	if (!wait_ready())
		goto out;
	get_profile();
	p->profile = dr.profile;
	get_caps();
	p->max_kbs = dr.max_kbs;
	p->def_kbs = dr.cur_kbs;
	if (!read_toc() && read_capacity(&last)) {
		memset(trk, 0, sizeof(trk));		/* sem TOC: uma faixa de dados ate a capacidade */
		t_first = t_last = 1;
		trk[1].ctrl = 4;
		trk[100].lba = last + 1;
	}
	if (read_capacity(&last))
		L("[PS2] capacidade: ultimo setor %d (%d MB)", last, (int)((u64)(last + 1) * 2048 >> 20));
	if (trk[100].lba <= 0) {
		S("   NAO CONSEGUI LER O TAMANHO DO DISCO");
		goto out;
	}
	p->disc = 1;
	p->size_mb = (int)((u64)trk[100].lba * 2048 >> 20);
	p->dvd = p->profile >= 0x10 && p->profile < 0x40;
	if (p->dvd)
		dvd_layers();
	start = trk[first_of(1) >= 0 ? first_of(1) : t_first].lba;
	ps2_identify(start);
	S("   DISCO: %s %s, %d MB, %s", p->dvd ? "DVD" : "CD", p->system, p->size_mb, p->code[0] ? p->code : "?");
	if (dead())
		goto out;

	/* 2-4 na velocidade do leitor e travada */
	for (i = 0; i < 2; i++) {
		int kbs = p->dvd ? 1385 : 706;
		if (i == 0) {
			S("   VELOCIDADE DO LEITOR: SEQUENCIA, LEITURAS PEQUENAS, BUSCA...");
			L("[PS2] ======== velocidade do leitor ========");
			lock_kbs = 0;
		} else {
			S("   VELOCIDADE TRAVADA (%s): SEQUENCIA, LEITURAS PEQUENAS, BUSCA...", p->dvd ? "DVD 1X" : "CD 4X");
			L("[PS2] ======== travada em %d KB/s ========", kbs);
			memset(&s, 0, sizeof(s));
			set_speed(kbs, 0, &s);
			lock_kbs = kbs;
		}
		get_caps();
		p->sp[i].cur_kbs = dr.cur_kbs;
		ps2_speed_block(i, start);
		if (dead())
			goto out;
	}

	/* c10: a mesma sequencia com a fila completa no USB (o NEUTRINOUSB n04 usa) */
	{
		static const int pers[3] = {8, 16, 19};
		int k, w;
		S("   FILA COMPLETA NO USB: SEQUENCIA 8/16/19 SETORES...");
		L("[PS2] ======== fila completa no USB (travada) ========");
		q_mode = 1;
		for (k = 0; k < 3; k++) {
			p->qkbs[k] = ps2_bench(start, pers[k], BENCH_MS, &w);
			p->qworst[k] = w;
			if (dead())
				break;
		}
		q_mode = 0;
		L("[PS2] fila completa: %d / %d / %d KB/s (sem fila: %d / %d / %d)", p->qkbs[0], p->qkbs[1], p->qkbs[2],
		  p->sp[1].kbs[0], p->sp[1].kbs[1], p->sp[1].kbs[2]);
		if (dead())
			goto out;
	}

	/* 5. na velocidade travada */
	S("   TRAVADAS FRENTE/VOLTA EM 4 PONTOS...");
	ps2_stall(start);
	if (dead())
		goto out;
	S("   TEMPO REAL 300 KB/S, CACHE 128 KB: 30 S EM SEQUENCIA + 30 S COM PULOS...");
	ps2_pace(start + 500, 300, 128, 30, 0, &p->seq300);
	if (dead())
		goto out;
	ps2_pace(start + 500, 300, 128, 30, 3, &p->jmp300);
	if (dead())
		goto out;
	S("   TEMPO REAL 600 KB/S, CACHE 128 KB: 30 S COM PULOS...");
	ps2_pace(start + 500, 600, 128, 30, 3, &p->jmp600);
	if (dead())
		goto out;
	S("   MOTOR...");
	{
		int lba = start + 5000;
		for (i = 0; i < 3 && lba + 4000 < trk[100].lba; i++) {
			u64 t0;
			if (timed(0, lba, 1) < 0)
				break;
			S("   MOTOR: %d S SEM LER NADA...  (O = PULAR)", waits[i]);
			t0 = ee_now_us();
			while (ee_now_us() - t0 < (u64)waits[i] * 1000000) {
				if (pad_new() & PAD_CIRCLE)
					goto motor_out;
				ee_wait_ms(16);
			}
			p->motor_ms[i] = timed(0, lba + 1000, 1);
			L("[PS2] motor: depois de %d s parado, %d ms", waits[i], p->motor_ms[i]);
			lba += 1000;
		}
	}
motor_out:
	S("   VOLTANDO O LEITOR PARA A VELOCIDADE PADRAO...");
	set_speed(0xFFFF, 1, NULL);
out:
	lock_kbs = 706;
	p->recoveries = recoveries;
	p->stuck = dr.stuck;
	p->gone = dr.gone;
	p->aborted = dr.aborted;
	L("[PS2] fim do teste (%d s)", (int)((ee_now_us() - t_all) / 1000000));
	log_save();
}

void ps2_summary(const ps2_result *p, const char *log_file)
{
	int i, k;

	screen_title("RESULTADO PS2");
	S("   LEITOR: %s %s", p->vendor, p->product);
	if (p->gone)
		S("   O LEITOR DESCONECTOU NO MEIO DO TESTE: FALTA DE ENERGIA?");
	if (p->stuck)
		S("   O LEITOR TRAVOU (SEM RESPOSTA). DESLIGUE E LIGUE O LEITOR.");
	if (p->recoveries)
		S("   LEITORA REINICIOU %d VEZ(ES) NO MEIO (RECUPERADA)", p->recoveries);
	if (p->disc) {
		S("   DISCO: %s %s %s, %d MB%s", p->dvd ? "DVD" : "CD", p->system, p->code[0] ? p->code : "",
		  p->size_mb, p->layers > 1 ? ", 2 CAMADAS" : "");
		for (i = 0; i < 2; i++) {
			const ps2_speed *v = &p->sp[i];
			if (v->kbs[1] < 0)
				continue;
			S("   %s (LEITOR %d KB/S):", i ? "TRAVADA" : "LIVRE", v->cur_kbs);
			S("     SEQUENCIA 8/16/19: %d / %d / %d KB/S (PIOR %d/%d/%d MS)", v->kbs[0], v->kbs[1], v->kbs[2],
			  v->worst[0], v->worst[1], v->worst[2]);
			S("     PEQUENAS ESPALHADAS: MEDIA %d MS, PIOR %d MS", v->small_avg, v->small_max);
			for (k = 0; k < PS2_SEEKS; k++)
				if (v->seek[k] >= 0)
					S("     BUSCA %d MB%s: %d MS", p->seek_mb[k], k == 5 ? " (CAMADA)" : "", v->seek[k]);
		}
		if (p->qkbs[1] >= 0)
			S("   FILA COMPLETA 8/16/19: %d / %d / %d KB/S (PIOR %d/%d/%d MS)", p->qkbs[0], p->qkbs[1],
			  p->qkbs[2], p->qworst[0], p->qworst[1], p->qworst[2]);
		S("   TRAVADAS FRENTE/VOLTA: %d (PIOR %d MS)", p->st_n, p->st_max);
		if (p->seq300.cuts >= 0)
			S("   300 KB/S SEQUENCIA: %d CORTES, PIOR %d MS", p->seq300.cuts, p->seq300.max_ms);
		if (p->jmp300.cuts >= 0)
			S("   300 KB/S PULOS: %d CORTES, 1O BLOCO %d MS (PIOR %d)", p->jmp300.cuts,
			  p->jmp300.jumps ? p->jmp300.jwait_sum / p->jmp300.jumps : 0, p->jmp300.jwait_max);
		if (p->jmp600.cuts >= 0)
			S("   600 KB/S PULOS: %d CORTES, 1O BLOCO %d MS (PIOR %d)", p->jmp600.cuts,
			  p->jmp600.jumps ? p->jmp600.jwait_sum / p->jmp600.jumps : 0, p->jmp600.jwait_max);
		if (p->motor_ms[0] >= 0)
			S("   MOTOR PARADO 3/6/15 S: %d / %d / %d MS", p->motor_ms[0], p->motor_ms[1], p->motor_ms[2]);
	}
	S(" ");
	if (log_file)
		S("   LOG: %s", log_file);
	else
		S("   SEM PENDRIVE: LOG NAO GRAVADO");
}
