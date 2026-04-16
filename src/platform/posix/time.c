#define _POSIX_C_SOURCE 200809L

#include "platform.h"

#include <unistd.h>

int platform_sleep_seconds(unsigned int seconds) {
    return sleep(seconds) == 0 ? 0 : -1;
}
