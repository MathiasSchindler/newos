#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define TAIL_BUFFER_SIZE 65536
#define TAIL_MAX_FOLLOW_FILES 32

typedef enum {
    TAIL_MODE_LINES,
    TAIL_MODE_BYTES
} TailMode;

typedef enum {
    TAIL_COUNT_LAST,
    TAIL_COUNT_FROM_START
} TailCountStyle;

typedef struct {
    TailMode mode;
    TailCountStyle style;
    unsigned long long count;
    int quiet;
    int verbose;
    int follow;
    int follow_name;
} TailOptions;

typedef struct {
    const char *path;
    int fd;
    int should_close;
    unsigned long long offset;
    unsigned long long inode;
} TailFollowState;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-n [+|-]COUNT | -c [+|-]COUNT] [-fFqv] [file ...]");
}

static int parse_count_value(const char *value_text, TailCountStyle *style_out, unsigned long long *count_out) {
    TailCountStyle style = TAIL_COUNT_LAST;

    if (value_text == 0 || value_text[0] == '\0') {
        tool_write_error("tail", "invalid ", "count");
        return -1;
    }

    if (value_text[0] == '+') {
        style = TAIL_COUNT_FROM_START;
        value_text += 1;
    } else if (value_text[0] == '-') {
        value_text += 1;
    }

    if (tool_parse_uint_arg(value_text, count_out, "tail", "count") != 0) {
        return -1;
    }

    *style_out = style;
    return 0;
}

static int is_digit_text(const char *text) {
    size_t i = 0;

    if (text == 0 || text[0] == '\0') {
        return 0;
    }

    while (text[i] != '\0') {
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        i += 1;
    }

    return 1;
}

static int parse_options(int argc, char **argv, TailOptions *options, int *arg_index_out) {
    int arg_index = 1;

    options->mode = TAIL_MODE_LINES;
    options->style = TAIL_COUNT_LAST;
    options->count = 10ULL;
    options->quiet = 0;
    options->verbose = 0;
    options->follow = 0;
    options->follow_name = 0;

    while (arg_index < argc && argv[arg_index][0] == '-' && argv[arg_index][1] != '\0') {
        const char *arg = argv[arg_index];
        const char *flag;

        if (rt_strcmp(arg, "--") == 0) {
            arg_index += 1;
            break;
        }

        if (rt_strcmp(arg, "-n") == 0 || rt_strcmp(arg, "-c") == 0) {
            if (arg_index + 1 >= argc || parse_count_value(argv[arg_index + 1], &options->style, &options->count) != 0) {
                return -1;
            }
            options->mode = (arg[1] == 'c') ? TAIL_MODE_BYTES : TAIL_MODE_LINES;
            arg_index += 2;
            continue;
        }

        if ((arg[1] == 'n' || arg[1] == 'c') && arg[2] != '\0') {
            if (parse_count_value(arg + 2, &options->style, &options->count) != 0) {
                return -1;
            }
            options->mode = (arg[1] == 'c') ? TAIL_MODE_BYTES : TAIL_MODE_LINES;
            arg_index += 1;
            continue;
        }

        if (is_digit_text(arg + 1)) {
            options->mode = TAIL_MODE_LINES;
            options->style = TAIL_COUNT_LAST;
            if (tool_parse_uint_arg(arg + 1, &options->count, "tail", "count") != 0) {
                return -1;
            }
            arg_index += 1;
            continue;
        }

        flag = arg + 1;
        while (*flag != '\0') {
            if (*flag == 'q') {
                options->quiet = 1;
                options->verbose = 0;
            } else if (*flag == 'v') {
                options->verbose = 1;
                options->quiet = 0;
            } else if (*flag == 'f') {
                options->follow = 1;
            } else if (*flag == 'F') {
                options->follow = 1;
                options->follow_name = 1;
            } else {
                return -1;
            }
            flag += 1;
        }

        arg_index += 1;
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
            lines_seen += 1ULL;
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

static int stream_to_stdout(int fd, unsigned long long *offset_out) {
    char buffer[4096];
    long bytes_read;
    unsigned long long offset = offset_out != 0 ? *offset_out : 0ULL;

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        if (rt_write_all(1, buffer, (size_t)bytes_read) != 0) {
            return -1;
        }
        offset += (unsigned long long)bytes_read;
    }

    if (offset_out != 0) {
        *offset_out = offset;
    }

    return bytes_read < 0 ? -1 : 0;
}

static int print_seekable_tail_bytes(int fd, unsigned long long byte_limit, unsigned long long *offset_out) {
    long long end = platform_seek(fd, 0, PLATFORM_SEEK_END);
    long long start = 0;
    unsigned long long offset;

    if (end < 0) {
        return -1;
    }

    if (byte_limit == 0ULL) {
        if (offset_out != 0) {
            *offset_out = (unsigned long long)end;
        }
        return 0;
    }

    if ((unsigned long long)end > byte_limit) {
        start = end - (long long)byte_limit;
    }

    if (platform_seek(fd, start, PLATFORM_SEEK_SET) < 0) {
        return -1;
    }

    offset = (unsigned long long)start;
    if (stream_to_stdout(fd, &offset) != 0) {
        return -1;
    }

    if (offset_out != 0) {
        *offset_out = offset;
    }
    return 0;
}

static int print_seekable_tail_lines(int fd, unsigned long long line_limit, unsigned long long *offset_out) {
    char buffer[4096];
    long long end = platform_seek(fd, 0, PLATFORM_SEEK_END);
    long long scan_end = end;
    long long pos;
    long long start = 0;
    unsigned long long lines_seen = 0ULL;
    unsigned long long offset;

    if (end < 0) {
        return -1;
    }

    if (line_limit == 0ULL || end == 0LL) {
        if (offset_out != 0) {
            *offset_out = (unsigned long long)end;
        }
        return 0;
    }

    if (platform_seek(fd, end - 1LL, PLATFORM_SEEK_SET) >= 0) {
        long bytes_read = platform_read(fd, buffer, 1U);
        if (bytes_read == 1 && buffer[0] == '\n') {
            scan_end -= 1LL;
        }
    }

    pos = scan_end;
    while (pos > 0LL) {
        size_t chunk_size = (size_t)(pos > (long long)sizeof(buffer) ? (long long)sizeof(buffer) : pos);
        long bytes_read;
        size_t i;

        pos -= (long long)chunk_size;
        if (platform_seek(fd, pos, PLATFORM_SEEK_SET) < 0) {
            return -1;
        }

        bytes_read = platform_read(fd, buffer, chunk_size);
        if (bytes_read < 0) {
            return -1;
        }

        for (i = (size_t)bytes_read; i > 0U; --i) {
            if (buffer[i - 1U] == '\n') {
                lines_seen += 1ULL;
                if (lines_seen >= line_limit) {
                    start = pos + (long long)i;
                    pos = 0LL;
                    break;
                }
            }
        }
    }

    if (platform_seek(fd, start, PLATFORM_SEEK_SET) < 0) {
        return -1;
    }

    offset = (unsigned long long)start;
    if (stream_to_stdout(fd, &offset) != 0) {
        return -1;
    }

    if (offset_out != 0) {
        *offset_out = offset;
    }
    return 0;
}

static int print_from_start_lines(int fd, unsigned long long start_line) {
    char buffer[4096];
    long bytes_read;
    unsigned long long current_line = 1ULL;

    if (start_line <= 1ULL) {
        start_line = 1ULL;
    }

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            if (current_line >= start_line && rt_write_char(1, buffer[i]) != 0) {
                return -1;
            }
            if (buffer[i] == '\n') {
                current_line += 1ULL;
            }
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

static int print_from_start_bytes(int fd, unsigned long long start_byte) {
    char buffer[4096];
    long bytes_read;
    unsigned long long current_byte = 1ULL;

    if (start_byte <= 1ULL) {
        start_byte = 1ULL;
    }

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            if (current_byte >= start_byte && rt_write_char(1, buffer[i]) != 0) {
                return -1;
            }
            current_byte += 1ULL;
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

static int print_from_start_bytes_seekable(int fd, unsigned long long start_byte, unsigned long long *offset_out) {
    unsigned long long offset = 0ULL;
    long long start = 0LL;

    if (start_byte > 1ULL) {
        start = (long long)(start_byte - 1ULL);
    }

    if (platform_seek(fd, start, PLATFORM_SEEK_SET) < 0) {
        return -1;
    }

    offset = (unsigned long long)start;
    if (stream_to_stdout(fd, &offset) != 0) {
        return -1;
    }

    if (offset_out != 0) {
        *offset_out = offset;
    }
    return 0;
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

static void print_header_line(const char *path, int separate) {
    if (separate) {
        rt_write_char(1, '\n');
    }
    rt_write_cstr(1, "==> ");
    rt_write_cstr(1, path);
    rt_write_line(1, " <==");
}

static void close_follow_state(TailFollowState *state) {
    if (state->fd >= 0) {
        tool_close_input(state->fd, state->should_close);
        state->fd = -1;
        state->should_close = 0;
    }
}

static int open_follow_state(TailFollowState *state) {
    PlatformDirEntry info;

    if (tool_open_input(state->path, &state->fd, &state->should_close) != 0) {
        state->fd = -1;
        return -1;
    }

    state->offset = 0ULL;
    state->inode = 0ULL;
    if (rt_strcmp(state->path, "-") != 0 && platform_get_path_info(state->path, &info) == 0) {
        state->inode = info.inode;
    }

    return 0;
}

static int poll_follow_state(TailFollowState *state,
                             const TailOptions *options,
                             int show_header,
                             int *last_output_index,
                             int index) {
    char buffer[4096];
    long bytes_read;
    int made_progress = 0;
    PlatformDirEntry info;

    if (state->fd < 0) {
        if (!options->follow_name) {
            return 0;
        }
        if (open_follow_state(state) == 0) {
            made_progress = 1;
        }
        return made_progress;
    }

    while ((bytes_read = platform_read(state->fd, buffer, sizeof(buffer))) > 0) {
        if (show_header && *last_output_index != index) {
            print_header_line(state->path, *last_output_index >= 0);
            *last_output_index = index;
        }
        if (rt_write_all(1, buffer, (size_t)bytes_read) != 0) {
            return -1;
        }
        state->offset += (unsigned long long)bytes_read;
        made_progress = 1;
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (rt_strcmp(state->path, "-") == 0) {
        return made_progress;
    }

    if (platform_get_path_info(state->path, &info) != 0) {
        if (options->follow_name) {
            close_follow_state(state);
            made_progress = 1;
        }
        return made_progress;
    }

    if (info.size < state->offset) {
        if (platform_seek(state->fd, 0, PLATFORM_SEEK_SET) >= 0) {
            state->offset = 0ULL;
            made_progress = 1;
        }
    }

    if (options->follow_name && state->inode != 0ULL && info.inode != 0ULL && info.inode != state->inode) {
        close_follow_state(state);
        if (open_follow_state(state) == 0) {
            made_progress = 1;
        }
    }

    return made_progress;
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
        if (options.style == TAIL_COUNT_FROM_START) {
            return ((options.mode == TAIL_MODE_BYTES ? print_from_start_bytes(0, options.count)
                                                     : print_from_start_lines(0, options.count)) == 0)
                       ? 0
                       : 1;
        }

        {
            char storage[TAIL_BUFFER_SIZE];
            size_t used;

            if (capture_tail_stream(0, storage, sizeof(storage), &used) != 0) {
                rt_write_line(2, "tail: read error");
                return 1;
            }

            return print_stream_result(storage, used, &options) == 0 ? 0 : 1;
        }
    }

    if (options.follow) {
        TailFollowState states[path_count > 0 ? path_count : 1];
        int state_count = 0;
        int last_output_index = show_header ? (path_count > 0 ? path_count - 1 : -1) : -1;
        int follow_error = 0;

        for (i = arg_index; i < argc; ++i) {
            TailFollowState state;
            PlatformDirEntry info;

            state.path = argv[i];
            state.fd = -1;
            state.should_close = 0;
            state.offset = 0ULL;
            state.inode = 0ULL;

            if (tool_open_input(argv[i], &state.fd, &state.should_close) != 0) {
                if (!options.follow_name) {
                    tool_write_error("tail", "cannot open ", argv[i]);
                    exit_code = 1;
                    continue;
                }
            } else {
                if (rt_strcmp(argv[i], "-") != 0 && platform_get_path_info(argv[i], &info) == 0) {
                    state.inode = info.inode;
                }

                if (show_header) {
                    print_header_line(argv[i], i > arg_index);
                }

                if (options.style == TAIL_COUNT_FROM_START) {
                    int result;

                    if (options.mode == TAIL_MODE_BYTES) {
                        result = print_from_start_bytes_seekable(state.fd, options.count, &state.offset);
                        if (result != 0) {
                            result = print_from_start_bytes(state.fd, options.count);
                            if (result == 0) {
                                long long end = platform_seek(state.fd, 0, PLATFORM_SEEK_END);
                                if (end >= 0) {
                                    state.offset = (unsigned long long)end;
                                }
                            }
                        }
                    } else {
                        result = print_from_start_lines(state.fd, options.count);
                        if (result == 0) {
                            long long end = platform_seek(state.fd, 0, PLATFORM_SEEK_END);
                            if (end >= 0) {
                                state.offset = (unsigned long long)end;
                            }
                        }
                    }

                    if (result != 0) {
                        tool_write_error("tail", "read error on ", argv[i]);
                        exit_code = 1;
                    }
                } else {
                    int result = (options.mode == TAIL_MODE_BYTES
                                      ? print_seekable_tail_bytes(state.fd, options.count, &state.offset)
                                      : print_seekable_tail_lines(state.fd, options.count, &state.offset));

                    if (result != 0) {
                        char storage[TAIL_BUFFER_SIZE];
                        size_t used;

                        if (capture_tail_stream(state.fd, storage, sizeof(storage), &used) != 0 ||
                            print_stream_result(storage, used, &options) != 0) {
                            tool_write_error("tail", "read error on ", argv[i]);
                            exit_code = 1;
                        } else {
                            long long end = platform_seek(state.fd, 0, PLATFORM_SEEK_END);
                            if (end >= 0) {
                                state.offset = (unsigned long long)end;
                            }
                        }
                    }
                }
            }

            states[state_count++] = state;
        }

        if (state_count == 0) {
            return exit_code;
        }

        for (;;) {
            int made_progress = 0;

            for (i = 0; i < state_count; ++i) {
                int poll_result = poll_follow_state(&states[i], &options, show_header, &last_output_index, i);
                if (poll_result < 0) {
                    tool_write_error("tail", "read error on ", states[i].path);
                    follow_error = 1;
                    break;
                }
                if (poll_result > 0) {
                    made_progress = 1;
                }
            }

            if (follow_error) {
                exit_code = 1;
                break;
            }

            if (!made_progress) {
                (void)platform_sleep_milliseconds(200ULL);
            }
        }

        for (i = 0; i < state_count; ++i) {
            close_follow_state(&states[i]);
        }

        return exit_code;
    }

    for (i = arg_index; i < argc; ++i) {
        int fd;
        int should_close;
        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            tool_write_error("tail", "cannot open ", argv[i]);
            exit_code = 1;
            continue;
        }

        if (show_header) {
            print_header_line(argv[i], i > arg_index);
        }

        if (options.style == TAIL_COUNT_FROM_START) {
            int result = (options.mode == TAIL_MODE_BYTES ? print_from_start_bytes_seekable(fd, options.count, 0)
                                                          : print_from_start_lines(fd, options.count));
            if (result != 0 && options.mode == TAIL_MODE_BYTES) {
                result = print_from_start_bytes(fd, options.count);
            }
            if (result != 0) {
                tool_write_error("tail", "read error on ", argv[i]);
                exit_code = 1;
            }
        } else {
            int result = (options.mode == TAIL_MODE_BYTES ? print_seekable_tail_bytes(fd, options.count, 0)
                                                          : print_seekable_tail_lines(fd, options.count, 0));
            if (result != 0) {
                char storage[TAIL_BUFFER_SIZE];
                size_t used;

                if (capture_tail_stream(fd, storage, sizeof(storage), &used) != 0 ||
                    print_stream_result(storage, used, &options) != 0) {
                    tool_write_error("tail", "read error on ", argv[i]);
                    exit_code = 1;
                }
            }
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
