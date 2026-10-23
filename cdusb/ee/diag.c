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

/* 1 setor, com o comando certo para o tipo da faixa */
static int read_one(int lba)
{
	int t = track_of(lba);
	if (is_data_t(t)) {
		if (lba >= safe_end(t))
			lba = safe_end(t) - 1;
		return read10(lba, 1);
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

/* leitura em sequencia por ms; devolve bytes por segundo (-1 = falhou) */
static int bench(int audio, int t, int per, int ms, int *ms_max)
{
	int start = trk[t].lba, end = audio ? track_end(t) : safe_end(t);
	int lba, r, n = 0, mx = 0, mn = 1 << 30, c, wraps = 0;
	int bytes_per = audio ? 2352 : 2048;
	u32 last, now, c0;
	u64 acc = 0, bytes = 0, limit = (u64)ms * EE_HZ_MS;
	char what[48];

	snprintf(what, sizeof(what), "%s, %d setores por comando", audio ? "musica" : "dados", per);
	if (end - start < per * 8) {
		L("[BENCH] %s: faixa curta demais", what);
		return -1;
	}
	/* primeira leitura fora da conta (busca + motor) */
	lba = start;
	r = audio ? readcd(lba, per, 1, 0x10, 0) : read10(lba, per);
	if (r != CDUSB_OK) {
		lcmd(what, r);
		return -1;
	}
	lba += per;
	last = ee_ticks();
	while (acc < limit) {
		if (lba + per > end) {
			lba = start;
			wraps++;
		}
		c0 = ee_ticks();
		r = audio ? readcd(lba, per, 1, 0x10, 0) : read10(lba, per);
		c = ee_ms(c0);
		now = ee_ticks();
		acc += (u32)(now - last);
		last = now;
		if (r != CDUSB_OK) {
			lcmd(what, r);
			L("[BENCH] %s: falhou no setor %d depois de %d comandos", what, lba, n);
			return -1;
		}
		bytes += (u64)per * bytes_per;
		n++;
		if (c > mx) mx = c;
		if (c < mn) mn = c;
		lba += per;
	}
	{
		u32 total_ms = (u32)(acc / EE_HZ_MS);
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

static int pick_far(void)
{
	int lba = trk[100].lba - 600;
	return lba > trk[t_first].lba ? lba : -1;
}

static int pick_near(void)
{
	return trk[t_first].lba + 300;
}

static void test_seek(void)
{
	int near = pick_near(), far = pick_far(), i, r, ms, sum = 0, cnt = 0;
	u32 t0;

	res->seek_avg = res->seek_max = -1;
	if (far < 0 || far - near < 20000) {
		L("[SEEK] disco curto demais para medir a busca");
		return;
	}
	for (i = 0; i < 7; i++) {
		int lba = (i & 1) ? far : near;
		lba += i * 8;			/* setores novos: nada no cache do leitor */
		t0 = ee_ticks();
		r = read_one(lba);
		ms = ee_ms(t0);
		L("[SEEK] setor %d: %s em %d ms", lba, rtxt(r), ms);
		if (r != CDUSB_OK)
			return;
		if (i == 0)
			continue;		/* a primeira so posiciona */
		sum += ms;
		cnt++;
		if (ms > res->seek_max)
			res->seek_max = ms;
	}
	res->seek_avg = sum / cnt;
	L("[SEEK] busca media %d ms, pior %d ms", res->seek_avg, res->seek_max);
}

static void test_motor(void)
{
	int lba = pick_near() + 2000, r, ms;
	u32 t0;

	res->spinup_ms = -1;
	if (lba >= trk[100].lba)
		return;
	S("   TESTE DO MOTOR: 15 S SEM LER NADA...  (O = PULAR)");
	t0 = ee_ticks();
	while (ee_ms(t0) < MOTOR_WAIT_MS) {
		if (pad_new() & PAD_CIRCLE) {
			L("[MOTOR] pulado pelo usuario");
			return;
		}
		ee_wait_ms(16);
	}
	t0 = ee_ticks();
	r = read_one(lba);
	ms = ee_ms(t0);
	L("[MOTOR] depois de 15 s parado: setor %d %s em %d ms", lba, rtxt(r), ms);
	if (r == CDUSB_OK)
		res->spinup_ms = ms;
}

/* ------------------------------------------------------------------ roteiro */
#define STOP_IF_DEAD() do { log_save(); if (res->stuck || res->gone) return; } while (0)

void diag_run(int d, const cdusb_dev_info *info, diag_result *r)
{
	int fd, fa;

	dev = d;
	sub = info->sub;
	res = r;
	memset(r, 0, sizeof(*r));
	r->kbs_data8 = r->kbs_data16 = r->kbs_audio = -1;
	r->seek_avg = r->seek_max = r->spinup_ms = -1;
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
	fa = first_of(0);
	if (fd >= 0 && r->data_ok) {
		S("   MEDINDO A VELOCIDADE DOS DADOS...");
		r->kbs_data8 = bench(0, fd, 8, BENCH_MS / 2, NULL);
		STOP_IF_DEAD();
		r->kbs_data16 = bench(0, fd, 16, BENCH_MS, &r->ms_max_data);
		STOP_IF_DEAD();
	}
	if (fa >= 0 && r->audio_ok) {
		/* a maior faixa de musica (a 1a pode ser curta) */
		int t, best = fa;
		for (t = t_first; t <= t_last; t++)
			if (!is_data_t(t) && track_end(t) - trk[t].lba > track_end(best) - trk[best].lba)
				best = t;
		S("   MEDINDO A VELOCIDADE DA MUSICA...");
		r->kbs_audio = bench(1, best, 16, BENCH_MS, &r->ms_max_audio);
		STOP_IF_DEAD();
	}

	S("   MEDINDO A BUSCA (PULOS NO DISCO)...");
	test_seek();
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

static const char *grade(int x10)
{
	return x10 >= 15 ? "OK" : x10 >= 10 ? "NO LIMITE" : "LENTO";
}

void diag_summary(const diag_result *r, const char *log_file)
{
	int ok = 1, x;

	screen_title("RESULTADO");
	S("   LEITOR:   %s %s %s", r->vendor, r->product, r->rev);
	S("             USB %04X:%04X, PORTA %d, SUBCLASSE %02X", r->vid, r->pid, r->port, r->sub);
	if (r->aborted) {
		S("   TESTE INTERROMPIDO ANTES DO DISCO");
		ok = 0;
	}
	if (r->gone) {
		S("   O LEITOR DESCONECTOU NO MEIO DO TESTE: FALTA DE ENERGIA?");
		S("   LIGUE O SEGUNDO PLUGUE DO CABO Y NUM CARREGADOR.");
		ok = 0;
	}
	if (r->stuck) {
		S("   O LEITOR TRAVOU (SEM RESPOSTA). DESLIGUE E LIGUE O LEITOR.");
		ok = 0;
	}
	if (!r->disc) {
		if (!r->aborted && !r->gone && !r->stuck)
			S("   NAO CONSEGUI LER O DISCO");
		ok = 0;
	} else {
		S("   DISCO:    %s", r->system);
		S("             %d FAIXAS: %d DE DADOS, %d DE MUSICA", r->last - r->first + 1, r->data_tracks,
		  r->audio_tracks);
		if (r->data_tracks) {
			if (r->kbs_data16 > 0) {
				x = times10(r->kbs_data16, 0);
				S("   DADOS:    %d KB/S = %d,%dx  (PRECISA 1x)  %s", r->kbs_data16 / 1024, x / 10, x % 10,
				  grade(x));
				if (x < 10)
					ok = 0;
			} else {
				S("   DADOS:    NAO CONSEGUI MEDIR");
				ok = 0;
			}
		}
		if (r->audio_tracks) {
			if (!r->audio_ok) {
				S("   MUSICA:   O LEITOR NAO ENTREGA A MUSICA PELO USB");
				ok = 0;
			} else if (r->audio_silent) {
				S("   MUSICA:   VEIO SO SILENCIO (CONFERIR NO LOG)");
				ok = 0;
			} else if (r->kbs_audio > 0) {
				x = times10(r->kbs_audio, 1);
				S("   MUSICA:   %d KB/S = %d,%dx  (PRECISA 1x)  %s", r->kbs_audio / 1024, x / 10, x % 10,
				  grade(x));
				if (x < 10)
					ok = 0;
			} else {
				S("   MUSICA:   NAO CONSEGUI MEDIR");
				ok = 0;
			}
			S("   SUBCANAL: Q %s, P-W %s", r->subq_ok ? "OK" : "NAO", r->subraw_ok ? "OK" : "NAO");
		}
		if (r->seek_avg >= 0)
			S("   BUSCA:    MEDIA %d MS, PIOR %d MS", r->seek_avg, r->seek_max);
		if (r->spinup_ms >= 0)
			S("   MOTOR:    DEPOIS DE 15 S PARADO, %d MS PARA VOLTAR A LER", r->spinup_ms);
	}
	S(" ");
	S("   VEREDITO: %s", ok ? "SERVE PARA PCE-CD, SEGA CD E NEO GEO CD" : "AINDA NAO SERVE (VER ACIMA)");
	if (log_file)
		S("   LOG: %s", log_file);
	else
		S("   SEM PENDRIVE: LOG NAO GRAVADO");
}
