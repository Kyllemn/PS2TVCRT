/*
 * NEUTRINOUSB - abre o Neutrino com o DVD/CD de PS2 da leitora USB.
 * Criado por Leonardo Torres.
 *
 * 1. sobe usbd -> cdusb (+ FINALIZE: o pendrive fica com o usbmass) ->
 *    bdm/usbmass (pendrive), como o EMBERUSB;
 * 2. acha o Neutrino no pendrive e instala nele o usbcd_bd.irx (modules/) e
 *    o bsd-usbcd.toml (config/) (pasta "plana" tambem: SAS);
 * 3. confere o disco (ISO9660 + SYSTEM.CNF/BOOT2);
 * 4. abre o Neutrino SEM reiniciar o IOP (ele le a configuracao dele pelo
 *    pendrive antes de reiniciar o IOP sozinho) com:
 *      -bsd=usbcd -bsdfs=bd -dvd=bdfs:usbcd0p0
 * Log: mass:/NEUTRINOUSB.TXT
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

#define VERSAO		"n02"
#define LOG_FILE	"mass:/NEUTRINOUSB.TXT"

#define IRX(n)	extern unsigned char n[]; extern unsigned int size_##n
IRX(irx_iomanx);
IRX(irx_filexio);
IRX(irx_usbd);
IRX(irx_cdusb);
IRX(irx_bdm);
IRX(irx_bdmfs_fatfs);
IRX(irx_usbmass_bd);
IRX(irx_usbcd_bd);
IRX(irx_usbd_mini);
IRX(loader_elf);

/* ------------------------------------------------------------------ abrir o Neutrino */
/* como o elf-loader da ps2sdk, mas com o nosso mini-loader (../loader), que
 * nunca reinicia o IOP. Nao volta se der certo. */
typedef struct {
	u8 ident[16];
	u16 type, machine;
	u32 version, entry, phoff, shoff, flags;
	u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf_hdr;

typedef struct {
	u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
} elf_phdr;

static int exec_noreset(const char *path, int argc, char *argv[])
{
	elf_hdr *eh = (elf_hdr *)loader_elf;
	elf_phdr *ph;
	char *nargv[3 + 4];
	int i;

	if (argc > 4)
		return -1;
	if (memcmp(eh->ident, "\x7f" "ELF", 4) != 0)
		return -2;
	nargv[0] = "";
	nargv[1] = (char *)path;
	nargv[2] = "0";
	for (i = 0; i < argc; i++)
		nargv[3 + i] = argv[i];
	ph = (elf_phdr *)(loader_elf + eh->phoff);
	for (i = 0; i < eh->phnum; i++) {
		if (ph[i].type != 1)
			continue;
		memcpy((void *)ph[i].vaddr, loader_elf + ph[i].offset, ph[i].filesz);
		if (ph[i].memsz > ph[i].filesz)
			memset((u8 *)ph[i].vaddr + ph[i].filesz, 0, ph[i].memsz - ph[i].filesz);
	}
	sceSifExitRpc();
	FlushCache(0);
	FlushCache(2);
	return ExecPS2((void *)eh->entry, NULL, argc + 3, nargv);
}

/* ------------------------------------------------------------------ log e tela */
static char logbuf[64 * 1024];
static int loglen, log_ok;

static void log_add(const char *s)
{
	int n = strlen(s);
	if (loglen + n + 2 < (int)sizeof(logbuf)) {
		memcpy(logbuf + loglen, s, n);
		loglen += n;
		logbuf[loglen++] = '\r';
		logbuf[loglen++] = '\n';
	}
}

static void L(const char *fmt, ...)
{
	char s[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);
	log_add(s);
}

static void S(const char *fmt, ...)
{
	char s[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);
	log_add(s);
	scr_printf("%s\n", s);
}

static void log_save(void)
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

static void title(const char *sub)
{
	scr_clear();
	scr_setXY(0, 1);
	scr_setfontcolor(0x00d0ff);
	scr_printf("   NEUTRINOUSB " VERSAO " - JOGOS DE PS2 PELA LEITORA USB (NEUTRINO)\n");
	scr_setfontcolor(0xffffff);
	if (sub)
		scr_printf("   %s\n", sub);
	scr_printf("\n");
}

/* ------------------------------------------------------------------ controle */
static char pad_area[256] __attribute__((aligned(64)));
static int pad_ok;
static u16 pad_prev = 0xffff;

static u16 pad_new(void)
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

/* X = 1, O = 0 */
static int ask(void)
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

static void fail(const char *msg)
{
	S(" ");
	S("   %s", msg);
	S("   O = SAIR");
	log_save();
	while (!(pad_new() & PAD_CIRCLE))
		ee_wait_ms(16);
	Exit(0);
}

/* ------------------------------------------------------------------ modulos */
static int load_buf(const char *name, void *p, unsigned int size, int arglen, const char *args)
{
	int ret, mr = -1;
	ret = SifExecModuleBuffer(p, size, arglen, args, &mr);
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

/* ------------------------------------------------------------------ disco */
static int dev, sub;
static cdusb_scsi_resp R;
static u8 sec[2352] __attribute__((aligned(64)));
static int t_first, t_last, t_lba[101], t_ctrl[101];

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

static int cmd(const u8 *cdb, int dir, void *data, int len, int tmo)
{
	return cdusb_scsi(dev, cdb, cdb_len(cdb[0]), dir, data, len, tmo, &R);
}

/* espera o disco; O desiste */
static int wait_disc(void)
{
	u8 c[12] = {0};
	int r, shown = 0;
	u32 t0 = ee_ticks();

	for (;;) {
		if (pad_new() & PAD_CIRCLE)
			return 0;
		r = cmd(c, 0, NULL, 0, 3000);
		if (r == CDUSB_OK)
			return 1;
		if (r == CDUSB_CHECK && ((R.sense >> 16) & 0x0f) == 2 && ((R.sense >> 8) & 0xff) == 0x3A) {
			if (shown != 1)
				S("   SEM DISCO: COLOQUE O CD DE PS1 NA LEITORA  (O = SAIR)");
			shown = 1;
		} else if (r == CDUSB_CHECK && ((R.sense >> 16) & 0x0f) == 6) {
			continue;
		} else if (shown != 2) {
			S("   PREPARANDO O DISCO...");
			shown = 2;
		}
		if (r < 0 && ee_ms(t0) > 60000)
			return 0;
		ee_wait_ms(300);
	}
}

/* setor de dados (2048) por READ(10) */
static const u8 *read_sec(int lba)
{
	u8 c[12] = {0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
	c[2] = lba >> 24; c[3] = lba >> 16; c[4] = lba >> 8; c[5] = lba;
	if (cmd(c, 1, sec, 2048, 8000) != CDUSB_OK)
		return NULL;
	return sec;
}

static u32 le32(const u8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

/* 0 = PS2 (code = SLUS_xxx.xx), 1 = PS1, < 0 = nao e PlayStation/erro */
static int disc_id(char *code, int max, char *vol)
{
	const u8 *u = read_sec(16);
	u32 root, rsize, off, k, f_lba = 0;
	char txt[2049], *p;
	int n = 0, i, ps2;

	code[0] = vol[0] = 0;
	if (!u || memcmp(u + 1, "CD001", 5) != 0)
		return -1;
	if (memcmp(u + 8, "PLAYSTATION", 11) != 0)
		return -2;
	memcpy(vol, u + 40, 32);
	vol[32] = 0;
	for (i = 31; i >= 0 && vol[i] == ' '; i--)
		vol[i] = 0;
	root = le32(u + 156 + 2);
	rsize = le32(u + 156 + 10);
	for (k = 0; k * 2048 < rsize && k < 8 && !f_lba; k++) {
		const u8 *d = read_sec(root + k);
		if (!d)
			return -3;
		for (off = 0; off < 2048 && d[off];) {
			if (d[off + 32] >= 10 && memcmp(d + off + 33, "SYSTEM.CNF", 10) == 0) {
				f_lba = le32(d + off + 2);
				break;
			}
			off += d[off];
		}
	}
	if (!f_lba || !(u = read_sec(f_lba)))
		return -4;
	memcpy(txt, u, 2048);
	txt[2048] = 0;
	L("[DISCO] SYSTEM.CNF: %.160s", txt);
	ps2 = strstr(txt, "BOOT2") != NULL;
	p = strstr(txt, "cdrom");
	if (p && (p = strchr(p, ':')) != NULL) {
		p++;
		while (*p == '\\')
			p++;
		while (*p && *p != ';' && *p != '\r' && *p != '\n' && n < max - 1)
			code[n++] = *p++;
		code[n] = 0;
	}
	return ps2 ? 0 : 1;
}

/* ------------------------------------------------------------------ Neutrino */
static int exists(const char *path)
{
	iox_stat_t s;
	return fileXioGetStat(path, &s) >= 0;
}

static int file_size(const char *path)
{
	iox_stat_t s;
	return fileXioGetStat(path, &s) >= 0 ? (int)s.size : -1;
}

/* grava o arquivo se faltar ou for diferente (tamanho); devolve 0 = ok */
static int install(const char *path, const void *data, int size, int only_if_missing)
{
	int fd, cur = file_size(path);

	if (cur == size || (only_if_missing && cur >= 0)) {
		L("[INSTALAR] %s ja esta la (%d bytes)", path, cur);
		return 0;
	}
	fd = fileXioOpen(path, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC, 0666);
	if (fd < 0) {
		L("[INSTALAR] %s: falhou abrir (%d)", path, fd);
		return -1;
	}
	if (fileXioWrite(fd, (void *)data, size) != size) {
		fileXioClose(fd);
		L("[INSTALAR] %s: falhou gravar", path);
		return -1;
	}
	fileXioClose(fd);
	L("[INSTALAR] %s gravado (%d bytes)", path, size);
	return 0;
}

static const char toml_usbcd[] =
	"# Name of loaded config, to show to user\n"
	"name = \"USB CD/DVD drive (usbcd_bd)\"\n"
	"\n"
	"# Leitora de CD/DVD USB externa como dispositivo de bloco (NEUTRINOUSB).\n"
	"# Uso: neutrino.elf -bsd=usbcd -bsdfs=bd -dvd=bdfs:usbcd0p0\n"
	"\n"
	"# Drivers this driver depends on (config file must exist)\n"
	"depends = [\"i_bdm\"]\n"
	"\n"
	"# Modules to load\n"
	"[[module]]\n"
	"file = \"usbd_mini.irx\"\n"
	"args = [\"thpri=7,8\"]\n"
	"env = [\"LE\", \"EE\"]\n"
	"[[module]]\n"
	"file = \"usbcd_bd.irx\"\n"
	"env = [\"LE\", \"EE\"]\n"
	"\n"
	"# Modules of the game that are faked/blocked\n"
	"[[fake]]\n"
	"file = \"USBD.IRX\"\n"
	"name = \"USB_driver\"\n"
	"version = 0x0204\n"
	"startrv = 2 # 0=RESIDENT_END, 1=NO_RESIDENT_END, 2=REMOVABLE_END\n";

static const char *neutrino_paths[] = {
	"mass:/neutrino/neutrino.elf", "mass:/APPS/neutrino/neutrino.elf", "mass:/APP/neutrino/neutrino.elf",
	"mass:/APPS/NEUTRINO/neutrino.elf", "mass:/neutrino.elf", NULL,
};

int main(int argc, char *argv[])
{
	cdusb_status st;
	static char ndir[160], path[220], cfgdir[200], moddir[200], code[32], vol[33], cwd[200];
	const char *np = NULL;
	char *nargv[4];
	int r, i, fd, flat;

	iop_reset();
	init_scr();
	title("PREPARANDO...");
	L("NEUTRINOUSB " VERSAO " (" __DATE__ " " __TIME__ ")");

	load_rom("rom0:XSIO2MAN");
	load_rom("rom0:XPADMAN");
	load_buf("iomanX", irx_iomanx, size_irx_iomanx, 0, NULL);
	load_buf("fileXio", irx_filexio, size_irx_filexio, 0, NULL);
	load_buf("usbd", irx_usbd, size_irx_usbd, 0, NULL);
	r = load_buf("cdusb", irx_cdusb, size_irx_cdusb, 0, NULL);
	if (r == 0)
		r = cdusb_bind();
	S("   PROCURANDO A LEITORA...");
	memset(&st, 0, sizeof(st));
	if (r == 0) {
		wait_devices(&st, 8000);
		if (cdusb_finalize(&st) == 0)
			wait_devices(&st, 8000);
	}
	load_buf("bdm", irx_bdm, size_irx_bdm, 0, NULL);
	load_buf("bdmfs_fatfs", irx_bdmfs_fatfs, size_irx_bdmfs_fatfs, 0, NULL);
	load_buf("usbmass_bd", irx_usbmass_bd, size_irx_usbmass_bd, 0, NULL);
	fileXioInit();
	for (i = 0; i < 40; i++) {
		fd = fileXioDopen("mass:/");
		if (fd >= 0) {
			fileXioDclose(fd);
			log_ok = 1;
			break;
		}
		ee_wait_ms(250);
	}
	padInit(0);
	pad_ok = padPortOpen(0, 0, pad_area) != 0;
	if (r != 0)
		fail("FALHA AO CARREGAR O DRIVER DA LEITORA (CDUSB)");
	if (!log_ok)
		fail("PENDRIVE NAO ENCONTRADO (O NEUTRINO FICA NELE)");

	/* Neutrino */
	for (i = 0; neutrino_paths[i]; i++)
		if (exists(neutrino_paths[i])) {
			np = neutrino_paths[i];
			break;
		}
	if (!np)
		fail("NEUTRINO NAO ENCONTRADO: COLOQUE EM mass:/neutrino/neutrino.elf");
	strcpy(ndir, np);
	*strrchr(ndir, '/') = 0;			/* "mass:/neutrino" */
	snprintf(path, sizeof(path), "%s/config/system.toml", ndir);
	flat = !exists(path);
	snprintf(cfgdir, sizeof(cfgdir), flat ? "%s" : "%s/config", ndir);
	snprintf(moddir, sizeof(moddir), flat ? "%s" : "%s/modules", ndir);
	L("[NEUTRINO] %s (%s)", np, flat ? "pasta plana" : "config/ e modules/");
	snprintf(path, sizeof(path), "%s/usbcd_bd.irx", moddir);
	if (install(path, irx_usbcd_bd, size_irx_usbcd_bd, 0) < 0)
		fail("NAO CONSEGUI GRAVAR O usbcd_bd.irx NA PASTA DO NEUTRINO");
	snprintf(path, sizeof(path), "%s/usbd_mini.irx", moddir);
	install(path, irx_usbd_mini, size_irx_usbd_mini, 1);
	snprintf(path, sizeof(path), "%s/bsd-usbcd.toml", cfgdir);
	if (install(path, toml_usbcd, sizeof(toml_usbcd) - 1, 0) < 0)
		fail("NAO CONSEGUI GRAVAR O bsd-usbcd.toml NA PASTA DO NEUTRINO");

	/* leitora e disco */
	for (;;) {
		if (cdusb_get_status(&st) == 0)
			wait_devices(&st, 3000);
		dev = find_cd(&st);
		if (dev >= 0)
			break;
		title("NENHUMA LEITORA DE CD/DVD ENCONTRADA");
		S("   PLUGUE A LEITORA (CABO Y LIGADO NO CARREGADOR).");
		S("   X = PROCURAR DE NOVO      O = SAIR");
		log_save();
		if (!ask())
			Exit(0);
	}
	sub = st.dev[dev].sub;
	title("ESPERANDO O DISCO...");
	S("   LEITORA: %.8s %.16s", st.dev[dev].inquiry + 8, st.dev[dev].inquiry + 16);
	if (!wait_disc())
		fail("O DISCO NAO FICOU PRONTO");
	r = disc_id(code, sizeof(code), vol);
	L("[DISCO] resultado %d, codigo %s, volume \"%s\"", r, code, vol);
	if (r == 1)
		fail("ESTE E UM CD DE PS1: USE O EMBERUSB");
	if (r < 0)
		fail("ESTE DISCO NAO E DE PLAYSTATION 2");

	title("PRONTO");
	S("   LEITORA:  %.8s %.16s", st.dev[dev].inquiry + 8, st.dev[dev].inquiry + 16);
	S("   JOGO:     %s  (%s)", code[0] ? code : "?", vol[0] ? vol : "?");
	S("   NEUTRINO: %s", np);
	S(" ");
	S("   X = JOGAR      O = SAIR");
	log_save();
	if (!ask())
		Exit(0);

	snprintf(cwd, sizeof(cwd), "-cwd=%s", ndir);
	nargv[0] = "-bsd=usbcd";
	nargv[1] = "-bsdfs=bd";
	nargv[2] = "-dvd=bdfs:usbcd0p0";
	nargv[3] = cwd;
	L("[NEUTRINO] abrindo %s %s %s %s %s (sem reiniciar o IOP)", np, nargv[0], nargv[1], nargv[2], nargv[3]);
	log_save();
	padPortClose(0, 0);
	padEnd();
	r = exec_noreset(np, 4, nargv);
	pad_ok = 0;
	L("[NEUTRINO] falhou ao abrir: %d", r);
	log_save();
	title("FALHA AO ABRIR O NEUTRINO");
	S("   %s: %d", np, r);
	for (;;)
		ee_wait_ms(1000);
	(void)argc;
	(void)argv;
	return 0;
}
