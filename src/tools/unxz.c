#include "archive_util.h"
#include "platform.h"
#include "runtime.h"

#define UNXZ_PATH_CAPACITY 1024
#define UNXZ_BUFFER_SIZE 4096

static int build_output_path(const char *input_path, char *buffer, size_t buffer_size) {
    size_t len = rt_strlen(input_path);

    if (len > 3 && input_path[len - 3] == '.' && input_path[len - 2] == 'x' && input_path[len - 1] == 'z') {
        if (len - 2 > buffer_size) {
            return -1;
        }
        memcpy(buffer, input_path, len - 3);
        buffer[len - 3] = '\0';
        return 0;
    }

    if (len + 5 > buffer_size) {
        return -1;
    }

    memcpy(buffer, input_path, len);
    memcpy(buffer + len, ".out", 5);
    return 0;
}

static int read_vli_from_memory(const unsigned char *buffer, size_t buffer_size, size_t *offset, unsigned long long *value_out) {
    unsigned long long value = 0;
    unsigned int shift = 0;

    while (*offset < buffer_size && shift < 63U) {
        unsigned char byte = buffer[*offset];
        *offset += 1;
        value |= ((unsigned long long)(byte & 0x7fU)) << shift;
        if ((byte & 0x80U) == 0U) {
            *value_out = value;
            return 0;
        }
        shift += 7U;
    }

    return -1;
}

static int skip_bytes(int fd, unsigned long long count) {
    unsigned char buffer[256];

    while (count > 0ULL) {
        size_t chunk = (count > sizeof(buffer)) ? sizeof(buffer) : (size_t)count;
        if (archive_read_exact(fd, buffer, chunk) != 0) {
            return -1;
        }
        count -= (unsigned long long)chunk;
    }

    return 0;
}

int main(int argc, char **argv) {
    unsigned char stream_header[12];
    unsigned char block_header[32];
    unsigned char buffer[UNXZ_BUFFER_SIZE];
    char output_path[UNXZ_PATH_CAPACITY];
    int input_fd;
    int output_fd;
    unsigned int check_type;

    if (argc != 2) {
        rt_write_line(2, "Usage: unxz file.xz");
        return 1;
    }

    if (build_output_path(argv[1], output_path, sizeof(output_path)) != 0) {
        rt_write_line(2, "unxz: output path too long");
        return 1;
    }

    input_fd = platform_open_read(argv[1]);
    if (input_fd < 0) {
        rt_write_line(2, "unxz: cannot open input");
        return 1;
    }

    output_fd = platform_open_write(output_path, 0644U);
    if (output_fd < 0) {
        platform_close(input_fd);
        rt_write_line(2, "unxz: cannot open output");
        return 1;
    }

    if (archive_read_exact(input_fd, stream_header, sizeof(stream_header)) != 0 ||
        stream_header[0] != 0xfd || stream_header[1] != '7' || stream_header[2] != 'z' ||
        stream_header[3] != 'X' || stream_header[4] != 'Z' || stream_header[5] != 0x00) {
        platform_close(input_fd);
        platform_close(output_fd);
        rt_write_line(2, "unxz: invalid xz header");
        return 1;
    }

    check_type = stream_header[7] & 0x0fU;

    for (;;) {
        unsigned char size_byte;
        unsigned int header_size;
        unsigned int block_bytes;

        if (archive_read_exact(input_fd, &size_byte, 1) != 0) {
            platform_close(input_fd);
            platform_close(output_fd);
            rt_write_line(2, "unxz: truncated stream");
            return 1;
        }

        if (size_byte == 0x00) {
            break;
        }

        header_size = (unsigned int)(4U * ((unsigned int)size_byte + 1U));
        if (header_size > sizeof(block_header) || header_size < 8U) {
            platform_close(input_fd);
            platform_close(output_fd);
            rt_write_line(2, "unxz: unsupported block header size");
            return 1;
        }

        block_header[0] = size_byte;
        if (archive_read_exact(input_fd, block_header + 1, header_size - 1U) != 0) {
            platform_close(input_fd);
            platform_close(output_fd);
            return 1;
        }

        {
            unsigned int header_crc = archive_read_u32_le(block_header + header_size - 4U);
            unsigned int calc_crc = archive_crc32_finish(archive_crc32_update(0xffffffffU, block_header, header_size - 4U));
            unsigned char flags = block_header[1];
            size_t offset = 2;
            unsigned long long filter_id = 0;
            unsigned long long props_size = 0;

            if (header_crc != calc_crc || (flags & 0x03U) != 0U) {
                platform_close(input_fd);
                platform_close(output_fd);
                rt_write_line(2, "unxz: unsupported xz block header");
                return 1;
            }

            if (read_vli_from_memory(block_header, header_size - 4U, &offset, &filter_id) != 0 ||
                read_vli_from_memory(block_header, header_size - 4U, &offset, &props_size) != 0 ||
                filter_id != 0x21ULL || props_size != 1ULL) {
                platform_close(input_fd);
                platform_close(output_fd);
                rt_write_line(2, "unxz: unsupported filter chain");
                return 1;
            }
        }

        block_bytes = header_size;
        {
            unsigned int actual_crc = 0xffffffffU;
            unsigned long long output_size = 0;

            for (;;) {
                unsigned char control;

                if (archive_read_exact(input_fd, &control, 1) != 0) {
                    platform_close(input_fd);
                    platform_close(output_fd);
                    return 1;
                }
                block_bytes += 1U;

                if (control == 0x00U) {
                    break;
                }

                if (control != 0x01U && control != 0x02U) {
                    platform_close(input_fd);
                    platform_close(output_fd);
                    rt_write_line(2, "unxz: unsupported compressed chunk");
                    return 1;
                }

                {
                    unsigned char size_bytes[2];
                    unsigned int remaining;

                    if (archive_read_exact(input_fd, size_bytes, 2) != 0) {
                        platform_close(input_fd);
                        platform_close(output_fd);
                        return 1;
                    }
                    block_bytes += 2U;
                    remaining = (((unsigned int)size_bytes[0] << 8) | (unsigned int)size_bytes[1]) + 1U;

                    while (remaining > 0U) {
                        unsigned int chunk = (remaining > sizeof(buffer)) ? (unsigned int)sizeof(buffer) : remaining;
                        if (archive_read_exact(input_fd, buffer, chunk) != 0 || rt_write_all(output_fd, buffer, chunk) != 0) {
                            platform_close(input_fd);
                            platform_close(output_fd);
                            return 1;
                        }
                        actual_crc = archive_crc32_update(actual_crc, buffer, chunk);
                        output_size += (unsigned long long)chunk;
                        block_bytes += chunk;
                        remaining -= chunk;
                    }
                }
            }

            if (skip_bytes(input_fd, (4U - (block_bytes & 3U)) & 3U) != 0) {
                platform_close(input_fd);
                platform_close(output_fd);
                return 1;
            }

            if (check_type == 1U) {
                unsigned char check[4];
                if (archive_read_exact(input_fd, check, sizeof(check)) != 0) {
                    platform_close(input_fd);
                    platform_close(output_fd);
                    return 1;
                }
                if (archive_read_u32_le(check) != archive_crc32_finish(actual_crc)) {
                    platform_close(input_fd);
                    platform_close(output_fd);
                    rt_write_line(2, "unxz: block CRC check failed");
                    return 1;
                }
            } else if (check_type != 0U) {
                platform_close(input_fd);
                platform_close(output_fd);
                rt_write_line(2, "unxz: unsupported integrity check");
                return 1;
            }

            (void)output_size;
        }
    }

    platform_close(input_fd);
    platform_close(output_fd);
    return 0;
}
