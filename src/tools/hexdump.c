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

static int hexdump_stream(int fd) {
    unsigned char buffer[16];
    unsigned long long offset = 0ULL;
    long bytes_read;

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

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
    }

    if (bytes_read < 0) {
        return -1;
    }

    write_hex_padded(offset, 8U);
    rt_write_char(1, '\n');
    return 0;
}

int main(int argc, char **argv) {
    int exit_code = 0;
    int i;

    if (argc == 1) {
        return hexdump_stream(0) == 0 ? 0 : 1;
    }

    for (i = 1; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "hexdump: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (hexdump_stream(fd) != 0) {
            rt_write_cstr(2, "hexdump: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}