#include "archive_util.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define GUNZIP_BUFFER_SIZE 4096
#define GUNZIP_PATH_CAPACITY 1024

static int read_exact(int fd, unsigned char *buffer, size_t count) {
    size_t offset = 0;

    while (offset < count) {
        long bytes = platform_read(fd, buffer + offset, count - offset);
        if (bytes <= 0) {
            return -1;
        }
        offset += (size_t)bytes;
    }

    return 0;
}

static int skip_bytes(int fd, unsigned int count) {
    unsigned char buffer[256];

    while (count > 0U) {
        unsigned int chunk = (count > sizeof(buffer)) ? (unsigned int)sizeof(buffer) : count;
        if (read_exact(fd, buffer, chunk) != 0) {
            return -1;
        }
        count -= chunk;
    }

    return 0;
}

static int skip_cstring(int fd) {
    unsigned char ch;

    do {
        if (read_exact(fd, &ch, 1) != 0) {
            return -1;
        }
    } while (ch != 0);

    return 0;
}

static unsigned int read_u32_le(const unsigned char *bytes) {
    return (unsigned int)bytes[0] |
           ((unsigned int)bytes[1] << 8) |
           ((unsigned int)bytes[2] << 16) |
           ((unsigned int)bytes[3] << 24);
}

static int build_output_path(const char *input_path, char *buffer, size_t buffer_size) {
    size_t len = rt_strlen(input_path);

    if (len > 3 && input_path[len - 3] == '.' && input_path[len - 2] == 'g' && input_path[len - 1] == 'z') {
        if (len - 2 > buffer_size) {
            return -1;
        }
        memcpy(buffer, input_path, len - 3);
        buffer[len - 3] = '\0';
        return 0;
    }

    if (len + 5 >= buffer_size) {
        return -1;
    }

    memcpy(buffer, input_path, len);
    memcpy(buffer + len, ".out", 5);
    return 0;
}

static int is_dash_path(const char *path) {
    return path != 0 && path[0] == '-' && path[1] == '\0';
}

static int decompress_stream(int input_fd, int output_fd) {
    unsigned char header[10];
    unsigned char trailer[8];
    unsigned char chunk[GUNZIP_BUFFER_SIZE];
    unsigned int crc = 0xffffffffU;
    unsigned int output_size = 0;
    int done = 0;

    if (read_exact(input_fd, header, sizeof(header)) != 0 || header[0] != 0x1f || header[1] != 0x8b || header[2] != 0x08) {
        rt_write_line(2, "gunzip: invalid gzip header");
        return 1;
    }

    if ((header[3] & 0x04U) != 0U) {
        unsigned char extra_len[2];
        if (read_exact(input_fd, extra_len, 2) != 0 || skip_bytes(input_fd, (unsigned int)extra_len[0] | ((unsigned int)extra_len[1] << 8)) != 0) {
            return 1;
        }
    }
    if ((header[3] & 0x08U) != 0U && skip_cstring(input_fd) != 0) {
        return 1;
    }
    if ((header[3] & 0x10U) != 0U && skip_cstring(input_fd) != 0) {
        return 1;
    }
    if ((header[3] & 0x02U) != 0U && skip_bytes(input_fd, 2) != 0) {
        return 1;
    }

    while (!done) {
        unsigned char block_header;
        unsigned char lens[4];
        unsigned int len;
        unsigned int nlen;

        if (read_exact(input_fd, &block_header, 1) != 0) {
            return 1;
        }

        if ((block_header & 0x06U) != 0U) {
            rt_write_line(2, "gunzip: unsupported compressed block type");
            return 1;
        }

        done = (block_header & 0x01U) ? 1 : 0;

        if (read_exact(input_fd, lens, 4) != 0) {
            return 1;
        }

        len = (unsigned int)lens[0] | ((unsigned int)lens[1] << 8);
        nlen = (unsigned int)lens[2] | ((unsigned int)lens[3] << 8);
        if (((len ^ nlen) & 0xffffU) != 0xffffU) {
            rt_write_line(2, "gunzip: corrupt stored block");
            return 1;
        }

        while (len > 0U) {
            unsigned int chunk_size = (len > sizeof(chunk)) ? (unsigned int)sizeof(chunk) : len;
            if (read_exact(input_fd, chunk, chunk_size) != 0 || rt_write_all(output_fd, chunk, chunk_size) != 0) {
                return 1;
            }
            crc = archive_crc32_update(crc, chunk, chunk_size);
            output_size += chunk_size;
            len -= chunk_size;
        }
    }

    if (read_exact(input_fd, trailer, sizeof(trailer)) != 0) {
        return 1;
    }

    crc ^= 0xffffffffU;
    if (read_u32_le(trailer) != crc || read_u32_le(trailer + 4) != output_size) {
        rt_write_line(2, "gunzip: CRC or size check failed");
        return 1;
    }

    return 0;
}

static int process_path(const char *input_path, int to_stdout) {
    char output_path[GUNZIP_PATH_CAPACITY];
    int input_fd = -1;
    int output_fd = -1;
    int close_input = 0;
    int close_output = 0;
    int status;

    if (tool_open_input(input_path, &input_fd, &close_input) != 0) {
        rt_write_line(2, "gunzip: cannot open input");
        return 1;
    }

    if (to_stdout || input_path == 0 || is_dash_path(input_path)) {
        output_fd = 1;
    } else {
        if (build_output_path(input_path, output_path, sizeof(output_path)) != 0) {
            tool_close_input(input_fd, close_input);
            rt_write_line(2, "gunzip: output path too long");
            return 1;
        }
        output_fd = platform_open_write(output_path, 0644U);
        if (output_fd < 0) {
            tool_close_input(input_fd, close_input);
            rt_write_line(2, "gunzip: cannot open output");
            return 1;
        }
        close_output = 1;
    }

    status = decompress_stream(input_fd, output_fd);
    tool_close_input(input_fd, close_input);
    if (close_output) {
        platform_close(output_fd);
    }
    return status;
}

int main(int argc, char **argv) {
    int to_stdout = 0;
    int processed = 0;
    int status = 0;
    int i;

    for (i = 1; i < argc; ++i) {
        if (rt_strcmp(argv[i], "--help") == 0) {
            tool_write_usage(tool_base_name(argv[0]), "[-c] [file.gz ...]");
            return 0;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            size_t j = 1;
            while (argv[i][j] != '\0') {
                if (argv[i][j] == 'c') {
                    to_stdout = 1;
                } else if (argv[i][j] != 'f' && argv[i][j] != 'k' && argv[i][j] != 'd') {
                    tool_write_error("gunzip", "unsupported option ", argv[i]);
                    return 1;
                }
                j += 1U;
            }
        }
    }

    for (i = 1; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            continue;
        }
        processed = 1;
        if (process_path(argv[i], to_stdout) != 0) {
            status = 1;
        }
    }

    if (!processed) {
        return process_path("-", 1);
    }

    return status;
}
