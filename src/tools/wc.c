#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    int show_lines;
    int show_words;
    int show_chars;
    int show_bytes;
    int show_max_line_length;
    int explicit_selection;
    int pad_output;
} WcOptions;

typedef struct {
    unsigned long long lines;
    unsigned long long words;
    unsigned long long chars;
    unsigned long long bytes;
    unsigned long long max_line_length;
} WcStats;

static unsigned int count_digits(unsigned long long value) {
    unsigned int digits = 1U;

    while (value >= 10ULL) {
        value /= 10ULL;
        digits += 1U;
    }

    return digits;
}

static int write_padded_uint(int fd, unsigned long long value, unsigned int minimum_width) {
    unsigned int digits = count_digits(value);
    unsigned int i;

    for (i = digits; i < minimum_width; ++i) {
        if (rt_write_char(fd, ' ') != 0) {
            return -1;
        }
    }

    return rt_write_uint(fd, value);
}

static size_t utf8_expected_length(unsigned char lead) {
    if ((lead & 0x80U) == 0U) {
        return 1U;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        return 2U;
    }
    if ((lead & 0xF0U) == 0xE0U) {
        return 3U;
    }
    if ((lead & 0xF8U) == 0xF0U) {
        return 4U;
    }
    return 1U;
}

static unsigned long long next_display_width(unsigned long long current_width, unsigned int codepoint) {
    if (codepoint == '\t') {
        return current_width + (8ULL - (current_width % 8ULL));
    }

    return current_width + (unsigned long long)rt_unicode_display_width(codepoint);
}

static void print_counts(const WcOptions *options, const WcStats *stats, const char *name) {
    int wrote_value = 0;
    unsigned int minimum_width = (options->pad_output && name != 0) ? 7U : 1U;

    if (!options->explicit_selection || options->show_lines) {
        (void)write_padded_uint(1, stats->lines, minimum_width);
        wrote_value = 1;
    }

    if (!options->explicit_selection || options->show_words) {
        if (wrote_value) {
            rt_write_char(1, ' ');
        }
        (void)write_padded_uint(1, stats->words, minimum_width);
        wrote_value = 1;
    }

    if (options->show_chars) {
        if (wrote_value) {
            rt_write_char(1, ' ');
        }
        (void)write_padded_uint(1, stats->chars, minimum_width);
        wrote_value = 1;
    }

    if (!options->explicit_selection || options->show_bytes) {
        if (wrote_value) {
            rt_write_char(1, ' ');
        }
        (void)write_padded_uint(1, stats->bytes, minimum_width);
        wrote_value = 1;
    }

    if (options->show_max_line_length) {
        if (wrote_value) {
            rt_write_char(1, ' ');
        }
        (void)write_padded_uint(1, stats->max_line_length, minimum_width);
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

static int count_stream(int fd, WcStats *stats_out) {
    char buffer[4096 + 4];
    long bytes_read;
    size_t carry = 0;
    int in_word = 0;
    unsigned long long lines = 0ULL;
    unsigned long long words = 0ULL;
    unsigned long long chars = 0ULL;
    unsigned long long bytes = 0ULL;
    unsigned long long current_line_length = 0ULL;
    unsigned long long max_line_length = 0ULL;

    while ((bytes_read = platform_read(fd, buffer + carry, sizeof(buffer) - carry)) > 0) {
        size_t total = carry + (size_t)bytes_read;
        size_t i = 0;

        bytes += (unsigned long long)bytes_read;
        carry = 0;

        while (i < total) {
            size_t before = i;
            unsigned int codepoint = 0;
            size_t needed = utf8_expected_length((unsigned char)buffer[i]);

            if (needed > 1U && needed > total - i) {
                carry = total - i;
                memmove(buffer, buffer + i, carry);
                break;
            }

            (void)rt_utf8_decode(buffer, total, &i, &codepoint);
            if (i == before) {
                i += 1U;
                codepoint = 0xfffdU;
            }

            chars += 1ULL;
            if (codepoint == '\n') {
                if (current_line_length > max_line_length) {
                    max_line_length = current_line_length;
                }
                current_line_length = 0ULL;
                lines += 1ULL;
            } else {
                current_line_length = next_display_width(current_line_length, codepoint);
            }

            if (rt_unicode_is_space(codepoint)) {
                in_word = 0;
            } else if (!in_word) {
                words += 1ULL;
                in_word = 1;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    while (carry > 0U) {
        size_t index = 0U;
        unsigned int codepoint = 0;
        size_t before = index;

        (void)rt_utf8_decode(buffer, carry, &index, &codepoint);
        if (index == before) {
            index += 1U;
            codepoint = 0xfffdU;
        }

        chars += 1ULL;
        current_line_length = next_display_width(current_line_length, codepoint);
        if (rt_unicode_is_space(codepoint)) {
            in_word = 0;
        } else if (!in_word) {
            words += 1ULL;
            in_word = 1;
        }

        if (index < carry) {
            memmove(buffer, buffer + index, carry - index);
        }
        carry -= index;
    }

    if (current_line_length > max_line_length) {
        max_line_length = current_line_length;
    }

    stats_out->lines = lines;
    stats_out->words = words;
    stats_out->chars = chars;
    stats_out->bytes = bytes;
    stats_out->max_line_length = max_line_length;
    return 0;
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-lwcmL] [file ...]");
}

int main(int argc, char **argv) {
    WcOptions options;
    int arg_index = 1;
    int file_count;
    int i;
    WcStats total_stats;
    int exit_code = 0;

    rt_memset(&options, 0, sizeof(options));
    rt_memset(&total_stats, 0, sizeof(total_stats));

    while (arg_index < argc && argv[arg_index][0] == '-' && argv[arg_index][1] != '\0') {
        const char *flag = argv[arg_index] + 1;

        if (rt_strcmp(argv[arg_index], "--") == 0) {
            arg_index += 1;
            break;
        }

        if (rt_strcmp(argv[arg_index], "--lines") == 0) {
            options.show_lines = 1;
            options.explicit_selection = 1;
            arg_index += 1;
            continue;
        }

        if (rt_strcmp(argv[arg_index], "--words") == 0) {
            options.show_words = 1;
            options.explicit_selection = 1;
            arg_index += 1;
            continue;
        }

        if (rt_strcmp(argv[arg_index], "--chars") == 0) {
            options.show_chars = 1;
            options.explicit_selection = 1;
            arg_index += 1;
            continue;
        }

        if (rt_strcmp(argv[arg_index], "--bytes") == 0) {
            options.show_bytes = 1;
            options.explicit_selection = 1;
            arg_index += 1;
            continue;
        }

        if (rt_strcmp(argv[arg_index], "--max-line-length") == 0) {
            options.show_max_line_length = 1;
            options.explicit_selection = 1;
            arg_index += 1;
            continue;
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
            } else if (*flag == 'm') {
                options.show_chars = 1;
                options.explicit_selection = 1;
            } else if (*flag == 'L') {
                options.show_max_line_length = 1;
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
    options.pad_output = file_count > 0;
    if (file_count <= 0) {
        WcStats stats;

        if (count_stream(0, &stats) != 0) {
            rt_write_line(2, "wc: read error");
            return 1;
        }

        print_counts(&options, &stats, 0);
        return 0;
    }

    for (i = arg_index; i < argc; ++i) {
        int fd;
        int should_close;
        WcStats stats;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "wc: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (count_stream(fd, &stats) != 0) {
            rt_write_cstr(2, "wc: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        } else {
            print_counts(&options, &stats, argv[i]);
            total_stats.lines += stats.lines;
            total_stats.words += stats.words;
            total_stats.chars += stats.chars;
            total_stats.bytes += stats.bytes;
            if (stats.max_line_length > total_stats.max_line_length) {
                total_stats.max_line_length = stats.max_line_length;
            }
        }

        tool_close_input(fd, should_close);
    }

    if (file_count > 1) {
        print_counts(&options, &total_stats, "total");
    }

    return exit_code;
}
