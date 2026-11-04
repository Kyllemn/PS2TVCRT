# psxnes - dynarec: entrada, encadeamento e saida dos blocos traduzidos.
# Registradores fixos dentro do codigo gerado:
#   s0=A s1=X s2=Y s3=Z(valor) s4=N(valor) s5=C s6=ciclos restantes s7=base da RAM do NES fp=S
	.set noreorder
	.set noat
	.text

# deslocamentos na struct Cpu (conferidos em dynarec.c com _Static_assert)
#define O_A 0
#define O_X 1
#define O_Y 2
#define O_S 3
#define O_PC 4
#define O_C 6
#define O_NZN 10
#define O_NZZ 11
#define O_CYC 12
#define O_RMAP 20

	.global dyn_enter
	.type dyn_enter, @function
# void dyn_enter(void *code)
dyn_enter:
	addiu $sp, $sp, -48
	sw $ra, 44($sp)
	sw $fp, 40($sp)
	sw $s7, 36($sp)
	sw $s6, 32($sp)
	sw $s5, 28($sp)
	sw $s4, 24($sp)
	sw $s3, 20($sp)
	sw $s2, 16($sp)
	sw $s1, 12($sp)
	sw $s0, 8($sp)
	lui $t0, %hi(cpu)
	addiu $t0, $t0, %lo(cpu)
	lbu $s0, O_A($t0)
	lbu $s1, O_X($t0)
	lbu $s2, O_Y($t0)
	lbu $fp, O_S($t0)
	lbu $s3, O_NZZ($t0)
	lbu $s4, O_NZN($t0)
	lbu $s5, O_C($t0)
	lw $t2, O_CYC($t0)
	lui $t1, %hi(dyn_target)
	lw $t1, %lo(dyn_target)($t1)
	lui $s7, %hi(nes_ram)
	addiu $s7, $s7, %lo(nes_ram)
	jr $a0
	subu $s6, $t1, $t2

# v0 = proximo PC do 6502. Continua direto no proximo bloco se ja traduzido e ainda ha ciclos.
	.global dyn_next
dyn_next:
	blez $s6, dyn_out
	srl $t1, $v0, 11
	lui $t0, %hi(cpu + O_RMAP)
	addiu $t0, $t0, %lo(cpu + O_RMAP)
	sll $t1, $t1, 2
	addu $t1, $t1, $t0
	lw $t1, 0($t1)
	srl $t5, $v0, 15          # pc < $8000 (RAM/E/S): volta ao C
	beqz $t5, dyn_out
	nop
	beqz $t1, dyn_out
	addu $t1, $t1, $v0
	# etiqueta = (endereco & 0x1fffff) | ((pc >> 13) << 21)
	sll $t1, $t1, 11
	srl $t1, $t1, 11
	srl $t2, $v0, 13
	sll $t2, $t2, 21
	or $t1, $t1, $t2
	srl $t2, $t1, 14
	xor $t2, $t2, $t1
	andi $t2, $t2, 0x3fff
	sll $t2, $t2, 3
	lui $t3, %hi(dyn_table)
	addiu $t3, $t3, %lo(dyn_table)
	addu $t2, $t2, $t3
	lw $t3, 0($t2)
	lw $t4, 4($t2)
	bne $t3, $t1, dyn_out
	nop
	jr $t4
	nop

# v0 = PC. Grava o estado de volta na struct e retorna ao C.
	.global dyn_out
dyn_out:
	lui $t0, %hi(cpu)
	addiu $t0, $t0, %lo(cpu)
	sb $s0, O_A($t0)
	sb $s1, O_X($t0)
	sb $s2, O_Y($t0)
	sb $fp, O_S($t0)
	sb $s3, O_NZZ($t0)
	sb $s4, O_NZN($t0)
	sb $s5, O_C($t0)
	sh $v0, O_PC($t0)
	lui $t1, %hi(dyn_target)
	lw $t1, %lo(dyn_target)($t1)
	nop
	subu $t1, $t1, $s6
	sw $t1, O_CYC($t0)
	lw $ra, 44($sp)
	lw $fp, 40($sp)
	lw $s7, 36($sp)
	lw $s6, 32($sp)
	lw $s5, 28($sp)
	lw $s4, 24($sp)
	lw $s3, 20($sp)
	lw $s2, 16($sp)
	lw $s1, 12($sp)
	lw $s0, 8($sp)
	jr $ra
	addiu $sp, $sp, 48
