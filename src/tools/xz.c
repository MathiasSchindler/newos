#include "archive_util.h"
#include "platform.h"
#include "runtime.h"

#define XZ_PATH_CAPACITY 1024
#define XZ_CHUNK_SIZE 65536U
#define XZ_SCAN_BUFFER 4096

static int build_output_path(const char *input_path, char *buffer, size_t buffer_size) {
    size_t len = rt_strlen(input_path);

    if (len + 4 > buffer_size) {
        return -1;
    }

    memcpy(buffer, input_path, len);
    memcpy(buffer + len, ".xz", 4);
    return 0;
}

static size_t append_vli(unsigned char *buffer, size_t offset, unsigned long long value) {
    do {
        unsigned char byte = (unsigned char)(value & 0x7fU);
        value >>= 7;
        if (value != 0ULL) {
            byte |= 0x80U;
        }
        buffer[offset++] = byte;
    } while (value != 0ULL);

    return offset;
}

int main(int argc, char **argv) {
    unsigned char stream_header[12] = { 0xfd, '7', 'z', 'X', 'Z', 0x00, 0x00, 0x01, 0, 0, 0, 0 };
    unsigned char block_header[12] = { 0x02, 0x00, 0x21, 0x01, 0x10, 0x00, 0x00, 0x00, 0, 0, 0, 0 };
    unsigned char index_bytes[64];
    unsigned char footer[12] = { 0 };
    unsigned char buffer[XZ_SCAN_BUFFER];
    unsigned int crc = 0xffffffffU;
    unsigned long long input_size = 0;
    unsigned long long block_data_size = 0;
    unsigned long long unpadded_size;
    unsigned int backward_size;
    char output_path[XZ_PATH_CAPACITY];
    int input_fd;
    int output_fd;
    size_t index_length = 0;
    int first_chunk = 1;

    if (argc != 2) {
        rt_write_line(2, "Usage: xz file");
        return 1;
    }

    if (build_output_path(argv[1], output_path, sizeof(output_path)) != 0) {
        rt_write_line(2, "xz: output path too long");
        return 1;
    }

    input_fd = platform_open_read(argv[1]);
    if (input_fd < 0) {
        rt_write_line(2, "xz: cannot open input");
        return 1;
    }

    for (;;) {
        long bytes_read = platform_read(input_fd, buffer, sizeof(buffer));

        if (bytes_read < 0) {
            platform_close(input_fd);
            rt_write_line(2, "xz: read failed");
            return 1;
        }

        if (bytes_read == 0) {
            break;
        }

        crc = archive_crc32_update(crc, buffer, (size_t)bytes_read);
        input_size += (unsigned long long)bytes_read;
    }

    platform_close(input_fd);

    input_fd = platform_open_read(argv[1]);
    if (input_fd < 0) {
        rt_write_line(2, "xz: cannot reopen input");
        return 1;
    }

    output_fd = platform_open_write(output_path, 0644U);
    if (output_fd < 0) {
        platform_close(input_fd);
        rt_write_line(2, "xz: cannot open output");
        return 1;
    }

    archive_store_u32_le(stream_header + 8, archive_crc32_finish(archive_crc32_update(0xffffffffU, stream_header + 6, 2)));
    archive_store_u32_le(block_header + 8, archive_crc32_finish(archive_crc32_update(0xffffffffU, block_header, 8)));

    if (rt_write_all(output_fd, stream_header, sizeof(stream_header)) != 0 ||
        rt_write_all(output_fd, block_header, sizeof(block_header)) != 0) {
        platform_close(input_fd);
        platform_close(output_fd);
        return 1;
    }

    for (;;) {
        long bytes_read = platform_read(input_fd, buffer, sizeof(buffer));

        if (bytes_read < 0) {
            platform_close(input_fd);
            platform_close(output_fd);
            rt_write_line(2, "xz: read failed");
            return 1;
        }

        if (bytes_read == 0) {
            break;
        }

        {
            unsigned char chunk_header[3];
            unsigned int chunk_size = (unsigned int)bytes_read - 1U;

            chunk_header[0] = (unsigned char)(first_chunk ? 0x01U : 0x02U);
            chunk_header[1] = (unsigned char)((chunk_size >> 8) & 0xffU);
            chunk_header[2] = (unsigned char)(chunk_size & 0xffU);

            if (rt_write_all(output_fd, chunk_header, sizeof(chunk_header)) != 0 ||
                rt_write_all(output_fd, buffer, (size_t)bytes_read) != 0) {
                platform_close(input_fd);
                platform_close(output_fd);
                return 1;
            }

            block_data_size += (unsigned long long)bytes_read + 3ULL;
            first_chunk = 0;
        }
    }

    {
        unsigned char end_marker = 0x00;
        unsigned char check[4];
        unsigned long long block_padding;
        unsigned long long block_size_before_check;

        if (rt_write_all(output_fd, &end_marker, 1) != 0) {
            platform_close(input_fd);
            platform_close(output_fd);
            return 1;
        }
        block_data_size += 1ULL;

        block_size_before_check = sizeof(block_header) + block_data_size;
        block_padding = (4ULL - (block_size_before_check & 3ULL)) & 3ULL;
        while (block_padding > 0ULL) {
            unsigned char zero = 0;
            if (rt_write_all(output_fd, &zero, 1) != 0) {
                platform_close(input_fd);
                platform_close(output_fd);
                return 1;
            }
            block_padding -= 1ULL;
        }

        archive_store_u32_le(check, archive_crc32_finish(crc));
        if (rt_write_all(output_fd, check, sizeof(check)) != 0) {
            platform_close(input_fd);
            platform_close(output_fd);
            return 1;
        }

        unpadded_size = sizeof(block_header) + block_data_size + sizeof(check);
    }

    index_bytes[index_length++] = 0x00;
    index_length = append_vli(index_bytes, index_length, 1ULL);
    index_length = append_vli(index_bytes, index_length, unpadded_size);
    index_length = append_vli(index_bytes, index_length, input_size);
    while ((index_length & 3U) != 0U) {
        index_bytes[index_length++] = 0x00;
    }

    if (rt_write_all(output_fd, index_bytes, index_length) != 0) {
        platform_close(input_fd);
        platform_close(output_fd);
        return 1;
    }

    {
        unsigned char index_crc[4];
        unsigned int crc_value = archive_crc32_finish(archive_crc32_update(0xffffffffU, index_bytes, index_length));
        archive_store_u32_le(index_crc, crc_value);
        if (rt_write_all(output_fd, index_crc, sizeof(index_crc)) != 0) {
            platform_close(input_fd);
            platform_close(output_fd);
            return 1;
        }
    }

    backward_size = (unsigned int)(((index_length + 4U) / 4U) - 1U);
    archive_store_u32_le(footer + 4, backward_size);
    footer[8] = 0x00;
    footer[9] = 0x01;
    footer[10] = 'Y';
    footer[11] = 'Z';
    archive_store_u32_le(footer, archive_crc32_finish(archive_crc32_update(0xffffffffU, footer + 4, 6)));

    if (rt_write_all(output_fd, footer, sizeof(footer)) != 0) {
        platform_close(input_fd);
        platform_close(output_fd);
        return 1;
    }

    platform_close(input_fd);
    platform_close(output_fd);
    return 0;
}
