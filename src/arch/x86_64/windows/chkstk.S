    .section .text$___chkstk_ms,"xr",one_only,___chkstk_ms
    .globl ___chkstk_ms
    .p2align 4
___chkstk_ms:
    pushq %rcx
    pushq %r10
    movq %rax, %r10
    leaq 24(%rsp), %rcx
    cmpq $0x1000, %rax
    jb 2f
1:
    subq $0x1000, %rcx
    testq %rax, (%rcx)
    subq $0x1000, %rax
    cmpq $0x1000, %rax
    jae 1b
2:
    subq %rax, %rcx
    testq %rax, (%rcx)
    movq %r10, %rax
    popq %r10
    popq %rcx
    ret