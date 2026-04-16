#define _POSIX_C_SOURCE 200809L

#include "platform.h"

#include <time.h>
#include <unistd.h>

int platform_sleep_seconds(unsigned int seconds) {
    return sleep(seconds) == 0 ? 0 : -1;
}

long long platform_get_epoch_time(void) {
    time_t now = time(NULL);
    return (now == (time_t)-1) ? 0 : (long long)now;
}
