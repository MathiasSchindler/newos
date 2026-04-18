#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void write_octal_padded(unsigned long long value, unsigned int width) {
    char digits[32];
    unsigned int i = 0;

    do {
        digits[i++] = (char)('0' + (value & 7ULL));
        value >>= 3ULL;
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

static int od_stream(int fd) {
    unsigned char buffer[16];
    unsigned long long offset = 0ULL;
    long bytes_read;

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        write_octal_padded(offset, 7U);
        for (i = 0; i < bytes_read; ++i) {
            rt_write_char(1, ' ');
            write_octal_padded(buffer[i], 3U);
        }
        rt_write_char(1, '\n');
        offset += (unsigned long long)bytes_read;
    }

    if (bytes_read < 0) {
        return -1;
    }

    write_octal_padded(offset, 7U);
    rt_write_char(1, '\n');
    return 0;
}

int main(int argc, char **argv) {
    int exit_code = 0;
    int i;

    if (argc == 1) {
        return od_stream(0) == 0 ? 0 : 1;
    }

    for (i = 1; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "od: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (od_stream(fd) != 0) {
            rt_write_cstr(2, "od: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
