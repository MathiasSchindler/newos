#include "platform.h"
#include "common.h"
#include "syscall.h"

int platform_get_identity(PlatformIdentity *identity_out) {
    unsigned long long uid;
    unsigned long long gid;

    if (identity_out == 0) {
        return -1;
    }

    uid = (unsigned long long)linux_syscall1(LINUX_SYS_GETUID, 0);
    gid = (unsigned long long)linux_syscall1(LINUX_SYS_GETGID, 0);
    identity_out->uid = (unsigned int)uid;
    identity_out->gid = (unsigned int)gid;
    linux_unsigned_to_string(uid, identity_out->username, sizeof(identity_out->username));
    linux_unsigned_to_string(gid, identity_out->groupname, sizeof(identity_out->groupname));
    return 0;
}
