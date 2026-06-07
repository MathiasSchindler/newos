#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define OD_MAX_WIDTH 32U
#define OD_IO_BUFFER_SIZE 4096U
#define OD_OUTPUT_BUFFER_SIZE 8192U

typedef enum {
    OD_ADDRESS_OCTAL,
    OD_ADDRESS_DECIMAL,
    OD_ADDRESS_HEX,
    OD_ADDRESS_NONE
} OdAddressBase;

typedef enum {
    OD_TYPE_OCTAL_BYTE,
    OD_TYPE_HEX_BYTE,
    OD_TYPE_DECIMAL_BYTE,
    OD_TYPE_UNSIGNED_BYTE,
    OD_TYPE_CHAR
} OdOutputType;

typedef struct {
    OdAddressBase address_base;
    OdOutputType output_type;
    unsigned long long skip;
    unsigned long long limit;
    int has_limit;
    unsigned int width;
} OdOptions;

typedef struct {
    char data[OD_OUTPUT_BUFFER_SIZE];
    size_t len;
} OdOutput;

static int output_flush(OdOutput *output) {
    if (output->len == 0U) {
        return 0;
    }
    if (rt_write_all(1, output->data, output->len) != 0) {
        return -1;
    }
    output->len = 0U;
    return 0;
}

static int output_append(OdOutput *output, const char *data, size_t len) {
    size_t i;

    if (len > sizeof(output->data)) {
        if (output_flush(output) != 0) {
            return -1;
        }
        return rt_write_all(1, data, len);
    }
    if (output->len + len > sizeof(output->data) && output_flush(output) != 0) {
        return -1;
    }
    for (i = 0U; i < len; ++i) {
        output->data[output->len + i] = data[i];
    }
    output->len += len;
    return 0;
}

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

static size_t append_padded_base(char *dest, size_t pos, unsigned long long value, unsigned int base, unsigned int width) {
    const char *digits = "0123456789abcdef";
    char buffer[32];
    unsigned int i = 0U;

    do {
        buffer[i++] = digits[value % base];
        value /= base;
    } while (value > 0ULL && i < sizeof(buffer));

    while (i < width) {
        dest[pos++] = '0';
        width -= 1U;
    }
    while (i > 0U) {
        i -= 1U;
        dest[pos++] = buffer[i];
    }
    return pos;
}

static size_t append_address(char *dest, size_t pos, unsigned long long offset, OdAddressBase base) {
    if (base == OD_ADDRESS_NONE) {
        return pos;
    }
    if (base == OD_ADDRESS_DECIMAL) {
        return append_padded_base(dest, pos, offset, 10U, 7U);
    } else if (base == OD_ADDRESS_HEX) {
        return append_padded_base(dest, pos, offset, 16U, 7U);
    }
    return append_padded_base(dest, pos, offset, 8U, 7U);
}

static size_t append_byte(char *dest, size_t pos, unsigned char value, OdOutputType type) {
    if (type == OD_TYPE_HEX_BYTE) {
        return append_padded_base(dest, pos, value, 16U, 2U);
    } else if (type == OD_TYPE_DECIMAL_BYTE || type == OD_TYPE_UNSIGNED_BYTE) {
        return append_padded_base(dest, pos, value, 10U, 3U);
    } else if (type == OD_TYPE_CHAR) {
        if (value >= 32U && value <= 126U) {
            pos = append_cstr(dest, pos, "  ");
            return append_char(dest, pos, (char)value);
        } else if (value == '\n') {
            return append_cstr(dest, pos, "\\n");
        } else if (value == '\t') {
            return append_cstr(dest, pos, "\\t");
        } else if (value == '\0') {
            return append_cstr(dest, pos, "\\0");
        }
        return append_padded_base(dest, pos, value, 8U, 3U);
    }
    return append_padded_base(dest, pos, value, 8U, 3U);
}

static int parse_number(const char *text, unsigned long long *value_out) {
    return rt_parse_uint(text, value_out);
}

static int discard_bytes(int fd, unsigned long long count) {
    unsigned char buffer[OD_IO_BUFFER_SIZE];

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

static int od_stream(int fd, const OdOptions *options) {
    unsigned char buffer[OD_IO_BUFFER_SIZE];
    OdOutput output;
    unsigned long long offset = options->skip;
    unsigned long long remaining = options->limit;
    long bytes_read;

    output.len = 0U;

    if (discard_bytes(fd, options->skip) != 0) {
        return -1;
    }

    while (!options->has_limit || remaining > 0ULL) {
        size_t want = options->width * (sizeof(buffer) / options->width);
        size_t cursor = 0U;

        if (want == 0U) {
            want = options->width;
        }

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
            char row[160];
            size_t row_pos = 0U;
            size_t row_len = (size_t)bytes_read - cursor;
            long i;

            if (row_len > options->width) {
                row_len = options->width;
            }

            row_pos = append_address(row, row_pos, offset, options->address_base);
            for (i = 0; i < (long)row_len; ++i) {
                if (options->address_base != OD_ADDRESS_NONE || i > 0) {
                    row_pos = append_char(row, row_pos, ' ');
                }
                row_pos = append_byte(row, row_pos, buffer[cursor + (size_t)i], options->output_type);
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

    if (options->address_base != OD_ADDRESS_NONE) {
        char row[16];
        size_t row_pos = append_address(row, 0U, offset, options->address_base);
        row_pos = append_char(row, row_pos, '\n');
        if (output_append(&output, row, row_pos) != 0) {
            return -1;
        }
    }
    return output_flush(&output);
}

static int parse_options(int argc, char **argv, OdOptions *options, int *first_file) {
    int i = 1;

    options->address_base = OD_ADDRESS_OCTAL;
    options->output_type = OD_TYPE_OCTAL_BYTE;
    options->skip = 0ULL;
    options->limit = 0ULL;
    options->has_limit = 0;
    options->width = 16U;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
        const char *arg = argv[i];
        const char *value = 0;

        if (rt_strcmp(arg, "--") == 0) {
            i += 1;
            break;
        }
        if (rt_strcmp(arg, "-A") == 0 || rt_strcmp(arg, "-t") == 0 ||
            rt_strcmp(arg, "-j") == 0 || rt_strcmp(arg, "-N") == 0 ||
            rt_strcmp(arg, "-w") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            value = argv[++i];
        } else if (arg[1] == 'A' || arg[1] == 't' || arg[1] == 'j' || arg[1] == 'N' || arg[1] == 'w') {
            value = arg + 2;
        } else {
            break;
        }

        if (arg[1] == 'A') {
            if (rt_strcmp(value, "d") == 0) {
                options->address_base = OD_ADDRESS_DECIMAL;
            } else if (rt_strcmp(value, "x") == 0) {
                options->address_base = OD_ADDRESS_HEX;
            } else if (rt_strcmp(value, "o") == 0) {
                options->address_base = OD_ADDRESS_OCTAL;
            } else if (rt_strcmp(value, "n") == 0) {
                options->address_base = OD_ADDRESS_NONE;
            } else {
                return -1;
            }
        } else if (arg[1] == 't') {
            if (rt_strcmp(value, "x1") == 0 || rt_strcmp(value, "x") == 0) {
                options->output_type = OD_TYPE_HEX_BYTE;
            } else if (rt_strcmp(value, "o1") == 0 || rt_strcmp(value, "o") == 0) {
                options->output_type = OD_TYPE_OCTAL_BYTE;
            } else if (rt_strcmp(value, "d1") == 0) {
                options->output_type = OD_TYPE_DECIMAL_BYTE;
            } else if (rt_strcmp(value, "u1") == 0 || rt_strcmp(value, "u") == 0) {
                options->output_type = OD_TYPE_UNSIGNED_BYTE;
            } else if (rt_strcmp(value, "c") == 0) {
                options->output_type = OD_TYPE_CHAR;
            } else {
                return -1;
            }
        } else if (arg[1] == 'j') {
            if (parse_number(value, &options->skip) != 0) {
                return -1;
            }
        } else if (arg[1] == 'N') {
            if (parse_number(value, &options->limit) != 0) {
                return -1;
            }
            options->has_limit = 1;
        } else if (arg[1] == 'w') {
            unsigned long long width = 0ULL;
            if (parse_number(value, &width) != 0 || width == 0ULL || width > OD_MAX_WIDTH) {
                return -1;
            }
            options->width = (unsigned int)width;
        }
        i += 1;
    }

    *first_file = i;
    return 0;
}

int main(int argc, char **argv) {
    OdOptions options;
    int first_file;
    int exit_code = 0;
    int i;

    if (parse_options(argc, argv, &options, &first_file) != 0) {
        rt_write_line(2, "Usage: od [-A d|o|x|n] [-t x1|o1|d1|u1|c] [-j SKIP] [-N COUNT] [-w WIDTH] [file ...]");
        return 1;
    }

    if (first_file == argc) {
        return od_stream(0, &options) == 0 ? 0 : 1;
    }

    for (i = first_file; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "od: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (od_stream(fd, &options) != 0) {
            rt_write_cstr(2, "od: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
