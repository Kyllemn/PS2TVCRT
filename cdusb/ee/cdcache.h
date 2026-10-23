/*
 * cdcache.h - disco do leitor USB para os emuladores (fase 2).
 * Criado por Leonardo Torres.
 *
 * Uma thread le adiantado (ate 4 s de musica) para uma memoria de 512
 * setores. Dados: leitura com espera (com prazo). Musica: leitura sem
 * espera (o que nao chegou vira silencio; a emulacao nunca para por I/O).
 */
#ifndef CDCACHE_H
#define CDCACHE_H

#include <tamtypes.h>

typedef struct {
	int first, last;		/* faixas */
	int lba[101];			/* 1..99 = inicio das faixas, 100 = fim do disco */
	int ctrl[101];			/* bit 2 = faixa de dados */
} cdcd_toc;

typedef struct {
	int cmds;			/* leituras no leitor */
	int last_ms, max_ms;		/* tempo da ultima / pior leitura */
	int repositions;		/* pulos (posicao nova) */
	int errors;			/* leituras que falharam */
	int ahead;			/* setores prontos a frente do emulador */
	int speed_locked;		/* 1 = leitor travado na velocidade baixa */
	int keepalives;			/* leituras para manter o leitor girando */
	int keep_max_ms;		/* pior delas */
} cdcd_stats;

/* abre o disco do leitor 'dev' (subclasse 'sub'): espera o disco, le a TOC,
 * trava o leitor em 10x (pede 4x) e aquece. 0 = ok. 'abort' (opcional)
 * devolve 1 para desistir enquanto espera o disco. */
int cdcd_open(int dev, int sub, int (*abort)(void));
void cdcd_close(void);			/* para a thread e devolve a velocidade padrao */

const cdcd_toc *cdcd_get_toc(void);
int cdcd_track_of(int lba);
int cdcd_is_data(int track);
int cdcd_track_end(int track);

/* dados: setores de 2048; musica: setores de 2352. Com espera ate timeout_ms.
 * Devolve setores lidos (< n = prazo ou erro). */
int cdcd_read(int lba, int n, void *dst, int timeout_ms);
/* sem espera: copia os setores que ja estao prontos a partir de lba (0 = nada
 * pronto; a leitura antecipada vai buscar) */
int cdcd_read_nb(int lba, int n, void *dst);

/* aviso de busca sem esperar: o leitor ja comeca a ler em lba. Para o
 * emulador chamar quando o jogo manda posicionar (PC Engine: busca antes do
 * "toca"); quando o "toca" chegar, a musica ja esta no buffer. */
void cdcd_seek(int lba);

/* o setor ja esta na memoria? (sem esperar; se nao estiver, a leitura vai
 * buscar). Para o CD emulado adiar a entrega em vez de parar a emulacao. */
int cdcd_ready(int lba);

void cdcd_get_stats(cdcd_stats *s);

#endif
