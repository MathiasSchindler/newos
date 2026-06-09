#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define HEXDUMP_IO_BUFFER_SIZE 16384U
#define HEXDUMP_OUTPUT_BUFFER_SIZE 16384U

typedef enum {
    HEXDUMP_FORMAT_CANONICAL,
    HEXDUMP_FORMAT_HEX16,
    HEXDUMP_FORMAT_DEC16,
    HEXDUMP_FORMAT_OCT16
} HexdumpFormat;

typedef enum {
    HEXDUMP_ADDRESS_HEX,
    HEXDUMP_ADDRESS_DECIMAL,
    HEXDUMP_ADDRESS_OCTAL,
    HEXDUMP_ADDRESS_NONE
} HexdumpAddressBase;

typedef struct {
    char data[HEXDUMP_OUTPUT_BUFFER_SIZE];
    size_t len;
} HexdumpOutput;

#define output_flush(output) tool_output_flush_buffer(1, (unsigned char *)(output)->data, &(output)->len)

#define output_append(output, source, length) tool_output_append_buffer(1, (unsigned char *)(output)->data, sizeof((output)->data), &(output)->len, (const unsigned char *)(source), (length))

static size_t append_char(char *dest, size_t pos, char ch) {
    dest[pos++] = ch;
    return pos;
}

static size_t append_cstr(char *dest, size_t pos, const char *text) {
    while (*text != '\0') {
        dest[pos++] = *text++;
    }
    return pos;
}

static size_t append_hex_byte(char *dest, size_t pos, unsigned char value) {
    const char *digits = "0123456789abcdef";
    dest[pos++] = digits[(value >> 4) & 0x0fU];
    dest[pos++] = digits[value & 0x0fU];
    return pos;
}

static size_t append_padded_base(char *dest, size_t pos, unsigned long long value, unsigned int base, unsigned int width) {
    char digits[32];
    unsigned int i = 0;
    const char *alphabet = "0123456789abcdef";

    do {
        digits[i++] = alphabet[value % base];
        value /= base;
    } while (value > 0ULL && i < sizeof(digits));

    while (i < width) {
        dest[pos++] = '0';
        width -= 1U;
    }

    while (i > 0U) {
        i -= 1U;
        dest[pos++] = digits[i];
    }
    return pos;
}

static size_t append_address(char *dest, size_t pos, unsigned long long offset, HexdumpAddressBase base) {
    if (base == HEXDUMP_ADDRESS_NONE) {
        return pos;
    }
    if (base == HEXDUMP_ADDRESS_DECIMAL) {
        return append_padded_base(dest, pos, offset, 10U, 8U);
    } else if (base == HEXDUMP_ADDRESS_OCTAL) {
        return append_padded_base(dest, pos, offset, 8U, 8U);
    }
    return append_padded_base(dest, pos, offset, 16U, 8U);
}

static size_t append_word(char *dest, size_t pos, unsigned int value, HexdumpFormat format) {
    if (format == HEXDUMP_FORMAT_DEC16) {
        return append_padded_base(dest, pos, value, 10U, 5U);
    } else if (format == HEXDUMP_FORMAT_OCT16) {
        return append_padded_base(dest, pos, value, 8U, 6U);
    }
    return append_padded_base(dest, pos, value, 16U, 4U);
}

typedef struct {
    unsigned long long skip;
    unsigned long long limit;
    int has_limit;
    HexdumpFormat format;
    HexdumpAddressBase address_base;
} HexdumpOptions;

static int discard_bytes(int fd, unsigned long long count) {
    unsigned char buffer[HEXDUMP_IO_BUFFER_SIZE];

    while (count > 0ULL) {
        size_t want = count > sizeof(buffer) ? sizeof(buffer) : (size_t)count;
        long got = platform_read(fd, buffer, want);
        if (got < 0) {
            return -1;
        }
        if (got == 0) {
            return 0;
        }
        count -= (unsigned long long)got;
    }
    return 0;
}

static int hexdump_stream(int fd, const HexdumpOptions *options) {
    unsigned char buffer[HEXDUMP_IO_BUFFER_SIZE];
    HexdumpOutput output;
    unsigned long long offset = options->skip;
    unsigned long long remaining = options->limit;
    long bytes_read;

    output.len = 0U;

    if (discard_bytes(fd, options->skip) != 0) {
        return -1;
    }

    while (!options->has_limit || remaining > 0ULL) {
        size_t want = sizeof(buffer);
        size_t cursor = 0U;

        if (options->has_limit && remaining < (unsigned long long)want) {
            want = (size_t)remaining;
        }
        bytes_read = platform_read(fd, buffer, want);
        if (bytes_read < 0) {
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }

        while (cursor < (size_t)bytes_read) {
            char row[96];
            size_t row_pos = 0U;
            size_t row_len = (size_t)bytes_read - cursor;
            long i;

            if (row_len > 16U) {
                row_len = 16U;
            }

            row_pos = append_address(row, row_pos, offset, options->address_base);
            row_pos = append_cstr(row, row_pos, "  ");
            for (i = 0; i < 16; ++i) {
                if (i < (long)row_len) {
                    row_pos = append_hex_byte(row, row_pos, buffer[cursor + (size_t)i]);
                } else {
                    row_pos = append_cstr(row, row_pos, "  ");
                }
                if (i != 15) {
                    row_pos = append_char(row, row_pos, ' ');
                }
            }

            row_pos = append_cstr(row, row_pos, "  |");
            for (i = 0; i < (long)row_len; ++i) {
                unsigned char ch = buffer[cursor + (size_t)i];
                row_pos = append_char(row, row_pos, (ch >= 32U && ch <= 126U) ? (char)ch : '.');
            }
            row_pos = append_cstr(row, row_pos, "|\n");
            if (output_append(&output, row, row_pos) != 0) {
                return -1;
            }
            offset += (unsigned long long)row_len;
            cursor += row_len;
        }
        if (options->has_limit) {
            remaining -= (unsigned long long)bytes_read;
        }
    }

    if (options->address_base != HEXDUMP_ADDRESS_NONE) {
        char row[16];
        size_t row_pos = append_address(row, 0U, offset, options->address_base);
        row_pos = append_char(row, row_pos, '\n');
        if (output_append(&output, row, row_pos) != 0) {
            return -1;
        }
    }
    return output_flush(&output);
}

static int hexdump_words_stream(int fd, const HexdumpOptions *options) {
    unsigned char buffer[HEXDUMP_IO_BUFFER_SIZE];
    HexdumpOutput output;
    unsigned long long offset = options->skip;
    unsigned long long remaining = options->limit;
    long bytes_read;

    output.len = 0U;

    if (discard_bytes(fd, options->skip) != 0) {
        return -1;
    }

    while (!options->has_limit || remaining > 0ULL) {
        size_t want = sizeof(buffer);
        size_t cursor = 0U;

        if (options->has_limit && remaining < (unsigned long long)want) {
            want = (size_t)remaining;
        }
        bytes_read = platform_read(fd, buffer, want);
        if (bytes_read < 0) {
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }

        while (cursor < (size_t)bytes_read) {
            char row[96];
            size_t row_pos = 0U;
            size_t row_len = (size_t)bytes_read - cursor;
            long i;

            if (row_len > 16U) {
                row_len = 16U;
            }

            row_pos = append_address(row, row_pos, offset, options->address_base);
            for (i = 0; i < (long)row_len; i += 2) {
                unsigned int value = buffer[cursor + (size_t)i];
                if (i + 1 < (long)row_len) {
                    value |= (unsigned int)buffer[cursor + (size_t)i + 1U] << 8U;
                }
                if (options->address_base != HEXDUMP_ADDRESS_NONE || i > 0) {
                    row_pos = append_char(row, row_pos, ' ');
                }
                row_pos = append_word(row, row_pos, value, options->format);
            }
            row_pos = append_char(row, row_pos, '\n');
            if (output_append(&output, row, row_pos) != 0) {
                return -1;
            }
            offset += (unsigned long long)row_len;
            cursor += row_len;
        }
        if (options->has_limit) {
            remaining -= (unsigned long long)bytes_read;
        }
    }

    if (options->address_base != HEXDUMP_ADDRESS_NONE) {
        char row[16];
        size_t row_pos = append_address(row, 0U, offset, options->address_base);
        row_pos = append_char(row, row_pos, '\n');
        if (output_append(&output, row, row_pos) != 0) {
            return -1;
        }
    }
    return output_flush(&output);
}

static int dump_stream(int fd, const HexdumpOptions *options) {
    if (options->format == HEXDUMP_FORMAT_CANONICAL) {
        return hexdump_stream(fd, options);
    }
    return hexdump_words_stream(fd, options);
}

static int parse_number(const char *text, unsigned long long *value_out) {
    return rt_parse_uint(text, value_out);
}

static int parse_options(int argc, char **argv, HexdumpOptions *options, int *first_file) {
    int i = 1;

    options->skip = 0ULL;
    options->limit = 0ULL;
    options->has_limit = 0;
    options->format = HEXDUMP_FORMAT_CANONICAL;
    options->address_base = HEXDUMP_ADDRESS_HEX;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
        const char *arg = argv[i];
        const char *value = 0;

        if (rt_strcmp(arg, "--") == 0) {
            i += 1;
            break;
        }
        if (rt_strcmp(arg, "-C") == 0 || rt_strcmp(arg, "-v") == 0) {
            if (rt_strcmp(arg, "-C") == 0) {
                options->format = HEXDUMP_FORMAT_CANONICAL;
            }
            i += 1;
            continue;
        }
        if (rt_strcmp(arg, "-x") == 0 || rt_strcmp(arg, "-d") == 0 || rt_strcmp(arg, "-o") == 0) {
            if (rt_strcmp(arg, "-x") == 0) {
                options->format = HEXDUMP_FORMAT_HEX16;
            } else if (rt_strcmp(arg, "-d") == 0) {
                options->format = HEXDUMP_FORMAT_DEC16;
            } else {
                options->format = HEXDUMP_FORMAT_OCT16;
            }
            i += 1;
            continue;
        }
        if (rt_strcmp(arg, "-n") == 0 || rt_strcmp(arg, "-s") == 0 || rt_strcmp(arg, "-A") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            value = argv[++i];
        } else if (arg[1] == 'n' || arg[1] == 's' || arg[1] == 'A') {
            value = arg + 2;
        } else {
            break;
        }

        if (arg[1] == 'n') {
            if (parse_number(value, &options->limit) != 0) {
                return -1;
            }
            options->has_limit = 1;
        } else if (arg[1] == 's') {
            if (parse_number(value, &options->skip) != 0) {
                return -1;
            }
        } else if (arg[1] == 'A') {
            if (rt_strcmp(value, "x") == 0) {
                options->address_base = HEXDUMP_ADDRESS_HEX;
            } else if (rt_strcmp(value, "d") == 0) {
                options->address_base = HEXDUMP_ADDRESS_DECIMAL;
            } else if (rt_strcmp(value, "o") == 0) {
                options->address_base = HEXDUMP_ADDRESS_OCTAL;
            } else if (rt_strcmp(value, "n") == 0) {
                options->address_base = HEXDUMP_ADDRESS_NONE;
            } else {
                return -1;
            }
        }
        i += 1;
    }

    *first_file = i;
    return 0;
}

int main(int argc, char **argv) {
    HexdumpOptions options;
    int first_file;
    int exit_code = 0;
    int i;

    if (parse_options(argc, argv, &options, &first_file) != 0) {
        rt_write_line(2, "Usage: hexdump [-C|-x|-d|-o] [-A BASE] [-v] [-s OFFSET] [-n LENGTH] [file ...]");
        return 1;
    }

    if (first_file == argc) {
        return dump_stream(0, &options) == 0 ? 0 : 1;
    }

    for (i = first_file; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "hexdump: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (dump_stream(fd, &options) != 0) {
            rt_write_cstr(2, "hexdump: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
