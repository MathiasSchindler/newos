#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    int number_all;
    int number_nonblank;
    int squeeze_blank;
} CatOptions;

typedef struct {
    unsigned long long next_line_number;
    int at_line_start;
    unsigned int blank_run;
} CatState;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-n] [-b] [-s] [file ...]");
}

static int write_line_number(unsigned long long value) {
    char digits[32];
    size_t digits_len;
    size_t padding;

    rt_unsigned_to_string(value, digits, sizeof(digits));
    digits_len = rt_strlen(digits);
    padding = (digits_len < 6U) ? (6U - digits_len) : 0U;

    while (padding > 0U) {
        if (rt_write_char(1, ' ') != 0) {
            return -1;
        }
        padding -= 1U;
    }

    if (rt_write_cstr(1, digits) != 0) {
        return -1;
    }
    return rt_write_char(1, '\t');
}

static int cat_from_fd(int fd, const CatOptions *options, CatState *state) {
    char buffer[4096];

    for (;;) {
        long bytes_read = platform_read(fd, buffer, sizeof(buffer));
        long i;

        if (bytes_read < 0) {
            return -1;
        }
        if (bytes_read == 0) {
            return 0;
        }

        for (i = 0; i < bytes_read; ++i) {
            char ch = buffer[i];
            int blank_line = state->at_line_start && ch == '\n';

            if (options->squeeze_blank && blank_line && state->blank_run > 0U) {
                continue;
            }

            if (state->at_line_start) {
                if (options->number_nonblank) {
                    if (ch != '\n') {
                        if (write_line_number(state->next_line_number++) != 0) {
                            return -1;
                        }
                    }
                } else if (options->number_all) {
                    if (write_line_number(state->next_line_number++) != 0) {
                        return -1;
                    }
                }
            }

            if (rt_write_char(1, ch) != 0) {
                return -1;
            }

            if (ch == '\n') {
                if (blank_line) {
                    state->blank_run += 1U;
                } else {
                    state->blank_run = 0U;
                }
                state->at_line_start = 1;
            } else {
                state->blank_run = 0U;
                state->at_line_start = 0;
            }
        }
    }
}

static int cat_path(const char *path, const CatOptions *options, CatState *state) {
    int fd;
    int should_close;
    int result;

    if (tool_open_input(path, &fd, &should_close) != 0) {
        return -1;
    }

    result = cat_from_fd(fd, options, state);
    tool_close_input(fd, should_close);
    return result;
}

int main(int argc, char **argv) {
    CatOptions options;
    CatState state;
    int argi = 1;
    int exit_code = 0;
    int i;

    rt_memset(&options, 0, sizeof(options));
    rt_memset(&state, 0, sizeof(state));
    state.next_line_number = 1ULL;
    state.at_line_start = 1;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag = argv[argi] + 1;

        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }

        while (*flag != '\0') {
            if (*flag == 'n') {
                options.number_all = 1;
            } else if (*flag == 'b') {
                options.number_all = 0;
                options.number_nonblank = 1;
            } else if (*flag == 's') {
                options.squeeze_blank = 1;
            } else {
                print_usage(argv[0]);
                return 1;
            }
            flag += 1;
        }

        argi += 1;
    }

    if (argi >= argc) {
        if (cat_path(NULL, &options, &state) != 0) {
            rt_write_line(2, "cat: stdin: I/O error");
            return 1;
        }
        return 0;
    }

    for (i = argi; i < argc; ++i) {
        if (cat_path(argv[i], &options, &state) != 0) {
            rt_write_cstr(2, "cat: ");
            rt_write_cstr(2, argv[i]);
            rt_write_line(2, ": I/O error");
            exit_code = 1;
        }
    }

    return exit_code;
}
