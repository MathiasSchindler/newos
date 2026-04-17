#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

enum {
    DD_STATUS_DEFAULT = 0,
    DD_STATUS_NOXFER = 1,
    DD_STATUS_NONE = 2
};

typedef struct {
    int sync_blocks;
    int continue_on_error;
    int preserve_output;
    int status_mode;
} DdOptions;

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

static int parse_number_arg(const char *text, unsigned long long *value_out, const char *what) {
    return tool_parse_uint_arg(text, value_out, "dd", what);
}

static int parse_keyword_list(const char *text, DdOptions *options) {
    size_t index = 0U;

    while (text[index] != '\0') {
        char token[32];
        size_t token_length = 0U;

        while (text[index] != '\0' && text[index] != ',') {
            if (token_length + 1U >= sizeof(token)) {
                return -1;
            }
            token[token_length++] = text[index++];
        }
        token[token_length] = '\0';

        if (token_length == 0U) {
            return -1;
        }
        if (rt_strcmp(token, "sync") == 0) {
            options->sync_blocks = 1;
        } else if (rt_strcmp(token, "noerror") == 0) {
            options->continue_on_error = 1;
        } else if (rt_strcmp(token, "notrunc") == 0) {
            options->preserve_output = 1;
        } else {
            return -1;
        }

        if (text[index] == ',') {
            index += 1U;
        }
    }

    return 0;
}

static int parse_status_arg(const char *text, DdOptions *options) {
    if (rt_strcmp(text, "default") == 0 || rt_strcmp(text, "progress") == 0) {
        options->status_mode = DD_STATUS_DEFAULT;
        return 0;
    }
    if (rt_strcmp(text, "noxfer") == 0) {
        options->status_mode = DD_STATUS_NOXFER;
        return 0;
    }
    if (rt_strcmp(text, "none") == 0) {
        options->status_mode = DD_STATUS_NONE;
        return 0;
    }
    return -1;
}

static int skip_input_bytes(int fd, unsigned long long byte_count) {
    char buffer[4096];

    if (byte_count > 0ULL && platform_seek(fd, (long long)byte_count, PLATFORM_SEEK_CUR) >= 0) {
        return 0;
    }

    while (byte_count > 0ULL) {
        size_t chunk = byte_count > sizeof(buffer) ? sizeof(buffer) : (size_t)byte_count;
        long bytes_read = platform_read(fd, buffer, chunk);
        if (bytes_read <= 0) {
            return -1;
        }
        byte_count -= (unsigned long long)bytes_read;
    }

    return 0;
}

static int write_zero_bytes(int fd, unsigned long long byte_count) {
    char buffer[4096];
    rt_memset(buffer, 0, sizeof(buffer));

    while (byte_count > 0ULL) {
        size_t chunk = byte_count > sizeof(buffer) ? sizeof(buffer) : (size_t)byte_count;
        if (rt_write_all(fd, buffer, chunk) != 0) {
            return -1;
        }
        byte_count -= (unsigned long long)chunk;
    }

    return 0;
}

static int seek_output_bytes(int fd, unsigned long long byte_count) {
    if (byte_count > 0ULL && platform_seek(fd, (long long)byte_count, PLATFORM_SEEK_CUR) >= 0) {
        return 0;
    }
    return write_zero_bytes(fd, byte_count);
}

static void report_summary(
    const DdOptions *options,
    unsigned long long full_in,
    unsigned long long partial_in,
    unsigned long long full_out,
    unsigned long long partial_out,
    unsigned long long bytes_written
) {
    if (options->status_mode == DD_STATUS_NONE) {
        return;
    }

    rt_write_uint(2, full_in);
    rt_write_char(2, '+');
    rt_write_uint(2, partial_in);
    rt_write_cstr(2, " records in\n");
    rt_write_uint(2, full_out);
    rt_write_char(2, '+');
    rt_write_uint(2, partial_out);
    rt_write_cstr(2, " records out\n");

    if (options->status_mode != DD_STATUS_NOXFER) {
        rt_write_uint(2, bytes_written);
        rt_write_cstr(2, " bytes copied\n");
    }
}

int main(int argc, char **argv) {
    const char *input_path = 0;
    const char *output_path = 0;
    unsigned long long block_size = 512ULL;
    unsigned long long count = 0ULL;
    unsigned long long skip = 0ULL;
    unsigned long long seek = 0ULL;
    unsigned long long blocks_processed = 0ULL;
    unsigned long long full_in = 0ULL;
    unsigned long long partial_in = 0ULL;
    unsigned long long full_out = 0ULL;
    unsigned long long partial_out = 0ULL;
    unsigned long long bytes_written = 0ULL;
    int in_fd;
    int out_fd = 1;
    int should_close_in = 0;
    DdOptions options = { 0, 0, 0, DD_STATUS_DEFAULT };
    int i;
    char buffer[4096];

    for (i = 1; i < argc; ++i) {
        if (starts_with(argv[i], "if=")) {
            input_path = argv[i] + 3;
        } else if (starts_with(argv[i], "of=")) {
            output_path = argv[i] + 3;
        } else if (starts_with(argv[i], "bs=")) {
            if (parse_number_arg(argv[i] + 3, &block_size, "block size") != 0 || block_size == 0ULL) {
                tool_write_error("dd", "invalid ", "block size");
                return 1;
            }
        } else if (starts_with(argv[i], "count=")) {
            if (parse_number_arg(argv[i] + 6, &count, "count") != 0) {
                return 1;
            }
        } else if (starts_with(argv[i], "skip=")) {
            if (parse_number_arg(argv[i] + 5, &skip, "skip") != 0) {
                return 1;
            }
        } else if (starts_with(argv[i], "seek=")) {
            if (parse_number_arg(argv[i] + 5, &seek, "seek") != 0) {
                return 1;
            }
        } else if (starts_with(argv[i], "conv=")) {
            if (parse_keyword_list(argv[i] + 5, &options) != 0) {
                tool_write_error("dd", "invalid ", "conversion");
                return 1;
            }
        } else if (starts_with(argv[i], "status=")) {
            if (parse_status_arg(argv[i] + 7, &options) != 0) {
                tool_write_error("dd", "invalid ", "status");
                return 1;
            }
        } else {
            tool_write_usage("dd", "[if=file] [of=file] [bs=n] [count=n] [skip=n] [seek=n] [conv=sync,noerror,notrunc] [status=default|noxfer|none]");
            return 1;
        }
    }

    if (tool_open_input(input_path, &in_fd, &should_close_in) != 0) {
        tool_write_error("dd", "cannot open ", "input");
        return 1;
    }

    if (output_path != 0) {
        out_fd = platform_open_write_mode(output_path, 0644U, options.preserve_output ? 0 : 1);
        if (out_fd < 0) {
            tool_close_input(in_fd, should_close_in);
            tool_write_error("dd", "cannot open ", "output");
            return 1;
        }
    }

    if (skip > 0ULL && skip_input_bytes(in_fd, skip * block_size) != 0) {
        tool_close_input(in_fd, should_close_in);
        if (output_path != 0) {
            platform_close(out_fd);
        }
        tool_write_error("dd", "failed while skipping ", "input");
        return 1;
    }

    if (seek > 0ULL && seek_output_bytes(out_fd, seek * block_size) != 0) {
        tool_close_input(in_fd, should_close_in);
        if (output_path != 0) {
            platform_close(out_fd);
        }
        tool_write_error("dd", "failed while seeking ", "output");
        return 1;
    }

    for (;;) {
        unsigned long long bytes_in_block = 0ULL;
        unsigned long long bytes_out_block = 0ULL;
        int hit_eof = 0;
        int had_error = 0;

        if (count > 0ULL && blocks_processed >= count) {
            break;
        }

        while (bytes_in_block < block_size) {
            size_t to_read = (block_size - bytes_in_block) > (unsigned long long)sizeof(buffer)
                                 ? sizeof(buffer)
                                 : (size_t)(block_size - bytes_in_block);
            long bytes_read = platform_read(in_fd, buffer, to_read);

            if (bytes_read < 0) {
                had_error = 1;
                if (!options.continue_on_error) {
                    tool_close_input(in_fd, should_close_in);
                    if (output_path != 0) {
                        platform_close(out_fd);
                    }
                    tool_write_error("dd", "read error", 0);
                    return 1;
                }
                tool_write_error("dd", "read error", 0);
                if (bytes_in_block < block_size &&
                    platform_seek(in_fd, (long long)(block_size - bytes_in_block), PLATFORM_SEEK_CUR) < 0) {
                    hit_eof = 1;
                }
                break;
            }
            if (bytes_read == 0) {
                hit_eof = 1;
                break;
            }
            if (rt_write_all(out_fd, buffer, (size_t)bytes_read) != 0) {
                tool_close_input(in_fd, should_close_in);
                if (output_path != 0) {
                    platform_close(out_fd);
                }
                tool_write_error("dd", "write error", 0);
                return 1;
            }

            bytes_in_block += (unsigned long long)bytes_read;
            bytes_out_block += (unsigned long long)bytes_read;
            bytes_written += (unsigned long long)bytes_read;
            if ((size_t)bytes_read < to_read) {
                hit_eof = 1;
                break;
            }
        }

        if (bytes_in_block == 0ULL && !had_error) {
            break;
        }

        if (options.sync_blocks && bytes_out_block < block_size) {
            if (write_zero_bytes(out_fd, block_size - bytes_out_block) != 0) {
                tool_close_input(in_fd, should_close_in);
                if (output_path != 0) {
                    platform_close(out_fd);
                }
                tool_write_error("dd", "write error", 0);
                return 1;
            }
            bytes_written += block_size - bytes_out_block;
            bytes_out_block = block_size;
        }

        if (bytes_in_block == block_size && !had_error) {
            full_in += 1ULL;
        } else {
            partial_in += 1ULL;
        }
        if (bytes_out_block == block_size) {
            full_out += 1ULL;
        } else if (bytes_out_block > 0ULL) {
            partial_out += 1ULL;
        }

        blocks_processed += 1ULL;
        if (hit_eof) {
            break;
        }
    }

    tool_close_input(in_fd, should_close_in);
    if (output_path != 0) {
        platform_close(out_fd);
    }
    report_summary(&options, full_in, partial_in, full_out, partial_out, bytes_written);
    return 0;
}
