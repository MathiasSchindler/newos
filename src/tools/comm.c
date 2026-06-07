#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define COMM_LINE_CAPACITY 2048
#define COMM_INPUT_BUFFER_SIZE 8192

typedef struct {
    int suppress[3];
} CommOptions;

typedef struct {
    int fd;
    int should_close;
    int eof;
    char buffer[COMM_INPUT_BUFFER_SIZE];
    size_t buffer_pos;
    size_t buffer_len;
    char line[COMM_LINE_CAPACITY];
    size_t line_len;
} CommReader;

static void print_usage(void) {
    tool_write_usage("comm", "[-123] FILE1 FILE2");
}

static void comm_reader_init(CommReader *reader) {
    rt_memset(reader, 0, sizeof(*reader));
    reader->fd = -1;
}

static int comm_reader_open(CommReader *reader, const char *path) {
    comm_reader_init(reader);
    return tool_open_input(path, &reader->fd, &reader->should_close);
}

static void comm_reader_close(CommReader *reader) {
    tool_close_input(reader->fd, reader->should_close);
    reader->fd = -1;
}

static int comm_reader_next(CommReader *reader) {
    reader->line_len = 0U;

    for (;;) {
        if (reader->buffer_pos >= reader->buffer_len) {
            long bytes_read;

            if (reader->eof) {
                if (reader->line_len > 0U) {
                    reader->line[reader->line_len] = '\0';
                    return 1;
                }
                return 0;
            }

            bytes_read = platform_read(reader->fd, reader->buffer, sizeof(reader->buffer));
            if (bytes_read < 0) {
                return -1;
            }
            if (bytes_read == 0) {
                reader->eof = 1;
                continue;
            }
            reader->buffer_pos = 0U;
            reader->buffer_len = (size_t)bytes_read;
        }

        while (reader->buffer_pos < reader->buffer_len) {
            size_t start = reader->buffer_pos;
            size_t span;

            while (reader->buffer_pos < reader->buffer_len && reader->buffer[reader->buffer_pos] != '\n') {
                reader->buffer_pos += 1U;
            }
            span = reader->buffer_pos - start;
            if (span > 0U) {
                size_t available = (sizeof(reader->line) - 1U) - reader->line_len;
                if (span > available) {
                    span = available;
                }
                if (span > 0U) {
                    memcpy(reader->line + reader->line_len, reader->buffer + start, span);
                    reader->line_len += span;
                }
            }
            if (reader->buffer_pos < reader->buffer_len && reader->buffer[reader->buffer_pos] == '\n') {
                reader->buffer_pos += 1U;
                reader->line[reader->line_len] = '\0';
                return 1;
            }
        }
    }
}

static int emit_line(ToolOutputBuffer *output, const CommOptions *options, int column, const char *line) {
    int previous;

    for (previous = 1; previous < column; ++previous) {
        if (!options->suppress[previous - 1]) {
            if (tool_output_buffer_write_char(output, '\t') != 0) {
                return -1;
            }
        }
    }
    if (tool_output_buffer_write_cstr(output, line) != 0 || tool_output_buffer_write_char(output, '\n') != 0) {
        return -1;
    }
    return 0;
}

static int compare_lines(const char *left, const char *right) {
    int cmp = rt_strcmp(left, right);

    if (cmp < 0) {
        return -1;
    }
    if (cmp > 0) {
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    CommOptions options;
    int argi = 1;
    CommReader left_reader;
    CommReader right_reader;
    ToolOutputBuffer output;
    int left_status;
    int right_status;

    rt_memset(&options, 0, sizeof(options));

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *arg = argv[argi];
        size_t i;

        if (rt_strcmp(arg, "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(arg, "--help") == 0) {
            print_usage();
            return 0;
        }
        if (rt_strcmp(arg, "-") == 0) {
            break;
        }

        for (i = 1U; arg[i] != '\0'; ++i) {
            if (arg[i] >= '1' && arg[i] <= '3') {
                options.suppress[arg[i] - '1'] = 1;
            } else {
                print_usage();
                return 1;
            }
        }
        argi += 1;
    }

    if (argc - argi != 2) {
        print_usage();
        return 1;
    }

    if (comm_reader_open(&left_reader, argv[argi]) != 0) {
        tool_write_error("comm", "cannot read ", argv[argi]);
        return 1;
    }
    if (comm_reader_open(&right_reader, argv[argi + 1]) != 0) {
        comm_reader_close(&left_reader);
        tool_write_error("comm", "cannot read ", argv[argi + 1]);
        return 1;
    }

    tool_output_buffer_init(&output, 1);
    left_status = comm_reader_next(&left_reader);
    right_status = comm_reader_next(&right_reader);

    while (left_status > 0 || right_status > 0) {
        int cmp;

        if (left_status <= 0) {
            cmp = 1;
        } else if (right_status <= 0) {
            cmp = -1;
        } else {
            cmp = compare_lines(left_reader.line, right_reader.line);
        }

        if (cmp < 0) {
            if (!options.suppress[0] && emit_line(&output, &options, 1, left_reader.line) != 0) {
                return 1;
            }
            left_status = comm_reader_next(&left_reader);
        } else if (cmp > 0) {
            if (!options.suppress[1] && emit_line(&output, &options, 2, right_reader.line) != 0) {
                return 1;
            }
            right_status = comm_reader_next(&right_reader);
        } else {
            if (!options.suppress[2] && emit_line(&output, &options, 3, left_reader.line) != 0) {
                return 1;
            }
            left_status = comm_reader_next(&left_reader);
            right_status = comm_reader_next(&right_reader);
        }

        if (left_status < 0 || right_status < 0) {
            tool_write_error("comm", "read error", 0);
            comm_reader_close(&left_reader);
            comm_reader_close(&right_reader);
            return 1;
        }
    }

    comm_reader_close(&left_reader);
    comm_reader_close(&right_reader);
    return tool_output_buffer_flush(&output) == 0 ? 0 : 1;
}
