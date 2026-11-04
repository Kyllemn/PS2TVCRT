/* psxnes - 6502 interpretado, pensado para o R3000 do PS1 (sem cache de dados):
 * durante a execucao os registradores do 6502 ficam em variaveis locais (registradores da MIPS);
 * o estado so volta para a struct na saida ou antes de acessar E/S (bus_read/bus_write). */
#include "cpu.h"

Cpu cpu;
int32_t cpu_target;
#ifdef PROFILE
long instr_count, idle_skipped;
long pc_cyc[65536];   /* ciclos gastos por endereco (para achar codigo quente) */
#endif

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

uint8_t cpu_getp(void) {
	return (cpu.nz_n & 0x80) | (cpu.v ? 0x40 : 0) | 0x20 | (cpu.d ? 8 : 0) |
	       (cpu.i ? 4 : 0) | (cpu.nz_z ? 0 : 2) | (cpu.c ? 1 : 0);
}
void cpu_setp(uint8_t p) {
	cpu.nz_n = p & 0x80; cpu.nz_z = (p & 2) ? 0 : 1;
	cpu.v = (p >> 6) & 1; cpu.d = (p >> 3) & 1; cpu.i = (p >> 2) & 1; cpu.c = p & 1;
}
static inline uint8_t rd0(uint16_t a) { uint8_t *p = cpu.rmap[a >> 11]; return p ? p[a] : bus_read(a); }
void cpu_reset(void) {
	cpu.a = cpu.x = cpu.y = 0; cpu.s = 0xfd;
	cpu_setp(0x24);
	cpu.pc = rd0(0xfffc) | (rd0(0xfffd) << 8);
	cpu.cycles += 7;
}

/* ---- macros sobre as variaveis locais de cpu_exec ---- */
#define SAVE()   (cpu.a = A, cpu.x = X, cpu.y = Y, cpu.s = S, cpu.pc = PC, cpu.cycles = CYC, \
                  cpu.nz_n = NZN, cpu.nz_z = NZZ, cpu.c = FC, cpu.v = FV, cpu.i = FI, cpu.d = FD)
#define LOAD()   (A = cpu.a, X = cpu.x, Y = cpu.y, S = cpu.s, PC = cpu.pc, CYC = cpu.cycles, \
                  NZN = cpu.nz_n, NZZ = cpu.nz_z, FC = cpu.c, FV = cpu.v, FI = cpu.i, FD = cpu.d)
#define rd(a_) ({ uint16_t _a = (a_); uint8_t *_p = rmap[_a >> 11]; uint8_t _r; \
                  if (__builtin_expect(_p != 0, 1)) _r = _p[_a]; else { cpu.cycles = CYC; _r = bus_read(_a); CYC = cpu.cycles; } _r; })
#define wr(a_, v_) do { uint16_t _a = (a_); uint8_t _v = (v_); uint8_t *_p = wmap[_a >> 11]; \
                  if (__builtin_expect(_p != 0, 1)) _p[_a] = _v; else { CYC = io_write(_a, _v, CYC); chk = 1; } } while (0)
/* pagina zero e pilha sao sempre a RAM interna */
#define rdz(a_) (ram[(uint8_t)(a_)])
#define wrz(a_, v_) (ram[(uint8_t)(a_)] = (v_))
#define rd16zp(z) ({ uint8_t _z = (z); (uint16_t)(rdz(_z) | (rdz((uint8_t)(_z + 1)) << 8)); })
#define SETNZ(v) (NZN = NZZ = (uint8_t)(v))
#define PUSH(v) (ram[0x100 | S--] = (v))
#define POP() (ram[0x100 | ++S])
#define GETP() ((NZN & 0x80) | (FV ? 0x40 : 0) | 0x20 | (FD ? 8 : 0) | (FI ? 4 : 0) | (NZZ ? 0 : 2) | (FC ? 1 : 0))
#define SETP(p_) do { uint8_t _p = (p_); NZN = _p & 0x80; NZZ = (_p & 2) ? 0 : 1; FV = (_p >> 6) & 1; FD = (_p >> 3) & 1; FI = (_p >> 2) & 1; FC = _p & 1; } while (0)
#define INTERRUPT(vec, brk) do { PUSH(PC >> 8); PUSH(PC & 0xff); PUSH((GETP() & ~0x10) | ((brk) ? 0x10 : 0)); \
                  FI = 1; PC = rd(vec) | (rd((vec) + 1) << 8); CYC += 7; } while (0)
#define OPB()  rd(PC++)
#define IMM()  (PC++)
#define IMMV() rd(PC++)
#define ZP()   (rd(PC++))
#define ZPX()  ((uint8_t)(rd(PC++) + X))
#define ZPY()  ((uint8_t)(rd(PC++) + Y))
#define ABS()  (t = rd(PC) | (rd(PC + 1) << 8), PC += 2, t)
#define absi(r_, pen) ({ uint16_t _b = rd(PC) | (rd(PC + 1) << 8); PC += 2; uint16_t _e = _b + (r_); \
                  if ((pen) && ((_b ^ _e) & 0xff00)) CYC++; _e; })
#define indx() rd16zp((uint8_t)(OPB() + X))
#define indy(pen) ({ uint16_t _b = rd16zp(OPB()), _e = _b + Y; if ((pen) && ((_b ^ _e) & 0xff00)) CYC++; _e; })
#define adc(m_) do { uint8_t _m = (m_); unsigned _s = A + _m + FC; FV = (~(A ^ _m) & (A ^ _s) & 0x80) != 0; \
                  FC = _s > 0xff; A = (uint8_t)_s; SETNZ(A); } while (0)
#define cmp(r_, m_) do { uint8_t _r = (r_), _m = (m_); FC = _r >= _m; SETNZ(_r - _m); } while (0)
#define asl(m_) ({ uint8_t _m = (m_); FC = _m >> 7; _m <<= 1; SETNZ(_m); _m; })
#define lsr(m_) ({ uint8_t _m = (m_); FC = _m & 1; _m >>= 1; SETNZ(_m); _m; })
#define rol(m_) ({ uint8_t _m = (m_), _c = FC; FC = _m >> 7; _m = (_m << 1) | _c; SETNZ(_m); _m; })
#define ror(m_) ({ uint8_t _m = (m_), _c = FC; FC = _m & 1; _m = (_m >> 1) | (_c << 7); SETNZ(_m); _m; })
#define RMWZ(addr, OP) do { uint8_t ea_ = (addr); ram[ea_] = OP(ram[ea_]); } while (0)
#define RMW(addr, OP) do { uint16_t ea_ = (addr); uint8_t m_ = rd(ea_); wr(ea_, m_); m_ = OP(m_); wr(ea_, m_); } while (0)
#ifdef PROFILE
#define SKIP() (idle_skipped += target - CYC, CYC = target)
#else
#ifdef NO_IDLE
#define SKIP() ((void)0)
#else
#define SKIP() (CYC = target)
#endif
#endif
#define branch(cond) do { int8_t _off = (int8_t)OPB(); if (cond) { uint16_t _n = PC + _off; \
                  CYC += ((PC ^ _n) & 0xff00) ? 2 : 1; \
                  if (_off < 0 && _off >= -10 && CYC < target && idle_body(rmap, PC, _n)) SKIP(); PC = _n; } } while (0)

/* laco ocioso: desvio para tras curto cujo corpo so le memoria. Nada muda ate o fim do trecho
 * atual (interrupcoes e PPU so mudam entre trechos), entao o tempo restante pode ser pulado. */
static int32_t __attribute__((noinline)) io_write(uint16_t a, uint8_t v, int32_t cyc) {
	cpu.cycles = cyc; bus_write(a, v); return cpu.cycles;
}
static int idle_body(uint8_t **rmap, uint16_t from, uint16_t to) {
	uint16_t a = to;
	while (a < from - 2) {
		uint8_t *p = rmap[a >> 11]; if (!p) return 0;
		switch (p[a]) {
		case 0xa5: case 0xa6: case 0xa4: case 0x24: case 0xc5: case 0x25: case 0x05: case 0xe4: case 0xc4: a += 2; break;
		case 0xad: case 0xae: case 0xac: case 0x2c: case 0xcd: case 0x2d: case 0x0d: case 0xec: case 0xcc: a += 3; break;
		case 0x29: case 0x09: case 0xc9: case 0xe0: case 0xc0: a += 2; break;
		default: return 0;
		}
	}
	return a == from - 2;
}

/* executa ate CYC >= target (step = 1 instrucao so) */
static void cpu_exec(int32_t target, int single) {
	uint8_t A, X, Y, S, NZN, NZZ, FC, FV, FI, FD;
	uint16_t PC, t, ea; uint8_t m;
	int32_t CYC;
	uint8_t **rmap = cpu.rmap, **wmap = cpu.wmap;
	uint8_t *ram = cpu.rmap[0];
	int chk = 1;
	LOAD();
	do {
		if (chk) {
			chk = 0;
			if (cpu_target < target) target = cpu_target;
			if (cpu.nmi) { cpu.nmi = 0; INTERRUPT(0xfffa, 0); }
			else if (cpu.irq && !FI) INTERRUPT(0xfffe, 0);
		}
		uint8_t op = rd(PC++);
#ifdef PROFILE
		instr_count++; pc_cyc[(uint16_t)(PC - 1)] += cyc[op];
#endif
		CYC += cyc[op];
		switch (op) {
	/* LDA */
	case 0xa9: A = IMMV(); SETNZ(A); break;
	case 0xa5: A = rdz(ZP()); SETNZ(A); break;
	case 0xb5: A = rdz(ZPX()); SETNZ(A); break;
	case 0xad: A = rd(ABS()); SETNZ(A); break;
	case 0xbd: A = rd(absi(X, 1)); SETNZ(A); break;
	case 0xb9: A = rd(absi(Y, 1)); SETNZ(A); break;
	case 0xa1: A = rd(indx()); SETNZ(A); break;
	case 0xb1: A = rd(indy(1)); SETNZ(A); break;
	/* LDX */
	case 0xa2: X = IMMV(); SETNZ(X); break;
	case 0xa6: X = rdz(ZP()); SETNZ(X); break;
	case 0xb6: X = rdz(ZPY()); SETNZ(X); break;
	case 0xae: X = rd(ABS()); SETNZ(X); break;
	case 0xbe: X = rd(absi(Y, 1)); SETNZ(X); break;
	/* LDY */
	case 0xa0: Y = IMMV(); SETNZ(Y); break;
	case 0xa4: Y = rdz(ZP()); SETNZ(Y); break;
	case 0xb4: Y = rdz(ZPX()); SETNZ(Y); break;
	case 0xac: Y = rd(ABS()); SETNZ(Y); break;
	case 0xbc: Y = rd(absi(X, 1)); SETNZ(Y); break;
	/* STA/STX/STY */
	case 0x85: wrz(ZP(), A); break;
	case 0x95: wrz(ZPX(), A); break;
	case 0x8d: wr(ABS(), A); break;
	case 0x9d: wr(absi(X, 0), A); break;
	case 0x99: wr(absi(Y, 0), A); break;
	case 0x81: wr(indx(), A); break;
	case 0x91: wr(indy(0), A); break;
	case 0x86: wrz(ZP(), X); break;
	case 0x96: wrz(ZPY(), X); break;
	case 0x8e: wr(ABS(), X); break;
	case 0x84: wrz(ZP(), Y); break;
	case 0x94: wrz(ZPX(), Y); break;
	case 0x8c: wr(ABS(), Y); break;
	/* transferencias */
	case 0xaa: X = A; SETNZ(X); break;
	case 0xa8: Y = A; SETNZ(Y); break;
	case 0x8a: A = X; SETNZ(A); break;
	case 0x98: A = Y; SETNZ(A); break;
	case 0xba: X = S; SETNZ(X); break;
	case 0x9a: S = X; break;
	/* pilha */
	case 0x48: PUSH(A); break;
	case 0x68: A = POP(); SETNZ(A); break;
	case 0x08: PUSH(GETP() | 0x10); break;
	case 0x28: SETP(POP()); chk = 1; break;
	/* logicas */
	case 0x29: A &= IMMV(); SETNZ(A); break;
	case 0x25: A &= rdz(ZP()); SETNZ(A); break;
	case 0x35: A &= rdz(ZPX()); SETNZ(A); break;
	case 0x2d: A &= rd(ABS()); SETNZ(A); break;
	case 0x3d: A &= rd(absi(X, 1)); SETNZ(A); break;
	case 0x39: A &= rd(absi(Y, 1)); SETNZ(A); break;
	case 0x21: A &= rd(indx()); SETNZ(A); break;
	case 0x31: A &= rd(indy(1)); SETNZ(A); break;
	case 0x09: A |= IMMV(); SETNZ(A); break;
	case 0x05: A |= rdz(ZP()); SETNZ(A); break;
	case 0x15: A |= rdz(ZPX()); SETNZ(A); break;
	case 0x0d: A |= rd(ABS()); SETNZ(A); break;
	case 0x1d: A |= rd(absi(X, 1)); SETNZ(A); break;
	case 0x19: A |= rd(absi(Y, 1)); SETNZ(A); break;
	case 0x01: A |= rd(indx()); SETNZ(A); break;
	case 0x11: A |= rd(indy(1)); SETNZ(A); break;
	case 0x49: A ^= IMMV(); SETNZ(A); break;
	case 0x45: A ^= rdz(ZP()); SETNZ(A); break;
	case 0x55: A ^= rdz(ZPX()); SETNZ(A); break;
	case 0x4d: A ^= rd(ABS()); SETNZ(A); break;
	case 0x5d: A ^= rd(absi(X, 1)); SETNZ(A); break;
	case 0x59: A ^= rd(absi(Y, 1)); SETNZ(A); break;
	case 0x41: A ^= rd(indx()); SETNZ(A); break;
	case 0x51: A ^= rd(indy(1)); SETNZ(A); break;
	/* BIT */
	case 0x24: m = rdz(ZP()); NZN = m; FV = (m >> 6) & 1; NZZ = m & A; break;
	case 0x2c: m = rd(ABS()); NZN = m; FV = (m >> 6) & 1; NZZ = m & A; break;
	/* ADC / SBC */
	case 0x69: adc(IMMV()); break;
	case 0x65: adc(rdz(ZP())); break;
	case 0x75: adc(rdz(ZPX())); break;
	case 0x6d: adc(rd(ABS())); break;
	case 0x7d: adc(rd(absi(X, 1))); break;
	case 0x79: adc(rd(absi(Y, 1))); break;
	case 0x61: adc(rd(indx())); break;
	case 0x71: adc(rd(indy(1))); break;
	case 0xe9: case 0xeb: adc(~IMMV()); break;
	case 0xe5: adc(~rdz(ZP())); break;
	case 0xf5: adc(~rdz(ZPX())); break;
	case 0xed: adc(~rd(ABS())); break;
	case 0xfd: adc(~rd(absi(X, 1))); break;
	case 0xf9: adc(~rd(absi(Y, 1))); break;
	case 0xe1: adc(~rd(indx())); break;
	case 0xf1: adc(~rd(indy(1))); break;
	/* comparacoes */
	case 0xc9: cmp(A, IMMV()); break;
	case 0xc5: cmp(A, rdz(ZP())); break;
	case 0xd5: cmp(A, rdz(ZPX())); break;
	case 0xcd: cmp(A, rd(ABS())); break;
	case 0xdd: cmp(A, rd(absi(X, 1))); break;
	case 0xd9: cmp(A, rd(absi(Y, 1))); break;
	case 0xc1: cmp(A, rd(indx())); break;
	case 0xd1: cmp(A, rd(indy(1))); break;
	case 0xe0: cmp(X, IMMV()); break;
	case 0xe4: cmp(X, rdz(ZP())); break;
	case 0xec: cmp(X, rd(ABS())); break;
	case 0xc0: cmp(Y, IMMV()); break;
	case 0xc4: cmp(Y, rdz(ZP())); break;
	case 0xcc: cmp(Y, rd(ABS())); break;
	/* INC/DEC */
	case 0xe6: ea = ZP(); m = rdz(ea) + 1; wrz(ea, m); SETNZ(m); break;
	case 0xf6: ea = ZPX(); m = rdz(ea) + 1; wrz(ea, m); SETNZ(m); break;
	case 0xee: ea = ABS(); m = rd(ea); wr(ea, m); m++; wr(ea, m); SETNZ(m); break;
	case 0xfe: ea = absi(X, 0); m = rd(ea); wr(ea, m); m++; wr(ea, m); SETNZ(m); break;
	case 0xc6: ea = ZP(); m = rdz(ea) - 1; wrz(ea, m); SETNZ(m); break;
	case 0xd6: ea = ZPX(); m = rdz(ea) - 1; wrz(ea, m); SETNZ(m); break;
	case 0xce: ea = ABS(); m = rd(ea); wr(ea, m); m--; wr(ea, m); SETNZ(m); break;
	case 0xde: ea = absi(X, 0); m = rd(ea); wr(ea, m); m--; wr(ea, m); SETNZ(m); break;
	case 0xe8: X++; SETNZ(X); break;
	case 0xc8: Y++; SETNZ(Y); break;
	case 0xca: X--; SETNZ(X); break;
	case 0x88: Y--; SETNZ(Y); break;
	/* deslocamentos */
	case 0x0a: A = asl(A); break;
	case 0x4a: A = lsr(A); break;
	case 0x2a: A = rol(A); break;
	case 0x6a: A = ror(A); break;
	case 0x06: RMWZ(ZP(), asl); break;
	case 0x16: RMWZ(ZPX(), asl); break;
	case 0x0e: RMW(ABS(), asl); break;
	case 0x1e: RMW(absi(X, 0), asl); break;
	case 0x46: RMWZ(ZP(), lsr); break;
	case 0x56: RMWZ(ZPX(), lsr); break;
	case 0x4e: RMW(ABS(), lsr); break;
	case 0x5e: RMW(absi(X, 0), lsr); break;
	case 0x26: RMWZ(ZP(), rol); break;
	case 0x36: RMWZ(ZPX(), rol); break;
	case 0x2e: RMW(ABS(), rol); break;
	case 0x3e: RMW(absi(X, 0), rol); break;
	case 0x66: RMWZ(ZP(), ror); break;
	case 0x76: RMWZ(ZPX(), ror); break;
	case 0x6e: RMW(ABS(), ror); break;
	case 0x7e: RMW(absi(X, 0), ror); break;
	/* saltos */
	case 0x4c: t = rd(PC) | (rd(PC + 1) << 8);
		if (t == PC - 1 && CYC < target) SKIP();   /* JMP * : ocioso */
		PC = t; break;
	case 0x6c: t = rd(PC) | (rd(PC + 1) << 8);   /* bug da pagina */
		PC = rd(t) | (rd((t & 0xff00) | ((t + 1) & 0xff)) << 8); break;
	case 0x20: t = rd(PC) | (rd(PC + 1) << 8); PC++;
		PUSH(PC >> 8); PUSH(PC & 0xff); PC = t; break;
	case 0x60: t = POP(); t |= POP() << 8; PC = t + 1; break;
	case 0x40: SETP(POP()); t = POP(); t |= POP() << 8; PC = t; chk = 1; break;
	case 0x00: PC++; CYC -= 7; INTERRUPT(0xfffe, 1); break;
	/* desvios */
	case 0x10: branch(!(NZN & 0x80)); break;
	case 0x30: branch(NZN & 0x80); break;
	case 0x50: branch(!FV); break;
	case 0x70: branch(FV); break;
	case 0x90: branch(!FC); break;
	case 0xb0: branch(FC); break;
	case 0xd0: branch(NZZ); break;
	case 0xf0: branch(!NZZ); break;
	/* flags */
	case 0x18: FC = 0; break;
	case 0x38: FC = 1; break;
	case 0x58: FI = 0; chk = 1; break;
	case 0x78: FI = 1; break;
	case 0xb8: FV = 0; break;
	case 0xd8: FD = 0; break;
	case 0xf8: FD = 1; break;
	/* NOPs (oficial e nao oficiais) */
	case 0xea: case 0x1a: case 0x3a: case 0x5a: case 0x7a: case 0xda: case 0xfa: break;
	case 0x80: case 0x82: case 0x89: case 0xc2: case 0xe2: PC++; break;
	case 0x04: case 0x44: case 0x64: PC++; break;
	case 0x14: case 0x34: case 0x54: case 0x74: case 0xd4: case 0xf4: PC++; break;
	case 0x0c: PC += 2; break;
	case 0x1c: case 0x3c: case 0x5c: case 0x7c: case 0xdc: case 0xfc: rd(absi(X, 1)); break;
	/* nao oficiais usados por alguns jogos e pelo nestest */
	case 0xa7: A = X = rdz(ZP()); SETNZ(A); break;           /* LAX */
	case 0xb7: A = X = rdz(ZPY()); SETNZ(A); break;
	case 0xaf: A = X = rd(ABS()); SETNZ(A); break;
	case 0xbf: A = X = rd(absi(Y, 1)); SETNZ(A); break;
	case 0xa3: A = X = rd(indx()); SETNZ(A); break;
	case 0xb3: A = X = rd(indy(1)); SETNZ(A); break;
	case 0x87: wrz(ZP(), A & X); break;                            /* SAX */
	case 0x97: wrz(ZPY(), A & X); break;
	case 0x8f: wr(ABS(), A & X); break;
	case 0x83: wr(indx(), A & X); break;
#define DCP(addr) do { ea = (addr); m = rd(ea) - 1; wr(ea, m); cmp(A, m); } while (0)
#define ISB(addr) do { ea = (addr); m = rd(ea) + 1; wr(ea, m); adc(~m); } while (0)
#define SLO(addr) do { ea = (addr); m = asl(rd(ea)); wr(ea, m); A |= m; SETNZ(A); } while (0)
#define RLA(addr) do { ea = (addr); m = rol(rd(ea)); wr(ea, m); A &= m; SETNZ(A); } while (0)
#define SRE(addr) do { ea = (addr); m = lsr(rd(ea)); wr(ea, m); A ^= m; SETNZ(A); } while (0)
#define RRA(addr) do { ea = (addr); m = ror(rd(ea)); wr(ea, m); adc(m); } while (0)
#define UNOFF(base, MAC) \
	case base+0x07: MAC(ZP()); break; case base+0x17: MAC(ZPX()); break; \
	case base+0x0f: MAC(ABS()); break; case base+0x1f: MAC(absi(X, 0)); break; \
	case base+0x1b: MAC(absi(Y, 0)); break; case base+0x03: MAC(indx()); break; \
	case base+0x13: MAC(indy(0)); break;
	UNOFF(0xc0, DCP)
	UNOFF(0xe0, ISB)
	UNOFF(0x00, SLO)
	UNOFF(0x20, RLA)
	UNOFF(0x40, SRE)
	UNOFF(0x60, RRA)
	case 0x0b: case 0x2b: A &= IMMV(); SETNZ(A); FC = A >> 7; break; /* ANC */
	case 0x4b: A &= IMMV(); A = lsr(A); break;                           /* ALR */
	case 0x6b: A &= IMMV(); A = (A >> 1) | (FC << 7); SETNZ(A);   /* ARR */
		FC = (A >> 6) & 1; FV = ((A >> 6) ^ (A >> 5)) & 1; break;
	case 0xcb: m = A & X; t = m - IMMV(); FC = !(t & 0x100); X = (uint8_t)t; SETNZ(X); break; /* AXS */
	default: break; /* KIL e instaveis: ignorados */
		}
	} while (!single && CYC < target);
	SAVE();
}

void cpu_step(void) { int n = cpu.nmi, i = cpu.irq; cpu.nmi = 0; cpu.irq = 0; cpu_exec(0, 1); cpu.nmi = n; cpu.irq = i; }
#ifndef DYNAREC
void cpu_run(int32_t target) { cpu_target = target; if (cpu.cycles < target) cpu_exec(target, 0); }
#endif
/* uma instrucao interpretada (atende interrupcao pendente antes); usada pelo dynarec */
void cpu_interp_one(void) { cpu_exec(cpu.cycles + 1, 1); }
