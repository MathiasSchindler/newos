#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    int show_lines;
    int show_words;
    int show_bytes;
    int explicit_selection;
} WcOptions;

static void print_counts(const WcOptions *options,
                         unsigned long long lines,
                         unsigned long long words,
                         unsigned long long bytes,
                         const char *name) {
    int wrote_value = 0;

    if (!options->explicit_selection || options->show_lines) {
        rt_write_uint(1, lines);
        wrote_value = 1;
    }

    if (!options->explicit_selection || options->show_words) {
        if (wrote_value) {
            rt_write_char(1, ' ');
        }
        rt_write_uint(1, words);
        wrote_value = 1;
    }

    if (!options->explicit_selection || options->show_bytes) {
        if (wrote_value) {
            rt_write_char(1, ' ');
        }
        rt_write_uint(1, bytes);
        wrote_value = 1;
    }

    if (name != 0) {
        if (wrote_value) {
            rt_write_char(1, ' ');
        }
        rt_write_cstr(1, name);
    }

    rt_write_char(1, '\n');
}

static int count_stream(int fd,
                        unsigned long long *lines_out,
                        unsigned long long *words_out,
                        unsigned long long *bytes_out) {
    char buffer[4096];
    long bytes_read;
    int in_word = 0;
    unsigned long long lines = 0;
    unsigned long long words = 0;
    unsigned long long bytes = 0;

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        bytes += (unsigned long long)bytes_read;
        for (i = 0; i < bytes_read; ++i) {
            char ch = buffer[i];

            if (ch == '\n') {
                lines += 1;
            }

            if (rt_is_space(ch)) {
                in_word = 0;
            } else if (!in_word) {
                words += 1;
                in_word = 1;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    *lines_out = lines;
    *words_out = words;
    *bytes_out = bytes;
    return 0;
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-lwc] [file ...]");
}

int main(int argc, char **argv) {
    WcOptions options;
    int arg_index = 1;
    int file_count;
    int i;
    unsigned long long total_lines = 0;
    unsigned long long total_words = 0;
    unsigned long long total_bytes = 0;
    int exit_code = 0;

    rt_memset(&options, 0, sizeof(options));

    while (arg_index < argc && argv[arg_index][0] == '-' && argv[arg_index][1] != '\0') {
        const char *flag = argv[arg_index] + 1;

        if (rt_strcmp(argv[arg_index], "--") == 0) {
            arg_index += 1;
            break;
        }

        while (*flag != '\0') {
            if (*flag == 'l') {
                options.show_lines = 1;
                options.explicit_selection = 1;
            } else if (*flag == 'w') {
                options.show_words = 1;
                options.explicit_selection = 1;
            } else if (*flag == 'c') {
                options.show_bytes = 1;
                options.explicit_selection = 1;
            } else {
                print_usage(argv[0]);
                return 1;
            }
            flag += 1;
        }

        arg_index += 1;
    }

    file_count = argc - arg_index;
    if (file_count <= 0) {
        unsigned long long lines;
        unsigned long long words;
        unsigned long long bytes;

        if (count_stream(0, &lines, &words, &bytes) != 0) {
            rt_write_line(2, "wc: read error");
            return 1;
        }

        print_counts(&options, lines, words, bytes, 0);
        return 0;
    }

    for (i = arg_index; i < argc; ++i) {
        int fd;
        int should_close;
        unsigned long long lines;
        unsigned long long words;
        unsigned long long bytes;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "wc: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (count_stream(fd, &lines, &words, &bytes) != 0) {
            rt_write_cstr(2, "wc: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        } else {
            print_counts(&options, lines, words, bytes, argv[i]);
            total_lines += lines;
            total_words += words;
            total_bytes += bytes;
        }

        tool_close_input(fd, should_close);
    }

    if (file_count > 1) {
        print_counts(&options, total_lines, total_words, total_bytes, "total");
    }

    return exit_code;
}
