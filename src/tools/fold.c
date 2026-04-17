#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define FOLD_LINE_CAPACITY 8192

typedef struct {
    unsigned long long width;
    int break_spaces;
} FoldOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-s] [-w WIDTH] [file ...]");
}

static int flush_prefix(const char *buffer, size_t count) {
    if (count == 0U) {
        return 0;
    }

    if (rt_write_all(1, buffer, count) != 0 || rt_write_char(1, '\n') != 0) {
        return -1;
    }

    return 0;
}

static int fold_stream(int fd, const FoldOptions *options) {
    char chunk[4096];
    char line[FOLD_LINE_CAPACITY];
    size_t line_len = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                if (line_len > 0U && rt_write_all(1, line, line_len) != 0) {
                    return -1;
                }
                if (rt_write_char(1, '\n') != 0) {
                    return -1;
                }
                line_len = 0;
                continue;
            }

            if (line_len + 1U < sizeof(line)) {
                line[line_len++] = ch;
            }

            if (line_len >= (size_t)options->width) {
                size_t split = line_len;

                if (options->break_spaces) {
                    while (split > 0U && line[split - 1U] != ' ' && line[split - 1U] != '\t') {
                        split -= 1U;
                    }
                }

                if (split == 0U || split == line_len) {
                    split = (size_t)options->width;
                }

                if (flush_prefix(line, split) != 0) {
                    return -1;
                }

                if (split < line_len) {
                    size_t keep_len = line_len - split;
                    size_t j;

                    for (j = 0; j < keep_len; ++j) {
                        line[j] = line[split + j];
                    }
                    line_len = keep_len;
                } else {
                    line_len = 0;
                }

                while (line_len > 0U && (line[0] == ' ' || line[0] == '\t')) {
                    size_t j;
                    for (j = 1U; j < line_len; ++j) {
                        line[j - 1U] = line[j];
                    }
                    line_len -= 1U;
                    if (!options->break_spaces) {
                        break;
                    }
                }
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (line_len > 0U) {
        if (rt_write_all(1, line, line_len) != 0) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    FoldOptions options;
    int argi = 1;
    int exit_code = 0;

    options.width = 80ULL;
    options.break_spaces = 0;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-s") == 0) {
            options.break_spaces = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-w") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &options.width, "fold", "width") != 0 || options.width == 0ULL) {
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

    if (argi == argc) {
        return fold_stream(0, &options) == 0 ? 0 : 1;
    }

    for (; argi < argc; ++argi) {
        int fd;
        int should_close;

        if (tool_open_input(argv[argi], &fd, &should_close) != 0) {
            tool_write_error("fold", "cannot open ", argv[argi]);
            exit_code = 1;
            continue;
        }

        if (fold_stream(fd, &options) != 0) {
            tool_write_error("fold", "read error on ", argv[argi]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
