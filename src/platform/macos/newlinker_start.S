.globl __start
.p2align 2
__start:
    adrp x8, _environ@PAGE
    add x8, x8, _environ@PAGEOFF
    add x9, x1, x0, lsl #3
    add x9, x9, #8
    str x9, [x8]
    bl _main
    mov x16, #1
    svc #0x80
1:
    b 1b