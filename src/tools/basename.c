#include "runtime.h"

#define BASENAME_BUFFER_CAPACITY 1024

static void print_usage(void) {
    rt_write_line(2, "Usage: basename [-a] [-s suffix] [-z] name ...");
}

static int starts_with(const char *text, const char *prefix) {
    size_t i = 0;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1U;
    }

    return 1;
}

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
        if (rt_strcmp(argv[argi], "--multiple") == 0) {
            multi_arg = 1;
        } else if (rt_strcmp(argv[argi], "--zero") == 0) {
            zero_terminated = 1;
        } else if (rt_strcmp(argv[argi], "--suffix") == 0) {
            if (argi + 1 >= argc) {
                print_usage();
                return 1;
            }
            suffix = argv[++argi];
            multi_arg = 1;
        } else if (starts_with(argv[argi], "--suffix=")) {
            suffix = argv[argi] + 9;
            multi_arg = 1;
        } else {
            const char *flag = argv[argi] + 1;
            while (*flag != '\0') {
                if (*flag == 'a') {
                    multi_arg = 1;
                    flag += 1;
                } else if (*flag == 'z') {
                    zero_terminated = 1;
                    flag += 1;
                } else if (*flag == 's') {
                    if (flag[1] != '\0') {
                        suffix = flag + 1;
                    } else if (argi + 1 < argc) {
                        suffix = argv[++argi];
                    } else {
                        suffix = 0;
                    }
                    multi_arg = 1;
                    flag += rt_strlen(flag);
                } else {
                    print_usage();
                    return 1;
                }
            }
        }
        if (suffix != 0 && suffix[0] == '\0') {
            print_usage();
            return 1;
        }
        argi += 1;
    }

    if (argi >= argc) {
        print_usage();
        return 1;
    }

    if (!multi_arg && suffix == 0 && argc - argi == 2) {
        extract_basename(argv[argi], result, sizeof(result));
        maybe_strip_suffix(result, argv[argi + 1]);
        return write_result(result, zero_terminated) == 0 ? 0 : 1;
    }

    if (!multi_arg && argc - argi != 1) {
        print_usage();
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
