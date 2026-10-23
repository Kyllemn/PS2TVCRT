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

#define VERSAO		"p01"
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

void ps2_cd_stats(unsigned *data_miss, unsigned *audio_miss);

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

void L(const char *fmt, ...)
{
	char s[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s, sizeof(s), fmt, ap);
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
	log_add(s);
	if (text_mode)
		scr_printf("%s\n", s);
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

static int16_t ring[RING_FRAMES * 2] __attribute__((aligned(64)));
static volatile unsigned ring_w, ring_r;	/* em quadros de som (L+R) */
static volatile int a_quit, a_done;
static volatile unsigned a_under, a_drop;
static u8 a_out[2048 * 4] __attribute__((aligned(64)));
static u8 a_stack[16 * 1024] __attribute__((aligned(16)));
extern void *_gp;

static void audio_thread(void *arg)
{
	int avail, n, i, under = 0;
	(void)arg;

	while (!a_quit) {
		avail = audsrv_available() / 4;		/* quadros livres no audsrv */
		while (avail > 0 && !a_quit) {
			unsigned w = ring_w, r = ring_r, have = w - r;
			int16_t *o = (int16_t *)a_out;
			n = avail > 2048 ? 2048 : avail;
			if (have == 0) {
				/* o jogo atrasou: silencio, nunca deixar o audsrv vazio */
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

static size_t RETRO_CALLCONV audio_batch_cb(const int16_t *data, size_t frames)
{
	size_t i;
	unsigned w = ring_w;

	for (i = 0; i < frames; i++) {
		if (w - ring_r >= RING_FRAMES - 1) {
			a_drop++;			/* buffer cheio: o jogo esta adiantado */
			break;
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
}

/* ------------------------------------------------------------------ nucleo */
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

/* ------------------------------------------------------------------ principal */
int main(int argc, char *argv[])
{
	cdusb_status st;
	struct retro_game_info info;
	cdcd_stats cs;
	int cd, r, hold = 0;
	u64 last_stat;
	unsigned last_frames = 0, dm, am;
	(void)argc;
	(void)argv;

	iop_reset();
	ChangeThreadPriority(GetThreadId(), 0x40);	/* leitura e som acima do jogo */
	init_scr();
	screen_title("PREPARANDO...");
	L("PCEUSB " VERSAO " (" __DATE__ " " __TIME__ ") - PC Engine CD pelo leitor USB");

	load_rom("rom0:SIO2MAN");
	load_rom("rom0:PADMAN");
	load_buf("iomanX", irx_iomanx, size_irx_iomanx);
	load_buf("fileXio", irx_filexio, size_irx_filexio);
	load_buf("usbd", irx_usbd, size_irx_usbd);
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
		r = cdcd_open(cd, st.dev[cd].sub, abort_o);
		log_save();
		if (r == 0)
			break;
		if (!ask_retry("NAO CONSEGUI LER O DISCO"))
			Exit(0);
	}

	S("   CARREGANDO O JOGO...");
	if (!audio_start())
		S("   (SEM SOM: FALHA NO AUDIO)");
	retro_set_environment(environ_cb);
	retro_set_video_refresh(video_cb);
	retro_set_audio_sample(audio_sample_cb);
	retro_set_audio_sample_batch(audio_batch_cb);
	retro_set_input_poll(input_poll_cb);
	retro_set_input_state(input_state_cb);
	retro_init();
	memset(&info, 0, sizeof(info));
	info.path = DISC_PATH;
	if (!retro_load_game(&info)) {
		L("[JOGO] retro_load_game falhou");
		cdcd_close();
		if (!ask_retry("FALHA AO ABRIR O JOGO"))
			Exit(0);
		Exit(0);
	}
	L("[JOGO] jogo carregado");
	log_save();

	video_start();
	last_stat = ee_now_us();
	for (;;) {
		retro_run();

		/* SELECT + START (segurar ~1 s) = sair */
		if ((pad_now & (PAD_SELECT | PAD_START)) == (PAD_SELECT | PAD_START)) {
			if (++hold > 60)
				break;
		} else
			hold = 0;

		if (ee_now_us() - last_stat > 10000000) {
			u64 now = ee_now_us();
			cdcd_get_stats(&cs);
			ps2_cd_stats(&dm, &am);
			L("[JOGO] %u quadros em %u ms (%u por s); som: %u faltas, %u sobras; CD: %d leituras, pior %d ms, "
			  "%d pulos, %d erros, manter girando %d; dados atrasados %u, musica sem chegar %u",
			  frames_drawn - last_frames, (unsigned)((now - last_stat) / 1000),
			  (unsigned)((u64)(frames_drawn - last_frames) * 1000000 / (now - last_stat)), a_under, a_drop,
			  cs.cmds, cs.max_ms, cs.repositions, cs.errors, cs.keepalives, dm, am);
			last_frames = frames_drawn;
			last_stat = now;
		}
	}

	L("[JOGO] saindo");
	a_quit = 1;
	retro_unload_game();
	retro_deinit();
	cdcd_close();
	audsrv_stop_audio();
	log_save();
	Exit(0);
	return 0;
}
