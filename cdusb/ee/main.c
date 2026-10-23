/*
 * main.c - CDUSB, diagnostico do leitor de CD USB no PS2 (fase 1).
 * Criado por Leonardo Torres.
 *
 * Ordem dos modulos (importa):
 *   usbd -> cdusb (pega todos os dispositivos de armazenamento e le o
 *   INQUIRY) -> FINALIZE (fica so com leitores de CD) -> bdm + usbmass_bd
 *   (o pendrive fica com eles; o log vai para mass:/CDUSB.TXT).
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>
#include <libpad.h>
#include <debug.h>

#include "cdusb_ee.h"
#include "diag.h"
#include "play.h"

#define VERSAO		"c05"
#define LOG_FILE	"mass:/CDUSB.TXT"

#define IRX(n)	extern unsigned char n[]; extern unsigned int size_##n
IRX(irx_iomanx);
IRX(irx_filexio);
IRX(irx_usbd);
IRX(irx_cdusb);
IRX(irx_bdm);
IRX(irx_bdmfs_fatfs);
IRX(irx_usbmass_bd);
IRX(irx_audsrv);

/* ------------------------------------------------------------------ log */
static char logbuf[256 * 1024];
static int loglen, log_ok;

/* chamado pela tela, pela leitura do disco e pelo audio: sem interrupcoes */
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

void S(const char *fmt, ...)
{
	char s[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);
	log_add(s);
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

/* ------------------------------------------------------------------ tela */
void screen_title(const char *sub)
{
	scr_clear();
	scr_setXY(0, 1);
	scr_setfontcolor(0x00d0ff);
	scr_printf("   CDUSB " VERSAO " - LEITOR DE CD USB NO PS2\n");
	scr_setfontcolor(0xffffff);
	if (sub)
		scr_printf("   %s\n", sub);
	scr_printf("\n");
}

/* ------------------------------------------------------------------ controle */
static char pad_area[256] __attribute__((aligned(64)));
static int pad_ok;
static u16 pad_prev = 0xffff;

static void pad_start(void)
{
	padInit(0);
	pad_ok = padPortOpen(0, 0, pad_area) != 0;
}

/* botoes recem-apertados (PAD_CROSS, PAD_CIRCLE...) */
u16 pad_new(void)
{
	struct padButtonStatus b;
	int s;
	u16 now, n;

	if (!pad_ok)
		return 0;
	s = padGetState(0, 0);
	if (s != PAD_STATE_STABLE && s != PAD_STATE_FINDCTP1)
		return 0;
	if (padRead(0, 0, &b) == 0)
		return 0;
	now = 0xffff ^ b.btns;
	n = now & ~pad_prev;
	pad_prev = now;
	return n;
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

static const char *state_txt(int s)
{
	switch (s) {
	case CDUSB_DEV_CONNECTED: return "CONECTADO";
	case CDUSB_DEV_READY: return "PRONTO";
	case CDUSB_DEV_ERROR: return "ERRO";
	case CDUSB_DEV_GONE: return "DESCONECTADO";
	default: return "LIVRE";
	}
}

static void log_devices(const cdusb_status *st, const char *when)
{
	int i;
	L("[USB] %s: versao %d, %s, %d conexoes, %d desconexoes", when, st->version,
	  st->finalized ? "so leitores de CD" : "todos os dispositivos", st->connects, st->disconnects);
	for (i = 0; i < CDUSB_MAXDEV; i++) {
		const cdusb_dev_info *d = &st->dev[i];
		if (d->state == CDUSB_DEV_FREE)
			continue;
		L("[USB] disp %d: %04X:%04X USB %X.%02X porta %d %s; interface %02X/%02X/%02X; "
		  "pacotes %d/%d; pede %d mA; tipo %d; erro %d; comandos %d, prazos %d, resets %d",
		  i, d->vid, d->pid, d->bcd_usb >> 8, d->bcd_usb & 0xff, d->port, state_txt(d->state),
		  d->cls, d->sub, d->proto, d->mps_in, d->mps_out, d->max_power * 2, d->ptype,
		  d->last_err, d->cmds, d->timeouts, d->resets);
		if (d->ptype != 0xFF)
			L("[USB] disp %d: INQUIRY \"%.8s\" \"%.16s\" \"%.4s\"", i, d->inquiry + 8,
			  d->inquiry + 16, d->inquiry + 32);
	}
}

/* espera os dispositivos terminarem de configurar (ate ms) */
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
		/* da 1,5 s para o usbd achar tudo antes de aceitar "nenhum ocupado" */
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

/* espera X (repetir) ou O (sair) */
static int ask_again(void)
{
	u16 b;
	for (;;) {
		b = pad_new();
		if (b & PAD_CROSS)
			return 1;
		if (b & PAD_CIRCLE)
			return 0;
		ee_wait_ms(16);
	}
}

int main(int argc, char *argv[])
{
	cdusb_status st;
	int cd;
	(void)argc;
	(void)argv;

	iop_reset();
	/* a thread de leitura do disco (0x30) tem prioridade sobre esta */
	ChangeThreadPriority(GetThreadId(), 0x40);
	init_scr();
	screen_title("PREPARANDO...");
	L("CDUSB " VERSAO " (" __DATE__ " " __TIME__ ") - diagnostico do leitor de CD USB");

	load_rom("rom0:SIO2MAN");
	load_rom("rom0:PADMAN");
	load_buf("iomanX", irx_iomanx, size_irx_iomanx);
	load_buf("fileXio", irx_filexio, size_irx_filexio);
	load_buf("usbd", irx_usbd, size_irx_usbd);
	if (load_buf("cdusb", irx_cdusb, size_irx_cdusb) < 0 || cdusb_bind() < 0) {
		S("FALHA AO CARREGAR O DRIVER CDUSB");
		/* sem o driver, pelo menos o pendrive para gravar o log */
		load_buf("bdm", irx_bdm, size_irx_bdm);
		load_buf("bdmfs_fatfs", irx_bdmfs_fatfs, size_irx_bdmfs_fatfs);
		load_buf("usbmass_bd", irx_usbmass_bd, size_irx_usbmass_bd);
		wait_pendrive();
		log_save();
		pad_start();
		S("O = SAIR");
		while (!(pad_new() & PAD_CIRCLE))
			ee_wait_ms(16);
		Exit(0);
	}

	S("PROCURANDO O LEITOR...");
	memset(&st, 0, sizeof(st));
	wait_devices(&st, 8000);
	log_devices(&st, "antes de separar");

	/* fica so com o leitor de CD; o pendrive vai para o usbmass */
	if (cdusb_finalize(&st) == 0)
		wait_devices(&st, 8000);
	log_devices(&st, "depois de separar");

	load_buf("bdm", irx_bdm, size_irx_bdm);
	load_buf("bdmfs_fatfs", irx_bdmfs_fatfs, size_irx_bdmfs_fatfs);
	load_buf("usbmass_bd", irx_usbmass_bd, size_irx_usbmass_bd);
	/* audio: libsd da ROM + audsrv */
	load_rom("rom0:LIBSD");
	load_buf("audsrv", irx_audsrv, size_irx_audsrv);
	S("PROCURANDO O PENDRIVE...");
	wait_pendrive();
	L("[BOOT] pendrive %s", log_ok ? "pronto: log em " LOG_FILE : "NAO encontrado (log so na tela)");
	log_save();
	pad_start();

	for (;;) {
		u16 b;

		if (cdusb_get_status(&st) == 0)
			wait_devices(&st, 3000);
		cd = find_cd(&st);
		screen_title("MENU");
		if (cd >= 0)
			S("   LEITOR: %.8s %.16s %.4s", st.dev[cd].inquiry + 8, st.dev[cd].inquiry + 16,
			  st.dev[cd].inquiry + 32);
		else {
			S("   NENHUM LEITOR DE CD ENCONTRADO");
			log_devices(&st, "procura");
			S("   PLUGUE O LEITOR: ELE APARECE AQUI SOZINHO.");
			S("   CONFIRA O CABO Y E SE O LEITOR ACENDE.");
		}
		if (!log_ok)
			S("   (SEM PENDRIVE: O LOG NAO E GRAVADO)");
		log_save();
		scr_printf("\n   X = TOCAR A MUSICA DO DISCO (FASE 2)\n");
		scr_printf("   QUADRADO = DIAGNOSTICO DO LEITOR (FASE 1)\n");
		scr_printf("   O = SAIR\n");
		/* espera um botao; se o leitor aparecer ou sumir, redesenha */
		{
			int n = 0, changed = 0;
			do {
				b = pad_new();
				ee_wait_ms(16);
				if (++n % 30 == 0 && cdusb_get_status(&st) == 0 && find_cd(&st) != cd) {
					changed = 1;
					break;
				}
			} while (!(b & (PAD_CROSS | PAD_SQUARE | PAD_CIRCLE)));
			if (changed) {
				ee_wait_ms(500);	/* deixa o leitor terminar de configurar */
				continue;
			}
		}
		if (b & PAD_CIRCLE)
			break;
		if (cd < 0)
			continue;
		if (b & PAD_CROSS) {
			play_run(cd, st.dev[cd].sub);
		} else {
			diag_result r;
			diag_run(cd, &st.dev[cd], &r);
			if (cdusb_get_status(&st) == 0)
				log_devices(&st, "fim do teste");
			diag_summary(&r, log_ok ? LOG_FILE : NULL);
			log_save();
			scr_printf("\n   X = VOLTAR AO MENU      O = SAIR\n");
			if (!ask_again())
				break;
		}
	}

	log_save();
	Exit(0);
	return 0;
}
