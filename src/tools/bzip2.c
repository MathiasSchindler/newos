#include "archive_util.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define BZIP_PATH_CAPACITY 1024
#define BZIP_IO_BUFFER 65536U
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

static int flush_literal_packet(ToolOutputBuffer *output, unsigned char *literal, size_t *literal_len) {
    unsigned char packet[1 + BZIP_PACKET_LIMIT];

    if (*literal_len == 0) {
        return 0;
    }

    packet[0] = (unsigned char)(*literal_len - 1U);
    memcpy(packet + 1, literal, *literal_len);
    if (tool_output_buffer_write(output, (const char *)packet, *literal_len + 1U) != 0) {
        return -1;
    }

    *literal_len = 0;
    return 0;
}

static int write_run_packet(ToolOutputBuffer *output, unsigned char value, size_t run_len) {
    while (run_len > 0) {
        size_t chunk = (run_len > BZIP_PACKET_LIMIT) ? BZIP_PACKET_LIMIT : run_len;
        unsigned char packet[2];

        packet[0] = (unsigned char)(0x80U | (unsigned char)(chunk - 1));
        packet[1] = value;
        if (tool_output_buffer_write(output, (const char *)packet, sizeof(packet)) != 0) {
            return -1;
        }

        run_len -= chunk;
    }

    return 0;
}

static int compress_stream(int input_fd, ToolOutputBuffer *output, unsigned int *crc_out, unsigned int *input_size_out) {
    unsigned char input[BZIP_IO_BUFFER];
    unsigned char literal[BZIP_PACKET_LIMIT];
    size_t literal_len = 0;
    int have_run = 0;
    unsigned char run_value = 0;
    size_t run_len = 0;
    unsigned int crc = 0xffffffffU;
    unsigned int input_size = 0;

    for (;;) {
        long bytes_read = platform_read(input_fd, input, sizeof(input));
        long i;

        if (bytes_read < 0) {
            return -1;
        }

        if (bytes_read == 0) {
            break;
        }

        crc = archive_crc32_update(crc, input, (size_t)bytes_read);
        input_size += (unsigned int)bytes_read;

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
                if (flush_literal_packet(output, literal, &literal_len) != 0 ||
                    write_run_packet(output, run_value, run_len) != 0) {
                    return -1;
                }
            } else {
                size_t j;
                for (j = 0; j < run_len; ++j) {
                    literal[literal_len] = run_value;
                    literal_len += 1;
                    if (literal_len == BZIP_PACKET_LIMIT && flush_literal_packet(output, literal, &literal_len) != 0) {
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
            if (flush_literal_packet(output, literal, &literal_len) != 0 ||
                write_run_packet(output, run_value, run_len) != 0) {
                return -1;
            }
        } else {
            size_t j;
            for (j = 0; j < run_len; ++j) {
                literal[literal_len] = run_value;
                literal_len += 1;
                if (literal_len == BZIP_PACKET_LIMIT && flush_literal_packet(output, literal, &literal_len) != 0) {
                    return -1;
                }
            }
        }
    }

    *crc_out = archive_crc32_finish(crc);
    *input_size_out = input_size;
    return flush_literal_packet(output, literal, &literal_len);
}

int main(int argc, char **argv) {
    unsigned char header[12] = { 'B', 'Z', 'h', '0', 0, 0, 0, 0, 0, 0, 0, 0 };
    ToolOutputBuffer output;
    char output_path[BZIP_PATH_CAPACITY];
    unsigned int input_size = 0;
    unsigned int crc = 0;
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

    output_fd = platform_open_write(output_path, 0644U);
    if (output_fd < 0) {
        platform_close(input_fd);
        rt_write_line(2, "bzip2: cannot open output");
        return 1;
    }

    tool_output_buffer_init(&output, output_fd);
    if (rt_write_all(output_fd, header, sizeof(header)) != 0 ||
        compress_stream(input_fd, &output, &crc, &input_size) != 0 ||
        tool_output_buffer_flush(&output) != 0) {
        platform_close(input_fd);
        platform_close(output_fd);
        rt_write_line(2, "bzip2: write failed");
        return 1;
    }

    archive_store_u32_le(header + 4, input_size);
    archive_store_u32_le(header + 8, crc);
    if (platform_seek(output_fd, 4, PLATFORM_SEEK_SET) != 4 || rt_write_all(output_fd, header + 4, 8) != 0) {
        platform_close(input_fd);
        platform_close(output_fd);
        rt_write_line(2, "bzip2: header update failed");
        return 1;
    }

    platform_close(input_fd);
    platform_close(output_fd);
    return 0;
}
