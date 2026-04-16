#include "platform.h"
#include "runtime.h"

int main(int argc, char **argv) {
    int exit_code = 0;
    int i;

    if (argc == 1) {
        if (platform_stream_file_to_stdout(NULL) != 0) {
            rt_write_line(2, "cat: stdin: I/O error");
            return 1;
        }
        return 0;
    }

    for (i = 1; i < argc; ++i) {
        if (platform_stream_file_to_stdout(argv[i]) != 0) {
            rt_write_cstr(2, "cat: ");
            rt_write_cstr(2, argv[i]);
            rt_write_line(2, ": I/O error");
            exit_code = 1;
        }
    }

    return exit_code;
}
