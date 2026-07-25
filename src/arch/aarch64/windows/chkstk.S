    .text
    .globl __chkstk
    .p2align 2
__chkstk:
    lsl x16, x15, #4
    mov x17, sp
    cmp x16, #0x1000
    b.lo 2f
1:
    sub x17, x17, #0x1000
    ldr xzr, [x17]
    sub x16, x16, #0x1000
    cmp x16, #0x1000
    b.hs 1b
2:
    sub x17, x17, x16
    ldr xzr, [x17]
    ret