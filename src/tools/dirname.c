#include "runtime.h"

#define DIRNAME_BUFFER_CAPACITY 1024

static void compute_dirname(const char *path, char *buffer, size_t buffer_size) {
    size_t len = rt_strlen(path);

    if (len == 0U) {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    rt_copy_string(buffer, buffer_size, path);
    len = rt_strlen(buffer);

    while (len > 1U && buffer[len - 1U] == '/') {
        buffer[len - 1U] = '\0';
        len -= 1U;
    }

    while (len > 0U && buffer[len - 1U] != '/') {
        buffer[len - 1U] = '\0';
        len -= 1U;
    }

    while (len > 1U && buffer[len - 1U] == '/') {
        buffer[len - 1U] = '\0';
        len -= 1U;
    }

    if (len == 0U) {
        rt_copy_string(buffer, buffer_size, ".");
    }
}

int main(int argc, char **argv) {
    char result[DIRNAME_BUFFER_CAPACITY];

    if (argc != 2) {
        rt_write_line(2, "Usage: dirname path");
        return 1;
    }

    compute_dirname(argv[1], result, sizeof(result));
    return rt_write_line(1, result) == 0 ? 0 : 1;
}