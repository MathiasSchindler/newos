#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define STRINGS_BUFFER_CAPACITY 1024

typedef struct {
    size_t min_length;
    int show_offset;
    unsigned int offset_base;
} StringsOptions;

static int is_printable_byte(unsigned char ch) {
    return ch >= 32U && ch <= 126U;
}

static void format_unsigned_value(unsigned long long value, unsigned int base, char *buffer, size_t buffer_size) {
    const char *digits = "0123456789abcdef";
    char scratch[64];
    size_t length = 0;
    size_t i;

    if (buffer_size == 0U) {
        return;
    }

    if (value == 0ULL) {
        if (buffer_size > 1U) {
            buffer[0] = '0';
            buffer[1] = '\0';
        } else {
            buffer[0] = '\0';
        }
        return;
    }

    while (value != 0ULL && length + 1U < sizeof(scratch)) {
        scratch[length++] = digits[value % base];
        value /= base;
    }

    for (i = 0; i < length && i + 1U < buffer_size; ++i) {
        buffer[i] = scratch[length - 1U - i];
    }
    buffer[i] = '\0';
}

static int flush_sequence(char *buffer, size_t *length, unsigned long long start_offset, const StringsOptions *options) {
    if (*length >= options->min_length) {
        char offset_buffer[64];

        buffer[*length] = '\0';
        if (options->show_offset) {
            format_unsigned_value(start_offset, options->offset_base, offset_buffer, sizeof(offset_buffer));
            if (rt_write_cstr(1, offset_buffer) != 0 || rt_write_char(1, ' ') != 0) {
                return -1;
            }
        }
        if (rt_write_line(1, buffer) != 0) {
            return -1;
        }
    }
    *length = 0;
    return 0;
}

static int strings_stream(int fd, const StringsOptions *options) {
    unsigned char input[4096];
    char current[STRINGS_BUFFER_CAPACITY];
    size_t current_len = 0;
    unsigned long long absolute_offset = 0ULL;
    unsigned long long start_offset = 0ULL;
    long bytes_read;

    while ((bytes_read = platform_read(fd, input, sizeof(input))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            if (is_printable_byte(input[i])) {
                if (current_len == 0U) {
                    start_offset = absolute_offset;
                }
                if (current_len + 1U < sizeof(current)) {
                    current[current_len++] = (char)input[i];
                }
            } else if (flush_sequence(current, &current_len, start_offset, options) != 0) {
                return -1;
            }
            absolute_offset += 1ULL;
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    return flush_sequence(current, &current_len, start_offset, options);
}

int main(int argc, char **argv) {
    StringsOptions options;
    int argi = 1;
    int exit_code = 0;
    int i;

    options.min_length = 4U;
    options.show_offset = 0;
    options.offset_base = 10U;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-n") == 0) {
            unsigned long long parsed = 0;
            if (argi + 1 >= argc || rt_parse_uint(argv[argi + 1], &parsed) != 0 || parsed == 0ULL) {
                rt_write_line(2, "strings: invalid minimum length");
                return 1;
            }
            options.min_length = (size_t)parsed;
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-o") == 0) {
            options.show_offset = 1;
            options.offset_base = 8U;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-t") == 0) {
            if (argi + 1 >= argc) {
                rt_write_line(2, "strings: missing radix for -t");
                return 1;
            }

            if (argv[argi + 1][0] == 'd' && argv[argi + 1][1] == '\0') {
                options.offset_base = 10U;
            } else if (argv[argi + 1][0] == 'o' && argv[argi + 1][1] == '\0') {
                options.offset_base = 8U;
            } else if (argv[argi + 1][0] == 'x' && argv[argi + 1][1] == '\0') {
                options.offset_base = 16U;
            } else {
                rt_write_line(2, "strings: invalid radix for -t");
                return 1;
            }
            options.show_offset = 1;
            argi += 2;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else {
            break;
        }
    }

    if (argi == argc) {
        return strings_stream(0, &options) == 0 ? 0 : 1;
    }

    for (i = argi; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "strings: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (strings_stream(fd, &options) != 0) {
            rt_write_cstr(2, "strings: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
