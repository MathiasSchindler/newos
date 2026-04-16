#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define AWK_LINE_CAPACITY 8192

typedef struct {
    unsigned long long field_index;
} AwkProgram;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " '{print}' or '{print $N}' [file ...]");
}

static int parse_program(const char *program_text, AwkProgram *program) {
    size_t i = 0;
    size_t out_len = 0;
    char normalized[128];

    while (program_text[i] != '\0' && out_len + 1 < sizeof(normalized)) {
        if (!rt_is_space(program_text[i])) {
            normalized[out_len++] = program_text[i];
        }
        i += 1;
    }
    normalized[out_len] = '\0';

    if (rt_strcmp(normalized, "{print}") == 0 || rt_strcmp(normalized, "print") == 0) {
        program->field_index = 0;
        return 0;
    }

    {
        const char *dollar = normalized;
        char digits[32];
        size_t digits_len = 0;
        unsigned long long field_index;

        while (*dollar != '\0' && *dollar != '$') {
            dollar += 1;
        }

        if (*dollar == '$') {
            dollar += 1;
            while (*dollar >= '0' && *dollar <= '9' && digits_len + 1 < sizeof(digits)) {
                digits[digits_len++] = *dollar;
                dollar += 1;
            }
            digits[digits_len] = '\0';

            if (digits_len > 0 && rt_parse_uint(digits, &field_index) == 0) {
                program->field_index = field_index;
                return 0;
            }
        }
    }

    return -1;
}

static int print_field(const char *line, unsigned long long field_index) {
    size_t i = 0;
    unsigned long long current_field = 0;

    if (field_index == 0) {
        return rt_write_line(1, line);
    }

    while (line[i] != '\0') {
        size_t start;
        size_t len = 0;

        while (line[i] != '\0' && rt_is_space(line[i])) {
            i += 1;
        }
        if (line[i] == '\0') {
            break;
        }

        start = i;
        while (line[i] != '\0' && !rt_is_space(line[i])) {
            i += 1;
            len += 1;
        }

        current_field += 1;
        if (current_field == field_index) {
            if (rt_write_all(1, line + start, len) != 0 || rt_write_char(1, '\n') != 0) {
                return -1;
            }
            return 0;
        }
    }

    return rt_write_char(1, '\n');
}

static int awk_stream(int fd, const AwkProgram *program) {
    char chunk[4096];
    char line[AWK_LINE_CAPACITY];
    size_t line_len = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                line[line_len] = '\0';
                if (print_field(line, program->field_index) != 0) {
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
        if (print_field(line, program->field_index) != 0) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    AwkProgram program;
    int i;
    int exit_code = 0;

    if (argc < 2 || parse_program(argv[1], &program) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (argc == 2) {
        return awk_stream(0, &program) == 0 ? 0 : 1;
    }

    for (i = 2; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "awk: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (awk_stream(fd, &program) != 0) {
            rt_write_cstr(2, "awk: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
