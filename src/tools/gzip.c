#include "archive_util.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

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

static int contains_slash(const char *text) {
    size_t i = 0;
    while (text[i] != '\0') {
        if (text[i] == '/') {
            return 1;
        }
        i += 1U;
    }
    return 0;
}

static int is_dash_path(const char *path) {
    return path != 0 && path[0] == '-' && path[1] == '\0';
}

static void get_program_dir(const char *argv0, char *buffer, size_t buffer_size) {
    size_t len;
    size_t i;

    if (argv0 == 0 || argv0[0] == '\0' || !contains_slash(argv0)) {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    len = rt_strlen(argv0);
    if (len + 1U > buffer_size) {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    memcpy(buffer, argv0, len + 1U);
    for (i = len; i > 0; --i) {
        if (buffer[i - 1] == '/') {
            if (i == 1U) {
                buffer[1] = '\0';
            } else {
                buffer[i - 1] = '\0';
            }
            return;
        }
    }

    rt_copy_string(buffer, buffer_size, ".");
}

static int build_helper_path(const char *argv0, const char *tool_name, char *buffer, size_t buffer_size) {
    char dir[GZIP_PATH_CAPACITY];

    if (argv0 == 0 || !contains_slash(argv0)) {
        rt_copy_string(buffer, buffer_size, tool_name);
        return 0;
    }

    get_program_dir(argv0, dir, sizeof(dir));
    return tool_join_path(dir, tool_name, buffer, buffer_size);
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

static int compress_stream(int input_fd, int output_fd) {
    unsigned char current[GZIP_BLOCK_SIZE];
    unsigned char next[GZIP_BLOCK_SIZE];
    const unsigned char header[10] = { 0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 };
    long current_size;
    unsigned int crc = 0xffffffffU;
    unsigned int input_size = 0;

    if (rt_write_all(output_fd, header, sizeof(header)) != 0) {
        return -1;
    }

    current_size = platform_read(input_fd, current, sizeof(current));
    if (current_size < 0) {
        return -1;
    }

    if (current_size == 0) {
        if (write_stored_block(output_fd, current, 0, 1) != 0) {
            return -1;
        }
    } else {
        for (;;) {
            long next_size = platform_read(input_fd, next, sizeof(next));
            int is_last;

            if (next_size < 0) {
                return -1;
            }

            is_last = (next_size == 0);
            crc = archive_crc32_update(crc, current, (size_t)current_size);
            input_size += (unsigned int)current_size;

            if (write_stored_block(output_fd, current, (unsigned int)current_size, is_last) != 0) {
                return -1;
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
        return -1;
    }

    return 0;
}

static int process_path(const char *input_path, int to_stdout, int force_overwrite, int keep_input) {
    char output_path[GZIP_PATH_CAPACITY];
    int input_fd = -1;
    int output_fd = -1;
    int close_input = 0;
    int close_output = 0;
    int have_output_path = 0;
    int status = 1;

    if (tool_open_input(input_path, &input_fd, &close_input) != 0) {
        rt_write_line(2, "gzip: cannot open input");
        return 1;
    }

    if (to_stdout || input_path == 0 || is_dash_path(input_path)) {
        output_fd = 1;
    } else {
        if (build_output_path(input_path, output_path, sizeof(output_path)) != 0) {
            tool_close_input(input_fd, close_input);
            rt_write_line(2, "gzip: output path too long");
            return 1;
        }
        if (!force_overwrite && tool_path_exists(output_path)) {
            tool_close_input(input_fd, close_input);
            tool_write_error("gzip", "output already exists (use -f): ", output_path);
            return 1;
        }
        output_fd = platform_open_write(output_path, 0644U);
        if (output_fd < 0) {
            tool_close_input(input_fd, close_input);
            rt_write_line(2, "gzip: cannot open output");
            return 1;
        }
        have_output_path = 1;
        close_output = 1;
    }

    status = (compress_stream(input_fd, output_fd) == 0) ? 0 : 1;
    tool_close_input(input_fd, close_input);
    if (close_output) {
        platform_close(output_fd);
    }
    if (status != 0 && have_output_path) {
        (void)platform_remove_file(output_path);
    } else if (status == 0 && have_output_path && !keep_input && input_path != 0 && !is_dash_path(input_path)) {
        (void)platform_remove_file(input_path);
    }
    return status;
}

static int dispatch_to_gunzip(const char *argv0, int argc, char **argv) {
    char helper_path[GZIP_PATH_CAPACITY];
    char cleaned_storage[16][16];
    char *helper_argv[64];
    size_t out = 0;
    size_t cleaned_count = 0;
    int i;
    int pid = 0;
    int status = 1;

    if (build_helper_path(argv0, "gunzip", helper_path, sizeof(helper_path)) != 0) {
        tool_write_error("gzip", "cannot locate ", "gunzip");
        return 1;
    }

    helper_argv[out++] = helper_path;
    for (i = 1; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            size_t in_pos = 1;
            size_t used = 1;

            if (cleaned_count >= sizeof(cleaned_storage) / sizeof(cleaned_storage[0])) {
                tool_write_error("gzip", "too many option groups", 0);
                return 1;
            }
            cleaned_storage[cleaned_count][0] = '-';
            while (argv[i][in_pos] != '\0' && used + 1U < sizeof(cleaned_storage[cleaned_count])) {
                if (argv[i][in_pos] != 'd') {
                    cleaned_storage[cleaned_count][used++] = argv[i][in_pos];
                }
                in_pos += 1U;
            }
            cleaned_storage[cleaned_count][used] = '\0';
            if (used > 1U) {
                helper_argv[out++] = cleaned_storage[cleaned_count++];
            }
        } else {
            helper_argv[out++] = argv[i];
        }
    }
    helper_argv[out] = 0;

    if (platform_spawn_process(helper_argv, 0, 1, 0, 0, 0, &pid) != 0 ||
        platform_wait_process(pid, &status) != 0) {
        tool_write_error("gzip", "cannot run ", "gunzip");
        return 1;
    }

    return status == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    int to_stdout = 0;
    int decompress_mode = 0;
    int force_overwrite = 0;
    int keep_input = 0;
    int processed = 0;
    int status = 0;
    int i;

    for (i = 1; i < argc; ++i) {
        if (rt_strcmp(argv[i], "--help") == 0) {
            tool_write_usage(tool_base_name(argv[0]), "[-c] [-d] [-f] [-k] [file ...]");
            return 0;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            size_t j = 1;
            while (argv[i][j] != '\0') {
                if (argv[i][j] == 'c') {
                    to_stdout = 1;
                } else if (argv[i][j] == 'd') {
                    decompress_mode = 1;
                } else if (argv[i][j] == 'f') {
                    force_overwrite = 1;
                } else if (argv[i][j] == 'k') {
                    keep_input = 1;
                } else if (argv[i][j] < '1' || argv[i][j] > '9') {
                    tool_write_error("gzip", "unsupported option ", argv[i]);
                    return 1;
                }
                j += 1U;
            }
        }
    }

    if (decompress_mode) {
        return dispatch_to_gunzip(argv[0], argc, argv);
    }

    for (i = 1; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            continue;
        }
        processed = 1;
        if (process_path(argv[i], to_stdout, force_overwrite, keep_input) != 0) {
            status = 1;
        }
    }

    if (!processed) {
        return process_path("-", 1, force_overwrite, keep_input);
    }

    return status;
}
