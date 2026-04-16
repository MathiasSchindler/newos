#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "common.h"

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

int platform_get_identity(PlatformIdentity *identity_out) {
    struct passwd *pw;
    struct group *gr;

    if (identity_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    identity_out->uid = (unsigned int)getuid();
    identity_out->gid = (unsigned int)getgid();

    pw = getpwuid((uid_t)identity_out->uid);
    gr = getgrgid((gid_t)identity_out->gid);

    if (pw != NULL) {
        posix_copy_string(identity_out->username, sizeof(identity_out->username), pw->pw_name);
    } else {
        snprintf(identity_out->username, sizeof(identity_out->username), "%u", identity_out->uid);
    }

    if (gr != NULL) {
        posix_copy_string(identity_out->groupname, sizeof(identity_out->groupname), gr->gr_name);
    } else {
        snprintf(identity_out->groupname, sizeof(identity_out->groupname), "%u", identity_out->gid);
    }

    return 0;
}
