#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define TAIL_BUFFER_SIZE 65536

typedef enum {
    TAIL_MODE_LINES,
    TAIL_MODE_BYTES
} TailMode;

typedef struct {
    TailMode mode;
    unsigned long long count;
    int quiet;
    int verbose;
} TailOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-n COUNT | -c COUNT] [-qv] [file ...]");
}

static int parse_count_value(const char *value_text, unsigned long long *count_out) {
    return (value_text != 0 && tool_parse_uint_arg(value_text, count_out, "tail", "count") == 0) ? 0 : -1;
}

static int parse_options(int argc, char **argv, TailOptions *options, int *arg_index_out) {
    int arg_index = 1;

    options->mode = TAIL_MODE_LINES;
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
            if (arg_index + 1 >= argc || parse_count_value(argv[arg_index + 1], &options->count) != 0) {
                return -1;
            }
            options->mode = (arg[1] == 'c') ? TAIL_MODE_BYTES : TAIL_MODE_LINES;
            arg_index += 2;
            continue;
        }

        if ((arg[1] == 'n' || arg[1] == 'c') && arg[2] != '\0') {
            if (parse_count_value(arg + 2, &options->count) != 0) {
                return -1;
            }
            options->mode = (arg[1] == 'c') ? TAIL_MODE_BYTES : TAIL_MODE_LINES;
            arg_index += 1;
            continue;
        }

        return -1;
    }

    *arg_index_out = arg_index;
    return 0;
}

static int capture_tail_stream(int fd, char *storage, size_t storage_size, size_t *used_out) {
    char chunk[4096];
    size_t used = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        size_t n = (size_t)bytes_read;

        if (n >= storage_size) {
            memcpy(storage, chunk + (n - storage_size), storage_size);
            used = storage_size;
            continue;
        }

        if (used + n > storage_size) {
            size_t drop = used + n - storage_size;
            memmove(storage, storage + drop, used - drop);
            used -= drop;
        }

        memcpy(storage + used, chunk, n);
        used += n;
    }

    if (bytes_read < 0) {
        return -1;
    }

    *used_out = used;
    return 0;
}

static int print_tail_lines(const char *storage, size_t used, unsigned long long line_limit) {
    size_t start = 0;
    size_t i = used;
    unsigned long long lines_seen = 0;

    if (line_limit == 0 || used == 0) {
        return 0;
    }

    if (i > 0 && storage[i - 1] == '\n') {
        i -= 1;
    }

    while (i > 0) {
        if (storage[i - 1] == '\n') {
            lines_seen += 1;
            if (lines_seen >= line_limit) {
                start = i;
                break;
            }
        }
        i -= 1;
    }

    return rt_write_all(1, storage + start, used - start);
}

static int print_tail_bytes(const char *storage, size_t used, unsigned long long byte_limit) {
    size_t start = 0;

    if (byte_limit == 0 || used == 0) {
        return 0;
    }

    if ((unsigned long long)used > byte_limit) {
        start = used - (size_t)byte_limit;
    }

    return rt_write_all(1, storage + start, used - start);
}

static int print_stream_result(const char *storage, size_t used, const TailOptions *options) {
    if (options->mode == TAIL_MODE_BYTES) {
        return print_tail_bytes(storage, used, options->count);
    }
    return print_tail_lines(storage, used, options->count);
}

static int should_print_header(const TailOptions *options, int path_count) {
    if (options->verbose) {
        return 1;
    }
    if (options->quiet) {
        return 0;
    }
    return path_count > 1;
}

int main(int argc, char **argv) {
    TailOptions options;
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
        char storage[TAIL_BUFFER_SIZE];
        size_t used;

        if (capture_tail_stream(0, storage, sizeof(storage), &used) != 0) {
            rt_write_line(2, "tail: read error");
            return 1;
        }

        return print_stream_result(storage, used, &options) == 0 ? 0 : 1;
    }

    for (i = arg_index; i < argc; ++i) {
        int fd;
        int should_close;
        char storage[TAIL_BUFFER_SIZE];
        size_t used;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            tool_write_error("tail", "cannot open ", argv[i]);
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

        if (capture_tail_stream(fd, storage, sizeof(storage), &used) != 0 ||
            print_stream_result(storage, used, &options) != 0) {
            tool_write_error("tail", "read error on ", argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
