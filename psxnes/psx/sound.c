/* psxnes - canais do APU tocados direto nas vozes do SPU (sem streaming: nao picota).
 * Vozes 0,1 = pulsos (ondas de 8 passos com o ciclo de trabalho do NES), 2 = triangulo (32 passos),
 * 3 = ruido (gerador de ruido do proprio SPU). Atualizado uma vez por frame. */
#include <stdint.h>
#include <string.h>
#include <psxspu.h>
#include <hwregs_c.h>
#include "../core/apu.h"

#define WAVE_BASE 0x1000
static const uint8_t duty_seq[4][8] = {
	{0,1,0,0,0,0,0,0}, {0,1,1,0,0,0,0,0}, {0,1,1,1,1,0,0,0}, {1,0,0,1,1,1,1,1}
};
static const uint8_t tri_seq[32] = {15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};

static uint8_t buf[512] __attribute__((aligned(4)));
static uint32_t addr_duty[4], addr_tri, addr_silence;
static uint8_t cur_duty[2] = {255, 255};

/* bloco ADPCM com filtro 0 e shift 0: amostra = nibble * 4096 */
static void block(uint8_t *b, const int8_t *nib, uint8_t flags) {
	b[0] = 0; b[1] = flags;
	for (int i = 0; i < 14; i++) b[2 + i] = (nib[2 * i] & 15) | ((nib[2 * i + 1] & 15) << 4);
}

static void upload(uint32_t addr, int bytes) {
	SpuSetTransferStartAddr(addr);
	SpuWrite((const uint32_t *)buf, bytes);
	SpuIsTransferCompleted(1);
}

void sound_init(void) {
	SpuInit();
	uint32_t a = WAVE_BASE;
	int8_t nib[28];
	/* pulsos: 56 amostras = 7 periodos de 8, em 2 blocos com laco */
	for (int d = 0; d < 4; d++) {
		for (int blk = 0; blk < 2; blk++) {
			for (int i = 0; i < 28; i++) nib[i] = duty_seq[d][(blk * 28 + i) & 7] ? 7 : -7;
			block(buf + blk * 16, nib, blk ? 3 : 4);
		}
		upload(a, 32); addr_duty[d] = a; a += 32;
	}
	/* triangulo: 224 amostras = 7 periodos de 32, em 8 blocos */
	for (int blk = 0; blk < 8; blk++) {
		for (int i = 0; i < 28; i++) nib[i] = (int8_t)tri_seq[(blk * 28 + i) & 31] - 8;
		block(buf + blk * 16, nib, blk == 0 ? 4 : (blk == 7 ? 3 : 0));
	}
	upload(a, 128); addr_tri = a; a += 128;
	memset(nib, 0, sizeof nib); block(buf, nib, 7); upload(a, 16); addr_silence = a;

	for (int v = 0; v < 4; v++) {
		SPU_CH_ADSR1(v) = 0x00ff; SPU_CH_ADSR2(v) = 0x0000;
		SPU_CH_VOL_L(v) = 0; SPU_CH_VOL_R(v) = 0;
		SPU_CH_FREQ(v) = 0x1000;
	}
	SPU_CH_ADDR(0) = getSPUAddr(addr_duty[2]); SPU_CH_ADDR(1) = getSPUAddr(addr_duty[2]);
	SPU_CH_ADDR(2) = getSPUAddr(addr_tri); SPU_CH_ADDR(3) = getSPUAddr(addr_silence);
	SPU_NOISE_MODE1 = 1 << 3;
	SPU_MASTER_VOL_L = 0x3fff; SPU_MASTER_VOL_R = 0x3fff;
	SpuSetKey(1, 0x0f);
}

static inline uint16_t clamp_pitch(uint32_t p) { return p > 0x3fff ? 0x3fff : (uint16_t)p; }

/* passo/shift do gerador de ruido do SPU mais proximo da taxa do NES */
static uint16_t noise_ctrl(uint16_t period) {
	uint32_t target = 1789773u / period;
	uint32_t best = 0, bdiff = 0xffffffff;
	for (int sh = 0; sh < 16; sh++) for (int st = 0; st < 4; st++) {
		uint32_t r = (44100u * (4 + st)) >> (17 - sh);
		if (sh >= 17) r = 44100u * (4 + st) << (sh - 17);
		uint32_t d = r > target ? r - target : target - r;
		if (d < bdiff) { bdiff = d; best = (sh << 2) | st; }
	}
	return best;
}

void sound_update(void) {
	uint32_t keyon = 0;
	for (int i = 0; i < 2; i++) {
		ApuOut *o = &apu_out[i];
		if (o->duty != cur_duty[i]) { cur_duty[i] = o->duty; SPU_CH_ADDR(i) = getSPUAddr(addr_duty[o->duty]); keyon |= 1 << i; }
		if (o->restart) { o->restart = 0; keyon |= 1 << i; }
		SPU_CH_FREQ(i) = clamp_pitch(83116u / (o->timer + 1u));
		uint16_t vol = o->vol * 300;
		SPU_CH_VOL_L(i) = vol; SPU_CH_VOL_R(i) = vol;
	}
	ApuOut *t = &apu_out[2];
	SPU_CH_FREQ(2) = clamp_pitch(166233u / (t->timer + 1u));
	SPU_CH_VOL_L(2) = SPU_CH_VOL_R(2) = t->vol ? 7400 : 0;
	ApuOut *n = &apu_out[3];
	if (n->restart) n->restart = 0;
	SPU_CTRL = (SPU_CTRL & ~0x3f00) | (noise_ctrl(n->timer) << 8);
	SPU_CH_VOL_L(3) = SPU_CH_VOL_R(3) = n->vol * 350;
	if (keyon) SpuSetKey(1, keyon);
}
