/*
 * cdcache.c - disco do leitor USB para os emuladores (fase 2).
 * Criado por Leonardo Torres.
 *
 * - Leitor travado em 10x (pede 4x; o ASUS nao desce de 10x). Na fase 1 a
 *   velocidade maxima (24x) travava 3,8 s ao trocar de rotacao; em 10x o
 *   pior comando foi 102 ms. A troca acontece aqui, na abertura, com uma
 *   leitura de aquecimento, nunca no meio do jogo. SO no leitor USB.
 * - Thread de leitura (prioridade maior que a do emulador) dorme enquanto
 *   o IOP fala com o leitor; le 16 setores por vez, ate 300 a frente.
 * - Dados por READ(10) (2048); musica por READ CD (2352). Os 150 setores
 *   antes da 1a faixa de musica num CD misto sao lidos um a um.
 */
#include <string.h>
#include <kernel.h>
#include <kernel_util.h>
#include <delaythread.h>

#include "cdusb_ee.h"
#include "cdcache.h"
#include "diag.h"

#define SLOTS		512
#define CHUNK		16
#define AHEAD		300
#define TMO_CMD		5000
#define TMO_READ	10000
#define LOCK_KBS	706		/* pede 4x */
#define READER_PRIO	0x30
#define KEEP_IDLE_MS	2000		/* sem ler ha 2 s: le 1 setor (o leitor desacelera em ~3 s) */

static int dev, sub;
static cdcd_toc toc;

static u8 cache[SLOTS][2352] __attribute__((aligned(64)));
static int tag[SLOTS];			/* setor guardado (-1 = nada) */
static u8 bad[SLOTS];			/* 1 = leitura falhou */
static u8 tmp[CHUNK * 2352] __attribute__((aligned(64)));

static volatile int consumer;		/* proximo setor que o emulador quer */
static volatile int fill;		/* proximo setor que a thread vai ler */
static volatile int restart;		/* 1 = recomecar a leitura em consumer */
static volatile int quit, running, done;
static int lock_sema = -1, kick_sema = -1, data_sema = -1, thread_id = -1;
static cdcd_stats st;
static u64 last_io;			/* ultimo comando de leitura no leitor */
static int keep_flip;
#define PRE_N		64		/* comeco da proxima faixa de musica (0,85 s) */
static u8 pre[PRE_N][2352] __attribute__((aligned(64)));
static volatile int pre_lba = -1, pre_n;	/* pre-carga (-1 = nada) */
static int after_jump;			/* 1 = proxima leitura e a primeira depois de um pulo */
static u8 reader_stack[16 * 1024] __attribute__((aligned(16)));
extern void *_gp;

/* ------------------------------------------------------------------ TOC */
const cdcd_toc *cdcd_get_toc(void)
{
	return &toc;
}

int cdcd_track_of(int lba)
{
	int t;
	for (t = toc.last; t > toc.first; t--)
		if (lba >= toc.lba[t])
			return t;
	return toc.first;
}

int cdcd_is_data(int t)
{
	return (toc.ctrl[t] & 4) != 0;
}

int cdcd_track_end(int t)
{
	return t < toc.last ? toc.lba[t + 1] : toc.lba[100];
}

/* fim da leitura em bloco numa faixa de dados (pausa antes da musica fica de fora) */
static int safe_end(int t)
{
	int e = cdcd_track_end(t);
	if (cdcd_is_data(t) && t < toc.last && !cdcd_is_data(t + 1) && e - 150 > toc.lba[t])
		e -= 150;
	return e;
}

/* ------------------------------------------------------------------ comandos */
static int cdb_len(u8 op)
{
	if (sub == 0x02)
		return 12;
	if (op < 0x20)
		return 6;
	if (op < 0x60)
		return 10;
	return 12;
}

static int cmd(const u8 *c, int dir, void *data, int len, int tmo, cdusb_scsi_resp *r)
{
	return cdusb_scsi(dev, c, cdb_len(c[0]), dir, data, len, tmo, r);
}

static int read_sectors(int lba, int n, int data, void *dst)
{
	u8 c[12];
	cdusb_scsi_resp r;

	memset(c, 0, sizeof(c));
	c[2] = lba >> 24; c[3] = lba >> 16; c[4] = lba >> 8; c[5] = lba;
	if (data) {
		c[0] = 0x28;
		c[7] = n >> 8; c[8] = n;
		return cmd(c, 1, dst, n * 2048, TMO_READ, &r);
	}
	c[0] = 0xBE; c[1] = 0x04;			/* READ CD, musica */
	c[6] = n >> 16; c[7] = n >> 8; c[8] = n;
	c[9] = 0x10;
	return cmd(c, 1, dst, n * 2352, TMO_READ, &r);
}

static int read_toc(void)
{
	u8 c[12] = {0x43, 0, 0, 0, 0, 0, 1, 0, 4, 0, 0, 0};
	cdusb_scsi_resp r;
	int len, i, have_end = 0;

	memset(tmp, 0, 4);
	if (cmd(c, 1, tmp, 4, TMO_CMD, &r) != CDUSB_OK)
		return 0;
	len = (tmp[0] << 8 | tmp[1]) + 2;
	if (len < 12 || len > 4 + 100 * 8)
		return 0;
	c[7] = len >> 8;
	c[8] = len;
	memset(tmp, 0, len);
	if (cmd(c, 1, tmp, len, TMO_CMD, &r) != CDUSB_OK)
		return 0;
	if (r.residue > 0 && r.residue < len)
		len -= r.residue;
	memset(&toc, 0, sizeof(toc));
	toc.first = tmp[2];
	toc.last = tmp[3];
	for (i = 4; i + 8 <= len; i += 8) {
		int t = tmp[i + 2], idx = (t == 0xAA) ? 100 : t;
		if (idx < 1 || idx > 100)
			continue;
		toc.lba[idx] = tmp[i + 4] << 24 | tmp[i + 5] << 16 | tmp[i + 6] << 8 | tmp[i + 7];
		toc.ctrl[idx] = tmp[i + 1] & 0x0f;
		if (idx == 100)
			have_end = 1;
	}
	return toc.first >= 1 && toc.last >= toc.first && toc.last <= 99 && have_end &&
	       toc.lba[100] > toc.lba[toc.last] && toc.lba[100] <= 450000;
}

/* espera o disco ficar pronto (troca de disco / motor girando) */
static int wait_ready(int (*abort)(void))
{
	u8 c[12] = {0};
	cdusb_scsi_resp r;
	int i, res;

	for (i = 0; i < 400; i++) {			/* ~2 min */
		if (abort && abort())
			return 0;
		res = cmd(c, 0, NULL, 0, TMO_CMD, &r);
		if (res == CDUSB_OK)
			return 1;
		if (res < 0 && (res == CDUSB_E_NODEV || res == CDUSB_E_EE_TIMEOUT))
			return 0;
		if (res == CDUSB_CHECK && ((r.sense >> 16) & 0x0f) == 6)
			continue;			/* aviso de troca de disco */
		DelayThread(300 * 1000);
	}
	return 0;
}

/* trava a velocidade (SET CD SPEED + SET STREAMING); o leitor arredonda */
static void set_speed(int kbs, int restore)
{
	u8 c[12], pd[28];
	cdusb_scsi_resp r;
	int end = toc.lba[100] > 0 ? toc.lba[100] - 1 : 0, a, b;

	memset(c, 0, sizeof(c));
	c[0] = 0xBB;
	c[2] = kbs >> 8; c[3] = kbs;
	c[4] = 0xFF; c[5] = 0xFF;
	a = cmd(c, 0, NULL, 0, TMO_CMD, &r);

	memset(pd, 0, sizeof(pd));
	if (restore) {
		pd[0] = 0x04;				/* RDD: padrao do leitor */
	} else {
		pd[8] = end >> 24; pd[9] = end >> 16; pd[10] = end >> 8; pd[11] = end;
		pd[14] = kbs >> 8; pd[15] = kbs;
		pd[18] = 1000 >> 8; pd[19] = 1000 & 0xff;
		pd[22] = kbs >> 8; pd[23] = kbs;
		pd[26] = 1000 >> 8; pd[27] = 1000 & 0xff;
	}
	memset(c, 0, sizeof(c));
	c[0] = 0xB6;
	c[10] = sizeof(pd);
	b = cdusb_scsi(dev, c, 12, 2, pd, sizeof(pd), TMO_CMD, &r);
	L("[CDCACHE] velocidade %s: SET CD SPEED %d, SET STREAMING %d", restore ? "padrao" : "travada", a, b);
	if (!restore)
		st.speed_locked = (a == CDUSB_OK || b == CDUSB_OK);
}

/* ------------------------------------------------------------------ thread de leitura */
static void lock(void)
{
	WaitSema(lock_sema);
}

static void unlock(void)
{
	SignalSema(lock_sema);
}

/* proxima faixa de musica depois de t (-1 = nenhuma) */
static int next_audio_track(int t)
{
	for (t++; t <= toc.last; t++)
		if (!cdcd_is_data(t))
			return t;
	return -1;
}

/* musica atual toda lida e a faixa seguinte e de dados (PC Engine: faixa 2)?
 * devolve a proxima faixa de musica (-1 = nao) */
static int boundary_next(int c, int f)
{
	int t = cdcd_track_of(c), n;

	if (!cdcd_is_data(t) && t < toc.last && cdcd_is_data(t + 1) && f >= cdcd_track_end(t) &&
	    (n = next_audio_track(t)) > 0)
		return n;
	return -1;
}

/* le os primeiros PRE_N setores da faixa n para a pre-carga: quando a musica
 * pular para la, o som sai da memoria na hora */
static void prefetch(int n)
{
	int lba = toc.lba[n], cnt = PRE_N, got = 0, k, r;
	u64 t0 = ee_now_us();

	if (cdcd_track_end(n) - lba < cnt)
		cnt = cdcd_track_end(n) - lba;
	lock();
	pre_lba = -1;
	pre_n = 0;
	unlock();
	while (got < cnt && !restart && !quit) {
		k = cnt - got > CHUNK ? CHUNK : cnt - got;
		r = read_sectors(lba + got, k, 0, tmp);
		if (r != CDUSB_OK)
			break;
		memcpy(pre[got], tmp, k * 2352);
		got += k;
	}
	last_io = ee_now_us();
	lock();
	if (got > 0) {
		pre_lba = lba;
		pre_n = got;
	}
	unlock();
	L("[CDCACHE] pre-carga da faixa %d: %d setores em %d ms", n, got, (int)((ee_now_us() - t0) / 1000));
}

/*
 * Manter girando: 1 setor, sem guardar. Se a musica atual ja foi toda lida e
 * a faixa seguinte e de dados (PC Engine: faixa 2), le no comeco da proxima
 * faixa de musica: o leitor ja fica la e a troca de faixa e imediata.
 * Senao le perto da posicao de leitura, alternando +256 (setor novo: o
 * leitor tem que ir ao disco e nao ao buffer dele).
 */
static void keepalive(void)
{
	int c = consumer, f = fill, t = cdcd_track_of(c), lba, n, data, r, ms;
	u64 t0;

	if ((n = boundary_next(c, f)) > 0) {
		/* logo depois da pre-carga: o leitor continua dali sem buscar */
		lba = (pre_lba == toc.lba[n] ? pre_lba + pre_n : toc.lba[n]) + (keep_flip ? 8 : 0);
		if (lba >= cdcd_track_end(n))
			lba = toc.lba[n];
	} else {
		lba = f < toc.lba[100] ? f : toc.lba[100] - 1;
		t = cdcd_track_of(lba);
		if (keep_flip && lba + 256 < (cdcd_is_data(t) ? safe_end(t) : cdcd_track_end(t)))
			lba += 256;
	}
	keep_flip ^= 1;
	t = cdcd_track_of(lba);
	data = cdcd_is_data(t);
	if (data && lba >= safe_end(t))
		lba = safe_end(t) - 1;
	t0 = ee_now_us();
	r = read_sectors(lba, 1, data, tmp);
	ms = (int)((ee_now_us() - t0) / 1000);
	last_io = ee_now_us();
	st.keepalives++;
	if (ms > st.keep_max_ms)
		st.keep_max_ms = ms;
	if (r != CDUSB_OK || ms > 300)
		L("[CDCACHE] manter girando: setor %d %s em %d ms", lba, r == CDUSB_OK ? "OK" : "FALHOU", ms);
}

static void reader(void *arg)
{
	int f, target, t, end, n, data, r, i, slot, gap;
	u64 t0, wait_us;

	(void)arg;
	while (!quit) {
		lock();
		if (restart) {
			int c = consumer;
			if (pre_lba >= 0 && c >= pre_lba && c < pre_lba + pre_n) {
				/* pulo para a pre-carga: passa para a memoria principal */
				for (i = c - pre_lba; i < pre_n; i++) {
					slot = (pre_lba + i) % SLOTS;
					memcpy(cache[slot], pre[i], 2352);
					bad[slot] = 0;
					tag[slot] = pre_lba + i;
				}
				fill = pre_lba + pre_n;
				pre_lba = -1;
			} else
				fill = c;
			restart = 0;
			st.repositions++;
			after_jump = 1;
		}
		f = fill;
		target = consumer + AHEAD;
		unlock();
		/* nao le adiante outro tipo de faixa (tocando musica, a faixa de dados
		 * seguinte nao interessa; o emulador pula para ela se precisar) */
		if (f >= 0 && f < toc.lba[100] && cdcd_is_data(cdcd_track_of(f)) != cdcd_is_data(cdcd_track_of(consumer)))
			f = target;
		if (f >= target || f >= toc.lba[100] || f < 0) {
			int nb = boundary_next(consumer, fill);
			if (nb > 0 && pre_lba != toc.lba[nb])
				prefetch(nb);
			else if (ee_now_us() - last_io > (u64)KEEP_IDLE_MS * 1000)
				keepalive();
			wait_us = 10000;
			WaitSemaEx(kick_sema, 1, &wait_us);
			continue;
		}
		t = cdcd_track_of(f);
		data = cdcd_is_data(t);
		end = data ? safe_end(t) : cdcd_track_end(t);
		gap = 0;
		if (data && f >= end)
			n = 1;				/* pausa antes da musica: um por vez */
		else {
			n = end - f;
			if (n > CHUNK)
				n = CHUNK;
			/* musica seguida de faixa de dados: os ultimos 150 setores sao a
			 * pausa da faixa de dados (nao e musica): silencio sem ler */
			if (!data && t < toc.last && cdcd_is_data(t + 1) && end - f <= 150)
				gap = 1;
			else if (!data && t < toc.last && cdcd_is_data(t + 1) && f + n > end - 150)
				n = end - 150 - f;
		}
		if (gap) {
			memset(tmp, 0, n * 2352);
			r = CDUSB_OK;
		} else {
			t0 = ee_now_us();
			r = read_sectors(f, n, data, tmp);
			if (r != CDUSB_OK)
				r = read_sectors(f, n, data, tmp);	/* uma segunda tentativa */
			st.last_ms = (int)((ee_now_us() - t0) / 1000);
			if (st.last_ms > st.max_ms)
				st.max_ms = st.last_ms;
			st.cmds++;
			last_io = ee_now_us();
			if (after_jump)
				L("[CDCACHE] pulo para o setor %d: 1a leitura %d ms", f, st.last_ms);
			else if (st.last_ms > 300)
				L("[CDCACHE] LENTO: setor %d x%d em %d ms", f, n, st.last_ms);
			after_jump = 0;
		}
		if (r != CDUSB_OK) {
			st.errors++;
			L("[CDCACHE] leitura falhou: setor %d x%d (%d)", f, n, r);
			if (r == CDUSB_E_EE_TIMEOUT || r == CDUSB_E_NODEV) {
				DelayThread(100 * 1000);	/* leitor sumiu: nao insiste rapido */
			}
		}

		lock();
		if (!restart && fill == f) {
			for (i = 0; i < n; i++) {
				slot = (f + i) % SLOTS;
				if (r == CDUSB_OK) {
					memcpy(cache[slot], tmp + i * (data ? 2048 : 2352), data ? 2048 : 2352);
					bad[slot] = 0;
				} else {
					memset(cache[slot], 0, 2352);	/* musica: silencio */
					bad[slot] = 1;
				}
				tag[slot] = f + i;
			}
			fill = f + n;
		}
		unlock();
		SignalSema(data_sema);
	}
	done = 1;
	ExitThread();
}

static int new_sema(int init)
{
	ee_sema_t s;
	memset(&s, 0, sizeof(s));
	s.init_count = init;
	s.max_count = 1;
	return CreateSema(&s);
}

/* ------------------------------------------------------------------ uso */
int cdcd_open(int d, int sb, int (*abort)(void))
{
	ee_thread_t th;
	int i, a, b;
	u64 t0;

	cdcd_close();
	dev = d;
	sub = sb;
	memset(&st, 0, sizeof(st));
	for (i = 0; i < SLOTS; i++)
		tag[i] = -1;

	if (!wait_ready(abort)) {
		L("[CDCACHE] disco nao ficou pronto");
		return -1;
	}
	if (!read_toc()) {
		L("[CDCACHE] TOC invalida");
		return -2;
	}
	L("[CDCACHE] disco: faixas %d a %d, fim %d", toc.first, toc.last, toc.lba[100]);

	set_speed(LOCK_KBS, 0);
	/* aquecimento: a troca de rotacao (ate ~4 s) acontece agora */
	t0 = ee_now_us();
	a = read_sectors(toc.lba[toc.first], 1, cdcd_is_data(toc.first), tmp);
	i = toc.lba[100] - 600;
	b = i > toc.lba[toc.first] ? read_sectors(i, 1, cdcd_is_data(cdcd_track_of(i)), tmp) : 0;
	L("[CDCACHE] aquecimento: %d / %d em %d ms", a, b, (int)((ee_now_us() - t0) / 1000));

	if (lock_sema < 0) {
		lock_sema = new_sema(1);
		kick_sema = new_sema(0);
		data_sema = new_sema(0);
	}
	consumer = fill = toc.lba[toc.first];
	pre_lba = -1;
	pre_n = 0;
	last_io = ee_now_us();
	keep_flip = 0;
	restart = 0;
	quit = done = 0;
	memset(&th, 0, sizeof(th));
	th.func = (void *)reader;
	th.stack = reader_stack;
	th.stack_size = sizeof(reader_stack);
	th.gp_reg = &_gp;
	th.initial_priority = READER_PRIO;
	thread_id = CreateThread(&th);
	if (thread_id < 0) {
		L("[CDCACHE] thread nao criada (%d)", thread_id);
		return -3;
	}
	StartThread(thread_id, NULL);
	running = 1;
	return 0;
}

void cdcd_close(void)
{
	int i;

	if (!running)
		return;
	quit = 1;
	SignalSema(kick_sema);
	for (i = 0; i < 300 && !done; i++)	/* ate ~15 s (leitura em andamento) */
		DelayThread(50 * 1000);
	if (done)
		DeleteThread(thread_id);
	thread_id = -1;
	running = 0;
	if (!cdusb_stuck())
		set_speed(0xFFFF, 1);
	L("[CDCACHE] fechado: %d leituras, pior %d ms, %d pulos, %d erros; manter girando %d (pior %d ms)",
	  st.cmds, st.max_ms, st.repositions, st.errors, st.keepalives, st.keep_max_ms);
}

/* setores prontos a partir de lba, copiados (o lock ja esta pego) */
static int take(int lba, int n, u8 *dst, int *badp)
{
	int i, slot, data, size;

	for (i = 0; i < n; i++) {
		slot = (lba + i) % SLOTS;
		if (tag[slot] != lba + i) {
			/* pre-carga (so musica) */
			if (pre_lba >= 0 && lba + i >= pre_lba && lba + i < pre_lba + pre_n) {
				memcpy(dst + i * 2352, pre[lba + i - pre_lba], 2352);
				continue;
			}
			break;
		}
		data = cdcd_is_data(cdcd_track_of(lba + i));
		size = data ? 2048 : 2352;
		memcpy(dst + i * size, cache[slot], size);
		if (bad[slot] && badp)
			*badp = 1;
	}
	return i;
}

/* o setor pedido vai chegar logo (a thread esta lendo para la)? */
static int coming(int lba)
{
	return !restart && lba >= consumer && lba >= fill - CHUNK && lba < fill + 2 * CHUNK;
}

int cdcd_read_nb(int lba, int n, void *dst)
{
	int k;

	if (!running)
		return 0;
	lock();
	k = take(lba, n, dst, NULL);
	if (k > 0) {
		consumer = lba + k;
		/* veio da pre-carga (longe da leitura): a thread vai para la */
		if (consumer > fill + 2 * CHUNK || consumer < fill - SLOTS + CHUNK)
			restart = 1;
	} else if (!coming(lba)) {
		consumer = lba;
		restart = 1;
	} else if (lba > consumer)
		consumer = lba;
	unlock();
	if (k == 0 || restart)
		SignalSema(kick_sema);
	return k;
}

int cdcd_read(int lba, int n, void *dst, int timeout_ms)
{
	u8 *p = dst;
	int got = 0, k, bad_hit = 0;
	u64 t0 = ee_now_us(), w;

	if (!running)
		return 0;
	while (got < n) {
		lock();
		k = take(lba + got, n - got, p, &bad_hit);
		if (k > 0) {
			consumer = lba + got + k;
			if (consumer > fill + 2 * CHUNK || consumer < fill - SLOTS + CHUNK)
				restart = 1;
			p += k * (cdcd_is_data(cdcd_track_of(lba + got)) ? 2048 : 2352);
			got += k;
		} else if (!coming(lba + got)) {
			consumer = lba + got;
			restart = 1;
		} else if (lba + got > consumer)
			consumer = lba + got;
		unlock();
		if (bad_hit)
			return got;			/* dado com erro: o emulador decide */
		if (got >= n)
			break;
		SignalSema(kick_sema);
		if (ee_now_us() - t0 > (u64)timeout_ms * 1000)
			break;
		w = 5000;
		WaitSemaEx(data_sema, 1, &w);
	}
	return got;
}

void cdcd_get_stats(cdcd_stats *s)
{
	lock();
	st.ahead = (running && !restart && fill > consumer) ? fill - consumer : 0;
	*s = st;
	unlock();
}
