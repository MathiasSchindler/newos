#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static int looks_like_text(const unsigned char *buffer, size_t length) {
    size_t i;

    if (length == 0U) {
        return 1;
    }

    for (i = 0; i < length; ++i) {
        unsigned char ch = buffer[i];
        if (ch == 0U) {
            return 0;
        }
        if ((ch < 32U || ch > 126U) && ch != '\n' && ch != '\r' && ch != '\t') {
            return 0;
        }
    }

    return 1;
}

static const char *detect_type(const unsigned char *buffer, size_t length) {
    if (length == 0U) {
        return "empty";
    }
    if (length >= 4U && buffer[0] == 0x7fU && buffer[1] == 'E' && buffer[2] == 'L' && buffer[3] == 'F') {
        return "ELF binary";
    }
    if (length >= 2U && buffer[0] == 0x1fU && buffer[1] == 0x8bU) {
        return "gzip compressed data";
    }
    if (length >= 4U && buffer[0] == 'B' && buffer[1] == 'Z' && buffer[2] == 'h') {
        return "bzip2 compressed data";
    }
    if (length >= 6U && buffer[0] == 0xfdU && buffer[1] == '7' && buffer[2] == 'z' && buffer[3] == 'X' && buffer[4] == 'Z' && buffer[5] == 0x00U) {
        return "XZ compressed data";
    }
    if (length >= 2U && buffer[0] == '#' && buffer[1] == '!') {
        return "script text executable";
    }
    if (length > 262U && buffer[257] == 'u' && buffer[258] == 's' && buffer[259] == 't' && buffer[260] == 'a' && buffer[261] == 'r') {
        return "tar archive";
    }
    return looks_like_text(buffer, length) ? "ASCII text" : "data";
}

static int describe_path(const char *path) {
    int is_directory = 0;
    int fd;
    int should_close;
    unsigned char buffer[512];
    long bytes_read;

    if (platform_path_is_directory(path, &is_directory) == 0 && is_directory) {
        rt_write_cstr(1, path);
        rt_write_line(1, ": directory");
        return 0;
    }

    if (tool_open_input(path, &fd, &should_close) != 0) {
        return -1;
    }

    bytes_read = platform_read(fd, buffer, sizeof(buffer));
    tool_close_input(fd, should_close);
    if (bytes_read < 0) {
        return -1;
    }

    rt_write_cstr(1, path ? path : "stdin");
    rt_write_cstr(1, ": ");
    rt_write_line(1, detect_type(buffer, (size_t)bytes_read));
    return 0;
}

int main(int argc, char **argv) {
    int exit_code = 0;
    int i;

    if (argc == 1) {
        return describe_path(NULL) == 0 ? 0 : 1;
    }

    for (i = 1; i < argc; ++i) {
        if (describe_path(argv[i]) != 0) {
            rt_write_cstr(2, "file: cannot inspect ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }
    }

    return exit_code;
}