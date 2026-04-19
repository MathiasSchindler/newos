#include "platform.h"
#include "runtime.h"
#include "syscall.h"

long platform_write(int fd, const void *buffer, size_t count) {
    return darwin_syscall3(DARWIN_SYS_WRITE, (long)fd, (long)buffer, (long)count);
}

long platform_read(int fd, void *buffer, size_t count) {
    return darwin_syscall3(DARWIN_SYS_READ, (long)fd, (long)buffer, (long)count);
}

int platform_close(int fd) {
    if (fd >= 0 && fd <= 2) {
        return 0;
    }
    return darwin_syscall1(DARWIN_SYS_CLOSE, (long)fd) < 0 ? -1 : 0;
}

int platform_get_process_id(void) {
    long pid = darwin_syscall0(DARWIN_SYS_GETPID);
    return pid < 0 ? -1 : (int)pid;
}
