#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define SED_PATTERN_CAPACITY 256
#define SED_LINE_CAPACITY 8192

typedef struct {
    char old_text[SED_PATTERN_CAPACITY];
    char new_text[SED_PATTERN_CAPACITY];
    int global;
} SedCommand;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " 's/old/new/[g]' [file ...]");
}

static int parse_substitution(const char *expr, SedCommand *command) {
    char delimiter;
    size_t i = 2;
    size_t old_len = 0;
    size_t new_len = 0;

    if (expr[0] != 's' || expr[1] == '\0') {
        return -1;
    }

    delimiter = expr[1];
    rt_memset(command, 0, sizeof(*command));

    while (expr[i] != '\0' && expr[i] != delimiter && old_len + 1 < sizeof(command->old_text)) {
        command->old_text[old_len++] = expr[i++];
    }
    if (expr[i] != delimiter) {
        return -1;
    }
    command->old_text[old_len] = '\0';
    i += 1;

    while (expr[i] != '\0' && expr[i] != delimiter && new_len + 1 < sizeof(command->new_text)) {
        command->new_text[new_len++] = expr[i++];
    }
    if (expr[i] != delimiter) {
        return -1;
    }
    command->new_text[new_len] = '\0';
    i += 1;

    if (expr[i] == 'g') {
        command->global = 1;
        i += 1;
    }

    return expr[i] == '\0' ? 0 : -1;
}

static int starts_with_at(const char *text, size_t pos, const char *pattern) {
    size_t i = 0;

    while (pattern[i] != '\0') {
        if (text[pos + i] == '\0' || text[pos + i] != pattern[i]) {
            return 0;
        }
        i += 1;
    }

    return 1;
}

static int apply_substitution(const SedCommand *command, const char *input, char *output, size_t output_size) {
    size_t in_pos = 0;
    size_t out_pos = 0;
    size_t old_len = rt_strlen(command->old_text);
    size_t new_len = rt_strlen(command->new_text);
    int replaced = 0;

    if (old_len == 0) {
        if (rt_strlen(input) + 1 > output_size) {
            return -1;
        }
        memcpy(output, input, rt_strlen(input) + 1);
        return 0;
    }

    while (input[in_pos] != '\0') {
        if ((!replaced || command->global) && starts_with_at(input, in_pos, command->old_text)) {
            if (out_pos + new_len + 1 > output_size) {
                return -1;
            }
            memcpy(output + out_pos, command->new_text, new_len);
            out_pos += new_len;
            in_pos += old_len;
            replaced = 1;
        } else {
            if (out_pos + 2 > output_size) {
                return -1;
            }
            output[out_pos++] = input[in_pos++];
        }
    }

    output[out_pos] = '\0';
    return 0;
}

static int sed_stream(int fd, const SedCommand *command) {
    char chunk[4096];
    char line[SED_LINE_CAPACITY];
    char out[SED_LINE_CAPACITY];
    size_t line_len = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                line[line_len] = '\0';
                if (apply_substitution(command, line, out, sizeof(out)) != 0 || rt_write_line(1, out) != 0) {
                    return -1;
                }
                line_len = 0;
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = ch;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (line_len > 0) {
        line[line_len] = '\0';
        if (apply_substitution(command, line, out, sizeof(out)) != 0 || rt_write_line(1, out) != 0) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    SedCommand command;
    int i;
    int exit_code = 0;

    if (argc < 2 || parse_substitution(argv[1], &command) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (argc == 2) {
        return sed_stream(0, &command) == 0 ? 0 : 1;
    }

    for (i = 2; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "sed: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (sed_stream(fd, &command) != 0) {
            rt_write_cstr(2, "sed: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
