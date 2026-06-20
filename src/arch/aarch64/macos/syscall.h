#ifndef NEWOS_ARCH_AARCH64_MACOS_SYSCALL_H
#define NEWOS_ARCH_AARCH64_MACOS_SYSCALL_H

#define DARWIN_SYS_EXIT 1
#define DARWIN_SYS_FORK 2
#define DARWIN_SYS_WAIT4 7
#define DARWIN_SYS_READ 3
#define DARWIN_SYS_WRITE 4
#define DARWIN_SYS_OPEN 5
#define DARWIN_SYS_CLOSE 6
#define DARWIN_SYS_LINK 9
#define DARWIN_SYS_UNLINK 10
#define DARWIN_SYS_CHDIR 12
#define DARWIN_SYS_MKNOD 14
#define DARWIN_SYS_CHMOD 15
#define DARWIN_SYS_CHOWN 16
#define DARWIN_SYS_GETPID 20
#define DARWIN_SYS_GETUID 24
#define DARWIN_SYS_RECVFROM 29
#define DARWIN_SYS_ACCEPT 30
#define DARWIN_SYS_ACCESS 33
#define DARWIN_SYS_SYNC 36
#define DARWIN_SYS_KILL 37
#define DARWIN_SYS_PIPE 42
#define DARWIN_SYS_GETGID 47
#define DARWIN_SYS_IOCTL 54
#define DARWIN_SYS_SYMLINK 57
#define DARWIN_SYS_EXECVE 59
#define DARWIN_SYS_READLINK 58
#define DARWIN_SYS_GETGROUPS 79
#define DARWIN_SYS_DUP2 90
#define DARWIN_SYS_FCNTL 92
#define DARWIN_SYS_SELECT 93
#define DARWIN_SYS_FSYNC 95
#define DARWIN_SYS_SOCKET 97
#define DARWIN_SYS_CONNECT 98
#define DARWIN_SYS_BIND 104
#define DARWIN_SYS_SETSOCKOPT 105
#define DARWIN_SYS_LISTEN 106
#define DARWIN_SYS_GETTIMEOFDAY 116
#define DARWIN_SYS_GETSOCKOPT 118
#define DARWIN_SYS_RENAME 128
#define DARWIN_SYS_MKFIFO 132
#define DARWIN_SYS_SENDTO 133
#define DARWIN_SYS_SHUTDOWN 134
#define DARWIN_SYS_UTIMES 138
#define DARWIN_SYS_STATFS 157
#define DARWIN_SYS_UNMOUNT 159
#define DARWIN_SYS_MMAP 197
#define DARWIN_SYS_MKDIR 136
#define DARWIN_SYS_RMDIR 137
#define DARWIN_SYS_LSEEK 199
#define DARWIN_SYS_FTRUNCATE 201
#define DARWIN_SYS_SYSCTL 202
#define DARWIN_SYS_POLL 230
#define DARWIN_SYS_SYSCTLBYNAME 274
#define DARWIN_SYS_STAT64 338
#define DARWIN_SYS_FSTAT64 339
#define DARWIN_SYS_LSTAT64 340
#define DARWIN_SYS_STATFS64 345
#define DARWIN_SYS_BSDTHREAD_CREATE 360
#define DARWIN_SYS_BSDTHREAD_TERMINATE 361
#define DARWIN_SYS_BSDTHREAD_REGISTER 366
#define DARWIN_SYS_LCHOWN 364
#define DARWIN_SYS_BSDTHREAD_CTL 478
#define DARWIN_SYS_GETENTROPY 500
#define DARWIN_SYS_ULOCK_WAIT 515
#define DARWIN_SYS_ULOCK_WAKE 516

#if defined(NEWOS_MACOS_NEWLINKER)
struct MacosStraceRecord;
void darwin_trace_record(struct MacosStraceRecord *record);
#endif

static inline long darwin_raw_syscall0(long number) {
    register long x16 __asm__("x16") = number;
    register long x0 __asm__("x0");

    __asm__ volatile("svc #0x80\n\tcneg %[ret], %[ret], cs" : [ret] "=r"(x0), "+r"(x16) : : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "memory", "cc");
    return x0;
}

static inline long darwin_raw_syscall1(long number, long arg0) {
    register long x16 __asm__("x16") = number;
    register long x0 __asm__("x0") = arg0;

    __asm__ volatile("svc #0x80\n\tcneg %[ret], %[ret], cs" : [ret] "+r"(x0), "+r"(x16) : : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "memory", "cc");
    return x0;
}

static inline long darwin_raw_syscall2(long number, long arg0, long arg1) {
    register long x16 __asm__("x16") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;

    __asm__ volatile("svc #0x80\n\tcneg %[ret], %[ret], cs" : [ret] "+r"(x0), "+r"(x1), "+r"(x16) : : "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "memory", "cc");
    return x0;
}

static inline long darwin_raw_syscall3(long number, long arg0, long arg1, long arg2) {
    register long x16 __asm__("x16") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;
    register long x2 __asm__("x2") = arg2;

    __asm__ volatile("svc #0x80\n\tcneg %[ret], %[ret], cs" : [ret] "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x16) : : "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "memory", "cc");
    return x0;
}

static inline long darwin_raw_syscall4(long number, long arg0, long arg1, long arg2, long arg3) {
    register long x16 __asm__("x16") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;
    register long x2 __asm__("x2") = arg2;
    register long x3 __asm__("x3") = arg3;

    __asm__ volatile("svc #0x80\n\tcneg %[ret], %[ret], cs" : [ret] "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x16) : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "memory", "cc");
    return x0;
}

static inline long darwin_raw_syscall5(long number, long arg0, long arg1, long arg2, long arg3, long arg4) {
    register long x16 __asm__("x16") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;
    register long x2 __asm__("x2") = arg2;
    register long x3 __asm__("x3") = arg3;
    register long x4 __asm__("x4") = arg4;

    __asm__ volatile("svc #0x80\n\tcneg %[ret], %[ret], cs" : [ret] "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x16) : : "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "memory", "cc");
    return x0;
}

static inline long darwin_raw_syscall6(long number, long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) {
    register long x16 __asm__("x16") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;
    register long x2 __asm__("x2") = arg2;
    register long x3 __asm__("x3") = arg3;
    register long x4 __asm__("x4") = arg4;
    register long x5 __asm__("x5") = arg5;

    __asm__ volatile("svc #0x80\n\tcneg %[ret], %[ret], cs" : [ret] "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x16) : : "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "memory", "cc");
    return x0;
}

#define darwin_syscall0 darwin_raw_syscall0
#define darwin_syscall1 darwin_raw_syscall1
#define darwin_syscall2 darwin_raw_syscall2
#define darwin_syscall3 darwin_raw_syscall3
#define darwin_syscall4 darwin_raw_syscall4
#define darwin_syscall5 darwin_raw_syscall5
#define darwin_syscall6 darwin_raw_syscall6

#endif
