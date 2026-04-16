#include "platform.h"
#include "runtime.h"

int main(void) {
    PlatformIdentity identity;

    if (platform_get_identity(&identity) != 0) {
        rt_write_line(2, "whoami: unavailable");
        return 1;
    }

    return rt_write_line(1, identity.username) == 0 ? 0 : 1;
}
