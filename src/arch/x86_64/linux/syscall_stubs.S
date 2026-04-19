.text

.global linux_syscall0
linux_syscall0:
    mov %rdi, %rax
    syscall
    ret

.global linux_syscall1
linux_syscall1:
    mov %rdi, %rax
    mov %rsi, %rdi
    syscall
    ret

.global linux_syscall2
linux_syscall2:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    syscall
    ret

.global linux_syscall3
linux_syscall3:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    syscall
    ret

.global linux_syscall4
linux_syscall4:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %r10
    syscall
    ret

.global linux_syscall5
linux_syscall5:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %r10
    mov %r9, %r8
    syscall
    ret
