#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define UNIQ_LINE_CAPACITY 4096

typedef struct {
    int count_only;
    int duplicates_only;
    int unique_only;
} UniqOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-cdu] [file]");
}

static int should_print_group(unsigned long long count, const UniqOptions *options) {
    if (options->duplicates_only) {
        return count > 1ULL;
    }

    if (options->unique_only) {
        return count == 1ULL;
    }

    return 1;
}

static int emit_group(const char *line, unsigned long long count, const UniqOptions *options) {
    if (!should_print_group(count, options)) {
        return 0;
    }

    if (options->count_only) {
        if (rt_write_uint(1, count) != 0 || rt_write_char(1, ' ') != 0) {
            return -1;
        }
    }

    return rt_write_line(1, line);
}

static int uniq_stream(int fd, const UniqOptions *options) {
    char chunk[4096];
    char previous[UNIQ_LINE_CAPACITY];
    char current[UNIQ_LINE_CAPACITY];
    size_t current_len = 0;
    unsigned long long repeat_count = 0;
    int have_previous = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                current[current_len] = '\0';

                if (!have_previous) {
                    rt_copy_string(previous, sizeof(previous), current);
                    repeat_count = 1ULL;
                    have_previous = 1;
                } else if (rt_strcmp(previous, current) == 0) {
                    repeat_count += 1ULL;
                } else {
                    if (emit_group(previous, repeat_count, options) != 0) {
                        return -1;
                    }
                    rt_copy_string(previous, sizeof(previous), current);
                    repeat_count = 1ULL;
                }

                current_len = 0;
            } else if (current_len + 1U < sizeof(current)) {
                current[current_len++] = ch;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (current_len > 0U) {
        current[current_len] = '\0';

        if (!have_previous) {
            rt_copy_string(previous, sizeof(previous), current);
            repeat_count = 1ULL;
            have_previous = 1;
        } else if (rt_strcmp(previous, current) == 0) {
            repeat_count += 1ULL;
        } else {
            if (emit_group(previous, repeat_count, options) != 0) {
                return -1;
            }
            rt_copy_string(previous, sizeof(previous), current);
            repeat_count = 1ULL;
        }
    }

    if (have_previous) {
        return emit_group(previous, repeat_count, options);
    }

    return 0;
}

int main(int argc, char **argv) {
    UniqOptions options;
    int argi = 1;
    int exit_code = 0;

    rt_memset(&options, 0, sizeof(options));

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag = argv[argi] + 1;

        if (argv[argi][1] == '-' && argv[argi][2] == '\0') {
            argi += 1;
            break;
        }

        while (*flag != '\0') {
            if (*flag == 'c') {
                options.count_only = 1;
            } else if (*flag == 'd') {
                options.duplicates_only = 1;
            } else if (*flag == 'u') {
                options.unique_only = 1;
            } else {
                print_usage(argv[0]);
                return 1;
            }
            flag += 1;
        }

        argi += 1;
    }

    if (argc - argi > 1) {
        print_usage(argv[0]);
        return 1;
    }

    if (argi == argc) {
        return uniq_stream(0, &options) == 0 ? 0 : 1;
    }

    {
        int fd;
        int should_close;

        if (tool_open_input(argv[argi], &fd, &should_close) != 0) {
            tool_write_error("uniq", "cannot open ", argv[argi]);
            return 1;
        }

        if (uniq_stream(fd, &options) != 0) {
            tool_write_error("uniq", "read error on ", argv[argi]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
