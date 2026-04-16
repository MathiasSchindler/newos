#include "archive_util.h"
#include "platform.h"
#include "runtime.h"

#define BZIP_PATH_CAPACITY 1024
#define BZIP_IO_BUFFER 4096
#define BZIP_PACKET_LIMIT 128

static int build_output_path(const char *input_path, char *buffer, size_t buffer_size) {
    size_t len = rt_strlen(input_path);

    if (len + 5 > buffer_size) {
        return -1;
    }

    memcpy(buffer, input_path, len);
    memcpy(buffer + len, ".bz2", 5);
    return 0;
}

static int flush_literal_packet(int fd, unsigned char *literal, size_t *literal_len) {
    unsigned char header;

    if (*literal_len == 0) {
        return 0;
    }

    header = (unsigned char)(*literal_len - 1);
    if (rt_write_all(fd, &header, 1) != 0 || rt_write_all(fd, literal, *literal_len) != 0) {
        return -1;
    }

    *literal_len = 0;
    return 0;
}

static int write_run_packet(int fd, unsigned char value, size_t run_len) {
    while (run_len > 0) {
        size_t chunk = (run_len > BZIP_PACKET_LIMIT) ? BZIP_PACKET_LIMIT : run_len;
        unsigned char packet[2];

        packet[0] = (unsigned char)(0x80U | (unsigned char)(chunk - 1));
        packet[1] = value;
        if (rt_write_all(fd, packet, sizeof(packet)) != 0) {
            return -1;
        }

        run_len -= chunk;
    }

    return 0;
}

static int append_literal_byte(int fd, unsigned char *literal, size_t *literal_len, unsigned char value) {
    literal[*literal_len] = value;
    *literal_len += 1;

    if (*literal_len == BZIP_PACKET_LIMIT) {
        return flush_literal_packet(fd, literal, literal_len);
    }

    return 0;
}

static int compress_stream(int input_fd, int output_fd) {
    unsigned char input[BZIP_IO_BUFFER];
    unsigned char literal[BZIP_PACKET_LIMIT];
    size_t literal_len = 0;
    int have_run = 0;
    unsigned char run_value = 0;
    size_t run_len = 0;

    for (;;) {
        long bytes_read = platform_read(input_fd, input, sizeof(input));
        long i;

        if (bytes_read < 0) {
            return -1;
        }

        if (bytes_read == 0) {
            break;
        }

        for (i = 0; i < bytes_read; ++i) {
            unsigned char value = input[i];

            if (!have_run) {
                run_value = value;
                run_len = 1;
                have_run = 1;
                continue;
            }

            if (value == run_value && run_len < BZIP_PACKET_LIMIT) {
                run_len += 1;
                continue;
            }

            if (run_len >= 4) {
                if (flush_literal_packet(output_fd, literal, &literal_len) != 0 ||
                    write_run_packet(output_fd, run_value, run_len) != 0) {
                    return -1;
                }
            } else {
                size_t j;
                for (j = 0; j < run_len; ++j) {
                    if (append_literal_byte(output_fd, literal, &literal_len, run_value) != 0) {
                        return -1;
                    }
                }
            }

            run_value = value;
            run_len = 1;
        }
    }

    if (have_run) {
        if (run_len >= 4) {
            if (flush_literal_packet(output_fd, literal, &literal_len) != 0 ||
                write_run_packet(output_fd, run_value, run_len) != 0) {
                return -1;
            }
        } else {
            size_t j;
            for (j = 0; j < run_len; ++j) {
                if (append_literal_byte(output_fd, literal, &literal_len, run_value) != 0) {
                    return -1;
                }
            }
        }
    }

    return flush_literal_packet(output_fd, literal, &literal_len);
}

int main(int argc, char **argv) {
    unsigned char header[12] = { 'B', 'Z', 'h', '0', 0, 0, 0, 0, 0, 0, 0, 0 };
    unsigned char buffer[BZIP_IO_BUFFER];
    char output_path[BZIP_PATH_CAPACITY];
    unsigned int crc = 0xffffffffU;
    unsigned int input_size = 0;
    int input_fd;
    int output_fd;

    if (argc != 2) {
        rt_write_line(2, "Usage: bzip2 file");
        return 1;
    }

    if (build_output_path(argv[1], output_path, sizeof(output_path)) != 0) {
        rt_write_line(2, "bzip2: output path too long");
        return 1;
    }

    input_fd = platform_open_read(argv[1]);
    if (input_fd < 0) {
        rt_write_line(2, "bzip2: cannot open input");
        return 1;
    }

    for (;;) {
        long bytes_read = platform_read(input_fd, buffer, sizeof(buffer));

        if (bytes_read < 0) {
            platform_close(input_fd);
            rt_write_line(2, "bzip2: read failed");
            return 1;
        }

        if (bytes_read == 0) {
            break;
        }

        crc = archive_crc32_update(crc, buffer, (size_t)bytes_read);
        input_size += (unsigned int)bytes_read;
    }

    platform_close(input_fd);

    archive_store_u32_le(header + 4, input_size);
    archive_store_u32_le(header + 8, archive_crc32_finish(crc));

    input_fd = platform_open_read(argv[1]);
    if (input_fd < 0) {
        rt_write_line(2, "bzip2: cannot reopen input");
        return 1;
    }

    output_fd = platform_open_write(output_path, 0644U);
    if (output_fd < 0) {
        platform_close(input_fd);
        rt_write_line(2, "bzip2: cannot open output");
        return 1;
    }

    if (rt_write_all(output_fd, header, sizeof(header)) != 0 || compress_stream(input_fd, output_fd) != 0) {
        platform_close(input_fd);
        platform_close(output_fd);
        rt_write_line(2, "bzip2: write failed");
        return 1;
    }

    platform_close(input_fd);
    platform_close(output_fd);
    return 0;
}
