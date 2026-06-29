#include "archive_util.h"
#include "compression/bzip2.h"
#include "concurrency.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define BUNZIP2_PATH_CAPACITY 1024
#define BUNZIP2_IO_BUFFER 65536U
#define BUNZIP2_DEFAULT_MAX_WORKERS 8U

typedef struct {
    int fd;
    unsigned char buffer[BUNZIP2_IO_BUFFER];
    size_t offset;
    size_t available;
} Bunzip2Input;

static int bunzip2_input_fill(Bunzip2Input *input) {
    long amount = platform_read(input->fd, input->buffer, sizeof(input->buffer));

    if (amount < 0) return -1;
    input->offset = 0U;
    input->available = (size_t)amount;
    return 0;
}

static int bunzip2_input_read_exact(Bunzip2Input *input, unsigned char *out, size_t count) {
    size_t copied = 0U;

    while (copied < count) {
        size_t remaining;
        size_t chunk;

        if (input->offset == input->available && (bunzip2_input_fill(input) != 0 || input->available == 0U)) return -1;
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
    return input->available == 0U ? -1 : 0;
}

static int bunzip2_read_callback(void *context, unsigned char *buffer, size_t capacity, size_t *size_out) {
    Bunzip2Input *input = (Bunzip2Input *)context;
    long amount = platform_read(input->fd, buffer, capacity);

    if (amount < 0) return -1;
    *size_out = (size_t)amount;
    return 0;
}

static int bunzip2_write_callback(void *context, const unsigned char *data, size_t size) {
    ToolOutputBuffer *output = (ToolOutputBuffer *)context;

    return tool_output_buffer_write(output, (const char *)data, size);
}

static int bunzip2_decompress_real_parallel(const char *input_path, ToolOutputBuffer *output, unsigned int worker_count) {
    unsigned char *data = 0;
    size_t size = 0U;
    int result;

    if (worker_count < 2U) return 1;
    if (tool_read_all_input(input_path, &data, &size) != 0) return 1;
    result = compression_bzip2_decompress_buffer_parallel(data, size, bunzip2_write_callback, output, worker_count);
    rt_free(data);
    return result;
}

static int bunzip2_decompress_real(const char *input_path, int output_fd) {
    Bunzip2Input input;
    ToolOutputBuffer output;
    int result;
    unsigned int worker_count;

    tool_output_buffer_init(&output, output_fd);
    worker_count = tool_worker_count_from_env("NEWOS_BUNZIP2_WORKERS", BUNZIP2_DEFAULT_MAX_WORKERS);
    result = bunzip2_decompress_real_parallel(input_path, &output, worker_count);
    if (result == 1) {
        input.fd = platform_open_read(input_path);
        input.offset = 0U;
        input.available = 0U;
        if (input.fd < 0) return -1;
        result = compression_bzip2_decompress_stream(bunzip2_read_callback, &input, bunzip2_write_callback, &output);
        if (platform_close(input.fd) != 0) result = -1;
    }
    if (tool_output_buffer_flush(&output) != 0) result = -1;
    return result;
}

static int bunzip2_decompress_minimal(int input_fd, int output_fd) {
    unsigned char header[12];
    unsigned char buffer[BUNZIP2_IO_BUFFER];
    unsigned char run_buffer[BUNZIP2_IO_BUFFER];
    Bunzip2Input input;
    ToolOutputBuffer output;
    unsigned int expected_size;
    unsigned int expected_crc;
    unsigned int actual_crc = 0xffffffffU;
    unsigned int output_size = 0U;
    int run_fill_ready = 0;
    unsigned char run_fill_value = 0;

    input.fd = input_fd;
    input.offset = 0U;
    input.available = 0U;
    if (bunzip2_input_read_exact(&input, header, sizeof(header)) != 0 || header[0] != 'B' || header[1] != 'Z' || header[2] != 'h' || header[3] != '0') return -1;
    expected_size = archive_read_u32_le(header + 4);
    expected_crc = archive_read_u32_le(header + 8);
    tool_output_buffer_init(&output, output_fd);

    while (output_size < expected_size) {
        unsigned char flag;
        unsigned int count;

        if (bunzip2_input_read_exact(&input, &flag, 1U) != 0) return -1;
        count = (unsigned int)(flag & 0x7fU) + 1U;
        if (count > expected_size - output_size) return -1;
        if ((flag & 0x80U) != 0U) {
            unsigned char value;
            unsigned int remaining = count;

            if (bunzip2_input_read_exact(&input, &value, 1U) != 0) return -1;
            if (!run_fill_ready || run_fill_value != value) {
                rt_memset(run_buffer, value, sizeof(run_buffer));
                run_fill_value = value;
                run_fill_ready = 1;
            }
            while (remaining > 0U) {
                unsigned int chunk = remaining > sizeof(buffer) ? (unsigned int)sizeof(buffer) : remaining;
                if (tool_output_buffer_write(&output, (const char *)run_buffer, chunk) != 0) return -1;
                actual_crc = archive_crc32_update(actual_crc, run_buffer, chunk);
                output_size += chunk;
                remaining -= chunk;
            }
        } else {
            if (bunzip2_input_ensure(&input) != 0) return -1;
            if (count <= input.available - input.offset) {
                const unsigned char *literal = input.buffer + input.offset;
                if (tool_output_buffer_write(&output, (const char *)literal, count) != 0) return -1;
                actual_crc = archive_crc32_update(actual_crc, literal, count);
                input.offset += count;
            } else {
                if (bunzip2_input_read_exact(&input, buffer, count) != 0 || tool_output_buffer_write(&output, (const char *)buffer, count) != 0) return -1;
                actual_crc = archive_crc32_update(actual_crc, buffer, count);
            }
            output_size += count;
        }
    }
    if (tool_output_buffer_flush(&output) != 0) return -1;
    actual_crc = archive_crc32_finish(actual_crc);
    return actual_crc == expected_crc ? 0 : -1;
}

static int read_header4(const char *path, unsigned char header[4]) {
    int fd = platform_open_read(path);
    long amount;

    if (fd < 0) return -1;
    amount = platform_read(fd, header, 4U);
    if (platform_close(fd) != 0) return -1;
    return amount == 4 ? 0 : -1;
}

int main(int argc, char **argv) {
    char output_path[BUNZIP2_PATH_CAPACITY];
    unsigned char header[4];
    int output_fd;
    int result;

    if (argc != 2) {
        rt_write_line(2, "Usage: bunzip2 file.bz2");
        return 1;
    }
    if (tool_path_replace_suffix_or_append(argv[1], ".bz2", ".out", output_path, sizeof(output_path)) != 0) {
        rt_write_line(2, "bunzip2: output path too long");
        return 1;
    }
    if (read_header4(argv[1], header) != 0) {
        rt_write_line(2, "bunzip2: cannot read input");
        return 1;
    }
    output_fd = platform_open_write(output_path, 0644U);
    if (output_fd < 0) {
        rt_write_line(2, "bunzip2: cannot open output");
        return 1;
    }
    if (header[0] == 'B' && header[1] == 'Z' && header[2] == 'h' && header[3] == '0') {
        int input_fd = platform_open_read(argv[1]);
        if (input_fd < 0) {
            result = -1;
        } else {
            result = bunzip2_decompress_minimal(input_fd, output_fd);
            if (platform_close(input_fd) != 0) result = -1;
        }
    } else {
        result = bunzip2_decompress_real(argv[1], output_fd);
    }
    if (platform_close(output_fd) != 0) result = -1;
    if (result != 0) {
        rt_write_line(2, "bunzip2: decompression failed");
        return 1;
    }
    return 0;
}
