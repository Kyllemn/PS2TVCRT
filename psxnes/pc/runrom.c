/* roda uma ROM no PC e grava frames em PPM. uso: runrom rom.nes saida_prefixo roteiro
 * roteiro: "frame:botoes,frame:botoes,..." botoes em hex (bit0=A,1=B,2=Sel,3=Start,4=Cima,5=Baixo,6=Esq,7=Dir)
 * e "shot:frame" para gravar. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/nes.h"
static const uint8_t pal[64][3] = {
{84,84,84},{0,30,116},{8,16,144},{48,0,136},{68,0,100},{92,0,48},{84,4,0},{60,24,0},{32,42,0},{8,58,0},{0,64,0},{0,60,0},{0,50,60},{0,0,0},{0,0,0},{0,0,0},
{152,150,152},{8,76,196},{48,50,236},{92,30,228},{136,20,176},{160,20,100},{152,34,32},{120,60,0},{84,90,0},{40,114,0},{8,124,0},{0,118,40},{0,102,120},{0,0,0},{0,0,0},{0,0,0},
{236,238,236},{76,154,236},{120,124,236},{176,98,236},{228,84,236},{236,88,180},{236,106,100},{212,136,32},{160,170,0},{116,196,0},{76,208,32},{56,204,108},{56,180,204},{60,60,60},{0,0,0},{0,0,0},
{236,238,236},{168,204,236},{188,188,236},{212,178,236},{236,174,236},{236,174,212},{236,180,176},{228,196,144},{204,210,120},{180,222,120},{168,226,144},{152,226,180},{160,214,228},{160,162,160},{0,0,0},{0,0,0}};
int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *rom = malloc(n); if (fread(rom, 1, n, f) != (size_t)n) return 1; fclose(f);
	if (nes_load(rom, n)) { printf("ROM nao suportada\n"); return 1; }
	nes_reset();
	int frames = atoi(argv[3]);
	for (int fr = 0; fr < frames; fr++) {
		pad_state = 0;
		for (int i = 4; i + 2 < argc + 0 && i < argc; i += 3) {
			int a = atoi(argv[i]), b = atoi(argv[i + 1]);
			if (fr >= a && fr < b) pad_state |= strtol(argv[i + 2], 0, 16);
		}
		nes_frame();
		if (fr % 30 == 29 || fr == frames - 1) {
			char nm[256]; sprintf(nm, "%s%05d.ppm", argv[2], fr + 1);
			FILE *o = fopen(nm, "wb"); fprintf(o, "P6 256 240 255\n");
			for (int y = 0; y < 240; y++) for (int x = 0; x < 256; x++) fwrite(pal[nes_screen[y][x] & 63], 1, 3, o);
			fclose(o);
		}
	}
	printf("ok %d frames\n", frames);
	return 0;
}
