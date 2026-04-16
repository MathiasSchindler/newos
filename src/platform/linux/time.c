#include "platform.h"
#include "common.h"
#include "syscall.h"

int platform_sleep_seconds(unsigned int seconds) {
    struct linux_timespec req;

    req.tv_sec = (long)seconds;
    req.tv_nsec = 0;
    return linux_syscall2(LINUX_SYS_NANOSLEEP, (long)&req, 0) < 0 ? -1 : 0;
}

long long platform_get_epoch_time(void) {
    struct linux_timespec now;

    if (linux_syscall2(LINUX_SYS_CLOCK_GETTIME, 0, (long)&now) < 0) {
        return 0;
    }

    return (long long)now.tv_sec;
}
