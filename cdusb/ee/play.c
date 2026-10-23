/*
 * play.c - toca a musica do disco pelo leitor USB (teste da fase 2).
 * Criado por Leonardo Torres.
 *
 * Usa o cdcache igual a um emulador: le a musica SEM esperar; o que nao
 * chegou vira silencio e conta como corte (fora logo depois de um pulo).
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <kernel.h>
#include <delaythread.h>
#include <libpad.h>
#include <debug.h>
#include <audsrv.h>

#include "cdusb_ee.h"
#include "cdcache.h"
#include "diag.h"
#include "play.h"

static int audio_ready;
static u8 abuf[4 * 2352] __attribute__((aligned(64)));
static u8 silence[2352] __attribute__((aligned(64)));

static int abort_o(void)
{
	return (pad_new() & PAD_CIRCLE) != 0;
}

/* proxima faixa de musica na direcao dir (-1 = nenhuma) */
static int next_audio(int t, int dir)
{
	const cdcd_toc *c = cdcd_get_toc();
	for (t += dir; t >= c->first && t <= c->last; t += dir)
		if (!cdcd_is_data(t))
			return t;
	return -1;
}

/* linha y da tela, apagando o que tinha antes */
static void line(int y, const char *fmt, ...)
{
	char s[96];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);
	if (n < 0)
		n = 0;
	while (n < 76 && n < (int)sizeof(s) - 1)
		s[n++] = ' ';
	s[n] = 0;
	scr_setXY(0, y);
	scr_printf("%s", s);
}

static void wait_o(void)
{
	S("   O = VOLTAR");
	while (!(pad_new() & PAD_CIRCLE))
		ee_wait_ms(16);
}

static int audio_start(void)
{
	struct audsrv_fmt_t fmt;

	if (audio_ready)
		return 1;
	if (audsrv_init() != 0) {
		L("[PLAY] audsrv_init falhou: %s", audsrv_get_error_string());
		return 0;
	}
	fmt.bits = 16;
	fmt.freq = 44100;
	fmt.channels = 2;
	if (audsrv_set_format(&fmt) != 0) {
		L("[PLAY] audsrv_set_format falhou: %s", audsrv_get_error_string());
		return 0;
	}
	audsrv_set_volume(MAX_VOLUME);
	audio_ready = 1;
	return 1;
}

void play_run(int dev, int sub)
{
	const cdcd_toc *toc;
	cdcd_stats cs;
	int r, t, n, pos, end, playing = 1, starting = 1, in_cut = 0, cuts = 0, k, want, i, fillc;
	u64 now, last_draw = 0, last_log = 0, jump_t;
	char bar[32];

	screen_title("MUSICA DO DISCO PELO LEITOR USB (FASE 2)");
	L("[PLAY] ---------------- tocar musica ----------------");
	if (!audio_start()) {
		S("   FALHA NO AUDIO DO PS2");
		log_save();
		wait_o();
		return;
	}
	S("   ESPERANDO O DISCO E TRAVANDO O LEITOR EM 10X...  (O = VOLTAR)");
	r = cdcd_open(dev, sub, abort_o);
	log_save();
	if (r < 0) {
		S("   NAO CONSEGUI ABRIR O DISCO (%d)", r);
		wait_o();
		return;
	}
	toc = cdcd_get_toc();
	t = next_audio(toc->first - 1, 1);
	if (t < 0) {
		S("   ESTE DISCO NAO TEM MUSICA");
		cdcd_close();
		log_save();
		wait_o();
		return;
	}
	pos = toc->lba[t];
	end = cdcd_track_end(t);
	jump_t = ee_now_us();

	screen_title("MUSICA DO DISCO PELO LEITOR USB (FASE 2)");
	line(14, "   ESQ / DIR = FAIXA     CIMA / BAIXO = 10 S     X = PAUSA     O = VOLTAR");

	for (;;) {
		int b = pad_new(), jumped = 0;

		if (b & PAD_CIRCLE)
			break;
		if (b & PAD_CROSS)
			playing = !playing;
		if (b & PAD_RIGHT) {
			n = next_audio(t, 1);
			if (n > 0) {
				t = n;
				pos = toc->lba[t];
				jumped = 1;
			}
		}
		if (b & PAD_LEFT) {
			if (pos - toc->lba[t] < 75 * 3 && (n = next_audio(t, -1)) > 0)
				t = n;
			pos = toc->lba[t];
			jumped = 1;
		}
		if (b & PAD_UP) {
			pos += 750;
			if (pos > cdcd_track_end(t) - 75)
				pos = cdcd_track_end(t) - 75;
			jumped = 1;
		}
		if (b & PAD_DOWN) {
			pos -= 750;
			if (pos < toc->lba[t])
				pos = toc->lba[t];
			jumped = 1;
		}
		if (jumped) {
			end = cdcd_track_end(t);
			starting = 1;
			in_cut = 0;
			jump_t = ee_now_us();
			L("[PLAY] pulo: faixa %d, setor %d", t, pos);
		}

		if (playing) {
			while (audsrv_available() >= 2352) {
				want = audsrv_available() / 2352;
				if (want > 4)
					want = 4;
				if (end - pos < want)
					want = end - pos;
				if (want <= 0) {
					/* fim da faixa: segue para a proxima (o leitor ja leu adiante) */
					n = next_audio(t, 1);
					if (n < 0) {
						playing = 0;
						L("[PLAY] fim do disco");
						break;
					}
					t = n;
					pos = toc->lba[t];
					end = cdcd_track_end(t);
					L("[PLAY] proxima faixa: %d", t);
					continue;
				}
				k = cdcd_read_nb(pos, want, abuf);
				if (k > 0) {
					audsrv_play_audio((char *)abuf, k * 2352);
					pos += k;
					if (starting) {
						L("[PLAY] som %d ms depois do pulo", (int)((ee_now_us() - jump_t) / 1000));
						starting = 0;
					}
					in_cut = 0;
				} else {
					audsrv_play_audio((char *)silence, 2352);
					if (!starting && !in_cut) {
						cuts++;
						L("[PLAY] CORTE no setor %d", pos);
					}
					in_cut = 1;
					break;
				}
			}
		}

		now = ee_now_us();
		if (now - last_draw > 200000) {
			int el = (pos - toc->lba[t]) / 75, len = (end - toc->lba[t]) / 75;
			last_draw = now;
			cdcd_get_stats(&cs);
			fillc = cs.ahead * 30 / 300;
			if (fillc > 30)
				fillc = 30;
			for (i = 0; i < 30; i++)
				bar[i] = i < fillc ? '#' : '-';
			bar[30] = 0;
			line(4, "   FAIXA %02d DE %02d     %02d:%02d / %02d:%02d     %s", t, toc->last, el / 60, el % 60,
			     len / 60, len % 60, !playing ? "PAUSADO" : starting ? "BUSCANDO..." : "TOCANDO");
			line(6, "   BUFFER  [%s]  %d,%d S", bar, cs.ahead / 75, (cs.ahead % 75) * 10 / 75);
			line(8, "   CORTES: %d     PULOS: %d     LEITURAS: %d     ERROS: %d", cuts, cs.repositions,
			     cs.cmds, cs.errors);
			line(9, "   ULTIMA LEITURA: %d MS     PIOR: %d MS     LEITOR: %s", cs.last_ms, cs.max_ms,
			     cs.speed_locked ? "TRAVADO EM 10X" : "VELOCIDADE PADRAO");
		}
		if (now - last_log > 5000000) {
			last_log = now;
			cdcd_get_stats(&cs);
			L("[PLAY] faixa %d setor %d: buffer %d setores, cortes %d, pulos %d, leituras %d, ultima %d ms, "
			  "pior %d ms, erros %d",
			  t, pos, cs.ahead, cuts, cs.repositions, cs.cmds, cs.last_ms, cs.max_ms, cs.errors);
		}
		DelayThread(4000);
	}

	audsrv_stop_audio();
	cdcd_close();
	L("[PLAY] saiu: cortes %d", cuts);
	log_save();
}
