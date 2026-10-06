/* Packed INT4 x INT16 -> INT32, signed D16MAC. count >= 64, count % 16 == 0.
 * Decode nibbles entirely in MXU registers; preserve XR1..11, control and
 * exact CP0 Status. RAM-only leaf, no scratch row or firmware calls. */
.set mips32
.set noreorder
.set noat
.macro xrmove op,xr,gpr
 .word (0x70000000 | (\gpr << 16) | (\xr << 6) | \op)
.endm
.macro xrload xr,gpr,offset
 .word (0x70000010 | (\gpr << 21) | ((\offset / 4) << 10) | (\xr << 6))
.endm
.macro xr4 op,a,b,c,d
 .word (0x70000000 | (\d << 18) | (\c << 14) | (\b << 10) | (\a << 6) | \op)
.endm
.macro terms8 offset
 xrload 3,4,\offset
 xr4 0x3d,4,3,3,5
 /* Shift the low nibble into each signed lane's sign position. */
 .word (0x70000034 | (4 << 22) | (11 << 18) | (4 << 14) | (5 << 10) | (10 << 6))
 .word (0x70000037 | (12 << 22) | (11 << 18) | (11 << 14) | (10 << 10) | (10 << 6))
 .word (0x70000037 | (12 << 22) | (4 << 18) | (4 << 14) | (5 << 10) | (5 << 6))
 xrload 6,5,(\offset * 4)
 xrload 7,5,(\offset * 4 + 4)
 .word (0x7000003d | (3 << 24) | (6 << 18) | (6 << 14) | (7 << 10) | (7 << 6))
 xr4 0x0a,1,10,6,2
 xr4 0x0a,8,5,7,9
 xrload 6,5,(\offset * 4 + 8)
 xrload 7,5,(\offset * 4 + 12)
 .word (0x7000003d | (3 << 24) | (6 << 18) | (6 << 14) | (7 << 10) | (7 << 6))
 xr4 0x0a,1,11,6,2
 xr4 0x0a,8,4,7,9
.endm
.section .text.mg_mxu_q4_dot,"ax",@progbits
.word 0x4d475431 /* independent-test range marker, not executed */
.global mg_mxu_q4_dot
.type mg_mxu_q4_dot,@function
mg_mxu_q4_dot:
 addiu $sp,$sp,-64
 mfc0 $t0,$12
 sw $t0,52($sp)
 srl $t1,$t0,1
 sll $t1,$t1,1
 mtc0 $t1,$12
 nop
 nop
 nop
 xrmove 0x2e,16,8
 sw $t0,48($sp)
 ori $t0,$t0,1
 xrmove 0x2f,16,8
 xrmove 0x2e,1,8
 sw $t0,0($sp)
 xrmove 0x2e,2,8
 sw $t0,4($sp)
 xrmove 0x2e,3,8
 sw $t0,8($sp)
 xrmove 0x2e,4,8
 sw $t0,12($sp)
 xrmove 0x2e,5,8
 sw $t0,16($sp)
 xrmove 0x2e,6,8
 sw $t0,20($sp)
 xrmove 0x2e,7,8
 sw $t0,24($sp)
 xrmove 0x2e,8,8
 sw $t0,28($sp)
 xrmove 0x2e,9,8
 sw $t0,32($sp)
 xrmove 0x2e,10,8
 sw $t0,36($sp)
 xrmove 0x2e,11,8
 sw $t0,40($sp)
 xrmove 0x2f,1,0
 xrmove 0x2f,2,0
 xrmove 0x2f,8,0
 xrmove 0x2f,9,0
 srl $t2,$a2,1
 addu $t3,$a0,$t2
1:
 terms8 0
 terms8 4
 addiu $a0,$a0,8
 bne $a0,$t3,1b
 addiu $a1,$a1,32
4:
 xrmove 0x2e,1,2
 xrmove 0x2e,2,3
 xrmove 0x2e,8,8
 xrmove 0x2e,9,9
 addu $v0,$v0,$v1
 addu $t0,$t0,$t1
 addu $v0,$v0,$t0
 lw $t0,0($sp)
 xrmove 0x2f,1,8
 lw $t0,4($sp)
 xrmove 0x2f,2,8
 lw $t0,8($sp)
 xrmove 0x2f,3,8
 lw $t0,12($sp)
 xrmove 0x2f,4,8
 lw $t0,16($sp)
 xrmove 0x2f,5,8
 lw $t0,20($sp)
 xrmove 0x2f,6,8
 lw $t0,24($sp)
 xrmove 0x2f,7,8
 lw $t0,28($sp)
 xrmove 0x2f,8,8
 lw $t0,32($sp)
 xrmove 0x2f,9,8
 lw $t0,36($sp)
 xrmove 0x2f,10,8
 lw $t0,40($sp)
 xrmove 0x2f,11,8
 lw $t0,48($sp)
 xrmove 0x2f,16,8
 lw $t0,52($sp)
 mtc0 $t0,$12
 nop
 nop
 nop
 jr $ra
 addiu $sp,$sp,64
.size mg_mxu_q4_dot,.-mg_mxu_q4_dot
.word 0x4d475432

/* Two independent 64-term dots sharing the signed INT4 decode. a3 points to
 * two INT32 results. Preserve all XR1..15, control and exact CP0 Status. */
.macro pairmac offset,reg,even_hi,even_lo,odd_hi,odd_lo
 xrload 6,\reg,(\offset * 4)
 xrload 7,\reg,(\offset * 4 + 4)
 .word (0x7000003d | (3 << 24) | (6 << 18) | (6 << 14) | (7 << 10) | (7 << 6))
 xr4 0x0a,\even_hi,10,6,\even_lo
 xr4 0x0a,\odd_hi,5,7,\odd_lo
 xrload 6,\reg,(\offset * 4 + 8)
 xrload 7,\reg,(\offset * 4 + 12)
 .word (0x7000003d | (3 << 24) | (6 << 18) | (6 << 14) | (7 << 10) | (7 << 6))
 xr4 0x0a,\even_hi,11,6,\even_lo
 xr4 0x0a,\odd_hi,4,7,\odd_lo
.endm
.macro pairterms8 offset
 xrload 3,4,\offset
 xr4 0x3d,4,3,3,5
 .word (0x70000034 | (4 << 22) | (11 << 18) | (4 << 14) | (5 << 10) | (10 << 6))
 .word (0x70000037 | (12 << 22) | (11 << 18) | (11 << 14) | (10 << 10) | (10 << 6))
 .word (0x70000037 | (12 << 22) | (4 << 18) | (4 << 14) | (5 << 10) | (5 << 6))
 pairmac \offset,5,1,2,8,9
 pairmac \offset,6,12,13,14,15
.endm
.section .text.mg_mxu_q4_pair,"ax",@progbits
.word 0x4d475433
.global mg_mxu_q4_pair
.type mg_mxu_q4_pair,@function
mg_mxu_q4_pair:
 addiu $sp,$sp,-80
 mfc0 $t0,$12
 sw $t0,68($sp)
 srl $t1,$t0,1
 sll $t1,$t1,1
 mtc0 $t1,$12
 nop
 nop
 nop
 xrmove 0x2e,16,8
 sw $t0,64($sp)
 ori $t0,$t0,1
 xrmove 0x2f,16,8
 xrmove 0x2e,1,8
 sw $t0,0($sp)
 xrmove 0x2e,2,8
 sw $t0,4($sp)
 xrmove 0x2e,3,8
 sw $t0,8($sp)
 xrmove 0x2e,4,8
 sw $t0,12($sp)
 xrmove 0x2e,5,8
 sw $t0,16($sp)
 xrmove 0x2e,6,8
 sw $t0,20($sp)
 xrmove 0x2e,7,8
 sw $t0,24($sp)
 xrmove 0x2e,8,8
 sw $t0,28($sp)
 xrmove 0x2e,9,8
 sw $t0,32($sp)
 xrmove 0x2e,10,8
 sw $t0,36($sp)
 xrmove 0x2e,11,8
 sw $t0,40($sp)
 xrmove 0x2e,12,8
 sw $t0,44($sp)
 xrmove 0x2e,13,8
 sw $t0,48($sp)
 xrmove 0x2e,14,8
 sw $t0,52($sp)
 xrmove 0x2e,15,8
 sw $t0,56($sp)
 xrmove 0x2f,1,0
 xrmove 0x2f,2,0
 xrmove 0x2f,8,0
 xrmove 0x2f,9,0
 xrmove 0x2f,12,0
 xrmove 0x2f,13,0
 xrmove 0x2f,14,0
 xrmove 0x2f,15,0
 addiu $t3,$a0,32
1:
 pairterms8 0
 pairterms8 4
 addiu $a0,$a0,8
 addiu $a1,$a1,32
 bne $a0,$t3,1b
 addiu $a2,$a2,32
 xrmove 0x2e,1,2
 xrmove 0x2e,2,3
 xrmove 0x2e,8,8
 xrmove 0x2e,9,9
 addu $v0,$v0,$v1
 addu $t0,$t0,$t1
 addu $v0,$v0,$t0
 sw $v0,0($a3)
 xrmove 0x2e,12,2
 xrmove 0x2e,13,3
 xrmove 0x2e,14,8
 xrmove 0x2e,15,9
 addu $v0,$v0,$v1
 addu $t0,$t0,$t1
 addu $v0,$v0,$t0
 sw $v0,4($a3)
 lw $t0,0($sp)
 xrmove 0x2f,1,8
 lw $t0,4($sp)
 xrmove 0x2f,2,8
 lw $t0,8($sp)
 xrmove 0x2f,3,8
 lw $t0,12($sp)
 xrmove 0x2f,4,8
 lw $t0,16($sp)
 xrmove 0x2f,5,8
 lw $t0,20($sp)
 xrmove 0x2f,6,8
 lw $t0,24($sp)
 xrmove 0x2f,7,8
 lw $t0,28($sp)
 xrmove 0x2f,8,8
 lw $t0,32($sp)
 xrmove 0x2f,9,8
 lw $t0,36($sp)
 xrmove 0x2f,10,8
 lw $t0,40($sp)
 xrmove 0x2f,11,8
 lw $t0,44($sp)
 xrmove 0x2f,12,8
 lw $t0,48($sp)
 xrmove 0x2f,13,8
 lw $t0,52($sp)
 xrmove 0x2f,14,8
 lw $t0,56($sp)
 xrmove 0x2f,15,8
 lw $t0,64($sp)
 xrmove 0x2f,16,8
 lw $t0,68($sp)
 mtc0 $t0,$12
 nop
 nop
 nop
 jr $ra
 addiu $sp,$sp,80
.size mg_mxu_q4_pair,.-mg_mxu_q4_pair
.word 0x4d475434
