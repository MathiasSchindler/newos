#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PASTE_DELIMITER_CAPACITY 64

typedef struct {
    int fd;
    int should_close;
    ToolRecordReader records;
    ToolByteBuffer line;
} LineReader;

typedef struct {
    char delimiters[PASTE_DELIMITER_CAPACITY];
    size_t delimiter_count;
    int serial_mode;
    int zero_terminated;
} PasteOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-sz] [-d DELIMS] [file ...]");
}

static int parse_delimiters(const char *text, PasteOptions *options) {
    size_t out = 0;
    size_t i = 0;

    while (text[i] != '\0') {
        char ch = text[i];

        if (ch == '\\' && text[i + 1] != '\0') {
            i += 1;
            ch = text[i];
            if (ch == 't') {
                ch = '\t';
            } else if (ch == 'n') {
                ch = '\n';
            } else if (ch == 'r') {
                ch = '\r';
            } else if (ch == 'b') {
                ch = '\b';
            } else if (ch == 'f') {
                ch = '\f';
            } else if (ch == 'v') {
                ch = '\v';
            } else if (ch == '0') {
                ch = '\0';
            }
        }

        if (out >= PASTE_DELIMITER_CAPACITY) {
            return -1;
        }
        options->delimiters[out++] = ch;
        i += 1;
    }

    options->delimiter_count = out;
    return 0;
}

static void init_reader(LineReader *reader, int fd, int should_close, char terminator) {
    reader->fd = fd;
    reader->should_close = should_close;
    tool_record_reader_init(&reader->records, fd, terminator);
    tool_byte_buffer_init(&reader->line);
}

static void free_reader(LineReader *reader) {
    tool_close_input(reader->fd, reader->should_close);
    tool_byte_buffer_free(&reader->line);
}

static char delimiter_at(const PasteOptions *options, size_t index) {
    if (options->delimiter_count == 0U) {
        return '\t';
    }

    return options->delimiters[index % options->delimiter_count];
}

static int write_delimiter(const PasteOptions *options, size_t index) {
    char delimiter = delimiter_at(options, index);

    if (delimiter == '\0') {
        return 0;
    }

    return rt_write_char(1, delimiter);
}

static int paste_parallel(ToolArray *readers, const PasteOptions *options) {
    int still_active = 1;
    char output_terminator = options->zero_terminated ? '\0' : '\n';

    while (still_active) {
        int any_line = 0;
        size_t i;

        still_active = 0;
        for (i = 0U; i < readers->count; ++i) {
            LineReader *reader = (LineReader *)tool_array_get(readers, i);
            int has_line = 0;
            if (tool_record_reader_next_buffer(&reader->records, &reader->line, &has_line) != 0) {
                return -1;
            }
            if (has_line) {
                any_line = 1;
                still_active = 1;
            }
        }

        if (!any_line) {
            break;
        }

        for (i = 0U; i < readers->count; ++i) {
            LineReader *reader = (LineReader *)tool_array_get(readers, i);
            if (i > 0) {
                if (write_delimiter(options, (size_t)(i - 1)) != 0) {
                    return -1;
                }
            }

            if (reader->line.size > 0U && rt_write_all(1, reader->line.data, reader->line.size) != 0) {
                return -1;
            }
        }

        if (rt_write_char(1, output_terminator) != 0) {
            return -1;
        }
    }

    return 0;
}

static int paste_serial(ToolArray *readers, const PasteOptions *options) {
    size_t i;
    char output_terminator = options->zero_terminated ? '\0' : '\n';

    for (i = 0U; i < readers->count; ++i) {
        LineReader *reader = (LineReader *)tool_array_get(readers, i);
        int field_index = 0;
        int has_any = 0;

        for (;;) {
            int has_line = 0;

            if (tool_record_reader_next_buffer(&reader->records, &reader->line, &has_line) != 0) {
                return -1;
            }

            if (!has_line) {
                break;
            }

            if (field_index > 0) {
                if (write_delimiter(options, (size_t)(field_index - 1)) != 0) {
                    return -1;
                }
            }

            if (reader->line.size > 0U && rt_write_all(1, reader->line.data, reader->line.size) != 0) {
                return -1;
            }

            field_index += 1;
            has_any = 1;
        }

        if (has_any || readers->count > 0U) {
            if (rt_write_char(1, output_terminator) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    PasteOptions options;
    int argi = 1;
    int exit_code = 0;
    ToolArray readers;
    size_t i;
    char output_terminator;

    options.delimiters[0] = '\t';
    options.delimiter_count = 1U;
    options.serial_mode = 0;
    options.zero_terminated = 0;
    tool_array_init(&readers, sizeof(LineReader));

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-s") == 0) {
            options.serial_mode = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-z") == 0) {
            options.zero_terminated = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-d") == 0) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            if (parse_delimiters(argv[argi + 1], &options) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else if (rt_strcmp(argv[argi], "-") == 0) {
            break;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    output_terminator = options.zero_terminated ? '\0' : '\n';

    if (argi == argc) {
        LineReader *reader = (LineReader *)tool_array_append(&readers);
        if (reader == 0) return 1;
        init_reader(reader, 0, 0, output_terminator);
    } else {
        for (; argi < argc; ++argi) {
            int fd;
            int should_close;
            LineReader *reader;

            if (tool_open_input(argv[argi], &fd, &should_close) != 0) {
                tool_write_error("paste", "cannot open ", argv[argi]);
                exit_code = 1;
                continue;
            }
            reader = (LineReader *)tool_array_append(&readers);
            if (reader == 0) {
                tool_close_input(fd, should_close);
                exit_code = 1;
                break;
            }
            init_reader(reader, fd, should_close, output_terminator);
        }
    }

    if (readers.count > 0U) {
        int result = options.serial_mode ? paste_serial(&readers, &options)
                                         : paste_parallel(&readers, &options);
        if (result != 0) {
            tool_write_error("paste", "read error", "");
            exit_code = 1;
        }
    }

    for (i = 0U; i < readers.count; ++i) {
        free_reader((LineReader *)tool_array_get(&readers, i));
    }
    tool_array_free(&readers);

    return exit_code;
}
