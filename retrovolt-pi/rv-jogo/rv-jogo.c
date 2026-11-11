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
#define VK_NO_PROTOTYPES
#include "libretro_vulkan.h"
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
static const char *FILTROS[] = {"NENHUM", "SCANLINES", "SCANLINES LEVES", "CRT", "SUAVE", "SCALE2X", "SCALE3X", "XBR"};
#define N_FILTROS 8
#define F_AMPLIA 4 /* daqui para frente: ampliadores (imagem intermediaria maior) */
static const char *a_filtro_arq;
static bool sem_filtro; /* menu do jogo: nunca com filtro */
static int imagem;       /* LCD: 0 = ORIGINAL (ampliacao inteira, nitida), 1 = TELA CHEIA (enche a altura) */
static const char *a_imagem_arq;
static int conv_menu_x, conv_menu_y; static double conv_menu_w = 1.0, conv_menu_h = 1.0; /* convergencia geral (do menu) */
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
/* base = valor geral (arquivo do nucleo, senao o padrao do nucleo); com --opcoes-jogo,
 * o que o jogador muda no menu vale so para este jogo (gravado so o que difere da base) */
static struct { char *k, *v, *base, *desc, *vals[64]; int nvals; } vars[MAXVAR];
static const char *a_opcoes_jogo;
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

static void opcoes_ler_arq(const char *arq, bool base);
static void opcoes_ler(void)
{
	if (a_opcoes)
		opcoes_ler_arq(a_opcoes, true);
	if (a_opcoes_jogo)
		opcoes_ler_arq(a_opcoes_jogo, false);
}

static void opcoes_ler_arq(const char *arq, bool base)
{
	FILE *f = fopen(arq, "r");
	if (!f)
		return;
	if (!base)
		LOG("opcoes deste jogo: %s", arq);
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
		if (*k) {
			var_set(k, v, false);
			if (base)
				for (int i = 0; i < nvars; i++)
					if (!strcmp(vars[i].k, k)) {
						free(vars[i].base);
						vars[i].base = strdup(v);
					}
		}
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
#define PRE_BLOCOS 8 /* comeco de cada musica guardado ao abrir o disco: 8 x 16 setores = 1,7 s */
#define PRE_SINC_S 6.0 /* no maximo isso antes do jogo; o resto em segundo plano, com a leitora ociosa */

static int cd_fd = -1;
static bool cd_arquivo; /* teste: --cd aponta para um .bin */
static char cd_cue[8192]; /* 99 faixas com INDEX 00 cabem (~6,5 KB) */
static int64_t cd_tam;
/* cd_mx protege o cache; cd_io_mx protege a leitora (so um comando por vez).
 * A leitura na leitora acontece sem cd_mx, para o jogo achar no cache o que ja
 * foi lido adiante enquanto outro bloco esta chegando. */
static pthread_mutex_t cd_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cd_io_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cd_cv = PTHREAD_COND_INITIALIZER;    /* um bloco ficou pronto */
static pthread_cond_t cd_pf_cv = PTHREAD_COND_INITIALIZER; /* pedido de leitura adiante */
/* cache dos blocos lidos. Padrao: NCACHE (1,8 MB). Dreamcast (--cd-cache MB): bem maior,
 * porque o jogo volta toda hora aos mesmos arquivos e cada busca nova na leitora custa
 * ~100 ms com o nucleo parado esperando. Memoria de cada vaga so na 1a vez que e usada. */
static struct cd_vaga_s { int64_t lba; uint64_t uso; bool lendo, adiantado; uint8_t *d; } *cd_cache;
static int ncache = NCACHE;
static uint64_t cd_relogio;
static volatile double cd_ult_leitura;
static volatile bool cd_em_menu; /* menu do jogo aberto: jogo parado, ninguem le o disco */
#define CD_ACORDA_MENU_S 3        /* com o menu aberto: a leitora desacelera em 3-4 s (LOG do autor) */
static int cd_acorda_s = 10; /* --cd-acorda: segundos sem leitura ate acordar a leitora (regra do autor: 10) */
static volatile int64_t cd_ult_lba;
static int64_t cd_pf_base = -1; /* primeiro bloco a ler adiante */
static uint64_t cd_pf_ger;      /* muda a cada pedido novo */
static long cd_n_adiante, cd_n_acertos_pf;
/* comeco das musicas, lido ao abrir o disco e nunca descartado: a troca dados -> musica
 * na leitora leva ~1,5 s e o jogo travava toda vez que uma musica comecava. Ordenado por
 * lba; so muda antes do jogo comecar (depois e so leitura, sem trava). */
static struct cd_fixo { int64_t lba; uint8_t *d; } *cd_fixos;
static int cd_nfixos, cd_cap_fixos;
static int64_t *cd_pre_pend; /* blocos de comeco de musica ainda nao lidos (vao em segundo plano) */
static int cd_pre_npend, cd_pre_prox, cd_pre_falhas;
static volatile double cd_ult_pedido; /* ultima leitura pedida pelo jogo */
/* copia do disco para a memoria (cache grande, Dreamcast): com a leitora livre, le em
 * ordem a partir do ponto mais baixo que o jogo ja leu e guarda nas vagas vazias. As
 * falas e efeitos ficam espalhados pelo disco (cada busca ~100 ms com o jogo parado);
 * depois de copiados, saem da memoria. O jogo tem sempre a vez. */
static int64_t cd_jogo_min = -1;
static long cd_n_copiados, cd_n_vazios;
static long cd_n_fixos_usados;

/* faixas do disco (para saber se um setor e de audio) */
static int cd_nfx;
static struct { int lba; bool dados; int modo; } cd_fx[100];
/* --cd-faixas (Dreamcast): cada faixa vira um arquivo virtual faixaNN.bin e o cue diz
 * as sessoes (CD-R do Dreamcast: musica na 1a sessao, jogo na 2a). O Flycast acha a
 * posicao de cada faixa pelo tamanho dos arquivos, entao o fim da 1a sessao conta. */
static bool cd_faixas;
static int cd_fx_fim[100];
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

/* tipo esperado: 0 = qualquer; 0x04 = so audio (a leitora recusa se for dados) */
static bool cd_timeout; /* a ultima leitura falhou por tempo (leitora ocupada/girando), nao por erro */
static int cd_sense_chave; /* 2 = nao pronta (sem disco / girando), 6 = atencao (trocou/reiniciou) */
static int cd_timeout_ms = 5000;
static bool cd_read_cd_t(uint32_t lba, int n, uint8_t *buf, uint8_t flags, uint8_t tipo)
{
	uint8_t cmd[12] = {0xBE, tipo, lba >> 24, lba >> 16, lba >> 8, lba, 0, 0, n, flags, 0, 0};
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
	io.timeout = cd_timeout_ms;
	int r = ioctl(cd_fd, SG_IO, &io);
	bool ok = r == 0 && io.status == 0 && io.host_status == 0 && (io.driver_status & 0x7) == 0;
	cd_timeout = !ok && r == 0 && io.host_status == 3; /* DID_TIME_OUT */
	cd_sense_chave = ok ? 0 : (sense[2] & 15);
	if (!ok && cd_n_erros++ < 8)
		LOG("cd: READ CD lba %u x%d flags %02x falhou: ioctl %d (%s) status %d host %d driver %d sense %x/%02x/%02x",
		    lba, n, flags, r, r ? strerror(errno) : "-", io.status, io.host_status, io.driver_status,
		    sense[2] & 15, sense[12], sense[13]);
	return ok;
}

static bool cd_read_cd(uint32_t lba, int n, uint8_t *buf, uint8_t flags)
{
	return cd_read_cd_t(lba, n, buf, flags, 0);
}

/* comando SCSI simples (MODE SENSE/SELECT); dir: SG_DXFER_FROM_DEV ou TO_DEV */
static bool cd_scsi(uint8_t *cmd, int cl, uint8_t *buf, int n, int dir)
{
	uint8_t sense[32] = {0};
	sg_io_hdr_t io;
	memset(&io, 0, sizeof io);
	io.interface_id = 'S';
	io.dxfer_direction = dir;
	io.cmd_len = cl;
	io.cmdp = cmd;
	io.mx_sb_len = sizeof sense;
	io.sbp = sense;
	io.dxfer_len = n;
	io.dxferp = buf;
	io.timeout = 3000;
	return ioctl(cd_fd, SG_IO, &io) == 0 && io.status == 0 && io.host_status == 0 && (io.driver_status & 0x7) == 0;
}

/* leitora "nao pronta" (sense 2) ou "atencao" (6): acordando ou se reiniciou depois de
 * travar. Espera ficar pronta (TEST UNIT READY) em vez de achar que o disco sumiu e
 * trocar de metodo (foi isso que deu tela preta no Neo Geo: setores vazios). */
static bool cd_espera_pronta(void)
{
	static int n_log;
	double t0 = agora();
	for (int i = 0; i < 50; i++) {
		uint8_t tur[6] = {0};
		if (cd_scsi(tur, 6, NULL, 0, SG_DXFER_NONE)) {
			if (n_log++ < 10)
				LOG("cd: leitora pronta de novo depois de %.1f s", agora() - t0);
			return true;
		}
		usleep(300000);
	}
	LOG("cd: leitora NAO ficou pronta em 15 s");
	return false;
}

/* temporizador de descanso da leitora (pagina 0x0D do padrao MMC, "inactivity
 * timer"): pede o maximo (32 min). Nao grava na leitora (vale ate desligar). */
static void cd_descanso_maximo(void)
{
	uint8_t b[64] = {0};
	uint8_t sen[10] = {0x5A, 0x08, 0x0D, 0, 0, 0, 0, 0, sizeof b, 0}; /* MODE SENSE(10), sem blocos */
	if (!cd_scsi(sen, 10, b, sizeof b, SG_DXFER_FROM_DEV) || (b[8] & 0x3F) != 0x0D) {
		LOG("cd: leitora nao informou o tempo de descanso (pagina 0x0D)");
		return;
	}
	int antes = b[11] & 0x0F;
	int tam = 8 + 2 + b[9];
	b[0] = b[1] = 0;           /* tamanho: reservado no MODE SELECT */
	b[8] &= 0x3F;              /* tira o bit PS */
	b[11] = (b[11] & 0xF0) | 0x0F;
	uint8_t sel[10] = {0x55, 0x10, 0, 0, 0, 0, 0, tam >> 8, tam & 0xff, 0}; /* MODE SELECT(10), PF */
	bool ok = cd_scsi(sel, 10, b, tam, SG_DXFER_TO_DEV);
	LOG("cd: tempo de descanso da leitora: codigo %X -> F (32 min): %s", antes, ok ? "aceito" : "RECUSADO");
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
	if (audio) {
		if (cd_metodo == 0 && cd_read_cd(lba, 1, b, 0x10))
			return true;
		if ((cd_sense_chave == 2 || cd_sense_chave == 6) && !cd_arquivo && cd_espera_pronta() && cd_read_cd(lba, 1, b, 0x10))
			return true;
		if (cd_audio_ioctl(lba, b) || cd_read_cd(lba, 1, b, 0x10))
			return true;
		/* "audio" pelo TOC mas e dados (fim da faixa de dados antes da musica) */
		return cd_read_cd(lba, 1, b, 0xF8) || cd_raw_ioctl(lba, b);
	}
	if (cd_metodo == 0 && cd_read_cd(lba, 1, b, 0xF8))
		return true;
	/* passou do tempo (leitora acordando): tenta de novo do mesmo jeito; um atraso
	 * nao e motivo para trocar de metodo pelo resto do jogo (antes: 22,7 s e depois
	 * tudo pelo metodo lento) */
	bool acordando = cd_sense_chave == 2 || cd_sense_chave == 6;
	bool demorou = cd_metodo == 0 && (cd_timeout || acordando);
	if (acordando && !cd_arquivo)
		cd_espera_pronta();
	if (demorou && cd_read_cd(lba, 1, b, 0xF8))
		return true;
	/* intervalo entre faixas (Sega CD, PC Engine): o TOC diz dados, o setor e audio */
	if (cd_metodo == 0 && cd_read_cd_t(lba, 1, b, 0x10, 0x04))
		return true;
	if (cd_metodo <= 1 && cd_raw_ioctl(lba, b)) {
		if (cd_metodo != 1 && !demorou) {
			cd_metodo = 1;
			LOG("cd: passando a ler pelo CDROMREADRAW");
		}
		return true;
	}
	if (cd_cozido(lba, b)) {
		if (cd_metodo != 2 && !demorou) {
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
	uint8_t fl = cd_fx[cd_faixa(lba0)].dados ? 0xF8 : 0x10;
	bool ok = mesma && cd_read_cd(lba0, n, d, fl);
	if (!ok && mesma && (cd_timeout || cd_sense_chave == 2 || cd_sense_chave == 6)) {
		LOG("cd: leitora %s (lba %lld): tentando de novo", cd_timeout ? "demorou mais de 5 s" : "nao estava pronta",
		    (long long)lba0);
		if (!cd_timeout)
			cd_espera_pronta();
		ok = cd_read_cd(lba0, n, d, fl);
	}
	if (!ok)
		for (int i = 0; i < n; i++)
			if (!cd_le_setor(lba0 + i, d + i * SETOR)) {
				memset(d + i * SETOR, 0, SETOR);
				if (cd_n_erros++ < 30)
					LOG("cd: setor %lld ilegivel", (long long)(lba0 + i));
			}
	double t = agora() - t0;
	if (t > 0.5) { /* diagnostico das travadas: onde e de que tipo era a leitura */
		LOG("cd: bloco lento: lba %lld (%s, faixa %d) %.0f ms; leitura anterior lba %lld",
		    (long long)lba0, cd_fx[cd_faixa(lba0)].dados ? "dados" : "musica", cd_faixa(lba0) + 1, t * 1e3, (long long)cd_ult_lba);
	}
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
	for (int k = 0; k < ncache; k++)
		if (cd_cache[k].lba == b0)
			return k;
	return -1;
}

/* escolhe a vaga mais antiga que nao esta sendo lida (com cd_mx) */
static int cd_vaga(void)
{
	int v = -1;
	for (int k = 0; k < ncache; k++)
		if (!cd_cache[k].lendo && (v < 0 || cd_cache[k].uso < cd_cache[v].uso))
			v = k;
	return v;
}

/* le o bloco b0 da leitora para a vaga k (entra e sai com cd_mx) */
static void cd_carrega(int k, int64_t b0)
{
	if (!cd_cache[k].d && !(cd_cache[k].d = malloc(BLOCO * SETOR))) {
		LOG("ERRO: sem memoria para o cache do CD");
		exit(3);
	}
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

static const uint8_t *cd_fixo_acha(int64_t b0)
{
	int a = 0, b = cd_nfixos - 1;
	while (a <= b) {
		int m = (a + b) / 2;
		if (cd_fixos[m].lba == b0)
			return cd_fixos[m].d;
		if (cd_fixos[m].lba < b0)
			a = m + 1;
		else
			b = m - 1;
	}
	return NULL;
}

static int64_t cd_ler(int64_t pos, uint8_t *s, int64_t len)
{
	if (pos >= cd_tam)
		return 0;
	if (pos + len > cd_tam)
		len = cd_tam - pos;
	int64_t feito = 0, ult = -1;
	cd_ult_pedido = agora();
	pthread_mutex_lock(&cd_mx);
	while (feito < len) {
		int64_t lba = (pos + feito) / SETOR;
		int64_t b0 = lba - lba % BLOCO;
		const uint8_t *fixo = cd_fixo_acha(b0);
		if (fixo) { /* comeco de musica guardado: sai da memoria, sem esperar a leitora */
			int64_t off = pos + feito - b0 * SETOR;
			int64_t n = BLOCO * SETOR - off;
			if (n > len - feito)
				n = len - feito;
			memcpy(s + feito, fixo + off, n);
			feito += n;
			if (ult != b0)
				cd_n_fixos_usados++;
			ult = b0;
			continue;
		}
		int k = cd_acha(b0);
		if (k >= 0 && cd_cache[k].lendo) {
			pthread_cond_wait(&cd_cv, &cd_mx); /* a leitura adiante ja esta trazendo */
			continue;
		}
		if (k < 0) {
			/* fora o comeco de cada faixa (IP.BIN e a lista de arquivos, lidos no boot):
			 * no CD-R o enchimento costuma vir antes e os arquivos do jogo no fim */
			int f = cd_faixa(b0);
			if (cd_fx[f].dados && b0 >= cd_fx[f].lba + 2048 && (cd_jogo_min < 0 || b0 < cd_jogo_min))
				cd_jogo_min = b0;
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
		for (int i = 0, lidos = 0; lidos < PF_BLOCOS && i < PF_BLOCOS + PRE_BLOCOS && ger == cd_pf_ger && !g_sair; i++) {
			int64_t b0 = cd_pf_base + i * BLOCO;
			if (b0 < 0 || b0 * SETOR >= cd_tam)
				break;
			if (cd_fixo_acha(b0))
				continue; /* ja guardado: nao conta, busca o que vem depois */
			lidos++;
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

/* bloco so de zeros (enchimento do CD-R): nao vale a vaga */
static bool cd_bloco_vazio(int64_t b0, const uint8_t *d)
{
	int off = cd_fx[cd_faixa(b0)].modo == 1 ? 16 : 24;
	for (int i = 0; i < BLOCO; i++) {
		const uint8_t *u = d + (size_t)i * SETOR + off;
		for (int j = 0; j < 2048; j++)
			if (u[j])
				return false;
	}
	return true;
}

static bool cd_lba_existe(int64_t lba);
static void *cd_copia_thread(void *x)
{
	(void)x;
	int64_t total = cd_tam / SETOR, prox = -1;
	double t0 = 0;
	long marca = 0;
	while (!g_sair) {
		/* o jogo leu ha pouco: espera ele sossegar (menu aberto: jogo parado, pode) */
		if (!cd_em_menu && agora() - cd_ult_pedido < 0.4) {
			usleep(30000);
			continue;
		}
		pthread_mutex_lock(&cd_mx);
		if (cd_jogo_min < 0) {
			pthread_mutex_unlock(&cd_mx);
			usleep(30000);
			continue;
		}
		if (prox < 0) {
			prox = cd_jogo_min;
			t0 = agora();
			LOG("cd: copiando o disco para a memoria em 2o plano (a partir do lba %lld)", (long long)prox);
		}
		while (prox < total && (cd_acha(prox) >= 0 || cd_fixo_acha(prox) || !cd_fx[cd_faixa(prox)].dados ||
		                        !cd_lba_existe(prox)))
			prox += BLOCO;
		int k = -1;
		for (int i = 0; i < ncache; i++)
			if (cd_cache[i].lba < 0 && !cd_cache[i].lendo) {
				k = i;
				break;
			}
		if (prox >= total || k < 0) {
			LOG("cd: copia para a memoria %s: %ld blocos (%.0f MB) em %.0f s, %ld vazios descartados",
			    k < 0 ? "parou (memoria cheia)" : "terminou", cd_n_copiados,
			    cd_n_copiados * (double)BLOCO * SETOR / 1048576, agora() - t0, cd_n_vazios);
			pthread_mutex_unlock(&cd_mx);
			return NULL;
		}
		int64_t b0 = prox;
		prox += BLOCO;
		cd_cache[k].uso = cd_relogio; /* idade de "agora", sem passar na frente do jogo */
		cd_cache[k].adiantado = false;
		cd_carrega(k, b0);
		if (cd_cache[k].lba == b0 && cd_bloco_vazio(b0, cd_cache[k].d)) {
			cd_cache[k].lba = -1; /* enchimento: libera a vaga */
			cd_n_vazios++;
		} else
			cd_n_copiados++;
		if (cd_n_copiados - marca >= 1783) { /* ~64 MB */
			marca = cd_n_copiados;
			LOG("cd: copia para a memoria: %.0f MB (%.0f s)", cd_n_copiados * (double)BLOCO * SETOR / 1048576,
			    agora() - t0);
		}
		pthread_mutex_unlock(&cd_mx);
	}
	return NULL;
}

static void msf(int lba, char *o)
{
	sprintf(o, "%02d:%02d:%02d", lba / 4500, lba / 75 % 60, lba % 75);
}

/* poe um bloco guardado na lista ordenada (com cd_mx, ou antes das threads) */
static void cd_fixo_poe(int64_t b0, uint8_t *d)
{
	int i = cd_nfixos;
	while (i > 0 && cd_fixos[i - 1].lba > b0)
		i--;
	if ((i > 0 && cd_fixos[i - 1].lba == b0) || cd_nfixos >= cd_cap_fixos) {
		free(d);
		return;
	}
	memmove(&cd_fixos[i + 1], &cd_fixos[i], (cd_nfixos - i) * sizeof *cd_fixos);
	cd_fixos[i].lba = b0;
	cd_fixos[i].d = d;
	cd_nfixos++;
}

/* le um bloco do comeco de musica: UM comando (3 s no maximo), sem os metodos lentos.
 * Antes: um bloco problematico perto do fim do disco levou 35 s e a leitora se
 * reiniciou ("sem disco") - o jogo do Neo Geo recebeu setores vazios. */
static bool cd_pre_le(int64_t b0, uint8_t *d)
{
	int64_t total = cd_tam / SETOR;
	int n = BLOCO;
	if (b0 + n > total)
		n = total - b0;
	if (n <= 0)
		return false;
	if (cd_arquivo || cd_metodo != 0) {
		cd_le_bloco(b0, d);
		return true;
	}
	memset(d, 0, BLOCO * SETOR);
	int ant = cd_timeout_ms;
	cd_timeout_ms = 3000;
	bool ok = true;
	int f0 = cd_faixa(b0), f1 = cd_faixa(b0 + n - 1);
	if (f0 == f1)
		ok = cd_read_cd(b0, n, d, cd_fx[f0].dados ? 0xF8 : 0x10);
	else
		for (int i = 0; i < n && ok; i++)
			ok = cd_read_cd(b0 + i, 1, d + i * SETOR, cd_fx[cd_faixa(b0 + i)].dados ? 0xF8 : 0x10);
	cd_timeout_ms = ant;
	cd_ult_leitura = agora();
	cd_ult_lba = b0;
	return ok;
}

/* guarda o comeco de cada faixa de musica (so musica: dados o jogo le em sequencia,
 * e a leitura adiante cobre). Ate PRE_SINC_S antes do jogo; o resto fica na fila e
 * cd_pre_thread le quando o jogo nao esta usando a leitora. */
static void cd_pre_musicas(void)
{
	int naudio = 0;
	for (int i = 0; i < cd_nfx; i++)
		naudio += !cd_fx[i].dados;
	if (!naudio)
		return;
	int64_t total = cd_tam / SETOR;
	cd_cap_fixos = naudio * (PRE_BLOCOS + 1);
	cd_fixos = calloc(cd_cap_fixos, sizeof *cd_fixos);
	cd_pre_pend = calloc(cd_cap_fixos, sizeof *cd_pre_pend);
	for (int i = 0; i < cd_nfx; i++) {
		if (cd_fx[i].dados)
			continue;
		int64_t ini = cd_fx[i].lba, fim = i + 1 < cd_nfx ? cd_fx[i + 1].lba : total;
		for (int64_t b0 = ini - ini % BLOCO; b0 < ini + PRE_BLOCOS * BLOCO && b0 < fim; b0 += BLOCO)
			if (!cd_pre_npend || cd_pre_pend[cd_pre_npend - 1] != b0)
				cd_pre_pend[cd_pre_npend++] = b0;
	}
	double t0 = agora();
	int seguidas = 0;
	while (cd_pre_prox < cd_pre_npend && agora() - t0 < PRE_SINC_S) {
		uint8_t *d = malloc(BLOCO * SETOR);
		if (!d)
			break;
		int64_t b0 = cd_pre_pend[cd_pre_prox++];
		if (cd_pre_le(b0, d)) {
			cd_fixo_poe(b0, d);
			seguidas = 0;
		} else {
			free(d);
			cd_pre_falhas++;
			if (++seguidas >= 3) { /* leitora com problema: para (o jogo vem primeiro) */
				LOG("cd: comeco das musicas: 3 falhas seguidas (lba %lld): parei", (long long)b0);
				if (cd_sense_chave == 2 || cd_sense_chave == 6)
					cd_espera_pronta();
				cd_pre_prox = cd_pre_npend;
			}
		}
	}
	LOG("cd: comeco das musicas: %d de %d blocos guardados (%d MB) em %.1f s; %d ficam para segundo plano; %d falhas",
	    cd_nfixos, cd_pre_npend, (int)((int64_t)cd_nfixos * BLOCO * SETOR >> 20), agora() - t0,
	    cd_pre_npend - cd_pre_prox, cd_pre_falhas);
}

/* continua guardando o comeco das musicas so com a leitora ociosa (o jogo sem pedir
 * nada ha 2 s): telas paradas, menus. Tambem mantem a leitora girando. */
static void *cd_pre_thread(void *x)
{
	(void)x;
	int seguidas = 0, feitos = 0;
	while (!g_sair && cd_pre_prox < cd_pre_npend) {
		if (agora() - cd_ult_pedido < 2.0) {
			usleep(200000);
			continue;
		}
		uint8_t *d = malloc(BLOCO * SETOR);
		if (!d)
			break;
		int64_t b0 = cd_pre_pend[cd_pre_prox++];
		pthread_mutex_lock(&cd_io_mx);
		bool ok = cd_pre_le(b0, d);
		pthread_mutex_unlock(&cd_io_mx);
		if (ok) {
			pthread_mutex_lock(&cd_mx);
			cd_fixo_poe(b0, d);
			pthread_mutex_unlock(&cd_mx);
			seguidas = 0;
			feitos++;
		} else {
			free(d);
			cd_pre_falhas++;
			if (++seguidas >= 3)
				break;
		}
	}
	LOG("cd: comeco das musicas em segundo plano: +%d blocos (total %d de %d), %d falhas",
	    feitos, cd_nfixos, cd_pre_npend, cd_pre_falhas);
	return NULL;
}

static void cd_cue_faixas(int fim)
{
	int s2 = 0;
	struct cdrom_multisession ms;
	memset(&ms, 0, sizeof ms);
	ms.addr_format = CDROM_LBA;
	if (ioctl(cd_fd, CDROMMULTISESSION, &ms) == 0 && ms.addr.lba > 0) {
		for (int i = 1; i < cd_nfx; i++)
			if (cd_fx[i].lba == ms.addr.lba && ms.addr.lba - 11400 > cd_fx[i - 1].lba)
				s2 = ms.addr.lba;
		if (!s2)
			LOG("cd: 2a sessao em %d nao bate com o TOC: tratado como sessao unica", ms.addr.lba);
	}
	int n = 0;
	if (s2)
		n += snprintf(cd_cue + n, sizeof cd_cue - n, "REM SESSION 01\n");
	for (int i = 0; i < cd_nfx && n < (int)sizeof cd_cue - 120; i++) {
		int ini = cd_fx[i].lba, f = i + 1 < cd_nfx ? cd_fx[i + 1].lba : fim;
		/* fim da 1a sessao = inicio da 2a menos lead-out (6750) + lead-in (4500) + pausa (150) */
		if (s2 && ini < s2 && f >= s2)
			f = s2 - 11400;
		if (f <= ini)
			f = ini + 1;
		cd_fx_fim[i] = f;
		if (s2 && ini == s2)
			n += snprintf(cd_cue + n, sizeof cd_cue - n, "REM SESSION 02\n");
		const char *tipo = !cd_fx[i].dados ? "AUDIO" : cd_fx[i].modo == 1 ? "MODE1/2352" : "MODE2/2352";
		n += snprintf(cd_cue + n, sizeof cd_cue - n, "FILE \"faixa%02d.bin\" BINARY\n  TRACK %02d %s\n    INDEX 01 00:00:00\n",
		              i + 1, i + 1, tipo);
	}
	LOG("cd: disco em faixas separadas (%d faixas, %s)", cd_nfx, s2 ? "2 sessoes" : "1 sessao");
}

static bool cd_abrir(const char *dev)
{
	cd_cache = calloc(ncache, sizeof *cd_cache);
	if (!cd_cache) {
		LOG("cd: sem memoria para o cache de %d blocos", ncache);
		return false;
	}
	for (int k = 0; k < ncache; k++)
		cd_cache[k].lba = -1;
	if (ncache > NCACHE)
		LOG("cd: cache de %d MB (%d blocos de %d setores)", (int)((int64_t)ncache * BLOCO * SETOR >> 20), ncache, BLOCO);
	cd_pf_base = -1;
	struct stat st;
	if (stat(dev, &st) == 0 && S_ISREG(st.st_mode)) {
		cd_arquivo = true;
		cd_fd = open(dev, O_RDONLY);
		cd_tam = st.st_size / SETOR * SETOR;
		cd_nfx = 1;
		cd_fx[0].dados = true;
		uint8_t h16[16] = {0};
		if (pread(cd_fd, h16, 16, 0) == 16 && h16[15] == 1)
			cd_fx[0].modo = 1; /* modo 1 (Sega CD, PC Engine): o cue tem que dizer */
		else
			cd_fx[0].modo = 2;
		snprintf(cd_cue, sizeof cd_cue, "FILE \"disco.bin\" BINARY\n  TRACK 01 MODE%d/2352\n    INDEX 01 00:00:00\n",
		         cd_fx[0].modo);
		/* teste no PC (sem leitora): RV_TESTE_FAIXAS="lba:a,lba:a" marca faixas de musica no
		 * .bin, roda a pre-carga e confere cada bloco guardado contra o arquivo */
		const char *tf = getenv("RV_TESTE_FAIXAS");
		if (tf) {
			for (const char *q = tf; *q && cd_nfx < 99; ) {
				int lba = atoi(q);
				cd_fx[cd_nfx].lba = lba;
				cd_fx[cd_nfx].dados = false;
				cd_fx[cd_nfx].modo = 0;
				cd_nfx++;
				q = strchr(q, ',');
				if (!q) break;
				q++;
			}
			cd_pre_musicas();
			int iguais = 0, difs = 0;
			uint8_t *a = malloc(BLOCO * SETOR), *b = malloc(BLOCO * SETOR);
			for (int i = 0; i < cd_nfixos; i++) {
				int64_t n = cd_ler(cd_fixos[i].lba * SETOR, a, BLOCO * SETOR);
				memset(b, 0, BLOCO * SETOR);
				if (pread(cd_fd, b, n, cd_fixos[i].lba * SETOR) < 0)
					n = -1;
				if (n > 0 && !memcmp(a, b, n)) iguais++; else difs++;
			}
			free(a); free(b);
			LOG("cd: auto-teste da musica guardada: %d blocos iguais ao disco, %d diferentes", iguais, difs);
		}
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
		cd_descanso_maximo();
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
		bool ant_dados = false;
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
			/* pausa de 2 s so existe com certeza na passagem dados -> musica; entre
			 * musicas o TOC nao diz, e marcar a pausa cortaria 2 s do fim da musica */
			if (!dados && ant >= 0 && ant_dados && lba - 150 > ant) {
				msf(lba - 150, m);
				n += snprintf(cd_cue + n, sizeof cd_cue - n, "    INDEX 00 %s\n", m);
			}
			msf(lba, m);
			n += snprintf(cd_cue + n, sizeof cd_cue - n, "    INDEX 01 %s\n", m);
			ant = lba;
			ant_dados = dados;
		}
		if (cd_faixas)
			cd_cue_faixas(fim);
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
		/* disco com musica (Sega CD, PC Engine CD): a 1a leitura de audio costuma
		 * trocar a rotacao da leitora (segundos). Faz isso agora, no carregamento,
		 * e nao no meio do jogo. */
		for (int i = 0; i < cd_nfx; i++)
			if (!cd_fx[i].dados) {
				t0 = agora();
				bool ok = cd_le_setor(cd_fx[i].lba + 16, b);
				LOG("cd: teste audio (faixa %d): %s (%.0f ms)", i + 1, ok ? "ok" : "FALHOU", (agora() - t0) * 1e3);
				t0 = agora();
				ok = cd_le_setor(l, b);
				LOG("cd: volta para dados: %s (%.0f ms)", ok ? "ok" : "FALHOU", (agora() - t0) * 1e3);
				break;
			}
		cd_pre_musicas();
		cd_ult_leitura = agora();
	}
	return cd_tam > 0;
}

/* ------------------------------------------------------------------ VFS */
enum { T_ARQ, T_CUE, T_BIN, T_FAIXA };
struct retro_vfs_file_handle {
	int tipo, fx;
	bool gravou;
	FILE *fp;
	char *path;
	int64_t pos;
};
struct retro_vfs_dir_handle {
	DIR *d;
	struct dirent *e;
	char *path;
};

/* caminho sem barras repetidas (o Flycast pede "/run/retrovolt/cd//faixa01.bin") */
static const char *vfs_limpo(const char *p, char *b, size_t n)
{
	size_t k = 0;
	for (; *p && k + 1 < n; p++)
		if (!(*p == '/' && k && b[k - 1] == '/'))
			b[k++] = *p;
	b[k] = 0;
	return b;
}

/* faixaNN.bin do --cd-faixas: devolve o indice (0..) ou -1 */
static int vfs_faixa(const char *p0)
{
	char b[512];
	const char *p = vfs_limpo(p0, b, sizeof b);
	const char *pre = CD_DIR "/faixa";
	size_t lp = strlen(pre);
	if (cd_fd < 0 || !cd_faixas || strncmp(p, pre, lp))
		return -1;
	int k = atoi(p + lp);
	return k >= 1 && k <= cd_nfx && !strcmp(p + lp + 2, ".bin") ? k - 1 : -1;
}
static int64_t faixa_tam(int i) { return (int64_t)(cd_fx_fim[i] - cd_fx[i].lba) * SETOR; }

static int vfs_tipo(const char *p0)
{
	char b[512];
	const char *p = vfs_limpo(p0, b, sizeof b);
	if (vfs_faixa(p) >= 0)
		return T_FAIXA;
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
	s->fx = t == T_FAIXA ? vfs_faixa(path) : 0;
	s->fp = fp;
	s->path = strdup(path);
	return s;
}
static int v_close(struct retro_vfs_file_handle *s)
{
	if (s->fp) {
		if (s->gravou) { /* memoria do Sega CD etc.: garante no pendrive antes de desligar */
			fflush(s->fp);
			fsync(fileno(s->fp));
		}
		fclose(s->fp);
	}
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
	if (s->tipo == T_FAIXA)
		return faixa_tam(s->fx);
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
	} else if (s->tipo == T_FAIXA) {
		int64_t tam = faixa_tam(s->fx);
		n = s->pos >= tam ? 0 : (int64_t)len < tam - s->pos ? (int64_t)len : tam - s->pos;
		if (n > 0)
			n = cd_ler((int64_t)cd_fx[s->fx].lba * SETOR + s->pos, buf, n);
	} else
		n = cd_ler(s->pos, buf, len);
	s->pos += n;
	return n;
}
static int64_t v_write(struct retro_vfs_file_handle *s, const void *buf, uint64_t len)
{
	if (s->tipo != T_ARQ)
		return -1;
	s->gravou = true;
	return (int64_t)fwrite(buf, 1, len, s->fp);
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
			*size = t == T_CUE ? (int32_t)strlen(cd_cue) : t == T_FAIXA ? (int32_t)faixa_tam(vfs_faixa(p))
			      : (int32_t)(cd_tam > 0x7fffffff ? 0x7fffffff : cd_tam);
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
		char *c = strdup(fk), *bar = strrchr(c, '/'), *dp = strchr(bar ? bar : c, ':');
		*dp = 0;
		fb.fake = c;
		int bits = 16; /* teste: RV_FAKEFB=arq:LxA ou arq:LxA:32 (tela de 32 bits) */
		sscanf(dp + 1, "%dx%d:%d", &fb.w, &fb.h, &bits);
		fb.bpp = bits == 32 ? 32 : 16;
		fb.stride = fb.w * fb.bpp / 8;
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
		uint8_t rgb[3];
		if (fb.bpp == 32) {
			uint32_t p = ((uint32_t *)fb.mem)[i];
			rgb[0] = p >> 16; rgb[1] = p >> 8; rgb[2] = p;
		} else {
			uint16_t p = ((uint16_t *)fb.mem)[i];
			rgb[0] = (p >> 11) << 3; rgb[1] = ((p >> 5) & 63) << 2; rgb[2] = (p & 31) << 3;
		}
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
static uint32_t *linha_pronta;
static uint32_t *linha_tmp;

/* desenha a imagem (w x h) no fb.back, ja com a convergencia.
 * geo_h: linhas do jogo usadas para a geometria (0 = h); efeitos: scanlines/CRT valem */
static void desenha_base(const void *data, unsigned w, unsigned h, size_t pitch, int fmt, double aspecto,
                         int geo_h, bool efeitos)
{
	/* jogo em 480i: no tubo em 480i e no LCD vao as 480 linhas; so o tubo em 240p
	 * (480i desligado ou indisponivel) usa uma linha sim, outra nao */
	bool alta = h > 300 && !geo_h;
	int passo = (alta && tubo && fb.h < 400 && !sem_filtro) ? 2 : 1;
	int eh = h / passo, lp, cw, x0, y0, dh;
	/* LCD: a ampliacao conta como 240 linhas (480i no 1440x900 = 3x, nao 1x) */
	int gh = geo_h ? geo_h : (alta && !tubo ? eh / 2 : eh);
	if (tubo) {
		lp = fb.h >= 400 && eh <= 300 ? 2 : 1; /* tubo em 480i: jogo 240p dobra, 480 vai linha a linha */
		cw = (int)lround(fb.w * conv_w);
		if (cw > fb.w) cw = fb.w;
		if (cw < (int)w) cw = w;
		x0 = (fb.w - cw) / 2 + conv_x;
		/* altura (LT/RT na convergencia do jogo): linhas do jogo esticadas/encolhidas */
		dh = (int)lround(eh * lp * conv_h);
		if (dh < 1) dh = 1;
		y0 = (fb.h - dh) / 2 + conv_y * (fb.h >= 400 ? 2 : 1);
	} else {
		lp = fb.h / gh;
		if (lp < 1) lp = 1;
		dh = gh * lp;
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
		free(linha_pronta);
		linha_pronta = malloc(sizeof(uint32_t) * (cw + 1));
	}
	int bpp = fb.bpp / 8;
	int i0 = x0 < 0 ? -x0 : 0;
	int i1 = x0 + cw > fb.w ? fb.w - x0 : cw;
	/* desempenho (Dreamcast 1280x480 no LCD): a linha da tela e montada UMA vez (so os
	 * pontos que aparecem) e copiada para as linhas repetidas; efeito so onde muda */
	bool reduz = cw <= (int)w;               /* imagem mais larga que a area: so os usados */
	bool direto = fmt == RETRO_PIXEL_FORMAT_XRGB8888 && bpp == 4;
	for (int j = 0; j < eh; j++) {
		const uint8_t *src = (const uint8_t *)data + (size_t)j * passo * pitch;
		if (direto) {
			const uint32_t *s32 = (const uint32_t *)src;
			for (int i = i0; i < i1; i++)
				linha_pronta[i] = s32[xmap[i]];
		} else if (reduz) {
			for (int i = i0; i < i1; i++)
				linha_pronta[i] = px_conv(src, xmap[i], fmt);
		} else {
			for (unsigned x = 0; x < w; x++)
				linha_tmp[x] = px_conv(src, x, fmt);
			for (int i = i0; i < i1; i++)
				linha_pronta[i] = linha_tmp[xmap[i]];
		}
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
			int ef = (efeitos && !tubo && lp >= 2 && !sem_filtro && filtro < F_AMPLIA) ? filtro : 0;
			int linha_esc = (ef == 1 || ef == 3) && k == lpj - 1 ? 2 : (ef == 2 && k == lpj - 1) ? 1 : 0;
			bool grade = ef == 3;
			if (!linha_esc && !grade) { /* linha sem efeito: copia a linha pronta */
				if (bpp == 2) {
					uint16_t *o = (uint16_t *)d;
					for (int i = i0; i < i1; i++)
						o[i] = (uint16_t)linha_pronta[i];
				} else
					memcpy((uint32_t *)d + i0, linha_pronta + i0, (size_t)(i1 - i0) * 4);
				continue;
			}
			if (bpp == 2) {
				uint16_t *o = (uint16_t *)d;
				for (int i = i0; i < i1; i++) {
					uint16_t p = linha_pronta[i];
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
					uint32_t p = linha_pronta[i];
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

/* ---- ampliadores do LCD (no processador; nada a baixar). Trabalham em 0xRRGGBB. */
static uint32_t *amp_src, *amp_dst;
static size_t amp_src_n, amp_dst_n;

static uint32_t px_rgb(const uint8_t *src, unsigned x, int fmt)
{
	unsigned r, g, b;
	if (fmt == RETRO_PIXEL_FORMAT_RGB565) {
		uint16_t p = ((const uint16_t *)src)[x];
		r = (p >> 11) << 3; g = ((p >> 5) & 63) << 2; b = (p & 31) << 3;
		r |= r >> 5; g |= g >> 6; b |= b >> 5;
	} else if (fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
		return ((const uint32_t *)src)[x] & 0xFFFFFF;
	} else {
		uint16_t p = ((const uint16_t *)src)[x];
		r = ((p >> 10) & 31) << 3; g = ((p >> 5) & 31) << 3; b = (p & 31) << 3;
		r |= r >> 5; g |= g >> 5; b |= b >> 5;
	}
	return (r << 16) | (g << 8) | b;
}

/* distancia de cor (parecida com YUV: o olho ve mais o brilho) */
static int cor_dist(uint32_t a, uint32_t b)
{
	int dr = (int)((a >> 16) & 255) - (int)((b >> 16) & 255);
	int dg = (int)((a >> 8) & 255) - (int)((b >> 8) & 255);
	int db = (int)(a & 255) - (int)(b & 255);
	int y = (dr * 299 + dg * 587 + db * 114) / 1000;
	int u = db - y, v = dr - y;
	return 48 * abs(y) + 7 * abs(u) + 6 * abs(v);
}

static uint32_t cor_mistura(uint32_t a, uint32_t b) /* 50% */
{
	return ((a >> 1) & 0x7F7F7F) + ((b >> 1) & 0x7F7F7F) + (a & b & 0x010101);
}

#define PX(xx, yy) S[(size_t)((yy) < 0 ? 0 : (yy) >= (int)h ? (int)h - 1 : (yy)) * w + \
                     ((xx) < 0 ? 0 : (xx) >= (int)w ? (int)w - 1 : (xx))]

static void amp_scale2x(const uint32_t *S, unsigned w, unsigned h, uint32_t *O)
{
	unsigned ow = w * 2;
	for (int y = 0; y < (int)h; y++)
		for (int x = 0; x < (int)w; x++) {
			uint32_t B = PX(x, y - 1), D = PX(x - 1, y), E = PX(x, y), F = PX(x + 1, y), H = PX(x, y + 1);
			uint32_t e0 = E, e1 = E, e2 = E, e3 = E;
			if (B != H && D != F) {
				if (D == B) e0 = D;
				if (B == F) e1 = F;
				if (D == H) e2 = D;
				if (H == F) e3 = F;
			}
			uint32_t *o = O + (size_t)y * 2 * ow + x * 2;
			o[0] = e0; o[1] = e1; o[ow] = e2; o[ow + 1] = e3;
		}
}

static void amp_scale3x(const uint32_t *S, unsigned w, unsigned h, uint32_t *O)
{
	unsigned ow = w * 3;
	for (int y = 0; y < (int)h; y++)
		for (int x = 0; x < (int)w; x++) {
			uint32_t A = PX(x - 1, y - 1), B = PX(x, y - 1), C = PX(x + 1, y - 1);
			uint32_t D = PX(x - 1, y), E = PX(x, y), F = PX(x + 1, y);
			uint32_t G = PX(x - 1, y + 1), H = PX(x, y + 1), I = PX(x + 1, y + 1);
			uint32_t e[9] = {E, E, E, E, E, E, E, E, E};
			if (B != H && D != F) {
				e[0] = D == B ? D : E;
				e[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
				e[2] = B == F ? F : E;
				e[3] = (D == B && E != G) || (D == H && E != A) ? D : E;
				e[5] = (B == F && E != I) || (H == F && E != C) ? F : E;
				e[6] = D == H ? D : E;
				e[7] = (D == H && E != I) || (H == F && E != G) ? H : E;
				e[8] = H == F ? F : E;
			}
			uint32_t *o = O + (size_t)y * 3 * ow + x * 3;
			for (int k = 0; k < 3; k++)
				for (int m = 0; m < 3; m++)
					o[k * ow + m] = e[k * 3 + m];
		}
}

/* xBR 2x (regras do nivel 1 de Hyllian, escrito aqui): para cada canto do ponto,
 * compara a "forca" das duas diagonais e, se a borda passa pelo canto, puxa a cor
 * do vizinho mais parecido (meio a meio, para a borda sair lisa) */
static void amp_xbr2x(const uint32_t *S, unsigned w, unsigned h, uint32_t *O)
{
	unsigned ow = w * 2;
	for (int y = 0; y < (int)h; y++)
		for (int x = 0; x < (int)w; x++) {
			uint32_t E = PX(x, y);
			uint32_t *o = O + (size_t)y * 2 * ow + x * 2;
			for (int c = 0; c < 4; c++) {
				int sx = (c & 1) ? 1 : -1, sy = (c & 2) ? 1 : -1;
				uint32_t F = PX(x + sx, y), H = PX(x, y + sy), I = PX(x + sx, y + sy);
				uint32_t res = E;
				if (E != F && E != H) {
					uint32_t B = PX(x, y - sy), D = PX(x - sx, y);
					uint32_t C = PX(x + sx, y - sy), G = PX(x - sx, y + sy);
					uint32_t F4 = PX(x + 2 * sx, y), H5 = PX(x, y + 2 * sy);
					uint32_t I4 = PX(x + 2 * sx, y + sy), I5 = PX(x + sx, y + 2 * sy);
					int e1 = cor_dist(E, C) + cor_dist(E, G) + cor_dist(I, F4) + cor_dist(I, H5) + 4 * cor_dist(H, F);
					int e2 = cor_dist(H, D) + cor_dist(H, I5) + cor_dist(F, I4) + cor_dist(F, B) + 4 * cor_dist(E, I);
					if (e1 < e2)
						res = cor_mistura(E, cor_dist(E, F) <= cor_dist(E, H) ? F : H);
				}
				o[((c & 2) ? ow : 0) + ((c & 1) ? 1 : 0)] = res;
			}
		}
}

/* suave: interpolacao linear k vezes (k fica perto da resolucao da tela).
 * Vermelho e azul juntos num inteiro (0x00RR00BB) e verde separado: 2 contas por ponto. */
static int *suave_x0, *suave_tx, suave_n;
static inline uint32_t lerp_px(uint32_t a, uint32_t b, unsigned t)
{
	uint32_t rb = (((a & 0xFF00FF) * (256 - t) + (b & 0xFF00FF) * t) >> 8) & 0xFF00FF;
	uint32_t g = (((a & 0x00FF00) * (256 - t) + (b & 0x00FF00) * t) >> 8) & 0x00FF00;
	return rb | g;
}
static void amp_suave(const uint32_t *S, unsigned w, unsigned h, uint32_t *O, int k)
{
	unsigned ow = w * k, oh = h * k;
	if ((int)ow > suave_n) {
		free(suave_x0); free(suave_tx);
		suave_x0 = malloc(sizeof(int) * ow); suave_tx = malloc(sizeof(int) * ow);
		suave_n = ow;
	}
	for (unsigned ox = 0; ox < ow; ox++) {
		int fx = (int)(((2 * ox + 1) * 256) / (2 * k)) - 128; /* centro do ponto, em 1/256 */
		int x0 = fx >> 8, tx = fx & 255;
		if (x0 < 0) { x0 = 0; tx = 0; }
		if (x0 >= (int)w - 1) { x0 = w - 1; tx = 0; }
		suave_x0[ox] = x0; suave_tx[ox] = tx;
	}
	for (unsigned oy = 0; oy < oh; oy++) {
		int fy = (int)(((2 * oy + 1) * 256) / (2 * k)) - 128;
		int y0 = fy >> 8, ty = fy & 255;
		if (y0 < 0) { y0 = 0; ty = 0; }
		if (y0 >= (int)h - 1) { y0 = h - 1; ty = 0; }
		const uint32_t *r0 = S + (size_t)y0 * w, *r1 = ty ? r0 + w : r0;
		uint32_t *o = O + (size_t)oy * ow;
		for (unsigned ox = 0; ox < ow; ox++) {
			int x0 = suave_x0[ox], tx = suave_tx[ox], x1 = tx ? x0 + 1 : x0;
			uint32_t top = lerp_px(r0[x0], r0[x1], tx);
			o[ox] = ty ? lerp_px(top, lerp_px(r1[x0], r1[x1], tx), ty) : top;
		}
	}
}
#undef PX

static void desenha(const void *data, unsigned w, unsigned h, size_t pitch, int fmt, double aspecto)
{
	/* ampliadores so no LCD, em jogo de baixa resolucao; senao o desenho normal */
	if (tubo || sem_filtro || filtro < F_AMPLIA || w > 400 || h > 300 || !w || !h) {
		desenha_base(data, w, h, pitch, fmt, aspecto, 0, true);
		return;
	}
	int k = filtro == 6 ? 3 : 2;
	if (filtro == 4) {
		k = fb.h / (int)h;
		if (k > 3) k = 3; /* 4x custava ~9 ms por quadro no PC: 3x e o limite seguro no Pi */
		if (k < 2) k = 2;
	}
	size_t ns = (size_t)w * h, nd = ns * k * k;
	if (ns > amp_src_n) { free(amp_src); amp_src = malloc(ns * 4); amp_src_n = ns; }
	if (nd > amp_dst_n) { free(amp_dst); amp_dst = malloc(nd * 4); amp_dst_n = nd; }
	for (unsigned y = 0; y < h; y++) {
		const uint8_t *src = (const uint8_t *)data + (size_t)y * pitch;
		for (unsigned x = 0; x < w; x++)
			amp_src[(size_t)y * w + x] = px_rgb(src, x, fmt);
	}
	if (filtro == 4)
		amp_suave(amp_src, w, h, amp_dst, k);
	else if (filtro == 5)
		amp_scale2x(amp_src, w, h, amp_dst);
	else if (filtro == 6)
		amp_scale3x(amp_src, w, h, amp_dst);
	else
		amp_xbr2x(amp_src, w, h, amp_dst);
	desenha_base(amp_dst, w * k, h * k, (size_t)w * k * 4, RETRO_PIXEL_FORMAT_XRGB8888, aspecto, h, false);
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


/* ------------------------------------------------------------------ saida do jogo (KMS)
 * O boot deixa a tela no fbdev em 16 bits (RGB565): branco quase puro vira esverdeado e
 * degrade vira degrau. Durante o jogo o rv-jogo usa telas proprias em 32 bits (dumb
 * buffers) no mesmo modo de video. No tubo, jogo em 480i: troca o HDMI para 480i (montado
 * a partir do 240p em uso: mesmo relogio e linha = mesmos 15,7 kHz; o kernel lista o 480i
 * da EDID como "1920x240i") e desenha as 480 linhas. Ao sair: volta o fbdev do boot. Se o
 * rv-jogo cair, o kernel devolve o modo do fbdev ao fechar o DRM.
 * --saida-32 0 desliga tudo (fica no fbdev 16 bits, 240p fixo); --tubo-480i 0 so o 480i. */
static int tubo_480i_ok = 1, saida32_ok = 1;
typedef struct { uint32_t fb_id; uint8_t *map; int pitch, h; size_t tam; } KmBuf;
static struct {
	bool pode, ativo, pode480, em480;
	int fd;
	uint32_t crtc, conn, fb_boot;
	struct drm_mode_modeinfo m_boot, m480;
	KmBuf b, b480;
	uint8_t *mem_boot;
	int stride_boot, h_boot, bpp_boot;
	int quer, conta; /* modo pedido pelo jogo e ha quantos quadros (evita piscar) */
} km = {.fd = -1};
static double km_tela_hz; /* Hz do modo novo depois de uma troca; o laco do jogo usa */

static double km_hz(const struct drm_mode_modeinfo *m)
{
	double hz = m->clock * 1000.0 / ((double)m->htotal * m->vtotal);
	return m->flags & DRM_MODE_FLAG_INTERLACE ? hz * 2 : hz;
}

static void km_monta_480i(void)
{
	struct drm_mode_modeinfo m = km.m_boot;
	m.vdisplay = 480;
	m.vsync_start = 480 + 6;  /* 3 linhas de frente por campo */
	m.vsync_end = 480 + 12;   /* 3 de sincronismo por campo */
	m.vtotal = 525;
	m.flags |= DRM_MODE_FLAG_INTERLACE;
	m.type = 1 << 5; /* DRM_MODE_TYPE_USERDEF */
	m.vrefresh = (unsigned)lround(km_hz(&m));
	snprintf(m.name, sizeof m.name, "%ux480i", m.hdisplay);
	km.m480 = m;
}

static bool km_buf(KmBuf *b, int w, int h)
{
	struct drm_mode_create_dumb cd;
	memset(&cd, 0, sizeof cd);
	cd.width = w;
	cd.height = h;
	cd.bpp = 32;
	if (ioctl(km.fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd))
		return false;
	struct drm_mode_fb_cmd fc;
	memset(&fc, 0, sizeof fc);
	fc.width = w;
	fc.height = h;
	fc.pitch = cd.pitch;
	fc.bpp = 32;
	fc.depth = 24;
	fc.handle = cd.handle;
	struct drm_mode_map_dumb md;
	memset(&md, 0, sizeof md);
	md.handle = cd.handle;
	if (ioctl(km.fd, DRM_IOCTL_MODE_ADDFB, &fc) || ioctl(km.fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
		return false;
	b->map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, km.fd, md.offset);
	if (b->map == MAP_FAILED) {
		b->map = NULL;
		return false;
	}
	memset(b->map, 0, cd.size);
	b->fb_id = fc.fb_id;
	b->pitch = cd.pitch;
	b->h = h;
	b->tam = (size_t)cd.pitch * h;
	return true;
}

/* acha o conector/CRTC que esta mostrando o fbdev (modo com o tamanho do fb0) */
static void km_prepara(void)
{
	if (fb.fake || !saida32_ok)
		return;
	char p[32];
	for (int c = 0; c < 4 && !km.pode; c++) {
		snprintf(p, sizeof p, "/dev/dri/card%d", c);
		int fd = open(p, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		uint32_t cons[16];
		struct drm_mode_card_res r;
		memset(&r, 0, sizeof r);
		if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) || !r.count_connectors) {
			close(fd);
			continue;
		}
		int nc = r.count_connectors > 16 ? 16 : r.count_connectors;
		memset(&r, 0, sizeof r);
		r.connector_id_ptr = (uintptr_t)cons;
		r.count_connectors = nc;
		if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r)) {
			close(fd);
			continue;
		}
		for (int i = 0; i < nc && !km.pode; i++) {
			struct drm_mode_get_connector gc;
			memset(&gc, 0, sizeof gc);
			gc.connector_id = cons[i];
			if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) || gc.connection != 1 || !gc.encoder_id)
				continue;
			struct drm_mode_get_encoder ge;
			memset(&ge, 0, sizeof ge);
			ge.encoder_id = gc.encoder_id;
			if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &ge) || !ge.crtc_id)
				continue;
			struct drm_mode_crtc cr;
			memset(&cr, 0, sizeof cr);
			cr.crtc_id = ge.crtc_id;
			if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &cr) || !cr.mode_valid || !cr.fb_id ||
			    cr.mode.hdisplay != fb.w || cr.mode.vdisplay != fb.h)
				continue;
			km.fd = fd;
			km.crtc = ge.crtc_id;
			km.conn = cons[i];
			km.fb_boot = cr.fb_id;
			km.m_boot = cr.mode;
			km.pode = true;
		}
		if (!km.pode)
			close(fd);
	}
	if (!km.pode) {
		LOG("saida: tela do boot nao encontrada no DRM: fica no fbdev (%d bits)", fb.bpp);
		return;
	}
	if (!km_buf(&km.b, fb.w, fb.h)) {
		LOG("saida: tela de 32 bits recusada (%s): fica no fbdev", strerror(errno));
		km.pode = false;
		return;
	}
	km.mem_boot = fb.mem;
	km.stride_boot = fb.stride;
	km.h_boot = fb.h;
	km.bpp_boot = fb.bpp;
	if (tubo && tubo_480i_ok) {
		km_monta_480i();
		km.pode480 = km_buf(&km.b480, fb.w, 480);
		if (km.pode480)
			LOG("tubo: 480i pronto: %ux480 total %ux%u, %.3f Hz por campo", km.m480.hdisplay, km.m480.htotal,
			    km.m480.vtotal, km_hz(&km.m480));
	}
}

static void fb_aponta(uint8_t *mem, int stride, int h, int bpp)
{
	fb.mem = mem;
	fb.stride = stride;
	fb.h = h;
	fb.bpp = bpp;
	fb.tam = (size_t)stride * h;
	free(fb.back);
	fb.back = calloc(1, fb.tam);
	ult_x0 = -99999; /* geometria nova: o desenho limpa e recalcula */
	osd_w = 0;       /* o aviso de resolucao mostra a saida nova */
	xmap_w = -1;     /* pontos convertidos de novo para o formato novo */
}

static bool km_crtc(uint32_t fb_id, const struct drm_mode_modeinfo *m)
{
	ioctl(km.fd, DRM_IOCTL_SET_MASTER, 0);
	struct drm_mode_crtc c;
	memset(&c, 0, sizeof c);
	c.crtc_id = km.crtc;
	c.fb_id = fb_id;
	c.set_connectors_ptr = (uintptr_t)&km.conn;
	c.count_connectors = 1;
	c.mode = *m;
	c.mode_valid = 1;
	return ioctl(km.fd, DRM_IOCTL_MODE_SETCRTC, &c) == 0;
}

/* comeco do jogo: tela de 32 bits no mesmo modo do boot */
static void km_entra(void)
{
	if (!km.pode)
		return;
	if (!km_crtc(km.b.fb_id, &km.m_boot)) {
		LOG("saida: troca para 32 bits recusada (%s): fica no fbdev", strerror(errno));
		km.pode = false;
		return;
	}
	km.ativo = true;
	fb_aponta(km.b.map, km.b.pitch, km.b.h, 32);
	LOG("saida: tela do jogo em 32 bits (%s)", km.m_boot.name);
}

static bool km_modo(bool p480)
{
	if (!km.ativo || !km.pode480 || p480 == km.em480)
		return true;
	const struct drm_mode_modeinfo *m = p480 ? &km.m480 : &km.m_boot;
	KmBuf *b = p480 ? &km.b480 : &km.b;
	if (!km_crtc(b->fb_id, m)) {
		LOG("tubo: troca para %s recusada (%s)%s", p480 ? "480i" : "240p", strerror(errno),
		    p480 ? ": fica em 240p" : "");
		if (p480)
			km.pode480 = false;
		return false;
	}
	km.em480 = p480;
	fb_aponta(b->map, b->pitch, b->h, 32);
	vs_seq = 0;                /* o vsync recomeca a contagem */
	km_tela_hz = km_hz(m);     /* o Pi avisa o vblank por campo: 59,9 Hz no 480i (LOG do autor) */
	LOG("tubo: saida agora %s; tela %.3f Hz", p480 ? "480i (entrelacado, 480 linhas)" : "240p", km_tela_hz);
	return true;
}

/* fim do jogo: devolve o fbdev do boot (o menu do RetroVolt desenha nele) */
static void km_sai(void)
{
	if (!km.ativo)
		return;
	if (!km_crtc(km.fb_boot, &km.m_boot))
		LOG("saida: devolver a tela do boot falhou (%s)", strerror(errno));
	km.ativo = km.em480 = false;
	fb_aponta(km.mem_boot, km.stride_boot, km.h_boot, km.bpp_boot);
}

/* chamado a cada quadro do jogo: 480i so depois de 20 quadros seguidos pedindo
 * (os 3D do Dreamcast alternam 640x478/1280x480 com videos em 640x238) */
static void km_quadro(unsigned h)
{
	if (!km.ativo || !km.pode480)
		return;
	int q = h > 300;
	if (q != km.quer) {
		km.quer = q;
		km.conta = 0;
	}
	if (++km.conta == 20)
		km_modo(q);
}

/* sinal fatal: devolve a tela do boot antes de morrer (nunca ficar sem imagem) */
static void km_fatal(int s)
{
	km_sai();
	signal(s, SIG_DFL);
	raise(s);
}

/* ------------------------------------------------------------------ GPU (nucleos 3D: Flycast)
 * Nucleo que desenha com OpenGL ES (SET_HW_RENDER): contexto EGL sem janela na GPU do
 * Pi (V3D, /dev/dri/renderD128), o nucleo desenha num FBO e o quadro pronto volta para a
 * memoria (glReadPixels) e segue o caminho de sempre (tubo, convergencia, efeitos, menu).
 * Tudo carregado na hora (dlopen): quem nao usa GPU nao depende de nada novo. */
typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface;
typedef int32_t EGLint;
typedef unsigned EGLBoolean, EGLenum;
typedef intptr_t EGLAttrib;
#define EGL_NONE 0x3038
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_SURFACE_TYPE 0x3033
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_PLATFORM_GBM_KHR 0x31D7
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_EXTENSIONS 0x3055
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_BGRA_EXT 0x80E1
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_SCISSOR_TEST 0x0C11
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_LINEAR 0x2601
#define GL_STREAM_READ 0x88E1
#define GL_MAP_READ_BIT 0x0001
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001

static struct {
	void *hegl, *hgles, *hgbm;
	EGLDisplay dpy;
	EGLContext ctx;
	int gbm_fd;
	void *gbm;
	unsigned fbo, tex, rb;
	unsigned w, h;
	bool ativo;           /* o nucleo pediu GPU e o contexto existe */
	bool bgra;            /* GL_EXT_read_format_bgra: le ja no formato da tela (sem trocar cor) */
	struct retro_hw_render_callback cb;
	uint32_t *q;          /* quadro lido da GPU, ja em XRGB8888 de cima para baixo */
	uint32_t *tmp;
	size_t cap;
	/* leitura em 2o plano: a GPU reduz (se o jogo desenhou largo demais) num FBO pequeno,
	 * copia para um PBO e o rv-jogo pega o quadro ANTERIOR, que ja esta pronto (sem esperar) */
	bool assinc;
	unsigned fbo2, tex2, w2, h2;  /* FBO reduzido */
	unsigned pbo[2];
	size_t pbo_cap[2];
	unsigned pw[2], ph[2];        /* tamanho guardado em cada PBO (0 = vazio) */
	void *sinc[2];
	int vez;
	/* EGL */
	EGLDisplay (*GetPlatformDisplay)(EGLenum, void *, const EGLAttrib *);
	EGLDisplay (*GetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
	EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
	EGLBoolean (*BindAPI)(EGLenum);
	EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
	EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
	EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
	EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext);
	EGLBoolean (*Terminate)(EGLDisplay);
	const char *(*QueryString)(EGLDisplay, EGLint);
	void *(*GetProcAddress)(const char *);
	/* GL */
	void (*GenFramebuffers)(int, unsigned *);
	void (*BindFramebuffer)(unsigned, unsigned);
	void (*FramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
	void (*GenRenderbuffers)(int, unsigned *);
	void (*BindRenderbuffer)(unsigned, unsigned);
	void (*RenderbufferStorage)(unsigned, unsigned, int, int);
	void (*FramebufferRenderbuffer)(unsigned, unsigned, unsigned, unsigned);
	unsigned (*CheckFramebufferStatus)(unsigned);
	void (*DeleteFramebuffers)(int, const unsigned *);
	void (*DeleteRenderbuffers)(int, const unsigned *);
	void (*GenTextures)(int, unsigned *);
	void (*DeleteTextures)(int, const unsigned *);
	void (*BindTexture)(unsigned, unsigned);
	void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
	void (*TexParameteri)(unsigned, unsigned, int);
	void (*PixelStorei)(unsigned, int);
	void (*ReadPixels)(int, int, int, int, unsigned, unsigned, void *);
	void (*GetIntegerv)(unsigned, int *);
	void (*BindBuffer)(unsigned, unsigned);
	const unsigned char *(*GetString)(unsigned);
	void (*BlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
	void (*GenBuffers)(int, unsigned *);
	void (*DeleteBuffers)(int, const unsigned *);
	void (*BufferData)(unsigned, intptr_t, const void *, unsigned);
	void *(*MapBufferRange)(unsigned, intptr_t, intptr_t, unsigned);
	unsigned char (*UnmapBuffer)(unsigned);
	void *(*FenceSync)(unsigned, unsigned);
	unsigned (*ClientWaitSync)(void *, unsigned, uint64_t);
	void (*DeleteSync)(void *);
	unsigned char (*IsEnabled)(unsigned);
	void (*Flush)(void);
	void (*Enable)(unsigned);
	void (*Disable)(unsigned);
} gl = {.gbm_fd = -1};

static retro_proc_address_t gl_proc(const char *n)
{
	void *p = gl.GetProcAddress ? gl.GetProcAddress(n) : NULL;
	if (!p && gl.hgles)
		p = dlsym(gl.hgles, n);
	return (retro_proc_address_t)p;
}
static uintptr_t gl_fb_atual(void) { return gl.fbo; }

static void *gl_abre_lib(const char *const *nomes)
{
	for (int i = 0; nomes[i]; i++) {
		void *h = dlopen(nomes[i], RTLD_NOW | RTLD_GLOBAL);
		if (h)
			return h;
	}
	return NULL;
}

/* display EGL: 1o sem janela (Mesa "surfaceless", escolhe a GPU de render sozinho);
 * se nao der, GBM em /dev/dri/renderD128 */
static bool gl_display(void)
{
	*(void **)&gl.GetPlatformDisplay = dlsym(gl.hegl, "eglGetPlatformDisplay");
	*(void **)&gl.GetPlatformDisplayEXT = gl.GetProcAddress ? gl.GetProcAddress("eglGetPlatformDisplayEXT") : NULL;
	EGLint ma, mi;
	for (int tent = 0; tent < 2; tent++) {
		void *nativo = NULL;
		EGLenum plat = EGL_PLATFORM_SURFACELESS_MESA;
		if (tent == 1) {
			static const char *const lg[] = {"libgbm.so.1", "libgbm.so", NULL};
			gl.hgbm = gl_abre_lib(lg);
			void *(*cria)(int) = gl.hgbm ? (void *(*)(int))dlsym(gl.hgbm, "gbm_create_device") : NULL;
			if (!cria)
				break;
			gl.gbm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
			if (gl.gbm_fd < 0)
				break;
			gl.gbm = cria(gl.gbm_fd);
			if (!gl.gbm)
				break;
			nativo = gl.gbm;
			plat = EGL_PLATFORM_GBM_KHR;
		}
		gl.dpy = gl.GetPlatformDisplay ? gl.GetPlatformDisplay(plat, nativo, NULL)
		       : gl.GetPlatformDisplayEXT ? gl.GetPlatformDisplayEXT(plat, nativo, NULL) : NULL;
		if (gl.dpy && gl.Initialize(gl.dpy, &ma, &mi)) {
			LOG("gpu: EGL %d.%d (%s)", ma, mi, tent ? "gbm" : "sem janela");
			return true;
		}
		gl.dpy = NULL;
	}
	return false;
}

static bool gl_cria_contexto(void)
{
	static const char *const le[] = {"libEGL.so.1", "libEGL.so", NULL};
	static const char *const lg[] = {"libGLESv2.so.2", "libGLESv2.so", NULL};
	gl.hegl = gl_abre_lib(le);
	gl.hgles = gl_abre_lib(lg);
	if (!gl.hegl || !gl.hgles) {
		LOG("gpu: ERRO: sem libEGL/libGLESv2 (instalar libegl1 libgles2)");
		return false;
	}
#define EGLF(n) *(void **)&gl.n = dlsym(gl.hegl, "egl" #n)
	EGLF(Initialize); EGLF(BindAPI); EGLF(ChooseConfig); EGLF(CreateContext); EGLF(MakeCurrent);
	EGLF(DestroyContext); EGLF(Terminate); EGLF(QueryString); EGLF(GetProcAddress);
#undef EGLF
	if (!gl.Initialize || !gl.CreateContext || !gl.MakeCurrent || !gl_display()) {
		LOG("gpu: ERRO: EGL nao abriu a GPU");
		return false;
	}
	gl.BindAPI(EGL_OPENGL_ES_API);
	const char *ext = gl.QueryString ? gl.QueryString(gl.dpy, EGL_EXTENSIONS) : "";
	EGLConfig cfg = NULL; /* EGL_NO_CONFIG_KHR: sem janela nao precisa de config */
	if (!ext || !strstr(ext, "EGL_KHR_no_config_context")) {
		const EGLint at[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
		EGLint n = 0;
		if (!gl.ChooseConfig(gl.dpy, at, &cfg, 1, &n) || n < 1) {
			LOG("gpu: ERRO: nenhuma configuracao GLES3");
			return false;
		}
	}
	/* GLES 3.2 -> 3.1 -> 3.0 (o Pi 5 faz 3.1; o nucleo pede 3.0 ou 2.0, tudo cabe) */
	static const int vers[][2] = {{3, 2}, {3, 1}, {3, 0}, {2, 0}};
	for (int i = 0; i < 4 && !gl.ctx; i++) {
		if ((int)gl.cb.version_major > vers[i][0] ||
		    ((int)gl.cb.version_major == vers[i][0] && (int)gl.cb.version_minor > vers[i][1]))
			break;
		const EGLint at[] = {EGL_CONTEXT_MAJOR_VERSION, vers[i][0], EGL_CONTEXT_MINOR_VERSION, vers[i][1], EGL_NONE};
		gl.ctx = gl.CreateContext(gl.dpy, cfg, NULL, at);
	}
	if (!gl.ctx || !gl.MakeCurrent(gl.dpy, NULL, NULL, gl.ctx)) {
		LOG("gpu: ERRO: contexto GLES nao foi criado");
		return false;
	}
#define GLF(n) *(void **)&gl.n = (void *)gl_proc("gl" #n)
	GLF(GenFramebuffers); GLF(BindFramebuffer); GLF(FramebufferTexture2D); GLF(GenRenderbuffers);
	GLF(BindRenderbuffer); GLF(RenderbufferStorage); GLF(FramebufferRenderbuffer); GLF(CheckFramebufferStatus);
	GLF(DeleteFramebuffers); GLF(DeleteRenderbuffers); GLF(GenTextures); GLF(DeleteTextures); GLF(BindTexture);
	GLF(TexImage2D); GLF(TexParameteri); GLF(PixelStorei); GLF(ReadPixels); GLF(GetIntegerv); GLF(BindBuffer);
	GLF(GetString); GLF(BlitFramebuffer); GLF(GenBuffers); GLF(DeleteBuffers); GLF(BufferData);
	GLF(MapBufferRange); GLF(UnmapBuffer); GLF(FenceSync); GLF(ClientWaitSync); GLF(DeleteSync);
	GLF(IsEnabled); GLF(Enable); GLF(Disable); GLF(Flush);
#undef GLF
	if (!gl.ReadPixels || !gl.GenFramebuffers || !gl.TexImage2D) {
		LOG("gpu: ERRO: GLES incompleto");
		return false;
	}
	LOG("gpu: %s / %s", gl.GetString ? (const char *)gl.GetString(GL_RENDERER) : "?",
	    gl.GetString ? (const char *)gl.GetString(GL_VERSION) : "?");
	const char *gx = gl.GetString ? (const char *)gl.GetString(GL_EXTENSIONS) : NULL;
	gl.bgra = gx && strstr(gx, "GL_EXT_read_format_bgra");
	gl.assinc = gl.BlitFramebuffer && gl.GenBuffers && gl.BufferData && gl.MapBufferRange && gl.UnmapBuffer &&
	            gl.FenceSync && gl.ClientWaitSync && gl.DeleteSync && gl.IsEnabled && gl.Enable && gl.Disable &&
	            gl.DeleteBuffers && gl.Flush;
	LOG("gpu: leitura do quadro %s", gl.assinc ? "em 2o plano (+1 quadro), reduzida a largura do console" : "direta");
	return true;
}

/* FBO do tamanho maximo do jogo (cor RGBA8 + profundidade/stencil) */
static bool gl_cria_fbo(unsigned w, unsigned h)
{
	if (w < 1) w = 640;
	if (h < 1) h = 480;
	if (w > 4096) w = 4096;
	if (h > 4096) h = 4096;
	if (gl.fbo) {
		gl.DeleteFramebuffers(1, &gl.fbo);
		gl.DeleteTextures(1, &gl.tex);
		if (gl.rb)
			gl.DeleteRenderbuffers(1, &gl.rb);
		gl.fbo = gl.tex = gl.rb = 0;
	}
	gl.GenTextures(1, &gl.tex);
	gl.BindTexture(GL_TEXTURE_2D, gl.tex);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	gl.BindTexture(GL_TEXTURE_2D, 0);
	gl.GenFramebuffers(1, &gl.fbo);
	gl.BindFramebuffer(GL_FRAMEBUFFER, gl.fbo);
	gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl.tex, 0);
	if (gl.cb.depth || gl.cb.stencil) {
		gl.GenRenderbuffers(1, &gl.rb);
		gl.BindRenderbuffer(GL_RENDERBUFFER, gl.rb);
		gl.RenderbufferStorage(GL_RENDERBUFFER, gl.cb.stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24, w, h);
		gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, gl.cb.stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
		                           GL_RENDERBUFFER, gl.rb);
		gl.BindRenderbuffer(GL_RENDERBUFFER, 0);
	}
	unsigned st = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
	gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
	if (st != GL_FRAMEBUFFER_COMPLETE) {
		LOG("gpu: ERRO: FBO %ux%u incompleto (0x%x)", w, h, st);
		return false;
	}
	gl.w = w;
	gl.h = h;
	LOG("gpu: FBO %ux%u%s", w, h, gl.rb ? " com profundidade" : "");
	return true;
}

#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_RENDERBUFFER_BINDING 0x8CA7
/* o nucleo passou a desenhar maior que o FBO (Flycast: 3D em 480i sai com o dobro da
 * largura, 1280 ou 1600, e avisa por SET_SYSTEM_AV_INFO): aumenta o FBO SEM trocar o
 * objeto (o nucleo guarda o numero dele) e devolve o estado do GL como estava, porque
 * o Flycast guarda em cache o que esta ligado */
static void gl_aumenta(unsigned w, unsigned h)
{
	if (!gl.ativo || (w <= gl.w && h <= gl.h))
		return;
	if (w < gl.w) w = gl.w;
	if (h < gl.h) h = gl.h;
	if (w > 4096) w = 4096;
	if (h > 4096) h = 4096;
	int tex = 0, rbo = 0;
	gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
	gl.GetIntegerv(GL_RENDERBUFFER_BINDING, &rbo);
	gl.BindTexture(GL_TEXTURE_2D, gl.tex);
	gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	gl.BindTexture(GL_TEXTURE_2D, tex);
	if (gl.rb) {
		gl.BindRenderbuffer(GL_RENDERBUFFER, gl.rb);
		gl.RenderbufferStorage(GL_RENDERBUFFER, gl.cb.stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24, w, h);
		gl.BindRenderbuffer(GL_RENDERBUFFER, rbo);
	}
	LOG("gpu: FBO aumentado de %ux%u para %ux%u", gl.w, gl.h, w, h);
	gl.w = w;
	gl.h = h;
}

/* depois do retro_load_game: cria tudo e chama o context_reset do nucleo */
static bool gl_inicia(void)
{
	if (!gl.cb.context_reset)
		return true; /* nucleo sem GPU */
	if (!gl.ctx || !gl_cria_fbo(av.geometry.max_width, av.geometry.max_height))
		return false;
	gl.ativo = true;
	gl.cb.context_reset();
	return true;
}

static void gl_fim(void)
{
	if (!gl.ativo)
		return;
	if (gl.cb.context_destroy)
		gl.cb.context_destroy();
	for (int k = 0; k < 2; k++) {
		if (gl.sinc[k])
			gl.DeleteSync(gl.sinc[k]);
		if (gl.pbo[k])
			gl.DeleteBuffers(1, &gl.pbo[k]);
		gl.sinc[k] = NULL;
		gl.pbo[k] = 0;
	}
	if (gl.fbo2) {
		gl.DeleteFramebuffers(1, &gl.fbo2);
		gl.DeleteTextures(1, &gl.tex2);
		gl.fbo2 = gl.tex2 = 0;
	}
	gl.ativo = false;
}

static void gl_fecha(void)
{
	if (gl.dpy) {
		gl.MakeCurrent(gl.dpy, NULL, NULL, NULL);
		if (gl.ctx)
			gl.DestroyContext(gl.dpy, gl.ctx);
		gl.Terminate(gl.dpy);
	}
	if (gl.gbm && gl.hgbm) {
		void (*destroi)(void *) = (void (*)(void *))dlsym(gl.hgbm, "gbm_device_destroy");
		if (destroi)
			destroi(gl.gbm);
	}
	if (gl.gbm_fd >= 0)
		close(gl.gbm_fd);
}

/* quadro pronto no FBO -> memoria (XRGB8888, linha 0 = topo da imagem) */
static const uint32_t *gl_le_quadro(unsigned w, unsigned h)
{
	if (w > gl.w) w = gl.w;
	if (h > gl.h) h = gl.h;
	size_t n = (size_t)w * h;
	if (n > gl.cap) {
		free(gl.q);
		free(gl.tmp);
		gl.q = malloc(n * 4);
		gl.tmp = malloc(n * 4);
		gl.cap = n;
	}
	int rfb = 0, pbo = 0;
	gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
	gl.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
	gl.BindFramebuffer(GL_READ_FRAMEBUFFER, gl.fbo);
	if (pbo)
		gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
	gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
	bool vira = gl.cb.bottom_left_origin;
	if (gl.bgra && !vira) { /* BGRA = XRGB8888 na memoria: direto, sem converter */
		gl.ReadPixels(0, 0, w, h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, gl.q);
		gl.BindFramebuffer(GL_READ_FRAMEBUFFER, rfb);
		if (pbo)
			gl.BindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
		return gl.q;
	}
	gl.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, gl.tmp);
	gl.BindFramebuffer(GL_READ_FRAMEBUFFER, rfb);
	if (pbo)
		gl.BindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
	/* RGBA (bytes) -> XRGB8888; o padrao libretro em GL ja tem a linha 0 em cima,
	 * bottom_left_origin = imagem "de pe" no GL: inverte */
	for (unsigned y = 0; y < h; y++) {
		const uint32_t *o = gl.tmp + (size_t)(gl.cb.bottom_left_origin ? h - 1 - y : y) * w;
		uint32_t *d = gl.q + (size_t)y * w;
		for (unsigned x = 0; x < w; x++) {
			uint32_t p = o[x];
			d[x] = (p & 0xff) << 16 | (p & 0xff00) | (p >> 16 & 0xff);
		}
	}
	return gl.q;
}

/* tamanho que vai para a memoria: o Dreamcast nunca manda mais que ~720 de largura nem
 * ~576 linhas para a TV (quando o jogo desenha 1280 de largura o proprio console reduz
 * pela metade, e resolucao interna maior tambem e reduzida); a GPU reduz pela metade
 * (filtro linear = media de 2 pontos) enquanto a metade ainda tiver 640x480 ou mais */
static void gl_reduzido(unsigned w, unsigned h, unsigned *ow, unsigned *oh)
{
	while (w >= 1280) /* 1280 -> 640, 1600 -> 800, 1920 -> 960; 853 (tela larga) fica */
		w /= 2;
	while (h >= 960) /* 960 -> 480; 720 fica */
		h /= 2;
	*ow = w;
	*oh = h;
}

static bool gl_fbo2(unsigned w, unsigned h)
{
	if (gl.fbo2 && w <= gl.w2 && h <= gl.h2)
		return true;
	if (w < gl.w2) w = gl.w2;
	if (h < gl.h2) h = gl.h2;
	if (!gl.fbo2) {
		gl.GenTextures(1, &gl.tex2);
		gl.GenFramebuffers(1, &gl.fbo2);
	}
	int tex = 0, dfb = 0;
	gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
	gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
	gl.BindTexture(GL_TEXTURE_2D, gl.tex2);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	gl.BindTexture(GL_TEXTURE_2D, tex);
	gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, gl.fbo2);
	gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl.tex2, 0);
	unsigned st = gl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
	gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, dfb);
	if (st != GL_FRAMEBUFFER_COMPLETE) {
		LOG("gpu: ERRO: FBO reduzido %ux%u incompleto (0x%x): leitura direta", w, h, st);
		gl.assinc = false;
		return false;
	}
	gl.w2 = w;
	gl.h2 = h;
	return true;
}

/* quadro N: a GPU reduz e copia para o PBO da vez (sem esperar); devolve o quadro N-1,
 * que a GPU ja terminou. NULL no 1o quadro. w e h viram o tamanho do quadro devolvido. */
static const uint32_t *gl_le_assinc(unsigned *pw, unsigned *ph)
{
	unsigned w = *pw, h = *ph, ow, oh;
	if (w > gl.w) w = gl.w;
	if (h > gl.h) h = gl.h;
	gl_reduzido(w, h, &ow, &oh);
	bool reduz = ow != w || oh != h;
	if (reduz && !gl_fbo2(ow, oh))
		return NULL;
	int rfb = 0, dfb = 0, pbo = 0;
	gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
	gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
	gl.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
	bool tesoura = gl.IsEnabled(GL_SCISSOR_TEST);
	gl.BindFramebuffer(GL_READ_FRAMEBUFFER, gl.fbo);
	if (reduz) {
		if (tesoura)
			gl.Disable(GL_SCISSOR_TEST);
		gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, gl.fbo2);
		gl.BlitFramebuffer(0, 0, w, h, 0, 0, ow, oh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		gl.BindFramebuffer(GL_READ_FRAMEBUFFER, gl.fbo2);
	}
	/* manda a copia deste quadro */
	int k = gl.vez;
	size_t tam = (size_t)ow * oh * 4;
	if (!gl.pbo[k])
		gl.GenBuffers(1, &gl.pbo[k]);
	gl.BindBuffer(GL_PIXEL_PACK_BUFFER, gl.pbo[k]);
	if (tam > gl.pbo_cap[k]) {
		gl.BufferData(GL_PIXEL_PACK_BUFFER, tam, NULL, GL_STREAM_READ);
		gl.pbo_cap[k] = tam;
	}
	gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
	gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
	gl.ReadPixels(0, 0, ow, oh, gl.bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	if (gl.sinc[k])
		gl.DeleteSync(gl.sinc[k]);
	gl.sinc[k] = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	/* a GPU do Pi so comeca o quadro quando recebe os comandos: sem isto ela so ia
	 * desenhar quando o rv-jogo fosse buscar o quadro (e o rv-jogo esperava do mesmo jeito) */
	gl.Flush();
	gl.pw[k] = ow;
	gl.ph[k] = oh;
	gl.vez = k ^ 1;
	/* pega o anterior */
	int a = k ^ 1;
	const uint32_t *res = NULL;
	if (gl.pw[a] && gl.sinc[a]) {
		unsigned aw = gl.pw[a], ah = gl.ph[a];
		size_t n = (size_t)aw * ah;
		if (n > gl.cap) {
			free(gl.q);
			free(gl.tmp);
			gl.q = malloc(n * 4);
			gl.tmp = NULL;
			gl.cap = n;
		}
		gl.ClientWaitSync(gl.sinc[a], GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ull); /* ja pronto; teto 100 ms */
		gl.BindBuffer(GL_PIXEL_PACK_BUFFER, gl.pbo[a]);
		const uint32_t *m = gl.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, n * 4, GL_MAP_READ_BIT);
		if (m && gl.q) {
			bool vira = gl.cb.bottom_left_origin;
			for (unsigned y = 0; y < ah; y++) {
				const uint32_t *o = m + (size_t)(vira ? ah - 1 - y : y) * aw;
				uint32_t *d = gl.q + (size_t)y * aw;
				if (gl.bgra)
					memcpy(d, o, (size_t)aw * 4);
				else
					for (unsigned x = 0; x < aw; x++) {
						uint32_t p = o[x];
						d[x] = (p & 0xff) << 16 | (p & 0xff00) | (p >> 16 & 0xff);
					}
			}
			res = gl.q;
			*pw = aw;
			*ph = ah;
		}
		if (m)
			gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
	}
	gl.BindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
	gl.BindFramebuffer(GL_READ_FRAMEBUFFER, rfb);
	gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, dfb);
	if (reduz && tesoura)
		gl.Enable(GL_SCISSOR_TEST);
	return res;
}

/* diagnostico: uma vez por tamanho informado (2 s depois de mudar), le o FBO INTEIRO,
 * anota no LOG ate onde vai a imagem de verdade e grava /run/retrovolt/gpu_WxH.ppm
 * (de cima para baixo como o jogo e visto) */
static void gl_foto(unsigned w, unsigned h)
{
	static unsigned fw, fh, nq, feitos[16][2], nfeitos;
	if (w != fw || h != fh) {
		fw = w; fh = h; nq = 0;
		return;
	}
	if (++nq != 120)
		return;
	for (unsigned i = 0; i < nfeitos; i++)
		if (feitos[i][0] == w && feitos[i][1] == h)
			return;
	if (nfeitos < 16) {
		feitos[nfeitos][0] = w;
		feitos[nfeitos++][1] = h;
	}
	size_t n = (size_t)gl.w * gl.h;
	uint32_t *p = malloc(n * 4);
	if (!p)
		return;
	int rfb = 0;
	gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
	gl.BindFramebuffer(GL_READ_FRAMEBUFFER, gl.fbo);
	gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
	gl.ReadPixels(0, 0, gl.w, gl.h, GL_RGBA, GL_UNSIGNED_BYTE, p);
	gl.BindFramebuffer(GL_READ_FRAMEBUFFER, rfb);
	/* caixa do que nao e preto (linhas do GL) */
	int x1 = -1, y1 = -1, x0 = gl.w, y0 = gl.h;
	for (unsigned y = 0; y < gl.h; y++)
		for (unsigned x = 0; x < gl.w; x++)
			if (p[(size_t)y * gl.w + x] & 0xf0f0f0) {
				if ((int)x < x0) x0 = x;
				if ((int)x > x1) x1 = x;
				if ((int)y < y0) y0 = y;
				if ((int)y > y1) y1 = y;
			}
	LOG("gpu: diagnostico: nucleo informou %ux%u; imagem no FBO %ux%u vai de x %d a %d, linha %d a %d",
	    w, h, gl.w, gl.h, x0, x1, y0, y1);
	char nome[96];
	snprintf(nome, sizeof nome, "/run/retrovolt/gpu_%ux%u.ppm", w, h);
	FILE *f = fopen(nome, "wb");
	if (f) {
		fprintf(f, "P6\n%u %u\n255\n", gl.w, gl.h);
		uint8_t *l = malloc(gl.w * 3);
		/* padrao libretro: linha 0 do GL = topo da imagem (bottom_left_origin inverte) */
		for (int i = 0; l && i < (int)gl.h; i++) {
			int y = gl.cb.bottom_left_origin ? (int)gl.h - 1 - i : i;
			for (unsigned x = 0; x < gl.w; x++) {
				uint32_t c = p[(size_t)y * gl.w + x];
				l[x * 3] = c & 0xff; l[x * 3 + 1] = c >> 8 & 0xff; l[x * 3 + 2] = c >> 16 & 0xff;
			}
			fwrite(l, 1, gl.w * 3, f);
		}
		free(l);
		fclose(f);
		LOG("gpu: diagnostico gravado em %s", nome);
	}
	free(p);
}

static bool gl_pedido(struct retro_hw_render_callback *cb)
{
	if (cb->context_type != RETRO_HW_CONTEXT_OPENGLES2 && cb->context_type != RETRO_HW_CONTEXT_OPENGLES3 &&
	    cb->context_type != RETRO_HW_CONTEXT_OPENGLES_VERSION) {
		LOG("gpu: nucleo pediu contexto %d: so GLES aqui", cb->context_type);
		return false;
	}
	gl.cb = *cb;
	if (!gl.ctx && !gl_cria_contexto())
		return false;
	cb->get_current_framebuffer = gl_fb_atual;
	cb->get_proc_address = gl_proc;
	gl.cb.get_current_framebuffer = gl_fb_atual;
	gl.cb.get_proc_address = gl_proc;
	LOG("gpu: nucleo desenha com GLES (tipo %d, %u.%u)", cb->context_type, cb->version_major, cb->version_minor);
	return true;
}


/* ------------------------------------------------------------------ GPU por Vulkan (Flycast)
 * Na GPU do Pi (V3D) o caminho GLES do Flycast pinta 2 vezes o pixel do canto de blocos 2D
 * (pontinhos em grade no Gunbird 2). Com --gpu vulkan o rv-jogo oferece Vulkan: cria a
 * instancia (sem janela), o nucleo cria o dispositivo (negociacao libretro), desenha numa
 * imagem dele e entrega por set_image; o rv-jogo copia a imagem para a memoria e segue o
 * caminho de sempre. libvulkan carregada na hora (dlopen). Sem Vulkan no Pi: fica no GLES. */
static int gpu_quer_vulkan = 1; /* --gpu vulkan|gles */
static struct {
	void *lib;
	bool pronto, pedido, ativo;
	VkInstance inst;
	VkPhysicalDevice gpu;
	VkDevice dev;
	VkQueue q;
	uint32_t qfi;
	const struct retro_hw_render_context_negotiation_interface_vulkan *neg;
	struct retro_hw_render_interface_vulkan iface;
	struct retro_hw_render_callback cb;
	pthread_mutex_t qmx;
	VkCommandPool pool;
	/* 2 vagas que se revezam: o quadro N e copiado enquanto o N-1 (ja copiado) vai
	 * para a tela; o Flycast tambem alterna 2 imagens (indice de sincronia 0/1) */
	VkCommandBuffer cmd[2];
	VkFence fence[2];
	VkBuffer buf[2];
	VkDeviceMemory mem[2];
	uint8_t *map[2];
	size_t cap[2];
	bool pend[2], bgra[2];
	unsigned pw[2], ph[2];
	uint32_t quadro;
	struct retro_vulkan_image img;
	bool tem_img;
	VkSemaphore sem[8], sinal;
	uint32_t nsem;
	VkCommandBuffer extra[8];
	uint32_t nextra;
	uint32_t *q32;
	size_t q32_cap;
	PFN_vkGetInstanceProcAddr gipa;
	PFN_vkGetDeviceProcAddr gdpa;
	PFN_vkCreateInstance CreateInstance;
	PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
	PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
	PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
	PFN_vkCreateDevice CreateDevice;
	PFN_vkGetDeviceQueue GetDeviceQueue;
	PFN_vkCreateCommandPool CreateCommandPool;
	PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
	PFN_vkCreateFence CreateFence;
	PFN_vkCreateBuffer CreateBuffer;
	PFN_vkDestroyBuffer DestroyBuffer;
	PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
	PFN_vkAllocateMemory AllocateMemory;
	PFN_vkFreeMemory FreeMemory;
	PFN_vkBindBufferMemory BindBufferMemory;
	PFN_vkMapMemory MapMemory;
	PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges;
	PFN_vkBeginCommandBuffer BeginCommandBuffer;
	PFN_vkEndCommandBuffer EndCommandBuffer;
	PFN_vkResetCommandBuffer ResetCommandBuffer;
	PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
	PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
	PFN_vkQueueSubmit QueueSubmit;
	PFN_vkQueueWaitIdle QueueWaitIdle;
	PFN_vkWaitForFences WaitForFences;
	PFN_vkResetFences ResetFences;
	PFN_vkDeviceWaitIdle DeviceWaitIdle;
} vk = {.qmx = PTHREAD_MUTEX_INITIALIZER};

/* chamado quando o nucleo pergunta a API: abre o Vulkan e confere que ha GPU */
static bool vk_prepara(void)
{
	if (vk.pronto)
		return true;
	if (!gpu_quer_vulkan)
		return false;
	vk.lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!vk.lib) {
		LOG("gpu: sem libvulkan.so.1 (sudo apt install mesa-vulkan-drivers libvulkan1): usa GLES");
		return false;
	}
	vk.gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk.lib, "vkGetInstanceProcAddr");
	if (!vk.gipa)
		return false;
	vk.CreateInstance = (PFN_vkCreateInstance)vk.gipa(NULL, "vkCreateInstance");
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "RetroVolt", 1, "rv-jogo", 1, VK_API_VERSION_1_1};
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	if (!vk.CreateInstance || vk.CreateInstance(&ici, NULL, &vk.inst) != VK_SUCCESS) {
		LOG("gpu: Vulkan nao abriu (sem driver?): usa GLES");
		return false;
	}
#define VKI(n) vk.n = (PFN_vk##n)vk.gipa(vk.inst, "vk" #n)
	VKI(EnumeratePhysicalDevices); VKI(GetPhysicalDeviceProperties); VKI(GetPhysicalDeviceMemoryProperties);
	VKI(GetPhysicalDeviceQueueFamilyProperties); VKI(CreateDevice); VKI(GetDeviceQueue);
	vk.gdpa = (PFN_vkGetDeviceProcAddr)vk.gipa(vk.inst, "vkGetDeviceProcAddr");
#undef VKI
	uint32_t n = 1;
	VkPhysicalDevice d = VK_NULL_HANDLE;
	if (!vk.EnumeratePhysicalDevices || !vk.gdpa ||
	    (vk.EnumeratePhysicalDevices(vk.inst, &n, &d) != VK_SUCCESS && !d) || !d) {
		LOG("gpu: Vulkan sem GPU: usa GLES");
		return false;
	}
	VkPhysicalDeviceProperties pp;
	vk.GetPhysicalDeviceProperties(d, &pp);
	vk.gpu = d;
	LOG("gpu: Vulkan %u.%u.%u em %s", VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion),
	    VK_VERSION_PATCH(pp.apiVersion), pp.deviceName);
	vk.pronto = true;
	return true;
}

static void vk_set_image(void *h, const struct retro_vulkan_image *img, uint32_t ns, const VkSemaphore *sem, uint32_t qf)
{
	(void)h; (void)qf;
	vk.img = *img;
	vk.tem_img = true;
	vk.nsem = ns > 8 ? 8 : ns;
	for (uint32_t i = 0; i < vk.nsem; i++)
		vk.sem[i] = sem[i];
}
static uint32_t vk_sync_index(void *h) { (void)h; return vk.quadro & 1; }
static uint32_t vk_sync_mask(void *h) { (void)h; return 3; }
static void vk_set_cmds(void *h, uint32_t n, const VkCommandBuffer *c)
{
	(void)h;
	vk.nextra = n > 8 ? 8 : n;
	for (uint32_t i = 0; i < vk.nextra; i++)
		vk.extra[i] = c[i];
}
static void vk_lock(void *h) { (void)h; pthread_mutex_lock(&vk.qmx); }
static void vk_unlock(void *h) { (void)h; pthread_mutex_unlock(&vk.qmx); }
/* espera o trabalho do indice atual (a copia que usou esta vaga 2 quadros atras) */
static void vk_wait_sync(void *h)
{
	(void)h;
	int i = vk.quadro & 1;
	if (vk.pend[i])
		vk.WaitForFences(vk.dev, 1, &vk.fence[i], VK_TRUE, 1000000000ull);
}
static void vk_set_sinal(void *h, VkSemaphore s) { (void)h; vk.sinal = s; }

static bool vk_pedido(struct retro_hw_render_callback *cb)
{
	if (!vk_prepara())
		return false;
	vk.cb = *cb;
	vk.pedido = true;
	LOG("gpu: nucleo desenha com Vulkan");
	return true;
}

/* depois do load_game: dispositivo (o nucleo cria pela negociacao), fila e a copia */
static bool vk_inicia(void)
{
	if (!vk.pedido)
		return true;
	struct retro_vulkan_context ctx;
	memset(&ctx, 0, sizeof ctx);
	if (vk.neg && vk.neg->create_device) {
		if (!vk.neg->create_device(&ctx, vk.inst, vk.gpu, VK_NULL_HANDLE, vk.gipa, NULL, 0, NULL, 0, NULL)) {
			LOG("gpu: o nucleo nao criou o dispositivo Vulkan");
			return false;
		}
		vk.gpu = ctx.gpu;
		vk.dev = ctx.device;
		vk.q = ctx.queue;
		vk.qfi = ctx.queue_family_index;
	} else {
		uint32_t nq = 0;
		VkQueueFamilyProperties qp[8];
		vk.GetPhysicalDeviceQueueFamilyProperties(vk.gpu, &nq, NULL);
		if (nq > 8) nq = 8;
		vk.GetPhysicalDeviceQueueFamilyProperties(vk.gpu, &nq, qp);
		for (vk.qfi = 0; vk.qfi < nq && !(qp[vk.qfi].queueFlags & VK_QUEUE_GRAPHICS_BIT); vk.qfi++)
			;
		float pr = 1.0f;
		VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, vk.qfi, 1, &pr};
		VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;
		if (vk.CreateDevice(vk.gpu, &dci, NULL, &vk.dev) != VK_SUCCESS)
			return false;
		vk.GetDeviceQueue(vk.dev, vk.qfi, 0, &vk.q);
	}
#define VKD(n) vk.n = (PFN_vk##n)vk.gdpa(vk.dev, "vk" #n)
	VKD(CreateCommandPool); VKD(AllocateCommandBuffers); VKD(CreateFence); VKD(CreateBuffer); VKD(DestroyBuffer);
	VKD(GetBufferMemoryRequirements); VKD(AllocateMemory); VKD(FreeMemory); VKD(BindBufferMemory); VKD(MapMemory);
	VKD(InvalidateMappedMemoryRanges); VKD(BeginCommandBuffer); VKD(EndCommandBuffer); VKD(ResetCommandBuffer);
	VKD(CmdPipelineBarrier); VKD(CmdCopyImageToBuffer); VKD(QueueSubmit); VKD(QueueWaitIdle); VKD(WaitForFences);
	VKD(ResetFences); VKD(DeviceWaitIdle);
#undef VKD
	VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
	                               VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, vk.qfi};
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, 0,
	                                   VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if (vk.CreateCommandPool(vk.dev, &pci, NULL, &vk.pool) != VK_SUCCESS)
		return false;
	cai.commandPool = vk.pool;
	cai.commandBufferCount = 2;
	if (vk.AllocateCommandBuffers(vk.dev, &cai, vk.cmd) != VK_SUCCESS ||
	    vk.CreateFence(vk.dev, &fci, NULL, &vk.fence[0]) != VK_SUCCESS || vk.CreateFence(vk.dev, &fci, NULL, &vk.fence[1]) != VK_SUCCESS)
		return false;
	struct retro_hw_render_interface_vulkan *f = &vk.iface;
	f->interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
	f->interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
	f->handle = &vk;
	f->instance = vk.inst;
	f->gpu = vk.gpu;
	f->device = vk.dev;
	f->get_device_proc_addr = vk.gdpa;
	f->get_instance_proc_addr = vk.gipa;
	f->queue = vk.q;
	f->queue_index = vk.qfi;
	f->set_image = vk_set_image;
	f->get_sync_index = vk_sync_index;
	f->get_sync_index_mask = vk_sync_mask;
	f->set_command_buffers = vk_set_cmds;
	f->wait_sync_index = vk_wait_sync;
	f->lock_queue = vk_lock;
	f->unlock_queue = vk_unlock;
	f->set_signal_semaphore = vk_set_sinal;
	vk.ativo = true;
	if (vk.cb.context_reset)
		vk.cb.context_reset();
	return true;
}

static bool vk_buf(int k, size_t tam)
{
	if (tam <= vk.cap[k])
		return true;
	if (vk.buf[k]) {
		vk.DeviceWaitIdle(vk.dev);
		vk.pend[k] = false;
		vk.DestroyBuffer(vk.dev, vk.buf[k], NULL);
		vk.FreeMemory(vk.dev, vk.mem[k], NULL);
		vk.buf[k] = VK_NULL_HANDLE;
		vk.cap[k] = 0;
	}
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, tam, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                          VK_SHARING_MODE_EXCLUSIVE};
	if (vk.CreateBuffer(vk.dev, &bci, NULL, &vk.buf[k]) != VK_SUCCESS)
		return false;
	VkMemoryRequirements mr;
	vk.GetBufferMemoryRequirements(vk.dev, vk.buf[k], &mr);
	VkPhysicalDeviceMemoryProperties mp;
	vk.GetPhysicalDeviceMemoryProperties(vk.gpu, &mp);
	uint32_t ti = UINT32_MAX;
	/* de preferencia memoria com cache para o processador ler rapido */
	for (int pass = 0; pass < 2 && ti == UINT32_MAX; pass++)
		for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
			VkMemoryPropertyFlags fl = mp.memoryTypes[i].propertyFlags;
			VkMemoryPropertyFlags quer = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			                             (pass == 0 ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			if ((mr.memoryTypeBits & (1u << i)) && (fl & quer) == quer) {
				ti = i;
				break;
			}
		}
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, ti};
	if (ti == UINT32_MAX || vk.AllocateMemory(vk.dev, &mai, NULL, &vk.mem[k]) != VK_SUCCESS ||
	    vk.BindBufferMemory(vk.dev, vk.buf[k], vk.mem[k], 0) != VK_SUCCESS ||
	    vk.MapMemory(vk.dev, vk.mem[k], 0, VK_WHOLE_SIZE, 0, (void **)&vk.map[k]) != VK_SUCCESS)
		return false;
	vk.cap[k] = tam;
	return true;
}

/* converte a vaga k (copia ja terminada) para XRGB8888 (linha 0 = topo) */
static const uint32_t *vk_converte(int k)
{
	VkMappedMemoryRange mrg = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, vk.mem[k], 0, VK_WHOLE_SIZE};
	vk.InvalidateMappedMemoryRanges(vk.dev, 1, &mrg);
	size_t n = (size_t)vk.pw[k] * vk.ph[k];
	if (n > vk.q32_cap) {
		free(vk.q32);
		vk.q32 = malloc(n * 4);
		vk.q32_cap = n;
	}
	const uint32_t *o = (const uint32_t *)vk.map[k];
	if (vk.bgra[k])
		for (size_t i = 0; i < n; i++)
			vk.q32[i] = o[i] & 0xffffff;
	else /* R8G8B8A8 (o do Flycast): bytes R,G,B,A -> XRGB */
		for (size_t i = 0; i < n; i++) {
			uint32_t p = o[i];
			vk.q32[i] = (p & 0xff) << 16 | (p & 0xff00) | (p >> 16 & 0xff);
		}
	return vk.q32;
}

/* quadro pronto na imagem do nucleo: pede a copia dele (sem esperar) e devolve o
 * quadro ANTERIOR, cuja copia ja terminou -> a GPU e o processador nao se esperam.
 * 1 quadro (16 ms) a mais de atraso. wp e hp: tamanho do quadro devolvido. */
static const uint32_t *vk_le_quadro(unsigned *wp, unsigned *hp)
{
	unsigned w = *wp, h = *hp;
	int k = vk.quadro & 1;
	if (!vk.tem_img || !w || !h)
		return NULL;
	if (vk.pend[k]) /* a vaga ainda guarda a copia de 2 quadros atras: espera ela */
		vk.WaitForFences(vk.dev, 1, &vk.fence[k], VK_TRUE, 1000000000ull);
	vk.pend[k] = false;
	if (!vk_buf(k, (size_t)w * h * 4))
		return NULL;
	VkImage im = vk.img.create_info.image;
	VkImageSubresourceRange sr = vk.img.create_info.subresourceRange;
	sr.levelCount = 1;
	sr.layerCount = 1;
	VkCommandBuffer cmd = vk.cmd[k];
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
	                               VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	vk.ResetCommandBuffer(cmd, 0);
	vk.BeginCommandBuffer(cmd, &bi);
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b.oldLayout = vk.img.image_layout;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = im;
	b.subresourceRange = sr;
	vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
	                      NULL, 1, &b);
	VkBufferImageCopy rg;
	memset(&rg, 0, sizeof rg);
	rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	rg.imageSubresource.layerCount = 1;
	rg.imageExtent.width = w;
	rg.imageExtent.height = h;
	rg.imageExtent.depth = 1;
	vk.CmdCopyImageToBuffer(cmd, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.buf[k], 1, &rg);
	b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b.newLayout = vk.img.image_layout;
	vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
	                      NULL, 1, &b);
	/* o buffer sera lido pelo processador */
	VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
	vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
	vk.EndCommandBuffer(cmd);
	VkCommandBuffer cbs[9];
	uint32_t ncb = 0;
	for (uint32_t i = 0; i < vk.nextra; i++)
		cbs[ncb++] = vk.extra[i];
	cbs[ncb++] = cmd;
	VkPipelineStageFlags st[8];
	for (uint32_t i = 0; i < vk.nsem; i++)
		st[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.waitSemaphoreCount = vk.nsem;
	si.pWaitSemaphores = vk.sem;
	si.pWaitDstStageMask = st;
	si.commandBufferCount = ncb;
	si.pCommandBuffers = cbs;
	if (vk.sinal) {
		si.signalSemaphoreCount = 1;
		si.pSignalSemaphores = &vk.sinal;
	}
	pthread_mutex_lock(&vk.qmx);
	vk.ResetFences(vk.dev, 1, &vk.fence[k]);
	VkResult r = vk.QueueSubmit(vk.q, 1, &si, vk.fence[k]);
	pthread_mutex_unlock(&vk.qmx);
	vk.nextra = 0;
	vk.nsem = 0;
	if (r != VK_SUCCESS)
		return NULL;
	vk.pend[k] = true;
	vk.pw[k] = w;
	vk.ph[k] = h;
	vk.bgra[k] = vk.img.create_info.format == VK_FORMAT_B8G8R8A8_UNORM || vk.img.create_info.format == VK_FORMAT_B8G8R8A8_SRGB;
	vk.quadro++;
	/* devolve o anterior (vaga do outro indice); no 1o quadro, espera este mesmo */
	int a = k ^ 1;
	if (!vk.pend[a] || !vk.pw[a])
		a = k;
	if (vk.WaitForFences(vk.dev, 1, &vk.fence[a], VK_TRUE, 1000000000ull) != VK_SUCCESS)
		return NULL;
	*wp = vk.pw[a];
	*hp = vk.ph[a];
	return vk_converte(a);
}

static void vk_fim(void)
{
	if (!vk.ativo)
		return;
	vk.DeviceWaitIdle(vk.dev);
	if (vk.cb.context_destroy)
		vk.cb.context_destroy();
	vk.ativo = false;
}

/* ultimo quadro do jogo (para o fundo do menu) */
static uint8_t *ult_q;
static unsigned ult_w, ult_h;
static size_t ult_pitch, ult_cap;
static int ult_fmt;
static uint32_t px_rgb(const uint8_t *src, unsigned x, int fmt);

/* diagnostico: ao abrir o menu do jogo grava o ultimo quadro EXATAMENTE como o nucleo
 * entregou (antes da convergencia/efeitos/saida) em /run/retrovolt/gpu_menu.ppm */
static void quadro_foto(void)
{
	if (!ult_q || !ult_w || !ult_h)
		return;
	FILE *f = fopen("/run/retrovolt/gpu_menu.ppm", "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%u %u\n255\n", ult_w, ult_h);
	uint8_t *l = malloc(ult_w * 3);
	for (unsigned y = 0; l && y < ult_h; y++) {
		const uint8_t *src = ult_q + (size_t)y * ult_pitch;
		for (unsigned x = 0; x < ult_w; x++) {
			uint32_t c = px_rgb(src, x, ult_fmt);
			l[x * 3] = c >> 16; l[x * 3 + 1] = c >> 8; l[x * 3 + 2] = c;
		}
		fwrite(l, 1, ult_w * 3, f);
	}
	free(l);
	fclose(f);
	LOG("quadro do jogo gravado em /run/retrovolt/gpu_menu.ppm (%ux%u)", ult_w, ult_h);
}
/* tempo do rv-jogo dentro de cada quadro (copia da GPU + desenho), para separar do nucleo */
static double t_meu_soma, t_run_soma;
static long n_medidos;
static void video_cb_real(const void *data, unsigned w, unsigned h, size_t pitch);
static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
	double t0 = agora();
	video_cb_real(data, w, h, pitch);
	t_meu_soma += agora() - t0;
}
static void video_cb_real(const void *data, unsigned w, unsigned h, size_t pitch)
{
	if (!data) /* repetir quadro: o fb.back ja tem a imagem */
		return;
	int fmt = pixfmt;
	if (data == RETRO_HW_FRAME_BUFFER_VALID && vk.ativo) {
		data = vk_le_quadro(&w, &h);
		if (!data)
			return;
		pitch = (size_t)w * 4;
		fmt = RETRO_PIXEL_FORMAT_XRGB8888;
	} else if (data == RETRO_HW_FRAME_BUFFER_VALID) {
		if (!gl.ativo)
			return;
		if (w > gl.w || h > gl.h)
			gl_aumenta(w, h);
		gl_foto(w, h);
		if (gl.assinc) {
			data = gl_le_assinc(&w, &h);
			if (!data)
				return;
		} else {
			data = gl_le_quadro(w, h);
			if (w > gl.w) w = gl.w;
			if (h > gl.h) h = gl.h;
		}
		pitch = (size_t)w * 4;
		fmt = RETRO_PIXEL_FORMAT_XRGB8888;
	}
	km_quadro(h); /* tubo: 480i quando o jogo e 480i */
	size_t t = pitch * h;
	if (t > ult_cap) {
		free(ult_q);
		ult_q = malloc(t);
		ult_cap = t;
	}
	memcpy(ult_q, data, t);
	ult_w = w; ult_h = h; ult_pitch = pitch; ult_fmt = fmt;
	desenha(data, w, h, pitch, fmt, av.geometry.aspect_ratio);
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
/* jogo em "1 quadro por vsync" com Hz um pouco diferente da tela (Neo Geo 59,19; PC Engine
 * 59,82...): o jogo anda tela/jogo mais rapido, e o som acompanha na mesma proporcao
 * (como o RetroArch). Ate 2% (~1/3 de semitom; ninguem percebe). */
static volatile double rs_esticar = 1.0;
#define UM_PRA_UM_MAX 0.02
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
	double passo = rs_passo_base * rs_esticar * (1.0 + (cheio - 0.5) * 0.01);
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
	if (a_opcoes_jogo) { /* vale so para este jogo: grava o que difere da base */
		mkdir_pai(a_opcoes_jogo);
		FILE *f = fopen(a_opcoes_jogo, "w");
		if (!f)
			return;
		fprintf(f, "# RETROVOLT: opcoes do nucleo so deste jogo (o resto vem de %s)\n", a_opcoes ? a_opcoes : "padrao");
		int n = 0;
		for (int i = 0; i < nvars; i++)
			if (!vars[i].base || strcmp(vars[i].v, vars[i].base)) {
				fprintf(f, "%s = \"%s\"\n", vars[i].k, vars[i].v);
				n++;
			}
		fclose(f);
		sync();
		LOG("opcoes do nucleo: %d gravadas so para este jogo em %s", n, a_opcoes_jogo);
		return;
	}
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
	/* gatilhos analogicos (Dreamcast L/R): o 8BitDo manda LT/RT como botao */
	if ((device & RETRO_DEVICE_MASK) == RETRO_DEVICE_ANALOG && index == RETRO_DEVICE_INDEX_ANALOG_BUTTON)
		return id < 16 && (b >> id & 1) ? 0x7fff : 0;
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
			for (int i = 0; i < nvars; i++)       /* sem valor geral gravado: a base e o padrao */
				if (!strcmp(vars[i].k, v->key) && !vars[i].base)
					vars[i].base = strdup(def);
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
		gl_aumenta(av.geometry.max_width, av.geometry.max_height);
		return true;
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		av = *(const struct retro_system_av_info *)data;
		LOG("nucleo mudou: %.3f Hz, som %.0f Hz", av.timing.fps, av.timing.sample_rate);
		gl_aumenta(av.geometry.max_width, av.geometry.max_height);
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
	case RETRO_ENVIRONMENT_SET_HW_RENDER:
		if (((struct retro_hw_render_callback *)data)->context_type == RETRO_HW_CONTEXT_VULKAN)
			return vk_pedido(data);
		return gl_pedido(data);
	case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
		/* Vulkan se pedido e se o Pi tem driver; senao GLES 3 (V3D com GLES 3.1) */
		*(unsigned *)data = vk_prepara() ? RETRO_HW_CONTEXT_VULKAN : RETRO_HW_CONTEXT_OPENGLES3;
		return true;
	case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE: {
		const struct retro_hw_render_context_negotiation_interface_vulkan *n = data;
		if (!n || n->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
			return false;
		vk.neg = n;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT: {
		struct retro_hw_render_context_negotiation_interface *n = data;
		if (!n || n->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
			return false;
		n->interface_version = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
		if (!vk.ativo)
			return false;
		*(const struct retro_hw_render_interface **)data = (const struct retro_hw_render_interface *)&vk.iface;
		return true;
	case RETRO_ENVIRONMENT_GET_JIT_CAPABLE: /* Flycast: sem isso desiste (aarch64 tem JIT) */
		*(bool *)data = true;
		return true;
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
static double cv_w, cv_h;
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
/* setor que existe no disco: no CD em faixas (2 sessoes) o vao entre as sessoes (lead-out
 * + lead-in, ~11400 setores) nao tem dados e a leitora recusa (sense 5/64/00) */
static bool cd_lba_existe(int64_t l)
{
	if (l < 0 || l >= cd_tam / SETOR)
		return false;
	if (!cd_faixas)
		return true;
	for (int i = 0; i < cd_nfx; i++)
		if (l >= cd_fx[i].lba && l < cd_fx_fim[i])
			return true;
	return false;
}

static void *cd_acorda_thread(void *x)
{
	(void)x;
	uint8_t b[SETOR];
	int n_falhas = 0, n_log = 0;
	unsigned volta = 0;
	while (!g_sair) {
		sleep(1);
		/* jogando: a regra do autor (--cd-acorda, 10 s). Menu aberto: 3 s (pedido do autor),
		 * para nao voltar do menu com a leitora parada (travadinha de 1-2 s) */
		int lim = cd_em_menu && cd_acorda_s > CD_ACORDA_MENU_S ? CD_ACORDA_MENU_S : cd_acorda_s;
		if (agora() - cd_ult_leitura < lim)
			continue;
		/* Le um setor PERTO de onde o jogo parou, mas ATRAS dele e cada vez num lugar
		 * diferente. Ler o mesmo setor do jogo nao adianta: ele ja esta na memoria
		 * interna da leitora, que responde sem girar o disco e dorme mesmo assim. */
		int64_t total = cd_tam / SETOR;
		int64_t l = cd_ult_lba - 300 - (int64_t)(volta++ % 8) * 75;
		if (l < 16)
			l = cd_ult_lba + 600 + (int64_t)(volta % 8) * 75; /* inicio do disco: le a frente, longe */
		if (l >= total)
			l = total > 32 ? total / 2 : 0;
		if (!cd_lba_existe(l)) /* caiu no vao entre sessoes: tenta a frente, depois o proprio lugar */
			l = cd_ult_lba + 600 + (int64_t)(volta % 8) * 75;
		if (!cd_lba_existe(l))
			l = cd_ult_lba;
		pthread_mutex_lock(&cd_io_mx);
		double t0 = agora();
		bool ok = cd_le_setor(l, b);
		double t = agora() - t0;
		cd_ult_leitura = agora();
		pthread_mutex_unlock(&cd_io_mx);
		if (!ok && n_falhas++ < 5)
			LOG("cd: leitura para manter a leitora girando falhou (lba %lld)", (long long)l);
		/* leitura lenta = o disco estava parando (prova de que o "acorda" falhou) */
		if (ok && (n_log < 3 || t > 0.3) && n_log++ < 40)
			LOG("cd: leitora acordada (lba %lld, %.0f ms)%s", (long long)l, t * 1e3,
			    t > 0.3 ? " - ESTAVA PARANDO" : "");
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
		LOG("filtro deste nucleo: %s%s", FILTROS[v], tubo ? " (sem efeito no tubo)" : "");
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
		fprintf(f, "%d\n# RETROVOLT: filtro deste nucleo (0 nenhum, 1 scanlines, 2 scanlines leves, 3 CRT, 4 suave, 5 scale2x, 6 scale3x, 7 xbr)\n", filtro);
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
	double w, h = 1.0;
	int n = fscanf(f, "%d %d %lf %lf", &x, &y, &w, &h);
	if (n >= 3) {
		conv_x = x;
		conv_y = y;
		conv_w = w;
		conv_h = n == 4 && h >= 0.80 && h <= 1.10 ? h : 1.0;
		LOG("convergencia deste nucleo: %d %d %.3f altura %.3f", x, y, w, conv_h);
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
		fprintf(f, "%d %d %.3f %.3f\n# RETROVOLT: posicao x, posicao y, largura, altura (so deste nucleo)\n",
		        conv_x, conv_y, conv_w, conv_h);
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
	snprintf(l, sizeof l, "X %+d  Y %+d  LARGURA %d%%  ALTURA %d%%", conv_x, conv_y, (int)lround(conv_w * 100),
	         (int)lround(conv_h * 100));
	fb_texto(-1, 70 + F->ch + 8, l, 240, 236, 225, true);
	fb_texto(-1, 70 + 2 * (F->ch + 8), "SETAS POSICAO  LB/RB LARGURA  LT/RT ALTURA", 120, 130, 160, true);
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
			conv_x = cv_x; conv_y = cv_y; conv_w = cv_w; conv_h = cv_h;
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
		/* LT/RT: altura (antes nao existia no jogo; o menu mandava e ninguem usava) */
		if (novo & (1u << B_L2)) conv_h -= 0.005;
		if (novo & (1u << B_R2)) conv_h += 0.005;
		if (novo & (1u << B_SELECT)) {
			conv_x = conv_menu_x; conv_y = conv_menu_y; conv_w = conv_menu_w; conv_h = conv_menu_h;
		}
		conv_x = conv_x < -300 ? -300 : conv_x > 300 ? 300 : conv_x;
		conv_y = conv_y < -20 ? -20 : conv_y > 20 ? 20 : conv_y;
		conv_w = conv_w < 0.70 ? 0.70 : conv_w > 1.10 ? 1.10 : conv_w;
		conv_h = conv_h < 0.80 ? 0.80 : conv_h > 1.10 ? 1.10 : conv_h;
		if (novo & ((1u << B_L2) | (1u << B_R2)))
			ult_x0 = -99999; /* geometria mudou: limpa as bordas */
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
				cv_x = conv_x; cv_y = conv_y; cv_w = conv_w; cv_h = conv_h;
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

/* temperatura e velocidade da CPU: o Pi reduz a velocidade sozinho perto de 80-85 C
 * (nao estraga; fica mais lento) - aparece aqui se um picote for calor */
static long ler_num(const char *arq)
{
	FILE *f = fopen(arq, "r");
	long v = -1;
	if (f) {
		if (fscanf(f, "%li", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}
static const char *calor_txt(void)
{
	static char t[96];
	long tc = ler_num("/sys/class/thermal/thermal_zone0/temp");
	long cur = ler_num("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
	long max = ler_num("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq");
	snprintf(t, sizeof t, "temperatura %ld C, cpu %ld/%ld MHz%s", tc / 1000, cur / 1000, max / 1000,
	         cur > 0 && max > 0 && cur < max * 9 / 10 ? " (REDUZIDA)" : "");
	return t;
}

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

/* ------------------------------------------------------------------ fundo do menu */
/* rv-jogo --fundo entrada.jpg|png saida.rgb L A
 * Decodifica a imagem, corta o excesso (preenche L x A mantendo a proporcao, centro)
 * e reduz pela media dos pontos. Saida: L e A (16 bits cada) + RGB. O menu (Python)
 * le esse arquivo pronto; decodificar JPG em Python puro levaria minutos. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WRITE
#include "stb_image.h"

static int fundo_converte(const char *ent, const char *sai, int L, int A)
{
	int w, h, n;
	if (L < 1 || A < 1 || L > 4096 || A > 4096)
		return 2;
	uint8_t *img = stbi_load(ent, &w, &h, &n, 3);
	if (!img) {
		fprintf(stderr, "fundo: nao abriu %s (%s)\n", ent, stbi_failure_reason());
		return 1;
	}
	/* janela da imagem com a proporcao da tela */
	double cx = 0, cy = 0, cw = w, chh = h;
	if ((double)w / h > (double)L / A) {
		cw = (double)h * L / A;
		cx = (w - cw) / 2;
	} else {
		chh = (double)w * A / L;
		cy = (h - chh) / 2;
	}
	uint8_t *out = malloc(4 + (size_t)L * A * 3);
	out[0] = L & 255; out[1] = L >> 8; out[2] = A & 255; out[3] = A >> 8;
	for (int y = 0; y < A; y++) {
		int y0 = cy + y * chh / A, y1 = cy + (y + 1) * chh / A;
		if (y1 <= y0) y1 = y0 + 1;
		if (y1 > h) y1 = h;
		for (int x = 0; x < L; x++) {
			int x0 = cx + x * cw / L, x1 = cx + (x + 1) * cw / L;
			if (x1 <= x0) x1 = x0 + 1;
			if (x1 > w) x1 = w;
			unsigned r = 0, g = 0, b = 0, k = 0;
			for (int yy = y0; yy < y1; yy++)
				for (int xx = x0; xx < x1; xx++) {
					const uint8_t *p = img + ((size_t)yy * w + xx) * 3;
					r += p[0]; g += p[1]; b += p[2]; k++;
				}
			uint8_t *o = out + 4 + ((size_t)y * L + x) * 3;
			o[0] = k ? r / k : 0; o[1] = k ? g / k : 0; o[2] = k ? b / k : 0;
		}
	}
	stbi_image_free(img);
	char tmp[4200];
	snprintf(tmp, sizeof tmp, "%s.tmp", sai);
	FILE *f = fopen(tmp, "wb");
	if (!f || fwrite(out, 1, 4 + (size_t)L * A * 3, f) != 4 + (size_t)L * A * 3) {
		if (f) fclose(f);
		free(out);
		return 3;
	}
	fclose(f);
	free(out);
	return rename(tmp, sai) ? 3 : 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc == 6 && !strcmp(argv[1], "--fundo"))
		return fundo_converte(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]));
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
		else if (!strcmp(k, "--cd-faixas")) cd_faixas = atoi(v) != 0;
		else if (!strcmp(k, "--tubo-480i")) tubo_480i_ok = atoi(v) != 0;
		else if (!strcmp(k, "--saida-32")) saida32_ok = atoi(v) != 0;
		else if (!strcmp(k, "--gpu")) gpu_quer_vulkan = !strcasecmp(v, "vulkan");
		else if (!strcmp(k, "--cd-cache")) { /* MB */
			int64_t n = (int64_t)atoi(v) * 1024 * 1024 / (BLOCO * SETOR);
			ncache = n < NCACHE ? NCACHE : n > 16384 ? 16384 : (int)n;
		}
		else if (!strcmp(k, "--cd-acorda")) cd_acorda_s = atoi(v) < 2 ? 2 : atoi(v) > 60 ? 60 : atoi(v);
		else if (!strcmp(k, "--volume")) volume = atoi(v) < 0 ? 0 : atoi(v) > 100 ? 100 : atoi(v);
		else if (!strcmp(k, "--audio")) a_audio = v;
		else if (!strcmp(k, "--opcoes")) a_opcoes = v;
		else if (!strcmp(k, "--opcoes-jogo")) a_opcoes_jogo = v;
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
	/* sem SIGSEGV aqui: o Flycast usa esse sinal para a memoria dele. Se o rv-jogo cair,
	 * o kernel fecha o DRM e devolve o modo do framebuffer (240p) sozinho */
	signal(SIGABRT, km_fatal);
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
	km_prepara();
	km_entra();
	menu_tamanho();
	if (!core.load_game(&gi)) {
		LOG("ERRO: o nucleo nao abriu o jogo");
		return 2;
	}
	core.get_system_av_info(&av);
	LOG("jogo: %ux%u %.3f Hz, som %.0f Hz", av.geometry.base_width, av.geometry.base_height, av.timing.fps, av.timing.sample_rate);
	if (!vk_inicia()) {
		LOG("ERRO: o Vulkan nao abriu para o nucleo");
		core.unload_game();
		return 2;
	}
	if (!gl_inicia()) {
		LOG("ERRO: a GPU nao abriu para o nucleo");
		core.unload_game();
		return 2;
	}
	core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
	core.set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
	sram_carrega();
	botoes_ler();
	conv_menu_x = conv_x; conv_menu_y = conv_y; conv_menu_w = conv_w; conv_menu_h = conv_h;
	/* altura do jogo: 100% ate ser ajustada aqui (encolher pula linhas da imagem do jogo;
	 * a do menu so vale se escolhida com SELECT) */
	conv_h = 1.0;
	conv_le();
	filtro_le();
	imagem_le();
	if (filtro < 0 || filtro >= N_FILTROS)
		filtro = 0;
	if (cd_fd >= 0 && !cd_arquivo) {
		pthread_t ta;
		cd_ult_leitura = agora();
		pthread_create(&ta, NULL, cd_acorda_thread, NULL);
		if (cd_pre_prox < cd_pre_npend) {
			pthread_t tpre;
			pthread_create(&tpre, NULL, cd_pre_thread, NULL);
		}
	}
	if (cd_fd >= 0) {
		pthread_t tp;
		pthread_create(&tp, NULL, cd_adiante_thread, NULL);
		if (ncache > NCACHE) { /* --cd-cache (Dreamcast) */
			pthread_t tc;
			pthread_create(&tc, NULL, cd_copia_thread, NULL);
		}
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
	/* o rv-jogo controla a saida: o Hz exato e a conta do modo de video (a medida as vezes
	 * erra no comeco, com a GPU ocupada: 61,6 Hz num LOG -> "ritmo pelo tempo" a toa) */
	if (km.ativo && vs_tipo) {
		double hz = km_hz(&km.m_boot);
		if (fabs(hz - tela_hz) > 0.05)
			LOG("tela: medida %.3f Hz, modo de video %.3f Hz: vale o modo", tela_hz, hz);
		tela_hz = hz;
	}
	bool um_pra_um = !fb.sem_vsync && fabs(av.timing.fps - tela_hz) / tela_hz < UM_PRA_UM_MAX;
	rs_esticar = um_pra_um && av.timing.fps > 0 ? tela_hz / av.timing.fps : 1.0;
	LOG("tela %.3f Hz; %s (jogo %.3f Hz, som x%.4f)", tela_hz, um_pra_um ? "1 quadro por vsync" : "ritmo pelo tempo",
	    av.timing.fps, rs_esticar);
	double fps_ult = av.timing.fps;

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
			cd_em_menu = true;
			menu_sel = 0;
			sub = 0;
			som_mudo = true;
			menu_fundo();
			LOG("menu aberto");
			quadro_foto();
			sram_grava(false);
		} else if (menu) {
			int escolha = menu_passo(b, novo, combo);
			if (escolha >= 0) {
				menu = false;
				cd_em_menu = false;
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
		if (km_tela_hz > 0) { /* tubo trocou 240p <-> 480i: ritmo da tela novo */
			tela_hz = km_tela_hz;
			km_tela_hz = 0;
			fps_ult = -1;
		}
		if (av.timing.fps != fps_ult) { /* o nucleo mudou de Hz (regiao, modo de video) */
			fps_ult = av.timing.fps;
			um_pra_um = !fb.sem_vsync && fps_ult > 0 && fabs(fps_ult - tela_hz) / tela_hz < UM_PRA_UM_MAX;
			rs_esticar = um_pra_um ? tela_hz / fps_ult : 1.0;
			acc = 0;
			LOG("jogo agora %.3f Hz; %s", fps_ult, um_pra_um ? "1 quadro por vsync" : "ritmo pelo tempo");
		}
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
		t_run_soma += t_run;
		n_medidos++;
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
				LOG("ritmo: %.1f quadros/s; cd: %ld blocos, media %.0f ms, max %.0f ms, metodo %d, adiante %ld/%ld usados, musica guardada %ld; som: buffer %d%%, vazio %ld; ressinc %ld",
				    300 / (t - t_ult), cd_n_blocos, cd_n_blocos ? cd_t_total / cd_n_blocos * 1e3 : 0, cd_t_max * 1e3, cd_metodo,
				    cd_n_acertos_pf, cd_n_adiante, cd_n_fixos_usados, r_cnt * 100 / RING, som_vazio, vs_ressinc);
			LOG("picos: controles %.1f ms, nucleo %.1f ms, copia %.1f ms; %s", pico_ctrl * 1e3, pico_run * 1e3,
			    pico_copia * 1e3, calor_txt());
			if (n_medidos)
				LOG("media por quadro: nucleo %.1f ms + rv-jogo (imagem) %.1f ms = %.1f ms (limite %.1f ms)",
				    (t_run_soma - t_meu_soma) * 1e3 / n_medidos, t_meu_soma * 1e3 / n_medidos,
				    t_run_soma * 1e3 / n_medidos, 1e3 / (tela_hz > 0 ? tela_hz : 60));
			t_run_soma = t_meu_soma = 0;
			n_medidos = 0;
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
	km_sai(); /* menu do RetroVolt: a tela do boot (240p no tubo) */
	vk_fim();
	gl_fim();
	core.unload_game();
	core.deinit();
	gl_fecha();
	LOG("fim (%ld quadros)", quadros);
	return ret;
}
