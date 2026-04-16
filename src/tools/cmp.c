#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

int main(int argc, char **argv) {
    int fd1;
    int fd2;
    int close1;
    int close2;
    char buf1[4096];
    char buf2[4096];
    unsigned long long byte_no = 1ULL;
    unsigned long long line_no = 1ULL;

    if (argc != 3) {
        rt_write_line(2, "Usage: cmp file1 file2");
        return 1;
    }

    if (tool_open_input(argv[1], &fd1, &close1) != 0 || tool_open_input(argv[2], &fd2, &close2) != 0) {
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
                rt_write_cstr(1, "cmp: files differ at byte ");
                rt_write_uint(1, byte_no);
                rt_write_cstr(1, ", line ");
                rt_write_uint(1, line_no);
                rt_write_char(1, '\n');
                tool_close_input(fd1, close1);
                tool_close_input(fd2, close2);
                return 1;
            }
            if (buf1[i] == '\n') {
                line_no += 1ULL;
            }
            byte_no += 1ULL;
        }

        if (read1 != read2) {
            rt_write_line(1, "cmp: files differ in length");
            tool_close_input(fd1, close1);
            tool_close_input(fd2, close2);
            return 1;
        }
    }

    tool_close_input(fd1, close1);
    tool_close_input(fd2, close2);
    return 0;
}