#include "runtime.h"

#define DIRNAME_BUFFER_CAPACITY 1024

static int write_result(const char *text, int zero_terminated) {
    if (zero_terminated) {
        return rt_write_all(1, text, rt_strlen(text)) == 0 && rt_write_char(1, '\0') == 0 ? 0 : -1;
    }
    return rt_write_line(1, text);
}

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
    int zero_terminated = 0;
    int argi = 1;
    int i;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "-z") == 0) {
            zero_terminated = 1;
        } else {
            rt_write_line(2, "Usage: dirname [-z] path ...");
            return 1;
        }
        argi += 1;
    }

    if (argi >= argc) {
        rt_write_line(2, "Usage: dirname [-z] path ...");
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        compute_dirname(argv[i], result, sizeof(result));
        if (write_result(result, zero_terminated) != 0) {
            return 1;
        }
    }

    return 0;
}
