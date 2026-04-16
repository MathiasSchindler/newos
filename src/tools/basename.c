#include "runtime.h"

#define BASENAME_BUFFER_CAPACITY 1024

static void extract_basename(const char *path, char *buffer, size_t buffer_size) {
    size_t len = rt_strlen(path);
    size_t start;
    size_t copy_len;

    if (len == 0U) {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    while (len > 1U && path[len - 1U] == '/') {
        len -= 1U;
    }

    if (len == 1U && path[0] == '/') {
        rt_copy_string(buffer, buffer_size, "/");
        return;
    }

    start = len;
    while (start > 0U && path[start - 1U] != '/') {
        start -= 1U;
    }

    copy_len = len - start;
    if (copy_len + 1U > buffer_size) {
        copy_len = buffer_size - 1U;
    }

    memcpy(buffer, path + start, copy_len);
    buffer[copy_len] = '\0';
}

int main(int argc, char **argv) {
    char result[BASENAME_BUFFER_CAPACITY];
    size_t result_len;
    size_t suffix_len;

    if (argc < 2 || argc > 3) {
        rt_write_line(2, "Usage: basename path [suffix]");
        return 1;
    }

    extract_basename(argv[1], result, sizeof(result));

    if (argc == 3) {
        result_len = rt_strlen(result);
        suffix_len = rt_strlen(argv[2]);
        if (suffix_len > 0U && suffix_len < result_len &&
            rt_strcmp(result + result_len - suffix_len, argv[2]) == 0) {
            result[result_len - suffix_len] = '\0';
        }
    }

    return rt_write_line(1, result) == 0 ? 0 : 1;
}