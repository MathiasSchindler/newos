#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define FOLD_LINE_CAPACITY 8192

typedef enum {
    FOLD_COUNT_COLUMNS = 0,
    FOLD_COUNT_BYTES = 1,
    FOLD_COUNT_CHARACTERS = 2
} FoldCountMode;

typedef struct {
    unsigned long long width;
    int break_spaces;
    FoldCountMode count_mode;
} FoldOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-bcs] [-w WIDTH] [file ...]");
}


static int flush_prefix(const char *buffer, size_t count) {
    if (count == 0U) {
        return 0;
    }

    if (rt_write_all(1, buffer, count) != 0 || rt_write_char(1, '\n') != 0) {
        return -1;
    }

    return 0;
}

static unsigned long long measure_units(const char *buffer, size_t count, const FoldOptions *options) {
    if (options->count_mode == FOLD_COUNT_BYTES) {
        return (unsigned long long)count;
    }
    if (options->count_mode == FOLD_COUNT_CHARACTERS) {
        size_t index = 0U;
        unsigned long long units = 0ULL;
        RtTextSegment segment;

        while (rt_text_next_segment(buffer, count, index, &segment) == 0) {
            if ((segment.flags & RT_TEXT_SEGMENT_ANSI) == 0U) {
                units += 1ULL;
            }
            index = segment.end;
        }
        return units;
    }
    return rt_text_display_width_n(buffer, count, 0ULL);
}

static int line_has_incomplete_tail(const char *buffer, size_t count, const FoldOptions *options) {
    return options->count_mode == FOLD_COUNT_BYTES ? 0 : rt_text_has_incomplete_tail(buffer, count);
}

static int segment_is_space(const char *buffer, size_t count, const RtTextSegment *segment, const FoldOptions *options) {
    if (options->count_mode == FOLD_COUNT_BYTES) {
        return buffer[segment->start] == ' ' || buffer[segment->start] == '\t';
    }
    return rt_text_segment_is_space(buffer, count, segment);
}

static unsigned long long apply_segment_units(unsigned long long current, const RtTextSegment *segment, const FoldOptions *options) {
    if (options->count_mode == FOLD_COUNT_BYTES) {
        return current + (unsigned long long)(segment->end - segment->start);
    }
    if (options->count_mode == FOLD_COUNT_CHARACTERS) {
        return current + (((segment->flags & RT_TEXT_SEGMENT_ANSI) == 0U) ? 1ULL : 0ULL);
    }
    return rt_text_apply_segment_width(current, segment);
}

static size_t find_split_point(const char *buffer, size_t count, const FoldOptions *options) {
    size_t index = 0U;
    size_t split = 0U;
    size_t last_space_split = 0U;
    unsigned long long units = 0ULL;

    while (index < count) {
        RtTextSegment segment;
        unsigned long long next_units;

        if (options->count_mode == FOLD_COUNT_BYTES) {
            segment.start = index;
            segment.end = index + 1U;
            segment.codepoint = (unsigned char)buffer[index];
            segment.display_width = 1U;
            segment.flags = 0U;
        } else if (rt_text_next_segment(buffer, count, index, &segment) != 0) {
            break;
        }

        next_units = apply_segment_units(units, &segment, options);

        if (next_units > options->width && split > 0U) {
            break;
        }
        if (next_units > options->width) {
            return segment.end;
        }

        split = segment.end;
        units = next_units;
        if (options->break_spaces && segment_is_space(buffer, count, &segment, options)) {
            last_space_split = segment.end;
        }
        index = segment.end;
    }

    if (options->break_spaces && last_space_split > 0U) {
        split = last_space_split;
    }

    return split;
}

static int line_starts_with_space(const char *buffer, size_t count, const FoldOptions *options, size_t *space_len_out) {
    RtTextSegment segment;

    if (count == 0U) {
        return 0;
    }
    if (options->count_mode == FOLD_COUNT_BYTES) {
        segment.start = 0U;
        segment.end = 1U;
        segment.codepoint = (unsigned char)buffer[0];
        segment.display_width = 1U;
        segment.flags = 0U;
    } else if (rt_text_next_segment(buffer, count, 0U, &segment) != 0) {
        return 0;
    }
    if (segment_is_space(buffer, count, &segment, options)) {
        *space_len_out = segment.end;
        return 1;
    }
    return 0;
}

static int fold_stream(int fd, const FoldOptions *options) {
    char chunk[4096];
    char line[FOLD_LINE_CAPACITY];
    size_t line_len = 0;
    long bytes_read;

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                if (line_len > 0U && rt_write_all(1, line, line_len) != 0) {
                    return -1;
                }
                if (rt_write_char(1, '\n') != 0) {
                    return -1;
                }
                line_len = 0;
                continue;
            }

            if (line_len + 1U < sizeof(line)) {
                line[line_len++] = ch;
            }

                        while (line_len > 0U &&
                                     !line_has_incomplete_tail(line, line_len, options) &&
                                     measure_units(line, line_len, options) > options->width) {
                size_t split = find_split_point(line, line_len, options);

                if (flush_prefix(line, split) != 0) {
                    return -1;
                }

                if (split < line_len) {
                    size_t keep_len = line_len - split;
                    size_t j;

                    for (j = 0; j < keep_len; ++j) {
                        line[j] = line[split + j];
                    }
                    line_len = keep_len;
                } else {
                    line_len = 0;
                }

                while (options->break_spaces && line_len > 0U) {
                    size_t space_len = 0U;
                    size_t j;

                    if (!line_starts_with_space(line, line_len, options, &space_len)) {
                        break;
                    }
                    for (j = space_len; j < line_len; ++j) {
                        line[j - space_len] = line[j];
                    }
                    line_len -= space_len;
                }
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (line_len > 0U) {
        if (rt_write_all(1, line, line_len) != 0) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    FoldOptions options;
    int argi = 1;
    int exit_code = 0;

    options.width = 80ULL;
    options.break_spaces = 0;
    options.count_mode = FOLD_COUNT_COLUMNS;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-b") == 0) {
            options.count_mode = FOLD_COUNT_BYTES;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-c") == 0) {
            options.count_mode = FOLD_COUNT_CHARACTERS;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-s") == 0) {
            options.break_spaces = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-w") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &options.width, "fold", "width") != 0 || options.width == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (argv[argi][1] == 'w' && argv[argi][2] != '\0') {
            if (tool_parse_uint_arg(argv[argi] + 2, &options.width, "fold", "width") != 0 || options.width == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 1;
        } else if (tool_text_is_decimal(argv[argi] + 1)) {
            if (tool_parse_uint_arg(argv[argi] + 1, &options.width, "fold", "width") != 0 || options.width == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else if (rt_strcmp(argv[argi], "-") == 0) {
            break;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (argi == argc) {
        return fold_stream(0, &options) == 0 ? 0 : 1;
    }

    for (; argi < argc; ++argi) {
        int fd;
        int should_close;

        if (tool_open_input(argv[argi], &fd, &should_close) != 0) {
            tool_write_error("fold", "cannot open ", argv[argi]);
            exit_code = 1;
            continue;
        }

        if (fold_stream(fd, &options) != 0) {
            tool_write_error("fold", "read error on ", argv[argi]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
