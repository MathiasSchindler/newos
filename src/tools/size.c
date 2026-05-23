#include "platform.h"
#include "runtime.h"
#include "tool_util.h"
#include "archive_util.h"

#define SIZE_MAX_SECTIONS 512U

#define ELF_SHT_PROGBITS 1U
#define ELF_SHT_NOBITS 8U
#define ELF_SHF_WRITE 1ULL
#define ELF_SHF_ALLOC 2ULL
#define ELF_SHF_EXECINSTR 4ULL

typedef struct {
    unsigned int type;
    unsigned long long flags;
    unsigned long long offset;
    unsigned long long size;
} SizeSection;

static unsigned short read_u16_le(const unsigned char *bytes) {
    return (unsigned short)bytes[0] | (unsigned short)((unsigned short)bytes[1] << 8U);
}

static unsigned int read_u32_le(const unsigned char *bytes) {
    return archive_read_u32_le(bytes);
}

static unsigned long long read_u64_le(const unsigned char *bytes) {
    return archive_read_u64_le(bytes);
}

static int read_region(int fd, unsigned long long offset, unsigned char *buffer, size_t size) {
    if (platform_seek(fd, (long long)offset, PLATFORM_SEEK_SET) < 0) {
        return -1;
    }
    return archive_read_exact(fd, buffer, size);
}

static void write_hex(unsigned long long value) {
    const char *digits = "0123456789abcdef";
    char temp[32];
    size_t count = 0U;

    rt_write_cstr(1, "0x");
    if (value == 0ULL) {
        rt_write_char(1, '0');
        return;
    }
    while (value != 0ULL && count < sizeof(temp)) {
        temp[count++] = digits[value & 0xfULL];
        value >>= 4U;
    }
    while (count > 0U) {
        rt_write_char(1, temp[--count]);
    }
}

static int size_file(const char *path, int print_name) {
    int fd = platform_open_read(path);
    unsigned char header[64];
    SizeSection sections[SIZE_MAX_SECTIONS];
    unsigned long long text = 0ULL;
    unsigned long long data = 0ULL;
    unsigned long long bss = 0ULL;
    unsigned long long file_size = 0ULL;
    unsigned long long shoff;
    unsigned short shentsize;
    unsigned short shnum;
    unsigned short i;

    if (fd < 0) {
        tool_write_error("size", "cannot open ", path);
        return 1;
    }
    {
        long long end_offset = platform_seek(fd, 0, PLATFORM_SEEK_END);
        file_size = end_offset < 0 ? 0ULL : (unsigned long long)end_offset;
    }
    if (read_region(fd, 0ULL, header, sizeof(header)) != 0 ||
        header[0] != 0x7fU || header[1] != 'E' || header[2] != 'L' || header[3] != 'F' ||
        header[4] != 2U || header[5] != 1U) {
        platform_close(fd);
        tool_write_error("size", "unsupported file: ", path);
        return 1;
    }
    shoff = read_u64_le(header + 40);
    shentsize = read_u16_le(header + 58);
    shnum = read_u16_le(header + 60);
    if (shnum > SIZE_MAX_SECTIONS || shentsize < 64U) {
        platform_close(fd);
        tool_write_error("size", "invalid section table: ", path);
        return 1;
    }
    for (i = 0U; i < shnum; ++i) {
        unsigned char raw[64];
        if (read_region(fd, shoff + ((unsigned long long)i * shentsize), raw, sizeof(raw)) != 0) {
            platform_close(fd);
            return 1;
        }
        sections[i].type = read_u32_le(raw + 4);
        sections[i].flags = read_u64_le(raw + 8);
        sections[i].offset = read_u64_le(raw + 24);
        sections[i].size = read_u64_le(raw + 32);
        if ((sections[i].flags & ELF_SHF_ALLOC) == 0ULL) {
            continue;
        }
        if (sections[i].type == ELF_SHT_NOBITS) {
            bss += sections[i].size;
        } else if ((sections[i].flags & ELF_SHF_EXECINSTR) != 0ULL) {
            text += sections[i].size;
        } else if ((sections[i].flags & ELF_SHF_WRITE) != 0ULL) {
            data += sections[i].size;
        } else if (sections[i].type == ELF_SHT_PROGBITS) {
            text += sections[i].size;
        }
    }
    platform_close(fd);
    rt_write_uint(1, text);
    rt_write_char(1, '\t');
    rt_write_uint(1, data);
    rt_write_char(1, '\t');
    rt_write_uint(1, bss);
    rt_write_char(1, '\t');
    rt_write_uint(1, text + data + bss);
    rt_write_char(1, '\t');
    write_hex(text + data + bss);
    rt_write_char(1, '\t');
    rt_write_uint(1, file_size);
    if (print_name) {
        rt_write_char(1, '\t');
        rt_write_cstr(1, path);
    }
    rt_write_char(1, '\n');
    return 0;
}

static void print_usage(void) {
    tool_write_usage("size", "FILE ...");
}

int main(int argc, char **argv) {
    int i;
    int status = 0;

    if (argc < 2 || rt_strcmp(argv[1], "--help") == 0 || rt_strcmp(argv[1], "-h") == 0) {
        print_usage();
        return argc < 2 ? 1 : 0;
    }
    rt_write_line(1, "text\tdata\tbss\tdec\thex\tfile\tname");
    for (i = 1; i < argc; ++i) {
        if (size_file(argv[i], 1) != 0) {
            status = 1;
        }
    }
    return status;
}
