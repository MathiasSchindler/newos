#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static int is_hostname_char(char ch) {
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '-' ||
           ch == '.';
}

static int is_valid_hostname(const char *name) {
    size_t i = 0;
    size_t label_length = 0;
    int previous_was_hyphen = 0;

    if (name == 0 || name[0] == '\0') {
        return 0;
    }

    while (name[i] != '\0') {
        if (i >= 253U || !is_hostname_char(name[i])) {
            return 0;
        }
        if (name[i] == '.') {
            if (label_length == 0U || previous_was_hyphen) {
                return 0;
            }
            label_length = 0U;
            previous_was_hyphen = 0;
        } else if (name[i] == '-') {
            if (label_length == 0U) {
                return 0;
            }
            label_length += 1U;
            previous_was_hyphen = 1;
        } else {
            label_length += 1U;
            previous_was_hyphen = 0;
        }
        i += 1U;
    }

    return label_length > 0U && !previous_was_hyphen;
}

int main(int argc, char **argv) {
    char name[256];

    if (argc == 1) {
        if (platform_get_hostname(name, sizeof(name)) != 0) {
            tool_write_error("hostname", "failed", 0);
            return 1;
        }
        return tool_write_visible_line(1, name) == 0 ? 0 : 1;
    }

    if (argc != 2) {
        tool_write_usage("hostname", "[NAME]");
        return 1;
    }

    if (!is_valid_hostname(argv[1])) {
        tool_write_error("hostname", "invalid hostname", 0);
        return 1;
    }

    if (platform_set_hostname(argv[1]) != 0) {
        tool_write_error("hostname", "cannot set hostname", 0);
        return 1;
    }

    return 0;
}
