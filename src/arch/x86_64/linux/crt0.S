.global _start
.extern __newos_stack_guard_init
.text
_start:
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    call __newos_stack_guard_init
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    call main
    mov %rax, %rdi
    mov $60, %rax
    syscall
