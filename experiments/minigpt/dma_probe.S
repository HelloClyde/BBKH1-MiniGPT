/* Preserve every volatile MIPS32 register plus HI/LO across the RAM-only job.
 * Replaces exactly LUI v0,b302 + LW v0,a8(v0) at 0x8004755c.
 * Calls the job once per DMA chunk, then polls without repeating the callback.
 * Callee-saved registers are preserved by the C ABI. No FPU on this target. */
    .set noreorder
    .set noat
    .text
    .globl dma_probe_trampoline
    .type dma_probe_trampoline,@function
dma_probe_trampoline:
    addiu $sp,$sp,-128
    sw $at,16($sp)
    sw $v0,20($sp)
    sw $v1,24($sp)
    sw $a0,28($sp)
    sw $a1,32($sp)
    sw $a2,36($sp)
    sw $a3,40($sp)
    sw $t0,44($sp)
    sw $t1,48($sp)
    sw $t2,52($sp)
    sw $t3,56($sp)
    sw $t4,60($sp)
    sw $t5,64($sp)
    sw $t6,68($sp)
    sw $t7,72($sp)
    sw $t8,76($sp)
    sw $t9,80($sp)
    sw $ra,84($sp)
    sw $gp,88($sp)
    mfhi $t0
    sw $t0,92($sp)
    mflo $t0
    sw $t0,96($sp)
    jal dma_probe_work
    nop
    lw $t0,92($sp)
    mthi $t0
    lw $t0,96($sp)
    mtlo $t0
    lw $at,16($sp)
    lw $v0,20($sp)
    lw $v1,24($sp)
    lw $a0,28($sp)
    lw $a1,32($sp)
    lw $a2,36($sp)
    lw $a3,40($sp)
    lw $t0,44($sp)
    lw $t1,48($sp)
    lw $t2,52($sp)
    lw $t3,56($sp)
    lw $t4,60($sp)
    lw $t5,64($sp)
    lw $t6,68($sp)
    lw $t7,72($sp)
    lw $t8,76($sp)
    lw $t9,80($sp)
    lw $ra,84($sp)
    lw $gp,88($sp)
    addiu $sp,$sp,128
dma_probe_poll:
    lui $v0,0xb302
    lw $v0,0xa8($v0)
    bnez $v0,dma_probe_poll
    lui $v1,0xb301
    j 0x8004756c
    nop
    .size dma_probe_trampoline,.-dma_probe_trampoline
