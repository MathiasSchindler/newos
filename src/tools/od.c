#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define OD_MAX_WIDTH 32U
#define OD_IO_BUFFER_SIZE 16384U
#define OD_OUTPUT_BUFFER_SIZE 16384U

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

#define output_flush(output) tool_output_flush_buffer(1, (unsigned char *)(output)->data, &(output)->len)

#define output_append(output, source, length) tool_output_append_buffer(1, (unsigned char *)(output)->data, sizeof((output)->data), &(output)->len, (const unsigned char *)(source), (length))

static size_t append_address(char *dest, size_t dest_size, size_t pos, unsigned long long offset, OdAddressBase base) {
    if (base == OD_ADDRESS_NONE) {
        return pos;
    }
    if (base == OD_ADDRESS_DECIMAL) {
        return tool_buffer_append_padded_base(dest, dest_size, pos, offset, 10U, 7U);
    } else if (base == OD_ADDRESS_HEX) {
        return tool_buffer_append_padded_base(dest, dest_size, pos, offset, 16U, 7U);
    }
    return tool_buffer_append_padded_base(dest, dest_size, pos, offset, 8U, 7U);
}

static size_t append_byte(char *dest, size_t dest_size, size_t pos, unsigned char value, OdOutputType type) {
    if (type == OD_TYPE_HEX_BYTE) {
        return tool_buffer_append_padded_base(dest, dest_size, pos, value, 16U, 2U);
    } else if (type == OD_TYPE_DECIMAL_BYTE || type == OD_TYPE_UNSIGNED_BYTE) {
        return tool_buffer_append_padded_base(dest, dest_size, pos, value, 10U, 3U);
    } else if (type == OD_TYPE_CHAR) {
        if (value >= 32U && value <= 126U) {
            pos = tool_buffer_append_cstr(dest, dest_size, pos, "  ");
            return tool_buffer_append_char(dest, dest_size, pos, (char)value);
        } else if (value == '\n') {
            return tool_buffer_append_cstr(dest, dest_size, pos, "\\n");
        } else if (value == '\t') {
            return tool_buffer_append_cstr(dest, dest_size, pos, "\\t");
        } else if (value == '\0') {
            return tool_buffer_append_cstr(dest, dest_size, pos, "\\0");
        }
        return tool_buffer_append_padded_base(dest, dest_size, pos, value, 8U, 3U);
    }
    return tool_buffer_append_padded_base(dest, dest_size, pos, value, 8U, 3U);
}

static int od_stream(int fd, const OdOptions *options) {
    unsigned char buffer[OD_IO_BUFFER_SIZE];
    OdOutput output;
    unsigned long long offset = options->skip;
    unsigned long long remaining = options->limit;
    long bytes_read;

    output.len = 0U;

    if (tool_discard_input_bytes(fd, options->skip) != 0) {
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

            row_pos = append_address(row, sizeof(row), row_pos, offset, options->address_base);
            for (i = 0; i < (long)row_len; ++i) {
                if (options->address_base != OD_ADDRESS_NONE || i > 0) {
                    row_pos = tool_buffer_append_char(row, sizeof(row), row_pos, ' ');
                }
                row_pos = append_byte(row, sizeof(row), row_pos, buffer[cursor + (size_t)i], options->output_type);
            }
            row_pos = tool_buffer_append_char(row, sizeof(row), row_pos, '\n');
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
        size_t row_pos = append_address(row, sizeof(row), 0U, offset, options->address_base);
        row_pos = tool_buffer_append_char(row, sizeof(row), row_pos, '\n');
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
            if (rt_parse_uint(value, &options->skip) != 0) {
                return -1;
            }
        } else if (arg[1] == 'N') {
            if (rt_parse_uint(value, &options->limit) != 0) {
                return -1;
            }
            options->has_limit = 1;
        } else if (arg[1] == 'w') {
            unsigned long long width = 0ULL;
            if (rt_parse_uint(value, &width) != 0 || width == 0ULL || width > OD_MAX_WIDTH) {
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
