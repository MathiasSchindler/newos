#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define SPLIT_MODE_LINES 1
#define SPLIT_MODE_BYTES 2

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-l COUNT | -b SIZE] [file [prefix]]");
}

static int parse_size_value(const char *text, unsigned long long *value_out) {
    char digits[32];
    size_t len = 0;
    unsigned long long value;
    unsigned long long multiplier = 1ULL;
    char suffix;

    if (text == 0 || text[0] == '\0') {
        return -1;
    }

    while (text[len] >= '0' && text[len] <= '9') {
        if (len + 1 >= sizeof(digits)) {
            return -1;
        }
        digits[len] = text[len];
        len += 1;
    }

    if (len == 0) {
        return -1;
    }

    digits[len] = '\0';
    if (rt_parse_uint(digits, &value) != 0) {
        return -1;
    }

    suffix = text[len];
    if (suffix != '\0') {
        if (text[len + 1] != '\0') {
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

static int make_output_name(const char *prefix, unsigned long long index, char *buffer, size_t buffer_size) {
    size_t prefix_len = rt_strlen(prefix);

    if (index >= 26ULL * 26ULL || prefix_len + 3 > buffer_size) {
        return -1;
    }

    memcpy(buffer, prefix, prefix_len);
    buffer[prefix_len] = (char)('a' + (char)((index / 26ULL) % 26ULL));
    buffer[prefix_len + 1] = (char)('a' + (char)(index % 26ULL));
    buffer[prefix_len + 2] = '\0';
    return 0;
}

static int ensure_output_open(int *fd_out, const char *prefix, unsigned long long *index_io) {
    char path[256];

    if (*fd_out >= 0) {
        return 0;
    }

    if (make_output_name(prefix, *index_io, path, sizeof(path)) != 0) {
        tool_write_error("split", "too many output files for prefix ", prefix);
        return -1;
    }

    *fd_out = platform_open_write(path, 0644U);
    if (*fd_out < 0) {
        tool_write_error("split", "cannot create ", path);
        return -1;
    }

    *index_io += 1ULL;
    return 0;
}

int main(int argc, char **argv) {
    int mode = SPLIT_MODE_LINES;
    unsigned long long limit = 1000ULL;
    const char *input_path = 0;
    const char *prefix = "x";
    int argi = 1;
    int input_fd;
    int should_close = 0;
    int out_fd = -1;
    unsigned long long part_index = 0ULL;
    unsigned long long units_in_part = 0ULL;
    char buffer[4096];
    int saw_input = 0;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-l") == 0) {
            if (argi + 1 >= argc || parse_size_value(argv[argi + 1], &limit) != 0 || limit == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            mode = SPLIT_MODE_LINES;
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-b") == 0) {
            if (argi + 1 >= argc || parse_size_value(argv[argi + 1], &limit) != 0 || limit == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            mode = SPLIT_MODE_BYTES;
            argi += 2;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (argi < argc) {
        input_path = argv[argi];
        argi += 1;
    }
    if (argi < argc) {
        prefix = argv[argi];
        argi += 1;
    }
    if (argi != argc) {
        print_usage(argv[0]);
        return 1;
    }

    if (tool_open_input(input_path, &input_fd, &should_close) != 0) {
        tool_write_error("split", "cannot open ", input_path != 0 ? input_path : "stdin");
        return 1;
    }

    for (;;) {
        long bytes_read = platform_read(input_fd, buffer, sizeof(buffer));
        long i;

        if (bytes_read < 0) {
            tool_close_input(input_fd, should_close);
            if (out_fd >= 0) {
                platform_close(out_fd);
            }
            tool_write_error("split", "read error", 0);
            return 1;
        }
        if (bytes_read == 0) {
            break;
        }

        saw_input = 1;
        for (i = 0; i < bytes_read; ++i) {
            if (ensure_output_open(&out_fd, prefix, &part_index) != 0) {
                tool_close_input(input_fd, should_close);
                return 1;
            }

            if (rt_write_all(out_fd, &buffer[i], 1U) != 0) {
                tool_close_input(input_fd, should_close);
                platform_close(out_fd);
                tool_write_error("split", "write error", 0);
                return 1;
            }

            if (mode == SPLIT_MODE_LINES) {
                if (buffer[i] == '\n') {
                    units_in_part += 1ULL;
                    if (units_in_part >= limit) {
                        platform_close(out_fd);
                        out_fd = -1;
                        units_in_part = 0ULL;
                    }
                }
            } else {
                units_in_part += 1ULL;
                if (units_in_part >= limit) {
                    platform_close(out_fd);
                    out_fd = -1;
                    units_in_part = 0ULL;
                }
            }
        }
    }

    if (!saw_input) {
        if (ensure_output_open(&out_fd, prefix, &part_index) != 0) {
            tool_close_input(input_fd, should_close);
            return 1;
        }
    }

    tool_close_input(input_fd, should_close);
    if (out_fd >= 0) {
        platform_close(out_fd);
    }

    return 0;
}
