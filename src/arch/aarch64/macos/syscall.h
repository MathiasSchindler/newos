#ifndef NEWOS_ARCH_AARCH64_MACOS_SYSCALL_H
#define NEWOS_ARCH_AARCH64_MACOS_SYSCALL_H

#define DARWIN_SYS_EXIT 1
#define DARWIN_SYS_READ 3
#define DARWIN_SYS_WRITE 4
#define DARWIN_SYS_CLOSE 6
#define DARWIN_SYS_GETPID 20

static inline long darwin_syscall0(long number) {
    register long x16 __asm__("x16") = number;
    register long x0 __asm__("x0");

    __asm__ volatile("svc #0x80" : "=r"(x0) : "r"(x16) : "memory");
    return x0;
}

static inline long darwin_syscall1(long number, long arg0) {
    register long x16 __asm__("x16") = number;
    register long x0 __asm__("x0") = arg0;

    __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x16) : "memory");
    return x0;
}

static inline long darwin_syscall2(long number, long arg0, long arg1) {
    register long x16 __asm__("x16") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;

    __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x16) : "memory");
    return x0;
}

static inline long darwin_syscall3(long number, long arg0, long arg1, long arg2) {
    register long x16 __asm__("x16") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;
    register long x2 __asm__("x2") = arg2;

    __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x16) : "memory");
    return x0;
}

#endif
