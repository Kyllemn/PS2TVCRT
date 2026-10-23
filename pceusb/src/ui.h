/*
 * ui.h - menu (L1+R1+L2+R2) e convergencia do PCEUSB.
 * Criado por Leonardo Torres.
 */
#ifndef UI_H
#define UI_H

#include <tamtypes.h>
#include <gsKit.h>

/* convergencia: mesmos limites do RETROVOLT (360) */
typedef struct {
	int h, v;		/* tamanho em %: 80..100, passo 2 */
	int x, y;		/* posicao: -40..40 e -30..30, passo 2 (pixels de 640x480) */
	int p240;		/* -1: sempre 240p no PS2 */
} tela_t;

extern tela_t tela;

/* main.c */
extern GSGLOBAL *gs;
extern u16 pad_now;
void pad_poll(void);
void L(const char *fmt, ...);

void ui_start(void);
void tela_load(void);
/* area do jogo na tela (640x240) e corte vertical da imagem de 'lines' linhas */
void tela_rect(unsigned lines, float *x0, float *y0, float *x1, float *y1, float *v0, float *v1);
/* menu: 0 = continuar, 1 = sair do jogo, 2 = reiniciar */
int ui_menu(void);

#endif
