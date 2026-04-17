#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

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

static int skip_input_bytes(int fd, unsigned long long byte_count) {
    char buffer[4096];

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

int main(int argc, char **argv) {
    const char *input_path = 0;
    const char *output_path = 0;
    unsigned long long block_size = 512ULL;
    unsigned long long count = 0ULL;
    unsigned long long skip = 0ULL;
    unsigned long long seek = 0ULL;
    unsigned long long remaining_bytes = 0ULL;
    int in_fd;
    int out_fd = 1;
    int should_close_in = 0;
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
        } else {
            tool_write_usage("dd", "[if=file] [of=file] [bs=n] [count=n] [skip=n] [seek=n]");
            return 1;
        }
    }

    if (tool_open_input(input_path, &in_fd, &should_close_in) != 0) {
        tool_write_error("dd", "cannot open ", "input");
        return 1;
    }

    if (output_path != 0) {
        out_fd = platform_open_write(output_path, 0644U);
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

    if (seek > 0ULL && write_zero_bytes(out_fd, seek * block_size) != 0) {
        tool_close_input(in_fd, should_close_in);
        if (output_path != 0) {
            platform_close(out_fd);
        }
        tool_write_error("dd", "failed while seeking ", "output");
        return 1;
    }

    remaining_bytes = (count > 0ULL) ? (count * block_size) : 0ULL;
    for (;;) {
        size_t to_read = sizeof(buffer);
        long bytes_read;

        if (count > 0ULL) {
            if (remaining_bytes == 0ULL) {
                break;
            }
            if ((unsigned long long)to_read > remaining_bytes) {
                to_read = (size_t)remaining_bytes;
            }
        }

        bytes_read = platform_read(in_fd, buffer, to_read);
        if (bytes_read < 0) {
            tool_close_input(in_fd, should_close_in);
            if (output_path != 0) {
                platform_close(out_fd);
            }
            tool_write_error("dd", "read error", 0);
            return 1;
        }
        if (bytes_read == 0) {
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

        if (count > 0ULL) {
            remaining_bytes -= (unsigned long long)bytes_read;
        }
    }

    tool_close_input(in_fd, should_close_in);
    if (output_path != 0) {
        platform_close(out_fd);
    }
    return 0;
}