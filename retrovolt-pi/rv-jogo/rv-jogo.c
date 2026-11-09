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
static const char *a_audio, *a_opcoes, *a_fonte = "6X10", *a_save;
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
} core;

static struct retro_system_av_info av;
static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;

/* variaveis do nucleo (opcoes): chave, valor */
#define MAXVAR 256
static struct { char *k, *v; } vars[MAXVAR];
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
#define NCACHE 8

static int cd_fd = -1;
static bool cd_arquivo; /* teste: --cd aponta para um .bin */
static char cd_cue[4096];
static int64_t cd_tam;
static pthread_mutex_t cd_mx = PTHREAD_MUTEX_INITIALIZER;
static struct { int64_t lba; uint64_t uso; uint8_t d[BLOCO * SETOR]; } cd_cache[NCACHE];
static uint64_t cd_relogio;

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
	bool ok = cd_metodo == 0 && cd_faixa(lba0) == cd_faixa(lba0 + n - 1) && cd_fx[cd_faixa(lba0)].dados &&
	          cd_read_cd(lba0, n, d, 0xF8);
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
	if (t > cd_t_max)
		cd_t_max = t;
}

static int64_t cd_ler(int64_t pos, uint8_t *s, int64_t len)
{
	if (pos >= cd_tam)
		return 0;
	if (pos + len > cd_tam)
		len = cd_tam - pos;
	int64_t feito = 0;
	pthread_mutex_lock(&cd_mx);
	while (feito < len) {
		int64_t lba = (pos + feito) / SETOR;
		int64_t b0 = lba - lba % BLOCO;
		int k, velho = 0;
		for (k = 0; k < NCACHE; k++)
			if (cd_cache[k].lba == b0)
				break;
		if (k == NCACHE) {
			for (k = 1; k < NCACHE; k++)
				if (cd_cache[k].uso < cd_cache[velho].uso)
					velho = k;
			k = velho;
			cd_le_bloco(b0, cd_cache[k].d);
			cd_cache[k].lba = b0;
		}
		cd_cache[k].uso = ++cd_relogio;
		int64_t off = pos + feito - b0 * SETOR;
		int64_t n = BLOCO * SETOR - off;
		if (n > len - feito)
			n = len - feito;
		memcpy(s + feito, cd_cache[k].d + off, n);
		feito += n;
	}
	pthread_mutex_unlock(&cd_mx);
	return feito;
}

static void msf(int lba, char *o)
{
	sprintf(o, "%02d:%02d:%02d", lba / 4500, lba / 75 % 60, lba % 75);
}

static bool cd_abrir(const char *dev)
{
	for (int k = 0; k < NCACHE; k++)
		cd_cache[k].lba = -1;
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

static double vs_mede(int tipo)
{
	if (!vs_espera(tipo))
		return 0;
	double t0 = agora();
	for (int i = 0; i < 6; i++)
		if (!vs_espera(tipo))
			return 0;
	return (agora() - t0) / 6;
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
	int passo = h > 300 ? 2 : 1; /* 480i do jogo: uma linha sim, outra nao */
	int eh = h / passo, lp, cw, x0, y0;
	if (tubo) {
		lp = fb.h >= 400 ? 2 : 1;
		cw = (int)lround(fb.w * conv_w);
		if (cw > fb.w) cw = fb.w;
		if (cw < (int)w) cw = w;
		x0 = (fb.w - cw) / 2 + conv_x;
		y0 = (fb.h - eh * lp) / 2 + conv_y * lp;
	} else {
		lp = fb.h / eh;
		if (lp < 1) lp = 1;
		int dh = eh * lp;
		cw = (int)lround(dh * (aspecto > 0 ? aspecto : 4.0 / 3.0));
		if (cw > fb.w) cw = fb.w;
		x0 = (fb.w - cw) / 2;
		y0 = (fb.h - dh) / 2;
	}
	if (x0 != ult_x0 || y0 != ult_y0 || cw != ult_cw || eh * lp != ult_dh) {
		memset(fb.back, 0, fb.tam); /* mudou a geometria: limpa as bordas */
		ult_x0 = x0; ult_y0 = y0; ult_cw = cw; ult_dh = eh * lp;
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
		for (int k = 0; k < lp; k++) {
			int yy = y0 + j * lp + k;
			if (yy < 0 || yy >= fb.h)
				continue;
			uint8_t *d = fb.back + (size_t)yy * fb.stride + (size_t)x0 * bpp;
			if (bpp == 2)
				for (int i = i0; i < i1; i++)
					((uint16_t *)d)[i] = linha_tmp[xmap[i]];
			else
				for (int i = i0; i < i1; i++)
					((uint32_t *)d)[i] = linha_tmp[xmap[i]];
		}
	}
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
		int16_t l = d[i * 2], r = d[i * 2 + 1];
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
	}
	pthread_mutex_unlock(&dv_mx);
}

static bool entrada_travada; /* depois do menu: espera soltar tudo */
static void input_poll(void) {}
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
	if (port > 1 || entrada_travada)
		return 0;
	uint32_t b = pad_bot[port] & 0xffff;
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
		*(bool *)data = false;
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
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
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
#define MW 320
#define MH 240
#define NAV 0x0885   /* (12,18,40) em RGB565 */
static uint16_t ov[MW * MH];
static uint16_t fundo[MW * MH];
static const Fonte *F;
static int menu_sel;
static const char *MENU[] = {"CONTINUAR", "REINICIAR JOGO", "SAIR DO JOGO"};

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

static void menu_fundo(void)
{
	/* ultimo quadro do jogo reduzido para 320x240 e escurecido (25%) */
	for (int j = 0; j < MH; j++)
		for (int i = 0; i < MW; i++) {
			uint16_t p = NAV;
			if (ult_q && ult_w && ult_h) {
				int sx = i * ult_w / MW, sy = j * ult_h / MH;
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

static void menu_desenha(void)
{
	uint16_t IVORY = rgb(240, 236, 225), ORANGE = rgb(255, 140, 40), SLATE = rgb(120, 130, 160);
	memcpy(ov, fundo, sizeof ov);
	m_rect(0, 0, 6, MH, ORANGE);
	m_rect(MW - 6, 0, 6, MH, ORANGE);
	m_text(16, 6, "RETRO", IVORY);
	m_text(16 + 5 * F->cw, 6, "VOLT", ORANGE);
	char hora[8];
	time_t t = time(NULL);
	strftime(hora, sizeof hora, "%H:%M", localtime(&t));
	m_rtext(MW - 12, 6, hora, IVORY);
	m_text(16, 28, "JOGO", IVORY);
	m_rect(16, 28 + F->ch + 2, 4 * F->cw, 2, ORANGE);
	m_text(16, 50, a_nome, SLATE);
	int y = 76;
	for (int i = 0; i < 3; i++) {
		bool on = i == menu_sel;
		m_text(24, y, MENU[i], on ? IVORY : SLATE);
		if (on) {
			int w = strlen(MENU[i]) * F->cw;
			m_rect(16, y - 1, 3, F->ch + 2, ORANGE);
			m_rect(24, y + F->ch + 2, w, 1, ORANGE);
			m_text(24 + w + F->cw, y, "->", ORANGE);
		}
		y += F->ch + 12;
	}
	m_text(16, MH - 4 - F->ch, "START+SELECT VOLTA", SLATE);
	m_rtext(MW - 12, MH - 4 - F->ch, "A ESCOLHE", SLATE);
	desenha(ov, MW, MH, MW * 2, RETRO_PIXEL_FORMAT_RGB565, 4.0 / 3.0);
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
	if (!core.load_game(&gi)) {
		LOG("ERRO: o nucleo nao abriu o jogo");
		return 2;
	}
	core.get_system_av_info(&av);
	LOG("jogo: %ux%u %.3f Hz, som %.0f Hz", av.geometry.base_width, av.geometry.base_height, av.timing.fps, av.timing.sample_rate);
	core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
	core.set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
	sram_carrega();
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
			som_mudo = true;
			menu_fundo();
			LOG("menu aberto");
			sram_grava(false);
		} else if (menu) {
			/* botao de baixo (A do Xbox/8BitDo) ou START escolhe; o da direita volta */
			int escolha = -1;
			if (combo || (novo & (1u << B_A)))
				escolha = 0;
			else if (novo & ((1u << B_B) | (1u << B_START)))
				escolha = menu_sel;
			else if (novo & (1u << B_UP))
				menu_sel = (menu_sel + 2) % 3;
			else if (novo & (1u << B_DOWN))
				menu_sel = (menu_sel + 1) % 3;
			if (escolha >= 0) {
				menu = false;
				som_mudo = false;
				entrada_travada = true;
				ult_x0 = -99999;
				if (escolha == 1) {
					LOG("reiniciar jogo");
					core.reset();
				} else if (escolha == 2) {
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
				LOG("ritmo: %.1f quadros/s; cd: %ld blocos, media %.0f ms, max %.0f ms, metodo %d; som: buffer %d%%, vazio %ld; ressinc %ld",
				    300 / (t - t_ult), cd_n_blocos, cd_n_blocos ? cd_t_total / cd_n_blocos * 1e3 : 0, cd_t_max * 1e3, cd_metodo,
				    r_cnt * 100 / RING, som_vazio, vs_ressinc);
			LOG("picos: controles %.1f ms, nucleo %.1f ms, copia %.1f ms", pico_ctrl * 1e3, pico_run * 1e3, pico_copia * 1e3);
			pico_ctrl = pico_run = pico_copia = 0;
			t_ult = t;
			cd_t_max = 0;
		}
		if (max_q && quadros >= max_q) {
			if (getenv("RV_MENU_TESTE")) {
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
	core.unload_game();
	core.deinit();
	LOG("fim (%ld quadros)", quadros);
	return ret;
}
