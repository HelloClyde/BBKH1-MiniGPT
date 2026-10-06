/* INT8 x INT16 -> INT32, exact two-lane D16MAC. No FP, allocation or service
 * calls. One bounded row/tile per IRQ mask; no entire-matrix critical section.
 * The original integer bound remains: 1408 * 128 * 4095 < INT32_MAX. */
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
.macro terms offset
 xrload 3,4,\offset
 xrload 6,5,(\offset * 2)
 xrload 7,5,(\offset * 2 + 4)
 xr4 0x3d,4,3,3,5 /* duplicate all four weight bytes */
 .word (0x70000037 | (8 << 22) | (5 << 18) | (5 << 14) | (4 << 10) | (4 << 6))
 xr4 0x0a,1,5,6,2 /* low pair, independent accumulator lanes */
 xr4 0x0a,8,4,7,9 /* high pair: no RAW dependency on the preceding MAC */
.endm
.section .text.mg_mxu_dot,"ax",@progbits
.word 0x4d475831 /* independent-test range marker, not executed */
.global mg_mxu_dot
.type mg_mxu_dot,@function
mg_mxu_dot:
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
 xrmove 0x2f,1,0
 xrmove 0x2f,2,0
 xrmove 0x2f,8,0
 xrmove 0x2f,9,0
 andi $t2,$a2,15
 subu $t3,$a2,$t2
 addu $t3,$a0,$t3
 beq $a0,$t3,2f
 nop
1:
 terms 0
 terms 4
 terms 8
 terms 12
 addiu $a0,$a0,16
 bne $a0,$t3,1b
 addiu $a1,$a1,32
2:
 beqz $t2,4f
 nop
3:
 terms 0
 addiu $a0,$a0,4
 addiu $t2,$t2,-4
 bnez $t2,3b
 addiu $a1,$a1,8
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
 lw $t0,48($sp)
 xrmove 0x2f,16,8
 lw $t0,52($sp)
 mtc0 $t0,$12
 nop
 nop
 nop
 jr $ra
 addiu $sp,$sp,64
.size mg_mxu_dot,.-mg_mxu_dot
.word 0x4d475832
