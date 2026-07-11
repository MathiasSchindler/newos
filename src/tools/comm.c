#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    int suppress[3];
} CommOptions;

typedef struct {
    int fd;
    int should_close;
    ToolRecordReader records;
    ToolByteBuffer line;
} CommReader;

static void print_usage(void) {
    tool_write_usage("comm", "[-123] FILE1 FILE2");
}

static void comm_reader_init(CommReader *reader) {
    rt_memset(reader, 0, sizeof(*reader));
    reader->fd = -1;
    tool_byte_buffer_init(&reader->line);
}

static int comm_reader_open(CommReader *reader, const char *path) {
    comm_reader_init(reader);
    if (tool_open_input(path, &reader->fd, &reader->should_close) != 0) return -1;
    tool_record_reader_init(&reader->records, reader->fd, '\n');
    return 0;
}

static void comm_reader_close(CommReader *reader) {
    tool_close_input(reader->fd, reader->should_close);
    tool_byte_buffer_free(&reader->line);
    reader->fd = -1;
}

static int comm_reader_next(CommReader *reader) {
    int has_record = 0;
    if (tool_record_reader_next_buffer(&reader->records, &reader->line, &has_record) != 0) return -1;
    return has_record;
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
            cmp = compare_lines((const char *)left_reader.line.data, (const char *)right_reader.line.data);
        }

        if (cmp < 0) {
            if (!options.suppress[0] && emit_line(&output, &options, 1, (const char *)left_reader.line.data) != 0) {
                return 1;
            }
            left_status = comm_reader_next(&left_reader);
        } else if (cmp > 0) {
            if (!options.suppress[1] && emit_line(&output, &options, 2, (const char *)right_reader.line.data) != 0) {
                return 1;
            }
            right_status = comm_reader_next(&right_reader);
        } else {
            if (!options.suppress[2] && emit_line(&output, &options, 3, (const char *)left_reader.line.data) != 0) {
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
