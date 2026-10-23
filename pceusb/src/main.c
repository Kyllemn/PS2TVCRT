/*
 * main.c - PCEUSB: PC Engine CD direto do leitor de CD USB no PS2.
 * Criado por Leonardo Torres.
 *
 * Nucleo: o Mednafen PCE Fast otimizado para PS2 (o mesmo do RETROVOLT),
 * System Card 3.0 dentro do ELF. Disco: cdusb.irx + cdcache (leitor travado
 * em 10x, leitura antecipada numa thread, manter girando).
 *
 * Video: gsKit, 240p (TV de tubo), textura de 16 bits no formato do GS
 * (ABGR1555, paleta do proprio nucleo para o PS2), sem filtro.
 * Som: buffer intermediario + thread de audio que alimenta o audsrv sem
 * parar (o audsrv tem ~107 ms e nao se protege quando esvazia).
 * Sair: SELECT + START.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <kernel.h>
#include <delaythread.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>
#include <libpad.h>
#include <debug.h>
#include <audsrv.h>
#include <gsKit.h>
#include <dmaKit.h>

#include <libretro.h>

#include "cdusb_ee.h"
#include "cdcache.h"
#include "diag.h"

#define VERSAO		"p03"
#define LOG_FILE	"mass:/PCEUSB.TXT"
#define DISC_PATH	"ps2disc:/disco.cue"

#define IRX(n)	extern unsigned char n[]; extern unsigned int size_##n
IRX(irx_iomanx);
IRX(irx_filexio);
IRX(irx_usbd);
IRX(irx_cdusb);
IRX(irx_bdm);
IRX(irx_bdmfs_fatfs);
IRX(irx_usbmass_bd);
IRX(irx_audsrv);

/* medidor do nucleo (ps2_prof.h, m10) */
unsigned prof_cyc[16], prof_cnt[16], prof_last;
int prof_cur, prof_stack[16], prof_sp;

void ps2_cd_stats(unsigned *data_miss, unsigned *audio_miss, unsigned *postponed);

/* pular a imagem do proximo quadro (lido pelo nucleo em retro_run) */
int ps2_skip_next;
extern const unsigned int ps2_syscard3_size;
extern const unsigned char ps2_syscard3[];

static const char *prof_name[10] = {
	"outros", "CPU", "fundo", "sprites", "spr-decod", "mistura", "VRAM", "PSG", "CD", "fim-som"
};

/* ------------------------------------------------------------------ log */
static char logbuf[256 * 1024];
static int loglen, log_ok;

static void log_add(const char *s)
{
	int n = strlen(s), on;

	on = DIntr();
	if (loglen + n + 2 < (int)sizeof(logbuf)) {
		memcpy(logbuf + loglen, s, n);
		loglen += n;
		logbuf[loglen++] = '\r';
		logbuf[loglen++] = '\n';
	}
	if (on)
		EIntr();
}

/* toda linha do log leva o tempo desde que o ELF abriu */
void L(const char *fmt, ...)
{
	char s[300];
	va_list ap;
	unsigned ms = (unsigned)(ee_now_us() / 1000);
	int n = snprintf(s, sizeof(s), "[%4u.%03u] ", ms / 1000, ms % 1000);
	va_start(ap, fmt);
	vsnprintf(s + n, sizeof(s) - n, fmt, ap);
	va_end(ap);
	log_add(s);
}

static int text_mode = 1;		/* 1 = tela de texto (antes do jogo) */

void S(const char *fmt, ...)
{
	char s[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);
	L("%s", s);
	if (text_mode)
		scr_printf("%s\n", s);
}

/* passo numerado: vai para o log e o log vai para o pendrive na hora */
static int step_n;
static void STEP(const char *fmt, ...)
{
	char s[200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);
	L("[PASSO %d] %s", ++step_n, s);
	log_save();
}

void log_save(void)
{
	int fd;
	if (!log_ok)
		return;
	fd = fileXioOpen(LOG_FILE, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC, 0666);
	if (fd < 0)
		return;
	fileXioWrite(fd, logbuf, loglen);
	fileXioClose(fd);
}

int log_ready(void)
{
	return log_ok;
}

void screen_title(const char *sub)
{
	scr_clear();
	scr_setXY(0, 1);
	scr_setfontcolor(0x00d0ff);
	scr_printf("   PCEUSB " VERSAO " - PC ENGINE CD PELO LEITOR USB\n");
	scr_setfontcolor(0xffffff);
	if (sub)
		scr_printf("   %s\n", sub);
	scr_printf("\n");
}

/* ------------------------------------------------------------------ controle */
static char pad_area[256] __attribute__((aligned(64)));
static int pad_ok;
static u16 pad_now, pad_prev = 0xffff;

static void pad_start(void)
{
	padInit(0);
	pad_ok = padPortOpen(0, 0, pad_area) != 0;
}

static void pad_poll(void)
{
	struct padButtonStatus b;
	int s;

	pad_prev = pad_now;
	if (!pad_ok)
		return;
	s = padGetState(0, 0);
	if (s != PAD_STATE_STABLE && s != PAD_STATE_FINDCTP1)
		return;
	if (padRead(0, 0, &b) == 0)
		return;
	pad_now = 0xffff ^ b.btns;
}

u16 pad_new(void)
{
	pad_poll();
	return pad_now & ~pad_prev;
}

/* ------------------------------------------------------------------ modulos */
static int load_buf(const char *name, void *p, unsigned int size)
{
	int ret, mr = -1;
	ret = SifExecModuleBuffer(p, size, 0, NULL, &mr);
	L("[BOOT] %s: %d / %d", name, ret, mr);
	return (ret < 0 || mr == 1) ? -1 : 0;
}

static int load_rom(const char *path)
{
	int ret = SifLoadModule(path, 0, NULL);
	L("[BOOT] %s: %d", path, ret);
	return ret < 0 ? -1 : 0;
}

static void iop_reset(void)
{
	SifInitRpc(0);
	while (!SifIopReset("", 0))
		;
	while (!SifIopSync())
		;
	SifInitRpc(0);
	sbv_patch_enable_lmb();
	sbv_patch_disable_prefix_check();
}

static void wait_devices(cdusb_status *st, int ms)
{
	u32 t0 = ee_ticks();
	int i, busy;

	do {
		ee_wait_ms(100);
		if (cdusb_get_status(st) < 0)
			return;
		busy = 0;
		for (i = 0; i < CDUSB_MAXDEV; i++)
			if (st->dev[i].state == CDUSB_DEV_CONNECTED)
				busy = 1;
	} while ((busy || ee_ms(t0) < 1500) && (int)ee_ms(t0) < ms);
}

static int find_cd(const cdusb_status *st)
{
	int i;
	for (i = 0; i < CDUSB_MAXDEV; i++)
		if (st->dev[i].state == CDUSB_DEV_READY && st->dev[i].ptype == 5)
			return i;
	return -1;
}

static void wait_pendrive(void)
{
	u32 t0 = ee_ticks();
	int fd;

	fileXioInit();
	while (ee_ms(t0) < 10000) {
		fd = fileXioDopen("mass:/");
		if (fd >= 0) {
			fileXioDclose(fd);
			log_ok = 1;
			return;
		}
		ee_wait_ms(250);
	}
}

/* ------------------------------------------------------------------ som */
#define AUDIO_PRIO	0x28		/* acima da leitura (0x30) e do jogo (0x40) */
#define RING_FRAMES	8192		/* ~186 ms de folga entre o jogo e o audsrv */
#define RING_TARGET	2048		/* nivel desejado (~46 ms): colchao contra tropecos */

static int16_t ring[RING_FRAMES * 2] __attribute__((aligned(64)));
static unsigned a_adj_drop, a_adj_dup;	/* ajuste fino do ritmo do som */
static volatile unsigned ring_w, ring_r;	/* em quadros de som (L+R) */
static volatile int a_quit, a_done;
static volatile unsigned a_under, a_drop;
static u8 a_out[2048 * 4] __attribute__((aligned(64)));
static u8 a_stack[16 * 1024] __attribute__((aligned(16)));
extern void *_gp;

static void audio_thread(void *arg)
{
	int avail, n, i, under = 1;		/* comeca enchendo o colchao */
	(void)arg;

	while (!a_quit) {
		avail = audsrv_available() / 4;		/* quadros livres no audsrv */
		while (avail > 0 && !a_quit) {
			unsigned w = ring_w, r = ring_r, have = w - r;
			int16_t *o = (int16_t *)a_out;
			n = avail > 2048 ? 2048 : avail;
			if (have == 0 || (under && have < RING_TARGET)) {
				/* o jogo atrasou: silencio, nunca deixar o audsrv vazio; no
				 * comeco e depois de uma falta so volta com o colchao cheio */
				memset(a_out, 0, n * 4);
				if (!under)
					a_under++;
				under = 1;
			} else {
				if ((unsigned)n > have)
					n = have;
				for (i = 0; i < n; i++, r++) {
					o[i * 2] = ring[(r % RING_FRAMES) * 2];
					o[i * 2 + 1] = ring[(r % RING_FRAMES) * 2 + 1];
				}
				ring_r = r;
				under = 0;
			}
			audsrv_play_audio((char *)a_out, n * 4);
			avail -= n;
		}
		DelayThread(4000);
	}
	a_done = 1;
	ExitThread();
}

static int audio_start(void)
{
	struct audsrv_fmt_t fmt;
	ee_thread_t th;
	int tid;

	if (audsrv_init() != 0) {
		L("[SOM] audsrv_init falhou: %s", audsrv_get_error_string());
		return 0;
	}
	fmt.bits = 16;
	fmt.freq = 44100;
	fmt.channels = 2;
	if (audsrv_set_format(&fmt) != 0) {
		L("[SOM] audsrv_set_format falhou: %s", audsrv_get_error_string());
		return 0;
	}
	audsrv_set_volume(MAX_VOLUME);
	memset(&th, 0, sizeof(th));
	th.func = (void *)audio_thread;
	th.stack = a_stack;
	th.stack_size = sizeof(a_stack);
	th.gp_reg = &_gp;
	th.initial_priority = AUDIO_PRIO;
	tid = CreateThread(&th);
	if (tid < 0)
		return 0;
	StartThread(tid, NULL);
	return 1;
}

/*
 * Ajuste fino: o PC Engine faz 59,826 quadros/s e a TV NTSC 59,94; preso no
 * retraco, o jogo produz um pouco mais de som do que o audsrv toca. Com o
 * colchao acima do alvo, 1 amostra em 200 e descartada; abaixo, 1 em 200 e
 * repetida (+-0,5%, inaudivel). Sem isso o buffer enche ou esvazia aos poucos.
 */
static size_t RETRO_CALLCONV audio_batch_cb(const int16_t *data, size_t frames)
{
	static unsigned adj;
	size_t i;
	unsigned w = ring_w, level = w - ring_r;
	int mode = level > RING_TARGET + 1024 ? 1 : (level < RING_TARGET - 1024 && level > 0) ? -1 : 0;

	for (i = 0; i < frames; i++) {
		if (w - ring_r >= RING_FRAMES - 2) {
			a_drop++;			/* buffer cheio: o jogo esta adiantado */
			break;
		}
		if (mode && ++adj >= 200) {
			adj = 0;
			if (mode > 0) {
				a_adj_drop++;
				continue;		/* descarta esta amostra */
			}
			ring[(w % RING_FRAMES) * 2] = data[i * 2];	/* repete esta amostra */
			ring[(w % RING_FRAMES) * 2 + 1] = data[i * 2 + 1];
			w++;
			a_adj_dup++;
		}
		ring[(w % RING_FRAMES) * 2] = data[i * 2];
		ring[(w % RING_FRAMES) * 2 + 1] = data[i * 2 + 1];
		w++;
	}
	ring_w = w;
	return frames;
}

static void RETRO_CALLCONV audio_sample_cb(int16_t l, int16_t r)
{
	int16_t s[2] = {l, r};
	audio_batch_cb(s, 1);
}

/* ------------------------------------------------------------------ video */
#define FB_W	512
#define FB_H	243

static GSGLOBAL *gs;
static GSTEXTURE tex;
static u16 fbuf[FB_W * 256] __attribute__((aligned(128)));
static unsigned frames_drawn;
static u64 vid_us_sum, vid_us_max, last_flip_us, vid_last_us;
static unsigned late_frames, worst_gap_ms;

static void video_start(void)
{
	gs = gsKit_init_global();
	gs->Mode = GS_MODE_NTSC;
	gs->Interlace = GS_NONINTERLACED;
	gs->Field = GS_FRAME;
	gs->Width = 640;
	gs->Height = 240;
	gs->PSM = GS_PSM_CT16;
	gs->PSMZ = GS_PSMZ_16S;
	gs->ZBuffering = GS_SETTING_OFF;
	gs->DoubleBuffering = GS_SETTING_ON;
	gs->PrimAlphaEnable = GS_SETTING_OFF;

	dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8,
		    1 << DMA_CHANNEL_GIF);
	dmaKit_chan_init(DMA_CHANNEL_GIF);
	gsKit_init_screen(gs);
	gsKit_TexManager_init(gs);
	gsKit_mode_switch(gs, GS_ONESHOT);

	memset(&tex, 0, sizeof(tex));
	tex.Width = FB_W;
	tex.Height = 256;
	tex.PSM = GS_PSM_CT16;
	tex.Mem = (u32 *)fbuf;
	tex.Filter = GS_FILTER_NEAREST;
	text_mode = 0;
}

static void RETRO_CALLCONV video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
	const u16 *src = data;
	float y0, y1;

	if (!data || !width || !height)
		return;				/* quadro pulado */
	if (height > 240)
		height = 240;
	/* o nucleo desenha direto no fbuf (GET_CURRENT_SOFTWARE_FRAMEBUFFER);
	 * se vier de outro lugar, copia */
	if (src < fbuf || src >= fbuf + FB_W * 256 || pitch != FB_W * 2) {
		unsigned y;
		for (y = 0; y < height; y++)
			memcpy(fbuf + y * FB_W, (const u8 *)data + y * pitch, width * 2);
		src = fbuf;
	}
	{
	u64 t0 = ee_now_us(), t1;
	tex.Mem = (u32 *)src;
	tex.Height = height;
	FlushCache(0);
	gsKit_TexManager_invalidate(gs, &tex);
	gsKit_TexManager_bind(gs, &tex);

	/* largura toda da tela; linhas 1:1 centralizadas (240p) */
	y0 = (float)((240 - (int)height) / 2);
	y1 = y0 + height;
	gsKit_prim_sprite_texture(gs, &tex, 0.0f, y0, 0.0f, 0.0f, 640.0f, y1, (float)width, (float)height, 2,
				  GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00));
	gsKit_queue_exec(gs);
	gsKit_sync_flip(gs);
	gsKit_TexManager_nextFrame(gs);
	frames_drawn++;
	t1 = ee_now_us();
	vid_last_us = t1 - t0;
	vid_us_sum += t1 - t0;
	if (t1 - t0 > vid_us_max)
		vid_us_max = t1 - t0;
	/* intervalo entre retracos: 16,7 ms normal; mais de 20 = retraco perdido */
	if (last_flip_us) {
		unsigned gap = (unsigned)((t1 - last_flip_us) / 1000);
		if (gap > 20)
			late_frames++;
		if (gap > worst_gap_ms)
			worst_gap_ms = gap;
	}
	last_flip_us = t1;
	}
}

/* ------------------------------------------------------------------ nucleo */
/* (o log de 1 s sai no passo "60 quadros emulados") */
static u16 joy;				/* bits RETRO_DEVICE_ID_JOYPAD_* */

static void RETRO_CALLCONV input_poll_cb(void)
{
	u16 b;

	pad_poll();
	b = pad_now;
	joy = 0;
	if (b & PAD_UP) joy |= 1 << RETRO_DEVICE_ID_JOYPAD_UP;
	if (b & PAD_DOWN) joy |= 1 << RETRO_DEVICE_ID_JOYPAD_DOWN;
	if (b & PAD_LEFT) joy |= 1 << RETRO_DEVICE_ID_JOYPAD_LEFT;
	if (b & PAD_RIGHT) joy |= 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
	if (b & PAD_CIRCLE) joy |= 1 << RETRO_DEVICE_ID_JOYPAD_A;	/* I */
	if (b & PAD_CROSS) joy |= 1 << RETRO_DEVICE_ID_JOYPAD_B;	/* II */
	if (b & PAD_SELECT) joy |= 1 << RETRO_DEVICE_ID_JOYPAD_SELECT;
	if (b & PAD_START) joy |= 1 << RETRO_DEVICE_ID_JOYPAD_START;	/* RUN */
}

static int16_t RETRO_CALLCONV input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)index;
	if (port != 0 || device != RETRO_DEVICE_JOYPAD)
		return 0;
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		return joy;
	return id < 16 ? (joy >> id) & 1 : 0;
}

static void RETRO_CALLCONV core_log(enum retro_log_level level, const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;
	(void)level;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	for (n = strlen(buf); n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'); n--)
		buf[n - 1] = 0;
	L("[NUCLEO] %s", buf);
}

static bool RETRO_CALLCONV environ_cb(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		/* a System Card vem de dentro do ELF (file.c pega pelo nome) */
		*(const char **)data = "mass:/PCEUSB";
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		/* no PS2 o nucleo ja entrega ABGR1555 (video.h) com o nome RGB565 */
		return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = core_log;
		return true;
	case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER: {
		struct retro_framebuffer *fb = data;
		fb->data = fbuf;
		fb->width = FB_W;
		fb->height = 256;
		fb->pitch = FB_W * 2;
		fb->format = RETRO_PIXEL_FORMAT_RGB565;
		fb->memory_flags = 0;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = false;
		return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE:
		L("[NUCLEO] mensagem: %s", ((const struct retro_message *)data)->msg);
		return true;
	default:
		return false;
	}
}

/* ------------------------------------------------------------------ telas */
static void wait_key(u16 keys)
{
	while (!(pad_new() & keys))
		ee_wait_ms(16);
}

static int abort_o(void)
{
	return (pad_new() & PAD_CIRCLE) != 0;
}

/* sem leitor ou sem disco: X tenta de novo, O sai (nunca tela sem saida) */
static int ask_retry(const char *msg)
{
	u16 b;
	screen_title(msg);
	S("   X = TENTAR DE NOVO      O = SAIR");
	log_save();
	for (;;) {
		b = pad_new();
		if (b & PAD_CROSS)
			return 1;
		if (b & PAD_CIRCLE)
			return 0;
		ee_wait_ms(16);
	}
}

/* ------------------------------------------------------------------ checagem */
static u8 chk_buf[16 * 2352] __attribute__((aligned(64)));

static int find_str(const u8 *b, int n, const char *str)
{
	int l = strlen(str), i;
	for (i = 0; i + l <= n; i++)
		if (b[i] == (u8)str[0] && memcmp(b + i, str, l) == 0)
			return i;
	return -1;
}

/* testa leitor, disco, som e BIOS antes de ligar o nucleo. 1 = X (iniciar),
 * 0 = O (sair), -1 = tentar de novo */
static int check_screen(const cdusb_dev_info *d, int audio_ok)
{
	const cdcd_toc *c = cdcd_get_toc();
	int t, fd = -1, fa = -1, nd = 0, na = 0, k, pce = 0, ok = 1, n;
	u64 t0;
	unsigned ms;

	screen_title("CHECAGEM ANTES DE LIGAR A BIOS");
	S("   LEITOR:  %.8s %.16s %.4s  (%04X:%04X)  OK", d->inquiry + 8, d->inquiry + 16, d->inquiry + 32,
	  d->vid, d->pid);
	for (t = c->first; t <= c->last; t++) {
		if (cdcd_is_data(t)) {
			nd++;
			if (fd < 0)
				fd = t;
		} else {
			na++;
			if (fa < 0 || cdcd_track_end(t) - c->lba[t] > cdcd_track_end(fa) - c->lba[fa])
				fa = t;
		}
	}
	S("   DISCO:   %d FAIXAS (%d DE DADOS, %d DE MUSICA), %d MIN", c->last - c->first + 1, nd, na,
	  c->lba[100] / 4500);

	if (fd < 0) {
		S("   DADOS:   O DISCO NAO TEM FAIXA DE DADOS  <-- NAO E PC ENGINE CD");
		ok = 0;
	} else {
		t0 = ee_now_us();
		n = cdcd_read(c->lba[fd], 16, chk_buf, 12000);
		ms = (unsigned)((ee_now_us() - t0) / 1000);
		if (n == 16) {
			for (k = 0; k < 3; k++)
				if (find_str(chk_buf + k * 2048, 2048, "PC Engine CD-ROM SYSTEM") >= 0)
					pce = 1;
			S("   DADOS:   FAIXA %d, 16 SETORES EM %u MS  OK", fd, ms);
			if (pce)
				S("   SISTEMA: PC ENGINE CD  OK");
			else
				S("   SISTEMA: ASSINATURA DO PC ENGINE NAO ENCONTRADA (PODE NAO FUNCIONAR)");
		} else {
			S("   DADOS:   FALHA NA LEITURA (%d DE 16 EM %u MS)", n, ms);
			ok = 0;
		}
	}
	if (fa >= 0) {
		t0 = ee_now_us();
		n = cdcd_read(c->lba[fa] + 750, 16, chk_buf, 12000);
		ms = (unsigned)((ee_now_us() - t0) / 1000);
		if (n == 16) {
			int nz = 0;
			for (k = 0; k < 16 * 2352; k++)
				if (chk_buf[k])
					nz++;
			S("   MUSICA:  FAIXA %d, 16 SETORES EM %u MS%s", fa, ms, nz ? "  OK" : "  (SO SILENCIO)");
		} else
			S("   MUSICA:  FALHA NA LEITURA (%d DE 16 EM %u MS)", n, ms);
	} else
		S("   MUSICA:  O DISCO NAO TEM FAIXA DE MUSICA");
	S("   SOM:     %s", audio_ok ? "AUDSRV OK" : "FALHA NO AUDIO (O JOGO RODA SEM SOM)");
	S("   BIOS:    SYSTEM CARD EMBUTIDA, %u KB%s", ps2_syscard3_size / 1024,
	  ps2_syscard3_size == 262144 ? "  OK" : "  (TAMANHO ESTRANHO)");
	S("   CONTROLE: O = I   X = II   SELECT   START = RUN   (SELECT+START 1 S = SAIR)");
	S(" ");
	if (!ok) {
		S("   X = TENTAR DE NOVO      O = SAIR");
		log_save();
		for (;;) {
			u16 b = pad_new();
			if (b & PAD_CROSS)
				return -1;
			if (b & PAD_CIRCLE)
				return 0;
			ee_wait_ms(16);
		}
	}
	S("   X = INICIAR      O = SAIR");
	log_save();
	for (;;) {
		u16 b = pad_new();
		if (b & PAD_CROSS)
			return 1;
		if (b & PAD_CIRCLE)
			return 0;
		ee_wait_ms(16);
	}
}

/* ------------------------------------------------------------------ principal */
int main(int argc, char *argv[])
{
	cdusb_status st;
	struct retro_game_info info;
	cdcd_stats cs;
	int cd, r, hold = 0, audio_ready = 0;
	u64 last_stat, last_save_us = 0, run_us_sum = 0, run_us_max = 0, frame_t;
	unsigned skipped = 0, last_skipped = 0, pp;
	int skip_row = 0;
	unsigned last_frames = 0, dm, am, run_count = 0, last_runs = 0, saves = 0;
	(void)argc;
	(void)argv;

	ee_now_us();					/* zero do relogio do log */
	iop_reset();
	ChangeThreadPriority(GetThreadId(), 0x40);	/* leitura e som acima do jogo */
	init_scr();
	screen_title("PREPARANDO...");
	L("PCEUSB " VERSAO " (" __DATE__ " " __TIME__ ") - PC Engine CD pelo leitor USB");
	L("[PASSO %d] IOP reiniciado, tela de texto pronta", ++step_n);

	load_rom("rom0:SIO2MAN");
	load_rom("rom0:PADMAN");
	load_buf("iomanX", irx_iomanx, size_irx_iomanx);
	load_buf("fileXio", irx_filexio, size_irx_filexio);
	load_buf("usbd", irx_usbd, size_irx_usbd);
	L("[PASSO %d] modulos basicos carregados", ++step_n);
	r = load_buf("cdusb", irx_cdusb, size_irx_cdusb);
	if (r == 0)
		r = cdusb_bind();
	S("PROCURANDO O LEITOR...");
	memset(&st, 0, sizeof(st));
	if (r == 0) {
		wait_devices(&st, 8000);
		if (cdusb_finalize(&st) == 0)	/* leitor fica com o cdusb; pendrive com o usbmass */
			wait_devices(&st, 8000);
	}
	load_buf("bdm", irx_bdm, size_irx_bdm);
	load_buf("bdmfs_fatfs", irx_bdmfs_fatfs, size_irx_bdmfs_fatfs);
	load_buf("usbmass_bd", irx_usbmass_bd, size_irx_usbmass_bd);
	load_rom("rom0:LIBSD");
	load_buf("audsrv", irx_audsrv, size_irx_audsrv);
	wait_pendrive();
	L("[BOOT] pendrive %s", log_ok ? "pronto: log em " LOG_FILE : "NAO encontrado");
	{
		int i;
		for (i = 0; i < CDUSB_MAXDEV; i++)
			if (st.dev[i].state != CDUSB_DEV_FREE)
				L("[USB] disp %d: %04X:%04X porta %d estado %d subclasse %02X tipo %d \"%.8s %.16s %.4s\"", i,
				  st.dev[i].vid, st.dev[i].pid, st.dev[i].port, st.dev[i].state, st.dev[i].sub,
				  st.dev[i].ptype, st.dev[i].inquiry + 8, st.dev[i].inquiry + 16, st.dev[i].inquiry + 32);
	}
	STEP("driver do leitor %s, pendrive %s, som carregado", r == 0 ? "OK" : "FALHOU", log_ok ? "OK" : "SEM");
	pad_start();
	if (r != 0) {
		screen_title("FALHA AO CARREGAR O DRIVER DO LEITOR");
		S("   O = SAIR");
		log_save();
		wait_key(PAD_CIRCLE);
		Exit(0);
	}

	/* leitor + disco (pode plugar o leitor com esta tela aberta) */
	for (;;) {
		if (cdusb_get_status(&st) == 0)
			wait_devices(&st, 3000);
		cd = find_cd(&st);
		if (cd < 0) {
			if (!ask_retry("NENHUM LEITOR DE CD ENCONTRADO: PLUGUE O LEITOR"))
				Exit(0);
			continue;
		}
		screen_title("ESPERANDO O DISCO...  (O = SAIR)");
		S("   LEITOR: %.8s %.16s %.4s", st.dev[cd].inquiry + 8, st.dev[cd].inquiry + 16,
		  st.dev[cd].inquiry + 32);
		S("   TRAVANDO O LEITOR EM 10X...");
		STEP("leitor %d encontrado; abrindo o disco", cd);
		r = cdcd_open(cd, st.dev[cd].sub, abort_o);
		STEP("disco: cdcd_open = %d", r);
		if (r != 0) {
			if (!ask_retry("NAO CONSEGUI LER O DISCO"))
				Exit(0);
			continue;
		}
		if (!audio_ready)
			audio_ready = audio_start();
		r = check_screen(&st.dev[cd], audio_ready);
		STEP("checagem: %s", r > 0 ? "X = iniciar" : r < 0 ? "tentar de novo" : "O = sair");
		if (r > 0)
			break;
		cdcd_close();
		if (r == 0) {
			log_save();
			Exit(0);
		}
	}

	screen_title("LIGANDO A BIOS (SYSTEM CARD)...");
	retro_set_environment(environ_cb);
	retro_set_video_refresh(video_cb);
	retro_set_audio_sample(audio_sample_cb);
	retro_set_audio_sample_batch(audio_batch_cb);
	retro_set_input_poll(input_poll_cb);
	retro_set_input_state(input_state_cb);
	STEP("retro_init");
	retro_init();
	STEP("retro_init OK; retro_load_game (System Card + disco)");
	memset(&info, 0, sizeof(info));
	info.path = DISC_PATH;
	if (!retro_load_game(&info)) {
		L("[JOGO] retro_load_game falhou");
		cdcd_close();
		if (!ask_retry("FALHA AO ABRIR O JOGO"))
			Exit(0);
		Exit(0);
	}
	STEP("jogo carregado; ligando o video (gsKit 240p)");
	video_start();
	STEP("video ligado; primeiro quadro");
	last_stat = ee_now_us();
	frame_t = ee_now_us();
	for (;;) {
		u64 r0 = ee_now_us(), rt;
		vid_last_us = 0;
		retro_run();
		/* so a emulacao: sem o envio da imagem e a espera do retraco */
		rt = ee_now_us() - r0 - vid_last_us;
		run_count++;
		run_us_sum += rt;
		if (rt > run_us_max)
			run_us_max = rt;
		/* pular imagem: so quando um quadro sozinho fica pesado demais */
		{
			u64 now = ee_now_us();
			if (vid_last_us == 0 && run_count > 1)
				skipped++;
			(void)now;
			(void)frame_t;
			(void)skip_row;
			/* p03: preso no retraco, um quadro emulado por retraco. A conta
			 * pelo relogio (p02) nunca zerava depois de um retraco perdido e
			 * ficava pulando uma imagem sim, outra nao. Agora so pula a imagem
			 * do proximo quadro se ESTE passou de 20 ms (nunca 2 seguidas) */
			ps2_skip_next = (rt > 20000 && !ps2_skip_next) ? 1 : 0;
		}
		if (run_count == 1)
			STEP("primeiro quadro emulado (%u ms)", (unsigned)(rt / 1000));
		else if (run_count == 60)
			STEP("60 quadros emulados");

		/* SELECT + START (segurar ~1 s) = sair */
		if ((pad_now & (PAD_SELECT | PAD_START)) == (PAD_SELECT | PAD_START)) {
			if (++hold > 60)
				break;
		} else
			hold = 0;

		if (ee_now_us() - last_stat > 5000000) {
			u64 now = ee_now_us(), span = now - last_stat;
			unsigned runs = run_count - last_runs, shown = frames_drawn - last_frames, i, tot = 0;
			char pbuf[200];
			int pl = 0;

			cdcd_get_stats(&cs);
			ps2_cd_stats(&dm, &am, &pp);
			for (i = 0; i < 10; i++)
				tot += prof_cyc[i] / 1000;
			for (i = 0; i < 10 && tot; i++)
				pl += snprintf(pbuf + pl, sizeof(pbuf) - pl, "%s %u%% ", prof_name[i],
					       (prof_cyc[i] / 1000) * 100 / tot);
			pbuf[pl] = 0;
			L("[DESEMPENHO] %u fps na tela, %u quadros emulados/s; emulacao media %u ms, pior %u ms (limite 16,7, sem o video); "
			  "video %u ms media, %u pior; retracos perdidos %u, maior intervalo %u ms",
			  (unsigned)((u64)shown * 1000000 / span), (unsigned)((u64)runs * 1000000 / span),
			  runs ? (unsigned)(run_us_sum / runs / 1000) : 0, (unsigned)(run_us_max / 1000),
			  shown ? (unsigned)(vid_us_sum / shown / 1000) : 0, (unsigned)(vid_us_max / 1000), late_frames,
			  worst_gap_ms);
			L("[DESEMPENHO] imagens puladas %u (o jogo seguiu na velocidade certa)", skipped - last_skipped);
			L("[DESEMPENHO] onde o tempo foi: %s", pbuf);
			last_skipped = skipped;
			L("[SOM] faltas %u, sobras %u, buffer %u de %u (alvo %u); ajuste fino: %u descartadas, %u repetidas",
			  a_under, a_drop, ring_w - ring_r, RING_FRAMES, RING_TARGET, a_adj_drop, a_adj_dup);
			L("[CD] leituras %d, ultima %d ms, pior %d ms, pulos %d, erros %d, manter girando %d (pior %d ms), "
			  "buffer %d setores; dados atrasados %u, musica sem chegar %u, setores de dados adiados %u",
			  cs.cmds, cs.last_ms, cs.max_ms, cs.repositions, cs.errors, cs.keepalives, cs.keep_max_ms, cs.ahead,
			  dm, am, pp);
			memset(prof_cyc, 0, sizeof(prof_cyc));
			memset(prof_cnt, 0, sizeof(prof_cnt));
			run_us_sum = run_us_max = 0;
			vid_us_sum = vid_us_max = 0;
			late_frames = worst_gap_ms = 0;
			last_frames = frames_drawn;
			last_runs = run_count;
			last_stat = now;
		}
		/* log no pendrive: 1 s, 10 s e depois a cada 60 s (pode dar um tranco) */
		{
			u64 now = ee_now_us();
			/* p03: 10 s e depois a cada 5 min (cada gravacao trava ~300 ms) */
			if ((saves == 0 && run_count >= 600) || (saves >= 1 && now - last_save_us > 300000000)) {
				L("[LOG] gravando o log no pendrive (pode dar um tranco)");
				log_save();
				last_save_us = ee_now_us();
				L("[LOG] gravado em %u ms", (unsigned)((last_save_us - now) / 1000));
				saves++;
			}
		}
	}

	STEP("saindo (SELECT + START)");
	a_quit = 1;
	retro_unload_game();
	retro_deinit();
	cdcd_close();
	audsrv_stop_audio();
	log_save();
	Exit(0);
	return 0;
}
