/*
 * usbcd_bd.irx - leitora de CD/DVD USB como dispositivo de bloco do BDM,
 * para o Neutrino ler o disco de PS2 inteiro como uma ISO:
 *   neutrino.elf -bsd=usbcd -bsdfs=bd -dvd=bdfs:usbcd0p0
 * Criado por Leonardo Torres.
 *
 * Roda nos dois ambientes do Neutrino: carga (bdm.irx de verdade) e jogo
 * (fhi_bd.irx, que tambem exporta bdm_connect_bd). So precisa do usbd.
 * O transporte USB (bulk-only, CBW/CSW, prazos, reset) e o do cdusb.irx.
 *
 * Licoes dos testes CDUSB c07-c09 (ASUS SDRW-08D1S-U):
 *  - setores de 2048 (READ(10)); o BDM usa setores de 512: 1 do disco = 4;
 *  - quanto maior o comando, melhor (19 setores = 729 KB/s, o teto do USB
 *    do PS2); aqui ate 32 (64 KB) quando o pedido e alinhado;
 *  - velocidade travada: CD pede 4x (a ASUS fica em 10x); DVD pede 1x (ela
 *    fica em ~2,4x). Livre, o DVD girou a 8x e travou 4 s; travado, nenhuma
 *    travada e a mesma velocidade (o limite e o USB: ~730 KB/s);
 *  - leitora que reinicia: espera ficar pronta, trava de novo e repete;
 *  - motor: le 1 setor quando fica parada (2 s), para nao desacelerar.
 *
 * n02 (cutscenes):
 *  - fila completa no USB: comando + todos os pedacos de 4 KB + resposta sao
 *    pedidos de uma vez (como o usbmass_bd da ps2sdk), sem o USB parar entre
 *    os pedacos. Se a leitora estranhar, volta sozinho para o jeito antigo;
 *  - leitura adiantada: depois de cada leitura em sequencia, uma thread ja
 *    busca os proximos setores enquanto o jogo processa (o USB nao fica
 *    parado entre os pedidos do jogo).
 *
 * Argumentos: speed=<KB/s> (0 = nao travar; padrao: CD 706, DVD 1385)
 *             keep=<ms> (0 = sem manter girando; padrao 2000)
 *             cache=<KB> (leitura adiantada; padrao 0 = desligada)
 *             fila=0 (desliga a fila completa; padrao ligada desde a n04)
 *             max=<setores> (por comando; padrao 64 desde a n05)
 * n03: na n02 as duas tecnicas ligadas pioraram muito as cutscenes; cada uma
 *      virou opcao. Teste: leitura adiantada = horrivel; fila completa = a
 *      melhor (cutscene lisa por mais tempo antes do limite do USB).
 * n04: fila completa por padrao; max=64 para testar comandos maiores.
 * n05: 64 setores por comando por padrao (no teste: levemente melhor).
 * n08: reaplica a trava de rotacao apos reset da leitora, mais tentativas de
 *      leitura (a ultima em partes de 16 setores) e zeros alem do fim do disco.
 */
#include "irx_imports.h"

#define MODNAME "usbcd_bd"
IRX_ID(MODNAME, 1, 6);

#define SUB_ATAPI	0x02
#define SUB_SFF8070	0x05
#define SUB_SCSI	0x06
#define PROTO_BULK	0x50
#define USB_CHUNK	4096
#define SECT		2048
#define MAX_PER_CMD	64		/* teto de max= */
#define TMO_READ	8000

enum { R_OK = 0, R_CHECK = 1, R_PHASE = 2, R_USB = -1, R_TIMEOUT = -100 };

/* ------------------------------------------------------------------ estado */
static struct {
	int devId;			/* -1 = nenhum */
	int ctrl, epIn, epOut;
	UsbEndpointDescriptor *dIn, *dOut;
	u8 config, ifnum, ifalt, sub;
	volatile int state;		/* 0 livre, 1 conectado, 2 pronto (no BDM), 3 ignorado */
} d = {-1};

static struct block_device bd;
static int io_lock, work_sema, xfer_sema;
static volatile u32 xfer_gen = 1;
static volatile int xfer_rc, xfer_tmo;
static u32 tag;
static int lock_kbs = -1, keep_ms = 2000, is_dvd;
static u32 disc_sectors, last_lba;
static volatile u32 last_io;
static volatile int speed_dirty;		/* n08: o reset da leitora pode soltar a trava de rotacao */

static u8 cbw[32] __attribute__((aligned(64)));
static u8 csw[16] __attribute__((aligned(64)));
static u8 small[64] __attribute__((aligned(64)));
static u8 bounce[SECT] __attribute__((aligned(64)));

/* fila completa (n02) */
static int use_queue = 1;			/* n04: ligada por padrao (fila=0 desliga) */
static int max_per_cmd = 64;			/* n05: 64 setores por comando (max= muda) */
static volatile int q_total, q_done, q_err;

/* leitura adiantada (n02) */
#define PF_CHUNK	16			/* setores por comando da leitura adiantada */
static int cache_kb;				/* n03: desligado (cache=KB liga) */
static u8 *pf_buf;
static u32 pf_cap;				/* setores que cabem */
static volatile u32 pf_lba, pf_have, pf_gen;	/* janela: [pf_lba, pf_lba + pf_have) valida */
static volatile int fg_wait;			/* o jogo esta esperando o USB */
static int pf_lock, pf_sema;

static sceUsbdLddOps ldd;

static u32 now_ms(void)
{
	iop_sys_clock_t c;
	u32 s, us;
	GetSystemTime(&c);
	SysClock2USec(&c, &s, &us);
	return s * 1000 + us / 1000;
}

/* ------------------------------------------------------------------ transferencias */
static struct {
	u8 *buf;
	u32 left, cur, done;
	int pipe;
} bx;

static void bulk_cb(int rc, int bytes, void *arg)
{
	if ((u32)arg != xfer_gen)
		return;
	if (rc == USB_RC_OK) {
		bx.done += bytes;
		bx.left -= bytes;
		bx.buf += bytes;
		if (bx.left > 0 && (u32)bytes == bx.cur) {
			bx.cur = bx.left > USB_CHUNK ? USB_CHUNK : bx.left;
			rc = sceUsbdBulkTransfer(bx.pipe, bx.buf, bx.cur, bulk_cb, arg);
			if (rc == USB_RC_OK)
				return;
		}
	}
	xfer_rc = rc;
	SignalSema(xfer_sema);
}

static void ctrl_cb(int rc, int bytes, void *arg)
{
	(void)bytes;
	if ((u32)arg != xfer_gen)
		return;
	xfer_rc = rc;
	SignalSema(xfer_sema);
}

static unsigned int alarm_cb(void *arg)
{
	if ((u32)arg == xfer_gen) {
		xfer_tmo = 1;
		iSignalSema(xfer_sema);
	}
	return 0;
}

static int xfer_wait(int ms)
{
	iop_sys_clock_t clk;
	void *g = (void *)xfer_gen;
	int tmo, rc;

	USec2SysClock((u32)ms * 1000, &clk);
	SetAlarm(&clk, alarm_cb, g);
	WaitSema(xfer_sema);
	tmo = xfer_tmo;
	rc = xfer_rc;
	xfer_gen++;
	CancelAlarm(alarm_cb, g);
	while (PollSema(xfer_sema) == 0)
		;
	xfer_tmo = 0;
	return tmo ? R_TIMEOUT : rc;
}

static int bulk(int in, void *buf, u32 len, int ms, u32 *got)
{
	int rc;

	bx.buf = buf;
	bx.left = len;
	bx.cur = len > USB_CHUNK ? USB_CHUNK : len;
	bx.done = 0;
	bx.pipe = in ? d.epIn : d.epOut;
	xfer_rc = -1;
	rc = sceUsbdBulkTransfer(bx.pipe, bx.buf, bx.cur, bulk_cb, (void *)xfer_gen);
	if (rc == USB_RC_OK)
		rc = xfer_wait(ms);
	if (got)
		*got = bx.done;
	return rc;
}

static int ctrl(int rt, int req, int val, int idx, int len, void *data)
{
	int rc;

	xfer_rc = -1;
	rc = sceUsbdControlTransfer(d.ctrl, rt, req, val, idx, len, data, ctrl_cb, (void *)xfer_gen);
	if (rc != USB_RC_OK)
		return rc;
	return xfer_wait(3000);
}

static void reopen(int in)
{
	if (in) {
		if (d.epIn >= 0)
			sceUsbdClosePipe(d.epIn);
		d.epIn = sceUsbdOpenPipe(d.devId, d.dIn);
	} else {
		if (d.epOut >= 0)
			sceUsbdClosePipe(d.epOut);
		d.epOut = sceUsbdOpenPipeAligned(d.devId, d.dOut);
	}
}

static void clear_halt(int in)
{
	UsbEndpointDescriptor *e = in ? d.dIn : d.dOut;
	ctrl(USB_DIR_OUT | USB_RECIP_ENDPOINT, USB_REQ_CLEAR_FEATURE, 0, e->bEndpointAddress, 0, NULL);
	reopen(in);
}

static void reset_recovery(void)
{
	reopen(1);
	reopen(0);
	ctrl(USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, 0xFF, 0, d.ifnum, 0, NULL);
	clear_halt(1);
	clear_halt(0);
	speed_dirty = 1;
}

static int read_csw(u32 t)
{
	u32 got = 0;
	int rc = bulk(1, csw, 13, 5000, &got);

	if (rc == USB_RC_STALL) {
		clear_halt(1);
		rc = bulk(1, csw, 13, 5000, &got);
	}
	if (rc != USB_RC_OK || got != 13)
		return rc == USB_RC_OK ? R_PHASE : rc;
	if ((csw[0] | csw[1] << 8 | csw[2] << 16 | (u32)csw[3] << 24) != 0x53425355 ||
	    (csw[4] | csw[5] << 8 | csw[6] << 16 | (u32)csw[7] << 24) != t)
		return R_PHASE;
	return R_OK;
}

/* comando SCSI completo (com a trava de E/S) */
static int scsi(const u8 *cdb, int dir, void *data, u32 len, int ms, int *sense)
{
	u32 t;
	int rc, r, status, cl;

	if (sense)
		*sense = 0;
	if (d.devId < 0)
		return R_USB;
	cl = d.sub == SUB_ATAPI ? 12 : cdb[0] < 0x20 ? 6 : cdb[0] < 0x60 ? 10 : 12;
	t = ++tag;
	memset(cbw, 0, 31);
	cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43;
	cbw[4] = t; cbw[5] = t >> 8; cbw[6] = t >> 16; cbw[7] = t >> 24;
	cbw[8] = len; cbw[9] = len >> 8; cbw[10] = len >> 16; cbw[11] = len >> 24;
	cbw[12] = dir == 1 ? 0x80 : 0x00;
	cbw[14] = cl;
	memcpy(cbw + 15, cdb, cl);

	rc = bulk(0, cbw, 31, 3000, NULL);
	if (rc != USB_RC_OK) {
		reset_recovery();
		r = rc == R_TIMEOUT ? R_TIMEOUT : R_USB;
		goto out;
	}
	if (len && dir) {
		rc = bulk(dir == 1, data, len, ms, NULL);
		if (rc == R_TIMEOUT) {
			reset_recovery();
			r = R_TIMEOUT;
			goto out;
		}
		if (rc == USB_RC_STALL)
			clear_halt(dir == 1);
		else if (rc != USB_RC_OK) {
			reset_recovery();
			r = R_USB;
			goto out;
		}
	}
	r = read_csw(t);
	if (r != R_OK) {
		reset_recovery();
		r = r == R_TIMEOUT ? R_TIMEOUT : R_PHASE;
		goto out;
	}
	status = csw[12];
	if (status == 2) {
		reset_recovery();
		r = R_PHASE;
	} else if (status == 1) {
		r = R_CHECK;
		if (sense) {
			u8 rs[12] = {0x03, 0, 0, 0, 18, 0, 0, 0, 0, 0, 0, 0};
			memset(small, 0, 18);
			if (scsi(rs, 1, small, 18, 3000, NULL) == R_OK)
				*sense = (small[2] & 0x0f) << 16 | small[12] << 8 | small[13];
		}
	} else
		r = R_OK;
out:
	last_io = now_ms();
	return r;
}

/* ------------------------------------------------------------------ leitora */
static int tur_wait(int ms)
{
	u8 c[12] = {0};
	u32 t0 = now_ms();
	int r, sense;

	while (now_ms() - t0 < (u32)ms && d.devId >= 0) {
		r = scsi(c, 0, NULL, 0, 3000, &sense);
		if (r == R_OK)
			return 1;
		if (r == R_CHECK && ((sense >> 16) & 0x0f) == 6)
			continue;		/* aviso de troca/reinicio */
		DelayThread(300 * 1000);
	}
	return 0;
}

static void set_speed(void)
{
	u8 c[12], pd[28];
	u32 end = last_lba;

	if (lock_kbs <= 0)
		return;
	memset(c, 0, sizeof(c));
	c[0] = 0xBB;
	c[2] = lock_kbs >> 8; c[3] = lock_kbs;
	c[4] = 0xFF; c[5] = 0xFF;
	scsi(c, 0, NULL, 0, 3000, NULL);
	memset(pd, 0, sizeof(pd));
	pd[8] = end >> 24; pd[9] = end >> 16; pd[10] = end >> 8; pd[11] = end;
	pd[14] = lock_kbs >> 8; pd[15] = lock_kbs;
	pd[18] = 1000 >> 8; pd[19] = 1000 & 0xff;
	pd[22] = lock_kbs >> 8; pd[23] = lock_kbs;
	pd[26] = 1000 >> 8; pd[27] = 1000 & 0xff;
	memset(c, 0, sizeof(c));
	c[0] = 0xB6;
	c[10] = sizeof(pd);
	memcpy(small, pd, sizeof(pd));
	scsi(c, 2, small, sizeof(pd), 3000, NULL);
}

/* n02: callback da fila completa: conta os pedacos; o ultimo acorda */
static void q_cb(int rc, int bytes, void *arg)
{
	(void)bytes;
	if ((u32)arg != xfer_gen)
		return;
	if (rc != USB_RC_OK && !q_err)
		q_err = rc ? rc : -1;
	if (++q_done == q_total)
		SignalSema(xfer_sema);
}

/* n02: READ(10) com comando, todos os pedacos e a resposta na fila de uma vez */
static int read10_queued(u32 lba, int n, void *dst)
{
	u8 c[12] = {0x28, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	u32 len = n * SECT, t, off;
	void *g;
	int rc, k, cl = d.sub == SUB_ATAPI ? 12 : 10;

	if (d.devId < 0)
		return R_USB;
	c[2] = lba >> 24; c[3] = lba >> 16; c[4] = lba >> 8; c[5] = lba;
	c[7] = n >> 8; c[8] = n;
	t = ++tag;
	memset(cbw, 0, 31);
	cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43;
	cbw[4] = t; cbw[5] = t >> 8; cbw[6] = t >> 16; cbw[7] = t >> 24;
	cbw[8] = len; cbw[9] = len >> 8; cbw[10] = len >> 16; cbw[11] = len >> 24;
	cbw[12] = 0x80;
	cbw[14] = cl;
	memcpy(cbw + 15, c, cl);
	memset(csw, 0, 13);

	g = (void *)xfer_gen;
	q_done = 0;
	q_err = 0;
	q_total = 2 + (len + USB_CHUNK - 1) / USB_CHUNK;
	rc = sceUsbdBulkTransfer(d.epOut, cbw, 31, q_cb, g);
	for (off = 0, k = 1; rc == USB_RC_OK && off < len; off += USB_CHUNK, k++)
		rc = sceUsbdBulkTransfer(d.epIn, (u8 *)dst + off, len - off > USB_CHUNK ? USB_CHUNK : len - off, q_cb, g);
	if (rc == USB_RC_OK)
		rc = sceUsbdBulkTransfer(d.epIn, csw, 13, q_cb, g);
	if (rc != USB_RC_OK) {
		/* parte nao entrou na fila: espera o que entrou e desiste */
		DelayThread(50 * 1000);
		xfer_gen++;
		while (PollSema(xfer_sema) == 0)
			;
		reset_recovery();
		last_io = now_ms();
		return R_USB;
	}
	rc = xfer_wait(TMO_READ);		/* acorda no ultimo pedaco (ou no prazo) */
	last_io = now_ms();
	if (rc == R_TIMEOUT || q_err) {
		reset_recovery();
		return rc == R_TIMEOUT ? R_TIMEOUT : R_USB;
	}
	if ((csw[0] | csw[1] << 8 | csw[2] << 16 | (u32)csw[3] << 24) != 0x53425355 ||
	    (csw[4] | csw[5] << 8 | csw[6] << 16 | (u32)csw[7] << 24) != t || csw[12] != 0) {
		reset_recovery();
		return R_PHASE;
	}
	return R_OK;
}

static int read10(u32 lba, int n, void *dst)
{
	u8 c[12] = {0x28, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	int r;

	if (use_queue && ((u32)dst & 3) == 0) {
		r = read10_queued(lba, n, dst);
		if (r == R_OK)
			return r;
		/* a leitora nao gostou da fila: confere pelo jeito antigo */
		c[2] = lba >> 24; c[3] = lba >> 16; c[4] = lba >> 8; c[5] = lba;
		c[7] = n >> 8; c[8] = n;
		if (tur_wait(15000) && scsi(c, 1, dst, n * SECT, TMO_READ, NULL) == R_OK) {
			use_queue = 0;		/* o antigo funciona e a fila nao: fica no antigo */
			return R_OK;
		}
		return r;
	}
	c[2] = lba >> 24; c[3] = lba >> 16; c[4] = lba >> 8; c[5] = lba;
	c[7] = n >> 8; c[8] = n;
	return scsi(c, 1, dst, n * SECT, TMO_READ, NULL);
}

/* leitura com novas tentativas depois de esperar a leitora; n08: reaplica a
 * trava de rotacao apos qualquer reset e, na ultima tentativa, le em partes */
static int read_robust(u32 lba, int n, void *dst)
{
	int r, i, k, m;

	if (speed_dirty) {
		speed_dirty = 0;
		set_speed();
	}
	r = read10(lba, n, dst);
	for (i = 0; r != R_OK && i < 3; i++) {
		if (!tur_wait(15000))
			break;
		set_speed();
		speed_dirty = 0;
		if (i < 2 || n <= 16) {
			r = read10(lba, n, dst);
			continue;
		}
		for (k = 0; k < n; k += m) {
			m = n - k < 16 ? n - k : 16;
			r = read10(lba + k, m, (u8 *)dst + k * SECT);
			if (r != R_OK)
				break;
		}
	}
	return r == R_OK ? 0 : -1;
}

/* ------------------------------------------------------------------ BDM */
/* n02: leitura adiantada em anel. O setor 'lba' mora na posicao lba % pf_cap;
 * a janela valida e [pf_lba, pf_lba + pf_have). Quando o jogo avanca dentro
 * dela, ela so anda (nada e copiado nem jogado fora). */
static u8 *pf_at(u32 lba)
{
	return pf_buf + (lba % pf_cap) * SECT;
}

/* copia da janela o que tiver de [lba, lba + n); devolve setores */
static u32 pf_take(u32 lba, u32 n, u8 *dst)
{
	u32 k = 0, i;

	if (!pf_buf)
		return 0;
	WaitSema(pf_lock);
	if (lba >= pf_lba && lba < pf_lba + pf_have) {
		k = pf_lba + pf_have - lba;
		if (k > n)
			k = n;
		for (i = 0; i < k;) {			/* ate a volta do anel, depois o resto */
			u32 pos = (lba + i) % pf_cap, run = pf_cap - pos;
			if (run > k - i)
				run = k - i;
			memcpy(dst + i * SECT, pf_buf + pos * SECT, run * SECT);
			i += run;
		}
	}
	SignalSema(pf_lock);
	return k;
}

/* o jogo vai continuar em 'next': a janela anda ate la ou recomeca ali */
static void pf_restart(u32 next)
{
	if (!pf_buf)
		return;
	WaitSema(pf_lock);
	if (next >= pf_lba && next <= pf_lba + pf_have) {
		pf_have -= next - pf_lba;
		pf_lba = next;
	} else {
		pf_lba = next;
		pf_have = 0;
		pf_gen++;
	}
	SignalSema(pf_lock);
	SignalSema(pf_sema);
}

static int bd_read(struct block_device *b, u64 sector, void *buffer, u16 count)
{
	u32 lba = (u32)(sector >> 2), off = ((u32)sector & 3) * 512, left = (u32)count * 512;
	u8 *p = buffer;
	(void)b;

	if (d.state != 2)
		return -EIO;
	while (left) {
		if (lba >= disc_sectors) {		/* n08: alem do fim do disco = zeros */
			memset(p, 0, left);
			break;
		}
		if (off == 0 && left >= SECT) {
			u32 n = left / SECT, k;
			if (n > (u32)max_per_cmd)
				n = max_per_cmd;
			if (n > disc_sectors - lba)
				n = disc_sectors - lba;
			k = pf_take(lba, n, p);			/* ja veio adiantado? */
			if (!k) {
				fg_wait = 1;
				WaitSema(io_lock);
				fg_wait = 0;
				k = pf_take(lba, n, p);		/* pode ter chegado enquanto esperava */
				if (!k) {
					if (((u32)p & 3) == 0) {
						if (read_robust(lba, n, p) < 0)
							goto fail;
						k = n;
					} else {
						if (read_robust(lba, 1, bounce) < 0)
							goto fail;
						memcpy(p, bounce, SECT);
						k = 1;
					}
				}
				SignalSema(io_lock);
			}
			p += k * SECT;
			left -= k * SECT;
			lba += k;
		} else {
			u32 k = SECT - off;
			if (k > left)
				k = left;
			fg_wait = 1;
			WaitSema(io_lock);
			fg_wait = 0;
			if (read_robust(lba, 1, bounce) < 0)
				goto fail;
			SignalSema(io_lock);
			memcpy(p, bounce + off, k);
			p += k;
			left -= k;
			lba++;
			off = 0;
		}
	}
	pf_restart(lba);		/* proximos setores enquanto o jogo processa */
	return count;
fail:
	SignalSema(io_lock);
	return -EIO;
}

/* n02: thread da leitura adiantada */
static void pf_thread(void *arg)
{
	(void)arg;
	for (;;) {
		WaitSema(pf_sema);
		for (;;) {
			u32 gen, at, n, pos;
			if (d.state != 2 || fg_wait)
				break;
			WaitSema(pf_lock);
			gen = pf_gen;
			at = pf_lba + pf_have;
			n = pf_cap - pf_have;
			SignalSema(pf_lock);
			if (n == 0 || at >= disc_sectors)
				break;
			if (n > PF_CHUNK)
				n = PF_CHUNK;
			pos = at % pf_cap;
			if (n > pf_cap - pos)
				n = pf_cap - pos;	/* nao passa da volta do anel */
			if (at + n > disc_sectors)
				n = disc_sectors - at;
			if (PollSema(io_lock) != 0)
				break;			/* o jogo esta lendo: deixa para depois */
			if (read10(at, n, pf_at(at)) != R_OK) {
				SignalSema(io_lock);
				break;
			}
			SignalSema(io_lock);
			WaitSema(pf_lock);
			if (gen == pf_gen && at == pf_lba + pf_have)
				pf_have += n;
			SignalSema(pf_lock);
		}
	}
}

static int bd_write(struct block_device *b, u64 sector, const void *buffer, u16 count)
{
	(void)b; (void)sector; (void)buffer; (void)count;
	return -EIO;
}

static void bd_flush(struct block_device *b)
{
	(void)b;
}

static int bd_stop(struct block_device *b)
{
	(void)b;
	return 0;
}

/* ------------------------------------------------------------------ dispositivos */
static UsbInterfaceDescriptor *find_mass_if(int devId, UsbConfigDescriptor **cfg_out)
{
	UsbDeviceDescriptor *dev;
	UsbConfigDescriptor *cfg;
	u8 *p, *end;

	dev = sceUsbdScanStaticDescriptor(devId, NULL, USB_DT_DEVICE);
	if (!dev || dev->bNumConfigurations < 1)
		return NULL;
	cfg = sceUsbdScanStaticDescriptor(devId, dev, USB_DT_CONFIG);
	if (!cfg)
		return NULL;
	if (cfg_out)
		*cfg_out = cfg;
	p = (u8 *)cfg;
	end = p + cfg->wTotalLength;
	while (p + 2 <= end && p[0] >= 2) {
		if (p[1] == USB_DT_INTERFACE && p + sizeof(UsbInterfaceDescriptor) <= end) {
			UsbInterfaceDescriptor *in = (UsbInterfaceDescriptor *)p;
			/* leitoras de CD/DVD: ATAPI (02) ou SFF-8070i (05); pendrive (06) fica de fora */
			if (in->bInterfaceClass == USB_CLASS_MASS_STORAGE && in->bInterfaceProtocol == PROTO_BULK &&
			    in->bNumEndpoints >= 2 &&
			    (in->bInterfaceSubClass == SUB_ATAPI || in->bInterfaceSubClass == SUB_SFF8070))
				return in;
		}
		p += p[0];
	}
	return NULL;
}

static int cd_probe(int devId)
{
	return d.devId < 0 && d.state == 0 && find_mass_if(devId, NULL) != NULL;
}

static int cd_connect(int devId)
{
	UsbConfigDescriptor *cfg = NULL;
	UsbInterfaceDescriptor *in = find_mass_if(devId, &cfg);
	u8 *p, *end;

	if (!in || d.devId >= 0)
		return 1;
	d.ctrl = sceUsbdOpenPipe(devId, NULL);
	d.epIn = d.epOut = -1;
	d.dIn = d.dOut = NULL;
	d.config = cfg->bConfigurationValue;
	d.ifnum = in->bInterfaceNumber;
	d.ifalt = in->bAlternateSetting;
	d.sub = in->bInterfaceSubClass;
	p = (u8 *)in + in->bLength;
	end = (u8 *)cfg + cfg->wTotalLength;
	for (; p + 2 <= end && p[0] >= 2 && p[1] != USB_DT_INTERFACE; p += p[0]) {
		UsbEndpointDescriptor *e = (UsbEndpointDescriptor *)p;
		if (p[1] != USB_DT_ENDPOINT || (e->bmAttributes & 3) != USB_ENDPOINT_XFER_BULK)
			continue;
		if ((e->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN && !d.dIn)
			d.dIn = e;
		else if ((e->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_OUT && !d.dOut)
			d.dOut = e;
	}
	if (!d.dIn || !d.dOut) {
		if (d.ctrl >= 0)
			sceUsbdClosePipe(d.ctrl);
		return 1;
	}
	d.epIn = sceUsbdOpenPipe(devId, d.dIn);
	d.epOut = sceUsbdOpenPipeAligned(devId, d.dOut);
	d.devId = devId;
	d.state = 1;
	SignalSema(work_sema);
	return 0;
}

static int cd_disconnect(int devId)
{
	if (devId != d.devId)
		return 0;
	if (d.state == 2)
		bdm_disconnect_bd(&bd);
	d.epIn = d.epOut = d.ctrl = -1;
	d.devId = -1;
	d.state = 0;
	return 0;
}

/* configura, le o INQUIRY, espera o disco e entrega ao BDM */
static int setup_dev(void)
{
	static const u8 inq[12] = {0x12, 0, 0, 0, 36, 0, 0, 0, 0, 0, 0, 0};
	u8 c[12];
	int rc, i, sense;

	rc = ctrl(USB_DIR_OUT | USB_RECIP_DEVICE, USB_REQ_SET_CONFIGURATION, d.config, 0, 0, NULL);
	if (rc != USB_RC_OK)
		return 0;
	rc = ctrl(USB_DIR_OUT | USB_RECIP_INTERFACE, USB_REQ_SET_INTERFACE, d.ifalt, d.ifnum, 0, NULL);
	if (rc == USB_RC_STALL) {
		clear_halt(1);
		clear_halt(0);
	}
	for (i = 0; i < 4; i++) {
		memset(small, 0, 36);
		rc = scsi(inq, 1, small, 36, 5000, &sense);
		if (rc == R_OK)
			break;
		DelayThread(200 * 1000);
	}
	if (rc != R_OK || (small[0] & 0x1f) != 5)
		return 0;		/* nao e leitora de CD/DVD */
	if (!tur_wait(60000))
		return 0;
	/* tamanho: READ CAPACITY */
	memset(c, 0, sizeof(c));
	c[0] = 0x25;
	if (scsi(c, 1, small, 8, 5000, NULL) != R_OK)
		return 0;
	last_lba = (u32)small[0] << 24 | small[1] << 16 | small[2] << 8 | small[3];
	disc_sectors = last_lba + 1;
	/* CD ou DVD: GET CONFIGURATION (perfil atual) */
	memset(c, 0, sizeof(c));
	c[0] = 0x46;
	c[8] = 8;
	is_dvd = 0;
	if (scsi(c, 1, small, 8, 3000, NULL) == R_OK) {
		int prof = small[6] << 8 | small[7];
		is_dvd = prof >= 0x10 && prof < 0x40;
	}
	if (lock_kbs < 0)
		lock_kbs = is_dvd ? 1385 : 706;
	set_speed();
	return 1;
}

static void work_thread(void *arg)
{
	(void)arg;
	for (;;) {
		WaitSema(work_sema);
		if (d.state != 1)
			continue;
		WaitSema(io_lock);
		if (d.state == 1 && setup_dev()) {
			memset(&bd, 0, sizeof(bd));
			bd.name = "usbcd";
			bd.devNr = 0;
			bd.parNr = 0;
			bd.sectorSize = 512;
			bd.sectorOffset = 0;
			bd.sectorCount = (u64)disc_sectors * 4;
			bd.read = bd_read;
			bd.write = bd_write;
			bd.flush = bd_flush;
			bd.stop = bd_stop;
			d.state = 2;
			SignalSema(io_lock);
			bdm_connect_bd(&bd);
		} else {
			if (d.state == 1)
				d.state = 3;	/* nao serve: fica de fora */
			SignalSema(io_lock);
		}
	}
}

/* manter girando: parada ha keep_ms, le 1 setor */
static void keep_thread(void *arg)
{
	u32 lba = 16;
	(void)arg;
	for (;;) {
		DelayThread(500 * 1000);
		if (d.state != 2 || keep_ms <= 0 || now_ms() - last_io < (u32)keep_ms)
			continue;
		if (PollSema(io_lock) != 0)
			continue;		/* tem leitura acontecendo */
		read10(lba, 1, bounce);
		lba = lba == 16 ? 17 : 16;
		SignalSema(io_lock);
	}
}

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

static int new_thread(void (*fn)(void *), int prio)
{
	iop_thread_t t;
	int id;
	t.attr = TH_C;
	t.option = 0;
	t.thread = fn;
	t.stacksize = 0x800;
	t.priority = prio;
	id = CreateThread(&t);
	if (id > 0)
		StartThread(id, NULL);
	return id;
}

int _start(int argc, char *argv[])
{
	int i;

	for (i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "speed=", 6))
			lock_kbs = strtol(argv[i] + 6, NULL, 10);
		else if (!strncmp(argv[i], "keep=", 5))
			keep_ms = strtol(argv[i] + 5, NULL, 10);
		else if (!strncmp(argv[i], "cache=", 6))
			cache_kb = strtol(argv[i] + 6, NULL, 10);
		else if (!strncmp(argv[i], "fila=", 5))
			use_queue = strtol(argv[i] + 5, NULL, 10) != 0;
		else if (!strncmp(argv[i], "max=", 4)) {
			max_per_cmd = strtol(argv[i] + 4, NULL, 10);
			if (max_per_cmd < 1)
				max_per_cmd = 1;
			if (max_per_cmd > MAX_PER_CMD)
				max_per_cmd = MAX_PER_CMD;
		}
	}
	if (cache_kb > 0) {
		pf_cap = cache_kb * 1024 / SECT;
		if (pf_cap < PF_CHUNK)
			pf_cap = PF_CHUNK;
		pf_buf = AllocSysMemory(ALLOC_FIRST, pf_cap * SECT, NULL);	/* sem memoria: sem adiantar */
	}
	io_lock = new_sema(1);
	work_sema = new_sema(0);
	xfer_sema = new_sema(0);
	pf_lock = new_sema(1);
	pf_sema = new_sema(0);
	if (io_lock < 0 || work_sema < 0 || xfer_sema < 0 || pf_lock < 0 || pf_sema < 0)
		return MODULE_NO_RESIDENT_END;
	if (new_thread(work_thread, 0x12) <= 0 || new_thread(keep_thread, 0x60) <= 0 ||
	    (pf_buf && new_thread(pf_thread, 0x50) <= 0))
		return MODULE_NO_RESIDENT_END;
	memset(&ldd, 0, sizeof(ldd));
	ldd.name = "usbcd_bd";
	ldd.probe = cd_probe;
	ldd.connect = cd_connect;
	ldd.disconnect = cd_disconnect;
	if (sceUsbdRegisterLdd(&ldd) != USB_RC_OK)
		return MODULE_NO_RESIDENT_END;
	return MODULE_RESIDENT_END;
}
