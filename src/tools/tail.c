#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define TAIL_BUFFER_SIZE 65536

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-n COUNT] [file ...]");
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

static int print_tail_buffer(const char *storage, size_t used, unsigned long long line_limit) {
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

int main(int argc, char **argv) {
    unsigned long long line_limit = 10;
    int arg_index = 1;
    int path_count;
    int i;
    int exit_code = 0;

    if (argc > 1 && rt_strcmp(argv[1], "-n") == 0) {
        if (argc < 3 || tool_parse_uint_arg(argv[2], &line_limit, "tail", "count") != 0) {
            print_usage(argv[0]);
            return 1;
        }
        arg_index = 3;
    }

    path_count = argc - arg_index;
    if (path_count <= 0) {
        char storage[TAIL_BUFFER_SIZE];
        size_t used;

        if (capture_tail_stream(0, storage, sizeof(storage), &used) != 0) {
            rt_write_line(2, "tail: read error");
            return 1;
        }

        return print_tail_buffer(storage, used, line_limit) == 0 ? 0 : 1;
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

        if (path_count > 1) {
            if (i > arg_index) {
                rt_write_char(1, '\n');
            }
            rt_write_cstr(1, "==> ");
            rt_write_cstr(1, argv[i]);
            rt_write_line(1, " <==");
        }

        if (capture_tail_stream(fd, storage, sizeof(storage), &used) != 0 || print_tail_buffer(storage, used, line_limit) != 0) {
            tool_write_error("tail", "read error on ", argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
