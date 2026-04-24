#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define OD_MAX_WIDTH 32U

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

static void write_padded_base(unsigned long long value, unsigned int base, unsigned int width) {
    const char *digits = "0123456789abcdef";
    char buffer[32];
    unsigned int i = 0U;

    do {
        buffer[i++] = digits[value % base];
        value /= base;
    } while (value > 0ULL && i < sizeof(buffer));

    while (i < width) {
        rt_write_char(1, '0');
        width -= 1U;
    }
    while (i > 0U) {
        i -= 1U;
        rt_write_char(1, buffer[i]);
    }
}

static void write_address(unsigned long long offset, OdAddressBase base) {
    if (base == OD_ADDRESS_NONE) {
        return;
    }
    if (base == OD_ADDRESS_DECIMAL) {
        write_padded_base(offset, 10U, 7U);
    } else if (base == OD_ADDRESS_HEX) {
        write_padded_base(offset, 16U, 7U);
    } else {
        write_padded_base(offset, 8U, 7U);
    }
}

static void write_byte(unsigned char value, OdOutputType type) {
    if (type == OD_TYPE_HEX_BYTE) {
        write_padded_base(value, 16U, 2U);
    } else if (type == OD_TYPE_DECIMAL_BYTE || type == OD_TYPE_UNSIGNED_BYTE) {
        write_padded_base(value, 10U, 3U);
    } else if (type == OD_TYPE_CHAR) {
        if (value >= 32U && value <= 126U) {
            rt_write_cstr(1, "  ");
            rt_write_char(1, (char)value);
        } else if (value == '\n') {
            rt_write_cstr(1, "\\n");
        } else if (value == '\t') {
            rt_write_cstr(1, "\\t");
        } else if (value == '\0') {
            rt_write_cstr(1, "\\0");
        } else {
            write_padded_base(value, 8U, 3U);
        }
    } else {
        write_padded_base(value, 8U, 3U);
    }
}

static int parse_number(const char *text, unsigned long long *value_out) {
    return rt_parse_uint(text, value_out);
}

static int discard_bytes(int fd, unsigned long long count) {
    unsigned char buffer[256];

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
    unsigned char buffer[OD_MAX_WIDTH];
    unsigned long long offset = options->skip;
    unsigned long long remaining = options->limit;
    long bytes_read;

    if (discard_bytes(fd, options->skip) != 0) {
        return -1;
    }

    while (!options->has_limit || remaining > 0ULL) {
        size_t want = options->width;
        long i;

        if (want > sizeof(buffer)) {
            want = sizeof(buffer);
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

        write_address(offset, options->address_base);
        for (i = 0; i < bytes_read; ++i) {
            if (options->address_base != OD_ADDRESS_NONE || i > 0) {
                rt_write_char(1, ' ');
            }
            write_byte(buffer[i], options->output_type);
        }
        rt_write_char(1, '\n');
        offset += (unsigned long long)bytes_read;
        if (options->has_limit) {
            remaining -= (unsigned long long)bytes_read;
        }
    }

    write_address(offset, options->address_base);
    if (options->address_base != OD_ADDRESS_NONE) {
        rt_write_char(1, '\n');
    }
    return 0;
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
