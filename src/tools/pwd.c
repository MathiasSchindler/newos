#include "platform.h"
#include "runtime.h"

int main(void) {
    char buffer[4096];

    if (platform_get_current_directory(buffer, sizeof(buffer)) != 0) {
        rt_write_line(2, "pwd: unable to read current directory");
        return 1;
    }

    return rt_write_line(1, buffer) == 0 ? 0 : 1;
}
