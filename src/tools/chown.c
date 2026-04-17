#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static int parse_owner_spec(const char *text, unsigned int *uid_out, unsigned int *gid_out) {
    char left[32];
    char right[32];
    size_t i = 0;
    size_t left_len = 0;
    size_t right_len = 0;
    unsigned long long value = 0;

    *uid_out = (unsigned int)-1;
    *gid_out = (unsigned int)-1;

    while (text[i] != '\0' && text[i] != ':') {
        if (left_len + 1 < sizeof(left)) {
            left[left_len++] = text[i];
        }
        i += 1;
    }
    left[left_len] = '\0';

    if (text[i] == ':') {
        i += 1;
        while (text[i] != '\0') {
            if (right_len + 1 < sizeof(right)) {
                right[right_len++] = text[i];
            }
            i += 1;
        }
    }
    right[right_len] = '\0';

    if (left_len > 0) {
        if (rt_parse_uint(left, &value) != 0) {
            return -1;
        }
        *uid_out = (unsigned int)value;
    }

    if (right_len > 0) {
        if (rt_parse_uint(right, &value) != 0) {
            return -1;
        }
        *gid_out = (unsigned int)value;
    }

    return (left_len == 0 && right_len == 0) ? -1 : 0;
}

int main(int argc, char **argv) {
    unsigned int uid;
    unsigned int gid;
    int i;
    int exit_code = 0;

    if (argc < 3) {
        tool_write_usage("chown", "OWNER[:GROUP] PATH...");
        return 1;
    }

    if (parse_owner_spec(argv[1], &uid, &gid) != 0) {
        tool_write_error("chown", "invalid owner spec ", argv[1]);
        return 1;
    }

    for (i = 2; i < argc; ++i) {
        if (platform_change_owner(argv[i], uid, gid) != 0) {
            tool_write_error("chown", "cannot change owner for ", argv[i]);
            exit_code = 1;
        }
    }

    return exit_code;
}
