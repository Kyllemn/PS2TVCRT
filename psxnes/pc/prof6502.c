/* perfil do codigo 6502 no PC: roda um roteiro de botoes e lista os enderecos que mais gastam ciclos.
 * uso: prof6502 rom.nes "w N;t BTN hold after;h BTN N;p" (BTN: A B SEL START U D L R; p = imprime e zera)
 * compilar com -DPROFILE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/nes.h"
#include "../core/cpu.h"
extern long pc_cyc[65536], instr_count, idle_skipped;
static int frame;
static void run(int n, int pad) { while (n--) { pad_state = pad; nes_frame(); frame++; } }
static int btn(const char *b) {
	static const char *nm[] = { "A", "B", "SEL", "START", "U", "D", "L", "R" };
	for (int i = 0; i < 8; i++) if (!strcmp(b, nm[i])) return 1 << i;
	return 0;
}
static int last_frame;
static void report(void) {
	long tot = 0; for (int i = 0; i < 65536; i++) tot += pc_cyc[i];
	int fr = frame - last_frame; last_frame = frame;
	printf("== frames %d..%d: %.0f ciclos uteis/frame (de 29781), idle %.0f/frame\n", frame - fr, frame, (double)tot / fr, (double)idle_skipped / fr);
	/* agrupa por blocos de 16 bytes */
	static long grp[4096]; memset(grp, 0, sizeof grp);
	for (int i = 0; i < 65536; i++) grp[i >> 4] += pc_cyc[i];
	for (int k = 0; k < 12; k++) {
		int best = 0; for (int i = 1; i < 4096; i++) if (grp[i] > grp[best]) best = i;
		if (!grp[best]) break;
		printf("  %04x-%04x %5.1f%%\n", best << 4, (best << 4) + 15, 100.0 * grp[best] / tot);
		grp[best] = 0;
	}
	memset(pc_cyc, 0, sizeof pc_cyc); idle_skipped = 0;
}
int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *rom = malloc(n); if (fread(rom, 1, n, f) != (size_t)n) return 1; fclose(f);
	if (nes_load(rom, n)) return 1;
	nes_reset();
	char *s = strdup(argv[2]);
	for (char *c = strtok(s, ";"); c; c = strtok(0, ";")) {
		char a[16], b[16]; int x = 0, y = 0;
		int k = sscanf(c, " %15s %15s %d %d", a, b, &x, &y);
		if (k < 1) continue;
		if (!strcmp(a, "w")) run(atoi(b), 0);
		else if (!strcmp(a, "t")) { run(x, btn(b)); run(y, 0); }
		else if (!strcmp(a, "h")) run(x, btn(b));
		else if (!strcmp(a, "p")) report();
	}
	return 0;
}
