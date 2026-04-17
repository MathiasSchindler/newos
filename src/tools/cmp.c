#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    int silent;
    int list_all;
    int has_limit;
    unsigned long long skip_left;
    unsigned long long skip_right;
    unsigned long long limit_bytes;
} CmpOptions;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-s] [-l] [-i SKIP1[:SKIP2]] [-n LIMIT] file1 file2");
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

static int parse_skip_spec(const char *text, unsigned long long *left_out, unsigned long long *right_out) {
    char left[32];
    char right[32];
    size_t i = 0U;
    size_t left_len = 0U;
    size_t right_len = 0U;
    unsigned long long left_value = 0ULL;
    unsigned long long right_value = 0ULL;

    if (text == 0 || text[0] == '\0') {
        return -1;
    }

    while (text[i] != '\0' && text[i] != ':') {
        if (left_len + 1U < sizeof(left)) {
            left[left_len++] = text[i];
        }
        i += 1U;
    }
    left[left_len] = '\0';

    if (left_len == 0U || rt_parse_uint(left, &left_value) != 0) {
        return -1;
    }

    if (text[i] == ':') {
        i += 1U;
        while (text[i] != '\0') {
            if (right_len + 1U < sizeof(right)) {
                right[right_len++] = text[i];
            }
            i += 1U;
        }
        right[right_len] = '\0';
        if (right_len == 0U || rt_parse_uint(right, &right_value) != 0) {
            return -1;
        }
    } else {
        right_value = left_value;
    }

    *left_out = left_value;
    *right_out = right_value;
    return 0;
}

static int skip_bytes(int fd, unsigned long long count) {
    char buffer[4096];

    while (count > 0ULL) {
        size_t chunk = sizeof(buffer);
        long bytes_read;

        if (count < (unsigned long long)chunk) {
            chunk = (size_t)count;
        }

        bytes_read = platform_read(fd, buffer, chunk);
        if (bytes_read < 0) {
            return -1;
        }
        if (bytes_read == 0) {
            return 1;
        }
        count -= (unsigned long long)bytes_read;
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
            } else if (*flag == 'i' || *flag == 'n') {
                const char *value = flag + 1;

                if (*value == '\0') {
                    argi += 1;
                    if (argi >= argc) {
                        print_usage(argv[0]);
                        return 1;
                    }
                    value = argv[argi];
                }

                if (*flag == 'i') {
                    if (parse_skip_spec(value, &options.skip_left, &options.skip_right) != 0) {
                        print_usage(argv[0]);
                        return 1;
                    }
                } else {
                    if (tool_parse_uint_arg(value, &options.limit_bytes, "cmp", "limit") != 0) {
                        return 1;
                    }
                    options.has_limit = 1;
                }
                break;
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

    {
        int skip_status1 = skip_bytes(fd1, options.skip_left);
        int skip_status2 = skip_bytes(fd2, options.skip_right);

        if (skip_status1 < 0 || skip_status2 < 0) {
            rt_write_line(2, "cmp: read error");
            tool_close_input(fd1, close1);
            tool_close_input(fd2, close2);
            return 1;
        }

        if (skip_status1 != 0 || skip_status2 != 0) {
            if (!options.silent) {
                rt_write_cstr(1, "cmp: EOF on ");
                rt_write_line(1, skip_status1 != 0 ? argv[argi] : argv[argi + 1]);
            }
            tool_close_input(fd1, close1);
            tool_close_input(fd2, close2);
            return 1;
        }
    }

    for (;;) {
        size_t request_size = sizeof(buf1);
        long read1;
        long read2;
        long i;
        long limit;

        if (options.has_limit && options.limit_bytes == 0ULL) {
            break;
        }
        if (options.has_limit && options.limit_bytes < (unsigned long long)request_size) {
            request_size = (size_t)options.limit_bytes;
        }

        read1 = platform_read(fd1, buf1, request_size);
        read2 = platform_read(fd2, buf2, request_size);
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

        if (options.has_limit) {
            options.limit_bytes -= (unsigned long long)limit;
        }
    }

    tool_close_input(fd1, close1);
    tool_close_input(fd2, close2);
    return differences;
}
