#include <string.h>
#include "apu.h"

ApuOut apu_out[4];
uint8_t apu_irq;

static const uint8_t len_tab[32] = {10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30};
static const uint16_t noise_tab[16] = {4,8,16,32,64,96,128,160,202,254,380,508,762,1016,2034,4068};

typedef struct {
	uint8_t enabled, length, halt;
	uint8_t const_vol, vol, env_start, env_div, env_decay;
	uint16_t timer;
	uint8_t duty;
	uint8_t sw_en, sw_period, sw_neg, sw_shift, sw_reload, sw_div;
	/* triangulo */
	uint8_t lin_reload_val, lin_counter, lin_reload, control;
	/* ruido */
	uint8_t mode;
} Chan;

static Chan ch[4];
static uint8_t seq_mode, irq_inhibit, frame_irq;
static int32_t next_step; static int step;

void apu_reset(void) {
	memset(ch, 0, sizeof ch); memset(apu_out, 0, sizeof apu_out);
	seq_mode = irq_inhibit = frame_irq = 0; apu_irq = 0;
	next_step = 7457; step = 0;
	ch[3].timer = noise_tab[0];
}

static void env_clock(Chan *c) {
	if (c->env_start) { c->env_start = 0; c->env_decay = 15; c->env_div = c->vol; return; }
	if (c->env_div) { c->env_div--; return; }
	c->env_div = c->vol;
	if (c->env_decay) c->env_decay--; else if (c->halt) c->env_decay = 15;
}
static int sweep_target(int i) {
	Chan *c = &ch[i]; int d = c->timer >> c->sw_shift;
	return c->sw_neg ? c->timer - d - (i == 0) : c->timer + d;
}
static int sweep_mute(int i) { return ch[i].timer < 8 || sweep_target(i) > 0x7ff; }
static void sweep_clock(int i) {
	Chan *c = &ch[i];
	if (c->sw_div == 0 && c->sw_en && c->sw_shift && !sweep_mute(i)) c->timer = sweep_target(i);
	if (c->sw_div == 0 || c->sw_reload) { c->sw_div = c->sw_period; c->sw_reload = 0; } else c->sw_div--;
}
static void quarter(void) {
	env_clock(&ch[0]); env_clock(&ch[1]); env_clock(&ch[3]);
	Chan *t = &ch[2];
	if (t->lin_reload) t->lin_counter = t->lin_reload_val; else if (t->lin_counter) t->lin_counter--;
	if (!t->control) t->lin_reload = 0;
}
static void half(void) {
	for (int i = 0; i < 4; i++) {
		int h = (i == 2) ? ch[i].control : ch[i].halt;
		if (!h && ch[i].length) ch[i].length--;
	}
	sweep_clock(0); sweep_clock(1);
}

static void update_out(void) {
	for (int i = 0; i < 2; i++) {
		Chan *c = &ch[i]; ApuOut *o = &apu_out[i];
		o->timer = c->timer; o->duty = c->duty;
		o->vol = (c->length && !sweep_mute(i)) ? (c->const_vol ? c->vol : c->env_decay) : 0;
	}
	Chan *t = &ch[2];
	apu_out[2].timer = t->timer;
	apu_out[2].vol = (t->length && t->lin_counter && t->timer >= 2) ? 15 : 0;
	Chan *n = &ch[3];
	apu_out[3].timer = n->timer; apu_out[3].noise_short = n->mode;
	apu_out[3].vol = n->length ? (n->const_vol ? n->vol : n->env_decay) : 0;
}

void apu_write(uint16_t a, uint8_t v) {
	int i;
	switch (a) {
	case 0x4000: case 0x4004: case 0x400c:
		i = (a == 0x400c) ? 3 : (a - 0x4000) >> 2;
		ch[i].duty = v >> 6; ch[i].halt = (v >> 5) & 1; ch[i].const_vol = (v >> 4) & 1; ch[i].vol = v & 15; break;
	case 0x4001: case 0x4005:
		i = (a - 0x4000) >> 2;
		ch[i].sw_en = v >> 7; ch[i].sw_period = (v >> 4) & 7; ch[i].sw_neg = (v >> 3) & 1; ch[i].sw_shift = v & 7; ch[i].sw_reload = 1; break;
	case 0x4002: case 0x4006: case 0x400a:
		i = (a - 0x4000) >> 2; ch[i].timer = (ch[i].timer & 0x700) | v; break;
	case 0x4003: case 0x4007: case 0x400b:
		i = (a - 0x4000) >> 2; ch[i].timer = (ch[i].timer & 0xff) | ((v & 7) << 8);
		if (ch[i].enabled) ch[i].length = len_tab[v >> 3];
		if (i == 2) ch[2].lin_reload = 1; else { ch[i].env_start = 1; apu_out[i].restart = 1; }
		break;
	case 0x4008: ch[2].control = v >> 7; ch[2].lin_reload_val = v & 0x7f; break;
	case 0x400e: ch[3].mode = v >> 7; ch[3].timer = noise_tab[v & 15]; break;
	case 0x400f: if (ch[3].enabled) ch[3].length = len_tab[v >> 3]; ch[3].env_start = 1; break;
	case 0x4015:
		for (i = 0; i < 4; i++) { ch[i].enabled = (v >> i) & 1; if (!ch[i].enabled) ch[i].length = 0; }
		break;
	case 0x4017:
		seq_mode = v >> 7; irq_inhibit = (v >> 6) & 1;
		if (irq_inhibit) { frame_irq = 0; apu_irq = 0; }
		if (seq_mode) { quarter(); half(); }
		break;
	}
	update_out();
}

uint8_t apu_read_status(void) {
	uint8_t r = 0;
	for (int i = 0; i < 4; i++) if (ch[i].length) r |= 1 << i;
	if (frame_irq) r |= 0x40;
	frame_irq = 0; apu_irq = 0;
	return r;
}

void apu_frame_start(int32_t c) { (void)c; }
int32_t apu_next_irq(void) {
	if (seq_mode || irq_inhibit) return 0x7fffffff;
	/* o IRQ sai no passo 4: faltam (4 - step) passos */
	int32_t t = next_step; for (int s = step + 1; s < 4; s++) t += (s & 1) ? 7456 : 7457;
	return t;
}

/* passos do sequenciador a cada ~7457 ciclos de CPU */
void apu_run(int32_t now) {
	while (now >= next_step) {
		step++;
		if (seq_mode == 0) {
			quarter();
			if (step == 2 || step == 4) half();
			if (step == 4) { if (!irq_inhibit) { frame_irq = 1; apu_irq = 1; } step = 0; }
		} else {
			if (step != 4) quarter();
			if (step == 2 || step == 5) half();
			if (step == 5) step = 0;
		}
		next_step += (step & 1) ? 7456 : 7457;
		update_out();
	}
}
