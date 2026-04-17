#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef enum {
    HEAD_MODE_LINES,
    HEAD_MODE_BYTES
} HeadMode;

typedef struct {
    HeadMode mode;
    unsigned long long count;
    int quiet;
    int verbose;
} HeadOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-n COUNT | -c COUNT] [-qv] [file ...]");
}

static int parse_count_value(const char *program_name,
                             const char *flag_name,
                             const char *value_text,
                             unsigned long long *count_out) {
    if (value_text == 0 || tool_parse_uint_arg(value_text, count_out, program_name, flag_name) != 0) {
        return -1;
    }
    return 0;
}

static int parse_options(int argc, char **argv, HeadOptions *options, int *arg_index_out) {
    int arg_index = 1;

    options->mode = HEAD_MODE_LINES;
    options->count = 10;
    options->quiet = 0;
    options->verbose = 0;

    while (arg_index < argc && argv[arg_index][0] == '-' && argv[arg_index][1] != '\0') {
        const char *arg = argv[arg_index];

        if (rt_strcmp(arg, "--") == 0) {
            arg_index += 1;
            break;
        }

        if (rt_strcmp(arg, "-q") == 0) {
            options->quiet = 1;
            options->verbose = 0;
            arg_index += 1;
            continue;
        }

        if (rt_strcmp(arg, "-v") == 0) {
            options->verbose = 1;
            options->quiet = 0;
            arg_index += 1;
            continue;
        }

        if (rt_strcmp(arg, "-n") == 0 || rt_strcmp(arg, "-c") == 0) {
            if (arg_index + 1 >= argc) {
                return -1;
            }
            if (parse_count_value("head", "count", argv[arg_index + 1], &options->count) != 0) {
                return -1;
            }
            options->mode = (arg[1] == 'c') ? HEAD_MODE_BYTES : HEAD_MODE_LINES;
            arg_index += 2;
            continue;
        }

        if ((arg[1] == 'n' || arg[1] == 'c') && arg[2] != '\0') {
            if (parse_count_value("head", "count", arg + 2, &options->count) != 0) {
                return -1;
            }
            options->mode = (arg[1] == 'c') ? HEAD_MODE_BYTES : HEAD_MODE_LINES;
            arg_index += 1;
            continue;
        }

        return -1;
    }

    *arg_index_out = arg_index;
    return 0;
}

static int print_head_lines(int fd, unsigned long long limit) {
    char buffer[4096];
    long bytes_read;
    unsigned long long lines_seen = 0;

    if (limit == 0) {
        return 0;
    }

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            if (rt_write_char(1, buffer[i]) != 0) {
                return -1;
            }

            if (buffer[i] == '\n') {
                lines_seen += 1;
                if (lines_seen >= limit) {
                    return 0;
                }
            }
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

static int print_head_bytes(int fd, unsigned long long limit) {
    char buffer[4096];
    long bytes_read;
    unsigned long long bytes_written = 0;

    if (limit == 0) {
        return 0;
    }

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        size_t to_write = (size_t)bytes_read;

        if (bytes_written + (unsigned long long)to_write > limit) {
            to_write = (size_t)(limit - bytes_written);
        }

        if (to_write > 0 && rt_write_all(1, buffer, to_write) != 0) {
            return -1;
        }

        bytes_written += (unsigned long long)to_write;
        if (bytes_written >= limit) {
            return 0;
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

static int print_stream(int fd, const HeadOptions *options) {
    if (options->mode == HEAD_MODE_BYTES) {
        return print_head_bytes(fd, options->count);
    }
    return print_head_lines(fd, options->count);
}

static int should_print_header(const HeadOptions *options, int path_count) {
    if (options->verbose) {
        return 1;
    }
    if (options->quiet) {
        return 0;
    }
    return path_count > 1;
}

int main(int argc, char **argv) {
    HeadOptions options;
    int arg_index = 1;
    int path_count;
    int i;
    int exit_code = 0;
    int show_header;

    if (parse_options(argc, argv, &options, &arg_index) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    path_count = argc - arg_index;
    show_header = should_print_header(&options, path_count);

    if (path_count <= 0) {
        return print_stream(0, &options) == 0 ? 0 : 1;
    }

    for (i = arg_index; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            tool_write_error("head", "cannot open ", argv[i]);
            exit_code = 1;
            continue;
        }

        if (show_header) {
            if (i > arg_index) {
                rt_write_char(1, '\n');
            }
            rt_write_cstr(1, "==> ");
            rt_write_cstr(1, argv[i]);
            rt_write_line(1, " <==");
        }

        if (print_stream(fd, &options) != 0) {
            tool_write_error("head", "read error on ", argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
