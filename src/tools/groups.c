#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#if __STDC_HOSTED__
#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
extern int getgrouplist(const char *, int, int *, int *);
#endif
#else
typedef unsigned int gid_t;
#endif

static void write_group_name(gid_t gid, int numeric) {
#if __STDC_HOSTED__
    if (!numeric) {
        struct group *gr = getgrgid(gid);
        if (gr != 0 && gr->gr_name != 0 && gr->gr_name[0] != '\0') {
            rt_write_cstr(1, gr->gr_name);
            return;
        }
    }
#else
    (void)numeric;
#endif
    rt_write_uint(1, (unsigned long long)gid);
}

static int compare_group_ids(const void *lhs, const void *rhs) {
    gid_t left = *(const gid_t *)lhs;
    gid_t right = *(const gid_t *)rhs;
    if (left < right) {
        return -1;
    }
    if (left > right) {
        return 1;
    }
    return 0;
}

static int print_groups_for_user(const char *requested_name, int include_name, int numeric) {
#if __STDC_HOSTED__
    gid_t *group_ids = 0;
    int group_capacity = 16;
    int group_count = group_capacity;
    gid_t primary_gid = 0;
    uid_t uid = 0;
    char username[PLATFORM_NAME_CAPACITY];
    int i;
    int first = 1;

    if (requested_name != 0) {
        struct passwd *pw = getpwnam(requested_name);
        if (pw == 0) {
            tool_write_error("groups", "unknown user ", requested_name);
            return 1;
        }
        uid = pw->pw_uid;
        primary_gid = pw->pw_gid;
        rt_copy_string(username, sizeof(username), pw->pw_name);
    } else {
        PlatformIdentity identity;
        if (platform_get_identity(&identity) != 0) {
            tool_write_error("groups", "cannot inspect groups for ", "current user");
            return 1;
        }
        uid = (uid_t)identity.uid;
        primary_gid = (gid_t)identity.gid;
        rt_copy_string(username, sizeof(username), identity.username);
    }

    group_ids = (gid_t *)malloc((size_t)group_capacity * sizeof(gid_t));
    if (group_ids == 0) {
        tool_write_error("groups", "out of memory", 0);
        return 1;
    }

    if (getgrouplist(username, (int)primary_gid, (int *)group_ids, &group_count) < 0) {
        gid_t *resized = (gid_t *)realloc(group_ids, (size_t)group_count * sizeof(gid_t));
        if (resized == 0) {
            free(group_ids);
            tool_write_error("groups", "out of memory", 0);
            return 1;
        }
        group_ids = resized;
        if (getgrouplist(username, (int)primary_gid, (int *)group_ids, &group_count) < 0) {
            free(group_ids);
            tool_write_error("groups", "cannot inspect groups for ", requested_name != 0 ? requested_name : username);
            return 1;
        }
    }

    (void)uid;
    qsort(group_ids, (size_t)group_count, sizeof(gid_t), compare_group_ids);

    if (include_name) {
        rt_write_cstr(1, username);
        rt_write_cstr(1, " : ");
    }

    for (i = 0; i < group_count; ++i) {
        if (i > 0 && group_ids[i] == group_ids[i - 1]) {
            continue;
        }
        if (!first) {
            rt_write_char(1, ' ');
        }
        write_group_name(group_ids[i], numeric);
        first = 0;
    }
    rt_write_char(1, '\n');
    free(group_ids);
    return 0;
#else
    (void)requested_name;
    (void)include_name;
    (void)numeric;
    tool_write_error("groups", "not available on this platform", 0);
    return 1;
#endif
}

int main(int argc, char **argv) {
    int i;
    int exit_code = 0;
    int numeric = 0;
    int first_user = argc;

    for (i = 1; i < argc; ++i) {
        if (rt_strcmp(argv[i], "-n") == 0) {
            numeric = 1;
        } else {
            first_user = i;
            break;
        }
    }

    if (argc == 1 || first_user >= argc) {
        return print_groups_for_user(0, 0, numeric);
    }

    for (i = first_user; i < argc; ++i) {
        if (rt_strcmp(argv[i], "-n") == 0) {
            continue;
        }
        if (print_groups_for_user(argv[i], 1, numeric) != 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
