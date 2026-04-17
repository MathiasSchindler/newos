#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    char mode;
    unsigned long long value;
} SizeSpec;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "-s SIZE file...");
}

static int parse_size_value(const char *text, unsigned long long *value_out) {
    char digits[32];
    size_t len = 0U;
    unsigned long long value;
    unsigned long long multiplier = 1ULL;
    char suffix;

    if (text == 0 || text[0] == '\0') {
        return -1;
    }

    while (text[len] >= '0' && text[len] <= '9') {
        if (len + 1U >= sizeof(digits)) {
            return -1;
        }
        digits[len] = text[len];
        len += 1U;
    }

    if (len == 0U) {
        return -1;
    }

    digits[len] = '\0';
    if (rt_parse_uint(digits, &value) != 0) {
        return -1;
    }

    suffix = text[len];
    if (suffix != '\0') {
        if (text[len + 1U] != '\0') {
            return -1;
        }
        if (suffix == 'k' || suffix == 'K') {
            multiplier = 1024ULL;
        } else if (suffix == 'm' || suffix == 'M') {
            multiplier = 1024ULL * 1024ULL;
        } else if (suffix == 'g' || suffix == 'G') {
            multiplier = 1024ULL * 1024ULL * 1024ULL;
        } else {
            return -1;
        }
    }

    *value_out = value * multiplier;
    return 0;
}

static int parse_size_spec(const char *text, SizeSpec *spec) {
    spec->mode = '=';

    if (text == 0 || text[0] == '\0') {
        return -1;
    }

    if (text[0] == '+' || text[0] == '-' || text[0] == '<' || text[0] == '>') {
        spec->mode = text[0];
        text += 1;
    }

    return parse_size_value(text, &spec->value);
}

static unsigned long long compute_target_size(unsigned long long current_size, const SizeSpec *spec) {
    if (spec->mode == '+') {
        return current_size + spec->value;
    }
    if (spec->mode == '-') {
        return current_size > spec->value ? current_size - spec->value : 0ULL;
    }
    if (spec->mode == '<') {
        return current_size < spec->value ? current_size : spec->value;
    }
    if (spec->mode == '>') {
        return current_size > spec->value ? current_size : spec->value;
    }
    return spec->value;
}

int main(int argc, char **argv) {
    SizeSpec spec;
    int argi = 1;
    int exit_code = 0;

    if (argc < 4 || rt_strcmp(argv[argi], "-s") != 0 || parse_size_spec(argv[argi + 1], &spec) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    argi += 2;
    while (argi < argc) {
        PlatformDirEntry entry;
        unsigned long long current_size = 0ULL;
        unsigned long long target_size;

        if (spec.mode != '=' && platform_get_path_info(argv[argi], &entry) == 0) {
            current_size = entry.size;
        }

        target_size = compute_target_size(current_size, &spec);
        if (platform_truncate_path(argv[argi], target_size) != 0) {
            tool_write_error("truncate", "cannot resize ", argv[argi]);
            exit_code = 1;
        }

        argi += 1;
    }

    return exit_code;
}
