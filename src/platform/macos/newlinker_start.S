.globl __start
.p2align 2
__start:
    adrp x8, _environ@PAGE
    add x8, x8, _environ@PAGEOFF
    str x2, [x8]
    bl _main
    mov x16, #1
    svc #0x80
1:
    b 1b