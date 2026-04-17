#include "platform.h"
#include "common.h"
#include "syscall.h"

struct linux_utsname_identity {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

int platform_get_hostname(char *buffer, size_t buffer_size) {
    struct linux_utsname_identity info;
    if (buffer == 0 || buffer_size == 0) {
        return -1;
    }
    if (linux_syscall1(LINUX_SYS_UNAME, (long)&info) < 0) {
        return -1;
    }
    linux_copy_string(buffer, (unsigned long)buffer_size, info.nodename);
    return 0;
}

int platform_set_hostname(const char *name) {
    return linux_syscall2(LINUX_SYS_SETHOSTNAME, (long)name, (long)linux_string_length(name)) < 0 ? -1 : 0;
}

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
