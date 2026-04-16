#include "archive_util.h"
#include "platform.h"
#include "runtime.h"

#define BUNZIP2_PATH_CAPACITY 1024
#define BUNZIP2_IO_BUFFER 4096

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
    char output_path[BUNZIP2_PATH_CAPACITY];
    unsigned int expected_size;
    unsigned int expected_crc;
    unsigned int actual_crc = 0xffffffffU;
    unsigned int output_size = 0;
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

    if (archive_read_exact(input_fd, header, sizeof(header)) != 0 ||
        header[0] != 'B' || header[1] != 'Z' || header[2] != 'h' || header[3] != '0') {
        platform_close(input_fd);
        platform_close(output_fd);
        rt_write_line(2, "bunzip2: invalid minimal bzip2 header");
        return 1;
    }

    expected_size = archive_read_u32_le(header + 4);
    expected_crc = archive_read_u32_le(header + 8);

    while (output_size < expected_size) {
        unsigned char flag;
        unsigned int count;

        if (archive_read_exact(input_fd, &flag, 1) != 0) {
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

            if (archive_read_exact(input_fd, &value, 1) != 0) {
                platform_close(input_fd);
                platform_close(output_fd);
                return 1;
            }

            while (remaining > 0U) {
                unsigned int chunk = (remaining > sizeof(buffer)) ? (unsigned int)sizeof(buffer) : remaining;
                unsigned int i;

                for (i = 0; i < chunk; ++i) {
                    buffer[i] = value;
                }

                if (rt_write_all(output_fd, buffer, chunk) != 0) {
                    platform_close(input_fd);
                    platform_close(output_fd);
                    return 1;
                }

                actual_crc = archive_crc32_update(actual_crc, buffer, chunk);
                output_size += chunk;
                remaining -= chunk;
            }
        } else {
            if (archive_read_exact(input_fd, buffer, count) != 0 || rt_write_all(output_fd, buffer, count) != 0) {
                platform_close(input_fd);
                platform_close(output_fd);
                return 1;
            }

            actual_crc = archive_crc32_update(actual_crc, buffer, count);
            output_size += count;
        }
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
