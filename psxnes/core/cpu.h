/* psxnes - nucleo do 6502 (2A03 sem decimal).
 * Mapa de memoria em paginas de 2 KB: se rmap[p] != 0 a leitura e direta
 * (rmap[p] ja vem deslocado para indexar pelo endereco inteiro); senao chama bus_read. */
#ifndef PSXNES_CPU_H
#define PSXNES_CPU_H
#include <stdint.h>

#define CPU_PAGES 32

typedef struct {
	uint8_t a, x, y, s;
	uint16_t pc;
	uint8_t c, v, i, d;   /* flags separadas */
	uint8_t nz_n, nz_z;   /* N = bit7 de nz_n, Z = (nz_z == 0) */
	int32_t cycles;       /* contador crescente (para o laco da frame) */
	uint8_t nmi, irq;     /* pedidos pendentes */
	uint8_t *rmap[CPU_PAGES];
	uint8_t *wmap[CPU_PAGES];
} Cpu;

extern Cpu cpu;

/* fornecidos pelo sistema (PC de teste ou PS1) */
uint8_t bus_read(uint16_t a);
void bus_write(uint16_t a, uint8_t v);

void cpu_reset(void);
uint8_t cpu_getp(void);
void cpu_setp(uint8_t p);
/* roda ate cpu.cycles >= alvo; devolve no fim da instrucao */
void cpu_run(int32_t target);
/* alvo corrente de cpu_run: o sistema pode baixar durante a execucao (ex.: IRQ reprogramado)
 * para a CPU devolver o controle e o laco da frame replanejar */
extern int32_t cpu_target;
/* executa exatamente uma instrucao (para o teste) */
void cpu_step(void);
void cpu_interp_one(void);
#endif
