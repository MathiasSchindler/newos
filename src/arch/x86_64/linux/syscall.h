#ifndef NEWOS_ARCH_X86_64_LINUX_SYSCALL_H
#define NEWOS_ARCH_X86_64_LINUX_SYSCALL_H

#define LINUX_SYS_READ 0
#define LINUX_SYS_WRITE 1
#define LINUX_SYS_CLOSE 3
#define LINUX_SYS_LSEEK 8
#define LINUX_SYS_IOCTL 16
#define LINUX_SYS_NANOSLEEP 35
#define LINUX_SYS_GETPID 39
#define LINUX_SYS_SYSLOG 103
#define LINUX_SYS_CLONE 56
#define LINUX_SYS_EXECVE 59
#define LINUX_SYS_EXIT 60
#define LINUX_SYS_WAIT4 61
#define LINUX_SYS_KILL 62
#define LINUX_SYS_UNAME 63
#define LINUX_SYS_FSYNC 74
#define LINUX_SYS_FDATASYNC 75
#define LINUX_SYS_TRUNCATE 76
#define LINUX_SYS_FTRUNCATE 77
#define LINUX_SYS_GETCWD 79
#define LINUX_SYS_CHDIR 80
#define LINUX_SYS_MOUNT 165
#define LINUX_SYS_UMOUNT2 166
#define LINUX_SYS_SYNC 162
#define LINUX_SYS_SETHOSTNAME 170
#define LINUX_SYS_GETUID 102
#define LINUX_SYS_GETGID 104
#define LINUX_SYS_STATFS 137
#define LINUX_SYS_CLOCK_GETTIME 228
#define LINUX_SYS_PIPE2 293
#define LINUX_SYS_OPENAT 257
#define LINUX_SYS_MKDIRAT 258
#define LINUX_SYS_FCHOWNAT 260
#define LINUX_SYS_NEWFSTATAT 262
#define LINUX_SYS_UNLINKAT 263
#define LINUX_SYS_RENAMEAT 264
#define LINUX_SYS_LINKAT 265
#define LINUX_SYS_SYMLINKAT 266
#define LINUX_SYS_READLINKAT 267
#define LINUX_SYS_FCHMODAT 268
#define LINUX_SYS_UTIMENSAT 280
#define LINUX_SYS_DUP3 292
#define LINUX_SYS_GETDENTS64 217

long linux_syscall0(long number);
long linux_syscall1(long number, long arg0);
long linux_syscall2(long number, long arg0, long arg1);
long linux_syscall3(long number, long arg0, long arg1, long arg2);
long linux_syscall4(long number, long arg0, long arg1, long arg2, long arg3);
long linux_syscall5(long number, long arg0, long arg1, long arg2, long arg3, long arg4);

#endif
