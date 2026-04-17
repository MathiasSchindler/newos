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

int platform_lookup_group(const char *groupname, unsigned int *gid_out) {
    PlatformIdentity identity;
    unsigned long long value = 0;

    if (groupname == 0 || groupname[0] == '\0' || gid_out == 0) {
        return -1;
    }

    if (rt_parse_uint(groupname, &value) == 0) {
        *gid_out = (unsigned int)value;
        return 0;
    }

    if (platform_get_identity(&identity) != 0) {
        return -1;
    }

    if (rt_strcmp(groupname, identity.groupname) != 0) {
        return -1;
    }

    *gid_out = identity.gid;
    return 0;
}

int platform_lookup_identity(const char *username, PlatformIdentity *identity_out) {
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

    if (username != 0 && username[0] != '\0' && rt_strcmp(username, identity_out->username) != 0) {
        return -1;
    }

    return 0;
}

int platform_get_identity(PlatformIdentity *identity_out) {
    return platform_lookup_identity(0, identity_out);
}

int platform_list_groups_for_identity(
    const PlatformIdentity *identity,
    PlatformGroupEntry *entries_out,
    size_t entry_capacity,
    size_t *count_out
) {
    if (identity == 0 || count_out == 0 || (entry_capacity > 0 && entries_out == 0)) {
        return -1;
    }

    *count_out = 0;
    if (entry_capacity == 0) {
        return 0;
    }

    entries_out[0].gid = identity->gid;
    linux_copy_string(entries_out[0].name, sizeof(entries_out[0].name), identity->groupname);
    *count_out = 1;
    return 0;
}

int platform_list_sessions(PlatformSessionEntry *entries_out, size_t entry_capacity, size_t *count_out) {
    PlatformIdentity identity;

    if (count_out == 0 || (entry_capacity > 0 && entries_out == 0)) {
        return -1;
    }

    if (platform_get_identity(&identity) != 0) {
        return -1;
    }

    *count_out = 1;
    if (entry_capacity == 0) {
        return 0;
    }

    linux_copy_string(entries_out[0].username, sizeof(entries_out[0].username), identity.username);
    entries_out[0].terminal[0] = '\0';
    entries_out[0].host[0] = '\0';
    entries_out[0].login_time = 0;
    return 0;
}
