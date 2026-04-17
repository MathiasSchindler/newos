#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    int silent;
    int list_all;
} CmpOptions;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-s] [-l] file1 file2");
}

static int write_difference(unsigned long long byte_no, unsigned char left, unsigned char right) {
    if (rt_write_uint(1, byte_no) != 0 ||
        rt_write_char(1, ' ') != 0 ||
        rt_write_uint(1, (unsigned long long)left) != 0 ||
        rt_write_char(1, ' ') != 0 ||
        rt_write_uint(1, (unsigned long long)right) != 0 ||
        rt_write_char(1, '\n') != 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    CmpOptions options;
    int argi = 1;
    int fd1;
    int fd2;
    int close1;
    int close2;
    char buf1[4096];
    char buf2[4096];
    unsigned long long byte_no = 1ULL;
    unsigned long long line_no = 1ULL;
    int differences = 0;

    rt_memset(&options, 0, sizeof(options));

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag = argv[argi] + 1;

        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }

        while (*flag != '\0') {
            if (*flag == 's') {
                options.silent = 1;
            } else if (*flag == 'l') {
                options.list_all = 1;
            } else {
                print_usage(argv[0]);
                return 1;
            }
            flag += 1;
        }

        argi += 1;
    }

    if (argc - argi != 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (tool_open_input(argv[argi], &fd1, &close1) != 0 || tool_open_input(argv[argi + 1], &fd2, &close2) != 0) {
        rt_write_line(2, "cmp: cannot open input files");
        return 1;
    }

    for (;;) {
        long read1 = platform_read(fd1, buf1, sizeof(buf1));
        long read2 = platform_read(fd2, buf2, sizeof(buf2));
        long i;
        long limit;

        if (read1 < 0 || read2 < 0) {
            rt_write_line(2, "cmp: read error");
            tool_close_input(fd1, close1);
            tool_close_input(fd2, close2);
            return 1;
        }

        if (read1 == 0 && read2 == 0) {
            break;
        }

        limit = (read1 < read2) ? read1 : read2;
        for (i = 0; i < limit; ++i) {
            if (buf1[i] != buf2[i]) {
                differences = 1;
                if (!options.silent) {
                    if (options.list_all) {
                        if (write_difference(byte_no, (unsigned char)buf1[i], (unsigned char)buf2[i]) != 0) {
                            tool_close_input(fd1, close1);
                            tool_close_input(fd2, close2);
                            return 1;
                        }
                    } else {
                        rt_write_cstr(1, "cmp: files differ at byte ");
                        rt_write_uint(1, byte_no);
                        rt_write_cstr(1, ", line ");
                        rt_write_uint(1, line_no);
                        rt_write_char(1, '\n');
                        tool_close_input(fd1, close1);
                        tool_close_input(fd2, close2);
                        return 1;
                    }
                }
            }
            if (buf1[i] == '\n') {
                line_no += 1ULL;
            }
            byte_no += 1ULL;
        }

        if (read1 != read2) {
            differences = 1;
            if (!options.silent) {
                rt_write_cstr(1, "cmp: EOF on ");
                rt_write_line(1, (read1 < read2) ? argv[argi] : argv[argi + 1]);
            }
            break;
        }
    }

    tool_close_input(fd1, close1);
    tool_close_input(fd2, close2);
    return differences;
}
