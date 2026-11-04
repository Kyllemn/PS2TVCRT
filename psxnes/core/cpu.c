/* psxnes - 6502 interpretado, pensado para o R3000 do PS1:
 * registradores num struct global (acesso por $gp), flags preguicosas,
 * leitura por tabela de paginas e um switch unico. */
#include "cpu.h"

Cpu cpu;
static int32_t run_target;

static inline uint8_t rd(uint16_t a) {
	uint8_t *p = cpu.rmap[a >> 11];
	return p ? p[a] : bus_read(a);
}
static inline void wr(uint16_t a, uint8_t v) {
	uint8_t *p = cpu.wmap[a >> 11];
	if (p) p[a] = v; else bus_write(a, v);
}
static inline uint16_t rd16zp(uint8_t a) { return rd(a) | (rd((uint8_t)(a + 1)) << 8); }

#define SETNZ(v) (cpu.nz_n = cpu.nz_z = (uint8_t)(v))
#define PUSH(v) wr(0x100 | cpu.s--, (v))
#define POP() rd(0x100 | ++cpu.s)

uint8_t cpu_getp(void) {
	return (cpu.nz_n & 0x80) | (cpu.v ? 0x40 : 0) | 0x20 | (cpu.d ? 8 : 0) |
	       (cpu.i ? 4 : 0) | (cpu.nz_z ? 0 : 2) | (cpu.c ? 1 : 0);
}
void cpu_setp(uint8_t p) {
	cpu.nz_n = p & 0x80; cpu.nz_z = (p & 2) ? 0 : 1;
	cpu.v = (p >> 6) & 1; cpu.d = (p >> 3) & 1; cpu.i = (p >> 2) & 1; cpu.c = p & 1;
}

void cpu_reset(void) {
	cpu.a = cpu.x = cpu.y = 0; cpu.s = 0xfd;
	cpu_setp(0x24);
	cpu.pc = rd(0xfffc) | (rd(0xfffd) << 8);
	cpu.cycles += 7;
}

static void interrupt(uint16_t vec, int brk) {
	PUSH(cpu.pc >> 8); PUSH(cpu.pc & 0xff);
	PUSH((cpu_getp() & ~0x10) | (brk ? 0x10 : 0));
	cpu.i = 1;
	cpu.pc = rd(vec) | (rd(vec + 1) << 8);
	cpu.cycles += 7;
}

static const uint8_t cyc[256] = {
/*      0 1 2 3 4 5 6 7 8 9 A B C D E F */
/*0*/   7,6,2,8,3,3,5,5,3,2,2,2,4,4,6,6,
/*1*/   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*2*/   6,6,2,8,3,3,5,5,4,2,2,2,4,4,6,6,
/*3*/   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*4*/   6,6,2,8,3,3,5,5,3,2,2,2,3,4,6,6,
/*5*/   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*6*/   6,6,2,8,3,3,5,5,4,2,2,2,5,4,6,6,
/*7*/   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*8*/   2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
/*9*/   2,6,2,6,4,4,4,4,2,5,2,5,5,5,5,5,
/*A*/   2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4,
/*B*/   2,5,2,5,4,4,4,4,2,4,2,4,4,4,4,4,
/*C*/   2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
/*D*/   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
/*E*/   2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6,
/*F*/   2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
};

/* enderecamentos; os de leitura somam 1 ciclo ao cruzar pagina (pen=1) */
#define IMM()  (cpu.pc++)
#define ZP()   (rd(cpu.pc++))
#define ZPX()  ((uint8_t)(rd(cpu.pc++) + cpu.x))
#define ZPY()  ((uint8_t)(rd(cpu.pc++) + cpu.y))
#define ABS()  (t = rd(cpu.pc) | (rd(cpu.pc + 1) << 8), cpu.pc += 2, t)
static inline uint16_t absi(uint8_t r, int pen) {
	uint16_t b = rd(cpu.pc) | (rd(cpu.pc + 1) << 8); cpu.pc += 2;
	uint16_t e = b + r;
	if (pen && ((b ^ e) & 0xff00)) cpu.cycles++;
	return e;
}
static inline uint16_t indx(void) { return rd16zp((uint8_t)(rd(cpu.pc++) + cpu.x)); }
static inline uint16_t indy(int pen) {
	uint16_t b = rd16zp(rd(cpu.pc++)), e = b + cpu.y;
	if (pen && ((b ^ e) & 0xff00)) cpu.cycles++;
	return e;
}

static inline void adc(uint8_t m) {
	unsigned s = cpu.a + m + cpu.c;
	cpu.v = (~(cpu.a ^ m) & (cpu.a ^ s) & 0x80) != 0;
	cpu.c = s > 0xff; cpu.a = (uint8_t)s; SETNZ(cpu.a);
}
static inline void cmp(uint8_t r, uint8_t m) { unsigned d = r - m; cpu.c = r >= m; SETNZ(d); }
static inline uint8_t asl(uint8_t m) { cpu.c = m >> 7; m <<= 1; SETNZ(m); return m; }
static inline uint8_t lsr(uint8_t m) { cpu.c = m & 1; m >>= 1; SETNZ(m); return m; }
static inline uint8_t rol(uint8_t m) { uint8_t c = cpu.c; cpu.c = m >> 7; m = (m << 1) | c; SETNZ(m); return m; }
static inline uint8_t ror(uint8_t m) { uint8_t c = cpu.c; cpu.c = m & 1; m = (m >> 1) | (c << 7); SETNZ(m); return m; }

/* laco ocioso: desvio para tras de ate 8 bytes cujo corpo so le memoria (sem escrever nada).
 * Nada muda ate o fim do trecho atual de execucao (interrupcoes e a PPU so mudam entre trechos),
 * entao o tempo restante pode ser pulado. */
static int idle_body(uint16_t from, uint16_t to) {
	uint16_t a = to;
	while (a < from - 2) {
		uint8_t op = rd(a);
		switch (op) {
		case 0xa5: case 0xa6: case 0xa4: case 0x24: case 0xc5: case 0x25: case 0x05: case 0xe4: case 0xc4: a += 2; break;
		case 0xad: case 0xae: case 0xac: case 0x2c: case 0xcd: case 0x2d: case 0x0d: case 0xec: case 0xcc: a += 3; break;
		case 0x29: case 0x09: case 0xc9: case 0xe0: case 0xc0: a += 2; break;
		default: return 0;
		}
	}
	return a == from - 2;
}
static inline void branch(int cond) {
	int8_t off = (int8_t)rd(cpu.pc++);
	if (cond) {
		uint16_t n = cpu.pc + off;
		cpu.cycles += ((cpu.pc ^ n) & 0xff00) ? 2 : 1;
		if (off < 0 && off >= -10 && cpu.cycles < run_target && idle_body(cpu.pc, n)) cpu.cycles = run_target;
		cpu.pc = n;
	}
}

/* leitura-modificacao-escrita com escrita dupla (como o 6502 real) */
#define RMW(addr, OP) do { uint16_t ea = (addr); uint8_t m = rd(ea); wr(ea, m); m = OP(m); wr(ea, m); } while (0)

void cpu_step(void) {
	uint16_t t, ea; uint8_t m;
	uint8_t op = rd(cpu.pc++);
	cpu.cycles += cyc[op];
	switch (op) {
	/* LDA */
	case 0xa9: cpu.a = rd(IMM()); SETNZ(cpu.a); break;
	case 0xa5: cpu.a = rd(ZP()); SETNZ(cpu.a); break;
	case 0xb5: cpu.a = rd(ZPX()); SETNZ(cpu.a); break;
	case 0xad: cpu.a = rd(ABS()); SETNZ(cpu.a); break;
	case 0xbd: cpu.a = rd(absi(cpu.x, 1)); SETNZ(cpu.a); break;
	case 0xb9: cpu.a = rd(absi(cpu.y, 1)); SETNZ(cpu.a); break;
	case 0xa1: cpu.a = rd(indx()); SETNZ(cpu.a); break;
	case 0xb1: cpu.a = rd(indy(1)); SETNZ(cpu.a); break;
	/* LDX */
	case 0xa2: cpu.x = rd(IMM()); SETNZ(cpu.x); break;
	case 0xa6: cpu.x = rd(ZP()); SETNZ(cpu.x); break;
	case 0xb6: cpu.x = rd(ZPY()); SETNZ(cpu.x); break;
	case 0xae: cpu.x = rd(ABS()); SETNZ(cpu.x); break;
	case 0xbe: cpu.x = rd(absi(cpu.y, 1)); SETNZ(cpu.x); break;
	/* LDY */
	case 0xa0: cpu.y = rd(IMM()); SETNZ(cpu.y); break;
	case 0xa4: cpu.y = rd(ZP()); SETNZ(cpu.y); break;
	case 0xb4: cpu.y = rd(ZPX()); SETNZ(cpu.y); break;
	case 0xac: cpu.y = rd(ABS()); SETNZ(cpu.y); break;
	case 0xbc: cpu.y = rd(absi(cpu.x, 1)); SETNZ(cpu.y); break;
	/* STA/STX/STY */
	case 0x85: wr(ZP(), cpu.a); break;
	case 0x95: wr(ZPX(), cpu.a); break;
	case 0x8d: wr(ABS(), cpu.a); break;
	case 0x9d: wr(absi(cpu.x, 0), cpu.a); break;
	case 0x99: wr(absi(cpu.y, 0), cpu.a); break;
	case 0x81: wr(indx(), cpu.a); break;
	case 0x91: wr(indy(0), cpu.a); break;
	case 0x86: wr(ZP(), cpu.x); break;
	case 0x96: wr(ZPY(), cpu.x); break;
	case 0x8e: wr(ABS(), cpu.x); break;
	case 0x84: wr(ZP(), cpu.y); break;
	case 0x94: wr(ZPX(), cpu.y); break;
	case 0x8c: wr(ABS(), cpu.y); break;
	/* transferencias */
	case 0xaa: cpu.x = cpu.a; SETNZ(cpu.x); break;
	case 0xa8: cpu.y = cpu.a; SETNZ(cpu.y); break;
	case 0x8a: cpu.a = cpu.x; SETNZ(cpu.a); break;
	case 0x98: cpu.a = cpu.y; SETNZ(cpu.a); break;
	case 0xba: cpu.x = cpu.s; SETNZ(cpu.x); break;
	case 0x9a: cpu.s = cpu.x; break;
	/* pilha */
	case 0x48: PUSH(cpu.a); break;
	case 0x68: cpu.a = POP(); SETNZ(cpu.a); break;
	case 0x08: PUSH(cpu_getp() | 0x10); break;
	case 0x28: cpu_setp(POP()); break;
	/* logicas */
	case 0x29: cpu.a &= rd(IMM()); SETNZ(cpu.a); break;
	case 0x25: cpu.a &= rd(ZP()); SETNZ(cpu.a); break;
	case 0x35: cpu.a &= rd(ZPX()); SETNZ(cpu.a); break;
	case 0x2d: cpu.a &= rd(ABS()); SETNZ(cpu.a); break;
	case 0x3d: cpu.a &= rd(absi(cpu.x, 1)); SETNZ(cpu.a); break;
	case 0x39: cpu.a &= rd(absi(cpu.y, 1)); SETNZ(cpu.a); break;
	case 0x21: cpu.a &= rd(indx()); SETNZ(cpu.a); break;
	case 0x31: cpu.a &= rd(indy(1)); SETNZ(cpu.a); break;
	case 0x09: cpu.a |= rd(IMM()); SETNZ(cpu.a); break;
	case 0x05: cpu.a |= rd(ZP()); SETNZ(cpu.a); break;
	case 0x15: cpu.a |= rd(ZPX()); SETNZ(cpu.a); break;
	case 0x0d: cpu.a |= rd(ABS()); SETNZ(cpu.a); break;
	case 0x1d: cpu.a |= rd(absi(cpu.x, 1)); SETNZ(cpu.a); break;
	case 0x19: cpu.a |= rd(absi(cpu.y, 1)); SETNZ(cpu.a); break;
	case 0x01: cpu.a |= rd(indx()); SETNZ(cpu.a); break;
	case 0x11: cpu.a |= rd(indy(1)); SETNZ(cpu.a); break;
	case 0x49: cpu.a ^= rd(IMM()); SETNZ(cpu.a); break;
	case 0x45: cpu.a ^= rd(ZP()); SETNZ(cpu.a); break;
	case 0x55: cpu.a ^= rd(ZPX()); SETNZ(cpu.a); break;
	case 0x4d: cpu.a ^= rd(ABS()); SETNZ(cpu.a); break;
	case 0x5d: cpu.a ^= rd(absi(cpu.x, 1)); SETNZ(cpu.a); break;
	case 0x59: cpu.a ^= rd(absi(cpu.y, 1)); SETNZ(cpu.a); break;
	case 0x41: cpu.a ^= rd(indx()); SETNZ(cpu.a); break;
	case 0x51: cpu.a ^= rd(indy(1)); SETNZ(cpu.a); break;
	/* BIT */
	case 0x24: m = rd(ZP()); cpu.nz_n = m; cpu.v = (m >> 6) & 1; cpu.nz_z = m & cpu.a; break;
	case 0x2c: m = rd(ABS()); cpu.nz_n = m; cpu.v = (m >> 6) & 1; cpu.nz_z = m & cpu.a; break;
	/* ADC / SBC */
	case 0x69: adc(rd(IMM())); break;
	case 0x65: adc(rd(ZP())); break;
	case 0x75: adc(rd(ZPX())); break;
	case 0x6d: adc(rd(ABS())); break;
	case 0x7d: adc(rd(absi(cpu.x, 1))); break;
	case 0x79: adc(rd(absi(cpu.y, 1))); break;
	case 0x61: adc(rd(indx())); break;
	case 0x71: adc(rd(indy(1))); break;
	case 0xe9: case 0xeb: adc(~rd(IMM())); break;
	case 0xe5: adc(~rd(ZP())); break;
	case 0xf5: adc(~rd(ZPX())); break;
	case 0xed: adc(~rd(ABS())); break;
	case 0xfd: adc(~rd(absi(cpu.x, 1))); break;
	case 0xf9: adc(~rd(absi(cpu.y, 1))); break;
	case 0xe1: adc(~rd(indx())); break;
	case 0xf1: adc(~rd(indy(1))); break;
	/* comparacoes */
	case 0xc9: cmp(cpu.a, rd(IMM())); break;
	case 0xc5: cmp(cpu.a, rd(ZP())); break;
	case 0xd5: cmp(cpu.a, rd(ZPX())); break;
	case 0xcd: cmp(cpu.a, rd(ABS())); break;
	case 0xdd: cmp(cpu.a, rd(absi(cpu.x, 1))); break;
	case 0xd9: cmp(cpu.a, rd(absi(cpu.y, 1))); break;
	case 0xc1: cmp(cpu.a, rd(indx())); break;
	case 0xd1: cmp(cpu.a, rd(indy(1))); break;
	case 0xe0: cmp(cpu.x, rd(IMM())); break;
	case 0xe4: cmp(cpu.x, rd(ZP())); break;
	case 0xec: cmp(cpu.x, rd(ABS())); break;
	case 0xc0: cmp(cpu.y, rd(IMM())); break;
	case 0xc4: cmp(cpu.y, rd(ZP())); break;
	case 0xcc: cmp(cpu.y, rd(ABS())); break;
	/* INC/DEC */
	case 0xe6: ea = ZP(); m = rd(ea) + 1; wr(ea, m); SETNZ(m); break;
	case 0xf6: ea = ZPX(); m = rd(ea) + 1; wr(ea, m); SETNZ(m); break;
	case 0xee: ea = ABS(); m = rd(ea); wr(ea, m); m++; wr(ea, m); SETNZ(m); break;
	case 0xfe: ea = absi(cpu.x, 0); m = rd(ea); wr(ea, m); m++; wr(ea, m); SETNZ(m); break;
	case 0xc6: ea = ZP(); m = rd(ea) - 1; wr(ea, m); SETNZ(m); break;
	case 0xd6: ea = ZPX(); m = rd(ea) - 1; wr(ea, m); SETNZ(m); break;
	case 0xce: ea = ABS(); m = rd(ea); wr(ea, m); m--; wr(ea, m); SETNZ(m); break;
	case 0xde: ea = absi(cpu.x, 0); m = rd(ea); wr(ea, m); m--; wr(ea, m); SETNZ(m); break;
	case 0xe8: cpu.x++; SETNZ(cpu.x); break;
	case 0xc8: cpu.y++; SETNZ(cpu.y); break;
	case 0xca: cpu.x--; SETNZ(cpu.x); break;
	case 0x88: cpu.y--; SETNZ(cpu.y); break;
	/* deslocamentos */
	case 0x0a: cpu.a = asl(cpu.a); break;
	case 0x4a: cpu.a = lsr(cpu.a); break;
	case 0x2a: cpu.a = rol(cpu.a); break;
	case 0x6a: cpu.a = ror(cpu.a); break;
	case 0x06: RMW(ZP(), asl); break;
	case 0x16: RMW(ZPX(), asl); break;
	case 0x0e: RMW(ABS(), asl); break;
	case 0x1e: RMW(absi(cpu.x, 0), asl); break;
	case 0x46: RMW(ZP(), lsr); break;
	case 0x56: RMW(ZPX(), lsr); break;
	case 0x4e: RMW(ABS(), lsr); break;
	case 0x5e: RMW(absi(cpu.x, 0), lsr); break;
	case 0x26: RMW(ZP(), rol); break;
	case 0x36: RMW(ZPX(), rol); break;
	case 0x2e: RMW(ABS(), rol); break;
	case 0x3e: RMW(absi(cpu.x, 0), rol); break;
	case 0x66: RMW(ZP(), ror); break;
	case 0x76: RMW(ZPX(), ror); break;
	case 0x6e: RMW(ABS(), ror); break;
	case 0x7e: RMW(absi(cpu.x, 0), ror); break;
	/* saltos */
	case 0x4c: t = rd(cpu.pc) | (rd(cpu.pc + 1) << 8);
		if (t == cpu.pc - 1 && cpu.cycles < run_target) cpu.cycles = run_target;   /* JMP * : ocioso */
		cpu.pc = t; break;
	case 0x6c: t = rd(cpu.pc) | (rd(cpu.pc + 1) << 8);   /* bug da pagina */
		cpu.pc = rd(t) | (rd((t & 0xff00) | ((t + 1) & 0xff)) << 8); break;
	case 0x20: t = rd(cpu.pc) | (rd(cpu.pc + 1) << 8); cpu.pc++;
		PUSH(cpu.pc >> 8); PUSH(cpu.pc & 0xff); cpu.pc = t; break;
	case 0x60: t = POP(); t |= POP() << 8; cpu.pc = t + 1; break;
	case 0x40: cpu_setp(POP()); t = POP(); t |= POP() << 8; cpu.pc = t; break;
	case 0x00: cpu.pc++; cpu.cycles -= 7; interrupt(0xfffe, 1); break;
	/* desvios */
	case 0x10: branch(!(cpu.nz_n & 0x80)); break;
	case 0x30: branch(cpu.nz_n & 0x80); break;
	case 0x50: branch(!cpu.v); break;
	case 0x70: branch(cpu.v); break;
	case 0x90: branch(!cpu.c); break;
	case 0xb0: branch(cpu.c); break;
	case 0xd0: branch(cpu.nz_z); break;
	case 0xf0: branch(!cpu.nz_z); break;
	/* flags */
	case 0x18: cpu.c = 0; break;
	case 0x38: cpu.c = 1; break;
	case 0x58: cpu.i = 0; break;
	case 0x78: cpu.i = 1; break;
	case 0xb8: cpu.v = 0; break;
	case 0xd8: cpu.d = 0; break;
	case 0xf8: cpu.d = 1; break;
	/* NOPs (oficial e nao oficiais) */
	case 0xea: case 0x1a: case 0x3a: case 0x5a: case 0x7a: case 0xda: case 0xfa: break;
	case 0x80: case 0x82: case 0x89: case 0xc2: case 0xe2: cpu.pc++; break;
	case 0x04: case 0x44: case 0x64: cpu.pc++; break;
	case 0x14: case 0x34: case 0x54: case 0x74: case 0xd4: case 0xf4: cpu.pc++; break;
	case 0x0c: cpu.pc += 2; break;
	case 0x1c: case 0x3c: case 0x5c: case 0x7c: case 0xdc: case 0xfc: rd(absi(cpu.x, 1)); break;
	/* nao oficiais usados por alguns jogos e pelo nestest */
	case 0xa7: cpu.a = cpu.x = rd(ZP()); SETNZ(cpu.a); break;           /* LAX */
	case 0xb7: cpu.a = cpu.x = rd(ZPY()); SETNZ(cpu.a); break;
	case 0xaf: cpu.a = cpu.x = rd(ABS()); SETNZ(cpu.a); break;
	case 0xbf: cpu.a = cpu.x = rd(absi(cpu.y, 1)); SETNZ(cpu.a); break;
	case 0xa3: cpu.a = cpu.x = rd(indx()); SETNZ(cpu.a); break;
	case 0xb3: cpu.a = cpu.x = rd(indy(1)); SETNZ(cpu.a); break;
	case 0x87: wr(ZP(), cpu.a & cpu.x); break;                            /* SAX */
	case 0x97: wr(ZPY(), cpu.a & cpu.x); break;
	case 0x8f: wr(ABS(), cpu.a & cpu.x); break;
	case 0x83: wr(indx(), cpu.a & cpu.x); break;
#define DCP(addr) do { ea = (addr); m = rd(ea) - 1; wr(ea, m); cmp(cpu.a, m); } while (0)
#define ISB(addr) do { ea = (addr); m = rd(ea) + 1; wr(ea, m); adc(~m); } while (0)
#define SLO(addr) do { ea = (addr); m = asl(rd(ea)); wr(ea, m); cpu.a |= m; SETNZ(cpu.a); } while (0)
#define RLA(addr) do { ea = (addr); m = rol(rd(ea)); wr(ea, m); cpu.a &= m; SETNZ(cpu.a); } while (0)
#define SRE(addr) do { ea = (addr); m = lsr(rd(ea)); wr(ea, m); cpu.a ^= m; SETNZ(cpu.a); } while (0)
#define RRA(addr) do { ea = (addr); m = ror(rd(ea)); wr(ea, m); adc(m); } while (0)
#define UNOFF(base, MAC) \
	case base+0x07: MAC(ZP()); break; case base+0x17: MAC(ZPX()); break; \
	case base+0x0f: MAC(ABS()); break; case base+0x1f: MAC(absi(cpu.x, 0)); break; \
	case base+0x1b: MAC(absi(cpu.y, 0)); break; case base+0x03: MAC(indx()); break; \
	case base+0x13: MAC(indy(0)); break;
	UNOFF(0xc0, DCP)
	UNOFF(0xe0, ISB)
	UNOFF(0x00, SLO)
	UNOFF(0x20, RLA)
	UNOFF(0x40, SRE)
	UNOFF(0x60, RRA)
	case 0x0b: case 0x2b: cpu.a &= rd(IMM()); SETNZ(cpu.a); cpu.c = cpu.a >> 7; break; /* ANC */
	case 0x4b: cpu.a &= rd(IMM()); cpu.a = lsr(cpu.a); break;                           /* ALR */
	case 0x6b: cpu.a &= rd(IMM()); cpu.a = (cpu.a >> 1) | (cpu.c << 7); SETNZ(cpu.a);   /* ARR */
		cpu.c = (cpu.a >> 6) & 1; cpu.v = ((cpu.a >> 6) ^ (cpu.a >> 5)) & 1; break;
	case 0xcb: m = cpu.a & cpu.x; t = m - rd(IMM()); cpu.c = !(t & 0x100); cpu.x = (uint8_t)t; SETNZ(cpu.x); break; /* AXS */
	default: break; /* KIL e instaveis: ignorados */
	}
}

void cpu_run(int32_t target) {
	run_target = target;
	while (cpu.cycles < target) {
		if (cpu.nmi) { cpu.nmi = 0; interrupt(0xfffa, 0); }
		else if (cpu.irq && !cpu.i) interrupt(0xfffe, 0);
		cpu_step();
	}
}
