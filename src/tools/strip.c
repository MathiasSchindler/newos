#include "platform.h"
#include "runtime.h"
#include "tool_util.h"
#include "archive_util.h"

#define STRIP_BUFFER_SIZE 4096U

static unsigned short read_u16_le(const unsigned char *bytes) {
    return (unsigned short)bytes[0] | (unsigned short)((unsigned short)bytes[1] << 8);
}

static unsigned long long read_u64_le_local(const unsigned char *bytes) {
    return archive_read_u64_le(bytes);
}

static void write_u16_le_local(unsigned char *bytes, unsigned short value) {
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8) & 0xffU);
}

static void write_u64_le_local(unsigned char *bytes, unsigned long long value) {
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8) & 0xffU);
    bytes[2] = (unsigned char)((value >> 16) & 0xffU);
    bytes[3] = (unsigned char)((value >> 24) & 0xffU);
    bytes[4] = (unsigned char)((value >> 32) & 0xffU);
    bytes[5] = (unsigned char)((value >> 40) & 0xffU);
    bytes[6] = (unsigned char)((value >> 48) & 0xffU);
    bytes[7] = (unsigned char)((value >> 56) & 0xffU);
}

static void build_temp_prefix(const char *target_path, const char *stem, char *buffer, size_t buffer_size) {
    size_t slash = 0U;
    size_t i = 0U;
    size_t prefix_length;

    while (target_path != 0 && target_path[i] != '\0') {
        if (target_path[i] == '/') {
            slash = i + 1U;
        }
        i += 1U;
    }

    if (slash == 0U) {
        rt_copy_string(buffer, buffer_size, "./");
        prefix_length = rt_strlen(buffer);
    } else {
        prefix_length = slash < (buffer_size - 1U) ? slash : (buffer_size - 1U);
        memcpy(buffer, target_path, prefix_length);
        buffer[prefix_length] = '\0';
    }

    rt_copy_string(buffer + prefix_length, buffer_size - prefix_length, stem);
}

static int copy_prefix(int input_fd, int output_fd, unsigned long long count) {
    unsigned char buffer[STRIP_BUFFER_SIZE];

    if (platform_seek(input_fd, 0, PLATFORM_SEEK_SET) < 0) {
        return -1;
    }

    while (count > 0ULL) {
        size_t chunk = count > sizeof(buffer) ? sizeof(buffer) : (size_t)count;
        long bytes_read = platform_read(input_fd, buffer, chunk);
        if (bytes_read <= 0) {
            return -1;
        }
        if (rt_write_all(output_fd, buffer, (size_t)bytes_read) != 0) {
            return -1;
        }
        count -= (unsigned long long)bytes_read;
    }
    return 0;
}

static int strip_one_file(const char *input_path, const char *output_path, int inplace) {
    PlatformDirEntry entry;
    unsigned char header[64];
    unsigned short elf_type;
    unsigned short phentsize;
    unsigned short phnum;
    unsigned long long phoff;
    unsigned long long file_size;
    unsigned long long copy_size = 0ULL;
    int is_elf = 0;
    int input_fd = -1;
    int output_fd = -1;
    char temp_path[1024];
    char temp_prefix[1024];
    unsigned short i;

    if (platform_get_path_info(input_path, &entry) != 0 || entry.is_dir) {
        rt_write_cstr(2, "strip: cannot access ");
        rt_write_line(2, input_path);
        return 1;
    }

    input_fd = platform_open_read(input_path);
    if (input_fd < 0) {
        rt_write_cstr(2, "strip: cannot open ");
        rt_write_line(2, input_path);
        return 1;
    }

    if (archive_read_exact(input_fd, header, sizeof(header)) != 0) {
        rt_write_cstr(2, "strip: cannot read ");
        rt_write_line(2, input_path);
        platform_close(input_fd);
        return 1;
    }

    if (header[0] == 0x7fU && header[1] == 'E' && header[2] == 'L' && header[3] == 'F' &&
        header[4] == 2U && header[5] == 1U) {
        is_elf = 1;
    }

    if (is_elf) {
        elf_type = read_u16_le(header + 16);
        if (elf_type == 1U) {
            rt_write_cstr(2, "strip: relocatable objects are not yet supported: ");
            rt_write_line(2, input_path);
            platform_close(input_fd);
            return 1;
        }

        phoff = read_u64_le_local(header + 32);
        phentsize = read_u16_le(header + 54);
        phnum = read_u16_le(header + 56);
    }
    file_size = entry.size;
    copy_size = file_size;

    if (is_elf && phoff > 0ULL && phnum > 0U && phentsize >= 56U) {
        copy_size = 0ULL;
        for (i = 0U; i < phnum; ++i) {
            unsigned char phdr[56];
            unsigned long long ph_offset;
            unsigned long long ph_filesz;
            unsigned long long end_offset;
            if (platform_seek(input_fd, (long long)(phoff + ((unsigned long long)i * (unsigned long long)phentsize)), PLATFORM_SEEK_SET) < 0 ||
                archive_read_exact(input_fd, phdr, sizeof(phdr)) != 0) {
                platform_close(input_fd);
                return 1;
            }
            ph_offset = read_u64_le_local(phdr + 8);
            ph_filesz = read_u64_le_local(phdr + 32);
            end_offset = ph_offset + ph_filesz;
            if (end_offset > copy_size) {
                copy_size = end_offset;
            }
        }
        if (copy_size == 0ULL || copy_size > file_size) {
            copy_size = file_size;
        }
    }

    if (inplace) {
        build_temp_prefix(input_path, ".newos-strip-", temp_prefix, sizeof(temp_prefix));
        output_fd = platform_create_temp_file(temp_path, sizeof(temp_path), temp_prefix, (entry.mode & 0777U) != 0U ? (entry.mode & 0777U) : 0644U);
    } else {
        output_fd = platform_open_write(output_path, (entry.mode & 0777U) != 0U ? (entry.mode & 0777U) : 0644U);
        temp_path[0] = '\0';
    }

    if (output_fd < 0) {
        rt_write_cstr(2, "strip: cannot create output for ");
        rt_write_line(2, input_path);
        platform_close(input_fd);
        return 1;
    }

    if (copy_prefix(input_fd, output_fd, copy_size) != 0) {
        rt_write_cstr(2, "strip: failed while copying ");
        rt_write_line(2, input_path);
        platform_close(input_fd);
        platform_close(output_fd);
        if (inplace) {
            (void)platform_remove_file(temp_path);
        }
        return 1;
    }

    if (is_elf) {
        write_u64_le_local(header + 40, 0ULL);
        write_u16_le_local(header + 58, 0U);
        write_u16_le_local(header + 60, 0U);
        write_u16_le_local(header + 62, 0U);

        if (platform_seek(output_fd, 0, PLATFORM_SEEK_SET) < 0 || rt_write_all(output_fd, header, sizeof(header)) != 0) {
            rt_write_cstr(2, "strip: failed to patch ELF header for ");
            rt_write_line(2, input_path);
            platform_close(input_fd);
            platform_close(output_fd);
            if (inplace) {
                (void)platform_remove_file(temp_path);
            }
            return 1;
        }
    }

    platform_close(input_fd);
    platform_close(output_fd);

    if (inplace) {
        if (platform_rename_path(temp_path, input_path) != 0) {
            rt_write_cstr(2, "strip: failed to replace ");
            rt_write_line(2, input_path);
            (void)platform_remove_file(temp_path);
            return 1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    const char *output_path = 0;
    int argi = 1;
    int exit_code = 0;
    int i;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-o") == 0) {
            if (argi + 1 >= argc) {
                tool_write_usage("strip", "[-o output] file ...");
                return 1;
            }
            output_path = argv[argi + 1];
            argi += 2;
        } else {
            tool_write_usage("strip", "[-o output] file ...");
            return 1;
        }
    }

    if (argi >= argc) {
        tool_write_usage("strip", "[-o output] file ...");
        return 1;
    }

    if (output_path != 0 && (argc - argi) != 1) {
        rt_write_line(2, "strip: -o requires exactly one input file");
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        if (strip_one_file(argv[i], output_path != 0 ? output_path : argv[i], output_path == 0) != 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
