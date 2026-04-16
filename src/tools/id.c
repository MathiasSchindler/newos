#include "platform.h"
#include "runtime.h"

int main(void) {
    PlatformIdentity identity;

    if (platform_get_identity(&identity) != 0) {
        rt_write_line(2, "id: unavailable");
        return 1;
    }

    rt_write_cstr(1, "uid=");
    rt_write_uint(1, identity.uid);
    rt_write_cstr(1, "(");
    rt_write_cstr(1, identity.username);
    rt_write_cstr(1, ") gid=");
    rt_write_uint(1, identity.gid);
    rt_write_cstr(1, "(");
    rt_write_cstr(1, identity.groupname);
    rt_write_line(1, ")");
    return 0;
}
