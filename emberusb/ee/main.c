/*
 * EMBERUSB - abre o Ember (PS1 no PS2) com o CD da leitora USB.
 * Criado por Leonardo Torres.
 *
 * 1. sobe os modulos: usbd -> cdusb (+ FINALIZE: o pendrive fica com o
 *    usbmass) -> bdm/usbmass (pendrive);
 * 2. acha o Ember no pendrive e o CD de PS1 na leitora; le o SYSTEM.CNF
 *    para dar a pasta do jogo (ex.: SLUS-00606, onde ficam os saves);
 * 3. sobe o vcd.irx: vcd:/... = mass:/..., e na pasta do jogo aparecem
 *    <jogo>.cue e <jogo>.bin vindos da leitora;
 * 4. abre vcd:/<pasta do Ember>/ember.elf SEM reiniciar o IOP (regra do
 *    Ember: ele usa os drivers que o lancador deixou carregados).
 * Log: mass:/EMBERUSB.TXT; o vcd.irx loga em <pasta do jogo>/VCD.TXT.
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

#define VERSAO		"e01"
#define LOG_FILE	"mass:/EMBERUSB.TXT"
#define CACHE_BLOCKS	"blocks=8"

#define IRX(n)	extern unsigned char n[]; extern unsigned int size_##n
IRX(irx_iomanx);
IRX(irx_filexio);
IRX(irx_usbd);
IRX(irx_cdusb);
IRX(irx_bdm);
IRX(irx_bdmfs_fatfs);
IRX(irx_usbmass_bd);
IRX(irx_vcd);
IRX(loader_elf);

/* ------------------------------------------------------------------ abrir o Ember */
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
	scr_printf("   EMBERUSB " VERSAO " - JOGOS DE PS1 PELA LEITORA USB (EMBER)\n");
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

static int read_toc(void)
{
	static u8 b[4 + 100 * 8] __attribute__((aligned(64)));
	u8 c[12] = {0x43, 0, 0, 0, 0, 0, 1, 0, 4, 0, 0, 0};
	int len, i, have_end = 0;

	if (cmd(c, 1, b, 4, 5000) != CDUSB_OK)
		return 0;
	len = (b[0] << 8 | b[1]) + 2;
	if (len < 12 || len > (int)sizeof(b))
		return 0;
	c[7] = len >> 8;
	c[8] = len;
	if (cmd(c, 1, b, len, 5000) != CDUSB_OK)
		return 0;
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
	return t_first >= 1 && t_last >= t_first && t_last <= 99 && have_end && t_lba[100] > t_lba[t_last];
}

/* setor de dados cru; devolve os 2048 bytes do usuario (modo 2 em +24, modo 1 em +16) */
static const u8 *read_user(int lba)
{
	u8 c[12] = {0xBE, 0, 0, 0, 0, 0, 0, 0, 1, 0xF8, 0, 0};
	c[2] = lba >> 24; c[3] = lba >> 16; c[4] = lba >> 8; c[5] = lba;
	if (cmd(c, 1, sec, 2352, 8000) != CDUSB_OK)
		return NULL;
	return sec + (sec[15] == 2 ? 24 : 16);
}

static u32 le32(const u8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

static int upper(int c)
{
	return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

/* "BOOT = cdrom:\SLUS_006.06;1" -> "SLUS-00606" */
static int game_id(char *out, int max, char *vol)
{
	const u8 *u = read_user(16);
	u32 root, rsize, off, k, f_lba = 0;
	char txt[2049], *p, *q;
	int n = 0, i;

	out[0] = vol[0] = 0;
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
		const u8 *d = read_user(root + k);
		if (!d)
			return -3;
		for (off = 0; off < 2048 && d[off];) {
			int rl = d[off], nl = d[off + 32];
			if (nl >= 10 && memcmp(d + off + 33, "SYSTEM.CNF", 10) == 0) {
				f_lba = le32(d + off + 2);
				break;
			}
			off += rl;
		}
	}
	if (!f_lba)
		return -4;
	u = read_user(f_lba);
	if (!u)
		return -5;
	memcpy(txt, u, 2048);
	txt[2048] = 0;
	L("[DISCO] SYSTEM.CNF: %.120s", txt);
	p = strstr(txt, "cdrom:");
	if (!p)
		p = strstr(txt, "CDROM:");
	if (!p)
		return -6;
	p += 6;
	while (*p == '\\' || *p == '/')
		p++;
	for (q = p; *q && *q != ';' && *q != '\r' && *q != '\n' && n < max - 2; q++) {
		int c = upper(*q);
		if (c == '\\' || c == '/') {
			n = 0;			/* pasta dentro do disco: fica so o arquivo */
			continue;
		}
		if (c == '.')
			continue;
		if (c == '_')
			c = '-';
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')
			out[n++] = c;
	}
	out[n] = 0;
	return n > 0 ? 0 : -7;
}

/* ------------------------------------------------------------------ Ember */
static int exists(const char *path)
{
	iox_stat_t s;
	return fileXioGetStat(path, &s) >= 0;
}

static const char *ember_paths[] = {
	"mass:/Ember/ember.elf", "mass:/APPS/Ember/ember.elf", "mass:/APP/Ember/ember.elf",
	"mass:/Ember/EMBER.ELF", "mass:/ember.elf", NULL,
};

int main(int argc, char *argv[])
{
	cdusb_status st;
	static char edir[160], gdir[200], gname[48], vol[33], vpath[220], cue[300], args[400], arg1[120];
	const char *ep = NULL;
	char *eargv[1];
	int r, i, n, fd;

	iop_reset();
	init_scr();
	title("PREPARANDO...");
	L("EMBERUSB " VERSAO " (" __DATE__ " " __TIME__ ")");

	load_rom("rom0:SIO2MAN");
	load_rom("rom0:PADMAN");
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
		fail("PENDRIVE NAO ENCONTRADO (O EMBER FICA NELE)");

	/* Ember */
	for (i = 0; ember_paths[i]; i++)
		if (exists(ember_paths[i])) {
			ep = ember_paths[i];
			break;
		}
	if (!ep)
		fail("EMBER NAO ENCONTRADO: COLOQUE EM mass:/Ember/ember.elf");
	strcpy(edir, ep + 5);				/* sem "mass:" */
	*strrchr(edir, '/') = 0;			/* pasta do Ember ("" = raiz) */
	L("[EMBER] %s (pasta \"%s\")", ep, edir);
	snprintf(vpath, sizeof(vpath), "mass:%s/bios.bin", edir);
	if (!exists(vpath))
		S("   AVISO: NAO ACHEI %s (O EMBER PRECISA DA BIOS)", vpath);

	/* leitora e disco */
	for (;;) {
		if (cdusb_get_status(&st) == 0)
			wait_devices(&st, 3000);
		dev = find_cd(&st);
		if (dev >= 0)
			break;
		title("NENHUMA LEITORA DE CD ENCONTRADA");
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
	if (!read_toc())
		fail("NAO CONSEGUI LER O SUMARIO DO DISCO");
	for (i = t_first, n = 0; i <= t_last; i++)
		if (!(t_ctrl[i] & 4))
			n++;
	L("[DISCO] faixas %d a %d (%d de musica), fim %d", t_first, t_last, n, t_lba[100]);
	if (!(t_ctrl[t_first] & 4))
		fail("ESTE CD NAO TEM DADOS: NAO E JOGO DE PS1");
	r = game_id(gname, sizeof(gname), vol);
	L("[DISCO] codigo %s (%d), volume \"%s\"", gname, r, vol);
	if (r == -2)
		fail("ESTE CD NAO E DE PLAYSTATION");
	if (r < 0)
		strcpy(gname, "CD-USB");
	snprintf(gdir, sizeof(gdir), "%s/games/%s", edir, gname);
	snprintf(vpath, sizeof(vpath), "mass:%s/games", edir);
	fileXioMkdir(vpath, 0777);
	snprintf(vpath, sizeof(vpath), "mass:%s", gdir);
	fileXioMkdir(vpath, 0777);

	/* vcd.irx */
	n = 0;
	n += sprintf(args + n, "root=mass:") + 1;
	n += sprintf(args + n, "dir=%s", gdir) + 1;
	n += sprintf(args + n, "name=%s", gname) + 1;
	n += sprintf(args + n, CACHE_BLOCKS) + 1;
	log_save();
	if (load_buf("vcd", irx_vcd, size_irx_vcd, n, args) != 0)
		fail("FALHA AO CARREGAR O VCD.IRX");
	snprintf(cue, sizeof(cue), "vcd:%s/%s.cue", gdir, gname);
	fd = fileXioOpen(cue, FIO_O_RDONLY, 0);
	if (fd < 0) {
		L("[VCD] abrir %s: %d", cue, fd);
		fail("O VCD NAO PREPAROU O DISCO (VER VCD.TXT NA PASTA DO JOGO)");
	}
	{
		char c[1024];
		int k = fileXioRead(fd, c, sizeof(c) - 1);
		fileXioClose(fd);
		if (k < 0)
			k = 0;
		c[k] = 0;
		L("[VCD] %s:\r\n%s", cue, c);
	}

	title("PRONTO");
	S("   LEITORA: %.8s %.16s", st.dev[dev].inquiry + 8, st.dev[dev].inquiry + 16);
	S("   JOGO:    %s  (%s)", gname, vol[0] ? vol : "?");
	S("   FAIXAS:  %d (FIM %d MIN)", t_last - t_first + 1, t_lba[100] / 4500);
	S("   EMBER:   %s", ep);
	S("   SAVES:   mass:%s", gdir);
	S(" ");
	S("   X = JOGAR      O = SAIR");
	log_save();
	if (!ask())
		Exit(0);

	snprintf(vpath, sizeof(vpath), "vcd:%s", ep + 5);
	snprintf(arg1, sizeof(arg1), "%s/%s.cue", gname, gname);
	eargv[0] = arg1;
	L("[EMBER] abrindo %s \"%s\" (sem reiniciar o IOP)", vpath, arg1);
	log_save();
	padPortClose(0, 0);
	padEnd();
	if (!exists(vpath))
		fail("O VCD NAO MOSTRA O EMBER (VER VCD.TXT)");
	r = exec_noreset(vpath, 1, eargv);
	/* so volta se falhou */
	pad_ok = 0;
	L("[EMBER] falhou ao abrir: %d", r);
	log_save();
	title("FALHA AO ABRIR O EMBER");
	S("   %s: %d", vpath, r);
	for (;;)
		ee_wait_ms(1000);
	(void)argc;
	(void)argv;
	return 0;
}
