#ifndef NEWOS_ARCH_AARCH64_LINUX_SYSCALL_H
#define NEWOS_ARCH_AARCH64_LINUX_SYSCALL_H

#define LINUX_SYS_GETCWD 17
#define LINUX_SYS_CHDIR 49
#define LINUX_SYS_FCHMODAT 53
#define LINUX_SYS_FCHOWNAT 54
#define LINUX_SYS_OPENAT 56
#define LINUX_SYS_CLOSE 57
#define LINUX_SYS_PIPE2 59
#define LINUX_SYS_GETDENTS64 61
#define LINUX_SYS_LSEEK 62
#define LINUX_SYS_READ 63
#define LINUX_SYS_WRITE 64
#define LINUX_SYS_UNLINKAT 35
#define LINUX_SYS_SYMLINKAT 36
#define LINUX_SYS_LINKAT 37
#define LINUX_SYS_RENAMEAT 38
#define LINUX_SYS_UMOUNT2 39
#define LINUX_SYS_MOUNT 40
#define LINUX_SYS_UNAME 160
#define LINUX_SYS_SETHOSTNAME 161
#define LINUX_SYS_UTIMENSAT 88
#define LINUX_SYS_DUP3 24
#define LINUX_SYS_IOCTL 29
#define LINUX_SYS_MKDIRAT 34
#define LINUX_SYS_NEWFSTATAT 79
#define LINUX_SYS_READLINKAT 78
#define LINUX_SYS_TRUNCATE 45
#define LINUX_SYS_FTRUNCATE 46
#define LINUX_SYS_STATFS 43
#define LINUX_SYS_SYNC 81
#define LINUX_SYS_FSYNC 82
#define LINUX_SYS_FDATASYNC 83
#define LINUX_SYS_EXIT 93
#define LINUX_SYS_NANOSLEEP 101
#define LINUX_SYS_SYSLOG 116
#define LINUX_SYS_CLOCK_GETTIME 113
#define LINUX_SYS_KILL 129
#define LINUX_SYS_GETPID 172
#define LINUX_SYS_GETUID 174
#define LINUX_SYS_GETGID 176
#define LINUX_SYS_CLONE 220
#define LINUX_SYS_EXECVE 221
#define LINUX_SYS_WAIT4 260

static inline long linux_syscall1(long number, long arg0) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = arg0;

    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static inline long linux_syscall0(long number) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = 0;

    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
    return x0;
}

static inline long linux_syscall2(long number, long arg0, long arg1) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;

    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

static inline long linux_syscall3(long number, long arg0, long arg1, long arg2) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;
    register long x2 __asm__("x2") = arg2;

    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static inline long linux_syscall4(long number, long arg0, long arg1, long arg2, long arg3) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;
    register long x2 __asm__("x2") = arg2;
    register long x3 __asm__("x3") = arg3;

    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}

static inline long linux_syscall5(long number, long arg0, long arg1, long arg2, long arg3, long arg4) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;
    register long x2 __asm__("x2") = arg2;
    register long x3 __asm__("x3") = arg3;
    register long x4 __asm__("x4") = arg4;

    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
    return x0;
}

#endif
