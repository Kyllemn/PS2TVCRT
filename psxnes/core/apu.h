/* psxnes - APU do NES (logica: contadores, envelope, sweep, sequenciador).
 * A saida e um estado por canal que o frontend aplica no chip de som (SPU no PS1). */
#ifndef PSXNES_APU_H
#define PSXNES_APU_H
#include <stdint.h>

typedef struct {
	uint16_t timer;     /* periodo do NES */
	uint8_t vol;        /* 0..15 (triangulo: 15 ligado / 0 desligado) */
	uint8_t duty;       /* pulsos */
	uint8_t restart;    /* nota reiniciada (escrita em $4003/$4007/$400B/$400F) */
	uint8_t noise_short;
} ApuOut;

extern ApuOut apu_out[4];       /* 0,1 = pulsos, 2 = triangulo, 3 = ruido */
extern uint8_t apu_irq;

void apu_reset(void);
void apu_write(uint16_t a, uint8_t v);
uint8_t apu_read_status(void);
void apu_run(int32_t cpu_cycles);   /* avanca o sequenciador ate o ciclo dado */
void apu_frame_start(int32_t cpu_cycles);
#endif
