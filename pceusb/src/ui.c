/*
 * ui.c - menu (L1+R1+L2+R2) e convergencia do PCEUSB.
 * Criado por Leonardo Torres.
 *
 * Tudo desenhado pelo GS: o texto numa textura de 320x240 (fonte 8x8 do
 * RETROVOLT, 40 colunas), retangulos e moldura como primitivas. O ajuste de
 * tela (tamanho, posicao, barras laterais) e so a posicao do retangulo da
 * imagem no GS: nenhum custo para o EE.
 *
 * Convergencia igual a do RETROVOLT (360):
 *   ESQ/DIR = H -/+, CIMA/BAIXO = V +/- (2% por toque, segurar repete por
 *   tempo: 450 ms e depois 110 ms); L1/R1 = X -/+ 2; L2/R2 = Y -/+ 2 (por
 *   aperto); X salva, O cancela, TRIANGULO padrao. Moldura dourada na area
 *   do jogo, ao vivo. Arquivo mass:/PCE/TELA_PCE.TXT: "H V P240 X Y".
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <kernel.h>
#include <delaythread.h>
#include <libpad.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>
#include <gsKit.h>
#include <libretro.h>

#include "cdusb_ee.h"
#include "font8x8.h"
#include "ui.h"

#define TELA_FILE	"mass:/PCE/TELA_PCE.TXT"
#define OW		320
#define OH		240

/* cores no formato do GS (ABGR1555, bit 15 = opaco) */
#define C_BLACK		0x8000
#define C_WHITE		0xFFFF
#define C_GOLD		(0x8000 | (4 << 10) | (26 << 5) | 31)
#define C_GRAY		(0x8000 | (16 << 10) | (16 << 5) | 16)
#define GOLD_RGBAQ	GS_SETREG_RGBAQ(0xFF, 0xD0, 0x20, 0x80, 0x00)

tela_t tela = {100, 100, 0, 0, -1};

static u16 olay[OW * OH] __attribute__((aligned(128)));
static GSTEXTURE otex;

void ui_start(void)
{
	memset(&otex, 0, sizeof(otex));
	otex.Width = OW;
	otex.Height = OH;
	otex.PSM = GS_PSM_CT16;
	otex.Mem = (u32 *)olay;
	otex.Filter = GS_FILTER_NEAREST;
}

/* ------------------------------------------------------------------ arquivo */
static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

void tela_load(void)
{
	char b[64];
	int fd, n, h, v, p, x, y;

	fd = fileXioOpen(TELA_FILE, FIO_O_RDONLY);
	if (fd < 0) {
		L("[TELA] %s nao existe: padrao 100 100 -1 0 0", TELA_FILE);
		return;
	}
	n = fileXioRead(fd, b, sizeof(b) - 1);
	fileXioClose(fd);
	if (n <= 0)
		return;
	b[n] = 0;
	if (sscanf(b, "%d %d %d %d %d", &h, &v, &p, &x, &y) != 5) {
		L("[TELA] %s ilegivel: padrao", TELA_FILE);
		return;
	}
	/* so aceita valores dentro dos limites */
	if (h >= 80 && h <= 100) tela.h = h & ~1;
	if (v >= 80 && v <= 100) tela.v = v & ~1;
	if (x >= -40 && x <= 40) tela.x = x;
	if (y >= -30 && y <= 30) tela.y = y;
	L("[TELA] convergencia lida: %d%% x %d%%, posicao %+d / %+d", tela.h, tela.v, tela.x, tela.y);
}

static void tela_save(void)
{
	char b[64];
	int fd, n;

	fileXioMkdir("mass:/PCE", 0777);
	n = snprintf(b, sizeof(b), "%d %d %d %d %d\r\n", tela.h, tela.v, -1, tela.x, tela.y);
	fd = fileXioOpen(TELA_FILE, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC, 0666);
	if (fd >= 0) {
		fileXioWrite(fd, b, n);
		fileXioClose(fd);
	}
	L("[VIDEO] convergencia salvo: %d%% x %d%%, posicao %+d / %+d (%s)", tela.h, tela.v, tela.x, tela.y,
	  fd >= 0 ? "ok" : "FALHOU");
}

/*
 * Area do jogo (tela de 640x240, 240p): largura = 640 x H%; altura = linhas
 * do jogo 1:1, cortada so se passar de 240 x V%; centralizada + posicao
 * (X em pixels de 640, Y em pixels de 480 -> metade em 240 linhas).
 */
void tela_rect(unsigned lines, float *x0, float *y0, float *x1, float *y1, float *v0, float *v1)
{
	int w = 640 * tela.h / 100, hmax = 240 * tela.v / 100, h = lines, cut = 0;

	if (h > hmax) {
		cut = (h - hmax) / 2;
		h = hmax;
	}
	*x0 = (float)((640 - w) / 2 + tela.x);
	*y0 = (float)((240 - h) / 2 + tela.y / 2);
	*x1 = *x0 + w;
	*y1 = *y0 + h;
	*v0 = (float)cut;
	*v1 = (float)(cut + h);
}

/* ------------------------------------------------------------------ texto */
static void o_clear(void)
{
	int i;
	for (i = 0; i < OW * OH; i++)
		olay[i] = C_BLACK;
}

static void o_text(int x, int y, u16 c, const char *s)
{
	for (; *s; s++, x += 8) {
		int ch = (u8)*s, r, b;
		const char *g;
		if (ch < 32 || ch > 126)
			ch = '?';
		g = &font8x8_basic[(ch - 32) * 8];
		for (r = 0; r < 8; r++)
			for (b = 0; b < 8; b++)
				if ((g[r] >> b) & 1) {
					int px = x + b, py = y + r;
					if (px >= 0 && px < OW && py >= 0 && py < OH)
						olay[py * OW + px] = c;
				}
	}
}

static void o_center(int y, u16 c, const char *fmt, ...)
{
	char s[64];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);
	o_text((OW - (int)strlen(s) * 8) / 2, y, c, s);
}

/* desenha a textura de texto no retangulo e mostra o quadro (espera o retraco) */
static void o_show(float x0, float y0, float x1, float y1, int frame)
{
	gsKit_clear(gs, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
	FlushCache(0);
	gsKit_TexManager_invalidate(gs, &otex);
	gsKit_TexManager_bind(gs, &otex);
	gsKit_prim_sprite_texture(gs, &otex, x0, y0, 0.0f, 0.0f, x1, y1, (float)OW, (float)OH, 2,
				  GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00));
	if (frame) {
		/* moldura dourada: 3 pontos nas laterais, 2 linhas em cima/embaixo */
		gsKit_prim_sprite(gs, x0, y0, x1, y0 + 2, 3, GOLD_RGBAQ);
		gsKit_prim_sprite(gs, x0, y1 - 2, x1, y1, 3, GOLD_RGBAQ);
		gsKit_prim_sprite(gs, x0, y0, x0 + 3, y1, 3, GOLD_RGBAQ);
		gsKit_prim_sprite(gs, x1 - 3, y0, x1, y1, 3, GOLD_RGBAQ);
	}
	gsKit_queue_exec(gs);
	gsKit_sync_flip(gs);
	gsKit_TexManager_nextFrame(gs);
}

/* ------------------------------------------------------------------ controle */
static u16 prev_btn;

static void wait_release(void)
{
	do {
		pad_poll();
		DelayThread(16000);
	} while (pad_now & (PAD_L1 | PAD_R1 | PAD_L2 | PAD_R2 | PAD_CROSS | PAD_CIRCLE | PAD_TRIANGLE |
			    PAD_SQUARE | PAD_START | PAD_SELECT));
	prev_btn = pad_now;
}

/* botoes recem-apertados */
static u16 pressed(void)
{
	u16 n = pad_now & ~prev_btn;
	prev_btn = pad_now;
	return n;
}

/* direcional por tempo: toque = 1 passo; segurando repete (450 ms, depois 110 ms) */
typedef struct {
	int dir;
	u64 next;
} rep_t;

static int nav(rep_t *r, int neg, int pos)
{
	int d = pos ? 1 : neg ? -1 : 0;
	u64 now = ee_now_us();

	if (!d) {
		r->dir = 0;
		return 0;
	}
	if (d != r->dir) {
		r->dir = d;
		r->next = now + 450000;
		return d;
	}
	if (now >= r->next) {
		r->next = now + 110000;
		return d;
	}
	return 0;
}

/* ------------------------------------------------------------------ convergencia */
static void convergencia(void)
{
	tela_t antes = tela;
	rep_t rh = {0, 0}, rv = {0, 0};
	int last = -1, key, dh, dv;
	u16 b;
	float x0, y0, x1, y1, v0, v1;

	wait_release();
	for (;;) {
		pad_poll();
		b = pressed();
		dh = nav(&rh, pad_now & PAD_LEFT, pad_now & PAD_RIGHT);
		dv = nav(&rv, pad_now & PAD_DOWN, pad_now & PAD_UP);
		if (dh)
			tela.h = clampi(tela.h + dh * 2, 80, 100);
		if (dv)
			tela.v = clampi(tela.v + dv * 2, 80, 100);
		if (b & PAD_L1) tela.x = clampi(tela.x - 2, -40, 40);
		if (b & PAD_R1) tela.x = clampi(tela.x + 2, -40, 40);
		if (b & PAD_L2) tela.y = clampi(tela.y - 2, -30, 30);
		if (b & PAD_R2) tela.y = clampi(tela.y + 2, -30, 30);
		if (b & PAD_TRIANGLE) {
			tela.h = tela.v = 100;
			tela.x = tela.y = 0;
		}
		if (b & PAD_CROSS) {
			tela_save();
			break;
		}
		if (b & PAD_CIRCLE) {
			tela = antes;
			L("[VIDEO] convergencia cancelada");
			break;
		}

		key = ((tela.h * 1000 + tela.v) * 100 + tela.x + 50) * 100 + tela.y + 50;
		if (key == last) {
			DelayThread(8000);
			continue;
		}
		last = key;
		o_clear();
		o_center(64, C_GOLD, "CONVERGENCIA");
		o_center(80, C_WHITE, "VALE SO PARA ESTE EMULADOR");
		o_center(108, C_WHITE, "TAMANHO %d%% X %d%%  POSICAO %+d / %+d", tela.h, tela.v, tela.x, tela.y);
		o_center(136, C_GRAY, "ESQ / DIR  TAMANHO HORIZONTAL");
		o_center(148, C_GRAY, "CIMA / BAIXO  TAMANHO VERTICAL");
		o_center(160, C_GRAY, "L1 / R1  MOVE PARA OS LADOS");
		o_center(172, C_GRAY, "L2 / R2  MOVE PARA CIMA / BAIXO");
		o_center(192, C_WHITE, "X SALVA   O CANCELA   TRIANGULO PADRAO");
		/* a moldura mostra a area do jogo: 240 linhas de altura maxima */
		tela_rect(240, &x0, &y0, &x1, &y1, &v0, &v1);
		o_show(x0, y0, x1, y1, 1);
	}
	wait_release();
}

/* ------------------------------------------------------------------ menu */
int ui_menu(void)
{
	static const char *item[4] = {"CONTINUAR", "AJUSTE DE TELA (CONVERGENCIA)", "REINICIAR O JOGO",
				      "SAIR DO JOGO"};
	rep_t rv = {0, 0};
	int sel = 0, i, d;
	u16 b;

	L("[MENU] aberto");
	wait_release();
	for (;;) {
		pad_poll();
		b = pressed();
		d = nav(&rv, pad_now & PAD_UP, pad_now & PAD_DOWN);
		if (d)
			sel = (sel + d + 4) % 4;
		if (b & PAD_CIRCLE)
			sel = 0, b = PAD_CROSS;
		if (b & PAD_CROSS) {
			L("[MENU] escolhido: %s", item[sel]);
			if (sel == 1) {
				convergencia();
				continue;
			}
			wait_release();
			return sel == 0 ? 0 : sel == 2 ? 2 : 1;
		}

		o_clear();
		o_center(40, C_GOLD, "PCEUSB - MENU");
		for (i = 0; i < 4; i++)
			o_center(80 + i * 20, i == sel ? C_GOLD : C_WHITE, "%s%s%s", i == sel ? "> " : "", item[i],
				 i == sel ? " <" : "");
		o_center(180, C_GRAY, "CIMA / BAIXO  ESCOLHER");
		o_center(192, C_GRAY, "X  CONFIRMAR      O  VOLTAR AO JOGO");
		o_show(0.0f, 0.0f, 640.0f, 240.0f, 0);
	}
}
