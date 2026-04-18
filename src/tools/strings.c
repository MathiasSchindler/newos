#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define STRINGS_BUFFER_CAPACITY 1024

typedef enum {
    STRINGS_ENCODING_7BIT,
    STRINGS_ENCODING_8BIT,
    STRINGS_ENCODING_16LE,
    STRINGS_ENCODING_16BE,
    STRINGS_ENCODING_32LE,
    STRINGS_ENCODING_32BE
} StringsEncoding;

typedef struct {
    size_t min_length;
    int show_offset;
    unsigned int offset_base;
    int print_file_name;
    StringsEncoding encoding;
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

static size_t strings_unit_size(StringsEncoding encoding) {
    if (encoding == STRINGS_ENCODING_16LE || encoding == STRINGS_ENCODING_16BE) {
        return 2U;
    }
    if (encoding == STRINGS_ENCODING_32LE || encoding == STRINGS_ENCODING_32BE) {
        return 4U;
    }
    return 1U;
}

static int decode_string_unit(const unsigned char *unit, StringsEncoding encoding, char *out) {
    if (encoding == STRINGS_ENCODING_7BIT) {
        if (unit[0] > 127U || !is_printable_byte(unit[0])) {
            return 0;
        }
        *out = (char)unit[0];
        return 1;
    }

    if (encoding == STRINGS_ENCODING_8BIT) {
        if (!is_printable_byte(unit[0])) {
            return 0;
        }
        *out = (char)unit[0];
        return 1;
    }

    if (encoding == STRINGS_ENCODING_16LE) {
        if (unit[1] != 0U || !is_printable_byte(unit[0])) {
            return 0;
        }
        *out = (char)unit[0];
        return 1;
    }

    if (encoding == STRINGS_ENCODING_16BE) {
        if (unit[0] != 0U || !is_printable_byte(unit[1])) {
            return 0;
        }
        *out = (char)unit[1];
        return 1;
    }

    if (encoding == STRINGS_ENCODING_32LE) {
        if (unit[1] != 0U || unit[2] != 0U || unit[3] != 0U || !is_printable_byte(unit[0])) {
            return 0;
        }
        *out = (char)unit[0];
        return 1;
    }

    if (unit[0] != 0U || unit[1] != 0U || unit[2] != 0U || !is_printable_byte(unit[3])) {
        return 0;
    }
    *out = (char)unit[3];
    return 1;
}

static int flush_sequence(char *buffer,
                          size_t *length,
                          unsigned long long start_offset,
                          const StringsOptions *options,
                          const char *file_name) {
    if (*length >= options->min_length) {
        char offset_buffer[64];

        buffer[*length] = '\0';
        if (options->print_file_name && file_name != 0) {
            if (rt_write_cstr(1, file_name) != 0 || rt_write_cstr(1, ": ") != 0) {
                return -1;
            }
        }
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
    *length = 0U;
    return 0;
}

static int strings_stream(int fd, const StringsOptions *options, const char *file_name) {
    unsigned char chunk[4096];
    unsigned char buffered[4096 + 4];
    char current[STRINGS_BUFFER_CAPACITY];
    size_t current_len = 0U;
    unsigned long long absolute_offset = 0ULL;
    unsigned long long start_offset = 0ULL;
    size_t pending = 0U;
    size_t unit_size = strings_unit_size(options->encoding);
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        size_t total = pending + (size_t)bytes_read;
        size_t i = 0U;

        memcpy(buffered + pending, chunk, (size_t)bytes_read);

        while (i + unit_size <= total) {
            char decoded = '\0';

            if (decode_string_unit(buffered + i, options->encoding, &decoded)) {
                if (current_len == 0U) {
                    start_offset = absolute_offset;
                }
                if (current_len + 1U < sizeof(current)) {
                    current[current_len++] = decoded;
                }
            } else if (flush_sequence(current, &current_len, start_offset, options, file_name) != 0) {
                return -1;
            }

            absolute_offset += (unsigned long long)unit_size;
            i += unit_size;
        }

        pending = total - i;
        if (pending > 0U) {
            memmove(buffered, buffered + i, pending);
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    return flush_sequence(current, &current_len, start_offset, options, file_name);
}

int main(int argc, char **argv) {
    StringsOptions options;
    int argi = 1;
    int exit_code = 0;
    int i;

    options.min_length = 4U;
    options.show_offset = 0;
    options.offset_base = 10U;
    options.print_file_name = 0;
    options.encoding = STRINGS_ENCODING_8BIT;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-n") == 0 || (argv[argi][1] == 'n' && argv[argi][2] != '\0')) {
            unsigned long long parsed = 0ULL;
            const char *value_text = (argv[argi][2] != '\0') ? argv[argi] + 2 : ((argi + 1 < argc) ? argv[argi + 1] : 0);

            if (value_text == 0 || rt_parse_uint(value_text, &parsed) != 0 || parsed == 0ULL) {
                rt_write_line(2, "strings: invalid minimum length");
                return 1;
            }
            options.min_length = (size_t)parsed;
            argi += (argv[argi][2] != '\0') ? 1 : 2;
        } else if (argv[argi][1] >= '0' && argv[argi][1] <= '9') {
            unsigned long long parsed = 0ULL;
            if (rt_parse_uint(argv[argi] + 1, &parsed) != 0 || parsed == 0ULL) {
                rt_write_line(2, "strings: invalid minimum length");
                return 1;
            }
            options.min_length = (size_t)parsed;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-o") == 0) {
            options.show_offset = 1;
            options.offset_base = 8U;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-f") == 0) {
            options.print_file_name = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-t") == 0 || (argv[argi][1] == 't' && argv[argi][2] != '\0')) {
            const char *radix = (argv[argi][2] != '\0') ? argv[argi] + 2 : ((argi + 1 < argc) ? argv[argi + 1] : 0);

            if (radix == 0) {
                rt_write_line(2, "strings: missing radix for -t");
                return 1;
            }

            if (radix[0] == 'd' && radix[1] == '\0') {
                options.offset_base = 10U;
            } else if (radix[0] == 'o' && radix[1] == '\0') {
                options.offset_base = 8U;
            } else if (radix[0] == 'x' && radix[1] == '\0') {
                options.offset_base = 16U;
            } else {
                rt_write_line(2, "strings: invalid radix for -t");
                return 1;
            }
            options.show_offset = 1;
            argi += (argv[argi][2] != '\0') ? 1 : 2;
        } else if (rt_strcmp(argv[argi], "-e") == 0 || (argv[argi][1] == 'e' && argv[argi][2] != '\0')) {
            const char *encoding = (argv[argi][2] != '\0') ? argv[argi] + 2 : ((argi + 1 < argc) ? argv[argi + 1] : 0);

            if (encoding == 0 || encoding[1] != '\0') {
                rt_write_line(2, "strings: invalid encoding for -e");
                return 1;
            }

            if (encoding[0] == 's') {
                options.encoding = STRINGS_ENCODING_7BIT;
            } else if (encoding[0] == 'S') {
                options.encoding = STRINGS_ENCODING_8BIT;
            } else if (encoding[0] == 'l') {
                options.encoding = STRINGS_ENCODING_16LE;
            } else if (encoding[0] == 'b') {
                options.encoding = STRINGS_ENCODING_16BE;
            } else if (encoding[0] == 'L') {
                options.encoding = STRINGS_ENCODING_32LE;
            } else if (encoding[0] == 'B') {
                options.encoding = STRINGS_ENCODING_32BE;
            } else {
                rt_write_line(2, "strings: invalid encoding for -e");
                return 1;
            }

            argi += (argv[argi][2] != '\0') ? 1 : 2;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else {
            break;
        }
    }

    if (argi == argc) {
        return strings_stream(0, &options, 0) == 0 ? 0 : 1;
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

        if (strings_stream(fd, &options, argv[i]) != 0) {
            rt_write_cstr(2, "strings: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
