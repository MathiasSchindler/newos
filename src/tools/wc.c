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

typedef struct {
    int lines;
    int words;
    int chars;
    int bytes;
    int max_line_length;
} WcScanNeeds;

#define WC_SCAN_BUFFER_SIZE 65536U
#define WC_SCAN_CARRY_CAPACITY 128U

static void write_json_wc_result(const WcStats *stats, const char *name) {
    if (tool_json_begin_event(1, "wc", "stdout", "wc_result") != 0) return;
    rt_write_cstr(1, ",\"data\":{");
    if (name != 0) {
        rt_write_cstr(1, "\"file\":");
        tool_json_write_string(1, name);
        rt_write_char(1, ',');
    } else {
        rt_write_cstr(1, "\"file\":null,");
    }

    rt_write_cstr(1, "\"lines\":");
    rt_write_uint(1, stats->lines);
    rt_write_cstr(1, ",\"words\":");
    rt_write_uint(1, stats->words);
    rt_write_cstr(1, ",\"chars\":");
    rt_write_uint(1, stats->chars);
    rt_write_cstr(1, ",\"bytes\":");
    rt_write_uint(1, stats->bytes);
    rt_write_cstr(1, ",\"max_line_length\":");
    rt_write_uint(1, stats->max_line_length);
    rt_write_char(1, '}');
    tool_json_end_event(1);
}

static int write_padded_uint(int fd, unsigned long long value, unsigned int minimum_width) {
    unsigned int digits = (unsigned int)tool_count_decimal_digits(value);
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

static unsigned long long next_display_width(unsigned long long current_width, unsigned int codepoint, unsigned int ambiguous_width) {
    RtTextSegment segment;

    segment.start = 0U;
    segment.end = 0U;
    segment.codepoint = codepoint;
    segment.display_width = codepoint == '\t' ? 0U : rt_unicode_display_width_mode(codepoint, ambiguous_width);
    segment.flags = 0U;
    if (codepoint == '\b') {
        segment.flags = RT_TEXT_SEGMENT_BACKSPACE;
        segment.display_width = 0U;
    } else if (codepoint == '\r') {
        segment.flags = RT_TEXT_SEGMENT_CARRIAGE_RETURN;
        segment.display_width = 0U;
    }
    return rt_text_apply_segment_width_tabstop(current_width, &segment, 8U);
}

static void account_display_codepoint(unsigned int codepoint, unsigned int ambiguous_width,
                                      RtGraphemeState *grapheme_state, unsigned long long *current_width) {
    unsigned int completed_width;

    if (codepoint == '\t' || codepoint == '\b' || codepoint == '\r' || codepoint < 0x20U ||
        (codepoint >= 0x7fU && codepoint < 0xa0U)) {
        *current_width += (unsigned long long)rt_grapheme_state_finish(grapheme_state);
        *current_width = next_display_width(*current_width, codepoint, ambiguous_width);
        return;
    }
    if (rt_grapheme_state_push(grapheme_state, codepoint, ambiguous_width, &completed_width) >= 0) {
        *current_width += (unsigned long long)completed_width;
    }
}

static void print_counts(const WcOptions *options, const WcStats *stats, const char *name) {
    int wrote_value = 0;
    unsigned int minimum_width = (options->pad_output && name != 0) ? 7U : 1U;

    if (tool_json_is_enabled()) {
        write_json_wc_result(stats, name);
        return;
    }

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

static void select_scan_needs(const WcOptions *options, WcScanNeeds *needs) {
    int json_output = tool_json_is_enabled();

    rt_memset(needs, 0, sizeof(*needs));
    if (json_output) {
        needs->lines = 1;
        needs->words = 1;
        needs->chars = 1;
        needs->bytes = 1;
        needs->max_line_length = 1;
        return;
    }

    if (!options->explicit_selection || options->show_lines) {
        needs->lines = 1;
    }
    if (!options->explicit_selection || options->show_words) {
        needs->words = 1;
    }
    if (options->show_chars) {
        needs->chars = 1;
    }
    if (!options->explicit_selection || options->show_bytes) {
        needs->bytes = 1;
    }
    if (options->show_max_line_length) {
        needs->max_line_length = 1;
    }
}

static int count_stream_bytes_lines(int fd, const WcScanNeeds *needs, WcStats *stats_out) {
    char buffer[WC_SCAN_BUFFER_SIZE];
    long bytes_read;
    unsigned long long lines = 0ULL;
    unsigned long long bytes = 0ULL;

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        size_t length = (size_t)bytes_read;
        size_t i;

        bytes += (unsigned long long)bytes_read;
        if (needs->lines) {
            for (i = 0U; i < length; ++i) {
                if (buffer[i] == '\n') {
                    lines += 1ULL;
                }
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    rt_memset(stats_out, 0, sizeof(*stats_out));
    stats_out->lines = lines;
    stats_out->bytes = bytes;
    return 0;
}

static void account_codepoint(const WcScanNeeds *needs, unsigned int codepoint, int *in_word,
                              unsigned long long *lines, unsigned long long *words,
                              unsigned long long *chars, unsigned long long *current_line_length,
                              unsigned long long *max_line_length, unsigned int ambiguous_width,
                              RtGraphemeState *grapheme_state) {
    if (needs->chars) {
        *chars += 1ULL;
    }

    if (codepoint == '\n') {
        if (needs->lines) {
            *lines += 1ULL;
        }
        if (needs->max_line_length) {
            *current_line_length += (unsigned long long)rt_grapheme_state_finish(grapheme_state);
            if (*current_line_length > *max_line_length) {
                *max_line_length = *current_line_length;
            }
            *current_line_length = 0ULL;
        }
    } else if (needs->max_line_length) {
        account_display_codepoint(codepoint, ambiguous_width, grapheme_state, current_line_length);
    }

    if (needs->words) {
        if (rt_unicode_is_space(codepoint)) {
            *in_word = 0;
        } else if (!*in_word) {
            *words += 1ULL;
            *in_word = 1;
        }
    }
}

static int count_stream_text(int fd, const WcScanNeeds *needs, WcStats *stats_out, unsigned int ambiguous_width) {
    char buffer[WC_SCAN_BUFFER_SIZE + WC_SCAN_CARRY_CAPACITY];
    long bytes_read;
    size_t carry = 0;
    int in_word = 0;
    unsigned long long lines = 0ULL;
    unsigned long long words = 0ULL;
    unsigned long long chars = 0ULL;
    unsigned long long bytes = 0ULL;
    unsigned long long current_line_length = 0ULL;
    unsigned long long max_line_length = 0ULL;
    RtGraphemeState grapheme_state;

    rt_grapheme_state_reset(&grapheme_state);

    while ((bytes_read = platform_read(fd, buffer + carry, WC_SCAN_BUFFER_SIZE)) > 0) {
        size_t total = carry + (size_t)bytes_read;
        size_t i = 0;

        bytes += (unsigned long long)bytes_read;
        carry = 0;

        while (i < total) {
            size_t before = i;
            unsigned int codepoint = 0;
            unsigned char ch = (unsigned char)buffer[i];

            if (ch == '\033' && needs->max_line_length) {
                RtTextSegment segment;

                if (rt_text_next_segment(buffer, total, i, &segment) == 0 &&
                    (segment.flags & RT_TEXT_SEGMENT_ANSI) != 0U) {
                    size_t ansi_index;

                    if ((segment.flags & RT_TEXT_SEGMENT_INCOMPLETE) != 0U && total - i < WC_SCAN_CARRY_CAPACITY) {
                        carry = total - i;
                        memmove(buffer, buffer + i, carry);
                        break;
                    }

                    if ((segment.flags & RT_TEXT_SEGMENT_INCOMPLETE) != 0U) {
                        segment.end = i + 1U;
                    }

                    for (ansi_index = i; ansi_index < segment.end; ++ansi_index) {
                        unsigned char ansi_ch = (unsigned char)buffer[ansi_index];

                        if (needs->chars) {
                            chars += 1ULL;
                        }
                        if (needs->lines && ansi_ch == '\n') {
                            lines += 1ULL;
                        }
                        if (needs->words) {
                            if (tool_ascii_is_space((char)ansi_ch)) {
                                in_word = 0;
                            } else if (!in_word) {
                                words += 1ULL;
                                in_word = 1;
                            }
                        }
                    }
                    i = segment.end;
                    continue;
                }
            }

            if (ch < 0x80U) {
                i += 1U;
                if (needs->chars) {
                    chars += 1ULL;
                }
                if (ch == '\n') {
                    if (needs->lines) {
                        lines += 1ULL;
                    }
                    if (needs->max_line_length) {
                        current_line_length += (unsigned long long)rt_grapheme_state_finish(&grapheme_state);
                        if (current_line_length > max_line_length) {
                            max_line_length = current_line_length;
                        }
                        current_line_length = 0ULL;
                    }
                } else if (needs->max_line_length) {
                    account_display_codepoint((unsigned int)ch, ambiguous_width, &grapheme_state, &current_line_length);
                }
                if (needs->words) {
                    if (tool_ascii_is_space((char)ch)) {
                        in_word = 0;
                    } else if (!in_word) {
                        words += 1ULL;
                        in_word = 1;
                    }
                }
                continue;
            }

            {
                size_t needed = utf8_expected_length(ch);

                if (needed > 1U && needed > total - i) {
                    carry = total - i;
                    memmove(buffer, buffer + i, carry);
                    break;
                }
            }

            (void)rt_utf8_decode(buffer, total, &i, &codepoint);
            if (i == before) {
                i += 1U;
                codepoint = 0xfffdU;
            }

            account_codepoint(needs, codepoint, &in_word, &lines, &words, &chars, &current_line_length,
                              &max_line_length, ambiguous_width, &grapheme_state);
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

        account_codepoint(needs, codepoint, &in_word, &lines, &words, &chars, &current_line_length,
                          &max_line_length, ambiguous_width, &grapheme_state);

        if (index < carry) {
            memmove(buffer, buffer + index, carry - index);
        }
        carry -= index;
    }

    if (needs->max_line_length) {
        current_line_length += (unsigned long long)rt_grapheme_state_finish(&grapheme_state);
        if (current_line_length > max_line_length) max_line_length = current_line_length;
    }

    stats_out->lines = lines;
    stats_out->words = words;
    stats_out->chars = chars;
    stats_out->bytes = bytes;
    stats_out->max_line_length = max_line_length;
    return 0;
}

static int count_stream(int fd, const WcOptions *options, WcStats *stats_out) {
    WcScanNeeds needs;

    select_scan_needs(options, &needs);
    if (!needs.words && !needs.chars && !needs.max_line_length) {
        return count_stream_bytes_lines(fd, &needs, stats_out);
    }
    return count_stream_text(fd, &needs, stats_out, tool_unicode_ambiguous_width());
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

        if (rt_strcmp(argv[arg_index], "--json") == 0) {
            tool_json_set_enabled(1);
            arg_index += 1;
            continue;
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

        if (count_stream(0, &options, &stats) != 0) {
            tool_write_error("wc", "read error", 0);
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
            tool_write_error("wc", "cannot open ", argv[i]);
            exit_code = 1;
            continue;
        }

        if (count_stream(fd, &options, &stats) != 0) {
            tool_write_error("wc", "read error on ", argv[i]);
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
