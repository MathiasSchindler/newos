#include "runtime.h"

#define BASENAME_BUFFER_CAPACITY 1024

static int write_result(const char *text, int zero_terminated) {
    if (zero_terminated) {
        return rt_write_all(1, text, rt_strlen(text)) == 0 && rt_write_char(1, '\0') == 0 ? 0 : -1;
    }
    return rt_write_line(1, text);
}

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

static void maybe_strip_suffix(char *buffer, const char *suffix) {
    size_t result_len;
    size_t suffix_len;

    if (suffix == 0 || suffix[0] == '\0') {
        return;
    }

    result_len = rt_strlen(buffer);
    suffix_len = rt_strlen(suffix);
    if (suffix_len > 0U && suffix_len < result_len &&
        rt_strcmp(buffer + result_len - suffix_len, suffix) == 0) {
        buffer[result_len - suffix_len] = '\0';
    }
}

int main(int argc, char **argv) {
    char result[BASENAME_BUFFER_CAPACITY];
    const char *suffix = 0;
    int multi_arg = 0;
    int zero_terminated = 0;
    int argi = 1;
    int i;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "-a") == 0) {
            multi_arg = 1;
        } else if (rt_strcmp(argv[argi], "-z") == 0) {
            zero_terminated = 1;
        } else if (rt_strcmp(argv[argi], "-s") == 0 || (argv[argi][0] == '-' && argv[argi][1] == 's' && argv[argi][2] != '\0')) {
            suffix = (rt_strcmp(argv[argi], "-s") == 0) ? ((argi + 1 < argc) ? argv[++argi] : 0) : (argv[argi] + 2);
            multi_arg = 1;
            if (suffix == 0 || suffix[0] == '\0') {
                rt_write_line(2, "Usage: basename [-a] [-s suffix] [-z] name ...");
                return 1;
            }
        } else {
            rt_write_line(2, "Usage: basename [-a] [-s suffix] [-z] name ...");
            return 1;
        }
        argi += 1;
    }

    if (argi >= argc) {
        rt_write_line(2, "Usage: basename [-a] [-s suffix] [-z] name ...");
        return 1;
    }

    if (!multi_arg && suffix == 0 && argc - argi == 2) {
        extract_basename(argv[argi], result, sizeof(result));
        maybe_strip_suffix(result, argv[argi + 1]);
        return write_result(result, zero_terminated) == 0 ? 0 : 1;
    }

    if (!multi_arg && argc - argi != 1) {
        rt_write_line(2, "Usage: basename [-a] [-s suffix] [-z] name ...");
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        extract_basename(argv[i], result, sizeof(result));
        maybe_strip_suffix(result, suffix);
        if (write_result(result, zero_terminated) != 0) {
            return 1;
        }
    }

    return 0;
}
