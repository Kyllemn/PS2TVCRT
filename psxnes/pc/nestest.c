/* compara o nucleo com o nestest.log (modo automatico, inicio em $C000) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/cpu.h"

static uint8_t ram[0x800], prg[0x8000];
uint8_t bus_read(uint16_t a) {
	if (a < 0x2000) return ram[a & 0x7ff];
	if (a >= 0x8000) return prg[a & 0x7fff];
	return 0xff;
}
void bus_write(uint16_t a, uint8_t v) { if (a < 0x2000) ram[a & 0x7ff] = v; }

int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb"); uint8_t h[16]; fread(h, 1, 16, f);
	fread(prg, 1, 0x4000, f); memcpy(prg + 0x4000, prg, 0x4000); fclose(f);
	FILE *log = fopen(argv[2], "r"); char line[256];
	memset(&cpu, 0, sizeof cpu);
	cpu.pc = 0xc000; cpu.s = 0xfd; cpu_setp(0x24); cpu.cycles = 7;
	int n = 0;
	while (fgets(line, sizeof line, log)) {
		unsigned pc, a, x, y, p, sp; long cyc;
		sscanf(line, "%4x", &pc);
		sscanf(strstr(line, "A:"), "A:%x X:%x Y:%x P:%x SP:%x", &a, &x, &y, &p, &sp);
		cyc = atol(strstr(line, "CYC:") + 4);
		if (cpu.pc != pc || cpu.a != a || cpu.x != x || cpu.y != y || cpu_getp() != p || cpu.s != sp || cpu.cycles != cyc) {
			printf("DIFERENCA na linha %d:\n  esperado: %s  obtido:   PC:%04X A:%02X X:%02X Y:%02X P:%02X SP:%02X CYC:%d\n",
			       n + 1, line, cpu.pc, cpu.a, cpu.x, cpu.y, cpu_getp(), cpu.s, cpu.cycles);
			return 1;
		}
		cpu_step(); n++;
	}
	printf("OK: %d instrucoes iguais ao nestest.log (resultado: $02=%02X $03=%02X)\n", n, ram[2], ram[3]);
	return 0;
}
