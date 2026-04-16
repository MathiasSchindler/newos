#include "platform.h"
#include "common.h"
#include "syscall.h"

int platform_sleep_seconds(unsigned int seconds) {
    struct linux_timespec req;

    req.tv_sec = (long)seconds;
    req.tv_nsec = 0;
    return linux_syscall2(LINUX_SYS_NANOSLEEP, (long)&req, 0) < 0 ? -1 : 0;
}
