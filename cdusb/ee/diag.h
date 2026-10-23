/*
 * diag.h - testes do leitor de CD USB (fase 1).
 * Criado por Leonardo Torres.
 */
#ifndef DIAG_H
#define DIAG_H

#include <tamtypes.h>
#include "cdusb_rpc.h"

/* main.c */
void L(const char *fmt, ...) __attribute__((format(printf, 1, 2)));	/* so no log */
void S(const char *fmt, ...) __attribute__((format(printf, 1, 2)));	/* tela + log */
void log_save(void);
int log_ready(void);
void screen_title(const char *sub);
u16 pad_new(void);

typedef struct {
	char vendor[9], product[17], rev[5];
	int vid, pid, port, sub;
	int disc;			/* 1 = disco pronto */
	int first, last, leadout;
	int data_tracks, audio_tracks;
	char system[40];
	int profile;
	int max_kbs, cur_kbs, caps;	/* pagina 2A (caps: byte 5) */
	int data_ok, raw_ok, audio_ok, audio_silent, subq_ok, subraw_ok;
	int kbs_data8, kbs_data16, kbs_audio;	/* -1 = nao testado */
	int ms_max_data, ms_max_audio;
	int seek_avg, seek_max;			/* -1 = nao testado */
	int spinup_ms;				/* -1 = nao testado */
	int gone;			/* leitor desconectou */
	int stuck;			/* IOP parou de responder */
	int aborted;			/* usuario saiu antes do disco */
} diag_result;

void diag_run(int dev, const cdusb_dev_info *info, diag_result *r);
void diag_summary(const diag_result *r, const char *log_file);

#endif
