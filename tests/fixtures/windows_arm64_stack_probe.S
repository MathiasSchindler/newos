    .text
    .globl stack_probe_abi
    .p2align 2
stack_probe_abi:
    stp x19, x20, [sp, #-32]!
    stp x29, x30, [sp, #16]
    mov x29, sp
    mov x19, x0
    mov x20, x1
    str x29, [x20, #8]
    sub x16, x29, x19, lsl #4
    str x16, [x20]
    mov x15, x19
    .macro seed register, value
    mov \register, #\value
    .endm
    seed x0, 256
    seed x1, 257
    seed x2, 258
    seed x3, 259
    seed x4, 260
    seed x5, 261
    seed x6, 262
    seed x7, 263
    seed x8, 264
    seed x9, 265
    seed x10, 266
    seed x11, 267
    seed x12, 268
    seed x13, 269
    seed x14, 270
    bl __chkstk
    mov x16, sp
    cmp x16, x29
    b.ne .Labi_failure
    cmp x15, x19
    b.ne .Labi_failure
    .macro check register, value
    cmp \register, #\value
    b.ne .Labi_failure
    .endm
    check x0, 256
    check x1, 257
    check x2, 258
    check x3, 259
    check x4, 260
    check x5, 261
    check x6, 262
    check x7, 263
    check x8, 264
    check x9, 265
    check x10, 266
    check x11, 267
    check x12, 268
    check x13, 269
    check x14, 270
    mov w0, #0
    b .Labi_return
.Labi_failure:
    mov w0, #20
.Labi_return:
    ldp x29, x30, [sp, #16]
    ldp x19, x20, [sp], #32
    ret

#if defined(STACK_PROBE_NOOP_TEST)
    .section .text$__chkstk,"xr",one_only,__chkstk
    .globl __chkstk
    .p2align 2
__chkstk:
    ret
#endif