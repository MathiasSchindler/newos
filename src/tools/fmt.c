#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define FMT_DEFAULT_WIDTH 75ULL
#define FMT_MAX_LINE 2048
#define FMT_MAX_WORD 512

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-w WIDTH] [file ...]");
}

static int flush_line(char *line, size_t *line_len) {
    if (*line_len == 0U) {
        return 0;
    }

    line[*line_len] = '\0';
    if (rt_write_line(1, line) != 0) {
        return -1;
    }

    *line_len = 0U;
    return 0;
}

static int append_word(char *line, size_t *line_len, const char *word, size_t word_len, unsigned long long width) {
    size_t max_width = (width > (unsigned long long)(FMT_MAX_LINE - 1)) ? (FMT_MAX_LINE - 1U) : (size_t)width;
    size_t copy_len = word_len;

    if (copy_len >= FMT_MAX_LINE) {
        copy_len = FMT_MAX_LINE - 1U;
    }

    if (*line_len == 0U) {
        memcpy(line, word, copy_len);
        *line_len = copy_len;
        if (*line_len >= max_width) {
            return flush_line(line, line_len);
        }
        return 0;
    }

    if (*line_len + 1U + copy_len <= max_width) {
        line[*line_len] = ' ';
        memcpy(line + *line_len + 1U, word, copy_len);
        *line_len += 1U + copy_len;
        return 0;
    }

    if (flush_line(line, line_len) != 0) {
        return -1;
    }

    memcpy(line, word, copy_len);
    *line_len = copy_len;
    if (*line_len >= max_width) {
        return flush_line(line, line_len);
    }

    return 0;
}

static int format_stream(int fd, unsigned long long width) {
    char chunk[2048];
    char word[FMT_MAX_WORD];
    char line[FMT_MAX_LINE];
    size_t word_len = 0U;
    size_t line_len = 0U;
    int input_line_has_content = 0;

    for (;;) {
        long bytes_read = platform_read(fd, chunk, sizeof(chunk));
        long i;

        if (bytes_read < 0) {
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\v' || ch == '\f' || ch == '\n') {
                if (word_len > 0U) {
                    if (append_word(line, &line_len, word, word_len, width) != 0) {
                        return -1;
                    }
                    word_len = 0U;
                    input_line_has_content = 1;
                }

                if (ch == '\n') {
                    if (!input_line_has_content) {
                        if (flush_line(line, &line_len) != 0) {
                            return -1;
                        }
                        if (rt_write_char(1, '\n') != 0) {
                            return -1;
                        }
                    }
                    input_line_has_content = 0;
                }
            } else if (word_len + 1U < sizeof(word)) {
                word[word_len++] = ch;
            }
        }
    }

    if (word_len > 0U) {
        if (append_word(line, &line_len, word, word_len, width) != 0) {
            return -1;
        }
    }

    return flush_line(line, &line_len);
}

int main(int argc, char **argv) {
    unsigned long long width = FMT_DEFAULT_WIDTH;
    int argi = 1;
    int exit_code = 0;

    if (argi + 1 < argc && rt_strcmp(argv[argi], "-w") == 0) {
        if (tool_parse_uint_arg(argv[argi + 1], &width, "fmt", "width") != 0 || width == 0ULL) {
            print_usage(argv[0]);
            return 1;
        }
        argi += 2;
    }

    if (argi == argc) {
        return format_stream(0, width) == 0 ? 0 : 1;
    }

    while (argi < argc) {
        int fd;
        int should_close;

        if (tool_open_input(argv[argi], &fd, &should_close) != 0) {
            tool_write_error("fmt", "cannot open ", argv[argi]);
            exit_code = 1;
        } else {
            if (format_stream(fd, width) != 0) {
                tool_write_error("fmt", "read error on ", argv[argi]);
                exit_code = 1;
            }
            tool_close_input(fd, should_close);
        }
        argi += 1;
    }

    return exit_code;
}
