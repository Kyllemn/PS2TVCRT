/*
 * cdusb.c - leitor de CD USB no IOP do PS2.
 * Criado por Leonardo Torres.
 *
 * - Fica com os leitores de CD/DVD USB (armazenamento USB, bulk-only).
 *   Antes do FINALIZE pega todos os dispositivos de armazenamento e le o
 *   INQUIRY de cada um; no FINALIZE solta tudo o que nao e leitor de CD
 *   (tipo 5), para o usbmass_bd (pendrive) carregado depois ficar com eles.
 * - So transporta comandos SCSI/MMC: a logica de CD fica no EE.
 * - Um comando por vez; toda espera tem prazo. Se o leitor nao responder,
 *   os canais sao reabertos e o leitor recebe reset do protocolo.
 * - Nunca muda velocidade nem energia do leitor por conta propria.
 *
 * Transporte bulk-only baseado no usbmass_bd do PS2SDK (AFL 2.0).
 */
#include <irx.h>
#include <loadcore.h>
#include <sifcmd.h>
#include <sysclib.h>
#include <thbase.h>
#include <thsemap.h>
#include <usbd.h>
#include <usbd_macro.h>

#include "cdusb_rpc.h"

IRX_ID("cdusb", 1, 1);

#define SUB_ATAPI	0x02
#define SUB_SFF8070	0x05
#define SUB_SCSI	0x06
#define PROTO_BULK	0x50
#define USB_CHUNK	4096	/* maior transferencia unica no USB 1.1 do PS2 */

#define CBW_SIG		0x43425355
#define CSW_SIG		0x53425355

typedef struct {
	int devId;		/* -1 = livre */
	int ctrl, epIn, epOut;
	UsbEndpointDescriptor *dIn, *dOut;
	u8 config, ifnum, ifalt;
	cdusb_dev_info info;
} cddev;

static cddev devs[CDUSB_MAXDEV];
static volatile int finalized, connects, disconnects;
static u32 cd_ids[CDUSB_MAXDEV];	/* vid << 16 | pid dos leitores achados */
static int n_cd_ids;

static int io_lock;		/* um comando por vez */
static int work_sema;		/* acorda a tarefa de configuracao */
static int xfer_sema;
static volatile u32 xfer_gen = 1;
static volatile int xfer_rc, xfer_bytes, xfer_tmo;
static u32 tag;

static u8 cbw[32] __attribute__((aligned(64)));
static u8 csw[16] __attribute__((aligned(64)));
static u8 small_buf[64] __attribute__((aligned(64)));
static u8 rpc_buf[sizeof(cdusb_scsi_resp) + CDUSB_DATA_MAX] __attribute__((aligned(64)));

static sceUsbdLddOps ldd;

/* ------------------------------------------------------------------ transferencias */
static struct {
	u8 *buf;
	u32 left, cur, done;
	int pipe;
} bx;

static void bulk_cb(int rc, int bytes, void *arg)
{
	if ((u32)arg != xfer_gen)
		return;				/* transferencia velha (prazo estourado) */
	if (rc == USB_RC_OK) {
		bx.done += bytes;
		bx.left -= bytes;
		bx.buf += bytes;
		/* pacote curto = o leitor parou de mandar: nao pede mais */
		if (bx.left > 0 && (u32)bytes == bx.cur) {
			bx.cur = bx.left > USB_CHUNK ? USB_CHUNK : bx.left;
			rc = sceUsbdBulkTransfer(bx.pipe, bx.buf, bx.cur, bulk_cb, arg);
			if (rc == USB_RC_OK)
				return;
		}
	}
	xfer_rc = rc;
	xfer_bytes = bx.done;
	SignalSema(xfer_sema);
}

static void ctrl_cb(int rc, int bytes, void *arg)
{
	if ((u32)arg != xfer_gen)
		return;
	xfer_rc = rc;
	xfer_bytes = bytes;
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

/* espera a transferencia iniciada com xfer_gen; CDUSB_E_TIMEOUT no prazo */
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
	xfer_gen++;			/* callbacks atrasados passam a ser ignorados */
	CancelAlarm(alarm_cb, g);
	while (PollSema(xfer_sema) == 0)
		;
	xfer_tmo = 0;
	return tmo ? CDUSB_E_TIMEOUT : rc;
}

static int bulk(cddev *d, int in, void *buf, u32 len, int ms, u32 *got)
{
	int rc;

	bx.buf = buf;
	bx.left = len;
	bx.cur = len > USB_CHUNK ? USB_CHUNK : len;
	bx.done = 0;
	bx.pipe = in ? d->epIn : d->epOut;
	xfer_rc = -1;
	rc = sceUsbdBulkTransfer(bx.pipe, bx.buf, bx.cur, bulk_cb, (void *)xfer_gen);
	if (rc == USB_RC_OK)
		rc = xfer_wait(ms);
	if (got)
		*got = bx.done;
	return rc;
}

static int ctrl(cddev *d, int rt, int req, int val, int idx, int len, void *data)
{
	int rc;

	xfer_rc = -1;
	rc = sceUsbdControlTransfer(d->ctrl, rt, req, val, idx, len, data, ctrl_cb, (void *)xfer_gen);
	if (rc != USB_RC_OK)
		return rc;
	return xfer_wait(3000);
}

/* canal novo = sequencia de pacotes zerada do lado do PS2 */
static void reopen(cddev *d, int in)
{
	if (in) {
		if (d->epIn >= 0)
			sceUsbdClosePipe(d->epIn);
		d->epIn = sceUsbdOpenPipe(d->devId, d->dIn);
	} else {
		if (d->epOut >= 0)
			sceUsbdClosePipe(d->epOut);
		d->epOut = sceUsbdOpenPipeAligned(d->devId, d->dOut);
	}
}

/* CLEAR_FEATURE(ENDPOINT_HALT) no leitor + canal novo no PS2 */
static void clear_halt(cddev *d, int in)
{
	UsbEndpointDescriptor *e = in ? d->dIn : d->dOut;
	ctrl(d, USB_DIR_OUT | USB_RECIP_ENDPOINT, USB_REQ_CLEAR_FEATURE, 0, e->bEndpointAddress, 0, NULL);
	reopen(d, in);
}

/* reset do protocolo bulk-only (usbmassbulk_10, 5.3.4) */
static void reset_recovery(cddev *d)
{
	d->info.resets++;
	reopen(d, 1);
	reopen(d, 0);
	ctrl(d, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, 0xFF, 0, d->ifnum, 0, NULL);
	clear_halt(d, 1);
	clear_halt(d, 0);
}

static int read_csw(cddev *d, u32 t)
{
	u32 got = 0;
	int rc = bulk(d, 1, csw, 13, 5000, &got);

	if (rc == USB_RC_STALL) {
		clear_halt(d, 1);
		rc = bulk(d, 1, csw, 13, 5000, &got);
	}
	if (rc != USB_RC_OK || got != 13)
		return rc == USB_RC_OK ? CDUSB_PHASE : rc;
	if ((csw[0] | csw[1] << 8 | csw[2] << 16 | (u32)csw[3] << 24) != CSW_SIG ||
	    (csw[4] | csw[5] << 8 | csw[6] << 16 | (u32)csw[7] << 24) != t)
		return CDUSB_PHASE;
	return CDUSB_OK;
}

/*
 * Um comando SCSI completo: CBW, dados, CSW. Na recusa (status 1) le o
 * REQUEST SENSE. Devolve CDUSB_OK, CDUSB_CHECK, CDUSB_PHASE ou erro < 0.
 */
static int scsi(cddev *d, const u8 *cdb, int cdb_len, int dir, u8 *data, u32 len, int ms,
		int *residue, int *sense, int *usb_rc)
{
	u32 t, got = 0;
	int rc, r, status;

	if (residue) *residue = 0;
	if (sense) *sense = 0;
	if (usb_rc) *usb_rc = 0;
	d->info.cmds++;

	t = ++tag;
	memset(cbw, 0, 31);
	cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43;	/* USBC */
	cbw[4] = t; cbw[5] = t >> 8; cbw[6] = t >> 16; cbw[7] = t >> 24;
	cbw[8] = len; cbw[9] = len >> 8; cbw[10] = len >> 16; cbw[11] = len >> 24;
	cbw[12] = (dir == 1) ? 0x80 : 0x00;
	cbw[13] = 0;					/* LUN 0 */
	cbw[14] = cdb_len;
	memcpy(cbw + 15, cdb, cdb_len);

	rc = bulk(d, 0, cbw, 31, 3000, NULL);
	if (rc != USB_RC_OK) {
		if (usb_rc) *usb_rc = rc;
		if (rc == CDUSB_E_TIMEOUT) d->info.timeouts++;
		reset_recovery(d);
		d->info.last_err = rc;
		return rc == CDUSB_E_TIMEOUT ? CDUSB_E_TIMEOUT : CDUSB_E_USB;
	}

	if (len && dir) {
		rc = bulk(d, dir == 1, data, len, ms, &got);
		if (rc == CDUSB_E_TIMEOUT) {
			d->info.timeouts++;
			d->info.last_err = rc;
			reset_recovery(d);
			return CDUSB_E_TIMEOUT;
		}
		if (rc == USB_RC_STALL)
			clear_halt(d, dir == 1);	/* recusou os dados: o CSW vem depois */
		else if (rc != USB_RC_OK) {
			if (usb_rc) *usb_rc = rc;
			d->info.last_err = rc;
			reset_recovery(d);
			return CDUSB_E_USB;
		}
	}

	r = read_csw(d, t);
	if (r != CDUSB_OK) {
		if (usb_rc) *usb_rc = r;
		if (r == CDUSB_E_TIMEOUT) d->info.timeouts++;
		d->info.last_err = r;
		reset_recovery(d);
		return r == CDUSB_E_TIMEOUT ? CDUSB_E_TIMEOUT : CDUSB_PHASE;
	}
	if (residue)
		*residue = csw[8] | csw[9] << 8 | csw[10] << 16 | csw[11] << 24;
	status = csw[12];
	if (status == 2) {
		reset_recovery(d);
		return CDUSB_PHASE;
	}
	if (status == 1) {
		if (sense) {
			u8 rs[12] = {0x03, 0, 0, 0, 18, 0, 0, 0, 0, 0, 0, 0};
			int r2;
			memset(small_buf, 0, 18);
			r2 = scsi(d, rs, 12, 1, small_buf, 18, 3000, NULL, NULL, NULL);
			if (r2 == CDUSB_OK)
				*sense = (small_buf[2] & 0x0f) << 16 | small_buf[12] << 8 | small_buf[13];
		}
		return CDUSB_CHECK;
	}
	(void)got;
	return CDUSB_OK;
}

/* ------------------------------------------------------------------ dispositivos */
static int is_cd_id(u32 id)
{
	int i;
	for (i = 0; i < n_cd_ids; i++)
		if (cd_ids[i] == id)
			return 1;
	return 0;
}

/* interface de armazenamento bulk-only do dispositivo (ou NULL) */
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
			if (in->bInterfaceClass == USB_CLASS_MASS_STORAGE &&
			    in->bInterfaceProtocol == PROTO_BULK && in->bNumEndpoints >= 2 &&
			    (in->bInterfaceSubClass == SUB_ATAPI || in->bInterfaceSubClass == SUB_SFF8070 ||
			     in->bInterfaceSubClass == SUB_SCSI))
				return in;
		}
		p += p[0];
	}
	return NULL;
}

static cddev *find_dev(int devId)
{
	int i;
	for (i = 0; i < CDUSB_MAXDEV; i++)
		if (devs[i].devId == devId)
			return &devs[i];
	return NULL;
}

static int cd_probe(int devId)
{
	UsbDeviceDescriptor *dev;
	UsbInterfaceDescriptor *in = find_mass_if(devId, NULL);

	if (!in || !find_dev(-1))
		return 0;
	if (!finalized)
		return 1;		/* antes do FINALIZE: pega tudo para ler o INQUIRY */
	/* leitor ligado depois: ATAPI (02) e SFF-8070i (05) sao de leitor de CD;
	 * pendrive usa SCSI (06), que fica para o usbmass */
	if (in->bInterfaceSubClass == SUB_ATAPI || in->bInterfaceSubClass == SUB_SFF8070)
		return 1;
	dev = sceUsbdScanStaticDescriptor(devId, NULL, USB_DT_DEVICE);
	return dev && is_cd_id((u32)dev->idVendor << 16 | dev->idProduct);
}

static int cd_connect(int devId)
{
	UsbDeviceDescriptor *dv;
	UsbConfigDescriptor *cfg = NULL;
	UsbInterfaceDescriptor *in;
	cddev *d = find_dev(-1);
	u8 *p, *end, path[16];
	int n;

	in = find_mass_if(devId, &cfg);
	if (!d || !in)
		return 1;
	dv = sceUsbdScanStaticDescriptor(devId, NULL, USB_DT_DEVICE);

	memset(d, 0, sizeof(*d));
	d->devId = -1;			/* continua livre ate o fim */
	d->ctrl = sceUsbdOpenPipe(devId, NULL);
	d->epIn = d->epOut = -1;
	d->config = cfg->bConfigurationValue;
	d->ifnum = in->bInterfaceNumber;
	d->ifalt = in->bAlternateSetting;

	/* endpoints bulk logo depois da interface */
	p = (u8 *)in + in->bLength;
	end = (u8 *)cfg + cfg->wTotalLength;
	for (n = 0; p + 2 <= end && p[0] >= 2 && p[1] != USB_DT_INTERFACE; p += p[0]) {
		UsbEndpointDescriptor *e = (UsbEndpointDescriptor *)p;
		if (p[1] != USB_DT_ENDPOINT || (e->bmAttributes & 3) != USB_ENDPOINT_XFER_BULK)
			continue;
		if ((e->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN && !d->dIn)
			d->dIn = e;
		else if ((e->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_OUT && !d->dOut)
			d->dOut = e;
		n++;
	}
	if (!d->dIn || !d->dOut) {
		if (d->ctrl >= 0)
			sceUsbdClosePipe(d->ctrl);
		d->devId = -1;
		return 1;
	}
	d->epIn = sceUsbdOpenPipe(devId, d->dIn);
	d->epOut = sceUsbdOpenPipeAligned(devId, d->dOut);

	d->info.vid = dv->idVendor;
	d->info.pid = dv->idProduct;
	d->info.bcd_device = dv->bcdDevice;
	d->info.bcd_usb = dv->bcdUSB;
	d->info.cls = in->bInterfaceClass;
	d->info.sub = in->bInterfaceSubClass;
	d->info.proto = in->bInterfaceProtocol;
	d->info.max_power = cfg->maxPower;
	d->info.mps_in = d->dIn->wMaxPacketSizeLB | d->dIn->wMaxPacketSizeHB << 8;
	d->info.mps_out = d->dOut->wMaxPacketSizeLB | d->dOut->wMaxPacketSizeHB << 8;
	d->info.ptype = 0xFF;
	memset(path, 0, sizeof(path));
	sceUsbdGetDeviceLocation(devId, path);
	d->info.port = (path[0] == 2) ? 1 : (path[0] == 1) ? 2 : 0;	/* mesma regra do usbmass */
	d->info.state = CDUSB_DEV_CONNECTED;
	connects++;
	__asm__ __volatile__("" : : : "memory");
	d->devId = devId;
	SignalSema(work_sema);
	return 0;
}

static int cd_disconnect(int devId)
{
	cddev *d = find_dev(devId);

	if (!d)
		return 0;
	/* o usbd ja retirou os canais; so marca */
	d->epIn = d->epOut = d->ctrl = -1;
	d->info.state = CDUSB_DEV_GONE;
	d->devId = -1;
	disconnects++;
	return 0;
}

/* configura e le o INQUIRY (fora do contexto do usbd) */
static void setup_dev(cddev *d)
{
	static const u8 inq[12] = {0x12, 0, 0, 0, 36, 0, 0, 0, 0, 0, 0, 0};
	int rc, i, sense;

	rc = ctrl(d, USB_DIR_OUT | USB_RECIP_DEVICE, USB_REQ_SET_CONFIGURATION, d->config, 0, 0, NULL);
	if (rc != USB_RC_OK) {
		d->info.last_err = rc;
		d->info.state = CDUSB_DEV_ERROR;
		return;
	}
	rc = ctrl(d, USB_DIR_OUT | USB_RECIP_INTERFACE, USB_REQ_SET_INTERFACE, d->ifalt, d->ifnum, 0, NULL);
	if (rc == USB_RC_STALL) {	/* so tem a configuracao padrao: normal */
		clear_halt(d, 1);
		clear_halt(d, 0);
	}
	/* GET MAX LUN (pode recusar) */
	small_buf[0] = 0;
	rc = ctrl(d, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE, 0xFE, 0, d->ifnum, 1, small_buf);
	if (rc == USB_RC_OK)
		d->info.max_lun = small_buf[0];
	else {
		clear_halt(d, 1);
		clear_halt(d, 0);
	}
	/* INQUIRY; alguns leitores so respondem depois de alguns pedidos */
	for (i = 0; i < 4; i++) {
		memset(small_buf, 0, 36);
		rc = scsi(d, inq, 12, 1, small_buf, 36, 5000, NULL, &sense, NULL);
		if (rc == CDUSB_OK) {
			memcpy(d->info.inquiry, small_buf, 36);
			d->info.ptype = small_buf[0] & 0x1f;
			d->info.state = CDUSB_DEV_READY;
			if (d->info.ptype == 5 && n_cd_ids < CDUSB_MAXDEV &&
			    !is_cd_id((u32)d->info.vid << 16 | d->info.pid))
				cd_ids[n_cd_ids++] = (u32)d->info.vid << 16 | d->info.pid;
			return;
		}
		DelayThread(200 * 1000);
	}
	d->info.last_err = rc;
	d->info.state = CDUSB_DEV_ERROR;
}

static void work_thread(void *arg)
{
	int i;
	(void)arg;
	for (;;) {
		WaitSema(work_sema);
		for (i = 0; i < CDUSB_MAXDEV; i++) {
			cddev *d = &devs[i];
			if (d->devId < 0 || d->info.state != CDUSB_DEV_CONNECTED)
				continue;
			WaitSema(io_lock);
			if (d->devId >= 0 && d->info.state == CDUSB_DEV_CONNECTED)
				setup_dev(d);
			SignalSema(io_lock);
		}
	}
}

/* solta tudo e registra de novo: o probe agora so aceita leitores de CD */
static void do_finalize(void)
{
	int i;

	WaitSema(io_lock);
	if (!finalized) {
		sceUsbdUnregisterLdd(&ldd);	/* o usbd retira os canais sem chamar o disconnect */
		for (i = 0; i < CDUSB_MAXDEV; i++) {
			devs[i].devId = -1;
			devs[i].info.state = CDUSB_DEV_FREE;
		}
		finalized = 1;
		ldd.next = ldd.prev = NULL;
		sceUsbdRegisterLdd(&ldd);	/* o usbd chama o probe de novo para quem esta livre */
	}
	SignalSema(io_lock);
}

/* ------------------------------------------------------------------ RPC */
static void fill_status(cdusb_status *s)
{
	int i;
	memset(s, 0, sizeof(*s));
	s->version = CDUSB_VERSION;
	s->finalized = finalized;
	s->connects = connects;
	s->disconnects = disconnects;
	for (i = 0; i < CDUSB_MAXDEV; i++)
		s->dev[i] = devs[i].info;
}

static void *rpc_fn(int fno, void *buf, int size)
{
	(void)size;
	switch (fno) {
	case CDUSB_CMD_STATUS:
		fill_status((cdusb_status *)buf);
		break;
	case CDUSB_CMD_FINALIZE:
		do_finalize();
		fill_status((cdusb_status *)buf);
		break;
	case CDUSB_CMD_SCSI: {
		cdusb_scsi_req q;
		cdusb_scsi_resp *r = (cdusb_scsi_resp *)buf;
		u8 *data = (u8 *)buf + sizeof(cdusb_scsi_resp);
		iop_sys_clock_t t0, t1;
		u32 s, us;
		cddev *d;

		memcpy(&q, buf, sizeof(q));
		memset(r, 0, sizeof(*r));
		if (q.dev < 0 || q.dev >= CDUSB_MAXDEV || q.len < 0 || q.len > CDUSB_DATA_MAX ||
		    q.cdb_len < 6 || q.cdb_len > 16 || (q.dir == 2 && q.len > 64) || q.dir < 0 || q.dir > 2) {
			r->result = CDUSB_E_ARGS;
			break;
		}
		if (q.timeout_ms < 500)
			q.timeout_ms = 500;
		if (q.dir == 2)
			memcpy(data, q.out, q.len);
		WaitSema(io_lock);
		d = &devs[q.dev];
		if (d->devId < 0 || d->info.state != CDUSB_DEV_READY) {
			r->result = CDUSB_E_NODEV;
		} else {
			GetSystemTime(&t0);
			r->result = scsi(d, q.cdb, q.cdb_len, q.dir, data, q.len, q.timeout_ms,
					 &r->residue, &r->sense, &r->usb_rc);
			GetSystemTime(&t1);
			/* diferenca de 64 bits, sem libgcc */
			t1.hi -= t0.hi + (t1.lo < t0.lo);
			t1.lo -= t0.lo;
			SysClock2USec(&t1, &s, &us);
			r->us = s * 1000000 + us;
		}
		SignalSema(io_lock);
		break;
	}
	default:
		break;
	}
	return buf;
}

static void rpc_thread(void *arg)
{
	static SifRpcDataQueue_t qd;
	static SifRpcServerData_t sd;
	(void)arg;
	sceSifInitRpc(0);
	sceSifSetRpcQueue(&qd, GetThreadId());
	sceSifRegisterRpc(&sd, CDUSB_RPC_ID, rpc_fn, rpc_buf, NULL, NULL, &qd);
	sceSifRpcLoop(&qd);
}

static int new_sema(int initial)
{
	iop_sema_t s;
	s.attr = 0;
	s.option = 0;
	s.initial = initial;
	s.max = 1;
	return CreateSema(&s);
}

static int new_thread(void (*fn)(void *), int prio, int stack)
{
	iop_thread_t t;
	int id;
	t.attr = TH_C;
	t.option = 0;
	t.thread = fn;
	t.stacksize = stack;
	t.priority = prio;
	id = CreateThread(&t);
	if (id > 0)
		StartThread(id, NULL);
	return id;
}

int _start(int argc, char *argv[])
{
	int i;
	(void)argc;
	(void)argv;

	for (i = 0; i < CDUSB_MAXDEV; i++)
		devs[i].devId = -1;
	io_lock = new_sema(1);
	work_sema = new_sema(0);
	xfer_sema = new_sema(0);
	if (io_lock < 0 || work_sema < 0 || xfer_sema < 0)
		return MODULE_NO_RESIDENT_END;

	if (new_thread(work_thread, 0x12, 0x1000) <= 0 || new_thread(rpc_thread, 0x20, 0x1000) <= 0)
		return MODULE_NO_RESIDENT_END;

	memset(&ldd, 0, sizeof(ldd));
	ldd.name = "cdusb";
	ldd.probe = cd_probe;
	ldd.connect = cd_connect;
	ldd.disconnect = cd_disconnect;
	if (sceUsbdRegisterLdd(&ldd) != USB_RC_OK)
		return MODULE_NO_RESIDENT_END;
	return MODULE_RESIDENT_END;
}
