/*
 * cdusb_iop.h - o cdusb.irx visto por outros modulos do IOP (biblioteca
 * "cdusb"). Criado por Leonardo Torres.
 *
 * Um modulo do IOP (ex.: vcd.irx) manda comandos para o leitor sem passar
 * pelo EE. Os comandos sao serializados com os do EE (RPC).
 */
#ifndef CDUSB_IOP_H
#define CDUSB_IOP_H

#include <types.h>
#include <irx.h>
#include "cdusb_rpc.h"

/* primeiro leitor de CD pronto (INQUIRY tipo 5); -1 = nenhum. 'sub' = subclasse */
int cdusb_iop_find_cd(int *sub);

/* comando SCSI; dir 0 = sem dados, 1 = leitura, 2 = escrita. Devolve
 * CDUSB_OK, CDUSB_CHECK (sense em *sense), CDUSB_PHASE ou erro < 0. */
int cdusb_iop_scsi(int dev, const u8 *cdb, int cdb_len, int dir, void *data, u32 len, int ms, int *sense);

#define cdusb_IMPORTS_start DECLARE_IMPORT_TABLE(cdusb, 1, 1)
#define cdusb_IMPORTS_end END_IMPORT_TABLE
#define I_cdusb_iop_find_cd DECLARE_IMPORT(4, cdusb_iop_find_cd)
#define I_cdusb_iop_scsi DECLARE_IMPORT(5, cdusb_iop_scsi)

#endif
