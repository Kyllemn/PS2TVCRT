/*
 * ps2_prof.h - medidor de tempo por parte do emulador (m10)
 *
 * Usa o contador de ciclos do processador do PS2 (registrador Count do
 * COP0, 294.912 MHz). Cada parte soma so o proprio tempo: quando uma parte
 * chama outra (a CPU escreve no som, por exemplo), o tempo da de dentro
 * nao conta na de fora.
 */
#ifndef PS2_PROF_H
#define PS2_PROF_H

#ifdef PS2
enum
{
	PROF_OUTROS = 0,	/* resto do quadro (VDC, controle, nucleo) */
	PROF_CPU,		/* HuC6280 */
	PROF_FUNDO,		/* DrawBG */
	PROF_SPRITES,		/* DrawSprites (sem a decodificacao) */
	PROF_SPR_DECOD,		/* CheckFixSpriteTileCache quando decodifica */
	PROF_MISTURA,		/* Mix* + borda */
	PROF_VRAM,		/* escrita na VRAM + cache de blocos do fundo */
	PROF_PSG,		/* som do PC Engine (PSG + Blip) */
	PROF_CD,		/* PCECD_Run (ADPCM, CD audio, leitura) */
	PROF_SOM_FIM,		/* fim do quadro de som (Blip end/read) */
	PROF_N
};

extern unsigned prof_cyc[PROF_N];
extern unsigned prof_cnt[PROF_N];
extern int prof_cur;
extern unsigned prof_last;
extern int prof_stack[16];
extern int prof_sp;

static inline unsigned prof_now(void)
{
	unsigned c;
	__asm__ __volatile__("mfc0 %0, $9" : "=r"(c));
	return c;
}

static inline void prof_enter(int id)
{
	unsigned n = prof_now();

	prof_cyc[prof_cur] += n - prof_last;
	prof_last = n;
	if (prof_sp < 16)
		prof_stack[prof_sp++] = prof_cur;
	prof_cur = id;
	prof_cnt[id]++;
}

static inline void prof_leave(void)
{
	unsigned n = prof_now();

	prof_cyc[prof_cur] += n - prof_last;
	prof_last = n;
	if (prof_sp > 0)
		prof_cur = prof_stack[--prof_sp];
	else
		prof_cur = PROF_OUTROS;
}

#define PROF_ENTER(id)	prof_enter(id)
#define PROF_LEAVE()	prof_leave()
#else
#define PROF_ENTER(id)	do { } while (0)
#define PROF_LEAVE()	do { } while (0)
#endif

#endif
