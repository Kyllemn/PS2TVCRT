/*
 * cdusb_ee.h - cliente do cdusb.irx no EE (base da futura libcdusb).
 * Criado por Leonardo Torres.
 */
#ifndef CDUSB_EE_H
#define CDUSB_EE_H

#include <tamtypes.h>
#include "cdusb_rpc.h"

/* erros do lado do EE */
#define CDUSB_E_EE_TIMEOUT	(-200)	/* o IOP nao respondeu: tudo parado */
#define CDUSB_E_EE_RPC		(-201)	/* sem conexao com o cdusb.irx */

/* relogio do EE (contador do processador, 294,912 MHz) */
u32 ee_ticks(void);
u32 ee_ms(u32 t0);			/* ms desde t0 (ate ~14 s) */
u32 ee_us(u32 t0);			/* us desde t0 (ate ~14 s) */
void ee_wait_ms(int ms);

int cdusb_bind(void);			/* 0 = conectado ao cdusb.irx */
int cdusb_stuck(void);			/* 1 = o IOP deixou de responder */
int cdusb_get_status(cdusb_status *st);
int cdusb_finalize(cdusb_status *st);
/* comando SCSI; dir 0 = sem dados, 1 = leitura, 2 = escrita (len <= 64).
 * Devolve res->result (CDUSB_OK, CDUSB_CHECK, ...). */
int cdusb_scsi(int dev, const u8 *cdb, int cdb_len, int dir, void *data, int len,
	       int timeout_ms, cdusb_scsi_resp *res);

#endif
