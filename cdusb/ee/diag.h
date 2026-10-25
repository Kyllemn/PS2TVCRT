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

#define DIAG_SPEEDS	4

/* resultado de uma velocidade (-1 = nao testado) */
typedef struct {
	const char *name;
	int kbs;			/* pedido ao leitor (0 = sem pedido) */
	int set_ok;			/* leitor aceitou SET CD SPEED / SET STREAMING (bits 1/2) */
	int cur_kbs;			/* velocidade que o leitor diz estar usando */
	int data_bps, audio_bps;		/* medicao continua */
	int data_max_ms, audio_max_ms;	/* pior comando */
	int slow;			/* comandos acima de 300 ms */
	int pace_min_ms;		/* tempo real 1x: menor folga do buffer de 1 s */
	int pace_cuts;			/* vezes que o buffer teria esvaziado */
	int pace_max_ms;		/* pior comando no tempo real */
	int seek_avg, seek_max;
} speed_result;

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
	int nsp;				/* velocidades testadas */
	speed_result sp[DIAG_SPEEDS];
	int spinup_ms;				/* -1 = nao testado */
	int gone;			/* leitor desconectou */
	int stuck;			/* IOP parou de responder */
	int aborted;			/* usuario saiu antes do disco */
} diag_result;

void diag_run(int dev, const cdusb_dev_info *info, diag_result *r);

/* teste PS1 (plano do Ember: disco do PS1 como .bin cru) */
typedef struct {
	char vendor[9], product[17];
	int disc, data_tracks, audio_tracks;
	char system[40];
	int raw_ok, raw_mode, subq_ok, cur_kbs;
	int raw_bps16, raw_max16, raw_bps8, raw_max8;	/* dados crus em sequencia */
	int cuts, pace_min_ms, pace_max_ms;		/* tempo real 2x com cache pequeno */
	int da_avg, da_max, ad_avg, ad_max;		/* troca dados->musica / musica->dados */
	int pregap_raw, pregap_audio;			/* pausa antes da musica (de 3 setores) */
	int seek_avg, seek_max, spinup_ms;
	int stuck, gone, aborted;
} ps1_result;

void ps1_run(int dev, const cdusb_dev_info *info, ps1_result *r);
void ps1_summary(const ps1_result *r, const char *log_file);
void diag_summary(const diag_result *r, const char *log_file);

#endif
