#include "archive_util.h"
#include "platform.h"
#include "runtime.h"

#define GZIP_BLOCK_SIZE 65535
#define GZIP_PATH_CAPACITY 1024

static int build_output_path(const char *input_path, char *buffer, size_t buffer_size) {
    size_t len = rt_strlen(input_path);

    if (len + 4 >= buffer_size) {
        return -1;
    }

    memcpy(buffer, input_path, len);
    buffer[len] = '.';
    buffer[len + 1] = 'g';
    buffer[len + 2] = 'z';
    buffer[len + 3] = '\0';
    return 0;
}

static int write_stored_block(int fd, const unsigned char *data, unsigned int len, int is_last) {
    unsigned char header[5];
    unsigned int nlen = 0xffffU - len;

    header[0] = (unsigned char)(is_last ? 1 : 0);
    header[1] = (unsigned char)(len & 0xffU);
    header[2] = (unsigned char)((len >> 8) & 0xffU);
    header[3] = (unsigned char)(nlen & 0xffU);
    header[4] = (unsigned char)((nlen >> 8) & 0xffU);

    if (rt_write_all(fd, header, sizeof(header)) != 0) {
        return -1;
    }

    return rt_write_all(fd, data, len);
}

static int write_u32_le(int fd, unsigned int value) {
    unsigned char bytes[4];

    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8) & 0xffU);
    bytes[2] = (unsigned char)((value >> 16) & 0xffU);
    bytes[3] = (unsigned char)((value >> 24) & 0xffU);
    return rt_write_all(fd, bytes, sizeof(bytes));
}

int main(int argc, char **argv) {
    unsigned char current[GZIP_BLOCK_SIZE];
    unsigned char next[GZIP_BLOCK_SIZE];
    const unsigned char header[10] = { 0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 };
    char output_path[GZIP_PATH_CAPACITY];
    int input_fd;
    int output_fd;
    long current_size;
    unsigned int crc = 0xffffffffU;
    unsigned int input_size = 0;

    if (argc != 2) {
        rt_write_line(2, "Usage: gzip file");
        return 1;
    }

    if (build_output_path(argv[1], output_path, sizeof(output_path)) != 0) {
        rt_write_line(2, "gzip: output path too long");
        return 1;
    }

    input_fd = platform_open_read(argv[1]);
    if (input_fd < 0) {
        rt_write_line(2, "gzip: cannot open input");
        return 1;
    }

    output_fd = platform_open_write(output_path, 0644U);
    if (output_fd < 0) {
        platform_close(input_fd);
        rt_write_line(2, "gzip: cannot open output");
        return 1;
    }

    if (rt_write_all(output_fd, header, sizeof(header)) != 0) {
        platform_close(input_fd);
        platform_close(output_fd);
        return 1;
    }

    current_size = platform_read(input_fd, current, sizeof(current));
    if (current_size < 0) {
        platform_close(input_fd);
        platform_close(output_fd);
        return 1;
    }

    if (current_size == 0) {
        if (write_stored_block(output_fd, current, 0, 1) != 0) {
            platform_close(input_fd);
            platform_close(output_fd);
            return 1;
        }
    } else {
        for (;;) {
            long next_size = platform_read(input_fd, next, sizeof(next));
            int is_last;

            if (next_size < 0) {
                platform_close(input_fd);
                platform_close(output_fd);
                return 1;
            }

            is_last = (next_size == 0);
            crc = archive_crc32_update(crc, current, (size_t)current_size);
            input_size += (unsigned int)current_size;

            if (write_stored_block(output_fd, current, (unsigned int)current_size, is_last) != 0) {
                platform_close(input_fd);
                platform_close(output_fd);
                return 1;
            }

            if (is_last) {
                break;
            }

            memcpy(current, next, (size_t)next_size);
            current_size = next_size;
        }
    }

    crc ^= 0xffffffffU;
    if (write_u32_le(output_fd, crc) != 0 || write_u32_le(output_fd, input_size) != 0) {
        platform_close(input_fd);
        platform_close(output_fd);
        return 1;
    }

    platform_close(input_fd);
    platform_close(output_fd);
    return 0;
}
