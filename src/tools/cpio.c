#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define CPIO_HEADER_SIZE 110U
#define CPIO_NAME_CAPACITY 1024U
#define CPIO_BUFFER_SIZE 4096U

static const char *archive_path = "-";
static int cpio_json;

static void print_usage(void) {
    tool_write_usage("cpio", "-t|-i|-o [-F ARCHIVE] [--json] [FILE ...]");
}

static int write_json_entry(const char *name, unsigned long long size, unsigned long long mode) {
    if (tool_json_begin_event(1, "cpio", "stdout", "entry") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{\"name\":") != 0) return -1;
    if (tool_json_write_string(1, name) != 0) return -1;
    if (rt_write_cstr(1, ",\"size\":") != 0) return -1;
    if (rt_write_uint(1, size) != 0) return -1;
    if (rt_write_cstr(1, ",\"mode\":") != 0) return -1;
    if (rt_write_uint(1, mode) != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static unsigned int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return (unsigned int)(ch - '0');
    if (ch >= 'a' && ch <= 'f') return (unsigned int)(ch - 'a' + 10);
    if (ch >= 'A' && ch <= 'F') return (unsigned int)(ch - 'A' + 10);
    return 0U;
}

static unsigned long long parse_hex8(const char *text) {
    unsigned long long value = 0ULL;
    int i;
    for (i = 0; i < 8; ++i) value = (value << 4U) | (unsigned long long)hex_digit(text[i]);
    return value;
}

static void write_hex8(char *out, unsigned long long value) {
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 7; i >= 0; --i) {
        out[i] = digits[value & 0xfULL];
        value >>= 4U;
    }
}

static unsigned long long pad4(unsigned long long value) {
    return (4ULL - (value & 3ULL)) & 3ULL;
}

static int read_exact(int fd, void *buffer, size_t size) {
    size_t used = 0U;
    while (used < size) {
        long bytes = platform_read(fd, (char *)buffer + used, size - used);
        if (bytes <= 0) return -1;
        used += (size_t)bytes;
    }
    return 0;
}

static int write_zero_pad(int fd, unsigned long long count) {
    static const char zeros[4] = {0, 0, 0, 0};
    return count == 0ULL ? 0 : rt_write_all(fd, zeros, (size_t)count);
}

static int skip_bytes(int fd, unsigned long long count) {
    char buffer[CPIO_BUFFER_SIZE];
    while (count > 0ULL) {
        size_t chunk = count > sizeof(buffer) ? sizeof(buffer) : (size_t)count;
        if (read_exact(fd, buffer, chunk) != 0) return -1;
        count -= (unsigned long long)chunk;
    }
    return 0;
}

static int copy_bytes(int in_fd, int out_fd, unsigned long long count) {
    char buffer[CPIO_BUFFER_SIZE];
    while (count > 0ULL) {
        size_t chunk = count > sizeof(buffer) ? sizeof(buffer) : (size_t)count;
        if (read_exact(in_fd, buffer, chunk) != 0) return -1;
        if (rt_write_all(out_fd, buffer, chunk) != 0) return -1;
        count -= (unsigned long long)chunk;
    }
    return 0;
}

static void fill_header(char *header, unsigned long long mode, unsigned long long size, unsigned long long namesize) {
    int i;
    for (i = 0; i < (int)CPIO_HEADER_SIZE; ++i) header[i] = '0';
    header[0] = '0'; header[1] = '7'; header[2] = '0'; header[3] = '7'; header[4] = '0'; header[5] = '1';
    write_hex8(header + 14, mode);
    write_hex8(header + 54, size);
    write_hex8(header + 94, namesize);
}

static int write_entry(int out_fd, const char *path) {
    PlatformDirEntry info;
    int in_fd = -1;
    char header[CPIO_HEADER_SIZE];
    unsigned long long mode;
    unsigned long long size;
    unsigned long long namesize = rt_strlen(path) + 1ULL;

    if (platform_get_path_info(path, &info) != 0) {
        tool_write_error("cpio", "cannot stat ", path);
        return -1;
    }
    mode = (unsigned long long)info.mode;
    size = info.is_dir ? 0ULL : info.size;
    fill_header(header, mode, size, namesize);
    if (rt_write_all(out_fd, header, sizeof(header)) != 0 ||
        rt_write_all(out_fd, path, (size_t)namesize) != 0 ||
        write_zero_pad(out_fd, pad4(CPIO_HEADER_SIZE + namesize)) != 0) {
        return -1;
    }
    if (!info.is_dir) {
        in_fd = platform_open_read(path);
        if (in_fd < 0) return -1;
        if (copy_bytes(in_fd, out_fd, size) != 0) {
            platform_close(in_fd);
            return -1;
        }
        platform_close(in_fd);
        if (write_zero_pad(out_fd, pad4(size)) != 0) return -1;
    }
    return 0;
}

static int write_trailer(int out_fd) {
    char header[CPIO_HEADER_SIZE];
    const char *name = "TRAILER!!!";
    unsigned long long namesize = rt_strlen(name) + 1ULL;
    fill_header(header, 0ULL, 0ULL, namesize);
    return rt_write_all(out_fd, header, sizeof(header)) != 0 ||
           rt_write_all(out_fd, name, (size_t)namesize) != 0 ||
           write_zero_pad(out_fd, pad4(CPIO_HEADER_SIZE + namesize)) != 0 ? -1 : 0;
}

static int create_archive(int argc, char **argv, int argi) {
    int fd = 1;
    int status = 0;

    if (rt_strcmp(archive_path, "-") != 0) {
        fd = platform_open_write(archive_path, 0644U);
        if (fd < 0) return 1;
    }
    for (; argi < argc; ++argi) {
        if (write_entry(fd, argv[argi]) != 0) status = 1;
    }
    if (write_trailer(fd) != 0) status = 1;
    if (fd != 1) platform_close(fd);
    return status;
}

static int read_archive(int extract) {
    int fd;
    int should_close;
    int status = 0;

    if (tool_open_input(archive_path, &fd, &should_close) != 0) return 1;
    for (;;) {
        char header[CPIO_HEADER_SIZE];
        char name[CPIO_NAME_CAPACITY];
        unsigned long long mode, size, namesize;
        int is_dir;

        if (read_exact(fd, header, sizeof(header)) != 0) break;
        if (header[0] != '0' || header[1] != '7' || header[2] != '0' || header[3] != '7' || header[4] != '0' || header[5] != '1') {
            status = 1;
            break;
        }
        mode = parse_hex8(header + 14);
        size = parse_hex8(header + 54);
        namesize = parse_hex8(header + 94);
        if (namesize == 0ULL || namesize >= sizeof(name)) { status = 1; break; }
        if (read_exact(fd, name, (size_t)namesize) != 0) { status = 1; break; }
        name[namesize - 1ULL] = '\0';
        if (skip_bytes(fd, pad4(CPIO_HEADER_SIZE + namesize)) != 0) { status = 1; break; }
        if (rt_strcmp(name, "TRAILER!!!") == 0) break;
        is_dir = ((mode & 0170000ULL) == 0040000ULL);
        if (!extract) {
            if (cpio_json) {
                if (write_json_entry(name, size, mode) != 0) { status = 1; break; }
            } else {
                rt_write_line(1, name);
            }
            if (skip_bytes(fd, size + pad4(size)) != 0) { status = 1; break; }
            continue;
        }
        if (is_dir) {
            (void)platform_make_directory(name, (unsigned int)(mode & 0777ULL));
        } else {
            int out_fd = platform_open_write(name, (unsigned int)(mode & 0777ULL));
            if (out_fd < 0 || copy_bytes(fd, out_fd, size) != 0) {
                status = 1;
                if (out_fd >= 0) platform_close(out_fd);
                break;
            }
            platform_close(out_fd);
            if (skip_bytes(fd, pad4(size)) != 0) { status = 1; break; }
        }
    }
    tool_close_input(fd, should_close);
    return status;
}

int main(int argc, char **argv) {
    int argi = 1;
    int mode = 0;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "-t") == 0) mode = 't';
        else if (rt_strcmp(argv[argi], "-i") == 0) mode = 'i';
        else if (rt_strcmp(argv[argi], "-o") == 0) mode = 'o';
        else if (rt_strcmp(argv[argi], "-F") == 0 && argi + 1 < argc) archive_path = argv[++argi];
        else if (rt_strcmp(argv[argi], "--json") == 0) { cpio_json = 1; tool_json_set_enabled(1); }
        else if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--help") == 0) { print_usage(); return 0; }
        else break;
        argi++;
    }
    if (mode == 0) {
        print_usage();
        return 1;
    }
    if (mode == 'o') {
        if (argi >= argc) { print_usage(); return 1; }
        return create_archive(argc, argv, argi);
    }
    return read_archive(mode == 'i');
}
