/*
 * cdusb_ee.c - cliente do cdusb.irx no EE.
 * Criado por Leonardo Torres.
 *
 * Toda chamada ao IOP e assincrona com prazo: se o IOP parar de responder,
 * o EE nao trava junto (marca "stuck" e devolve erro).
 */
#include <kernel.h>
#include <sifrpc.h>
#include <string.h>

#include "cdusb_ee.h"

#define EE_HZ_MS	294912u		/* ciclos por ms */

static SifRpcClientData_t client __attribute__((aligned(64)));
static u8 sbuf[128] __attribute__((aligned(64)));
static u8 rbuf[(sizeof(cdusb_scsi_resp) + CDUSB_DATA_MAX + 63) & ~63] __attribute__((aligned(64)));
static int bound, stuck;

u32 ee_ticks(void)
{
	u32 v;
	__asm__ __volatile__("mfc0 %0, $9" : "=r"(v));
	return v;
}

u32 ee_ms(u32 t0)
{
	return (ee_ticks() - t0) / EE_HZ_MS;
}

u32 ee_us(u32 t0)
{
	return (u32)(((u64)(ee_ticks() - t0) * 1000) / EE_HZ_MS);
}

/* o contador volta a zero a cada ~14,5 s: soma os pedacos */
u64 ee_now_us(void)
{
	static u32 last;
	static u64 acc;
	static int init;
	u32 n = ee_ticks();

	if (!init) {
		last = n;
		init = 1;
	}
	acc += (u32)(n - last);
	last = n;
	return acc * 1000 / EE_HZ_MS;
}

void ee_wait_ms(int ms)
{
	while (ms > 0) {
		int step = ms > 1000 ? 1000 : ms;
		u32 t0 = ee_ticks();
		while (ee_ms(t0) < (u32)step)
			;
		ms -= step;
	}
}

int cdusb_bind(void)
{
	int i;

	for (i = 0; i < 300; i++) {		/* ate ~3 s */
		if (SifBindRpc(&client, CDUSB_RPC_ID, 0) >= 0 && client.server) {
			bound = 1;
			return 0;
		}
		ee_wait_ms(10);
	}
	return -1;
}

int cdusb_stuck(void)
{
	return stuck;
}

/* chamada com prazo; o contador do processador volta a zero a cada ~14 s,
 * por isso o tempo e somado aos poucos */
static int call(int fno, void *s, int ssize, void *r, int rsize, int timeout_ms)
{
	u32 last, now;
	u64 acc = 0, limit = (u64)timeout_ms * EE_HZ_MS;

	if (!bound)
		return CDUSB_E_EE_RPC;
	if (stuck)
		return CDUSB_E_EE_TIMEOUT;
	if (SifCallRpc(&client, fno, SIF_RPC_M_NOWAIT, s, ssize, r, rsize, NULL, NULL) < 0)
		return CDUSB_E_EE_RPC;
	last = ee_ticks();
	while (SifCheckStatRpc(&client)) {
		now = ee_ticks();
		acc += (u32)(now - last);
		last = now;
		if (acc > limit) {
			stuck = 1;
			return CDUSB_E_EE_TIMEOUT;
		}
	}
	return 0;
}

#define STATUS_RSIZE	((sizeof(cdusb_status) + 63) & ~63)

int cdusb_get_status(cdusb_status *st)
{
	int r;
	memset(sbuf, 0, 16);
	r = call(CDUSB_CMD_STATUS, sbuf, 16, rbuf, STATUS_RSIZE, 3000);
	if (r == 0)
		memcpy(st, rbuf, sizeof(*st));
	return r;
}

int cdusb_finalize(cdusb_status *st)
{
	int r;
	memset(sbuf, 0, 16);
	r = call(CDUSB_CMD_FINALIZE, sbuf, 16, rbuf, STATUS_RSIZE, 10000);
	if (r == 0)
		memcpy(st, rbuf, sizeof(*st));
	return r;
}

int cdusb_scsi(int dev, const u8 *cdb, int cdb_len, int dir, void *data, int len,
	       int timeout_ms, cdusb_scsi_resp *res)
{
	cdusb_scsi_req *q = (cdusb_scsi_req *)sbuf;
	int r, rsize;

	memset(res, 0, sizeof(*res));
	if (len < 0 || len > CDUSB_DATA_MAX || (dir == 2 && len > 64) || cdb_len > 16) {
		res->result = CDUSB_E_ARGS;
		return res->result;
	}
	memset(q, 0, sizeof(*q));
	q->dev = dev;
	q->dir = dir;
	q->len = dir ? len : 0;
	q->timeout_ms = timeout_ms;
	q->cdb_len = cdb_len;
	memcpy(q->cdb, cdb, cdb_len);
	if (dir == 2 && len)
		memcpy(q->out, data, len);

	rsize = (sizeof(cdusb_scsi_resp) + (dir == 1 ? len : 0) + 63) & ~63;
	r = call(CDUSB_CMD_SCSI, q, sizeof(*q), rbuf, rsize, timeout_ms + 3000);
	if (r < 0) {
		res->result = r;
		return r;
	}
	memcpy(res, rbuf, sizeof(*res));
	if (dir == 1 && len && data)
		memcpy(data, rbuf + sizeof(cdusb_scsi_resp), len);
	return res->result;
}
