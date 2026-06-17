#include "archive_util.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define BUNZIP2_PATH_CAPACITY 1024
#define BUNZIP2_IO_BUFFER 65536U

typedef struct {
    int fd;
    unsigned char buffer[BUNZIP2_IO_BUFFER];
    size_t offset;
    size_t available;
} Bunzip2Input;

static int bunzip2_input_fill(Bunzip2Input *input) {
    long amount;

    amount = platform_read(input->fd, input->buffer, sizeof(input->buffer));
    if (amount < 0) return -1;
    input->offset = 0;
    input->available = (size_t)amount;
    return 0;
}

static int bunzip2_input_read_exact(Bunzip2Input *input, unsigned char *out, size_t count) {
    size_t copied = 0;

    while (copied < count) {
        size_t remaining;
        size_t chunk;

        if (input->offset == input->available) {
            if (bunzip2_input_fill(input) != 0 || input->available == 0) {
                return -1;
            }
        }

        remaining = input->available - input->offset;
        chunk = count - copied;
        if (chunk > remaining) chunk = remaining;

        memcpy(out + copied, input->buffer + input->offset, chunk);
        input->offset += chunk;
        copied += chunk;
    }

    return 0;
}

static int bunzip2_input_ensure(Bunzip2Input *input) {
    if (input->offset < input->available) return 0;
    if (bunzip2_input_fill(input) != 0) return -1;
    return input->available == 0 ? -1 : 0;
}

static int build_output_path(const char *input_path, char *buffer, size_t buffer_size) {
    size_t len = rt_strlen(input_path);

    if (len > 4 && input_path[len - 4] == '.' && input_path[len - 3] == 'b' && input_path[len - 2] == 'z' && input_path[len - 1] == '2') {
        if (len - 3 > buffer_size) {
            return -1;
        }
        memcpy(buffer, input_path, len - 4);
        buffer[len - 4] = '\0';
        return 0;
    }

    if (len + 5 > buffer_size) {
        return -1;
    }

    memcpy(buffer, input_path, len);
    memcpy(buffer + len, ".out", 5);
    return 0;
}

int main(int argc, char **argv) {
    unsigned char header[12];
    unsigned char buffer[BUNZIP2_IO_BUFFER];
    unsigned char run_buffer[BUNZIP2_IO_BUFFER];
    Bunzip2Input input;
    ToolOutputBuffer output;
    char output_path[BUNZIP2_PATH_CAPACITY];
    unsigned int expected_size;
    unsigned int expected_crc;
    unsigned int actual_crc = 0xffffffffU;
    unsigned int output_size = 0;
    int run_fill_ready = 0;
    unsigned char run_fill_value = 0;
    int input_fd;
    int output_fd;

    if (argc != 2) {
        rt_write_line(2, "Usage: bunzip2 file.bz2");
        return 1;
    }

    if (build_output_path(argv[1], output_path, sizeof(output_path)) != 0) {
        rt_write_line(2, "bunzip2: output path too long");
        return 1;
    }

    input_fd = platform_open_read(argv[1]);
    if (input_fd < 0) {
        rt_write_line(2, "bunzip2: cannot open input");
        return 1;
    }

    output_fd = platform_open_write(output_path, 0644U);
    if (output_fd < 0) {
        platform_close(input_fd);
        rt_write_line(2, "bunzip2: cannot open output");
        return 1;
    }

    input.fd = input_fd;
    input.offset = 0;
    input.available = 0;

    if (bunzip2_input_read_exact(&input, header, sizeof(header)) != 0 ||
        header[0] != 'B' || header[1] != 'Z' || header[2] != 'h' || header[3] != '0') {
        platform_close(input_fd);
        platform_close(output_fd);
        rt_write_line(2, "bunzip2: invalid minimal bzip2 header");
        return 1;
    }

    expected_size = archive_read_u32_le(header + 4);
    expected_crc = archive_read_u32_le(header + 8);
    tool_output_buffer_init(&output, output_fd);

    while (output_size < expected_size) {
        unsigned char flag;
        unsigned int count;

        if (bunzip2_input_read_exact(&input, &flag, 1) != 0) {
            platform_close(input_fd);
            platform_close(output_fd);
            rt_write_line(2, "bunzip2: unexpected end of input");
            return 1;
        }

        count = (unsigned int)(flag & 0x7fU) + 1U;
        if (count > expected_size - output_size) {
            platform_close(input_fd);
            platform_close(output_fd);
            rt_write_line(2, "bunzip2: corrupt packet length");
            return 1;
        }

        if ((flag & 0x80U) != 0U) {
            unsigned char value;
            unsigned int remaining = count;

            if (bunzip2_input_read_exact(&input, &value, 1) != 0) {
                platform_close(input_fd);
                platform_close(output_fd);
                return 1;
            }

            if (!run_fill_ready || run_fill_value != value) {
                rt_memset(run_buffer, value, sizeof(run_buffer));
                run_fill_value = value;
                run_fill_ready = 1;
            }

            while (remaining > 0U) {
                unsigned int chunk = (remaining > sizeof(buffer)) ? (unsigned int)sizeof(buffer) : remaining;

                if (tool_output_buffer_write(&output, (const char *)run_buffer, chunk) != 0) {
                    platform_close(input_fd);
                    platform_close(output_fd);
                    return 1;
                }

                actual_crc = archive_crc32_update(actual_crc, run_buffer, chunk);
                output_size += chunk;
                remaining -= chunk;
            }
        } else {
            if (bunzip2_input_ensure(&input) != 0) {
                platform_close(input_fd);
                platform_close(output_fd);
                return 1;
            }

            if (count <= input.available - input.offset) {
                const unsigned char *literal = input.buffer + input.offset;
                if (tool_output_buffer_write(&output, (const char *)literal, count) != 0) {
                    platform_close(input_fd);
                    platform_close(output_fd);
                    return 1;
                }
                actual_crc = archive_crc32_update(actual_crc, literal, count);
                input.offset += count;
            } else {
                if (bunzip2_input_read_exact(&input, buffer, count) != 0 || tool_output_buffer_write(&output, (const char *)buffer, count) != 0) {
                    platform_close(input_fd);
                    platform_close(output_fd);
                    return 1;
                }
                actual_crc = archive_crc32_update(actual_crc, buffer, count);
            }

            output_size += count;
        }
    }

    if (tool_output_buffer_flush(&output) != 0) {
        platform_close(input_fd);
        platform_close(output_fd);
        return 1;
    }

    platform_close(input_fd);
    platform_close(output_fd);

    actual_crc = archive_crc32_finish(actual_crc);
    if (actual_crc != expected_crc) {
        rt_write_line(2, "bunzip2: CRC check failed");
        return 1;
    }

    return 0;
}
