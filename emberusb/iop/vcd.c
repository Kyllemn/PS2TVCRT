/*
 * vcd.irx - pasta virtual para o Ember: o CD de PS1 na leitora USB vira
 * <nome>.cue + <nome>.bin. Criado por Leonardo Torres.
 *
 * O Ember le tudo pelos drivers que o lancador deixou no IOP e acha a
 * propria pasta pelo argv[0]. O lancador abre o Ember como
 * "vcd:/Ember/ember.elf": tudo em vcd:/... passa direto para mass:/...
 * (ember.elf, bios.bin, settings.txt, memory cards), menos dois arquivos
 * na pasta do jogo, que vem da leitora:
 *   <dir>/<nome>.cue  gerado pela TOC do disco
 *   <dir>/<nome>.bin  o disco inteiro, 2352 bytes por setor
 *
 * Licoes dos testes c07/c08 (ASUS SDRW-08D1S-U):
 *  - leitura sempre de 16 setores (com 8 a leitora travava 1,5-2 s);
 *  - dados crus por READ CD (tipo qualquer, 0xF8); musica por READ CD
 *    CD-DA; a pausa de 150 setores antes da musica sai como musica;
 *  - velocidade travada (pede 4x, a ASUS fica em 10x);
 *  - o motor as vezes desacelera em ~6 s parado (3 s para voltar): uma
 *    leitura a cada 2 s parado mantem o disco girando;
 *  - a leitora pode parar de responder (prazo, reset): espera ela voltar,
 *    trava a velocidade de novo e repete a leitura.
 *
 * Argumentos: root=mass: dir=/Ember/games/SLUS-00606 name=SLUS-00606 blocks=8
 */
#include "irx_imports.h"
#include "cdusb_iop.h"

#define MODNAME "vcd"
IRX_ID(MODNAME, 1, 1);

#define SECT		2352
#define BLK		16			/* setores por leitura (nunca menos) */
#define BLK_BYTES	(BLK * SECT)
#define MAX_BLOCKS	16
#define TMO_READ	6000
#define KEEP_MS		2000
#define SLOW_MS		300
#define LOG_FLUSH_MS	20000
#define STAT_MS		30000
#define LOCK_KBS	706

/* ------------------------------------------------------------------ estado */
static char root[16] = "mass:";
static char gdir[180] = "/Ember/games/CD-USB";
static char gname[48] = "CD-USB";
static char cue_path[256], bin_path[256], log_path[256];
static int nblocks = 8;

static int cd = -1, cdsub;
static int t_first, t_last, t_lba[101], t_ctrl[101];
static int data_mode = 2;		/* PS1 = modo 2 (XA); conferido no setor 16 */
static volatile int ready;		/* 0 = preparando, 1 = pronto, -1 = falhou */
static char cue[6144];
static int cue_len;
static unsigned int bin_size;

typedef struct {
	int base;			/* 1o setor do bloco (-1 = vazio) */
	volatile int st;		/* 0 vazio, 1 lendo, 2 pronto */
	volatile int pin;		/* o Ember esta copiando dele */
	u32 used;
} slot_t;
static slot_t slots[MAX_BLOCKS];
static u8 *cache;
static int lock = -1;
static u32 tick;
static volatile int want = -1;		/* proximo bloco que o Ember deve pedir */
static unsigned int last_io;
static u8 scratch[SECT] __attribute__((aligned(64)));

static struct {
	unsigned int reqs, hits, waits, misses, pref, slow, worst, errors, recov, keeps, keep_worst;
} st;

/* ------------------------------------------------------------------ log */
static char logbuf[6144];
static int loglen, log_lock = -1;

static unsigned int now_ms(void)
{
	iop_sys_clock_t c;
	u32 s, us;
	GetSystemTime(&c);
	SysClock2USec(&c, &s, &us);
	return (unsigned int)(s * 1000 + us / 1000);
}

static void L(const char *fmt, ...)
{
	static char line[640];
	va_list ap;
	int n;
	unsigned int t = now_ms();

	if (log_lock >= 0)
		WaitSema(log_lock);
	n = sprintf(line, "[%5u.%03u] ", t / 1000, t % 1000);
	va_start(ap, fmt);
	n += vsprintf(line + n, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 3)
		n = sizeof(line) - 3;
	line[n++] = '\r';
	line[n++] = '\n';
	line[n] = 0;
	if (loglen + n < (int)sizeof(logbuf)) {
		memcpy(logbuf + loglen, line, n);
		loglen += n;
	}
	if (log_lock >= 0)
		SignalSema(log_lock);
}

static void log_flush(void)
{
	static char out[sizeof(logbuf)];
	int n, fd;

	WaitSema(log_lock);
	n = loglen;
	memcpy(out, logbuf, n);
	loglen = 0;
	SignalSema(log_lock);
	if (!n)
		return;
	fd = iomanX_open(log_path, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_APPEND, 0666);
	if (fd < 0)
		return;
	iomanX_write(fd, out, n);
	iomanX_close(fd);
}

/* ------------------------------------------------------------------ comandos */
static int cdb_len(u8 op)
{
	if (cdsub == 0x02)
		return 12;
	if (op < 0x20)
		return 6;
	if (op < 0x60)
		return 10;
	return 12;
}

static int cmd(const u8 *cdb, int dir, void *data, u32 len, int ms, int *sense)
{
	int r = cdusb_iop_scsi(cd, cdb, cdb_len(cdb[0]), dir, data, len, ms, sense);
	last_io = now_ms();
	return r;
}

static void put_lba(u8 *c, int lba)
{
	c[2] = lba >> 24; c[3] = lba >> 16; c[4] = lba >> 8; c[5] = lba;
}

/* espera o disco ficar pronto (acha o leitor de novo se ele reconectou) */
static int wait_ready(int ms)
{
	u8 c[12] = {0};
	unsigned int t0 = now_ms();
	int r, sense;

	while (now_ms() - t0 < (u32)ms) {
		int i = cdusb_iop_find_cd(&cdsub);
		if (i < 0) {
			DelayThread(300 * 1000);
			continue;
		}
		cd = i;
		r = cmd(c, 0, NULL, 0, 3000, &sense);
		if (r == CDUSB_OK)
			return 1;
		if (r == CDUSB_CHECK && ((sense >> 16) & 0x0f) == 6)
			continue;		/* aviso de troca/reinicio: de novo ja */
		DelayThread(300 * 1000);
	}
	return 0;
}

static void set_speed(void)
{
	u8 c[12], pd[28];
	int end = t_lba[100] > 0 ? t_lba[100] - 1 : 0, sense, r1, r2;

	memset(c, 0, sizeof(c));
	c[0] = 0xBB;
	c[2] = LOCK_KBS >> 8; c[3] = LOCK_KBS & 0xff;
	c[4] = 0xFF; c[5] = 0xFF;
	r1 = cmd(c, 0, NULL, 0, 3000, &sense);
	memset(pd, 0, sizeof(pd));
	pd[8] = end >> 24; pd[9] = end >> 16; pd[10] = end >> 8; pd[11] = end;
	pd[14] = LOCK_KBS >> 8; pd[15] = LOCK_KBS & 0xff;
	pd[18] = 1000 >> 8; pd[19] = 1000 & 0xff;
	pd[22] = LOCK_KBS >> 8; pd[23] = LOCK_KBS & 0xff;
	pd[26] = 1000 >> 8; pd[27] = 1000 & 0xff;
	memset(c, 0, sizeof(c));
	c[0] = 0xB6;
	c[10] = sizeof(pd);
	r2 = cmd(c, 2, pd, sizeof(pd), 3000, &sense);
	L("velocidade travada: SET CD SPEED %d, SET STREAMING %d", r1, r2);
}

static int read_toc(void)
{
	static u8 b[4 + 100 * 8] __attribute__((aligned(64)));
	u8 c[12] = {0x43, 0, 0, 0, 0, 0, 1, 0, 4, 0, 0, 0};
	int r, len, i, sense, have_end = 0;

	memset(b, 0, 4);
	r = cmd(c, 1, b, 4, 5000, &sense);
	if (r != CDUSB_OK)
		return 0;
	len = (b[0] << 8 | b[1]) + 2;
	if (len < 12 || len > (int)sizeof(b))
		return 0;
	c[7] = len >> 8;
	c[8] = len;
	memset(b, 0, len);
	if (cmd(c, 1, b, len, 5000, &sense) != CDUSB_OK)
		return 0;
	memset(t_lba, 0, sizeof(t_lba));
	memset(t_ctrl, 0, sizeof(t_ctrl));
	t_first = b[2];
	t_last = b[3];
	for (i = 4; i + 8 <= len; i += 8) {
		int t = b[i + 2], idx = t == 0xAA ? 100 : t;
		if (idx < 1 || idx > 100)
			continue;
		t_lba[idx] = b[i + 4] << 24 | b[i + 5] << 16 | b[i + 6] << 8 | b[i + 7];
		t_ctrl[idx] = b[i + 1] & 0x0f;
		if (idx == 100)
			have_end = 1;
	}
	if (t_first < 1 || t_last < t_first || t_last > 99 || !have_end || t_lba[100] <= t_lba[t_last] ||
	    t_lba[100] > 450000)
		return 0;
	return 1;
}

/* ------------------------------------------------------------------ disco */
static int track_of(int lba)
{
	int t;
	for (t = t_last; t > t_first; t--)
		if (lba >= t_lba[t])
			return t;
	return t_first;
}

static int is_data(int t)
{
	return (t_ctrl[t] & 4) != 0;
}

static int track_end(int t)
{
	return t < t_last ? t_lba[t + 1] : t_lba[100];
}

/* 0 = fora do disco (zeros), 1 = musica (inclui a pausa antes dela), 2 = dados crus */
static int kind_of(int s)
{
	int t, e;
	if (s < 0 || s >= t_lba[100])
		return 0;
	t = track_of(s);
	if (!is_data(t))
		return 1;
	e = track_end(t);
	if (t < t_last && !is_data(t + 1) && e - 150 > t_lba[t])
		e -= 150;
	return s < e ? 2 : 1;
}

static int read_run(int lba, int n, int kind, u8 *dst, int *sense)
{
	u8 c[12] = {0xBE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	c[1] = kind == 1 ? (1 << 2) : 0;
	put_lba(c, lba);
	c[6] = n >> 16; c[7] = n >> 8; c[8] = n;
	c[9] = kind == 1 ? 0x10 : 0xF8;
	return cmd(c, 1, dst, n * SECT, TMO_READ, sense);
}

static void recover(int r, int lba)
{
	unsigned int t0 = now_ms();
	st.recov++;
	L("leitora sem resposta/reiniciou (resultado %d, setor %d): recuperando", r, lba);
	DelayThread(300 * 1000);
	if (wait_ready(20000))
		set_speed();
	L("recuperacao em %u ms", now_ms() - t0);
}

static void read_robust(int lba, int n, int kind, u8 *dst)
{
	int r, sense, k;

	for (k = 0; k < 2; k++) {
		r = read_run(lba, n, kind, dst, &sense);
		if (r == CDUSB_OK)
			return;
		L("leitura falhou: setor %d x%d tipo %d (resultado %d, sense %06X)", lba, n, kind, r, sense);
		if (k == 0 && (r != CDUSB_CHECK || ((sense >> 16) & 0x0f) == 6 || ((sense >> 16) & 0x0f) == 2))
			recover(r, lba);
	}
	st.errors++;
	memset(dst, 0, n * SECT);		/* setor ruim: zeros, nunca travar */
}

static void load_block(int i, int base)
{
	u8 *dst = cache + i * BLK_BYTES;
	unsigned int t0 = now_ms(), ms;
	int s = base, e = base + BLK;

	while (s < e) {
		int k = kind_of(s), n = 1;
		while (s + n < e && kind_of(s + n) == k)
			n++;
		if (k == 0)
			memset(dst, 0, n * SECT);
		else
			read_robust(s, n, k, dst);
		dst += n * SECT;
		s += n;
	}
	ms = now_ms() - t0;
	if (ms > st.worst)
		st.worst = ms;
	if (ms > SLOW_MS) {
		st.slow++;
		L("LENTO: bloco %d em %u ms", base, ms);
	}
}

/* ------------------------------------------------------------------ cache */
static int find_slot(int base)
{
	int i;
	for (i = 0; i < nblocks; i++)
		if (slots[i].base == base)
			return i;
	return -1;
}

/* para o Ember: vazio, senao o mais antigo que nao esteja em uso */
static int victim_any(void)
{
	int i, best = -1;
	for (i = 0; i < nblocks; i++) {
		if (slots[i].st == 1 || slots[i].pin)
			continue;
		if (slots[i].base < 0)
			return i;
		if (best < 0 || slots[i].used < slots[best].used)
			best = i;
	}
	return best;
}

/* para a leitura adiantada: so blocos fora da janela [w - BLK, w + ahead) */
static int victim_ahead(int w, int ahead)
{
	int i, best = -1;
	for (i = 0; i < nblocks; i++) {
		int b = slots[i].base;
		if (slots[i].st == 1 || slots[i].pin)
			continue;
		if (b < 0)
			return i;
		if (b >= w - BLK && b < w + ahead * BLK)
			continue;
		if (best < 0 || slots[i].used < slots[best].used)
			best = i;
	}
	return best;
}

/* bloco com o setor 'base' na memoria (fica preso ate unpin) */
static int get_block(int base)
{
	int i, waited = 0;

	for (;;) {
		WaitSema(lock);
		i = find_slot(base);
		if (i >= 0 && slots[i].st == 2) {
			slots[i].pin = 1;
			slots[i].used = ++tick;
			SignalSema(lock);
			if (waited)
				st.waits++;
			else
				st.hits++;
			break;
		}
		if (i >= 0) {				/* a leitura adiantada esta trazendo */
			SignalSema(lock);
			waited = 1;
			DelayThread(2000);
			continue;
		}
		i = victim_any();
		if (i < 0) {
			SignalSema(lock);
			DelayThread(2000);
			continue;
		}
		slots[i].base = base;
		slots[i].st = 1;
		slots[i].pin = 1;
		SignalSema(lock);
		st.misses++;
		load_block(i, base);
		WaitSema(lock);
		slots[i].st = 2;
		slots[i].used = ++tick;
		SignalSema(lock);
		break;
	}
	want = base + BLK;
	return i;
}

static void unpin(int i)
{
	slots[i].pin = 0;
}

static int bin_read(u32 pos, u8 *dst, int size)
{
	int done = 0;

	if (pos >= bin_size)
		return 0;
	if ((u32)size > bin_size - pos)
		size = bin_size - pos;
	st.reqs++;
	while (done < size) {
		u32 sect = pos / SECT, off = pos % SECT;
		int base = sect - sect % BLK, i = get_block(base);
		u32 inblk = (sect - base) * SECT + off, n = BLK_BYTES - inblk;
		if (n > (u32)(size - done))
			n = size - done;
		memcpy(dst + done, cache + i * BLK_BYTES + inblk, n);
		unpin(i);
		done += n;
		pos += n;
	}
	return done;
}

/* leitura adiantada + manter girando + log (thread propria) */
static void worker(void)
{
	unsigned int last_flush = now_ms(), last_stat = now_ms();
	int ahead = nblocks - 2;

	for (;;) {
		int w = want, k;

		DelayThread(5000);
		if (w >= 0) {
			for (k = 0; k < ahead; k++) {
				int b = w + k * BLK, i;
				if (b >= t_lba[100] || want != w)
					break;
				WaitSema(lock);
				if (find_slot(b) >= 0) {
					SignalSema(lock);
					continue;
				}
				i = victim_ahead(w, ahead);
				if (i < 0) {
					SignalSema(lock);
					break;
				}
				slots[i].base = b;
				slots[i].st = 1;
				SignalSema(lock);
				load_block(i, b);
				WaitSema(lock);
				slots[i].st = 2;
				slots[i].used = ++tick;
				SignalSema(lock);
				st.pref++;
			}
		}
		/* parado ha 2 s: le 1 setor perto de onde o jogo esta */
		if (w >= 0 && now_ms() - last_io > KEEP_MS) {
			int s = w < t_lba[100] ? w : t_lba[100] - 1, sense, r, kd = kind_of(s);
			unsigned int t0 = now_ms(), ms;
			r = read_run(s, 1, kd ? kd : 2, scratch, &sense);
			ms = now_ms() - t0;
			st.keeps++;
			if (ms > st.keep_worst)
				st.keep_worst = ms;
			if (r != CDUSB_OK || ms > SLOW_MS)
				L("manter girando: setor %d resultado %d em %u ms", s, r, ms);
		}
		if (now_ms() - last_stat > STAT_MS) {
			last_stat = now_ms();
			L("pedidos %u: na memoria %u, esperou a leitura adiantada %u, leu na hora %u; adiantados %u; "
			  "lentos %u (pior %u ms); erros %u; recuperacoes %u; manter girando %u (pior %u ms)",
			  st.reqs, st.hits, st.waits, st.misses, st.pref, st.slow, st.worst, st.errors, st.recov,
			  st.keeps, st.keep_worst);
		}
		if (now_ms() - last_flush > LOG_FLUSH_MS) {
			last_flush = now_ms();
			log_flush();
		}
	}
}

/* ------------------------------------------------------------------ .cue */
static void msf(char *o, int lba)
{
	sprintf(o, "%02d:%02d:%02d", lba / 4500, (lba / 75) % 60, lba % 75);
}

static void make_cue(void)
{
	char a[16];
	int t, n = 0;

	n += sprintf(cue + n, "FILE \"%s.bin\" BINARY\r\n", gname);
	for (t = t_first; t <= t_last && n < (int)sizeof(cue) - 128; t++) {
		n += sprintf(cue + n, "  TRACK %02d %s\r\n", t,
			     !is_data(t) ? "AUDIO" : data_mode == 1 ? "MODE1/2352" : "MODE2/2352");
		if (!is_data(t) && t > t_first && is_data(t - 1) && t_lba[t] >= 150) {
			msf(a, t_lba[t] - 150);
			n += sprintf(cue + n, "    INDEX 00 %s\r\n", a);
		}
		msf(a, t_lba[t]);
		n += sprintf(cue + n, "    INDEX 01 %s\r\n", a);
	}
	cue_len = n;
	bin_size = (u32)t_lba[100] * SECT;
}

/* ------------------------------------------------------------------ preparo */
static void init_thread(void *arg)
{
	int i, t;
	unsigned int t0 = now_ms();
	(void)arg;

	L("vcd 1.1: pasta %s%s, jogo %s, %d blocos de cache", root, gdir, gname, nblocks);
	L("memoria livre no IOP: %u KB", (unsigned int)QueryMaxFreeMemSize() / 1024);
	while (nblocks >= 3 && !(cache = AllocSysMemory(ALLOC_FIRST, nblocks * BLK_BYTES, NULL)))
		nblocks--;
	if (!cache) {
		L("sem memoria para o cache");
		ready = -1;
		log_flush();
		ExitDeleteThread();
	}
	for (i = 0; i < MAX_BLOCKS; i++)
		slots[i].base = -1;
	L("cache: %d blocos (%d KB); memoria livre depois: %u KB", nblocks, nblocks * BLK_BYTES / 1024,
	  (unsigned int)QueryMaxFreeMemSize() / 1024);
	if (!wait_ready(30000) || !read_toc()) {
		L("disco nao ficou pronto ou TOC invalida");
		ready = -1;
		log_flush();
		ExitDeleteThread();
	}
	for (t = t_first; t <= t_last; t++)
		L("faixa %d: setor %d %s", t, t_lba[t], is_data(t) ? "DADOS" : "MUSICA");
	L("fim do disco: setor %d", t_lba[100]);
	set_speed();
	if (is_data(t_first)) {
		int sense;
		if (read_run(t_lba[t_first] + 16, 1, 2, scratch, &sense) == CDUSB_OK &&
		    (scratch[15] == 1 || scratch[15] == 2))
			data_mode = scratch[15];
		L("faixa de dados: modo %d", data_mode);
	}
	make_cue();
	L("cue (%d bytes), bin %u bytes; pronto em %u ms", cue_len, bin_size, now_ms() - t0);
	ready = 1;
	log_flush();
	worker();
}

/* ------------------------------------------------------------------ caminhos */
static int plock = -1;			/* np_/fp_ (a pilha de quem chama e pequena) */
static char np_[256], fp_[272], np2_[256], fp2_[272];

static int lower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int same(const char *a, const char *b)
{
	while (*a && *b && lower(*a) == lower(*b))
		a++, b++;
	return *a == 0 && *b == 0;
}

/* "/Ember//games\X/" -> "/Ember/games/X" */
static void norm(char *o, const char *in, int max)
{
	int n = 0;
	o[n++] = '/';
	for (; *in && n < max - 1; in++) {
		char c = *in == '\\' ? '/' : *in;
		if (c == '/' && o[n - 1] == '/')
			continue;
		o[n++] = c;
	}
	if (n > 1 && o[n - 1] == '/')
		n--;
	o[n] = 0;
}

static void full(char *o, const char *np)
{
	strcpy(o, root);
	strcpy(o + strlen(o), np);
}

/* 1 = .cue, 2 = .bin, 0 = arquivo de verdade */
static int virt(const char *np)
{
	if (same(np, cue_path))
		return 1;
	if (same(np, bin_path))
		return 2;
	return 0;
}

static int wait_init(void)
{
	int n = 0;
	while (ready == 0 && n++ < 3000)
		DelayThread(10000);
	return ready == 1;
}

/* ------------------------------------------------------------------ arquivos */
typedef struct {
	int used, kind, fd, game_dir, vstage;
	u32 pos;
} vfile;
static vfile files[32];

static vfile *new_file(void)
{
	int i;
	for (i = 0; i < 32; i++)
		if (!files[i].used) {
			memset(&files[i], 0, sizeof(files[i]));
			files[i].used = 1;
			files[i].fd = -1;
			return &files[i];
		}
	return NULL;
}

static int v_close(iomanX_iop_file_t *f);

static int v_init(iomanX_iop_device_t *d) { (void)d; return 0; }
static int v_deinit(iomanX_iop_device_t *d) { (void)d; return 0; }

static int v_open(iomanX_iop_file_t *f, const char *name, int flags, int mode)
{
	vfile *v;
	int k, r = 0;

	WaitSema(plock);
	norm(np_, name, sizeof(np_));
	k = virt(np_);
	if (k && (flags & FIO_O_WRONLY))
		r = -13;
	else if (!(v = new_file()))
		r = -24;
	else {
		v->kind = k;
		if (!k) {
			full(fp_, np_);
			v->fd = iomanX_open(fp_, flags, mode);
			if (v->fd < 0) {
				r = v->fd;
				v->used = 0;
			}
		} else
			L("o Ember abriu %s", np_);
		if (!r)
			f->privdata = v;
	}
	SignalSema(plock);
	if (!r && k && !wait_init()) {
		v_close(f);
		r = -2;
	}
	return r;
}

static int v_close(iomanX_iop_file_t *f)
{
	vfile *v = f->privdata;
	if (!v)
		return -9;
	if (v->fd >= 0)
		iomanX_close(v->fd);
	v->used = 0;
	return 0;
}

static int v_read(iomanX_iop_file_t *f, void *ptr, int size)
{
	vfile *v = f->privdata;
	int n;

	if (!v)
		return -9;
	if (v->kind == 0)
		return iomanX_read(v->fd, ptr, size);
	if (size <= 0)
		return 0;
	if (v->kind == 1) {
		if (v->pos >= (u32)cue_len)
			return 0;
		n = cue_len - v->pos;
		if (n > size)
			n = size;
		memcpy(ptr, cue + v->pos, n);
	} else
		n = bin_read(v->pos, ptr, size);
	v->pos += n;
	return n;
}

static int v_write(iomanX_iop_file_t *f, void *ptr, int size)
{
	vfile *v = f->privdata;
	if (!v)
		return -9;
	if (v->kind)
		return -13;
	return iomanX_write(v->fd, ptr, size);
}

static u32 vsize(vfile *v)
{
	return v->kind == 1 ? (u32)cue_len : bin_size;
}

static int v_lseek(iomanX_iop_file_t *f, int off, int whence)
{
	vfile *v = f->privdata;
	s64 p;

	if (!v)
		return -9;
	if (v->kind == 0)
		return iomanX_lseek(v->fd, off, whence);
	p = whence == FIO_SEEK_SET ? off : whence == FIO_SEEK_CUR ? (s64)v->pos + off : (s64)vsize(v) + off;
	if (p < 0)
		return -22;
	v->pos = (u32)p;
	return (int)v->pos;
}

static s64 v_lseek64(iomanX_iop_file_t *f, s64 off, int whence)
{
	vfile *v = f->privdata;
	s64 p;

	if (!v)
		return -9;
	if (v->kind == 0)
		return iomanX_lseek64(v->fd, off, whence);
	p = whence == FIO_SEEK_SET ? off : whence == FIO_SEEK_CUR ? (s64)v->pos + off : (s64)vsize(v) + off;
	if (p < 0 || p > 0xFFFFFFFFll)
		return -22;
	v->pos = (u32)p;
	return p;
}

static int v_ioctl(iomanX_iop_file_t *f, int cmdn, void *param)
{
	vfile *v = f->privdata;
	if (!v || v->kind)
		return -22;
	return iomanX_ioctl(v->fd, cmdn, param);
}

static void fill_stat(iox_stat_t *s, u32 size)
{
	memset(s, 0, sizeof(*s));
	s->mode = FIO_S_IFREG | FIO_S_IRUSR | FIO_S_IRGRP | FIO_S_IROTH;
	s->attr = 0;
	s->size = size;
}

static int v_getstat(iomanX_iop_file_t *f, const char *name, iox_stat_t *s)
{
	int k, r;
	(void)f;

	WaitSema(plock);
	norm(np_, name, sizeof(np_));
	k = virt(np_);
	if (!k) {
		full(fp_, np_);
		r = iomanX_getstat(fp_, s);
		SignalSema(plock);
		return r;
	}
	SignalSema(plock);
	if (!wait_init())
		return -2;
	fill_stat(s, k == 1 ? (u32)cue_len : bin_size);
	return 0;
}

static int v_chstat(iomanX_iop_file_t *f, const char *name, iox_stat_t *s, unsigned int mask)
{
	int r = -13;
	(void)f;
	WaitSema(plock);
	norm(np_, name, sizeof(np_));
	if (!virt(np_)) {
		full(fp_, np_);
		r = iomanX_chstat(fp_, s, mask);
	}
	SignalSema(plock);
	return r;
}

static int v_remove(iomanX_iop_file_t *f, const char *name)
{
	int r = -13;
	(void)f;
	WaitSema(plock);
	norm(np_, name, sizeof(np_));
	if (!virt(np_)) {
		full(fp_, np_);
		r = iomanX_remove(fp_);
	}
	SignalSema(plock);
	return r;
}

static int v_mkdir(iomanX_iop_file_t *f, const char *name, int mode)
{
	int r;
	(void)f;
	WaitSema(plock);
	norm(np_, name, sizeof(np_));
	full(fp_, np_);
	r = iomanX_mkdir(fp_, mode);
	SignalSema(plock);
	return r;
}

static int v_rmdir(iomanX_iop_file_t *f, const char *name)
{
	int r;
	(void)f;
	WaitSema(plock);
	norm(np_, name, sizeof(np_));
	full(fp_, np_);
	r = iomanX_rmdir(fp_);
	SignalSema(plock);
	return r;
}

static int v_rename(iomanX_iop_file_t *f, const char *a, const char *b)
{
	int r = -13;
	(void)f;
	WaitSema(plock);
	norm(np_, a, sizeof(np_));
	norm(np2_, b, sizeof(np2_));
	if (!virt(np_) && !virt(np2_)) {
		full(fp_, np_);
		full(fp2_, np2_);
		r = iomanX_rename(fp_, fp2_);
	}
	SignalSema(plock);
	return r;
}

static int v_dopen(iomanX_iop_file_t *f, const char *name)
{
	vfile *v;
	int r = 0;

	WaitSema(plock);
	v = new_file();
	if (!v)
		r = -24;
	else {
		norm(np_, name, sizeof(np_));
		full(fp_, np_);
		v->kind = 0;
		v->game_dir = same(np_, gdir);
		v->fd = iomanX_dopen(fp_);
		if (v->fd < 0) {
			r = v->fd;
			v->used = 0;
		} else
			f->privdata = v;
	}
	SignalSema(plock);
	return r;
}

static int v_dclose(iomanX_iop_file_t *f)
{
	vfile *v = f->privdata;
	if (!v)
		return -9;
	if (v->fd >= 0)
		iomanX_dclose(v->fd);
	v->used = 0;
	return 0;
}

static int v_dread(iomanX_iop_file_t *f, iox_dirent_t *d)
{
	vfile *v = f->privdata;
	char cn[80], bn[80];
	int r;

	if (!v)
		return -9;
	sprintf(cn, "%s.cue", gname);
	sprintf(bn, "%s.bin", gname);
	for (;;) {
		r = iomanX_dread(v->fd, d);
		if (r <= 0)
			break;
		if (v->game_dir && (same(d->name, cn) || same(d->name, bn)))
			continue;		/* o virtual manda */
		return r;
	}
	if (v->game_dir && v->vstage < 2 && wait_init()) {
		memset(d, 0, sizeof(*d));
		strcpy(d->name, v->vstage == 0 ? cn : bn);
		fill_stat(&d->stat, v->vstage == 0 ? (u32)cue_len : bin_size);
		v->vstage++;
		return strlen(d->name);
	}
	return r < 0 ? r : 0;
}

static int v_devctl(iomanX_iop_file_t *f, const char *name, int c, void *arg, unsigned int al, void *buf,
		    unsigned int bl)
{
	int r;
	(void)f;
	WaitSema(plock);
	norm(np_, name, sizeof(np_));
	full(fp_, np_);
	r = iomanX_devctl(fp_, c, arg, al, buf, bl);
	SignalSema(plock);
	return r;
}

IOMANX_RETURN_VALUE_IMPL(48);

static iomanX_iop_device_ops_t ops = {
	v_init,
	v_deinit,
	IOMANX_RETURN_VALUE(48),	/* format */
	v_open,
	v_close,
	v_read,
	v_write,
	v_lseek,
	v_ioctl,
	v_remove,
	v_mkdir,
	v_rmdir,
	v_dopen,
	v_dclose,
	v_dread,
	v_getstat,
	v_chstat,
	v_rename,
	IOMANX_RETURN_VALUE(48),	/* chdir */
	IOMANX_RETURN_VALUE(48),	/* sync */
	IOMANX_RETURN_VALUE(48),	/* mount */
	IOMANX_RETURN_VALUE(48),	/* umount */
	v_lseek64,
	v_devctl,
	IOMANX_RETURN_VALUE(48),	/* symlink */
	IOMANX_RETURN_VALUE(48),	/* readlink */
	IOMANX_RETURN_VALUE(48),	/* ioctl2 */
};

static iomanX_iop_device_t vdev = {
	"vcd", IOP_DT_FS | IOP_DT_FSEXT, 1, "CD da leitora USB como .cue/.bin", &ops,
};

/* ------------------------------------------------------------------ inicio */
static int new_sema(int initial)
{
	iop_sema_t s;
	s.attr = 0;
	s.option = 0;
	s.initial = initial;
	s.max = 1;
	return CreateSema(&s);
}

static void copy_arg(char *dst, const char *src, int max)
{
	strncpy(dst, src, max - 1);
	dst[max - 1] = 0;
}

int _start(int argc, char *argv[])
{
	iop_thread_t t;
	char tmp[180];
	int i, id;

	for (i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "root=", 5))
			copy_arg(root, argv[i] + 5, sizeof(root));
		else if (!strncmp(argv[i], "dir=", 4)) {
			copy_arg(tmp, argv[i] + 4, sizeof(tmp));
			norm(gdir, tmp, sizeof(gdir));
		} else if (!strncmp(argv[i], "name=", 5))
			copy_arg(gname, argv[i] + 5, sizeof(gname));
		else if (!strncmp(argv[i], "blocks=", 7)) {
			nblocks = strtol(argv[i] + 7, NULL, 10);
			if (nblocks < 3)
				nblocks = 3;
			if (nblocks > MAX_BLOCKS)
				nblocks = MAX_BLOCKS;
		}
	}
	sprintf(cue_path, "%s/%s.cue", gdir, gname);
	sprintf(bin_path, "%s/%s.bin", gdir, gname);
	sprintf(log_path, "%s%s/VCD.TXT", root, gdir);

	lock = new_sema(1);
	log_lock = new_sema(1);
	plock = new_sema(1);
	if (lock < 0 || log_lock < 0 || plock < 0)
		return MODULE_NO_RESIDENT_END;
	iomanX_DelDrv(vdev.name);
	if (iomanX_AddDrv(&vdev) != 0)
		return MODULE_NO_RESIDENT_END;

	t.attr = TH_C;
	t.option = 0;
	t.thread = init_thread;
	t.stacksize = 0x2000;
	t.priority = 0x28;
	id = CreateThread(&t);
	if (id <= 0)
		return MODULE_NO_RESIDENT_END;
	StartThread(id, NULL);
	return MODULE_RESIDENT_END;
}
