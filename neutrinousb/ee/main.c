/*
 * PS2USB (NEUTRINOUSB) - jogos de PS2 e PS1 pela leitora USB externa.
 * Criado por Leonardo Torres.
 *
 * 1. sobe usbd -> cdusb (+ FINALIZE: o pendrive fica com o usbmass) ->
 *    bdm/usbmass (pendrive) e o XMCMAN (cartao de memoria);
 * 2. confere o disco (ISO9660 + SYSTEM.CNF): BOOT2 = PS2, BOOT = PS1;
 * 3. PS2: acha o Neutrino (cartao de memoria primeiro, depois o pendrive),
 *    instala nele o usbcd_bd.irx (modules/) e o bsd-usbcd.toml (config/)
 *    (pasta "plana" tambem: SAS) e abre SEM reiniciar o IOP com:
 *      -bsd=usbcd -bsdfs=bd -dvd=bdfs:usbcd0p0
 * 4. PS1 (n09): acha o Ember (mesma ordem), sobe o vcd.irx (o CD vira
 *    <jogo>.cue/.bin em <Ember>/games/<jogo>) e abre vcd:/<Ember>/ember.elf
 *    SEM reiniciar o IOP.
 * Log: mass:/PS2USB.TXT (so com make LOG=1)
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

#define VERSAO		"n09"
/* log so na compilacao de diagnostico (make LOG=1); a versao final nao grava log */
#define LOG_FILE	"mass:/PS2USB.TXT"

#define IRX(n)	extern const unsigned char n[]; extern unsigned int size_##n
IRX(irx_iomanx);
IRX(irx_filexio);
IRX(irx_usbd);
IRX(irx_cdusb);
IRX(irx_bdm);
IRX(irx_bdmfs_fatfs);
IRX(irx_usbmass_bd);
IRX(irx_usbcd_bd);
IRX(irx_vcd);
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

/* ------------------------------------------------------------------ selo */
/*
 * Selo de integridade: CRC32 do codigo (.text) e das constantes (.rodata:
 * mensagens, drivers embutidos, carregador). O valor e gravado no arquivo
 * depois de compilar (tools/selo.py procura a marca em rv_seal). Se qualquer
 * byte mudar, o programa nao roda.
 */
extern char _ftext[], _etext[], _frodata[], _erodata[];
volatile u32 rv_seal[4] = {0x544C5652, 0x4C41534C, 0, 0};	/* "RVLT" "LSAL" crc tamanho */

static u32 crc32_upd(u32 c, const u8 *p, u32 n)
{
	static u32 tab[256];
	u32 i, k;

	if (!tab[1])
		for (i = 0; i < 256; i++) {
			u32 v = i;
			for (k = 0; k < 8; k++)
				v = (v >> 1) ^ (0xEDB88320u & (0u - (v & 1)));
			tab[i] = v;
		}
	while (n--)
		c = tab[(c ^ *p++) & 0xff] ^ (c >> 8);
	return c;
}

static int seal_ok(void)
{
	u32 c = 0xFFFFFFFFu;
	u32 nt = (u32)(_etext - _ftext), nr = (u32)(_erodata - _frodata);

	c = crc32_upd(c, (const u8 *)_ftext, nt);
	c = crc32_upd(c, (const u8 *)_frodata, nr);
	return rv_seal[3] == nt + nr && rv_seal[2] == (c ^ 0xFFFFFFFFu);
}

/* ------------------------------------------------------------------ log e tela */
static int log_ok;	/* pendrive acessivel */
#ifdef COM_LOG
static char logbuf[64 * 1024];
static int loglen;

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

#else
#define log_add(s)	((void)0)
#define L(...)		((void)0)
#endif

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

#ifdef COM_LOG
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
#else
#define log_save()	((void)0)
#endif

static void title(const char *sub)
{
	scr_clear();
	scr_setXY(0, 1);
	scr_setfontcolor(0x00d0ff);
	scr_printf("   RETROVOLT. LEITOR DE CDVD EXTERNO - LEONARDO TORRES 25/10/2026\n\n");
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
static int load_buf(const char *name, const void *p, unsigned int size, int arglen, const char *args)
{
	int ret, mr = -1;
	ret = SifExecModuleBuffer((void *)p, size, arglen, args, &mr);
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
				S("   SEM DISCO: COLOQUE O DISCO NA LEITORA  (O = SAIR)");
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
/* n08: CD ou DVD pelo perfil atual da leitora (GET CONFIGURATION) */
static const char *disc_media(void)
{
	static u8 buf[8] __attribute__((aligned(64)));
	u8 c[12] = {0x46, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0};
	int prof;

	if (cmd(c, 1, buf, 8, 3000) != CDUSB_OK)
		return NULL;
	prof = buf[6] << 8 | buf[7];
	L("[DISCO] perfil 0x%04x", prof);
	if (prof >= 0x08 && prof < 0x10)
		return "cd";
	if (prof >= 0x10 && prof < 0x40)
		return "dvd";
	return NULL;
}

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

/* grava o arquivo (only_if_missing: so se faltar); devolve 0 = ok */
static int install(const char *path, const void *data, int size, int only_if_missing)
{
	int fd, cur = file_size(path);

	/* n03: o driver e sempre regravado (versoes diferentes podem ter o mesmo tamanho) */
	if (only_if_missing && cur >= 0) {
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

/* bsd-usbcd.toml; 'args' vai para o usbcd_bd.irx (ex.: "\"fila=1\"") */
static int write_toml(const char *path, const char *args, const char *media)
{
	static char t[1200];
	int n, fd;

	n = snprintf(t, sizeof(t),
		"# Name of loaded config, to show to user\n"
		"name = \"USB CD/DVD drive (usbcd_bd)\"\n"
		"\n"
		"# Leitora de CD/DVD USB externa como dispositivo de bloco (NEUTRINOUSB).\n"
		"# Uso: neutrino.elf -bsd=usbcd -bsdfs=bd -dvd=bdfs:usbcd0p0\n"
		"# Opcoes do usbcd_bd.irx: fila=0 (sem fila completa no USB), max=<setores>\n"
		"# (por comando, padrao 64), cache=<KB> (leitura adiantada), speed=<KB/s>, keep=<ms>\n"
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
		"%s%s%s"
		"env = [\"LE\", \"EE\"]\n"
		"\n"
		"# Modules of the game that are faked/blocked\n"
		"[[fake]]\n"
		"file = \"USBD.IRX\"\n"
		"name = \"USB_driver\"\n"
		"version = 0x0204\n"
		"startrv = 2 # 0=RESIDENT_END, 1=NO_RESIDENT_END, 2=REMOVABLE_END\n"
		"%s%s%s",
		args[0] ? "args = [" : "", args, args[0] ? "]\n" : "",
		media ? "\n# n08: tipo do disco da leitora (sem isso o Neutrino chuta pelo tamanho)\n[cdvdman]\nmedia_type = \"" : "",
		media ? media : "", media ? "\"\n" : "");
	fd = fileXioOpen(path, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC, 0666);
	if (fd < 0)
		return -1;
	if (fileXioWrite(fd, t, n) != n) {
		fileXioClose(fd);
		return -1;
	}
	fileXioClose(fd);
	L("[INSTALAR] %s gravado (opcoes: %s)", path, args[0] ? args : "nenhuma");
	return 0;
}

/* ------------------------------------------------------------------ PS1 (n09) */
static int t_first, t_last, t_lba[101], t_ctrl[101];

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

static int upper(int c)
{
	return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

/* "BOOT = cdrom:\SLUS_006.06;1" -> "SLUS-00606" (nome da pasta de saves) */
static int ps1_id(char *out, int max, char *vol)
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
			if (d[off + 32] >= 10 && memcmp(d + off + 33, "SYSTEM.CNF", 10) == 0) {
				f_lba = le32(d + off + 2);
				break;
			}
			off += d[off];
		}
	}
	if (!f_lba)
		return -4;
	u = read_user(f_lba);
	if (!u)
		return -5;
	memcpy(txt, u, 2048);
	txt[2048] = 0;
	L("[PS1] SYSTEM.CNF: %.120s", txt);
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

/* ------------------------------------------------------------------ carregadores */
/* n09: procura no cartao de memoria primeiro; o pendrive so depois */
static const char *const app_devs[] = {"mc0:", "mc1:", "mass:", NULL};
static const char *const neutrino_files[] = {
	"/neutrino/neutrino.elf", "/APPS/neutrino/neutrino.elf", "/APP/neutrino/neutrino.elf",
	"/APPS/NEUTRINO/neutrino.elf", "/NEUTRINO/neutrino.elf", "/neutrino.elf", NULL,
};
static const char *const ember_files[] = {
	"/Ember/ember.elf", "/Ember/EMBER.ELF", "/APPS/Ember/ember.elf", "/APP/Ember/ember.elf",
	"/EMBER/ember.elf", "/EMBER/EMBER.ELF", "/APPS/EMBER/ember.elf", "/ember.elf", NULL,
};
static int mass_waited;

static int mass_ready(void)
{
	int fd = fileXioDopen("mass:/");
	if (fd < 0)
		return 0;
	fileXioDclose(fd);
	log_ok = 1;
	return 1;
}

static int find_app(const char *const *files, char *out, int max)
{
	int d, i;

	for (d = 0; app_devs[d]; d++) {
		if (!strcmp(app_devs[d], "mass:") && !mass_waited) {
			for (i = 0; i < 20 && !mass_ready(); i++)	/* pendrive: ate 5 s */
				ee_wait_ms(250);
			mass_waited = 1;
		}
		for (i = 0; files[i]; i++) {
			snprintf(out, max, "%s%s", app_devs[d], files[i]);
			if (exists(out))
				return 1;
		}
	}
	return 0;
}

/* acha o carregador ou deixa colocar e procurar de novo */
static void locate(const char *const *files, char *out, int max)
{
	while (!find_app(files, out, max)) {
		L("[APP] %s nao encontrado", files[0]);
		title("ARQUIVOS DO CARREGADOR NAO ENCONTRADOS");
		S("   COLOQUE OS ARQUIVOS NO CARTAO DE MEMORIA OU NO PENDRIVE.");
		S("   X = PROCURAR DE NOVO      O = SAIR");
		if (!ask())
			Exit(0);
		title("PROCURANDO...");
		mass_waited = 0;
	}
}

static void usb_warning(void)
{
	S(" ");
	scr_setfontcolor(0x00d0ff);
	S("   AO JOGAR PELA USB, PODE HAVER MUITOS ENGASGOS NAS CUTSCENES.");
	S("   ISSO E UMA LIMITACAO DA VELOCIDADE DA USB E NAO TEM NADA A VER");
	S("   COM O DESEMPENHO DO SOFTWARE.");
	scr_setfontcolor(0xffffff);
	S(" ");
	S("   X = JOGAR");
	S("   O = SAIR");
	log_save();
	if (!ask())
		Exit(0);
}

/* PS1: Ember + vcd.irx (o CD da leitora vira <jogo>.cue/.bin na pasta do jogo) */
static void run_ps1(const char *inq)
{
	static char ep[200], devn[8], edir[160], gdir[200], gname[48], vol[33], vpath[220], args[400], arg1[120];
	char *eargv[1];
	int r, n, dl;

	if (!read_toc())
		fail("NAO CONSEGUI LER O SUMARIO DO DISCO");
	L("[PS1] faixas %d a %d, fim %d", t_first, t_last, t_lba[100]);
	if (!(t_ctrl[t_first] & 4))
		fail("ESTE CD NAO TEM DADOS: NAO E JOGO DE PLAYSTATION");
	r = ps1_id(gname, sizeof(gname), vol);
	L("[PS1] codigo %s (%d), volume \"%s\"", gname, r, vol);
	if (r == -2)
		fail("ESTE CD NAO E DE PLAYSTATION");
	if (r < 0)
		strcpy(gname, "CD-USB");

	locate(ember_files, ep, sizeof(ep));
	dl = strchr(ep, ':') - ep + 1;			/* "mc0:" / "mass:" */
	memcpy(devn, ep, dl);
	devn[dl] = 0;
	strcpy(edir, ep + dl);
	*strrchr(edir, '/') = 0;			/* pasta do Ember ("" = raiz) */
	L("[PS1] carregador %s", ep);
	snprintf(vpath, sizeof(vpath), "%s%s/bios.bin", devn, edir);
	if (!exists(vpath))
		S("   AVISO: BIOS DO PS1 NAO ENCONTRADA NA PASTA DO CARREGADOR");
	snprintf(gdir, sizeof(gdir), "%s/games/%s", edir, gname);
	snprintf(vpath, sizeof(vpath), "%s%s/games", devn, edir);
	fileXioMkdir(vpath, 0777);
	snprintf(vpath, sizeof(vpath), "%s%s", devn, gdir);
	fileXioMkdir(vpath, 0777);

	n = 0;
	n += sprintf(args + n, "root=%s", devn) + 1;
	n += sprintf(args + n, "dir=%s", gdir) + 1;
	n += sprintf(args + n, "name=%s", gname) + 1;
	n += sprintf(args + n, "blocks=16") + 1;	/* maximo; deixa 640 KB livres no IOP */
#ifndef COM_LOG
	n += sprintf(args + n, "log=0") + 1;
#endif
	if (load_buf("vcd", irx_vcd, size_irx_vcd, n, args) != 0)
		fail("FALHA AO PREPARAR O DISCO DE PS1");
	snprintf(vpath, sizeof(vpath), "vcd:%s/%s.cue", gdir, gname);
	if (!exists(vpath))
		fail("FALHA AO PREPARAR O DISCO DE PS1");

	title("PRONTO");
	S("   LEITORA:  %.8s %.16s", inq + 8, inq + 16);
	S("   JOGO:     %s  (%s)", gname, vol[0] ? vol : "?");
	S("   TIPO:     PS1 - CD (%d FAIXAS)", t_last - t_first + 1);
	usb_warning();

	snprintf(vpath, sizeof(vpath), "vcd:%s", ep + dl);
	snprintf(arg1, sizeof(arg1), "%s/%s.cue", gname, gname);
	eargv[0] = arg1;
	L("[PS1] abrindo %s \"%s\" (sem reiniciar o IOP)", vpath, arg1);
	log_save();
	padPortClose(0, 0);
	padEnd();
	r = exec_noreset(vpath, 1, eargv);
	pad_ok = 0;
	L("[PS1] falhou ao abrir: %d", r);
	log_save();
	title("FALHA AO INICIAR O JOGO");
	S("   CODIGO %d", r);
	for (;;)
		ee_wait_ms(1000);
}

int main(int argc, char *argv[])
{
	cdusb_status st;
	static char ndir[200], ndir2[200], path[220], cfgdir[200], moddir[200], code[32], vol[33], cwd[200], tomlpath[220];
	const char *np, *media;
	char *nargv[4];
	int r, flat;

	if (!seal_ok()) {
		init_scr();
		scr_setfontcolor(0x0000ff);
		scr_printf("\n\n   ARQUIVO ALTERADO OU CORROMPIDO.\n");
		scr_printf("   ESTE PROGRAMA NAO PODE SER MODIFICADO.\n");
		for (;;)
			SleepThread();
	}
	iop_reset();
	init_scr();
	title("PREPARANDO...");
	L("NEUTRINOUSB " VERSAO " (" __DATE__ " " __TIME__ ")");

	load_rom("rom0:XSIO2MAN");
	load_rom("rom0:XPADMAN");
	load_buf("iomanX", irx_iomanx, size_irx_iomanx, 0, NULL);
	load_buf("fileXio", irx_filexio, size_irx_filexio, 0, NULL);
	load_rom("rom0:XMCMAN");	/* cartao de memoria (mc0:/mc1:) via iomanX */
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
	padInit(0);
	pad_ok = padPortOpen(0, 0, pad_area) != 0;
	if (r != 0)
		fail("FALHA AO CARREGAR O DRIVER DA LEITORA (CDUSB)");
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
	if (r == 1 || (r < 0 && r != -2 && ps1_id(path, 48, vol) == 0))
		run_ps1((const char *)st.dev[dev].inquiry);	/* PS1: nao volta */
	if (r < 0)
		fail("ESTE DISCO NAO E DE PLAYSTATION");
	media = disc_media();

	/* PS2: Neutrino */
	locate(neutrino_files, ndir, sizeof(ndir));
	np = ndir;
	strcpy(path, np);
	*strrchr(path, '/') = 0;
	strcpy(ndir2, path);				/* "mc0:/neutrino" */
	snprintf(path, sizeof(path), "%s/config/system.toml", ndir2);
	flat = !exists(path);
	snprintf(cfgdir, sizeof(cfgdir), flat ? "%s" : "%s/config", ndir2);
	snprintf(moddir, sizeof(moddir), flat ? "%s" : "%s/modules", ndir2);
	L("[NEUTRINO] %s (%s)", np, flat ? "pasta plana" : "config/ e modules/");
	snprintf(path, sizeof(path), "%s/usbcd_bd.irx", moddir);
	if (install(path, irx_usbcd_bd, size_irx_usbcd_bd, 0) < 0)
		fail("NAO CONSEGUI GRAVAR O DRIVER DA LEITORA");
	snprintf(path, sizeof(path), "%s/usbd_mini.irx", moddir);
	if (!exists(path)) {
		L("[NEUTRINO] falta %s (vem com o Neutrino)", path);
		fail("ARQUIVOS DO CARREGADOR INCOMPLETOS");
	}
	snprintf(tomlpath, sizeof(tomlpath), "%s/bsd-usbcd.toml", cfgdir);
	if (write_toml(tomlpath, "", media) < 0)
		fail("NAO CONSEGUI GRAVAR A CONFIGURACAO");

	title("PRONTO");
	S("   LEITORA:  %.8s %.16s", st.dev[dev].inquiry + 8, st.dev[dev].inquiry + 16);
	S("   JOGO:     %s  (%s)", code[0] ? code : "?", vol[0] ? vol : "?");
	S("   TIPO:     PS2 - %s", media ? (media[0] == 'c' ? "CD" : "DVD") : "?");
	usb_warning();

	snprintf(cwd, sizeof(cwd), "-cwd=%s", ndir2);
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
	title("FALHA AO INICIAR O JOGO");
	S("   CODIGO %d", r);
	for (;;)
		ee_wait_ms(1000);
	(void)argc;
	(void)argv;
	return 0;
}
