#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void write_hex_digit(unsigned int value) {
    rt_write_char(1, (char)(value < 10U ? ('0' + value) : ('a' + (value - 10U))));
}

static void write_hex_byte(unsigned char value) {
    write_hex_digit((unsigned int)((value >> 4) & 0x0fU));
    write_hex_digit((unsigned int)(value & 0x0fU));
}

static void write_hex_padded(unsigned long long value, unsigned int width) {
    char digits[32];
    unsigned int i = 0;

    do {
        unsigned int nibble = (unsigned int)(value & 0x0fULL);
        digits[i++] = (char)(nibble < 10U ? ('0' + nibble) : ('a' + (nibble - 10U)));
        value >>= 4ULL;
    } while (value > 0ULL && i < sizeof(digits));

    while (i < width) {
        rt_write_char(1, '0');
        width -= 1U;
    }

    while (i > 0U) {
        i -= 1U;
        rt_write_char(1, digits[i]);
    }
}

typedef struct {
    unsigned long long skip;
    unsigned long long limit;
    int has_limit;
} HexdumpOptions;

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

static int hexdump_stream(int fd, const HexdumpOptions *options) {
    unsigned char buffer[16];
    unsigned long long offset = options->skip;
    unsigned long long remaining = options->limit;
    long bytes_read;

    if (discard_bytes(fd, options->skip) != 0) {
        return -1;
    }

    while (!options->has_limit || remaining > 0ULL) {
        size_t want = sizeof(buffer);
        long i;

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

        write_hex_padded(offset, 8U);
        rt_write_cstr(1, "  ");
        for (i = 0; i < 16; ++i) {
            if (i < bytes_read) {
                write_hex_byte(buffer[i]);
            } else {
                rt_write_cstr(1, "  ");
            }
            if (i != 15) {
                rt_write_char(1, ' ');
            }
        }

        rt_write_cstr(1, "  |");
        for (i = 0; i < bytes_read; ++i) {
            unsigned char ch = buffer[i];
            rt_write_char(1, (ch >= 32U && ch <= 126U) ? (char)ch : '.');
        }
        rt_write_line(1, "|");
        offset += (unsigned long long)bytes_read;
        if (options->has_limit) {
            remaining -= (unsigned long long)bytes_read;
        }
    }

    write_hex_padded(offset, 8U);
    rt_write_char(1, '\n');
    return 0;
}

static int parse_number(const char *text, unsigned long long *value_out) {
    return rt_parse_uint(text, value_out);
}

static int parse_options(int argc, char **argv, HexdumpOptions *options, int *first_file) {
    int i = 1;

    options->skip = 0ULL;
    options->limit = 0ULL;
    options->has_limit = 0;

    while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
        const char *arg = argv[i];
        const char *value = 0;

        if (rt_strcmp(arg, "--") == 0) {
            i += 1;
            break;
        }
        if (rt_strcmp(arg, "-C") == 0 || rt_strcmp(arg, "-v") == 0) {
            i += 1;
            continue;
        }
        if (rt_strcmp(arg, "-n") == 0 || rt_strcmp(arg, "-s") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            value = argv[++i];
        } else if (arg[1] == 'n' || arg[1] == 's') {
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
        rt_write_line(2, "Usage: hexdump [-C] [-v] [-s OFFSET] [-n LENGTH] [file ...]");
        return 1;
    }

    if (first_file == argc) {
        return hexdump_stream(0, &options) == 0 ? 0 : 1;
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

        if (hexdump_stream(fd, &options) != 0) {
            rt_write_cstr(2, "hexdump: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
