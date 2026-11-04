/* psxnes - dynarec 6502 -> MIPS R3000 (PS1).
 * Traduz blocos de codigo da ROM (>= $8000) para codigo nativo. Os blocos sao indexados pelo
 * endereco real do byte na ROM (nunca mudam: ROM nao e gravavel), entao a troca de bancos do
 * MMC3 nao invalida nada. Instrucoes raras ou complicadas voltam ao interpretador (cpu.c).
 * Registradores: s0=A s1=X s2=Y s3=Z s4=N s5=C s6=ciclos restantes s7=RAM fp=S (ver dyn.S). */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <psxapi.h>
#include "../core/cpu.h"
#include "../core/nes.h"
#ifdef NES_PROF
#include <hwregs_c.h>
#define PT() TIMER_VALUE(2)
volatile uint32_t prof_trans, prof_nat, prof_int;
#define PROF(var, stmt) { uint16_t _a = PT(); stmt; var += (uint16_t)(PT() - _a); }
#else
#define PROF(var, stmt) { stmt; }
#endif

_Static_assert(offsetof(Cpu, a) == 0 && offsetof(Cpu, x) == 1 && offsetof(Cpu, y) == 2 && offsetof(Cpu, s) == 3, "cpu");
_Static_assert(offsetof(Cpu, pc) == 4 && offsetof(Cpu, c) == 6 && offsetof(Cpu, nz_n) == 10 && offsetof(Cpu, nz_z) == 11, "cpu");
_Static_assert(offsetof(Cpu, cycles) == 12 && offsetof(Cpu, rmap) == 20, "cpu");

void dyn_enter(void *code);
void dyn_next(void);
void dyn_out(void);
int32_t dyn_target;

/* etiqueta = endereco fisico do byte na ROM (21 bits) | janela de 8 KB do 6502 (bits 21-23):
 * o mesmo banco mapeado em $8000 e em $A000 gera blocos diferentes (os PCs embutidos mudam). */
typedef struct { uint32_t tag; uint32_t *code; } Ent;
#define TBL_BITS 14
Ent dyn_table[1 << TBL_BITS];
static inline uint32_t mk_tag(uint8_t *key, uint16_t pc) { return ((uint32_t)key & 0x1fffff) | ((uint32_t)(pc >> 13) << 21); }
static inline Ent *slot(uint32_t tag) { return &dyn_table[(tag ^ (tag >> 14)) & ((1 << TBL_BITS) - 1)]; }

#define CODE_WORDS (208 * 1024)         /* 832 KB de codigo gerado */
static uint32_t code_buf[CODE_WORDS];
static uint32_t *cp;                    /* ponteiro de emissao */
static uint32_t *cend;
uint32_t dbg_op[256];
volatile uint32_t dbg_r[6]; volatile uint16_t dbg_ring[256]; volatile uint32_t dbg_ri;
volatile uint32_t dbg_enter, dbg_trans, dbg_interp, dbg_pc, dbg_wrap;

/* ---------------- emissor MIPS ---------------- */
enum { ZERO = 0, AT = 1, V0 = 2, V1 = 3, A0 = 4, A1 = 5, T0 = 8, T1 = 9, T2 = 10, T3 = 11, T4 = 12,
       RA_ = 16, RX = 17, RY = 18, RZ = 19, RN = 20, RC = 21, RCYC = 22, RRAM = 23, RS = 30, RRA = 31 };
#define E(w) (*cp++ = (w))
#define R(rs, rt, rd, sa, fn) E(((rs) << 21) | ((rt) << 16) | ((rd) << 11) | ((sa) << 6) | (fn))
#define I(op, rs, rt, imm) E(((op) << 26) | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xffff))
#define NOP()            E(0)
#define ADDU(d, s, t)    R(s, t, d, 0, 0x21)
#define SUBU(d, s, t)    R(s, t, d, 0, 0x23)
#define AND(d, s, t)     R(s, t, d, 0, 0x24)
#define OR(d, s, t)      R(s, t, d, 0, 0x25)
#define XOR(d, s, t)     R(s, t, d, 0, 0x26)
#define SLTU(d, s, t)    R(s, t, d, 0, 0x2b)
#define SLL(d, t, sa)    R(0, t, d, sa, 0x00)
#define SRL(d, t, sa)    R(0, t, d, sa, 0x02)
#define JR(s)            R(s, 0, 0, 0, 0x08)
#define MOVE(d, s)       ADDU(d, s, ZERO)
#define ADDIU(t, s, i)   I(0x09, s, t, i)
#define SLTIU(t, s, i)   I(0x0b, s, t, i)
#define ANDI(t, s, i)    I(0x0c, s, t, i)
#define ORI(t, s, i)     I(0x0d, s, t, i)
#define XORI(t, s, i)    I(0x0e, s, t, i)
#define LUI(t, i)        I(0x0f, 0, t, i)
#define LBU(t, off, s)   I(0x24, s, t, off)
#define LW(t, off, s)    I(0x23, s, t, off)
#define SB(t, off, s)    I(0x28, s, t, off)
#define BEQ(s, t, off)   I(0x04, s, t, off)
#define BNE(s, t, off)   I(0x05, s, t, off)
#define BLEZ(s, off)     I(0x06, s, 0, off)
#define J(addr)          E((0x02 << 26) | (((uint32_t)(addr) >> 2) & 0x3ffffff))
#define JAL(addr)        E((0x03 << 26) | (((uint32_t)(addr) >> 2) & 0x3ffffff))

static void li(int r, uint32_t v) {
	if (v < 0x10000) ORI(r, ZERO, v);
	else { LUI(r, v >> 16); if (v & 0xffff) ORI(r, r, v & 0xffff); }
}
/* endereco absoluto: lui r,%hi ; e devolve %lo para usar como deslocamento */
/* devolve o deslocamento e poe em HB o registrador base: s7 (RAM do NES) se o alvo esta a menos
 * de 32 KB dela (1 instrucao a menos), senao 'r' carregado com lui */
static int HB;
static int16_t hi_lo(int r, const void *p) {
	int32_t d = (int32_t)((uint32_t)p - (uint32_t)nes_ram);
	if (d >= -32768 && d < 32768) { HB = RRAM; return (int16_t)d; }
	uint32_t a = (uint32_t)p; uint16_t lo = a & 0xffff; uint32_t hi = (a + 0x8000) >> 16;
	LUI(r, hi); HB = r; return (int16_t)lo;
}
static uint32_t *branch_here(void) { return cp; }
static void patch_branch(uint32_t *at, uint32_t *to) { *at = (*at & 0xffff0000) | ((to - at - 1) & 0xffff); }

/* ---------------- ajudantes chamados pelo codigo gerado (ABI C) ---------------- */
/* 'rem' = ciclos restantes (s6) ja descontado o bloco ate esta instrucao: da o ciclo exato do acesso */
static uint8_t h_read(uint32_t a, int32_t rem) {
	uint8_t *p = cpu.rmap[a >> 11]; if (p) return p[a];
	cpu.cycles = dyn_target - rem; return bus_read(a);
}
/* devolve os ciclos extras (DMA) nos bits 0-15 e, no bit 16, "o bloco precisa sair": interrupcao
 * pendente, alvo da CPU reduzido, ou a janela de 8 KB do proprio bloco trocou de banco.
 * 'win' = endereco do inicio do bloco. */
static int32_t h_write(uint32_t a, uint32_t v, int32_t rem, uint32_t win) {
	uint8_t *p = cpu.wmap[a >> 11];
	if (p) { p[a] = v; return 0; }
	int32_t c0 = dyn_target - rem; cpu.cycles = c0;
	uint8_t *before = cpu.rmap[win >> 11];
	bus_write(a, v);
	int32_t r = cpu.cycles - c0;
	if (cpu.nmi || (cpu.irq && !cpu.i) || cpu_target < dyn_target || cpu.rmap[win >> 11] != before) r |= 0x10000;
	return r;
}
static uint32_t h_jmpind(uint32_t t) { return h_read(t, 0) | (h_read((t & 0xff00) | ((t + 1) & 0xff), 0) << 8); }

/* ---------------- emissao de acessos ---------------- */
static int blk_cycles;   /* ciclos do bloco ate a instrucao atual (inclusive) */
#define A2_ 6
#define A3_ 7
static uint16_t blk_pc0;
/* le o byte do endereco 16 bits em 'ra' para o registrador 'rd' (rd deve ser T3 ou outro t) */
static void emit_read_dyn(int rd, int ra) {
	/* t1 = rmap[a>>11]; if (!t1) h_read(a) */
	SRL(T1, ra, 11); SLL(T1, T1, 2);
	int16_t lo = hi_lo(T2, &cpu.rmap[0]); ADDU(T1, T1, HB); LW(T1, lo, T1); NOP();
	uint32_t *b1 = branch_here(); BEQ(T1, ZERO, 0); ADDU(T1, T1, ra);
	LBU(rd, 0, T1);
	uint32_t *b2 = branch_here(); BEQ(ZERO, ZERO, 0); NOP();
	patch_branch(b1, cp);
	MOVE(A0, ra); JAL(h_read); ADDIU(A1, RCYC, -blk_cycles); MOVE(rd, V0);
	patch_branch(b2, cp);
}
/* leitura de endereco conhecido na traducao */
static void emit_read_abs(int rd, uint16_t a) {
	if (a < 0x2000) { LBU(rd, a & 0x7ff, RRAM); NOP(); return; }
	if (a >= 0x6000 && a < 0x8000 && cpu.rmap[a >> 11]) { int16_t lo = hi_lo(T1, cart.prgram + (a - 0x6000)); LBU(rd, lo, HB); NOP(); return; }
	if (a >= 0x8000) {   /* ROM: banco pode mudar -> consulta a tabela em tempo de execucao */
		int16_t lo = hi_lo(T1, &cpu.rmap[a >> 11]); LW(T1, lo, HB); li(T2, a); ADDU(T1, T1, T2); LBU(rd, 0, T1); NOP(); return;
	}
	li(A0, a); JAL(h_read); ADDIU(A1, RCYC, -blk_cycles); MOVE(rd, V0);
}

/* fim de bloco: v0 = proximo pc, consome ciclos e segue para dyn_next (ou dyn_out) */
static void emit_exit(uint16_t pc, int extra, int to_c) {
	li(V0, pc);
	if (to_c) J(dyn_out); else J(dyn_next);
	ADDIU(RCYC, RCYC, -(blk_cycles + extra));
}
/* depois de h_write: desconta ciclos extras; se pedido, sai para o C no proximo pc */
static void emit_after_write(uint16_t next_pc) {
	SRL(T1, V0, 16); ANDI(V0, V0, 0xffff); SUBU(RCYC, RCYC, V0);
	uint32_t *b = branch_here(); BEQ(T1, ZERO, 0); NOP();
	emit_exit(next_pc, 0, 1);
	patch_branch(b, cp);
}
static void emit_exit_idle(uint16_t pc) { li(V0, pc); J(dyn_out); MOVE(RCYC, ZERO); }

#define SETNZ(r) (MOVE(RZ, r), MOVE(RN, r))

static const uint8_t cyc[256] = {
	7,6,2,8,3,3,5,5,3,2,2,2,4,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
	6,6,2,8,3,3,5,5,4,2,2,2,4,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
	6,6,2,8,3,3,5,5,3,2,2,2,3,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
	6,6,2,8,3,3,5,5,4,2,2,2,5,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
	2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4, 2,6,2,6,4,4,4,4,2,5,2,5,5,5,5,5,
	2,6,2,6,3,3,3,3,2,2,2,2,4,4,4,4, 2,5,2,5,4,4,4,4,2,4,2,4,4,4,4,4,
	2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
	2,6,2,8,3,3,5,5,2,2,2,2,4,4,6,6, 2,5,2,8,4,4,6,6,2,4,2,7,4,4,7,7,
};

/* modos de enderecamento */
enum { M_IMP, M_IMM, M_ZP, M_ZPX, M_ZPY, M_ABS, M_ABX, M_ABY, M_INX, M_INY, M_REL, M_IND };
static const uint8_t len_of[] = { 1, 2, 2, 2, 2, 3, 3, 3, 2, 2, 2, 3 };

static uint8_t rd6502(uint16_t a) { uint8_t *p = cpu.rmap[a >> 11]; return p ? p[a] : 0; }

/* calcula o endereco efetivo (modos indexados/indiretos) em T0 */
static void emit_ea(int mode, uint16_t op16) {
	switch (mode) {
	case M_ZPX: ADDIU(T0, RX, op16 & 0xff); ANDI(T0, T0, 0xff); break;
	case M_ZPY: ADDIU(T0, RY, op16 & 0xff); ANDI(T0, T0, 0xff); break;
	case M_ABX: li(T0, op16); ADDU(T0, T0, RX); ANDI(T0, T0, 0xffff); break;
	case M_ABY: li(T0, op16); ADDU(T0, T0, RY); ANDI(T0, T0, 0xffff); break;
	case M_INX: ADDIU(T1, RX, op16 & 0xff); ANDI(T1, T1, 0xff); ADDU(T2, T1, RRAM); LBU(T0, 0, T2);
		ADDIU(T1, T1, 1); ANDI(T1, T1, 0xff); ADDU(T2, T1, RRAM); LBU(T1, 0, T2); NOP(); SLL(T1, T1, 8); OR(T0, T0, T1); break;
	case M_INY: { uint8_t z = op16 & 0xff; LBU(T0, z, RRAM); LBU(T1, (uint8_t)(z + 1), RRAM); NOP();
		SLL(T1, T1, 8); OR(T0, T0, T1); ADDU(T0, T0, RY); ANDI(T0, T0, 0xffff); break; }
	}
}
/* le o operando de uma instrucao de leitura para T3 */
static void emit_load_operand(int mode, uint16_t op16) {
	switch (mode) {
	case M_IMM: li(T3, op16 & 0xff); break;
	case M_ZP: LBU(T3, op16 & 0xff, RRAM); NOP(); break;
	case M_ABS: emit_read_abs(T3, op16); break;
	case M_ZPX: case M_ZPY: emit_ea(mode, op16); ADDU(T0, T0, RRAM); LBU(T3, 0, T0); NOP(); break;
	default: emit_ea(mode, op16); emit_read_dyn(T3, T0); break;
	}
}

/* escrita do registrador 'rv'. Devolve 1 se o bloco precisa terminar (E/S ou mapper). */
static int emit_store(int mode, uint16_t op16, int rv, uint16_t next_pc) {
	if (mode == M_ZP) { SB(rv, op16 & 0xff, RRAM); return 0; }
	if (mode == M_ZPX || mode == M_ZPY) { emit_ea(mode, op16); ADDU(T0, T0, RRAM); SB(rv, 0, T0); return 0; }
	if (mode == M_ABS) {
		if (op16 < 0x2000) { SB(rv, op16 & 0x7ff, RRAM); return 0; }
		if (op16 >= 0x6000 && op16 < 0x8000 && cpu.wmap[op16 >> 11]) { int16_t lo = hi_lo(T1, cart.prgram + (op16 - 0x6000)); SB(rv, lo, HB); return 0; }
		li(A3_, blk_pc0); li(A0, op16); MOVE(A1, rv); JAL(h_write); ADDIU(A2_, RCYC, -blk_cycles);
		emit_after_write(next_pc);
		return 0;
	}
	/* indexado/indireto: caminho rapido pela wmap; caminho lento chama h_write e sai do bloco */
	emit_ea(mode, op16);
	SRL(T1, T0, 11); SLL(T1, T1, 2);
	int16_t lo = hi_lo(T2, &cpu.wmap[0]); ADDU(T1, T1, HB); LW(T1, lo, T1); NOP();
	uint32_t *b1 = branch_here(); BEQ(T1, ZERO, 0); ADDU(T1, T1, T0);
	SB(rv, 0, T1);
	uint32_t *b2 = branch_here(); BEQ(ZERO, ZERO, 0); NOP();
	patch_branch(b1, cp);
	li(A3_, blk_pc0); MOVE(A0, T0); MOVE(A1, rv); JAL(h_write); ADDIU(A2_, RCYC, -blk_cycles);
	emit_after_write(next_pc);
	patch_branch(b2, cp);
	return 0;
}

static void emit_adc(int rm) {   /* A = A + rm + C ; V, C, NZ */
	ADDU(T4, RA_, rm); ADDU(T4, T4, RC);
	XOR(T1, RA_, rm); XORI(T1, T1, 0xff); XOR(T2, RA_, T4); AND(T1, T1, T2); SRL(T1, T1, 7); ANDI(T1, T1, 1);
	int16_t lo = hi_lo(T2, &cpu.v); SB(T1, lo, HB);
	SRL(RC, T4, 8); ANDI(RA_, T4, 0xff); SETNZ(RA_);
}
static void emit_cmp(int rr, int rm) { SUBU(T1, rr, rm); ANDI(T1, T1, 0xff); SETNZ(T1); SLTU(RC, rr, rm); XORI(RC, RC, 1); }

/* ---------------- tabela de opcodes suportados ---------------- */
typedef struct { uint8_t kind, mode; } Op;
enum { K_NONE, K_LDA, K_LDX, K_LDY, K_STA, K_STX, K_STY, K_ADC, K_SBC, K_AND, K_ORA, K_EOR, K_CMP, K_CPX, K_CPY,
       K_BIT, K_INCM, K_DECM, K_ASLM, K_LSRM, K_ROLM, K_RORM, K_ASLA, K_LSRA, K_ROLA, K_RORA,
       K_INX, K_INY, K_DEX, K_DEY, K_TAX, K_TAY, K_TXA, K_TYA, K_TSX, K_TXS, K_PHA, K_PLA, K_PHP,
       K_CLC, K_SEC, K_CLV, K_SEI, K_CLI, K_CLD, K_SED, K_NOP, K_JMP, K_JMPI, K_JSR, K_RTS, K_BR };
static Op ops[256];
static void def(int op, int k, int m) { ops[op].kind = k; ops[op].mode = m; }
static void init_ops(void) {
	static const uint8_t grp[8] = { M_INX, M_ZP, M_IMM, M_ABS, M_INY, M_ZPX, M_ABY, M_ABX };
	/* grupo 1 (cc=01) */
	static const uint8_t g1[8] = { K_ORA, K_AND, K_EOR, K_ADC, K_STA, K_LDA, K_CMP, K_SBC };
	for (int a = 0; a < 8; a++) for (int b = 0; b < 8; b++) {
		int op = (a << 5) | (b << 2) | 1;
		if (g1[a] == K_STA && grp[b] == M_IMM) continue;
		def(op, g1[a], grp[b]);
	}
	def(0xa2, K_LDX, M_IMM); def(0xa6, K_LDX, M_ZP); def(0xb6, K_LDX, M_ZPY); def(0xae, K_LDX, M_ABS); def(0xbe, K_LDX, M_ABY);
	def(0xa0, K_LDY, M_IMM); def(0xa4, K_LDY, M_ZP); def(0xb4, K_LDY, M_ZPX); def(0xac, K_LDY, M_ABS); def(0xbc, K_LDY, M_ABX);
	def(0x86, K_STX, M_ZP); def(0x96, K_STX, M_ZPY); def(0x8e, K_STX, M_ABS);
	def(0x84, K_STY, M_ZP); def(0x94, K_STY, M_ZPX); def(0x8c, K_STY, M_ABS);
	def(0xe0, K_CPX, M_IMM); def(0xe4, K_CPX, M_ZP); def(0xec, K_CPX, M_ABS);
	def(0xc0, K_CPY, M_IMM); def(0xc4, K_CPY, M_ZP); def(0xcc, K_CPY, M_ABS);
	def(0x24, K_BIT, M_ZP); def(0x2c, K_BIT, M_ABS);
	/* memoria RMW: so pagina zero e absoluto (RAM); o resto volta ao interpretador */
	def(0xe6, K_INCM, M_ZP); def(0xf6, K_INCM, M_ZPX); def(0xee, K_INCM, M_ABS);
	def(0xc6, K_DECM, M_ZP); def(0xd6, K_DECM, M_ZPX); def(0xce, K_DECM, M_ABS);
	def(0x06, K_ASLM, M_ZP); def(0x16, K_ASLM, M_ZPX); def(0x0e, K_ASLM, M_ABS);
	def(0x46, K_LSRM, M_ZP); def(0x56, K_LSRM, M_ZPX); def(0x4e, K_LSRM, M_ABS);
	def(0x26, K_ROLM, M_ZP); def(0x36, K_ROLM, M_ZPX); def(0x2e, K_ROLM, M_ABS);
	def(0x66, K_RORM, M_ZP); def(0x76, K_RORM, M_ZPX); def(0x6e, K_RORM, M_ABS);
	def(0x0a, K_ASLA, M_IMP); def(0x4a, K_LSRA, M_IMP); def(0x2a, K_ROLA, M_IMP); def(0x6a, K_RORA, M_IMP);
	def(0xe8, K_INX, M_IMP); def(0xc8, K_INY, M_IMP); def(0xca, K_DEX, M_IMP); def(0x88, K_DEY, M_IMP);
	def(0xaa, K_TAX, M_IMP); def(0xa8, K_TAY, M_IMP); def(0x8a, K_TXA, M_IMP); def(0x98, K_TYA, M_IMP);
	def(0xba, K_TSX, M_IMP); def(0x9a, K_TXS, M_IMP);
	def(0x48, K_PHA, M_IMP); def(0x68, K_PLA, M_IMP); def(0x08, K_PHP, M_IMP);
	def(0x18, K_CLC, M_IMP); def(0x38, K_SEC, M_IMP); def(0xb8, K_CLV, M_IMP); def(0x78, K_SEI, M_IMP);
	def(0x58, K_CLI, M_IMP); def(0xd8, K_CLD, M_IMP); def(0xf8, K_SED, M_IMP); def(0xea, K_NOP, M_IMP);
	def(0x4c, K_JMP, M_ABS); def(0x6c, K_JMPI, M_IND); def(0x20, K_JSR, M_ABS); def(0x60, K_RTS, M_IMP);
	for (int i = 0; i < 8; i++) def(0x10 + i * 0x20, K_BR, M_REL);
}

/* corpo so de leituras (para detectar laco ocioso) */
static int readonly_kind(int k) {
	return k == K_LDA || k == K_LDX || k == K_LDY || k == K_CMP || k == K_CPX || k == K_CPY || k == K_BIT ||
	       k == K_AND || k == K_ORA || k == K_EOR || k == K_NOP;
}

/* RMW em memoria: carrega em T3, aplica, grava */
static void emit_rmw(int k, int mode, uint16_t op16) {
	int base; int16_t off = 0;
	if (mode == M_ZP) { base = RRAM; off = op16 & 0xff; }
	else if (mode == M_ZPX) { emit_ea(M_ZPX, op16); ADDU(T0, T0, RRAM); base = T0; }
	else if (op16 < 0x2000) { base = RRAM; off = op16 & 0x7ff; }
	else { off = hi_lo(T0, cart.prgram + (op16 - 0x6000)); base = HB; }
	LBU(T3, off, base); NOP();
	switch (k) {
	case K_INCM: ADDIU(T3, T3, 1); ANDI(T3, T3, 0xff); break;
	case K_DECM: ADDIU(T3, T3, -1); ANDI(T3, T3, 0xff); break;
	case K_ASLM: SRL(RC, T3, 7); SLL(T3, T3, 1); ANDI(T3, T3, 0xff); break;
	case K_LSRM: ANDI(RC, T3, 1); SRL(T3, T3, 1); break;
	case K_ROLM: SLL(T3, T3, 1); OR(T3, T3, RC); SRL(RC, T3, 8); ANDI(T3, T3, 0xff); break;
	case K_RORM: SLL(T1, RC, 8); OR(T1, T1, T3); ANDI(RC, T3, 1); SRL(T3, T1, 1); break;
	}
	SB(T3, off, base); SETNZ(T3);
}

/* traduz um bloco que comeca em pc; devolve NULL se a primeira instrucao nao e suportada */
static uint32_t *translate(uint16_t pc0) {
	if (cend - cp < 4096) {      /* buffer cheio: recomeca tudo */
		cp = code_buf; memset(dyn_table, 0, sizeof dyn_table);
		FlushCache();             /* so aqui: codigo novo em endereco nunca executado nao tem linha velha no cache */
		dbg_wrap++;
	}
	/* cada bloco comeca numa linha nova do cache de instrucoes (16 bytes): a linha do fim do bloco
	 * anterior pode ter sido carregada com palavras ainda nao escritas, e nunca e executada de novo */
	while ((uint32_t)cp & 15) *cp++ = 0;
	uint32_t *start = cp;
	uint16_t pc = pc0;
	blk_cycles = 0; blk_pc0 = pc0;
	int n = 0;
	for (;;) {
		uint8_t opc = rd6502(pc);
		Op o = ops[opc];
		int len = len_of[o.mode];
		/* limites: tipo nao suportado, bloco longo, instrucao atravessando banco de 8 KB */
		if (o.kind == K_NONE || n >= 48 || ((pc ^ (pc + len - 1)) & 0xe000) || (n && ((pc ^ pc0) & 0xe000))) {
			if (n == 0) return 0;
			emit_exit(pc, 0, o.kind == K_NONE);   /* volta ao C para interpretar o raro */
			break;
		}
		uint16_t op16 = len >= 2 ? rd6502(pc + 1) | (len == 3 ? rd6502(pc + 2) << 8 : 0) : 0;
		uint16_t next = pc + len;
		blk_cycles += cyc[opc];
		n++;
		int k = o.kind, m = o.mode, end = 0;
		switch (k) {
		case K_LDA: emit_load_operand(m, op16); MOVE(RA_, T3); SETNZ(RA_); break;
		case K_LDX: emit_load_operand(m, op16); MOVE(RX, T3); SETNZ(RX); break;
		case K_LDY: emit_load_operand(m, op16); MOVE(RY, T3); SETNZ(RY); break;
		case K_AND: emit_load_operand(m, op16); AND(RA_, RA_, T3); SETNZ(RA_); break;
		case K_ORA: emit_load_operand(m, op16); OR(RA_, RA_, T3); SETNZ(RA_); break;
		case K_EOR: emit_load_operand(m, op16); XOR(RA_, RA_, T3); SETNZ(RA_); break;
		case K_ADC: emit_load_operand(m, op16); emit_adc(T3); break;
		case K_SBC: emit_load_operand(m, op16); XORI(T3, T3, 0xff); emit_adc(T3); break;
		case K_CMP: emit_load_operand(m, op16); emit_cmp(RA_, T3); break;
		case K_CPX: emit_load_operand(m, op16); emit_cmp(RX, T3); break;
		case K_CPY: emit_load_operand(m, op16); emit_cmp(RY, T3); break;
		case K_BIT: { emit_load_operand(m, op16); MOVE(RN, T3); AND(RZ, T3, RA_);
			SRL(T1, T3, 6); ANDI(T1, T1, 1); int16_t lo = hi_lo(T2, &cpu.v); SB(T1, lo, HB); break; }
		case K_STA: end = emit_store(m, op16, RA_, next); break;
		case K_STX: end = emit_store(m, op16, RX, next); break;
		case K_STY: end = emit_store(m, op16, RY, next); break;
		case K_INCM: case K_DECM: case K_ASLM: case K_LSRM: case K_ROLM: case K_RORM:
			if (m == M_ABS && !(op16 < 0x2000 || (op16 >= 0x6000 && op16 < 0x8000 && cpu.wmap[op16 >> 11]))) {
				blk_cycles -= cyc[opc]; n--;
				if (n == 0) return 0;
				emit_exit(pc, 0, 1); end = 2; break;
			}
			emit_rmw(k, m, op16); break;
		case K_ASLA: SRL(RC, RA_, 7); SLL(RA_, RA_, 1); ANDI(RA_, RA_, 0xff); SETNZ(RA_); break;
		case K_LSRA: ANDI(RC, RA_, 1); SRL(RA_, RA_, 1); SETNZ(RA_); break;
		case K_ROLA: SLL(RA_, RA_, 1); OR(RA_, RA_, RC); SRL(RC, RA_, 8); ANDI(RA_, RA_, 0xff); SETNZ(RA_); break;
		case K_RORA: SLL(T1, RC, 8); OR(T1, T1, RA_); ANDI(RC, RA_, 1); SRL(RA_, T1, 1); SETNZ(RA_); break;
		case K_INX: ADDIU(RX, RX, 1); ANDI(RX, RX, 0xff); SETNZ(RX); break;
		case K_INY: ADDIU(RY, RY, 1); ANDI(RY, RY, 0xff); SETNZ(RY); break;
		case K_DEX: ADDIU(RX, RX, -1); ANDI(RX, RX, 0xff); SETNZ(RX); break;
		case K_DEY: ADDIU(RY, RY, -1); ANDI(RY, RY, 0xff); SETNZ(RY); break;
		case K_TAX: MOVE(RX, RA_); SETNZ(RX); break;
		case K_TAY: MOVE(RY, RA_); SETNZ(RY); break;
		case K_TXA: MOVE(RA_, RX); SETNZ(RA_); break;
		case K_TYA: MOVE(RA_, RY); SETNZ(RA_); break;
		case K_TSX: MOVE(RX, RS); SETNZ(RX); break;
		case K_TXS: MOVE(RS, RX); break;
		case K_PHA: ADDU(T0, RRAM, RS); SB(RA_, 0x100, T0); ADDIU(RS, RS, -1); ANDI(RS, RS, 0xff); break;
		case K_PLA: ADDIU(RS, RS, 1); ANDI(RS, RS, 0xff); ADDU(T0, RRAM, RS); LBU(RA_, 0x100, T0); NOP(); SETNZ(RA_); break;
		case K_PHP: {  /* P = N V 1 1 D I Z C */
			ANDI(T3, RN, 0x80); ORI(T3, T3, 0x30); OR(T3, T3, RC);
			SLTIU(T1, RZ, 1); SLL(T1, T1, 1); OR(T3, T3, T1);
			int16_t lo = hi_lo(T2, &cpu.v); LBU(T1, lo, HB); NOP(); SLL(T1, T1, 6); OR(T3, T3, T1);
			lo = hi_lo(T2, &cpu.d); LBU(T1, lo, HB); NOP(); SLL(T1, T1, 3); OR(T3, T3, T1);
			lo = hi_lo(T2, &cpu.i); LBU(T1, lo, HB); NOP(); SLL(T1, T1, 2); OR(T3, T3, T1);
			ADDU(T0, RRAM, RS); SB(T3, 0x100, T0); ADDIU(RS, RS, -1); ANDI(RS, RS, 0xff); break; }
		case K_CLC: MOVE(RC, ZERO); break;
		case K_SEC: ORI(RC, ZERO, 1); break;
		case K_CLV: { int16_t lo = hi_lo(T2, &cpu.v); SB(ZERO, lo, HB); break; }
		case K_SEI: { ORI(T1, ZERO, 1); int16_t lo = hi_lo(T2, &cpu.i); SB(T1, lo, HB); break; }
		case K_CLI: { int16_t lo = hi_lo(T2, &cpu.i); SB(ZERO, lo, HB); emit_exit(next, 0, 1); end = 2; break; }
		case K_CLD: { int16_t lo = hi_lo(T2, &cpu.d); SB(ZERO, lo, HB); break; }
		case K_SED: { ORI(T1, ZERO, 1); int16_t lo = hi_lo(T2, &cpu.d); SB(T1, lo, HB); break; }
		case K_NOP: break;
		case K_JMP:
#ifndef NO_IDLE
			if (op16 == pc) { emit_exit_idle(pc); end = 2; break; }
#endif
			emit_exit(op16, 0, 0); end = 2; break;
		case K_JMPI: li(A0, op16); JAL(h_jmpind); NOP(); MOVE(T0, V0); MOVE(V0, T0); J(dyn_next); ADDIU(RCYC, RCYC, -blk_cycles); end = 2; break;
		case K_JSR: { uint16_t ret = pc + 2;
			ADDU(T0, RRAM, RS); li(T1, ret >> 8); SB(T1, 0x100, T0); ADDIU(RS, RS, -1); ANDI(RS, RS, 0xff);
			ADDU(T0, RRAM, RS); li(T1, ret & 0xff); SB(T1, 0x100, T0); ADDIU(RS, RS, -1); ANDI(RS, RS, 0xff);
			emit_exit(op16, 0, 0); end = 2; break; }
		case K_RTS:
			ADDIU(RS, RS, 1); ANDI(RS, RS, 0xff); ADDU(T0, RRAM, RS); LBU(T1, 0x100, T0);
			ADDIU(RS, RS, 1); ANDI(RS, RS, 0xff); ADDU(T0, RRAM, RS); LBU(T2, 0x100, T0); NOP();
			SLL(T2, T2, 8); OR(V0, T1, T2); ADDIU(V0, V0, 1); ANDI(V0, V0, 0xffff);
			J(dyn_next); ADDIU(RCYC, RCYC, -blk_cycles); end = 2; break;
		case K_BR: {
			uint16_t tgt = next + (int8_t)(op16 & 0xff);
			int cond = opc >> 6, want = (opc >> 5) & 1;
			/* t1 = flag: 0=N 1=V 2=C 3=Z */
			switch (cond) {
			case 0: ANDI(T1, RN, 0x80); break;
			case 1: { int16_t lo = hi_lo(T2, &cpu.v); LBU(T1, lo, HB); NOP(); break; }
			case 2: MOVE(T1, RC); break;
			case 3: SLTIU(T1, RZ, 1); break;
			}
			uint32_t *b = branch_here();
			if (want) BNE(T1, ZERO, 0); else BEQ(T1, ZERO, 0);
			NOP();
			emit_exit(next, 0, 0);                         /* nao tomado */
			patch_branch(b, cp);
			int extra = 1 + (((next ^ tgt) & 0xff00) ? 1 : 0);
			/* laco ocioso: desvio para o inicio do proprio bloco, corpo so de leituras */
			int idle = 0;
#ifndef NO_IDLE
			if (tgt == pc0 && pc - pc0 <= 10) {
				idle = 1;
				for (uint16_t a = pc0; a < pc; ) { Op q = ops[rd6502(a)]; if (!readonly_kind(q.kind) || (q.mode == M_ABS && 0)) { idle = 0; break; } a += len_of[q.mode]; }
			}
#endif
			if (idle) emit_exit_idle(tgt); else emit_exit(tgt, extra, 0);
			end = 2; break; }
		}
		if (end == 1) { emit_exit(next, 0, 1); break; }   /* escreveu em E/S/mapper: volta ao C */
		if (end == 2) break;
		pc = next;
	}
	return start;
}

/* ---------------- despachante ---------------- */
static uint32_t *lookup(uint32_t tag) { Ent *e = slot(tag); return e->tag == tag ? e->code : 0; }
static void insert(uint32_t tag, uint32_t *code) { Ent *e = slot(tag); e->tag = tag; e->code = code; }

static int inited;
void dyn_init(void) { init_ops(); cp = code_buf; cend = code_buf + CODE_WORDS; memset(dyn_table, 0, sizeof dyn_table); FlushCache(); inited = 1; }

void cpu_run(int32_t target) {
	if (!inited) dyn_init();
	cpu_target = target;
	while (cpu.cycles < cpu_target) {
		if (cpu.nmi || (cpu.irq && !cpu.i)) { dbg_interp++; PROF(prof_int, cpu_interp_one()); continue; }
		uint16_t pc = cpu.pc;
		uint8_t *p = cpu.rmap[pc >> 11];
		if (pc >= 0x8000 && p) {
			uint32_t tag = mk_tag(p + pc, pc);
			Ent *e = slot(tag);
			uint32_t *code = e->tag == tag ? e->code : 0;
			/* bit 31 na etiqueta: "comeca com opcode nao traduzivel" (dyn_next nunca casa com ela) */
			if (!code && e->tag != (tag | 0x80000000u)) {
				dbg_trans++; PROF(prof_trans, code = translate(pc));
				if (code) insert(tag, code); else { e->tag = tag | 0x80000000u; e->code = 0; }
			}
			if (code) { dbg_enter++; dbg_pc = pc; dyn_target = cpu_target; PROF(prof_nat, dyn_enter(code));
#ifdef NES_PROF
				{ uint16_t q = cpu.pc; uint8_t *pp = cpu.rmap[q >> 11];
				  if (cpu.cycles >= cpu_target) dbg_r[0]++; else if (cpu.nmi || (cpu.irq && !cpu.i)) dbg_r[1]++;
				  else if (q < 0x8000 || !pp) dbg_r[2]++; else { uint32_t tg = mk_tag(pp + q, q); Ent *e2 = slot(tg);
				    if (e2->tag == tg) { dbg_r[3]++; dbg_ring[dbg_ri++ & 255] = q; } else if (e2->tag == (tg | 0x80000000u)) dbg_r[4]++; else dbg_r[5]++; } }
#endif
				continue; }
		}
		dbg_interp++; dbg_op[rd6502(cpu.pc)]++; PROF(prof_int, cpu_interp_one());
	}
}
