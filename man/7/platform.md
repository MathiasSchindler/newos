# PLATFORM

## NAME

platform - the platform abstraction layers for hosted and freestanding builds

## DESCRIPTION

The platform component isolates operating-system-specific behaviour behind a uniform interface so that tool and runtime code does not call OS APIs directly. Two concrete platform layers are provided: a POSIX layer for hosted development builds and a Linux layer for the freestanding cross-compiled target. An architecture-specific CRT and syscall header complete the freestanding support for AArch64.

## STRUCTURE

### POSIX platform (src/platform/posix)

Used in the hosted build (make host). Calls standard POSIX library functions.

- fs.c — file and directory operations (open, read, write, stat, readdir, …)
- process.c — process creation, waiting, signal delivery, environment access
- identity.c — user and group identity queries
- net.c — socket creation and basic TCP/UDP I/O
- time.c — wall-clock and monotonic time

### Linux platform (src/platform/linux)

Used in the freestanding cross-compiled build (make freestanding). Makes raw Linux system calls; does not link against libc.

- fs.c — file and directory operations via Linux syscalls
- process.c — fork, exec, wait, kill, and environment via syscalls
- identity.c — getuid, getgid, and related syscalls
- net.c — socket and connect syscalls
- time.c — clock_gettime and gettimeofday syscalls

### AArch64/Linux arch layer (src/arch/aarch64/linux)

- crt0.S — C runtime startup; sets up the initial stack frame and calls main
- syscall.h — inline assembly helpers for making raw AArch64 Linux system calls

### Platform header

- src/platform/posix/common.h and src/platform/linux/common.h declare the uniform interface that the runtime and tools program against
- src/shared/platform.h includes the correct common.h based on build configuration

## LIMITATIONS

- Only Linux/AArch64 is supported as a freestanding target; other architectures have no arch layer yet
- The Linux platform layer targets the stable Linux syscall ABI; kernel versions below 3.x may be missing some calls
- IPv6 support in the net modules is minimal

## SEE ALSO

man, project-layout, runtime, build
