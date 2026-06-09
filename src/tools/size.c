#include "platform.h"
#include "runtime.h"
#include "tool_util.h"
#include "archive_util.h"

#define SIZE_MAX_SECTIONS 512U
#define SIZE_MAX_MACHO_COMMANDS 256U

#define ELF_SHT_PROGBITS 1U
#define ELF_SHT_NOBITS 8U
#define ELF_SHF_WRITE 1ULL
#define ELF_SHF_ALLOC 2ULL
#define ELF_SHF_EXECINSTR 4ULL

#define MACHO_MAGIC_64_LE 0xfeedfacfU
#define MACHO_FAT_MAGIC_LE 0xbebafecaU
#define MACHO_FAT_MAGIC_64_LE 0xbfbafecaU
#define MACHO_CPU_TYPE_ARM64 0x0100000cU
#define MACHO_LC_SEGMENT_64 0x19U
#define MACHO_S_ZEROFILL 1U
#define MACHO_S_GB_ZEROFILL 12U
#define MACHO_S_THREAD_LOCAL_ZEROFILL 18U
#define MACHO_VM_PROT_WRITE 2U
#define MACHO_VM_PROT_EXECUTE 4U

static int size_json;
static int size_segments;
static unsigned long long size_object_base;

typedef struct {
    unsigned int type;
    unsigned long long flags;
    unsigned long long offset;
    unsigned long long size;
} SizeSection;

#define read_u16_le tool_read_u16_le
#define read_u32_le tool_read_u32_le
#define read_u32_be tool_read_u32_be
#define read_u64_le tool_read_u64_le

#define read_region(fd, offset, buffer, count) archive_read_region((fd), size_object_base, (offset), (buffer), (count))
#define read_file_region archive_read_file_region

static int select_macho_fat_slice(int fd, unsigned long long *offset_out, unsigned long long *size_out) {
    unsigned char header[8];
    unsigned int magic;
    unsigned int arch_count;
    unsigned int index;
    *offset_out = 0ULL;
    *size_out = 0ULL;
    if (read_file_region(fd, 0ULL, header, sizeof(header)) != 0) return -1;
    magic = read_u32_le(header + 0);
    if (magic != MACHO_FAT_MAGIC_LE && magic != MACHO_FAT_MAGIC_64_LE) return -1;
    arch_count = read_u32_be(header + 4);
    if (arch_count > 32U) return -1;
    for (index = 0U; index < arch_count; ++index) {
        unsigned char raw[32];
        unsigned int entry_size = magic == MACHO_FAT_MAGIC_64_LE ? 32U : 20U;
        unsigned long long entry = 8ULL + (unsigned long long)index * (unsigned long long)entry_size;
        unsigned int cputype;
        if (read_file_region(fd, entry, raw, entry_size) != 0) return -1;
        cputype = read_u32_be(raw + 0);
        if (cputype == MACHO_CPU_TYPE_ARM64 || index == 0U) {
            *offset_out = magic == MACHO_FAT_MAGIC_64_LE ? (((unsigned long long)read_u32_be(raw + 8) << 32U) | (unsigned long long)read_u32_be(raw + 12)) : (unsigned long long)read_u32_be(raw + 8);
            *size_out = magic == MACHO_FAT_MAGIC_64_LE ? (((unsigned long long)read_u32_be(raw + 16) << 32U) | (unsigned long long)read_u32_be(raw + 20)) : (unsigned long long)read_u32_be(raw + 12);
            if (cputype == MACHO_CPU_TYPE_ARM64) return 0;
        }
    }
    return *offset_out != 0ULL ? 0 : -1;
}

#define copy_fixed_name tool_copy_printable_bytes

static int macho_is_zerofill_type(unsigned int type) {
    return type == MACHO_S_ZEROFILL || type == MACHO_S_GB_ZEROFILL || type == MACHO_S_THREAD_LOCAL_ZEROFILL;
}

static int name_starts_with(const char *name, const char *prefix) {
    size_t i;

    for (i = 0U; prefix[i] != '\0'; ++i) {
        if (name[i] != prefix[i]) return 0;
    }
    return 1;
}

static int add_macho_section_size(const char *segment,
                                  const char *section,
                                  unsigned int initprot,
                                  unsigned int flags,
                                  unsigned long long size,
                                  unsigned long long *text,
                                  unsigned long long *data,
                                  unsigned long long *bss) {
    unsigned int type = flags & 0xffU;

    if (size == 0ULL) return 0;
    if (macho_is_zerofill_type(type) || rt_strcmp(section, "__bss") == 0 || rt_strcmp(section, "__common") == 0) {
        *bss += size;
    } else if (name_starts_with(segment, "__DATA") || rt_strcmp(section, "__data") == 0) {
        *data += size;
    } else if ((initprot & MACHO_VM_PROT_EXECUTE) != 0U || rt_strcmp(segment, "__TEXT") == 0) {
        *text += size;
    } else if ((initprot & MACHO_VM_PROT_WRITE) != 0U) {
        *data += size;
    } else {
        *text += size;
    }
    return 0;
}

static int size_macho64_file(int fd,
                             const unsigned char *header,
                             unsigned long long file_size,
                             unsigned long long *text,
                             unsigned long long *data,
                             unsigned long long *bss) {
    unsigned int ncmds = read_u32_le(header + 16);
    unsigned int sizeofcmds = read_u32_le(header + 20);
    unsigned int command_index;
    unsigned long long command_offset = 32ULL;

    *text = 0ULL;
    *data = 0ULL;
    *bss = 0ULL;

    if (ncmds > SIZE_MAX_MACHO_COMMANDS || sizeofcmds > file_size || 32ULL + (unsigned long long)sizeofcmds > file_size) {
        return -1;
    }

    for (command_index = 0U; command_index < ncmds; ++command_index) {
        unsigned char command_header[8];
        unsigned int command;
        unsigned int command_size;

        if (command_offset + 8ULL > file_size || read_region(fd, command_offset, command_header, sizeof(command_header)) != 0) {
            return -1;
        }
        command = read_u32_le(command_header + 0);
        command_size = read_u32_le(command_header + 4);
        if (command_size < 8U || command_offset + (unsigned long long)command_size > file_size) {
            return -1;
        }

        if (command == MACHO_LC_SEGMENT_64 && command_size >= 72U) {
            unsigned char segment_command[72];
            char segment_name[17];
            unsigned int initprot;
            unsigned int nsects;
            unsigned int section_index;

            if (read_region(fd, command_offset, segment_command, sizeof(segment_command)) != 0) {
                return -1;
            }
            copy_fixed_name(segment_name, sizeof(segment_name), segment_command + 8, 16U);
            initprot = read_u32_le(segment_command + 60);
            nsects = read_u32_le(segment_command + 64);
            if (nsects > SIZE_MAX_SECTIONS || 72U + nsects * 80U > command_size) {
                return -1;
            }
            for (section_index = 0U; section_index < nsects; ++section_index) {
                unsigned char section_command[80];
                unsigned long long section_offset = command_offset + 72ULL + ((unsigned long long)section_index * 80ULL);
                char section_name[17];
                char section_segment[17];
                unsigned long long section_size;
                unsigned int section_flags;

                if (read_region(fd, section_offset, section_command, sizeof(section_command)) != 0) {
                    return -1;
                }
                copy_fixed_name(section_name, sizeof(section_name), section_command + 0, 16U);
                copy_fixed_name(section_segment, sizeof(section_segment), section_command + 16, 16U);
                if (section_segment[0] == '\0') {
                    rt_copy_string(section_segment, sizeof(section_segment), segment_name);
                }
                section_size = read_u64_le(section_command + 40);
                section_flags = read_u32_le(section_command + 64);
                add_macho_section_size(section_segment, section_name, initprot, section_flags, section_size, text, data, bss);
            }
        }

        command_offset += (unsigned long long)command_size;
    }
    return 0;
}

static int write_json_macho_segment_size(const char *path,
                                         const char *segment,
                                         const char *section,
                                         unsigned long long size,
                                         int zerofill,
                                         unsigned long long fileoff,
                                         unsigned long long vmaddr) {
    if (tool_json_begin_event(1, "size", "stdout", section != 0 ? "macho_section_size" : "macho_segment_size") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
    if (rt_write_cstr(1, "\"file\":") != 0 || tool_json_write_string(1, path) != 0) return -1;
    if (rt_write_cstr(1, ",\"segment\":") != 0 || tool_json_write_string(1, segment) != 0) return -1;
    if (section != 0 && (rt_write_cstr(1, ",\"section\":") != 0 || tool_json_write_string(1, section) != 0)) return -1;
    if (rt_write_cstr(1, ",\"size\":") != 0 || rt_write_uint(1, size) != 0) return -1;
    if (rt_write_cstr(1, ",\"zerofill\":") != 0 || rt_write_cstr(1, zerofill ? "true" : "false") != 0) return -1;
    if (rt_write_cstr(1, ",\"file_offset\":") != 0 || rt_write_uint(1, fileoff) != 0) return -1;
    if (rt_write_cstr(1, ",\"vmaddr\":") != 0 || rt_write_uint(1, vmaddr) != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int print_macho_segment_sizes(int fd, const char *path, const unsigned char *header, unsigned long long file_size) {
    unsigned int ncmds = read_u32_le(header + 16);
    unsigned int sizeofcmds = read_u32_le(header + 20);
    unsigned int command_index;
    unsigned long long command_offset = 32ULL;
    unsigned long long grand_total = 0ULL;

    if (ncmds > SIZE_MAX_MACHO_COMMANDS || sizeofcmds > file_size || 32ULL + (unsigned long long)sizeofcmds > file_size) return -1;
    for (command_index = 0U; command_index < ncmds; ++command_index) {
        unsigned char command_header[8];
        unsigned int command;
        unsigned int command_size;
        if (command_offset + 8ULL > file_size || read_region(fd, command_offset, command_header, sizeof(command_header)) != 0) return -1;
        command = read_u32_le(command_header + 0);
        command_size = read_u32_le(command_header + 4);
        if (command_size < 8U || command_offset + (unsigned long long)command_size > file_size) return -1;
        if (command == MACHO_LC_SEGMENT_64 && command_size >= 72U) {
            unsigned char segment_command[72];
            char segment_name[17];
            unsigned long long vmaddr;
            unsigned long long vmsize;
            unsigned long long fileoff;
            unsigned long long filesize;
            unsigned int nsects;
            unsigned int section_index;
            unsigned long long section_total = 0ULL;

            if (read_region(fd, command_offset, segment_command, sizeof(segment_command)) != 0) return -1;
            copy_fixed_name(segment_name, sizeof(segment_name), segment_command + 8, 16U);
            vmaddr = read_u64_le(segment_command + 24);
            vmsize = read_u64_le(segment_command + 32);
            fileoff = read_u64_le(segment_command + 40);
            filesize = read_u64_le(segment_command + 48);
            nsects = read_u32_le(segment_command + 64);
            if (nsects > SIZE_MAX_SECTIONS || 72U + nsects * 80U > command_size) return -1;
            if (size_json) {
                if (write_json_macho_segment_size(path, segment_name, 0, vmsize, filesize == 0ULL && vmsize != 0ULL, fileoff, vmaddr) != 0) return -1;
            } else {
                rt_write_cstr(1, "Segment ");
                rt_write_cstr(1, segment_name[0] != '\0' ? segment_name : "(none)");
                rt_write_cstr(1, ": ");
                rt_write_uint(1, vmsize);
                if (filesize == 0ULL && vmsize != 0ULL) rt_write_cstr(1, " (zero fill)");
                rt_write_char(1, '\n');
            }
            grand_total += vmsize;
            for (section_index = 0U; section_index < nsects; ++section_index) {
                unsigned char section_command[80];
                unsigned long long section_offset = command_offset + 72ULL + ((unsigned long long)section_index * 80ULL);
                char section_name[17];
                unsigned long long section_addr;
                unsigned long long section_size;
                unsigned int section_fileoff;
                unsigned int flags;
                int zerofill;
                if (read_region(fd, section_offset, section_command, sizeof(section_command)) != 0) return -1;
                copy_fixed_name(section_name, sizeof(section_name), section_command + 0, 16U);
                section_addr = read_u64_le(section_command + 32);
                section_size = read_u64_le(section_command + 40);
                section_fileoff = read_u32_le(section_command + 48);
                flags = read_u32_le(section_command + 64);
                zerofill = macho_is_zerofill_type(flags & 0xffU) || rt_strcmp(section_name, "__bss") == 0 || rt_strcmp(section_name, "__common") == 0;
                section_total += section_size;
                if (size_json) {
                    if (write_json_macho_segment_size(path, segment_name, section_name, section_size, zerofill, (unsigned long long)section_fileoff, section_addr) != 0) return -1;
                } else {
                    rt_write_cstr(1, "\tSection ");
                    rt_write_cstr(1, section_name);
                    rt_write_cstr(1, ": ");
                    rt_write_uint(1, section_size);
                    if (zerofill) rt_write_cstr(1, " (zerofill)");
                    rt_write_char(1, '\n');
                }
            }
            if (!size_json && nsects != 0U) {
                rt_write_cstr(1, "\ttotal ");
                rt_write_uint(1, section_total);
                rt_write_char(1, '\n');
            }
        }
        command_offset += (unsigned long long)command_size;
    }
    if (!size_json) {
        rt_write_cstr(1, "total ");
        rt_write_uint(1, grand_total);
        rt_write_char(1, '\n');
    }
    return 0;
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

static int write_json_size(const char *path,
                           unsigned long long text,
                           unsigned long long data,
                           unsigned long long bss,
                           unsigned long long file_size) {
    unsigned long long total = text + data + bss;

    if (tool_json_begin_event(1, "size", "stdout", "size") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{\"file\":") != 0) return -1;
    if (tool_json_write_string(1, path) != 0) return -1;
    if (rt_write_cstr(1, ",\"text\":") != 0) return -1;
    if (rt_write_uint(1, text) != 0) return -1;
    if (rt_write_cstr(1, ",\"data_size\":") != 0) return -1;
    if (rt_write_uint(1, data) != 0) return -1;
    if (rt_write_cstr(1, ",\"bss\":") != 0) return -1;
    if (rt_write_uint(1, bss) != 0) return -1;
    if (rt_write_cstr(1, ",\"total\":") != 0) return -1;
    if (rt_write_uint(1, total) != 0) return -1;
    if (rt_write_cstr(1, ",\"file_size\":") != 0) return -1;
    if (rt_write_uint(1, file_size) != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
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
    size_object_base = 0ULL;
    {
        long long end_offset = platform_seek(fd, 0, PLATFORM_SEEK_END);
        file_size = end_offset < 0 ? 0ULL : (unsigned long long)end_offset;
    }
    if (read_region(fd, 0ULL, header, sizeof(header)) != 0) {
        platform_close(fd);
        tool_write_error("size", "unsupported file: ", path);
        return 1;
    }
    if (read_u32_le(header) == MACHO_FAT_MAGIC_LE || read_u32_le(header) == MACHO_FAT_MAGIC_64_LE) {
        unsigned long long slice_offset;
        unsigned long long slice_size;
        if (select_macho_fat_slice(fd, &slice_offset, &slice_size) == 0) {
            size_object_base = slice_offset;
            file_size = slice_size;
            if (read_region(fd, 0ULL, header, sizeof(header)) != 0) {
                platform_close(fd);
                tool_write_error("size", "unsupported file: ", path);
                return 1;
            }
        }
    }
    if (read_u32_le(header) == MACHO_MAGIC_64_LE) {
        int result = size_macho64_file(fd, header, file_size, &text, &data, &bss);
        if (result != 0) {
            platform_close(fd);
            tool_write_error("size", "invalid Mach-O file: ", path);
            return 1;
        }
        if (size_json) {
            if (size_segments && print_macho_segment_sizes(fd, path, header, file_size) != 0) {
                platform_close(fd);
                return 1;
            }
            platform_close(fd);
            return write_json_size(path, text, data, bss, file_size) == 0 ? 0 : 1;
        }
        if (size_segments) {
            int segment_result = print_macho_segment_sizes(fd, path, header, file_size);
            platform_close(fd);
            if (segment_result != 0) return 1;
            return 0;
        }
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
        platform_close(fd);
        return 0;
    }
    if (header[0] != 0x7fU || header[1] != 'E' || header[2] != 'L' || header[3] != 'F' ||
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
    if (size_json) {
        return write_json_size(path, text, data, bss, file_size) == 0 ? 0 : 1;
    }
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
    tool_write_usage("size", "[-m|--segments] [--json] FILE ...");
}

int main(int argc, char **argv) {
    int i;
    int status = 0;
    int argi = 1;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "--help") == 0 || rt_strcmp(argv[argi], "-h") == 0) {
            print_usage();
            return 0;
        } else if (rt_strcmp(argv[argi], "--json") == 0) {
            size_json = 1;
            tool_json_set_enabled(1);
        } else if (rt_strcmp(argv[argi], "-m") == 0 || rt_strcmp(argv[argi], "--segments") == 0) {
            size_segments = 1;
        } else {
            tool_write_error("size", "unknown option: ", argv[argi]);
            return 1;
        }
        argi += 1;
    }
    if (argi >= argc) {
        print_usage();
        return 1;
    }
    if (!size_json && !size_segments) {
        rt_write_line(1, "text\tdata\tbss\tdec\thex\tfile\tname");
    }
    for (i = argi; i < argc; ++i) {
        if (size_file(argv[i], 1) != 0) {
            status = 1;
        }
    }
    return status;
}
