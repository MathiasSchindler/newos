#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define SPLIT_MODE_LINES 1
#define SPLIT_MODE_BYTES 2
#define SPLIT_MODE_LINE_BYTES 3

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-l COUNT | -b SIZE | -C SIZE | -n CHUNKS] [-a SUFFIX_LEN] [-d] [file [prefix]]");
}

static int parse_size_value(const char *text, unsigned long long *value_out) {
    char digits[32];
    size_t len = 0;
    unsigned long long value;
    unsigned long long multiplier = 1ULL;
    char suffix;

    if (text == 0 || text[0] == '\0') {
        return -1;
    }

    while (text[len] >= '0' && text[len] <= '9') {
        if (len + 1 >= sizeof(digits)) {
            return -1;
        }
        digits[len] = text[len];
        len += 1;
    }

    if (len == 0) {
        return -1;
    }

    digits[len] = '\0';
    if (rt_parse_uint(digits, &value) != 0) {
        return -1;
    }

    suffix = text[len];
    if (suffix != '\0') {
        if (text[len + 1] != '\0') {
            return -1;
        }

        if (suffix == 'k' || suffix == 'K') {
            multiplier = 1024ULL;
        } else if (suffix == 'm' || suffix == 'M') {
            multiplier = 1024ULL * 1024ULL;
        } else if (suffix == 'g' || suffix == 'G') {
            multiplier = 1024ULL * 1024ULL * 1024ULL;
        } else {
            return -1;
        }
    }

    *value_out = value * multiplier;
    return 0;
}

static int make_output_name(const char *prefix,
                            unsigned long long index,
                            unsigned long long suffix_length,
                            int numeric_suffixes,
                            char *buffer,
                            size_t buffer_size) {
    size_t prefix_len = rt_strlen(prefix);
    size_t i;

    if (suffix_length == 0ULL || prefix_len + (size_t)suffix_length + 1U > buffer_size) {
        return -1;
    }

    memcpy(buffer, prefix, prefix_len);

    for (i = 0U; i < (size_t)suffix_length; ++i) {
        size_t pos = prefix_len + (size_t)suffix_length - 1U - i;

        if (numeric_suffixes) {
            buffer[pos] = (char)('0' + (char)(index % 10ULL));
            index /= 10ULL;
        } else {
            buffer[pos] = (char)('a' + (char)(index % 26ULL));
            index /= 26ULL;
        }
    }

    if (index != 0ULL) {
        return -1;
    }

    buffer[prefix_len + (size_t)suffix_length] = '\0';
    return 0;
}

static int ensure_output_open(int *fd_out,
                              const char *prefix,
                              unsigned long long *index_io,
                              unsigned long long suffix_length,
                              int numeric_suffixes) {
    char path[256];

    if (*fd_out >= 0) {
        return 0;
    }

    if (make_output_name(prefix, *index_io, suffix_length, numeric_suffixes, path, sizeof(path)) != 0) {
        tool_write_error("split", "too many output files for prefix ", prefix);
        return -1;
    }

    *fd_out = platform_open_write(path, 0644U);
    if (*fd_out < 0) {
        tool_write_error("split", "cannot create ", path);
        return -1;
    }

    *index_io += 1ULL;
    return 0;
}

static int starts_with_text(const char *text, const char *prefix) {
    size_t i = 0U;
    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1U;
    }
    return 1;
}

static const char *option_attached_value(const char *arg, const char *short_name, const char *long_name) {
    size_t short_len = rt_strlen(short_name);
    size_t long_len = rt_strlen(long_name);

    if (starts_with_text(arg, short_name) && arg[short_len] != '\0') {
        return arg + short_len;
    }
    if (starts_with_text(arg, long_name) && arg[long_len] == '=') {
        return arg + long_len + 1U;
    }
    return 0;
}

static int compute_chunk_limit(int fd, unsigned long long chunk_count, unsigned long long *limit_out) {
    long long start;
    long long end;
    unsigned long long total_size;

    if (chunk_count == 0ULL || limit_out == 0) {
        return -1;
    }

    start = platform_seek(fd, 0, PLATFORM_SEEK_CUR);
    if (start < 0) {
        start = 0;
    }

    end = platform_seek(fd, 0, PLATFORM_SEEK_END);
    if (end < 0 || platform_seek(fd, 0, PLATFORM_SEEK_SET) < 0) {
        return -1;
    }

    total_size = (unsigned long long)end;
    *limit_out = total_size / chunk_count;
    if (total_size % chunk_count != 0ULL) {
        *limit_out += 1ULL;
    }
    if (*limit_out == 0ULL) {
        *limit_out = 1ULL;
    }

    return 0;
}

static int write_line_bytes_chunk(const char *line,
                                  size_t line_len,
                                  int *fd_out,
                                  const char *prefix,
                                  unsigned long long *index_io,
                                  unsigned long long suffix_length,
                                  int numeric_suffixes,
                                  unsigned long long limit,
                                  unsigned long long *bytes_in_part_io) {
    size_t offset = 0U;

    while (offset < line_len) {
        size_t remaining = line_len - offset;
        unsigned long long space;
        size_t write_len;

        if (*fd_out >= 0 && *bytes_in_part_io > 0ULL &&
            (unsigned long long)remaining <= limit &&
            *bytes_in_part_io + (unsigned long long)remaining > limit) {
            platform_close(*fd_out);
            *fd_out = -1;
            *bytes_in_part_io = 0ULL;
        }

        if (ensure_output_open(fd_out, prefix, index_io, suffix_length, numeric_suffixes) != 0) {
            return -1;
        }

        if (*bytes_in_part_io >= limit) {
            platform_close(*fd_out);
            *fd_out = -1;
            *bytes_in_part_io = 0ULL;
            continue;
        }

        space = limit - *bytes_in_part_io;
        write_len = remaining;
        if ((unsigned long long)write_len > space) {
            write_len = (size_t)space;
        }

        if (write_len == 0U) {
            platform_close(*fd_out);
            *fd_out = -1;
            *bytes_in_part_io = 0ULL;
            continue;
        }

        if (rt_write_all(*fd_out, line + offset, write_len) != 0) {
            tool_write_error("split", "write error", 0);
            return -1;
        }

        offset += write_len;
        *bytes_in_part_io += (unsigned long long)write_len;

        if (*bytes_in_part_io >= limit) {
            platform_close(*fd_out);
            *fd_out = -1;
            *bytes_in_part_io = 0ULL;
        }
    }

    return 0;
}

static int split_line_bytes_mode(int input_fd,
                                 const char *prefix,
                                 unsigned long long *index_io,
                                 unsigned long long suffix_length,
                                 int numeric_suffixes,
                                 unsigned long long limit) {
    char read_buffer[2048];
    char line_buffer[4096];
    size_t line_len = 0U;
    int out_fd = -1;
    unsigned long long bytes_in_part = 0ULL;
    int saw_input = 0;

    for (;;) {
        long bytes_read = platform_read(input_fd, read_buffer, sizeof(read_buffer));
        long i;

        if (bytes_read < 0) {
            if (out_fd >= 0) {
                platform_close(out_fd);
            }
            tool_write_error("split", "read error", 0);
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }

        saw_input = 1;
        for (i = 0; i < bytes_read; ++i) {
            if (line_len + 1U >= sizeof(line_buffer)) {
                if (write_line_bytes_chunk(line_buffer,
                                           line_len,
                                           &out_fd,
                                           prefix,
                                           index_io,
                                           suffix_length,
                                           numeric_suffixes,
                                           limit,
                                           &bytes_in_part) != 0) {
                    if (out_fd >= 0) {
                        platform_close(out_fd);
                    }
                    return -1;
                }
                line_len = 0U;
            }

            line_buffer[line_len++] = read_buffer[i];
            if (read_buffer[i] == '\n') {
                if (write_line_bytes_chunk(line_buffer,
                                           line_len,
                                           &out_fd,
                                           prefix,
                                           index_io,
                                           suffix_length,
                                           numeric_suffixes,
                                           limit,
                                           &bytes_in_part) != 0) {
                    if (out_fd >= 0) {
                        platform_close(out_fd);
                    }
                    return -1;
                }
                line_len = 0U;
            }
        }
    }

    if (line_len > 0U) {
        if (write_line_bytes_chunk(line_buffer,
                                   line_len,
                                   &out_fd,
                                   prefix,
                                   index_io,
                                   suffix_length,
                                   numeric_suffixes,
                                   limit,
                                   &bytes_in_part) != 0) {
            if (out_fd >= 0) {
                platform_close(out_fd);
            }
            return -1;
        }
    }

    if (!saw_input) {
        if (ensure_output_open(&out_fd, prefix, index_io, suffix_length, numeric_suffixes) != 0) {
            return -1;
        }
    }

    if (out_fd >= 0) {
        platform_close(out_fd);
    }

    return 0;
}

int main(int argc, char **argv) {
    int mode = SPLIT_MODE_LINES;
    unsigned long long limit = 1000ULL;
    unsigned long long chunk_count = 0ULL;
    const char *input_path = 0;
    const char *prefix = "x";
    int argi = 1;
    int input_fd;
    int should_close = 0;
    int out_fd = -1;
    unsigned long long part_index = 0ULL;
    unsigned long long units_in_part = 0ULL;
    unsigned long long suffix_length = 2ULL;
    int numeric_suffixes = 0;
    char buffer[4096];
    int saw_input = 0;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *value_text = 0;

        if (rt_strcmp(argv[argi], "-l") == 0 || rt_strcmp(argv[argi], "--lines") == 0) {
            if (argi + 1 >= argc || parse_size_value(argv[argi + 1], &limit) != 0 || limit == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            mode = SPLIT_MODE_LINES;
            chunk_count = 0ULL;
            argi += 2;
        } else if ((value_text = option_attached_value(argv[argi], "-l", "--lines")) != 0) {
            if (parse_size_value(value_text, &limit) != 0 || limit == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            mode = SPLIT_MODE_LINES;
            chunk_count = 0ULL;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-b") == 0 || rt_strcmp(argv[argi], "--bytes") == 0) {
            if (argi + 1 >= argc || parse_size_value(argv[argi + 1], &limit) != 0 || limit == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            mode = SPLIT_MODE_BYTES;
            chunk_count = 0ULL;
            argi += 2;
        } else if ((value_text = option_attached_value(argv[argi], "-b", "--bytes")) != 0) {
            if (parse_size_value(value_text, &limit) != 0 || limit == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            mode = SPLIT_MODE_BYTES;
            chunk_count = 0ULL;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-C") == 0 || rt_strcmp(argv[argi], "--line-bytes") == 0) {
            if (argi + 1 >= argc || parse_size_value(argv[argi + 1], &limit) != 0 || limit == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            mode = SPLIT_MODE_LINE_BYTES;
            chunk_count = 0ULL;
            argi += 2;
        } else if ((value_text = option_attached_value(argv[argi], "-C", "--line-bytes")) != 0) {
            if (parse_size_value(value_text, &limit) != 0 || limit == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            mode = SPLIT_MODE_LINE_BYTES;
            chunk_count = 0ULL;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-n") == 0 || rt_strcmp(argv[argi], "--number") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &chunk_count, "split", "chunks") != 0 || chunk_count == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            mode = SPLIT_MODE_BYTES;
            argi += 2;
        } else if ((value_text = option_attached_value(argv[argi], "-n", "--number")) != 0) {
            if (tool_parse_uint_arg(value_text, &chunk_count, "split", "chunks") != 0 || chunk_count == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            mode = SPLIT_MODE_BYTES;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-a") == 0 || rt_strcmp(argv[argi], "--suffix-length") == 0) {
            if (argi + 1 >= argc ||
                tool_parse_uint_arg(argv[argi + 1], &suffix_length, "split", "suffix length") != 0 ||
                suffix_length == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if ((value_text = option_attached_value(argv[argi], "-a", "--suffix-length")) != 0) {
            if (tool_parse_uint_arg(value_text, &suffix_length, "split", "suffix length") != 0 || suffix_length == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-d") == 0) {
            numeric_suffixes = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--numeric-suffixes") == 0) {
            numeric_suffixes = 1;
            part_index = 0ULL;
            argi += 1;
        } else if (starts_with_text(argv[argi], "--numeric-suffixes=")) {
            numeric_suffixes = 1;
            if (tool_parse_uint_arg(argv[argi] + 19, &part_index, "split", "numeric suffix start") != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (argi < argc) {
        input_path = argv[argi];
        argi += 1;
    }
    if (argi < argc) {
        prefix = argv[argi];
        argi += 1;
    }
    if (argi != argc) {
        print_usage(argv[0]);
        return 1;
    }

    if (tool_open_input(input_path, &input_fd, &should_close) != 0) {
        tool_write_error("split", "cannot open ", input_path != 0 ? input_path : "stdin");
        return 1;
    }

    if (chunk_count > 0ULL) {
        if (compute_chunk_limit(input_fd, chunk_count, &limit) != 0) {
            tool_close_input(input_fd, should_close);
            tool_write_error("split", "cannot determine size for ", input_path != 0 ? input_path : "stdin");
            return 1;
        }
    }

    if (mode == SPLIT_MODE_LINE_BYTES) {
        int result = split_line_bytes_mode(input_fd, prefix, &part_index, suffix_length, numeric_suffixes, limit);
        tool_close_input(input_fd, should_close);
        return result == 0 ? 0 : 1;
    }

    for (;;) {
        long bytes_read = platform_read(input_fd, buffer, sizeof(buffer));
        long i;

        if (bytes_read < 0) {
            tool_close_input(input_fd, should_close);
            if (out_fd >= 0) {
                platform_close(out_fd);
            }
            tool_write_error("split", "read error", 0);
            return 1;
        }
        if (bytes_read == 0) {
            break;
        }

        saw_input = 1;
        for (i = 0; i < bytes_read; ++i) {
            if (ensure_output_open(&out_fd, prefix, &part_index, suffix_length, numeric_suffixes) != 0) {
                tool_close_input(input_fd, should_close);
                return 1;
            }

            if (rt_write_all(out_fd, &buffer[i], 1U) != 0) {
                tool_close_input(input_fd, should_close);
                platform_close(out_fd);
                tool_write_error("split", "write error", 0);
                return 1;
            }

            if (mode == SPLIT_MODE_LINES) {
                if (buffer[i] == '\n') {
                    units_in_part += 1ULL;
                    if (units_in_part >= limit) {
                        platform_close(out_fd);
                        out_fd = -1;
                        units_in_part = 0ULL;
                    }
                }
            } else {
                units_in_part += 1ULL;
                if (units_in_part >= limit) {
                    platform_close(out_fd);
                    out_fd = -1;
                    units_in_part = 0ULL;
                }
            }
        }
    }

    if (!saw_input) {
        if (ensure_output_open(&out_fd, prefix, &part_index, suffix_length, numeric_suffixes) != 0) {
            tool_close_input(input_fd, should_close);
            return 1;
        }
    }

    tool_close_input(input_fd, should_close);
    if (out_fd >= 0) {
        platform_close(out_fd);
    }

    return 0;
}
