/*
 * rv-jogo - RETROVOLT Pi: roda um nucleo libretro direto no framebuffer.
 *
 * - video: /dev/fb0 (1920x240 no tubo, ou LCD), espera o vsync e copia o quadro
 *   inteiro de uma vez; no tubo cada linha do jogo vai para uma linha da TV e a
 *   largura e ampliada para a largura da convergencia.
 * - som: ALSA (libasound carregada na hora), com ajuste fino de velocidade para
 *   nao faltar nem sobrar som.
 * - controles: evdev direto (8BitDo/Xbox e teclado), START+SELECT abre o menu.
 * - CD: a leitora vira um "disco virtual" /run/retrovolt/cd/disco.cue + disco.bin,
 *   lido setor a setor (READ CD, 2352 bytes) pela interface VFS do libretro.
 *   Nada de comando de velocidade na leitora.
 * - saves: memory card/bateria (SAVE_RAM) gravado a cada 5 s se mudou e ao sair.
 *
 * uso: rv-jogo --nucleo X.so (--jogo ARQ | --cd /dev/sr0) --bios DIR --saves DIR
 *              --nome NOME [--conv x,y,larg,alt] [--fonte 6X10] [--audio DISP]
 *              [--opcoes ARQ]
 * saida: 0 = saiu pelo menu; 2 = erro ao abrir; 3 = nucleo ruim
 * teste no PC: RV_FAKEFB=saida.ppm:1920x240 RV_FRAMES=600 (sem vsync, grava o ultimo quadro)
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <drm/drm.h>
#include <linux/cdrom.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <pthread.h>
#include <scsi/sg.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "libretro.h"
#include "fonte.h"

#define LOG(...) do { printf("rv-jogo: " __VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)

static volatile sig_atomic_t g_sair;
static double agora(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------ opcoes */
static const char *a_nucleo, *a_jogo, *a_cd, *a_bios = ".", *a_saves = ".", *a_nome = "jogo";
static const char *a_audio, *a_opcoes, *a_fonte = "6X10", *a_save, *a_states, *a_conv_arq;
static int filtro; /* LCD: 0 nenhum, 1 scanlines, 2 scanlines leves, 3 CRT */
static const char *FILTROS[] = {"NENHUM", "SCANLINES", "SCANLINES LEVES", "CRT"};
#define N_FILTROS 4
static const char *a_filtro_arq;
static bool sem_filtro; /* menu do jogo: nunca com filtro */
static int imagem;       /* LCD: 0 = ORIGINAL (ampliacao inteira, nitida), 1 = TELA CHEIA (enche a altura) */
static const char *a_imagem_arq;
static int conv_menu_x, conv_menu_y; static double conv_menu_w = 1.0; /* convergencia geral (do menu) */
static int volume = 100, volume_ini = -1;  /* 0..100 */
static int cd_vel = 4;                     /* velocidade da leitora (x); 0 = maxima */
static int32_t ganho = 32768;              /* Q15 */
static void volume_aplica(void) { double g = volume / 100.0; ganho = (int32_t)lround(g * g * 32768); }
static int conv_x, conv_y;
static double conv_w = 1.0, conv_h = 1.0;

/* ------------------------------------------------------------------ nucleo */
static struct {
	void *h;
	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample)(retro_audio_sample_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);
	void (*init)(void);
	void (*deinit)(void);
	void (*get_system_info)(struct retro_system_info *);
	void (*get_system_av_info)(struct retro_system_av_info *);
	void (*set_controller_port_device)(unsigned, unsigned);
	bool (*load_game)(const struct retro_game_info *);
	void (*unload_game)(void);
	void (*run)(void);
	void (*reset)(void);
	void *(*get_memory_data)(unsigned);
	size_t (*get_memory_size)(unsigned);
	size_t (*serialize_size)(void);
	bool (*serialize)(void *, size_t);
	bool (*unserialize)(const void *, size_t);
} core;

static struct retro_system_av_info av;
static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;

/* variaveis do nucleo (opcoes): chave, valor */
#define MAXVAR 256
static struct { char *k, *v, *desc, *vals[64]; int nvals; } vars[MAXVAR];
static bool vars_mudou;   /* opcao mudada no menu: o nucleo le no proximo quadro */
static int nvars;

static void var_set(const char *k, const char *v, bool so_se_novo)
{
	for (int i = 0; i < nvars; i++)
		if (!strcmp(vars[i].k, k)) {
			if (!so_se_novo) {
				free(vars[i].v);
				vars[i].v = strdup(v);
			}
			return;
		}
	if (nvars < MAXVAR) {
		vars[nvars].k = strdup(k);
		vars[nvars].v = strdup(v);
		nvars++;
	}
}

static void opcoes_ler(void)
{
	if (!a_opcoes)
		return;
	FILE *f = fopen(a_opcoes, "r");
	if (!f)
		return;
	char l[512];
	while (fgets(l, sizeof l, f)) {
		char *c = strchr(l, '#');
		if (c)
			*c = 0;
		char *eq = strchr(l, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *k = l, *v = eq + 1;
		while (*k == ' ' || *k == '\t') k++;
		for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
		while (*v == ' ' || *v == '\t' || *v == '"') v++;
		for (char *e = v + strlen(v); e > v && strchr(" \t\r\n\"", e[-1]); ) *--e = 0;
		if (*k)
			var_set(k, v, false);
	}
	fclose(f);
}

/* ------------------------------------------------------------------ CD virtual */
#define CD_DIR "/run/retrovolt/cd"
#define CD_CUE CD_DIR "/disco.cue"
#define CD_BIN CD_DIR "/disco.bin"
#define SETOR 2352
#define BLOCO 16 /* setores por leitura */
#define NCACHE 48
#define PF_BLOCOS 6 /* blocos lidos adiante, em segundo plano */

static int cd_fd = -1;
static bool cd_arquivo; /* teste: --cd aponta para um .bin */
static char cd_cue[4096];
static int64_t cd_tam;
/* cd_mx protege o cache; cd_io_mx protege a leitora (so um comando por vez).
 * A leitura na leitora acontece sem cd_mx, para o jogo achar no cache o que ja
 * foi lido adiante enquanto outro bloco esta chegando. */
static pthread_mutex_t cd_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cd_io_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cd_cv = PTHREAD_COND_INITIALIZER;    /* um bloco ficou pronto */
static pthread_cond_t cd_pf_cv = PTHREAD_COND_INITIALIZER; /* pedido de leitura adiante */
static struct { int64_t lba; uint64_t uso; bool lendo, adiantado; uint8_t d[BLOCO * SETOR]; } cd_cache[NCACHE];
static uint64_t cd_relogio;
static volatile double cd_ult_leitura;
static volatile int64_t cd_ult_lba;
static int64_t cd_pf_base = -1; /* primeiro bloco a ler adiante */
static uint64_t cd_pf_ger;      /* muda a cada pedido novo */
static long cd_n_adiante, cd_n_acertos_pf;

/* faixas do disco (para saber se um setor e de audio) */
static int cd_nfx;
static struct { int lba; bool dados; int modo; } cd_fx[100];
/* metodo de leitura: 0 = READ CD (SG_IO), 1 = CDROMREADRAW, 2 = setor de 2048 + cabecalho montado */
static int cd_metodo;
static long cd_n_blocos;
static double cd_t_total, cd_t_max;
static int cd_n_erros;

static int cd_faixa(int64_t lba)
{
	int k = 0;
	for (int i = 0; i < cd_nfx; i++)
		if (cd_fx[i].lba <= lba)
			k = i;
	return k;
}

static bool cd_read_cd(uint32_t lba, int n, uint8_t *buf, uint8_t flags)
{
	uint8_t cmd[12] = {0xBE, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, 0, n, flags, 0, 0};
	uint8_t sense[32] = {0};
	sg_io_hdr_t io;
	memset(&io, 0, sizeof io);
	io.interface_id = 'S';
	io.dxfer_direction = SG_DXFER_FROM_DEV;
	io.cmd_len = sizeof cmd;
	io.cmdp = cmd;
	io.mx_sb_len = sizeof sense;
	io.sbp = sense;
	io.dxfer_len = n * SETOR;
	io.dxferp = buf;
	io.timeout = 5000;
	int r = ioctl(cd_fd, SG_IO, &io);
	bool ok = r == 0 && io.status == 0 && io.host_status == 0 && (io.driver_status & 0x7) == 0;
	if (!ok && cd_n_erros++ < 8)
		LOG("cd: READ CD lba %u x%d flags %02x falhou: ioctl %d (%s) status %d host %d driver %d sense %x/%02x/%02x",
		    lba, n, flags, r, r ? strerror(errno) : "-", io.status, io.host_status, io.driver_status,
		    sense[2] & 15, sense[12], sense[13]);
	return ok;
}

static void bcd_msf(int64_t lba, uint8_t *o)
{
	int a = lba + 150, m = a / 4500, sg = a / 75 % 60, f = a % 75;
	o[0] = (m / 10) << 4 | m % 10;
	o[1] = (sg / 10) << 4 | sg % 10;
	o[2] = (f / 10) << 4 | f % 10;
}

static bool cd_raw_ioctl(int64_t lba, uint8_t *b)
{
	int a = lba + 150;
	b[0] = a / 4500; b[1] = a / 75 % 60; b[2] = a % 75;
	return ioctl(cd_fd, CDROMREADRAW, b) == 0;
}

static bool cd_audio_ioctl(int64_t lba, uint8_t *b)
{
	struct cdrom_read_audio ra;
	memset(&ra, 0, sizeof ra);
	ra.addr.lba = lba;
	ra.addr_format = CDROM_LBA;
	ra.nframes = 1;
	ra.buf = b;
	return ioctl(cd_fd, CDROMREADAUDIO, &ra) == 0;
}

static bool cd_cozido(int64_t lba, uint8_t *b)
{
	int modo = cd_fx[cd_faixa(lba)].modo;
	int off = modo == 1 ? 16 : 24;
	memset(b, 0, SETOR);
	if (pread(cd_fd, b + off, 2048, lba * 2048) != 2048)
		return false;
	memset(b + 1, 0xff, 10);
	bcd_msf(lba, b + 12);
	b[15] = modo == 1 ? 1 : 2;
	if (modo != 1) {
		b[18] = b[22] = 0x08; /* subcabecalho: dados, forma 1 */
	}
	return true;
}

static bool cd_le_setor(int64_t lba, uint8_t *b)
{
	bool audio = !cd_fx[cd_faixa(lba)].dados;
	if (audio)
		return (cd_metodo == 0 && cd_read_cd(lba, 1, b, 0x10)) || cd_audio_ioctl(lba, b) || cd_read_cd(lba, 1, b, 0x10);
	if (cd_metodo == 0 && cd_read_cd(lba, 1, b, 0xF8))
		return true;
	if (cd_metodo <= 1 && cd_raw_ioctl(lba, b)) {
		if (cd_metodo != 1) {
			cd_metodo = 1;
			LOG("cd: passando a ler pelo CDROMREADRAW");
		}
		return true;
	}
	if (cd_cozido(lba, b)) {
		if (cd_metodo != 2) {
			cd_metodo = 2;
			LOG("cd: passando a ler setores de 2048 (sem audio XA)");
		}
		return true;
	}
	return false;
}

static void cd_le_bloco(int64_t lba0, uint8_t *d)
{
	int64_t total = cd_tam / SETOR;
	int n = BLOCO;
	if (lba0 + n > total)
		n = total - lba0;
	memset(d, 0, BLOCO * SETOR);
	if (n <= 0)
		return;
	if (cd_arquivo) {
		if (pread(cd_fd, d, n * SETOR, lba0 * SETOR) < 0)
			LOG("cd: erro lendo %lld", (long long)lba0);
		return;
	}
	double t0 = agora();
	bool mesma = cd_metodo == 0 && cd_faixa(lba0) == cd_faixa(lba0 + n - 1);
	bool ok = mesma && cd_read_cd(lba0, n, d, cd_fx[cd_faixa(lba0)].dados ? 0xF8 : 0x10);
	if (!ok)
		for (int i = 0; i < n; i++)
			if (!cd_le_setor(lba0 + i, d + i * SETOR)) {
				memset(d + i * SETOR, 0, SETOR);
				if (cd_n_erros++ < 30)
					LOG("cd: setor %lld ilegivel", (long long)(lba0 + i));
			}
	double t = agora() - t0;
	cd_n_blocos++;
	cd_t_total += t;
	cd_ult_leitura = agora();
	cd_ult_lba = lba0;
	if (t > cd_t_max)
		cd_t_max = t;
}

/* procura o bloco no cache (com cd_mx) */
static int cd_acha(int64_t b0)
{
	for (int k = 0; k < NCACHE; k++)
		if (cd_cache[k].lba == b0)
			return k;
	return -1;
}

/* escolhe a vaga mais antiga que nao esta sendo lida (com cd_mx) */
static int cd_vaga(void)
{
	int v = -1;
	for (int k = 0; k < NCACHE; k++)
		if (!cd_cache[k].lendo && (v < 0 || cd_cache[k].uso < cd_cache[v].uso))
			v = k;
	return v;
}

/* le o bloco b0 da leitora para a vaga k (entra e sai com cd_mx) */
static void cd_carrega(int k, int64_t b0)
{
	cd_cache[k].lba = b0;
	cd_cache[k].lendo = true;
	pthread_mutex_unlock(&cd_mx);
	pthread_mutex_lock(&cd_io_mx);
	cd_le_bloco(b0, cd_cache[k].d);
	pthread_mutex_unlock(&cd_io_mx);
	pthread_mutex_lock(&cd_mx);
	cd_cache[k].lendo = false;
	pthread_cond_broadcast(&cd_cv);
}

static int64_t cd_ler(int64_t pos, uint8_t *s, int64_t len)
{
	if (pos >= cd_tam)
		return 0;
	if (pos + len > cd_tam)
		len = cd_tam - pos;
	int64_t feito = 0, ult = -1;
	pthread_mutex_lock(&cd_mx);
	while (feito < len) {
		int64_t lba = (pos + feito) / SETOR;
		int64_t b0 = lba - lba % BLOCO;
		int k = cd_acha(b0);
		if (k >= 0 && cd_cache[k].lendo) {
			pthread_cond_wait(&cd_cv, &cd_mx); /* a leitura adiante ja esta trazendo */
			continue;
		}
		if (k < 0) {
			k = cd_vaga();
			if (k < 0) {
				pthread_cond_wait(&cd_cv, &cd_mx);
				continue;
			}
			cd_carrega(k, b0);
			continue; /* confere de novo: a vaga pode ter mudado enquanto lia */
		}
		if (cd_cache[k].adiantado) {
			cd_cache[k].adiantado = false;
			cd_n_acertos_pf++;
		}
		cd_cache[k].uso = ++cd_relogio;
		int64_t off = pos + feito - b0 * SETOR;
		int64_t n = BLOCO * SETOR - off;
		if (n > len - feito)
			n = len - feito;
		memcpy(s + feito, cd_cache[k].d + off, n);
		feito += n;
		ult = b0;
	}
	if (ult >= 0 && cd_pf_base != ult + BLOCO) {
		cd_pf_base = ult + BLOCO;
		cd_pf_ger++;
		pthread_cond_signal(&cd_pf_cv);
	}
	pthread_mutex_unlock(&cd_mx);
	return feito;
}

/* leitura adiante: enquanto o jogo usa um bloco, os proximos ja vem para o cache */
static void *cd_adiante_thread(void *x)
{
	(void)x;
	pthread_mutex_lock(&cd_mx);
	uint64_t ger = cd_pf_ger;
	while (!g_sair) {
		while (ger == cd_pf_ger && !g_sair)
			pthread_cond_wait(&cd_pf_cv, &cd_mx);
		ger = cd_pf_ger;
		for (int i = 0; i < PF_BLOCOS && ger == cd_pf_ger && !g_sair; i++) {
			int64_t b0 = cd_pf_base + i * BLOCO;
			if (b0 < 0 || b0 * SETOR >= cd_tam)
				break;
			if (cd_acha(b0) >= 0)
				continue;
			int k = cd_vaga();
			if (k < 0)
				break;
			cd_cache[k].uso = ++cd_relogio;
			cd_cache[k].adiantado = true;
			cd_carrega(k, b0);
			cd_n_adiante++;
		}
	}
	pthread_mutex_unlock(&cd_mx);
	return NULL;
}

static void msf(int lba, char *o)
{
	sprintf(o, "%02d:%02d:%02d", lba / 4500, lba / 75 % 60, lba % 75);
}

static bool cd_abrir(const char *dev)
{
	for (int k = 0; k < NCACHE; k++)
		cd_cache[k].lba = -1;
	cd_pf_base = -1;
	struct stat st;
	if (stat(dev, &st) == 0 && S_ISREG(st.st_mode)) {
		cd_arquivo = true;
		cd_fd = open(dev, O_RDONLY);
		cd_tam = st.st_size / SETOR * SETOR;
		cd_nfx = 1;
		cd_fx[0].dados = true;
		cd_fx[0].modo = 2;
		snprintf(cd_cue, sizeof cd_cue, "FILE \"disco.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
	} else {
		cd_fd = open(dev, O_RDONLY | O_NONBLOCK);
		if (cd_fd < 0) {
			LOG("cd: nao abriu %s: %s", dev, strerror(errno));
			return false;
		}
		/* PS1 precisa de 2x; a leitora no maximo faz barulho a toa. A leitura
		 * adiantada (128 setores) cobre as esperas. Leitora que nao aceita: ignora. */
		if (ioctl(cd_fd, CDROM_SELECT_SPEED, cd_vel) < 0)
			LOG("cd: leitora nao aceitou velocidade %dx (%s): segue no padrao", cd_vel, strerror(errno));
		else
			LOG("cd: velocidade da leitora %dx", cd_vel);
		struct cdrom_tochdr th;
		if (ioctl(cd_fd, CDROMREADTOCHDR, &th) < 0) {
			LOG("cd: sem TOC (sem disco?): %s", strerror(errno));
			return false;
		}
		struct cdrom_tocentry te;
		memset(&te, 0, sizeof te);
		te.cdte_track = CDROM_LEADOUT;
		te.cdte_format = CDROM_LBA;
		ioctl(cd_fd, CDROMREADTOCENTRY, &te);
		int fim = te.cdte_addr.lba;
		cd_tam = (int64_t)fim * SETOR;
		int n = snprintf(cd_cue, sizeof cd_cue, "FILE \"disco.bin\" BINARY\n");
		int ant = -1;
		for (int t = th.cdth_trk0; t <= th.cdth_trk1 && n < (int)sizeof cd_cue - 200; t++) {
			memset(&te, 0, sizeof te);
			te.cdte_track = t;
			te.cdte_format = CDROM_LBA;
			if (ioctl(cd_fd, CDROMREADTOCENTRY, &te) < 0)
				continue;
			int lba = te.cdte_addr.lba;
			bool dados = te.cdte_ctrl & CDROM_DATA_TRACK;
			const char *tipo = "AUDIO";
			int modo = 2;
			if (dados) {
				uint8_t s[SETOR];
				if (cd_read_cd(lba, 1, s, 0xF8) || cd_raw_ioctl(lba, s))
					modo = s[15] == 1 ? 1 : 2;
				tipo = modo == 1 ? "MODE1/2352" : "MODE2/2352";
			}
			if (cd_nfx < 100) {
				cd_fx[cd_nfx].lba = lba;
				cd_fx[cd_nfx].dados = dados;
				cd_fx[cd_nfx].modo = modo;
				cd_nfx++;
			}
			char m[16];
			n += snprintf(cd_cue + n, sizeof cd_cue - n, "  TRACK %02d %s\n", t, tipo);
			if (!dados && ant >= 0 && lba - 150 > ant) {
				msf(lba - 150, m);
				n += snprintf(cd_cue + n, sizeof cd_cue - n, "    INDEX 00 %s\n", m);
			}
			msf(lba, m);
			n += snprintf(cd_cue + n, sizeof cd_cue - n, "    INDEX 01 %s\n", m);
			ant = lba;
		}
	}
	mkdir("/run/retrovolt", 0755);
	mkdir(CD_DIR, 0755);
	FILE *f = fopen(CD_CUE, "w");
	if (f) {
		fputs(cd_cue, f);
		fclose(f);
	}
	LOG("cd: %lld setores; cue:\n%s", (long long)(cd_tam / SETOR), cd_cue);
	if (!cd_arquivo) {
		uint8_t b[SETOR];
		int64_t l = cd_fx[0].dados ? cd_fx[0].lba + 16 : 16;
		for (int i = 0; i < cd_nfx; i++)
			if (cd_fx[i].dados) {
				l = cd_fx[i].lba + 16;
				break;
			}
		double t0 = agora();
		bool m0 = cd_read_cd(l, 1, b, 0xF8);
		double t1 = agora();
		bool m1 = cd_raw_ioctl(l, b);
		double t2 = agora();
		bool m2 = cd_cozido(l, b);
		LOG("cd: teste setor %lld: READ CD %s (%.0f ms), READRAW %s (%.0f ms), 2048 %s",
		    (long long)l, m0 ? "ok" : "FALHOU", (t1 - t0) * 1e3, m1 ? "ok" : "FALHOU", (t2 - t1) * 1e3, m2 ? "ok" : "FALHOU");
		cd_metodo = m0 ? 0 : m1 ? 1 : 2;
	}
	return cd_tam > 0;
}

/* ------------------------------------------------------------------ VFS */
enum { T_ARQ, T_CUE, T_BIN };
struct retro_vfs_file_handle {
	int tipo;
	FILE *fp;
	char *path;
	int64_t pos;
};
struct retro_vfs_dir_handle {
	DIR *d;
	struct dirent *e;
	char *path;
};

static int vfs_tipo(const char *p)
{
	if (cd_fd >= 0 && !strcmp(p, CD_CUE))
		return T_CUE;
	if (cd_fd >= 0 && !strcmp(p, CD_BIN))
		return T_BIN;
	return T_ARQ;
}
static const char *v_get_path(struct retro_vfs_file_handle *s) { return s->path; }
static struct retro_vfs_file_handle *v_open(const char *path, unsigned mode, unsigned hints)
{
	(void)hints;
	int t = vfs_tipo(path);
	FILE *fp = NULL;
	if (t == T_ARQ) {
		const char *m = "rb";
		if (mode == RETRO_VFS_FILE_ACCESS_WRITE)
			m = "wb";
		else if (mode == RETRO_VFS_FILE_ACCESS_READ_WRITE)
			m = "w+b";
		if (mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING)
			m = "r+b";
		fp = fopen(path, m);
		if (!fp && (mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING))
			fp = fopen(path, "w+b");
		if (!fp)
			return NULL;
	} else if (mode & RETRO_VFS_FILE_ACCESS_WRITE)
		return NULL;
	struct retro_vfs_file_handle *s = calloc(1, sizeof *s);
	s->tipo = t;
	s->fp = fp;
	s->path = strdup(path);
	return s;
}
static int v_close(struct retro_vfs_file_handle *s)
{
	if (s->fp)
		fclose(s->fp);
	free(s->path);
	free(s);
	return 0;
}
static int64_t v_size(struct retro_vfs_file_handle *s)
{
	if (s->tipo == T_CUE)
		return strlen(cd_cue);
	if (s->tipo == T_BIN)
		return cd_tam;
	struct stat st;
	return fstat(fileno(s->fp), &st) ? -1 : st.st_size;
}
static int64_t v_tell(struct retro_vfs_file_handle *s)
{
	return s->tipo == T_ARQ ? ftello(s->fp) : s->pos;
}
static int64_t v_seek(struct retro_vfs_file_handle *s, int64_t off, int w)
{
	if (s->tipo == T_ARQ) {
		int ww = w == RETRO_VFS_SEEK_POSITION_CURRENT ? SEEK_CUR : w == RETRO_VFS_SEEK_POSITION_END ? SEEK_END : SEEK_SET;
		return fseeko(s->fp, off, ww) ? -1 : ftello(s->fp);
	}
	int64_t base = w == RETRO_VFS_SEEK_POSITION_CURRENT ? s->pos : w == RETRO_VFS_SEEK_POSITION_END ? v_size(s) : 0;
	if (base + off < 0)
		return -1;
	return s->pos = base + off;
}
static int64_t v_read(struct retro_vfs_file_handle *s, void *buf, uint64_t len)
{
	if (s->tipo == T_ARQ)
		return fread(buf, 1, len, s->fp);
	int64_t n;
	if (s->tipo == T_CUE) {
		int64_t tam = strlen(cd_cue);
		n = s->pos >= tam ? 0 : (int64_t)len < tam - s->pos ? (int64_t)len : tam - s->pos;
		memcpy(buf, cd_cue + s->pos, n);
	} else
		n = cd_ler(s->pos, buf, len);
	s->pos += n;
	return n;
}
static int64_t v_write(struct retro_vfs_file_handle *s, const void *buf, uint64_t len)
{
	return s->tipo == T_ARQ ? (int64_t)fwrite(buf, 1, len, s->fp) : -1;
}
static int v_flush(struct retro_vfs_file_handle *s) { return s->tipo == T_ARQ ? fflush(s->fp) : 0; }
static int v_remove(const char *p) { return remove(p); }
static int v_rename(const char *a, const char *b) { return rename(a, b); }
static int64_t v_truncate(struct retro_vfs_file_handle *s, int64_t len)
{
	return s->tipo == T_ARQ ? ftruncate(fileno(s->fp), len) : -1;
}
static int v_stat(const char *p, int32_t *size)
{
	int t = vfs_tipo(p);
	if (t != T_ARQ) {
		if (size)
			*size = t == T_CUE ? (int32_t)strlen(cd_cue) : (int32_t)(cd_tam > 0x7fffffff ? 0x7fffffff : cd_tam);
		return RETRO_VFS_STAT_IS_VALID;
	}
	struct stat st;
	if (stat(p, &st))
		return 0;
	if (size)
		*size = (int32_t)st.st_size;
	return RETRO_VFS_STAT_IS_VALID | (S_ISDIR(st.st_mode) ? RETRO_VFS_STAT_IS_DIRECTORY : 0) |
	       (S_ISCHR(st.st_mode) ? RETRO_VFS_STAT_IS_CHARACTER_SPECIAL : 0);
}
static int v_stat64(const char *p, int64_t *size)
{
	int32_t s32 = 0;
	int r = v_stat(p, &s32);
	if (size) {
		*size = s32;
		if (vfs_tipo(p) == T_BIN)
			*size = cd_tam;
		else if (r && vfs_tipo(p) == T_ARQ) {
			struct stat st;
			if (!stat(p, &st))
				*size = st.st_size;
		}
	}
	return r;
}
static int v_mkdir(const char *d)
{
	if (!mkdir(d, 0755))
		return 0;
	return errno == EEXIST ? -2 : -1;
}
static struct retro_vfs_dir_handle *v_opendir(const char *dir, bool hidden)
{
	(void)hidden;
	DIR *d = opendir(dir);
	if (!d)
		return NULL;
	struct retro_vfs_dir_handle *h = calloc(1, sizeof *h);
	h->d = d;
	h->path = strdup(dir);
	return h;
}
static bool v_readdir(struct retro_vfs_dir_handle *h) { return (h->e = readdir(h->d)) != NULL; }
static const char *v_dname(struct retro_vfs_dir_handle *h) { return h->e ? h->e->d_name : NULL; }
static bool v_disdir(struct retro_vfs_dir_handle *h)
{
	if (!h->e)
		return false;
	if (h->e->d_type != DT_UNKNOWN && h->e->d_type != DT_LNK)
		return h->e->d_type == DT_DIR;
	char p[4096];
	struct stat st;
	snprintf(p, sizeof p, "%s/%s", h->path, h->e->d_name);
	return !stat(p, &st) && S_ISDIR(st.st_mode);
}
static int v_closedir(struct retro_vfs_dir_handle *h)
{
	closedir(h->d);
	free(h->path);
	free(h);
	return 0;
}
static struct retro_vfs_interface vfs = {
	v_get_path, v_open, v_close, v_size, v_tell, v_seek, v_read, v_write, v_flush, v_remove, v_rename,
	v_truncate, v_stat, v_mkdir, v_opendir, v_readdir, v_dname, v_disdir, v_closedir, v_stat64,
};

/* ------------------------------------------------------------------ framebuffer */
static struct {
	int w, h, stride, bpp, fd;
	uint8_t *mem, *back;
	size_t tam;
	char *fake;
	bool sem_vsync;
} fb;
static bool tubo;

static void fb_abrir(void)
{
	const char *fk = getenv("RV_FAKEFB");
	if (fk) {
		char *c = strdup(fk), *dp = strrchr(c, ':');
		*dp = 0;
		fb.fake = c;
		sscanf(dp + 1, "%dx%d", &fb.w, &fb.h);
		fb.bpp = 16;
		fb.stride = fb.w * 2;
		fb.tam = (size_t)fb.stride * fb.h;
		fb.mem = calloc(1, fb.tam);
		fb.sem_vsync = true;
	} else {
		struct fb_var_screeninfo v;
		struct fb_fix_screeninfo f;
		fb.fd = open("/dev/fb0", O_RDWR);
		if (fb.fd < 0 || ioctl(fb.fd, FBIOGET_VSCREENINFO, &v) || ioctl(fb.fd, FBIOGET_FSCREENINFO, &f)) {
			LOG("ERRO: /dev/fb0: %s", strerror(errno));
			exit(2);
		}
		fb.w = v.xres;
		fb.h = v.yres;
		fb.bpp = v.bits_per_pixel;
		fb.stride = f.line_length;
		fb.tam = (size_t)fb.stride * fb.h;
		fb.mem = mmap(NULL, fb.tam, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, 0);
		if (fb.mem == MAP_FAILED) {
			LOG("ERRO: mmap fb0");
			exit(2);
		}
	}
	fb.back = calloc(1, fb.tam);
	tubo = fb.w >= fb.h * 2.5;
	LOG("video: %dx%d %dbpp (%s)", fb.w, fb.h, fb.bpp, tubo ? "TUBO" : "LCD");
}

/* espera do vsync: 1 = DRM (vblank do CRTC), 2 = fbdev; sem nenhum = ritmo pelo relogio.
 * No Pi 5 o FBIO_WAITFORVSYNC do fbdev volta na hora (nao espera nada): por isso
 * cada metodo e medido e so vale se der um intervalo de tela de verdade (12-25 ms). */
static int vs_tipo, vs_drm = -1;
static unsigned vs_crtc, vs_seq;
static bool vs_medindo = true;
static long vs_ressinc;

static bool vs_espera(int tipo)
{
	if (tipo == 1) {
		union drm_wait_vblank v;
		memset(&v, 0, sizeof v);
		unsigned crtc = (vs_crtc << _DRM_VBLANK_HIGH_CRTC_SHIFT) & _DRM_VBLANK_HIGH_CRTC_MASK;
		if (vs_medindo || !vs_seq) {
			v.request.type = _DRM_VBLANK_RELATIVE | crtc;
			v.request.sequence = 1;
			if (ioctl(vs_drm, DRM_IOCTL_WAIT_VBLANK, &v))
				return false;
			vs_seq = v.reply.sequence;
			return true;
		}
		/* espera o PROXIMO vblank depois do ultimo mostrado: se o quadro atrasou
		 * um pouco, volta na hora e o jogo nao perde o passo */
		v.request.type = _DRM_VBLANK_ABSOLUTE | crtc;
		v.request.sequence = vs_seq + 1;
		if (ioctl(vs_drm, DRM_IOCTL_WAIT_VBLANK, &v))
			return false;
		if ((int)(v.reply.sequence - (vs_seq + 1)) > 3) { /* atraso grande (ex.: CD): ressincroniza */
			vs_seq = v.reply.sequence;
			vs_ressinc++;
		} else
			vs_seq++;
		return true;
	}
	uint32_t z = 0;
	return ioctl(fb.fd, FBIO_WAITFORVSYNC, &z) == 0;
}

static int cmp_dbl(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

/* mede o vsync: mediana de 30 intervalos (uma medida so pegava um atraso e
 * dava 60,6 Hz, o que tirava o jogo do "1 quadro por vsync") */
static double vs_mede(int tipo)
{
	enum { N = 30 };
	double iv[N];
	if (!vs_espera(tipo))
		return 0;
	double t0 = agora(), ti = t0;
	for (int i = 0; i < N; i++) {
		if (!vs_espera(tipo))
			return 0;
		double t = agora();
		iv[i] = t - ti;
		ti = t;
		if (i == 2 && t - t0 < 0.003)
			return (t - t0) / 3; /* nao espera de verdade: sai ja */
	}
	qsort(iv, N, sizeof iv[0], cmp_dbl);
	double soma = 0;
	int n = 0;
	for (int i = 0; i < N; i++)
		if (fabs(iv[i] - iv[N / 2]) < 0.001) {
			soma += iv[i];
			n++;
		}
	return n ? soma / n : iv[N / 2];
}

static double vs_escolhe(void)
{
	if (fb.fake)
		return 0;
	char p[32];
	for (int c = 0; c < 4 && !vs_tipo; c++) {
		snprintf(p, sizeof p, "/dev/dri/card%d", c);
		int fd = open(p, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		vs_drm = fd;
		for (vs_crtc = 0; vs_crtc < 4; vs_crtc++) {
			double t = vs_mede(1);
			LOG("vsync: %s crtc %u: %.2f ms", p, vs_crtc, t * 1e3);
			if (t > 0.012 && t < 0.025) {
				vs_tipo = 1;
				return 1.0 / t;
			}
		}
		close(fd);
		vs_drm = -1;
	}
	double t = vs_mede(2);
	LOG("vsync: fbdev: %.2f ms", t * 1e3);
	if (t > 0.012 && t < 0.025) {
		vs_tipo = 2;
		return 1.0 / t;
	}
	LOG("vsync: nenhum funcionou: ritmo pelo relogio");
	return 0;
}

static void fb_vsync(void)
{
	if (vs_tipo && !vs_espera(vs_tipo)) {
		LOG("vsync: parou de funcionar: ritmo pelo relogio");
		vs_tipo = 0;
	}
	fb.sem_vsync = vs_tipo == 0;
}

static void fb_mostra(void)
{
	fb_vsync();
	memcpy(fb.mem, fb.back, fb.tam);
}

static void fb_grava_fake(void)
{
	if (!fb.fake)
		return;
	FILE *f = fopen(fb.fake, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", fb.w, fb.h);
	for (int i = 0; i < fb.w * fb.h; i++) {
		uint16_t p = ((uint16_t *)fb.mem)[i];
		uint8_t rgb[3] = {(p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3};
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

/* converte uma linha do nucleo para o formato do fb (16 ou 32 bpp) */
static inline uint32_t px_conv(const uint8_t *src, int x, int fmt)
{
	uint32_t r, g, b;
	if (fmt == RETRO_PIXEL_FORMAT_RGB565) {
		uint16_t p = ((const uint16_t *)src)[x];
		if (fb.bpp == 16)
			return p;
		r = (p >> 11) << 3; g = ((p >> 5) & 63) << 2; b = (p & 31) << 3;
	} else if (fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
		uint32_t p = ((const uint32_t *)src)[x];
		r = (p >> 16) & 255; g = (p >> 8) & 255; b = p & 255;
	} else {
		uint16_t p = ((const uint16_t *)src)[x];
		r = ((p >> 10) & 31) << 3; g = ((p >> 5) & 31) << 3; b = (p & 31) << 3;
	}
	if (fb.bpp == 16)
		return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
	return (r << 16) | (g << 8) | b;
}

static int ult_x0 = -99999, ult_y0, ult_cw, ult_dh;
static int *xmap;
static int xmap_w = -1, xmap_cw = -1;
static uint32_t *linha_tmp;

/* desenha a imagem (w x h) no fb.back, ja com a convergencia */
static void desenha(const void *data, unsigned w, unsigned h, size_t pitch, int fmt, double aspecto)
{
	int passo = (h > 300 && !sem_filtro) ? 2 : 1; /* 480i do jogo: uma linha sim, outra nao */
	int eh = h / passo, lp, cw, x0, y0, dh;
	if (tubo) {
		lp = fb.h >= 400 ? 2 : 1;
		cw = (int)lround(fb.w * conv_w);
		if (cw > fb.w) cw = fb.w;
		if (cw < (int)w) cw = w;
		x0 = (fb.w - cw) / 2 + conv_x;
		y0 = (fb.h - eh * lp) / 2 + conv_y * lp;
		dh = eh * lp;
	} else {
		lp = fb.h / eh;
		if (lp < 1) lp = 1;
		dh = eh * lp;
		if (imagem == 1 && !sem_filtro) /* TELA CHEIA: usa a altura toda (linhas de 3 ou 4 pontos) */
			dh = fb.h;
		cw = (int)lround(dh * (aspecto > 0 ? aspecto : 4.0 / 3.0));
		if (cw > fb.w) cw = fb.w;
		x0 = (fb.w - cw) / 2;
		y0 = (fb.h - dh) / 2;
	}
	if (x0 != ult_x0 || y0 != ult_y0 || cw != ult_cw || dh != ult_dh) {
		memset(fb.back, 0, fb.tam); /* mudou a geometria: limpa as bordas */
		ult_x0 = x0; ult_y0 = y0; ult_cw = cw; ult_dh = dh;
	}
	if ((int)w != xmap_w || cw != xmap_cw) {
		free(xmap);
		xmap = malloc(sizeof(int) * cw);
		for (int i = 0; i < cw; i++)
			xmap[i] = (int)((int64_t)i * w / cw);
		xmap_w = w; xmap_cw = cw;
		free(linha_tmp);
		linha_tmp = malloc(sizeof(uint32_t) * (w + 1));
	}
	int bpp = fb.bpp / 8;
	int i0 = x0 < 0 ? -x0 : 0;
	int i1 = x0 + cw > fb.w ? fb.w - x0 : cw;
	for (int j = 0; j < eh; j++) {
		const uint8_t *src = (const uint8_t *)data + (size_t)j * passo * pitch;
		for (unsigned x = 0; x < w; x++)
			linha_tmp[x] = px_conv(src, x, fmt);
		/* linhas da tela desta linha do jogo (sempre lp no tubo/ORIGINAL; 3 ou 4 em TELA CHEIA) */
		int ya = (int)((int64_t)j * dh / eh), yb = (int)((int64_t)(j + 1) * dh / eh);
		int lpj = yb - ya;
		for (int k = 0; k < lpj; k++) {
			int yy = y0 + ya + k;
			if (yy < 0 || yy >= fb.h)
				continue;
			uint8_t *d = fb.back + (size_t)yy * fb.stride + (size_t)x0 * bpp;
			/* efeitos (so no LCD, com a imagem ampliada 2x ou mais):
			 * scanlines = ultima linha de cada grupo mais escura (50% ou 25%);
			 * CRT = scanlines 50% + grade de fosforo (1 coluna em 3 com 25% menos luz) */
			int ef = (!tubo && lp >= 2 && !sem_filtro) ? filtro : 0;
			int linha_esc = (ef == 1 || ef == 3) && k == lpj - 1 ? 2 : (ef == 2 && k == lpj - 1) ? 1 : 0;
			bool grade = ef == 3;
			if (bpp == 2) {
				uint16_t *o = (uint16_t *)d;
				for (int i = i0; i < i1; i++) {
					uint16_t p = linha_tmp[xmap[i]];
					if (linha_esc == 2)
						p = (p >> 1) & 0x7BEF;
					else if (linha_esc == 1)
						p -= (p >> 2) & 0x39E7;
					if (grade && (x0 + i) % 3 == 2)
						p -= (p >> 2) & 0x39E7;
					o[i] = p;
				}
			} else {
				uint32_t *o = (uint32_t *)d;
				for (int i = i0; i < i1; i++) {
					uint32_t p = linha_tmp[xmap[i]];
					if (linha_esc == 2)
						p = (p >> 1) & 0x7F7F7F;
					else if (linha_esc == 1)
						p -= (p >> 2) & 0x3F3F3F;
					if (grade && (x0 + i) % 3 == 2)
						p -= (p >> 2) & 0x3F3F3F;
					o[i] = p;
				}
			}
		}
	}
}

/* aviso de resolucao: "640x480 @ 59.94 Hz" por 3 s no canto superior direito
 * da imagem, ao abrir o jogo e sempre que a resolucao do jogo mudar */
static const Fonte *F;
/* avisos na tela do jogo: fundo laranja, letra azul, cantos arredondados.
 * resolucao: canto superior direito; controle: canto inferior esquerdo (mais largo).
 * texto que nao cabe rola dentro da caixa (letreiro). */
typedef struct { char txt[160]; double ini, ate; int max; bool baixo; } Aviso;
static Aviso av_res = {.max = 46}, av_ctl = {.max = 50, .baixo = true};
static char osd_pend[160];
static bool osd_tem_pend;

static void aviso_poe(Aviso *a, const char *t)
{
	snprintf(a->txt, sizeof a->txt, "%s", t);
	int n = strlen(a->txt);
	a->ini = agora();
	a->ate = a->ini + 3 + (n > a->max ? (n - a->max) / 8.0 + 1.5 : 0);
}
static unsigned osd_w, osd_h;

static void fb_ponto(int x, int y, int r, int g, int b)
{
	if (x < 0 || y < 0 || x >= fb.w || y >= fb.h)
		return;
	uint8_t *d = fb.back + (size_t)y * fb.stride;
	if (fb.bpp == 16)
		((uint16_t *)d)[x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
	else
		((uint32_t *)d)[x] = (r << 16) | (g << 8) | b;
}

static void aviso_desenha(Aviso *a)
{
	if (!a->txt[0] || ult_x0 == -99999 || agora() > a->ate)
		return;
	int sx = tubo ? ult_cw / 320 : fb.h / 240;
	if (sx < 1) sx = 1;
	int sy = tubo ? (fb.h >= 400 ? 2 : 1) : sx;
	int n = strlen(a->txt), cabe = (ult_cw / sx - 40) / F->cw;
	int max = a->max < cabe ? a->max : cabe, vis = n > max ? max : n;
	int tw = vis * F->cw * sx, th = F->ch * sy;
	int x1 = a->baixo ? ult_x0 + 14 * sx : ult_x0 + ult_cw - tw - 14 * sx;
	int y1 = a->baixo ? ult_y0 + ult_dh - th - 10 * sy : ult_y0 + 8 * sy;
	int bx0 = x1 - 5 * sx, bx1 = x1 + tw + 5 * sx, by0 = y1 - 3 * sy, by1 = y1 + th + 2 * sy;
	for (int y = by0; y < by1; y++)
		for (int x = bx0; x < bx1; x++) {
			bool canto = (x < bx0 + sx || x >= bx1 - sx) && (y < by0 + sy || y >= by1 - sy);
			if (!canto)
				fb_ponto(x, y, 255, 140, 40);
		}
	int desl = 0; /* letreiro: espera 1 s, rola 8 letras por segundo, para no fim */
	if (n > max) {
		double t = agora() - a->ini - 1.0;
		desl = t > 0 ? (int)(t * 8 * F->cw) : 0;
		if (desl > (n - max) * F->cw)
			desl = (n - max) * F->cw;
	}
	for (int k = 0; k < n; k++) {
		int o = (unsigned char)a->txt[k] - 32;
		if (o < 0 || o > 223)
			continue;
		for (int r = 0; r < F->ch; r++) {
			uint8_t bits = F->b[o * F->ch + r];
			for (int c = 0; c < F->cw; c++)
				if (bits >> c & 1)
					for (int yy = 0; yy < sy; yy++)
						for (int xx = 0; xx < sx; xx++) {
							int px = x1 + (k * F->cw + c - desl) * sx + xx;
							if (px >= x1 && px < x1 + tw)
								fb_ponto(px, y1 + r * sy + yy, 12, 18, 40);
						}
		}
	}
}

static void osd_desenha(void)
{
	aviso_desenha(&av_res);
	aviso_desenha(&av_ctl);
}

/* ultimo quadro do jogo (para o fundo do menu) */
static uint8_t *ult_q;
static unsigned ult_w, ult_h;
static size_t ult_pitch, ult_cap;
static int ult_fmt;

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
	if (!data) /* repetir quadro: o fb.back ja tem a imagem */
		return;
	size_t t = pitch * h;
	if (t > ult_cap) {
		free(ult_q);
		ult_q = malloc(t);
		ult_cap = t;
	}
	memcpy(ult_q, data, t);
	ult_w = w; ult_h = h; ult_pitch = pitch; ult_fmt = pixfmt;
	desenha(data, w, h, pitch, pixfmt, av.geometry.aspect_ratio);
	if (w != osd_w || h != osd_h) {
		osd_w = w;
		osd_h = h;
		/* jogo: mais de 288 linhas = entrelacado (i), senao progressivo (p);
		 * saida: o que vai de verdade para a TV/monitor */
		char sai[32], t[96];
		if (tubo)
			snprintf(sai, sizeof sai, "TUBO %d%s", fb.h >= 400 ? fb.h : fb.h, fb.h >= 400 ? "i" : "p");
		else
			snprintf(sai, sizeof sai, "%dx%d", fb.w, fb.h);
		snprintf(t, sizeof t, "%ux%u%s %.2fHz  SAIDA %s", w, h, h > 288 ? "i" : "p", av.timing.fps, sai);
		aviso_poe(&av_res, t);
		LOG("resolucao do jogo: %s", t);
	}
}

/* ------------------------------------------------------------------ som (ALSA via dlopen) */
static struct {
	void *h;
	int (*open)(void **, const char *, int, int);
	int (*set_params)(void *, int, int, unsigned, unsigned, int, unsigned);
	long (*writei)(void *, const void *, unsigned long);
	int (*recover)(void *, int, int);
	int (*close)(void *);
	void *pcm;
} al;
#define SAIDA_HZ 48000
#define RING 8192
static int16_t ring[RING * 2];
static int r_rd, r_wr, r_cnt;
static pthread_mutex_t r_mx = PTHREAD_MUTEX_INITIALIZER;
static double rs_pos, rs_passo_base = 1.0;
static int16_t rs_ant[2];
static bool som_ok, som_mudo;
static long som_vazio;

static void *som_thread(void *p)
{
	(void)p;
	int16_t buf[256 * 2];
	struct sched_param sp = {.sched_priority = 20};
	if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp))
		LOG("som: sem prioridade de tempo real");
	while (!g_sair) {
		pthread_mutex_lock(&r_mx);
		int n = r_cnt < 256 ? r_cnt : 256;
		if (n < 64 && r_cnt < 64 && n > 0) {
			/* quase vazio: espera um pouco antes de completar com silencio */
			pthread_mutex_unlock(&r_mx);
			usleep(2000);
			pthread_mutex_lock(&r_mx);
			n = r_cnt < 256 ? r_cnt : 256;
		}
		for (int i = 0; i < n; i++) {
			buf[i * 2] = ring[r_rd * 2];
			buf[i * 2 + 1] = ring[r_rd * 2 + 1];
			r_rd = (r_rd + 1) % RING;
		}
		r_cnt -= n;
		pthread_mutex_unlock(&r_mx);
		int m = n;
		if (n == 0) {
			memset(buf, 0, 64 * 4);
			m = 64;
			som_vazio++;
		}
		long e = al.writei(al.pcm, buf, m);
		if (e < 0)
			al.recover(al.pcm, (int)e, 1);
	}
	return NULL;
}

static void som_abrir(double hz_nucleo)
{
	const char *libs[] = {"libasound.so.2", "libasound.so"};
	for (int i = 0; i < 2 && !al.h; i++)
		al.h = dlopen(libs[i], RTLD_NOW);
	if (!al.h) {
		LOG("som: libasound nao encontrada: sem som");
		return;
	}
	al.open = dlsym(al.h, "snd_pcm_open");
	al.set_params = dlsym(al.h, "snd_pcm_set_params");
	al.writei = dlsym(al.h, "snd_pcm_writei");
	al.recover = dlsym(al.h, "snd_pcm_recover");
	al.close = dlsym(al.h, "snd_pcm_close");
	const char *devs[] = {a_audio, "hdmi:CARD=vc4hdmi0,DEV=0", "hdmi:CARD=vc4hdmi1,DEV=0", "default", "sysdefault"};
	for (int i = 0; i < 5; i++) {
		if (!devs[i])
			continue;
		if (al.open(&al.pcm, devs[i], 0 /* PLAYBACK */, 0) < 0)
			continue;
		/* S16_LE=2, RW_INTERLEAVED=3, 2 canais, 48 kHz, reamostrar sim, 40 ms */
		if (al.set_params(al.pcm, 2, 3, 2, SAIDA_HZ, 1, 40000) < 0) {
			al.close(al.pcm);
			al.pcm = NULL;
			continue;
		}
		LOG("som: %s", devs[i]);
		break;
	}
	if (!al.pcm) {
		LOG("som: nenhuma saida funcionou: sem som");
		return;
	}
	rs_passo_base = hz_nucleo / SAIDA_HZ;
	som_ok = true;
	pthread_t t;
	pthread_create(&t, NULL, som_thread, NULL);
}

static size_t audio_batch(const int16_t *d, size_t frames)
{
	if (!som_ok || som_mudo)
		return frames;
	pthread_mutex_lock(&r_mx);
	/* controle de ritmo: mantem o buffer pela metade (+-0,5% de velocidade) */
	double cheio = (double)r_cnt / RING;
	double passo = rs_passo_base * (1.0 + (cheio - 0.5) * 0.01);
	for (size_t i = 0; i < frames; i++) {
		int16_t l = (int16_t)((d[i * 2] * ganho) >> 15), r = (int16_t)((d[i * 2 + 1] * ganho) >> 15);
		while (rs_pos <= 1.0) {
			if (r_cnt < RING) {
				ring[r_wr * 2] = rs_ant[0] + (l - rs_ant[0]) * rs_pos;
				ring[r_wr * 2 + 1] = rs_ant[1] + (r - rs_ant[1]) * rs_pos;
				r_wr = (r_wr + 1) % RING;
				r_cnt++;
			}
			rs_pos += passo;
		}
		rs_pos -= 1.0;
		rs_ant[0] = l;
		rs_ant[1] = r;
	}
	pthread_mutex_unlock(&r_mx);
	return frames;
}
static void audio_sample(int16_t l, int16_t r)
{
	int16_t d[2] = {l, r};
	audio_batch(d, 1);
}

/* ------------------------------------------------------------------ controles (evdev) */
#define MAXDEV 12
enum { B_B, B_Y, B_SELECT, B_START, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_A, B_X, B_L, B_R, B_L2, B_R2, B_L3, B_R3, B_MENU = 16 };
static struct dev {
	int fd, porta;
	bool pad;
	char nome[80], path[300];
	uint32_t bot; /* botoes apertados (bits B_*) */
	int hatx, haty;
	int16_t ana[4];
	struct input_absinfo abs[ABS_CNT];
} devs[MAXDEV];
static int ndevs;
static uint32_t pad_bot[2];
static int16_t pad_ana[2][4];

static int mapa_tecla(int c)
{
	switch (c) {
	case BTN_SOUTH: return B_B;
	case BTN_EAST: return B_A;
	case BTN_NORTH: return B_Y; /* xpad: botao X (esquerda) */
	case BTN_WEST: return B_X;  /* xpad: botao Y (cima) */
	case BTN_TL: return B_L;
	case BTN_TR: return B_R;
	case BTN_TL2: return B_L2;
	case BTN_TR2: return B_R2;
	case BTN_THUMBL: return B_L3;
	case BTN_THUMBR: return B_R3;
	case BTN_SELECT: return B_SELECT;
	case BTN_START: return B_START;
	case BTN_MODE: return B_MENU;
	case BTN_DPAD_UP: case KEY_UP: return B_UP;
	case BTN_DPAD_DOWN: case KEY_DOWN: return B_DOWN;
	case BTN_DPAD_LEFT: case KEY_LEFT: return B_LEFT;
	case BTN_DPAD_RIGHT: case KEY_RIGHT: return B_RIGHT;
	case KEY_Z: return B_B;
	case KEY_X: return B_A;
	case KEY_A: return B_Y;
	case KEY_S: return B_X;
	case KEY_Q: return B_L;
	case KEY_W: return B_R;
	case KEY_ENTER: return B_START;
	case KEY_RIGHTSHIFT: case KEY_BACKSPACE: return B_SELECT;
	case KEY_ESC: return B_MENU;
	}
	return -1;
}

#define TEM_BIT(arr, b) ((arr)[(b) / 8] & (1 << ((b) % 8)))
/* procura de controles numa thread separada e so quando /dev/input muda:
 * abrir alguns aparelhos (CEC do HDMI, botoes da caixa, "mouse" do receptor)
 * e lento e travava o jogo a cada 2 s */
static pthread_mutex_t dv_mx = PTHREAD_MUTEX_INITIALIZER;
static struct { char p[300]; ino_t ino; } rej[64];
static int nrej;

static bool ja_tem(const char *p, ino_t ino)
{
	for (int i = 0; i < nrej; i++)
		if (rej[i].ino == ino && !strcmp(rej[i].p, p))
			return true;
	bool r = false;
	pthread_mutex_lock(&dv_mx);
	for (int i = 0; i < ndevs; i++)
		if (!strcmp(devs[i].path, p))
			r = true;
	pthread_mutex_unlock(&dv_mx);
	return r;
}

static void controles_busca(void)
{
	DIR *d = opendir("/dev/input");
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		char p[300];
		struct stat st;
		snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
		if (stat(p, &st) || ja_tem(p, st.st_ino))
			continue;
		int fd = open(p, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		uint8_t keys[KEY_MAX / 8 + 1] = {0};
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
		bool pad = TEM_BIT(keys, BTN_SOUTH) || TEM_BIT(keys, BTN_GAMEPAD);
		bool tecl = TEM_BIT(keys, KEY_ENTER) && TEM_BIT(keys, KEY_Z);
		if (!pad && !tecl) {
			close(fd);
			int k;
			for (k = 0; k < nrej; k++)
				if (!strcmp(rej[k].p, p))
					break;
			if (k == nrej && nrej < 64)
				nrej++;
			if (k < 64) {
				strcpy(rej[k].p, p);
				rej[k].ino = st.st_ino;
			}
			continue;
		}
		static struct dev v;
		memset(&v, 0, sizeof v);
		v.fd = fd;
		v.pad = pad;
		strcpy(v.path, p);
		ioctl(fd, EVIOCGNAME(sizeof v.nome), v.nome);
		for (int a = 0; a < ABS_CNT; a++)
			ioctl(fd, EVIOCGABS(a), &v.abs[a]);
		pthread_mutex_lock(&dv_mx);
		if (ndevs < MAXDEV) {
			int npads = 0;
			for (int i = 0; i < ndevs; i++)
				npads += devs[i].pad;
			v.porta = pad && npads > 0 ? 1 : 0;
			devs[ndevs++] = v;
			LOG("controle: %s (%s) -> jogador %d", v.nome, p, v.porta + 1);
			if (v.pad) {   /* aviso na tela (a thread so deixa pendente; o laco principal mostra) */
				snprintf(osd_pend, sizeof osd_pend, "CONTROLE %d: %s", v.porta + 1, v.nome);
				osd_tem_pend = true;
			}
		} else
			close(fd);
		pthread_mutex_unlock(&dv_mx);
	}
	closedir(d);
}

static void *busca_thread(void *x)
{
	(void)x;
	struct timespec ult = {0, 0};
	while (!g_sair) {
		struct stat st;
		if (!stat("/dev/input", &st) && (st.st_mtim.tv_sec != ult.tv_sec || st.st_mtim.tv_nsec != ult.tv_nsec)) {
			ult = st.st_mtim;
			usleep(300000); /* deixa o aparelho novo terminar de aparecer */
			controles_busca();
		}
		usleep(500000);
	}
	return NULL;
}

/* CONTROLES: remap[botao do nucleo] = botao do controle */
static int remap[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static uint32_t pad_core[2]; /* botoes ja remapeados, como o nucleo ve */

static double abs_norm(struct dev *v, int c, int val) /* -1..1 */
{
	struct input_absinfo *a = &v->abs[c];
	if (a->maximum <= a->minimum)
		return val;
	return (2.0 * (val - a->minimum) / (a->maximum - a->minimum)) - 1.0;
}

static void controles_le(void)
{
	struct input_event ev[64];
	pthread_mutex_lock(&dv_mx);
	for (int i = 0; i < ndevs; i++) {
		struct dev *v = &devs[i];
		for (;;) {
			ssize_t n = read(v->fd, ev, sizeof ev);
			if (n <= 0) {
				if (n < 0 && errno != EAGAIN) { /* desconectou */
					LOG("controle removido: %s", v->nome);
					close(v->fd);
					devs[i] = devs[--ndevs];
					i--;
				}
				break;
			}
			for (int k = 0; k < (int)(n / sizeof *ev); k++) {
				struct input_event *e = &ev[k];
				if (e->type == EV_KEY) {
					int b = mapa_tecla(e->code);
					if (b < 0)
						continue;
					if (e->value)
						v->bot |= 1u << b;
					else
						v->bot &= ~(1u << b);
				} else if (e->type == EV_ABS) {
					double x = abs_norm(v, e->code, e->value);
					switch (e->code) {
					case ABS_HAT0X: v->hatx = x < -0.5 ? -1 : x > 0.5 ? 1 : 0; break;
					case ABS_HAT0Y: v->haty = x < -0.5 ? -1 : x > 0.5 ? 1 : 0; break;
					case ABS_X: v->ana[0] = x * 32767; break;
					case ABS_Y: v->ana[1] = x * 32767; break;
					case ABS_RX: v->ana[2] = x * 32767; break;
					case ABS_RY: v->ana[3] = x * 32767; break;
					case ABS_Z: case ABS_BRAKE:
						if (x > 0.0) v->bot |= 1u << (B_L2 + 16); else v->bot &= ~(1u << (B_L2 + 16));
						break;
					case ABS_RZ: case ABS_GAS:
						if (x > 0.0) v->bot |= 1u << (B_R2 + 16); else v->bot &= ~(1u << (B_R2 + 16));
						break;
					}
				}
			}
		}
	}
	for (int p = 0; p < 2; p++) {
		uint32_t b = 0;
		int16_t a[4] = {0};
		for (int i = 0; i < ndevs; i++) {
			struct dev *v = &devs[i];
			if (v->porta != p)
				continue;
			uint32_t m = v->bot;
			m |= (m >> 16) & ((1u << B_L2) | (1u << B_R2)); /* gatilhos analogicos */
			m &= 0x1ffff;
			if (v->hatx < 0) m |= 1u << B_LEFT;
			if (v->hatx > 0) m |= 1u << B_RIGHT;
			if (v->haty < 0) m |= 1u << B_UP;
			if (v->haty > 0) m |= 1u << B_DOWN;
			b |= m;
			for (int k = 0; k < 4; k++)
				if (abs(v->ana[k]) > abs(a[k]))
					a[k] = v->ana[k];
		}
		pad_bot[p] = b;
		memcpy(pad_ana[p], a, sizeof a);
		uint32_t c = 0;
		for (int id = 0; id < 16; id++)
			if (b >> remap[id] & 1)
				c |= 1u << id;
		pad_core[p] = c;
	}
	pthread_mutex_unlock(&dv_mx);
}

static bool entrada_travada; /* depois do menu: espera soltar tudo */
/* CONTROLES (menu do jogo): remap[botao do nucleo] = botao do controle; salvo por nucleo */
static char *bot_desc[16];
static int bot_ordem[16], n_ordem; /* botoes na ordem em que o nucleo descreve */
static const char *a_botoes;
static const char *NOME_FIS[16] = {"A", "X", "SELECT", "START", "CIMA", "BAIXO", "ESQUERDA", "DIREITA",
                                   "B", "Y", "LB", "RB", "LT", "RT", "L3", "R3"};

static void botoes_ler(void)
{
	FILE *f = a_botoes ? fopen(a_botoes, "r") : NULL;
	if (!f)
		return;
	int a, b;
	char l[64];
	while (fgets(l, sizeof l, f))
		if (sscanf(l, "%d = %d", &a, &b) == 2 && a >= 0 && a < 16 && b >= 0 && b < 16)
			remap[a] = b;
	fclose(f);
	LOG("controles: lidos de %s", a_botoes);
}

static void mkdir_pai(const char *path)
{
	char d[4400];
	snprintf(d, sizeof d, "%s", path);
	for (char *p = d + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(d, 0755);
			*p = '/';
		}
}

static void botoes_grava(void)
{
	if (!a_botoes)
		return;
	mkdir_pai(a_botoes);
	FILE *f = fopen(a_botoes, "w");
	if (!f)
		return;
	fprintf(f, "# RETROVOLT: botao do nucleo = botao do controle (0=A 1=X 2=SELECT 3=START 4-7=direcional 8=B 9=Y 10=LB 11=RB 12=LT 13=RT 14=L3 15=R3)\n");
	for (int i = 0; i < 16; i++)
		fprintf(f, "%d = %d\n", i, remap[i]);
	fclose(f);
	sync();
	LOG("controles: gravados em %s", a_botoes);
}

static void opcoes_grava(void)
{
	if (!a_opcoes)
		return;
	mkdir_pai(a_opcoes);
	FILE *f = fopen(a_opcoes, "w");
	if (!f)
		return;
	fprintf(f, "# RETROVOLT: opcoes do nucleo (mudadas no menu do jogo)\n");
	for (int i = 0; i < nvars; i++)
		fprintf(f, "%s = \"%s\"\n", vars[i].k, vars[i].v);
	fclose(f);
	sync();
	LOG("opcoes do nucleo: gravadas em %s", a_opcoes);
}
static void input_poll(void) {}
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
	if (port > 1 || entrada_travada)
		return 0;
	uint32_t b = pad_core[port];
	if ((device & RETRO_DEVICE_MASK) == RETRO_DEVICE_JOYPAD) {
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
			return b;
		return id < 16 ? (b >> id) & 1 : 0;
	}
	if ((device & RETRO_DEVICE_MASK) == RETRO_DEVICE_ANALOG && index < 2 && id < 2)
		return pad_ana[port][index * 2 + id];
	return 0;
}

/* ------------------------------------------------------------------ saves (SAVE_RAM) */
static char srm_path[4096];
static uint8_t *srm_copia;
static size_t srm_tam;

static void sram_carrega(void)
{
	srm_tam = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	uint8_t *m = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
	if (!srm_tam || !m)
		return;
	snprintf(srm_path, sizeof srm_path, "%s/%s.srm", a_saves, a_save ? a_save : a_nome);
	FILE *f = fopen(srm_path, "rb");
	if (f) {
		size_t n = fread(m, 1, srm_tam, f);
		fclose(f);
		LOG("save: carregado %s (%zu bytes)", srm_path, n);
	}
	srm_copia = malloc(srm_tam);
	memcpy(srm_copia, m, srm_tam);
}

static void sram_grava(bool forca)
{
	uint8_t *m = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
	if (!srm_copia || !m)
		return;
	if (!forca && !memcmp(m, srm_copia, srm_tam))
		return;
	char tmp[4200];
	snprintf(tmp, sizeof tmp, "%s.tmp", srm_path);
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		LOG("save: ERRO ao gravar %s: %s", tmp, strerror(errno));
		return;
	}
	fwrite(m, 1, srm_tam, f);
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	rename(tmp, srm_path);
	memcpy(srm_copia, m, srm_tam);
	LOG("save: gravado %s", srm_path);
}

/* ------------------------------------------------------------------ ambiente */
static void log_cb(enum retro_log_level lv, const char *fmt, ...)
{
	if (lv < RETRO_LOG_INFO)
		return;
	char b[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	size_t n = strlen(b);
	while (n && b[n - 1] == '\n')
		b[--n] = 0;
	printf("nucleo: %s\n", b);
	fflush(stdout);
}

static bool environment(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		pixfmt = *(enum retro_pixel_format *)data;
		return pixfmt <= RETRO_PIXEL_FORMAT_RGB565;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char **)data = a_bios;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = a_saves;
		return true;
	case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
		*(const char **)data = a_bios;
		return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = log_cb;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES: {
		const struct retro_variable *v = data;
		for (; v && v->key; v++) {
			const char *p = strchr(v->value, ';');
			if (!p)
				continue;
			p++;
			while (*p == ' ')
				p++;
			char def[256];
			size_t n = strcspn(p, "|");
			if (n >= sizeof def)
				n = sizeof def - 1;
			memcpy(def, p, n);
			def[n] = 0;
			var_set(v->key, def, true);
			for (int i = 0; i < nvars; i++)       /* guarda descricao e valores para o menu */
				if (!strcmp(vars[i].k, v->key) && !vars[i].desc) {
					vars[i].desc = strndup(v->value, strchr(v->value, ';') - v->value);
					char *lista = strdup(p), *sv = NULL;
					for (char *t = strtok_r(lista, "|", &sv); t && vars[i].nvals < 64; t = strtok_r(NULL, "|", &sv))
						vars[i].vals[vars[i].nvals++] = t;
				}
		}
		return true;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *v = data;
		for (int i = 0; i < nvars; i++)
			if (!strcmp(vars[i].k, v->key)) {
				v->value = vars[i].v;
				return true;
			}
		v->value = NULL;
		return false;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = vars_mudou;
		vars_mudou = false;
		return true;
	case RETRO_ENVIRONMENT_GET_VFS_INTERFACE: {
		struct retro_vfs_interface_info *vi = data;
		if (vi->required_interface_version > 3)
			return false;
		vi->required_interface_version = 3;
		vi->iface = &vfs;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		if (data)
			*(int *)data = 3;
		return true;
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
		av.geometry = *(const struct retro_game_geometry *)data;
		return true;
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		av = *(const struct retro_system_av_info *)data;
		LOG("nucleo mudou: %.3f Hz, som %.0f Hz", av.timing.fps, av.timing.sample_rate);
		if (som_ok)
			rs_passo_base = av.timing.sample_rate / SAIDA_HZ;
		return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE:
		LOG("mensagem: %s", ((const struct retro_message *)data)->msg);
		return true;
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: {
		const struct retro_input_descriptor *d = data;
		for (; d && d->description; d++)
			if (d->port == 0 && d->device == RETRO_DEVICE_JOYPAD && d->id < 16 && !bot_desc[d->id]) {
				bot_desc[d->id] = strdup(d->description);
				bot_ordem[n_ordem++] = d->id;
			}
		return true;
	}
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
	case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
	case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
	case RETRO_ENVIRONMENT_SET_ROTATION:
		return true;
	}
	return false;
}

/* ------------------------------------------------------------------ menu por cima do jogo */
/* tela logica do menu do jogo = a do menu principal: tubo 320x240; LCD: o maior
 * numero inteiro de vezes que 240 cabe na altura, enchendo a tela (1440x900 -> 480x300 x3) */
static int mw = 320, mh = 240, m_esc = 1, m_ox, m_oy;
#define MW mw
#define MH mh
#define OV_MAX (720 * 480)
#define NAV 0x0885   /* (12,18,40) em RGB565 */
static uint16_t ov[OV_MAX];
static uint16_t fundo[OV_MAX];

static void menu_tamanho(void)
{
	if (tubo)
		return;
	int e = fb.h / 240;
	if (e < 1) e = 1;
	while (e > 1 && fb.w / e < 320)
		e--;
	mw = fb.w / e;
	mh = fb.h / e;
	if (mw > 720) mw = 720;
	if (mw * mh > OV_MAX) mh = OV_MAX / mw;
	m_esc = e;
	m_ox = (fb.w - mw * e) / 2;
	m_oy = (fb.h - mh * e) / 2;
	LOG("menu do jogo: %dx%d ampliado %dx", mw, mh, e);
}
static int menu_sel;
#define N_MENU 12
enum { M_CONTINUAR, M_SALVAR, M_CARREGAR, M_VOLUME, M_CONV, M_IMAGEM, M_FILTRO, M_OPCOES, M_CONTROLES, M_REINICIAR,
       M_LISTA, M_SAIR };
static const char *MENU[N_MENU] = {"CONTINUAR", "SALVAR ESTADO", "CARREGAR ESTADO", "VOLUME", "CONVERGENCIA",
                                   "IMAGEM", "FILTRO DE IMAGEM", "OPCOES DO NUCLEO", "CONTROLES", "REINICIAR JOGO",
                                   "VOLTAR PARA A LISTA", "SAIR DO JOGO"};
/* itens que nao valem para a tela atual: CONVERGENCIA so no tubo; IMAGEM e FILTRO so no LCD */
static bool menu_oculto(int i)
{
	return (i == M_CONV && !tubo) || ((i == M_IMAGEM || i == M_FILTRO) && tubo);
}
static int sub;           /* 0 = menu, 1 = opcoes do nucleo, 2 = controles, 3 = esperando botao,
                           * 4 = configurar um por um (assistente), 5 = espacos (save state),
                           * 6 = confirmar substituir, 7 = convergencia */
static int slot_modo, slot_sel; /* 0 = salvar, 1 = carregar */
static int cv_x, cv_y;          /* convergencia ao entrar (B desfaz) */
static double cv_w;
static int ass_pos;       /* assistente: posicao em bot_ordem */
static int sub_sel, sub_ini;
static bool sub_mudou;

static uint16_t rgb(int r, int g, int b) { return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); }
static void m_rect(int x, int y, int w, int h, uint16_t c)
{
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			if (i >= 0 && i < MW && j >= 0 && j < MH)
				ov[j * MW + i] = c;
}
static void m_text(int x, int y, const char *s, uint16_t c)
{
	for (int n = 0; s[n]; n++) {
		int o = (unsigned char)s[n] - 32;
		if (o < 0 || o > 223)
			continue;
		for (int r = 0; r < F->ch; r++) {
			uint8_t b = F->b[o * F->ch + r];
			for (int k = 0; k < F->cw; k++)
				if (b >> k & 1) {
					int xx = x + n * F->cw + k, yy = y + r;
					if (xx >= 0 && xx < MW && yy >= 0 && yy < MH)
						ov[yy * MW + xx] = c;
				}
		}
	}
}
static void m_rtext(int x, int y, const char *s, uint16_t c) { m_text(x - (int)strlen(s) * F->cw, y, s, c); }

/* marca: RETRO (raio) VOLT em tamanho maior, igual ao menu principal */
static void m_logo(int x, int y)
{
	static const char *RAIO[] = {".....####", "....####.", "....###..", "...####..", "...###...", "..#######",
	                             "..######.", ".....###.", "....###..", "....##...", "...##....", "...#.....", "..#......"};
	int esc = F->ch <= 10 ? 2 : 1;
	const char *p1 = "RETRO", *p2 = "VOLT";
	uint16_t IV = rgb(240, 236, 225), OR = rgb(255, 140, 40), AM = rgb(255, 196, 40);
	for (int pass = 0; pass < 2; pass++) {
		const char *t = pass ? p2 : p1;
		int x0 = pass ? x + 5 * F->cw * esc + 7 : x; /* raio colado no O e no V */
		for (int n = 0; t[n]; n++) {
			int o = (unsigned char)t[n] - 32;
			for (int r = 0; r < F->ch; r++)
				for (int k = 0; k < F->cw; k++)
					if (F->b[o * F->ch + r] >> k & 1)
						m_rect(x0 + (n * F->cw + k) * esc, y + r * esc, esc, esc, pass ? OR : IV);
		}
	}
	int xr = x + 5 * F->cw * esc - 3, alt = F->ch * esc;
	for (int j = 0; j < 13; j++)
		for (int i = 0; RAIO[j][i]; i++)
			if (RAIO[j][i] == '#')
				m_rect(xr + i, y + j * alt / 13 + 1, 1, alt / 13 + 1, AM);
}

static void menu_fundo(void)
{
	/* ultimo quadro do jogo escurecido (25%), no mesmo lugar em que ele aparece na
	 * tela; em volta (laterais do LCD) fica o azul do menu */
	int gx = 0, gy = 0, gw = MW, gh = MH;
	if (!tubo && ult_x0 != -99999) {
		gx = (ult_x0 - m_ox) / m_esc;
		gy = (ult_y0 - m_oy) / m_esc;
		gw = ult_cw / m_esc;
		gh = ult_dh / m_esc;
	}
	for (int j = 0; j < MH; j++)
		for (int i = 0; i < MW; i++) {
			uint16_t p = NAV;
			int li = i - gx, lj = j - gy;
			if (ult_q && ult_w && ult_h && li >= 0 && lj >= 0 && li < gw && lj < gh) {
				int sx = li * ult_w / gw, sy = lj * ult_h / gh;
				const uint8_t *src = ult_q + (size_t)sy * ult_pitch;
				int bpp_antes = fb.bpp;
				fb.bpp = 16;
				p = px_conv(src, sx, ult_fmt);
				fb.bpp = bpp_antes;
				p = (p >> 2) & 0x39E7;
			}
			fundo[j * MW + i] = p;
		}
}

/* ------------------------------------------------------------------ leitora acordada */
/* parada num menu a leitora "dorme" e o jogo trava ao voltar. A cada 10 s sem
 * leitura, le 1 setor direto da leitora (fora do cache) para ela continuar girando. */
static void *cd_acorda_thread(void *x)
{
	(void)x;
	uint8_t b[SETOR];
	int n_falhas = 0;
	while (!g_sair) {
		sleep(2);
		if (agora() - cd_ult_leitura < 10)
			continue;
		/* le onde o jogo parou (dados ou audio, com o comando certo para cada um):
		 * no PC Engine a faixa 1 e audio e a leitura de dados falhava, a leitora dormia */
		pthread_mutex_lock(&cd_io_mx);
		int64_t l = cd_ult_lba;
		if (l < 0 || l * SETOR >= cd_tam)
			l = 16;
		bool ok = cd_le_setor(l, b);
		cd_ult_leitura = agora();
		pthread_mutex_unlock(&cd_io_mx);
		if (!ok && n_falhas++ < 5)
			LOG("cd: leitura para manter a leitora girando falhou (lba %lld)", (long long)l);
	}
	return NULL;
}

/* ------------------------------------------------------------------ PNG simples (miniaturas) */
static uint32_t crc_tab[256];
static uint32_t crc32_(const uint8_t *d, size_t n, uint32_t c)
{
	if (!crc_tab[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t k = i;
			for (int j = 0; j < 8; j++)
				k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1;
			crc_tab[i] = k;
		}
	c = ~c;
	for (size_t i = 0; i < n; i++)
		c = crc_tab[(c ^ d[i]) & 255] ^ (c >> 8);
	return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void png_bloco(FILE *f, const char *tipo, const uint8_t *d, uint32_t n)
{
	uint8_t h[8];
	be32(h, n);
	memcpy(h + 4, tipo, 4);
	fwrite(h, 1, 8, f);
	if (n)
		fwrite(d, 1, n, f);
	uint32_t c = crc32_((const uint8_t *)tipo, 4, 0);
	c = crc32_(d, n, c);
	be32(h, c);
	fwrite(h, 1, 4, f);
}
/* grava RGB (w*h*3) como PNG sem compressao (deflate "stored") */
static bool png_grava(const char *path, const uint8_t *rgb, int w, int h)
{
	size_t cru = (size_t)(w * 3 + 1) * h, nb = (cru + 65534) / 65535;
	size_t tz = 2 + cru + nb * 5 + 4;
	uint8_t *z = malloc(tz), *q = z;
	*q++ = 0x78; *q++ = 0x01;
	uint32_t a = 1, b2 = 0;
	uint8_t *linha = malloc(cru), *l = linha;
	for (int y = 0; y < h; y++) {
		*l++ = 0;
		memcpy(l, rgb + (size_t)y * w * 3, w * 3);
		l += w * 3;
	}
	for (size_t i = 0; i < cru; i++) {
		a = (a + linha[i]) % 65521;
		b2 = (b2 + a) % 65521;
	}
	for (size_t off = 0; off < cru; off += 65535) {
		size_t n = cru - off < 65535 ? cru - off : 65535;
		*q++ = off + n >= cru ? 1 : 0;
		*q++ = n; *q++ = n >> 8; *q++ = ~n; *q++ = (~n) >> 8;
		memcpy(q, linha + off, n);
		q += n;
	}
	be32(q, (b2 << 16) | a);
	q += 4;
	FILE *f = fopen(path, "wb");
	bool ok = f != NULL;
	if (f) {
		uint8_t ih[13];
		fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
		be32(ih, w); be32(ih + 4, h);
		ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
		png_bloco(f, "IHDR", ih, 13);
		png_bloco(f, "IDAT", z, q - z);
		png_bloco(f, "IEND", NULL, 0);
		fclose(f);
	}
	free(z);
	free(linha);
	return ok;
}
/* le de volta so os PNG gravados acima (sem compressao); devolve RGB ou NULL */
static uint8_t *png_le(const char *path, int *pw, int *ph)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc(n), *z = malloc(n);
	size_t nz = 0;
	int w = 0, h = 0;
	bool ok = fread(d, 1, n, f) == (size_t)n && n > 8 && !memcmp(d, "\x89PNG", 4);
	fclose(f);
	for (long p = 8; ok && p + 12 <= n;) {
		uint32_t len = (uint32_t)d[p] << 24 | d[p + 1] << 16 | d[p + 2] << 8 | d[p + 3];
		if (p + 12 + (long)len > n)
			break;
		if (!memcmp(d + p + 4, "IHDR", 4)) {
			w = d[p + 8] << 24 | d[p + 9] << 16 | d[p + 10] << 8 | d[p + 11];
			h = d[p + 12] << 24 | d[p + 13] << 16 | d[p + 14] << 8 | d[p + 15];
		} else if (!memcmp(d + p + 4, "IDAT", 4)) {
			memcpy(z + nz, d + p + 8, len);
			nz += len;
		}
		p += 12 + len;
	}
	uint8_t *rgb = NULL;
	if (ok && w > 0 && h > 0 && w <= 640 && h <= 480 && nz > 2) {
		size_t cru = (size_t)(w * 3 + 1) * h, got = 0;
		uint8_t *lin = malloc(cru);
		size_t p = 2;
		while (p + 5 <= nz && got < cru) {
			int fim = z[p] & 1;
			if ((z[p] >> 1) & 3) /* comprimido: nao e nosso */
				break;
			size_t len = z[p + 1] | z[p + 2] << 8;
			p += 5;
			if (p + len > nz || got + len > cru)
				break;
			memcpy(lin + got, z + p, len);
			got += len;
			p += len;
			if (fim)
				break;
		}
		if (got == cru) {
			rgb = malloc((size_t)w * h * 3);
			for (int y = 0; y < h; y++)
				memcpy(rgb + (size_t)y * w * 3, lin + (size_t)y * (w * 3 + 1) + 1, w * 3);
			*pw = w;
			*ph = h;
		}
		free(lin);
	}
	free(d);
	free(z);
	return rgb;
}

/* ------------------------------------------------------------------ save states (10 espacos) */
#define N_SLOTS 10
#define MINI_W 640
#define MINI_H 480
static struct { bool tem; char quando[24]; uint16_t mini[MINI_W * MINI_H]; int mw, mh; bool tem_mini; } slots[N_SLOTS];
static int pv_x, pv_y, pv_w, pv_h; /* caixa da previa (tela logica) */
static char slot_msg[48];
static double slot_msg_ate;

static void slot_path(int n, const char *ext, char *o, size_t t)
{
	snprintf(o, t, "%s/estado_%d.%s", a_states ? a_states : a_saves, n + 1, ext);
}

/* miniatura do ultimo quadro do jogo (antes do menu), sem nada por cima */
/* miniatura = o ultimo quadro do jogo NA RESOLUCAO DELE (ate 640x480), sem pular pontos;
 * a reducao (com media dos pontos) so acontece na hora de mostrar */
static void slot_mini_rgb(uint8_t *rgb, int *pw, int *ph)
{
	int mw2 = ult_w ? (int)ult_w : 320, mh2 = ult_h ? (int)ult_h : 240;
	if (mw2 > MINI_W) mw2 = MINI_W;
	if (mh2 > MINI_H) mh2 = MINI_H;
	*pw = mw2;
	*ph = mh2;
	for (int y = 0; y < mh2; y++)
		for (int x = 0; x < mw2; x++) {
			uint8_t *o = rgb + ((size_t)y * mw2 + x) * 3;
			o[0] = o[1] = o[2] = 0;
			if (!ult_q || !ult_w)
				continue;
			const uint8_t *src = ult_q + (size_t)(y * ult_h / mh2) * ult_pitch;
			int sx = x * ult_w / mw2, bb = fb.bpp;
			fb.bpp = 32;
			uint32_t p = px_conv(src, sx, ult_fmt);
			fb.bpp = bb;
			o[0] = p >> 16; o[1] = p >> 8; o[2] = p;
		}
}

static void slots_le(void)
{
	for (int i = 0; i < N_SLOTS; i++) {
		char p[4200];
		struct stat st;
		slot_path(i, "state", p, sizeof p);
		slots[i].tem = !stat(p, &st);
		slots[i].tem_mini = false;
		if (!slots[i].tem)
			continue;
		strftime(slots[i].quando, sizeof slots[i].quando, "%d/%m %H:%M", localtime(&st.st_mtime));
		slot_path(i, "png", p, sizeof p);
		int w, h;
		uint8_t *rgb = png_le(p, &w, &h);
		if (rgb) {      /* miniaturas antigas (160x120, 320x240) tambem servem */
			int mw2 = w > MINI_W ? MINI_W : w, mh2 = h > MINI_H ? MINI_H : h;
			for (int y = 0; y < mh2; y++)
				for (int x = 0; x < mw2; x++) {
					uint8_t *q = rgb + ((size_t)(y * h / mh2) * w + x * w / mw2) * 3;
					slots[i].mini[y * mw2 + x] = ((q[0] >> 3) << 11) | ((q[1] >> 2) << 5) | (q[2] >> 3);
				}
			slots[i].mw = mw2;
			slots[i].mh = mh2;
			slots[i].tem_mini = true;
			free(rgb);
		}
	}
}

static bool slot_salva(int n)
{
	size_t t = core.serialize_size();
	if (!t)
		return false;
	uint8_t *d = malloc(t);
	bool ok = core.serialize(d, t);
	char p[4200], tmp[4300];
	slot_path(n, "state", p, sizeof p);
	mkdir_pai(p);
	snprintf(tmp, sizeof tmp, "%s.tmp", p);
	FILE *f = ok ? fopen(tmp, "wb") : NULL;
	if (f) {
		ok = fwrite(d, 1, t, f) == t;
		fflush(f);
		fsync(fileno(f));
		fclose(f);
		if (ok)
			rename(tmp, p);
	} else
		ok = false;
	free(d);
	if (ok) {
		static uint8_t rgb[MINI_W * MINI_H * 3];
		int mw2, mh2;
		slot_mini_rgb(rgb, &mw2, &mh2);
		slot_path(n, "png", p, sizeof p);
		png_grava(p, rgb, mw2, mh2);
		sync();
	}
	LOG("estado: slot %d %s", n + 1, ok ? "salvo" : "ERRO ao salvar");
	return ok;
}

static bool slot_carrega(int n)
{
	char p[4200];
	slot_path(n, "state", p, sizeof p);
	FILE *f = fopen(p, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long t = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc(t);
	bool ok = fread(d, 1, t, f) == (size_t)t && core.unserialize(d, t);
	fclose(f);
	free(d);
	LOG("estado: slot %d %s", n + 1, ok ? "carregado" : "ERRO ao carregar");
	return ok;
}

/* ------------------------------------------------------------------ filtro por nucleo */
static void filtro_le(void)
{
	FILE *f = a_filtro_arq ? fopen(a_filtro_arq, "r") : NULL;
	int v;
	if (f && fscanf(f, "%d", &v) == 1 && v >= 0 && v < N_FILTROS) {
		filtro = v;
		LOG("filtro deste nucleo: %s", FILTROS[v]);
	}
	if (f)
		fclose(f);
}
static void filtro_grava(void)
{
	if (!a_filtro_arq)
		return;
	mkdir_pai(a_filtro_arq);
	FILE *f = fopen(a_filtro_arq, "w");
	if (f) {
		fprintf(f, "%d\n# RETROVOLT: filtro deste nucleo (0 nenhum, 1 scanlines, 2 scanlines leves, 3 CRT)\n", filtro);
		fclose(f);
		sync();
	}
}

static void imagem_le(void)
{
	FILE *f = a_imagem_arq ? fopen(a_imagem_arq, "r") : NULL;
	int v;
	if (f && fscanf(f, "%d", &v) == 1)
		imagem = v ? 1 : 0;
	if (f)
		fclose(f);
}
static void imagem_grava(void)
{
	if (!a_imagem_arq)
		return;
	mkdir_pai(a_imagem_arq);
	FILE *f = fopen(a_imagem_arq, "w");
	if (f) {
		fprintf(f, "%d\n# RETROVOLT: imagem deste nucleo no LCD (0 ORIGINAL nitida, 1 TELA CHEIA)\n", imagem);
		fclose(f);
		sync();
	}
}

/* ------------------------------------------------------------------ convergencia por nucleo */
static void conv_le(void)
{
	FILE *f = a_conv_arq ? fopen(a_conv_arq, "r") : NULL;
	if (!f)
		return;
	int x, y;
	double w;
	if (fscanf(f, "%d %d %lf", &x, &y, &w) == 3) {
		conv_x = x;
		conv_y = y;
		conv_w = w;
		LOG("convergencia deste nucleo: %d %d %.3f", x, y, w);
	}
	fclose(f);
}
static void conv_grava(void)
{
	if (!a_conv_arq)
		return;
	mkdir_pai(a_conv_arq);
	FILE *f = fopen(a_conv_arq, "w");
	if (f) {
		fprintf(f, "%d %d %.3f\n# RETROVOLT: posicao x, posicao y, largura (so deste nucleo)\n", conv_x, conv_y, conv_w);
		fclose(f);
		sync();
	}
}

/* texto direto no framebuffer, em coordenadas 320x240 da imagem do jogo */
static void fb_texto(int lx, int ly, const char *t, int r, int g, int b, bool caixa)
{
	int sx = ult_cw / 320 > 0 ? ult_cw / 320 : 1;
	int sy = ult_dh / 240 > 0 ? ult_dh / 240 : 1;
	int n = strlen(t), X = ult_x0 + lx * ult_cw / 320, Y = ult_y0 + ly * sy;
	if (lx < 0) /* centralizado */
		X = ult_x0 + (ult_cw - n * F->cw * sx) / 2;
	if (caixa)
		for (int y = Y - 2 * sy; y < Y + (F->ch + 2) * sy; y++)
			for (int x = X - 4 * sx; x < X + (n * F->cw + 4) * sx; x++)
				fb_ponto(x, y, 12, 18, 40);
	for (int k = 0; k < n; k++) {
		int o = (unsigned char)t[k] - 32;
		if (o < 0 || o > 223)
			continue;
		for (int rr = 0; rr < F->ch; rr++) {
			uint8_t bits = F->b[o * F->ch + rr];
			for (int c = 0; c < F->cw; c++)
				if (bits >> c & 1)
					for (int yy = 0; yy < sy; yy++)
						for (int xx = 0; xx < sx; xx++)
							fb_ponto(X + (k * F->cw + c) * sx + xx, Y + rr * sy + yy, r, g, b);
		}
	}
}

static void fb_retangulo(int x, int y, int w, int h, int r, int g, int b)
{
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			fb_ponto(i, j, r, g, b);
}

/* convergencia no jogo: a imagem do jogo com moldura e cantos laranja */
static void conv_desenha(void)
{
	if (ult_q)
		desenha(ult_q, ult_w, ult_h, ult_pitch, ult_fmt, av.geometry.aspect_ratio);
	int sx = ult_cw / 320 > 0 ? ult_cw / 320 : 1, sy = ult_dh / 240 > 0 ? ult_dh / 240 : 1;
	int x0 = ult_x0, y0 = ult_y0, w = ult_cw, h = ult_dh;
	fb_retangulo(x0, y0, w, 2 * sy, 255, 140, 40);
	fb_retangulo(x0, y0 + h - 2 * sy, w, 2 * sy, 255, 140, 40);
	fb_retangulo(x0, y0, sx, h, 255, 140, 40);
	fb_retangulo(x0 + w - sx, y0, sx, h, 255, 140, 40);
	int cx = 24 * sx, cy = 6 * sy, lx = 6 * sx, ly = 24 * sy;
	fb_retangulo(x0, y0, cx, cy, 255, 140, 40);
	fb_retangulo(x0 + w - cx, y0, cx, cy, 255, 140, 40);
	fb_retangulo(x0, y0 + h - cy, cx, cy, 255, 140, 40);
	fb_retangulo(x0 + w - cx, y0 + h - cy, cx, cy, 255, 140, 40);
	fb_retangulo(x0, y0, lx, ly, 255, 140, 40);
	fb_retangulo(x0 + w - lx, y0, lx, ly, 255, 140, 40);
	fb_retangulo(x0, y0 + h - ly, lx, ly, 255, 140, 40);
	fb_retangulo(x0 + w - lx, y0 + h - ly, lx, ly, 255, 140, 40);
	char l[64];
	fb_texto(-1, 70, "CONVERGENCIA DESTE NUCLEO", 255, 140, 40, true);
	snprintf(l, sizeof l, "X %+d  Y %+d  LARGURA %d%%", conv_x, conv_y, (int)lround(conv_w * 100));
	fb_texto(-1, 70 + F->ch + 8, l, 240, 236, 225, true);
	fb_texto(-1, 70 + 2 * (F->ch + 8), "SETAS POSICAO  LB/RB LARGURA", 120, 130, 160, true);
	fb_texto(-1, 70 + 3 * (F->ch + 8), "SELECT IGUAL AO MENU", 120, 130, 160, true);
	fb_texto(-1, 70 + 4 * (F->ch + 8), "A SALVA  B VOLTA", 240, 236, 225, true);
}

/* ponto (x, y) da miniatura reduzida/ampliada para dw x dh: media dos pontos quando reduz */
static void mini_ponto(int sl, int x, int y, int dw, int dh, int *r, int *g, int *b)
{
	int sw = slots[sl].mw, sh = slots[sl].mh;
	int x0 = x * sw / dw, x1 = (x + 1) * sw / dw, y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
	if (x1 <= x0) x1 = x0 + 1;
	if (y1 <= y0) y1 = y0 + 1;
	int sr = 0, sg = 0, sb = 0, n = 0;
	for (int yy = y0; yy < y1 && yy < sh; yy++)
		for (int xx = x0; xx < x1 && xx < sw; xx++) {
			uint16_t p = slots[sl].mini[yy * sw + xx];
			sr += (p >> 11) << 3;
			sg += ((p >> 5) & 63) << 2;
			sb += (p & 31) << 3;
			n++;
		}
	if (!n)
		n = 1;
	*r = sr / n; *g = sg / n; *b = sb / n;
}

static void slot_previa_nitida(void)
{
	/* com a pergunta "SUBSTITUIR?" aberta, a previa nitida nao e desenhada (ficaria por cima) */
	if (tubo || m_esc < 2 || !slots[slot_sel].tem_mini || sub == 6)
		return;
	int X = m_ox + pv_x * m_esc, Y = m_oy + pv_y * m_esc, w = pv_w * m_esc, h = pv_h * m_esc;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int r, g, b;
			mini_ponto(slot_sel, x, y, w, h, &r, &g, &b);
			fb_ponto(X + x, Y + y, r, g, b);
		}
}

static void m_item(int y, const char *txt, bool on, const char *valor, uint16_t cor_valor)
{
	uint16_t IVORY = rgb(240, 236, 225), ORANGE = rgb(255, 140, 40), SLATE = rgb(120, 130, 160);
	m_text(24, y, txt, on ? IVORY : SLATE);
	if (on) {
		int w = strlen(txt) * F->cw;
		m_rect(16, y - 1, 3, F->ch + 2, ORANGE);
		m_rect(24, y + F->ch + 2, w, 1, ORANGE);
		if (!valor)
			m_text(24 + w + F->cw, y, "->", ORANGE);
	}
	if (valor)
		m_rtext(MW - 14, y, valor, on ? ORANGE : cor_valor);
}

static void maius(char *d, const char *s, int max)
{
	int i = 0;
	for (; s[i] && i < max - 1; i++)
		d[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];
	d[i] = 0;
}

static void menu_desenha(void)
{
	uint16_t IVORY = rgb(240, 236, 225), ORANGE = rgb(255, 140, 40), SLATE = rgb(120, 130, 160);
	memcpy(ov, fundo, sizeof ov);
	m_rect(0, 0, 6, MH, ORANGE);
	m_rect(MW - 6, 0, 6, MH, ORANGE);
	m_logo(16, 3);
	char hora[8];
	time_t t = time(NULL);
	strftime(hora, sizeof hora, "%H:%M", localtime(&t));
	m_rtext(MW - 12, 3 + (F->ch <= 10 ? F->ch / 2 : 0), hora, IVORY);
	if (sub == 7) {
		conv_desenha();
		return;
	}
	const char *titulo = sub == 0 ? "JOGO" : sub == 1 ? "OPCOES DO NUCLEO" : sub >= 5 ? (slot_modo ? "CARREGAR ESTADO" : "SALVAR ESTADO") : "CONTROLES";
	if (sub == 4 && ass_pos >= n_ordem)
		ass_pos = 0;
	m_text(16, 30, titulo, IVORY);
	m_rect(16, 30 + F->ch + 2, strlen(titulo) * F->cw, 2, ORANGE);
	int passo = F->ch + 7, y0 = 52 + F->ch;
	int cabem = (MH - 12 - F->ch - y0) / passo;
	if (sub == 0) {
		m_text(16, 50, a_nome, SLATE);
		if (agora() < slot_msg_ate)
			m_rtext(MW - 12, 50, slot_msg, rgb(90, 220, 140));
		int y = 66, visiveis = 0;
		for (int i = 0; i < N_MENU; i++)
			visiveis += !menu_oculto(i);
		int passo_m = (MH - 20 - F->ch - y) / visiveis;
		if (passo_m > F->ch + 6) passo_m = F->ch + 6;
		for (int i = 0; i < N_MENU; i++) {
			if (menu_oculto(i))
				continue;
			bool on = i == menu_sel;
			if (i == M_VOLUME) {
				char nv[16];
				snprintf(nv, sizeof nv, on ? "< %d >" : "%d", volume);
				int bx = 24 + 18 * F->cw, bw = 90;
				m_item(y, MENU[i], on, nv, SLATE);
				m_rect(bx, y + F->ch / 2 - 1, bw, 3, rgb(40, 50, 80));
				m_rect(bx, y + F->ch / 2 - 1, bw * volume / 100, 3, on ? ORANGE : SLATE);
			} else if (i == M_IMAGEM) {
				char ni[24];
				snprintf(ni, sizeof ni, tubo ? "SO NO LCD" : on ? "< %s >" : "%s", imagem ? "TELA CHEIA" : "ORIGINAL");
				m_item(y, MENU[i], on, ni, SLATE);
			} else if (i == M_FILTRO) {
				char nf[24];
				if (tubo)
					snprintf(nf, sizeof nf, "SO NO LCD");
				else
					snprintf(nf, sizeof nf, on ? "< %s >" : "%s", FILTROS[filtro]);
				m_item(y, MENU[i], on, nf, SLATE);
			} else
				m_item(y, MENU[i], on, NULL, SLATE);
			y += passo_m;
		}
		m_text(16, MH - 4 - F->ch, "START+SELECT VOLTA", SLATE);
		m_rtext(MW - 12, MH - 4 - F->ch, "A ESCOLHE", SLATE);
	} else if (sub == 5 || sub == 6) {
		/* lista dos 10 espacos a esquerda; miniatura do selecionado a direita */
		for (int i = 0; i < N_SLOTS; i++) {
			char l[16];
			snprintf(l, sizeof l, "SLOT %d", i + 1);
			int y = y0 + i * (F->ch + 5);
			bool on = i == slot_sel;
			m_text(24, y, l, on ? IVORY : SLATE);
			m_text(24 + 8 * F->cw, y, slots[i].tem ? slots[i].quando : "VAZIO", on ? ORANGE : SLATE);
			if (on) {
				m_rect(16, y - 1, 3, F->ch + 2, ORANGE);
				m_rect(24, y + F->ch + 1, strlen(l) * F->cw, 1, ORANGE);
			}
		}
		/* previa grande: o que sobra a direita da lista, em 4:3 */
		int col = 24 + 20 * F->cw + 12;
		pv_w = MW - 14 - col;
		int maxh = MH - y0 - 16 - F->ch;
		if (pv_w * 3 / 4 > maxh)
			pv_w = maxh * 4 / 3;
		pv_h = pv_w * 3 / 4;
		pv_x = MW - 14 - pv_w;
		pv_y = y0 + F->ch + 6;
		if (pv_y + pv_h > MH - 14 - F->ch) {
			pv_h = MH - 14 - F->ch - pv_y;
			pv_w = pv_h * 4 / 3;
			pv_x = MW - 14 - pv_w;
		}
		m_text(pv_x, y0, slots[slot_sel].tem ? slots[slot_sel].quando : "VAZIO",
		       slots[slot_sel].tem ? IVORY : SLATE);
		m_rect(pv_x - 2, pv_y - 2, pv_w + 4, pv_h + 4, slots[slot_sel].tem ? ORANGE : rgb(40, 50, 80));
		m_rect(pv_x, pv_y, pv_w, pv_h, rgb(12, 18, 40));
		if (slots[slot_sel].tem_mini) {
			for (int y = 0; y < pv_h; y++)
				for (int x = 0; x < pv_w; x++) {
					int r, g, b;
					mini_ponto(slot_sel, x, y, pv_w, pv_h, &r, &g, &b);
					ov[(pv_y + y) * MW + pv_x + x] = rgb(r, g, b);
				}
		} else
			m_text(pv_x + (pv_w - 5 * F->cw) / 2, pv_y + pv_h / 2 - F->ch / 2, "VAZIO", SLATE);
		m_text(16, MH - 4 - F->ch, slot_modo ? "A CARREGA  B VOLTA" : "A SALVA  B VOLTA", SLATE);
		if (sub == 6) {
			char l1[40];
			snprintf(l1, sizeof l1, "SUBSTITUIR O SLOT %d?", slot_sel + 1);
			int w = 26 * F->cw, bx = (MW - w) / 2 - 6, by = MH / 2 - F->ch - 10, bh = 2 * F->ch + 22;
			m_rect(bx, by, w + 12, bh, rgb(255, 255, 255));            /* contorno branco */
			m_rect(bx + 1, by + 1, w + 10, bh - 2, rgb(24, 44, 110));  /* fundo azul */
			m_text((MW - (int)strlen(l1) * F->cw) / 2, by + 8, l1, IVORY);
			m_text((MW - 13 * F->cw) / 2, by + 12 + F->ch, "A SIM   B NAO", IVORY);
		}
	} else if (sub == 1) {
		if (!nvars)
			m_text(24, y0, "ESTE NUCLEO NAO TEM OPCOES", SLATE);
		if (sub_sel < sub_ini) sub_ini = sub_sel;
		if (sub_sel >= sub_ini + cabem) sub_ini = sub_sel - cabem + 1;
		for (int k = 0; k < cabem && sub_ini + k < nvars; k++) {
			int i = sub_ini + k;
			char d[48], v[24];
			maius(d, vars[i].desc ? vars[i].desc : vars[i].k, (MW - 40) / F->cw - 12);
			maius(v, vars[i].v, 12);
			char vv[32];
			snprintf(vv, sizeof vv, i == sub_sel ? "<%s>" : "%s", v);
			m_item(y0 + k * passo, d, i == sub_sel, vv, SLATE);
		}
		m_text(16, MH - 4 - F->ch, "<- -> MUDA  B VOLTA", SLATE);
		char pos[16];
		snprintf(pos, sizeof pos, "%d/%d", sub_sel + 1, nvars);
		m_rtext(MW - 12, MH - 4 - F->ch, pos, SLATE);
	} else if (sub == 4) {
		int id = bot_ordem[ass_pos];
		char d[40], l[48];
		maius(d, bot_desc[id] ? bot_desc[id] : NOME_FIS[id], 24);
		snprintf(l, sizeof l, "%d DE %d", ass_pos + 1, n_ordem);
		m_text(24, y0, l, SLATE);
		m_text(24, y0 + 2 * passo, "APERTE O BOTAO PARA:", IVORY);
		m_text(24, y0 + 3 * passo, d, ORANGE);
		snprintf(l, sizeof l, "AGORA: %s", NOME_FIS[remap[id]]);
		m_text(24, y0 + 5 * passo, l, SLATE);
		m_text(16, MH - 4 - F->ch, "START+SELECT CANCELA", SLATE);
	} else {
		/* CONFIGURAR UM POR UM + 16 botoes do nucleo + PADRAO */
		int n = 18;
		if (sub_sel < sub_ini) sub_ini = sub_sel;
		if (sub_sel >= sub_ini + cabem) sub_ini = sub_sel - cabem + 1;
		for (int k = 0; k < cabem && sub_ini + k < n; k++) {
			int i = sub_ini + k - 1;
			if (i == -1) {
				m_item(y0 + k * passo, "CONFIGURAR UM POR UM", sub_sel == 0, NULL, SLATE);
				continue;
			}
			if (i == 16) {
				m_item(y0 + k * passo, "VOLTAR AO PADRAO", i + 1 == sub_sel, NULL, SLATE);
				continue;
			}
			char d[40];
			maius(d, bot_desc[i] ? bot_desc[i] : NOME_FIS[i], 20);
			const char *val = (sub == 3 && i + 1 == sub_sel) ? "APERTE..." : NOME_FIS[remap[i]];
			m_item(y0 + k * passo, d, i + 1 == sub_sel, val, remap[i] != i ? ORANGE : SLATE);
		}
		m_text(16, MH - 4 - F->ch, sub == 3 ? "APERTE O BOTAO NOVO" : "A TROCA  B VOLTA", SLATE);
	}
	sem_filtro = true;
	desenha(ov, MW, MH, MW * 2, RETRO_PIXEL_FORMAT_RGB565, (double)MW / MH);
	sem_filtro = false;
	if (sub == 5 || sub == 6)
		slot_previa_nitida();
}

/* um passo do menu do jogo: -1 = continua no menu; senao M_CONTINUAR / M_REINICIAR / M_SAIR */
static int menu_passo(uint32_t b, uint32_t novo, bool combo)
{
	const uint32_t OK = (1u << B_B) | (1u << B_START), VOLTA = 1u << B_A;
	/* repeticao ao segurar (cima/baixo/esquerda/direita) */
	static double prox;
	static uint32_t seg;
	uint32_t dirs = b & ((1u << B_UP) | (1u << B_DOWN) | (1u << B_LEFT) | (1u << B_RIGHT));
	uint32_t rep = novo & dirs;
	if (rep) {
		seg = rep;
		prox = agora() + 0.4;
	} else if (dirs && (dirs & seg) && agora() >= prox) {
		rep = dirs & seg;
		prox = agora() + 0.08;
	}
	if (sub == 7) { /* convergencia deste nucleo, com o jogo atras */
		if (combo || (novo & VOLTA)) {
			conv_x = cv_x; conv_y = cv_y; conv_w = cv_w;
			ult_x0 = -99999;
			sub = 0;
			return combo ? M_CONTINUAR : -1;
		}
		int passo_x = tubo ? 6 : 2;
		if (rep & (1u << B_LEFT)) conv_x -= passo_x;
		if (rep & (1u << B_RIGHT)) conv_x += passo_x;
		if (rep & (1u << B_UP)) conv_y -= 1;
		if (rep & (1u << B_DOWN)) conv_y += 1;
		if (novo & (1u << B_L)) conv_w -= 0.005;
		if (novo & (1u << B_R)) conv_w += 0.005;
		if (novo & (1u << B_SELECT)) {
			conv_x = conv_menu_x; conv_y = conv_menu_y; conv_w = conv_menu_w;
		}
		conv_x = conv_x < -300 ? -300 : conv_x > 300 ? 300 : conv_x;
		conv_y = conv_y < -20 ? -20 : conv_y > 20 ? 20 : conv_y;
		conv_w = conv_w < 0.70 ? 0.70 : conv_w > 1.10 ? 1.10 : conv_w;
		if (novo & (1u << B_B)) {
			conv_grava();
			snprintf(slot_msg, sizeof slot_msg, "CONVERGENCIA SALVA");
			slot_msg_ate = agora() + 2.5;
			ult_x0 = -99999;
			sub = 0;
		}
		return -1;
	}
	if (sub == 6) { /* substituir o espaco usado? */
		if (novo & (1u << B_B)) {
			bool ok = slot_salva(slot_sel);
			snprintf(slot_msg, sizeof slot_msg, ok ? "SALVO NO SLOT %d" : "ERRO AO SALVAR", slot_sel + 1);
			slot_msg_ate = agora() + 2.5;
			sub = 0;
		} else if (novo & VOLTA)
			sub = 5;
		return -1;
	}
	if (sub == 5) {
		if (combo)
			return M_CONTINUAR;
		if (novo & VOLTA) {
			sub = 0;
			return -1;
		}
		if (rep & (1u << B_UP))
			slot_sel = (slot_sel + N_SLOTS - 1) % N_SLOTS;
		if (rep & (1u << B_DOWN))
			slot_sel = (slot_sel + 1) % N_SLOTS;
		if (novo & OK) {
			if (!slot_modo) {
				if (slots[slot_sel].tem) {
					sub = 6;
					return -1;
				}
				bool ok = slot_salva(slot_sel);
				snprintf(slot_msg, sizeof slot_msg, ok ? "SALVO NO SLOT %d" : "ERRO AO SALVAR", slot_sel + 1);
				slot_msg_ate = agora() + 2.5;
				sub = 0;
			} else if (slots[slot_sel].tem) {
				bool ok = slot_carrega(slot_sel);
				sub = 0;
				if (ok)
					return M_CONTINUAR;
				snprintf(slot_msg, sizeof slot_msg, "ERRO AO CARREGAR");
				slot_msg_ate = agora() + 2.5;
			}
		}
		return -1;
	}
	if (sub == 4) { /* assistente: um botao de cada vez, na ordem do nucleo */
		if (combo) {
			sub = 2;
			return -1;
		}
		uint32_t m = novo & 0xffff;
		for (int f = 0; m && f < 16; f++)
			if (m >> f & 1) {
				remap[bot_ordem[ass_pos]] = f;
				sub_mudou = true;
				if (++ass_pos >= n_ordem) {
					botoes_grava();
					sub_mudou = false;
					sub = 2;
				}
				break;
			}
		return -1;
	}
	if (sub == 3) { /* esperando o botao novo para o item sub_sel (botao sub_sel - 1) */
		uint32_t m = novo & 0xffff;
		if (m && !combo) {
			for (int f = 0; f < 16; f++)
				if (m >> f & 1) {
					remap[sub_sel - 1] = f;
					sub_mudou = true;
					LOG("controles: %s -> %s", bot_desc[sub_sel - 1] ? bot_desc[sub_sel - 1] : NOME_FIS[sub_sel - 1], NOME_FIS[f]);
					break;
				}
			sub = 2;
		}
		return -1;
	}
	if (combo) {
		if (sub == 1 && sub_mudou) opcoes_grava();
		if (sub == 2 && sub_mudou) botoes_grava();
		sub = 0;
		return M_CONTINUAR;
	}
	if (sub == 0) {
		if (novo & VOLTA)
			return M_CONTINUAR;
		if (rep & (1u << B_UP))
			do
				menu_sel = (menu_sel + N_MENU - 1) % N_MENU;
			while (menu_oculto(menu_sel));
		if (rep & (1u << B_DOWN))
			do
				menu_sel = (menu_sel + 1) % N_MENU;
			while (menu_oculto(menu_sel));
		if (menu_sel == M_IMAGEM && !tubo && (novo & ((1u << B_LEFT) | (1u << B_RIGHT)))) {
			imagem = !imagem;
			imagem_grava();
			LOG("imagem: %s", imagem ? "TELA CHEIA" : "ORIGINAL");
		}
		if (menu_sel == M_FILTRO && !tubo && (novo & ((1u << B_LEFT) | (1u << B_RIGHT)))) {
			filtro = (filtro + ((novo & (1u << B_LEFT)) ? N_FILTROS - 1 : 1)) % N_FILTROS;
			filtro_grava();
			LOG("filtro: %s", FILTROS[filtro]);
		}
		if (menu_sel == M_VOLUME && (rep & ((1u << B_LEFT) | (1u << B_RIGHT)))) {
			int dv = (rep & (1u << B_LEFT)) ? -5 : 5;
			volume = volume + dv < 0 ? 0 : volume + dv > 100 ? 100 : volume + dv;
			volume_aplica();
		}
		if (novo & OK) {
			if (menu_sel == M_SALVAR || menu_sel == M_CARREGAR) {
				slot_modo = menu_sel == M_CARREGAR;
				slots_le();
				sub = 5;
				return -1;
			}
			if (menu_sel == M_CONV) {
				cv_x = conv_x; cv_y = conv_y; cv_w = conv_w;
				sub = 7;
				ult_x0 = -99999;
				return -1;
			}
			if (menu_sel == M_OPCOES || menu_sel == M_CONTROLES) {
				sub = menu_sel == M_OPCOES ? 1 : 2;
				sub_sel = sub_ini = 0;
				sub_mudou = false;
				return -1;
			}
			if (menu_sel == M_CONTINUAR || menu_sel == M_REINICIAR || menu_sel == M_SAIR || menu_sel == M_LISTA)
				return menu_sel;
		}
		return -1;
	}
	int n = sub == 1 ? nvars : 18;
	if (novo & VOLTA) {
		if (sub == 1 && sub_mudou) opcoes_grava();
		if (sub == 2 && sub_mudou) botoes_grava();
		sub = 0;
		return -1;
	}
	if (n && (rep & (1u << B_UP)))
		sub_sel = (sub_sel + n - 1) % n;
	if (n && (rep & (1u << B_DOWN)))
		sub_sel = (sub_sel + 1) % n;
	if (sub == 1 && nvars && (rep & ((1u << B_LEFT) | (1u << B_RIGHT))) && vars[sub_sel].nvals) {
		int k = 0, nv = vars[sub_sel].nvals;
		for (int i = 0; i < nv; i++)
			if (!strcmp(vars[sub_sel].vals[i], vars[sub_sel].v))
				k = i;
		k = (k + ((rep & (1u << B_LEFT)) ? nv - 1 : 1)) % nv;
		free(vars[sub_sel].v);
		vars[sub_sel].v = strdup(vars[sub_sel].vals[k]);
		vars_mudou = true;
		sub_mudou = true;
		LOG("opcao: %s = %s", vars[sub_sel].k, vars[sub_sel].v);
	}
	if (sub == 2 && (novo & OK)) {
		if (sub_sel == 0) {
			if (!n_ordem) /* nucleo nao descreveu os botoes: todos os 16 */
				for (int i = 0; i < 16; i++)
					bot_ordem[n_ordem++] = i;
			ass_pos = 0;
			sub = 4;
		} else if (sub_sel == 17) {
			for (int i = 0; i < 16; i++)
				remap[i] = i;
			sub_mudou = true;
		} else
			sub = 3;
	}
	return -1;
}

/* ------------------------------------------------------------------ principal */
static void *simbolo(const char *n)
{
	void *p = dlsym(core.h, n);
	if (!p) {
		LOG("ERRO: nucleo sem %s", n);
		exit(3);
	}
	return p;
}

static void ao_sinal(int s) { (void)s; g_sair = 1; }

/* CPU no maximo durante o jogo (o "ondemand" demora a subir o clock e o quadro atrasa) */
static void governador(bool jogo)
{
	static char antes[32];
	char p[96];
	for (int c = 0; c < 8; c++) {
		snprintf(p, sizeof p, "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor", c);
		FILE *f = fopen(p, jogo ? "r+" : "w");
		if (!f)
			continue;
		if (jogo) {
			if (!antes[0] && fgets(antes, sizeof antes, f))
				antes[strcspn(antes, "\n")] = 0;
			rewind(f);
			fputs("performance", f);
			LOG("cpu: policy%d %s -> performance", c, antes);
		} else if (antes[0])
			fputs(antes, f);
		fclose(f);
	}
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 1; i + 1 < argc; i += 2) {
		const char *k = argv[i], *v = argv[i + 1];
		if (!strcmp(k, "--nucleo")) a_nucleo = v;
		else if (!strcmp(k, "--jogo")) a_jogo = v;
		else if (!strcmp(k, "--cd")) a_cd = v;
		else if (!strcmp(k, "--bios")) a_bios = v;
		else if (!strcmp(k, "--saves")) a_saves = v;
		else if (!strcmp(k, "--nome")) a_nome = v;
		else if (!strcmp(k, "--save")) a_save = v;
		else if (!strcmp(k, "--botoes")) a_botoes = v;
		else if (!strcmp(k, "--states")) a_states = v;
		else if (!strcmp(k, "--conv-arq")) a_conv_arq = v;
		else if (!strcmp(k, "--filtro")) filtro = atoi(v);
		else if (!strcmp(k, "--filtro-arq")) a_filtro_arq = v;
		else if (!strcmp(k, "--imagem-arq")) a_imagem_arq = v;
		else if (!strcmp(k, "--cd-vel")) cd_vel = atoi(v);
		else if (!strcmp(k, "--volume")) volume = atoi(v) < 0 ? 0 : atoi(v) > 100 ? 100 : atoi(v);
		else if (!strcmp(k, "--audio")) a_audio = v;
		else if (!strcmp(k, "--opcoes")) a_opcoes = v;
		else if (!strcmp(k, "--fonte")) a_fonte = v;
		else if (!strcmp(k, "--conv")) sscanf(v, "%d,%d,%lf,%lf", &conv_x, &conv_y, &conv_w, &conv_h);
	}
	if (!a_nucleo || (!a_jogo && !a_cd)) {
		fprintf(stderr, "uso: rv-jogo --nucleo X.so (--jogo ARQ | --cd /dev/sr0) [--bios D] [--saves D] [--nome N]\n");
		return 2;
	}
	F = &FONTES[0];
	for (int i = 0; i < N_FONTES; i++)
		if (!strcasecmp(FONTES[i].nome, a_fonte))
			F = &FONTES[i];
	signal(SIGTERM, ao_sinal);
	signal(SIGINT, ao_sinal);
	mkdir(a_saves, 0755);

	if (a_cd) {
		if (!cd_abrir(a_cd))
			return 2;
		a_jogo = CD_CUE;
	}

	core.h = dlopen(a_nucleo, RTLD_NOW | RTLD_LOCAL);
	if (!core.h) {
		LOG("ERRO: %s", dlerror());
		return 3;
	}
	core.set_environment = simbolo("retro_set_environment");
	core.set_video_refresh = simbolo("retro_set_video_refresh");
	core.set_audio_sample = simbolo("retro_set_audio_sample");
	core.set_audio_sample_batch = simbolo("retro_set_audio_sample_batch");
	core.set_input_poll = simbolo("retro_set_input_poll");
	core.set_input_state = simbolo("retro_set_input_state");
	core.init = simbolo("retro_init");
	core.deinit = simbolo("retro_deinit");
	core.get_system_info = simbolo("retro_get_system_info");
	core.get_system_av_info = simbolo("retro_get_system_av_info");
	core.set_controller_port_device = simbolo("retro_set_controller_port_device");
	core.load_game = simbolo("retro_load_game");
	core.unload_game = simbolo("retro_unload_game");
	core.run = simbolo("retro_run");
	core.reset = simbolo("retro_reset");
	core.get_memory_data = simbolo("retro_get_memory_data");
	core.get_memory_size = simbolo("retro_get_memory_size");
	core.serialize_size = simbolo("retro_serialize_size");
	core.serialize = simbolo("retro_serialize");
	core.unserialize = simbolo("retro_unserialize");

	var_set("pcsx_rearmed_cd_readahead", "128", false); /* le o CD adiantado: menos espera */
	opcoes_ler();
	core.set_environment(environment);
	core.set_video_refresh(video_cb);
	core.set_audio_sample(audio_sample);
	core.set_audio_sample_batch(audio_batch);
	core.set_input_poll(input_poll);
	core.set_input_state(input_state);
	core.init();

	struct retro_system_info si = {0};
	core.get_system_info(&si);
	LOG("nucleo: %s %s; jogo: %s", si.library_name, si.library_version, a_jogo);
	struct retro_game_info gi = {.path = a_jogo};
	void *buf = NULL;
	if (!si.need_fullpath) {
		FILE *f = fopen(a_jogo, "rb");
		if (!f) {
			LOG("ERRO: nao abriu %s", a_jogo);
			return 2;
		}
		fseek(f, 0, SEEK_END);
		gi.size = ftell(f);
		fseek(f, 0, SEEK_SET);
		buf = malloc(gi.size);
		if (fread(buf, 1, gi.size, f) != gi.size)
			LOG("AVISO: leitura curta");
		fclose(f);
		gi.data = buf;
	}
	fb_abrir();
	menu_tamanho();
	if (!core.load_game(&gi)) {
		LOG("ERRO: o nucleo nao abriu o jogo");
		return 2;
	}
	core.get_system_av_info(&av);
	LOG("jogo: %ux%u %.3f Hz, som %.0f Hz", av.geometry.base_width, av.geometry.base_height, av.timing.fps, av.timing.sample_rate);
	core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
	core.set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
	sram_carrega();
	botoes_ler();
	conv_menu_x = conv_x; conv_menu_y = conv_y; conv_menu_w = conv_w;
	conv_le();
	filtro_le();
	imagem_le();
	if (filtro < 0 || filtro >= N_FILTROS)
		filtro = 0;
	if (cd_fd >= 0 && !cd_arquivo) {
		pthread_t ta;
		cd_ult_leitura = agora();
		pthread_create(&ta, NULL, cd_acorda_thread, NULL);
	}
	if (cd_fd >= 0) {
		pthread_t tp;
		pthread_create(&tp, NULL, cd_adiante_thread, NULL);
	}
	volume_aplica();
	volume_ini = volume;
	som_abrir(av.timing.sample_rate);

	/* ritmo: 1 quadro do jogo por vsync quando as frequencias batem (<0,6%);
	 * senao (ex.: jogo PAL 50 Hz numa tela 60 Hz) acumula o tempo */
	double tela_hz = vs_escolhe();
	vs_medindo = false;
	fb.sem_vsync = vs_tipo == 0;
	if (tela_hz <= 0)
		tela_hz = tubo ? 60.005 : 60.0;
	bool um_pra_um = !fb.sem_vsync && fabs(av.timing.fps - tela_hz) / tela_hz < 0.006;
	LOG("tela %.3f Hz; %s", tela_hz, um_pra_um ? "1 quadro por vsync" : "ritmo pelo tempo");

	controles_busca();
	if (getenv("RV_TESTE_CTL")) { /* teste: aviso de controle */
		snprintf(osd_pend, sizeof osd_pend, "CONTROLE 1: %s", getenv("RV_TESTE_CTL"));
		osd_tem_pend = true;
	}
	pthread_t tb;
	pthread_create(&tb, NULL, busca_thread, NULL);
	governador(true);

	int max_q = getenv("RV_FRAMES") ? atoi(getenv("RV_FRAMES")) : 0;
	long quadros = 0;
	bool menu = false;
	uint32_t ant = 0;
	double acc = 0, t_prox = agora();
	int ret = 0;
	while (!g_sair) {
		double t_loop0 = agora();
		controles_le();
		uint32_t b = pad_bot[0] | pad_bot[1];
		uint32_t novo = b & ~ant;
		bool combo = ((b & (1u << B_START)) && (b & (1u << B_SELECT)) &&
		              (novo & ((1u << B_START) | (1u << B_SELECT)))) || (novo & (1u << B_MENU));
		ant = b;
		if (entrada_travada && !(b & 0xffff))
			entrada_travada = false;
		if (!menu && combo) {
			menu = true;
			menu_sel = 0;
			sub = 0;
			som_mudo = true;
			menu_fundo();
			LOG("menu aberto");
			sram_grava(false);
		} else if (menu) {
			int escolha = menu_passo(b, novo, combo);
			if (escolha >= 0) {
				menu = false;
				som_mudo = false;
				entrada_travada = true;
				ult_x0 = -99999;
				if (escolha == M_REINICIAR) {
					LOG("reiniciar jogo");
					core.reset();
				} else if (escolha == M_LISTA) {
					LOG("voltar para a lista");
					ret = 10; /* o menu volta para a lista de jogos deste sistema */
					break;
				} else if (escolha == M_SAIR) {
					LOG("sair do jogo");
					break;
				}
			} else {
				menu_desenha();
				fb_mostra();
				continue;
			}
		}
		double tq = agora();
		static double pico_ctrl, pico_run, pico_copia;
		if (tq - t_loop0 > pico_ctrl) pico_ctrl = tq - t_loop0;
		if (um_pra_um) {
			core.run();
		} else {
			acc += av.timing.fps / tela_hz;
			while (acc >= 1.0) {
				core.run();
				acc -= 1.0;
			}
		}
		if (osd_tem_pend) {
			osd_tem_pend = false;
			aviso_poe(&av_ctl, osd_pend);
		}
		osd_desenha();
		double t_run = agora() - tq;
		if (t_run > pico_run) pico_run = t_run;
		static int n_lentos;
		if (agora() - tq > 0.5 && n_lentos++ < 20)
			LOG("quadro %ld lento: %.1f s (cd: %ld blocos, metodo %d)", quadros, agora() - tq, cd_n_blocos, cd_metodo);
		if (quadros == 60 || quadros == 600)
			LOG("quadro %ld ok; tela do jogo %ux%u", quadros, ult_w, ult_h);
		if (fb.sem_vsync && !fb.fake) {
			t_prox += 1.0 / (um_pra_um ? av.timing.fps : tela_hz);
			double d = t_prox - agora();
			if (d > 0) {
				struct timespec ts = {(time_t)d, (long)((d - (time_t)d) * 1e9)};
				nanosleep(&ts, NULL);
			} else if (d < -0.1)
				t_prox = agora();
		}
		fb_vsync();
		double tc = agora();
		memcpy(fb.mem, fb.back, fb.tam);
		if (agora() - tc > pico_copia) pico_copia = agora() - tc;
		if (++quadros % 300 == 0) {
			sram_grava(false);
			static double t_ult;
			double t = agora();
			if (t_ult > 0)
				LOG("ritmo: %.1f quadros/s; cd: %ld blocos, media %.0f ms, max %.0f ms, metodo %d, adiante %ld/%ld usados; som: buffer %d%%, vazio %ld; ressinc %ld",
				    300 / (t - t_ult), cd_n_blocos, cd_n_blocos ? cd_t_total / cd_n_blocos * 1e3 : 0, cd_t_max * 1e3, cd_metodo,
				    cd_n_acertos_pf, cd_n_adiante, r_cnt * 100 / RING, som_vazio, vs_ressinc);
			LOG("picos: controles %.1f ms, nucleo %.1f ms, copia %.1f ms", pico_ctrl * 1e3, pico_run * 1e3, pico_copia * 1e3);
			pico_ctrl = pico_run = pico_copia = 0;
			t_ult = t;
			cd_t_max = 0;
		}
		if (max_q && quadros >= max_q) {
			if (getenv("RV_MENU_TESTE")) {
				if (getenv("RV_SALVA_TESTE")) {
					slot_salva(2);
					slot_carrega(2);
				}
				if (getenv("RV_MENU_SUB"))
					sub = atoi(getenv("RV_MENU_SUB"));
				slots_le();
				slot_sel = 2;
				menu_fundo();
				menu_desenha();
				fb_mostra();
			}
			break;
		}
	}
	sram_grava(false);
	fb_grava_fake();
	g_sair = 1;
	governador(false);
	if (volume != volume_ini) { /* o menu do RetroVolt le e guarda na configuracao */
		FILE *fv = fopen("/run/retrovolt/volume", "w");
		if (fv) {
			fprintf(fv, "%d\n", volume);
			fclose(fv);
		}
		LOG("volume: %d", volume);
	}
	core.unload_game();
	core.deinit();
	LOG("fim (%ld quadros)", quadros);
	return ret;
}
