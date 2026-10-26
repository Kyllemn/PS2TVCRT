/*
 * cdusb_rpc.h - conversa entre o cdusb.irx (IOP) e o EE.
 * Criado por Leonardo Torres.
 *
 * O IRX so transporta comandos de leitor de CD (SCSI/MMC) pelo USB
 * (protocolo bulk-only). Toda a logica de CD (TOC, setores, musica) fica
 * no EE. Os pedidos sao serializados: um comando por vez.
 */
#ifndef CDUSB_RPC_H
#define CDUSB_RPC_H

#define CDUSB_RPC_ID		0x52564344	/* "RVCD" */
#define CDUSB_VERSION		1

#define CDUSB_MAXDEV		4
/* maior leitura de uma vez: 16 setores crus com subcodigo (16 x 2448) */
#define CDUSB_DATA_MAX		(16 * 2448)

enum {
	CDUSB_CMD_STATUS = 1,	/* devolve cdusb_status */
	CDUSB_CMD_SCSI,		/* envia cdusb_scsi_req, recebe cdusb_scsi_resp + dados */
	CDUSB_CMD_FINALIZE,	/* fica so com os leitores de CD; o resto vai para o usbmass */
};

/* estado de cada dispositivo */
enum {
	CDUSB_DEV_FREE = 0,
	CDUSB_DEV_CONNECTED,	/* usbd entregou, falta configurar */
	CDUSB_DEV_READY,	/* configurado e INQUIRY lido */
	CDUSB_DEV_ERROR,	/* falhou ao configurar ou no INQUIRY */
	CDUSB_DEV_GONE,		/* foi desconectado (cabo, energia) */
};

/* resultado de um comando */
#define CDUSB_OK		0	/* comando aceito */
#define CDUSB_CHECK		1	/* leitor recusou: ver sense */
#define CDUSB_PHASE		2	/* erro de fase do protocolo (reset feito) */
#define CDUSB_E_USB		(-1)	/* erro de transferencia USB */
#define CDUSB_E_TIMEOUT		(-100)	/* leitor nao respondeu no prazo (reset feito) */
#define CDUSB_E_NODEV		(-101)	/* dispositivo nao existe ou nao esta pronto */
#define CDUSB_E_ARGS		(-102)	/* pedido invalido */

typedef struct {
	unsigned short vid, pid, bcd_device, bcd_usb;
	unsigned char cls, sub, proto, port;	/* interface de armazenamento; porta 1/2 (0 = hub) */
	unsigned char state, ptype;		/* ptype = tipo do INQUIRY (5 = CD/DVD), 0xFF = nao lido */
	unsigned char max_power, max_lun;	/* max_power em unidades de 2 mA */
	unsigned short mps_in, mps_out;		/* tamanho de pacote dos endpoints */
	int last_err;
	int cmds, timeouts, resets;
	char inquiry[36];			/* resposta crua do INQUIRY */
	int pad[3];
} cdusb_dev_info;				/* 96 bytes */

typedef struct {
	int version;
	int finalized;
	int connects, disconnects;		/* eventos desde que o modulo subiu */
	cdusb_dev_info dev[CDUSB_MAXDEV];
} cdusb_status;					/* 16 + 4 x 96 = 400 bytes */

typedef struct {
	int dev;		/* indice em cdusb_status.dev */
	int dir;		/* 0 sem dados, 1 leitor -> PS2, 2 PS2 -> leitor,
				 * 3 leitor -> PS2 com fila completa no USB (c10: teste) */
	int len;		/* bytes de dados (<= CDUSB_DATA_MAX; saida <= 64) */
	int timeout_ms;
	int cdb_len;		/* 6, 10 ou 12 */
	int pad[3];
	unsigned char cdb[16];
	unsigned char out[64];	/* dados de saida (pequenos) */
} cdusb_scsi_req;		/* 32 + 16 + 64 = 112 bytes */

typedef struct {
	int result;		/* CDUSB_OK, CDUSB_CHECK, CDUSB_PHASE ou erro < 0 */
	int usb_rc;		/* codigo do usbd na falha */
	int residue;		/* bytes que o leitor deixou de mandar */
	int sense;		/* CHAVE << 16 | ASC << 8 | ASCQ (0 = nada) */
	int us;			/* tempo do comando no IOP */
	int pad[3];
	/* seguem len bytes de dados */
} cdusb_scsi_resp;		/* 32 bytes */

#endif
