#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PASTE_MAX_FILES 16
#define PASTE_LINE_CAPACITY 4096

typedef struct {
    int fd;
    char chunk[4096];
    long chunk_len;
    long chunk_pos;
    int eof;
} LineReader;

typedef struct {
    const char *delimiters;
    int serial_mode;
} PasteOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-s] [-d DELIMS] [file ...]");
}

static char parse_escape_char(const char *text) {
    if (text[0] == '\\' && text[1] != '\0' && text[2] == '\0') {
        if (text[1] == 't') {
            return '\t';
        }
        if (text[1] == 'n') {
            return '\n';
        }
        if (text[1] == '0') {
            return '\0';
        }
        return text[1];
    }

    return text[0];
}

static void init_reader(LineReader *reader, int fd) {
    reader->fd = fd;
    reader->chunk_len = 0;
    reader->chunk_pos = 0;
    reader->eof = 0;
}

static int read_next_line(LineReader *reader, char *line, size_t line_size, int *has_line_out) {
    size_t line_len = 0;

    while (!reader->eof) {
        if (reader->chunk_pos >= reader->chunk_len) {
            reader->chunk_len = platform_read(reader->fd, reader->chunk, sizeof(reader->chunk));
            reader->chunk_pos = 0;

            if (reader->chunk_len < 0) {
                return -1;
            }

            if (reader->chunk_len == 0) {
                reader->eof = 1;
                break;
            }
        }

        while (reader->chunk_pos < reader->chunk_len) {
            char ch = reader->chunk[reader->chunk_pos++];

            if (ch == '\n') {
                line[line_len] = '\0';
                *has_line_out = 1;
                return 0;
            }

            if (line_len + 1U < line_size) {
                line[line_len++] = ch;
            }
        }
    }

    if (line_len > 0U) {
        line[line_len] = '\0';
        *has_line_out = 1;
        return 0;
    }

    *has_line_out = 0;
    return 0;
}

static char delimiter_at(const char *delimiters, size_t index) {
    size_t len = rt_strlen(delimiters);

    if (len == 0U) {
        return '\t';
    }

    return delimiters[index % len];
}

static int paste_parallel(LineReader *readers, int file_count, const PasteOptions *options) {
    int still_active = 1;

    while (still_active) {
        int any_line = 0;
        int i;
        char lines[PASTE_MAX_FILES][PASTE_LINE_CAPACITY];
        int has_line[PASTE_MAX_FILES];

        still_active = 0;
        for (i = 0; i < file_count; ++i) {
            has_line[i] = 0;
            if (read_next_line(&readers[i], lines[i], sizeof(lines[i]), &has_line[i]) != 0) {
                return -1;
            }

            if (has_line[i]) {
                any_line = 1;
                still_active = 1;
            }
        }

        if (!any_line) {
            break;
        }

        for (i = 0; i < file_count; ++i) {
            if (i > 0) {
                if (rt_write_char(1, delimiter_at(options->delimiters, (size_t)(i - 1))) != 0) {
                    return -1;
                }
            }

            if (has_line[i] && rt_write_cstr(1, lines[i]) != 0) {
                return -1;
            }
        }

        if (rt_write_char(1, '\n') != 0) {
            return -1;
        }
    }

    return 0;
}

static int paste_serial(LineReader *readers, int file_count, const PasteOptions *options) {
    int i;

    for (i = 0; i < file_count; ++i) {
        int field_index = 0;
        int has_any = 0;

        for (;;) {
            char line[PASTE_LINE_CAPACITY];
            int has_line = 0;

            if (read_next_line(&readers[i], line, sizeof(line), &has_line) != 0) {
                return -1;
            }

            if (!has_line) {
                break;
            }

            if (field_index > 0) {
                if (rt_write_char(1, delimiter_at(options->delimiters, (size_t)(field_index - 1))) != 0) {
                    return -1;
                }
            }

            if (rt_write_cstr(1, line) != 0) {
                return -1;
            }

            field_index += 1;
            has_any = 1;
        }

        if (has_any || file_count > 0) {
            if (rt_write_char(1, '\n') != 0) {
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
    LineReader readers[PASTE_MAX_FILES];
    int fds[PASTE_MAX_FILES];
    int should_close[PASTE_MAX_FILES];
    int file_count = 0;
    int i;

    options.delimiters = "\t";
    options.serial_mode = 0;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-s") == 0) {
            options.serial_mode = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-d") == 0) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            if (argv[argi + 1][0] == '\\' && argv[argi + 1][1] != '\0' && argv[argi + 1][2] == '\0') {
                static char single[2];
                single[0] = parse_escape_char(argv[argi + 1]);
                single[1] = '\0';
                options.delimiters = single;
            } else {
                options.delimiters = argv[argi + 1];
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

    if (argi == argc) {
        fds[0] = 0;
        should_close[0] = 0;
        file_count = 1;
    } else {
        for (; argi < argc; ++argi) {
            if (file_count >= PASTE_MAX_FILES) {
                tool_write_error("paste", "too many input files", "");
                exit_code = 1;
                break;
            }

            if (tool_open_input(argv[argi], &fds[file_count], &should_close[file_count]) != 0) {
                tool_write_error("paste", "cannot open ", argv[argi]);
                exit_code = 1;
                continue;
            }

            file_count += 1;
        }
    }

    for (i = 0; i < file_count; ++i) {
        init_reader(&readers[i], fds[i]);
    }

    if (file_count > 0) {
        int result = options.serial_mode ? paste_serial(readers, file_count, &options)
                                         : paste_parallel(readers, file_count, &options);
        if (result != 0) {
            tool_write_error("paste", "read error", "");
            exit_code = 1;
        }
    }

    for (i = 0; i < file_count; ++i) {
        tool_close_input(fds[i], should_close[i]);
    }

    return exit_code;
}
