# SYSCALL-TABLE

## NAME

syscall-table - auditing the freestanding Linux syscall surface

## OVERVIEW

The freestanding Linux build links project tools without libc and routes OS interaction through `src/platform/linux/` plus the selected `src/arch/<arch>/linux/` syscall layer. This page documents how to read and refresh the syscall surface. It no longer carries a checked-in per-tool matrix because the visible tool set changes quickly and stale rows are worse than no matrix.

The current `Makefile` builds 178 visible tools for each supported build mode where practical. A syscall audit should therefore be regenerated from the current `build/freestanding-linux-<arch>/` tree whenever it is needed for review.

## CURRENT SURFACE

The x86-64 freestanding syscall header currently declares these Linux syscalls for direct platform use:

| group | syscalls |
| --- | --- |
| basic I/O | `read`, `write`, `close`, `lseek`, `ioctl`, `poll`, `ppoll`, `fcntl`, `dup3`, `pipe2`, `close_range` |
| process and signals | `clone`, `execve`, `exit`, `wait4`, `kill`, `rt_sigaction`, `getpid` |
| filesystem | `openat`, `mkdirat`, `mknodat`, `newfstatat`, `getdents64`, `unlinkat`, `renameat`, `linkat`, `symlinkat`, `readlinkat`, `fchmodat`, `fchownat`, `utimensat`, `truncate`, `ftruncate`, `fsync`, `fdatasync`, `statfs`, `getcwd`, `chdir` |
| system and identity | `uname`, `syslog`, `sync`, `mount`, `umount2`, `reboot`, `sethostname`, `getuid`, `getgid`, `setuid`, `setgid`, `setgroups`, `clock_gettime`, `nanosleep`, `getrandom` |
| networking | `socket`, `connect`, `accept`, `accept4`, `shutdown`, `bind`, `listen`, `setsockopt` |

The AArch64 freestanding header mirrors the same broad platform contract with that architecture's Linux syscall numbers.

## AUDIT METHOD

A useful per-tool matrix is a generated artifact, not hand-maintained prose. To refresh one:

1. Build the target tree, for example `make freestanding TARGET_ARCH=x86_64`.
2. Enumerate tools from the `TOOLS` variable in the root `Makefile`.
3. Disassemble or inspect each binary under `build/freestanding-linux-x86_64/`.
4. Record syscall numbers loaded before calls to the architecture syscall helpers or inline syscall instructions.
5. Map syscall numbers back through `src/arch/x86_64/linux/syscall.h`.

For AArch64, use `build/freestanding-linux-aarch64/` and `src/arch/aarch64/linux/syscall.h` instead.

## INTERPRETATION

- Static presence means the syscall appears in the binary's reachable code after linking; it does not mean every invocation exercises it.
- Shared platform code can make a syscall appear in several tools even when only one option path needs it.
- Section garbage collection affects the result, so audit the same build flags that matter for the review.
- Hidden helper outputs and compatibility symlinks should be excluded unless the audit explicitly wants them.
- Any checked result should include the build directory, target architecture, compiler/linker flags, and git revision used to generate it.

## COMMON FINDINGS

Small filters usually retain only basic I/O and process exit. Filesystem tools pull in `openat`, `newfstatat`, directory walking, and link/unlink/rename operations. Shell, build, service, and process tools add `clone`, `execve`, `wait4`, `kill`, pipes, and polling. Network tools add socket calls, and TLS-capable tools may also include random-number and certificate/file access paths through the shared TLS and crypto layers.

The freestanding stack-guard startup path can use `getrandom` or `/dev/urandom` through `openat`/`read` when compiler stack-protector instrumentation is enabled or when the guard initializer is otherwise linked into the binary.

## SEE ALSO

build, platform, runtime, testing
