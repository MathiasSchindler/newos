#include "platform.h"
#include "runtime.h"
#include "tool_util.h"
#include "archive_util.h"

#define OBJDUMP_MAX_SECTIONS 256U
#define OBJDUMP_NAME_TABLE_CAPACITY 65536U

typedef struct {
    unsigned int name;
    unsigned int type;
    unsigned long long flags;
    unsigned long long addr;
    unsigned long long offset;
    unsigned long long size;
    unsigned int link;
    unsigned long long entsize;
} ElfSectionInfo;

typedef struct {
    unsigned short type;
    unsigned short machine;
    unsigned long long entry;
    unsigned long long phoff;
    unsigned long long shoff;
    unsigned short phnum;
    unsigned short shnum;
    unsigned short shstrndx;
    unsigned short shentsize;
} ElfHeaderInfo;

typedef struct {
    unsigned int magic;
    unsigned int cputype;
    unsigned int cpusubtype;
    unsigned int filetype;
    unsigned int ncmds;
    unsigned int sizeofcmds;
    unsigned int flags;
} MachHeaderInfo;

static unsigned short read_u16_le(const unsigned char *bytes) {
    return (unsigned short)bytes[0] | (unsigned short)((unsigned short)bytes[1] << 8);
}

static unsigned int read_u32_le_local(const unsigned char *bytes) {
    return archive_read_u32_le(bytes);
}

static unsigned long long read_u64_le_local(const unsigned char *bytes) {
    return archive_read_u64_le(bytes);
}

static const char *macho_machine_name(unsigned int cputype) {
    unsigned int family = cputype & 0x00ffffffU;

    if (family == 7U) return "x86-64";
    if (family == 12U) return "aarch64";
    return "unknown";
}

static const char *macho_type_name(unsigned int type) {
    if (type == 1U) return "object";
    if (type == 2U) return "executable";
    if (type == 6U) return "shared-library";
    if (type == 8U) return "bundle";
    return "unknown";
}

static int read_region(int fd, unsigned long long offset, unsigned char *buffer, size_t size) {
    if (platform_seek(fd, (long long)offset, PLATFORM_SEEK_SET) < 0) {
        return -1;
    }
    return archive_read_exact(fd, buffer, size);
}

static const char *machine_name(unsigned short machine) {
    if (machine == 62U) return "x86-64";
    if (machine == 183U) return "aarch64";
    return "unknown";
}

static const char *type_name(unsigned short type) {
    if (type == 1U) return "relocatable";
    if (type == 2U) return "executable";
    if (type == 3U) return "shared-object";
    return "unknown";
}

static const char *section_type_name(unsigned int type) {
    if (type == 0U) return "NULL";
    if (type == 1U) return "PROGBITS";
    if (type == 2U) return "SYMTAB";
    if (type == 3U) return "STRTAB";
    if (type == 8U) return "NOBITS";
    if (type == 11U) return "DYNSYM";
    return "OTHER";
}

static void write_hex_value(unsigned long long value) {
    char digits[32];
    size_t count = 0U;

    rt_write_cstr(1, "0x");
    do {
        unsigned int nibble = (unsigned int)(value & 0xfULL);
        digits[count++] = (char)(nibble < 10U ? ('0' + nibble) : ('a' + (nibble - 10U)));
        value >>= 4ULL;
    } while (value != 0ULL && count < sizeof(digits));

    while (count > 0U) {
        count -= 1U;
        rt_write_char(1, digits[count]);
    }
}

static void write_hex_byte(unsigned char value) {
    static const char digits[] = "0123456789abcdef";
    rt_write_char(1, digits[(value >> 4) & 0x0fU]);
    rt_write_char(1, digits[value & 0x0fU]);
}

static int parse_elf_header(int fd, ElfHeaderInfo *info) {
    unsigned char header[64];
    if (read_region(fd, 0ULL, header, sizeof(header)) != 0) {
        return -1;
    }
    if (!(header[0] == 0x7fU && header[1] == 'E' && header[2] == 'L' && header[3] == 'F')) {
        return -1;
    }
    if (header[4] != 2U || header[5] != 1U) {
        return -1;
    }
    info->type = read_u16_le(header + 16);
    info->machine = read_u16_le(header + 18);
    info->entry = read_u64_le_local(header + 24);
    info->phoff = read_u64_le_local(header + 32);
    info->shoff = read_u64_le_local(header + 40);
    info->phnum = read_u16_le(header + 56);
    info->shentsize = read_u16_le(header + 58);
    info->shnum = read_u16_le(header + 60);
    info->shstrndx = read_u16_le(header + 62);
    return 0;
}

static int parse_macho_header(int fd, MachHeaderInfo *info) {
    unsigned char header[32];
    unsigned int magic;

    if (read_region(fd, 0ULL, header, sizeof(header)) != 0) {
        return -1;
    }
    magic = read_u32_le_local(header + 0);
    if (magic != 0xfeedfacfU) {
        return -1;
    }

    info->magic = magic;
    info->cputype = read_u32_le_local(header + 4);
    info->cpusubtype = read_u32_le_local(header + 8);
    info->filetype = read_u32_le_local(header + 12);
    info->ncmds = read_u32_le_local(header + 16);
    info->sizeofcmds = read_u32_le_local(header + 20);
    info->flags = read_u32_le_local(header + 24);
    return 0;
}

static int load_sections(int fd, const ElfHeaderInfo *header, ElfSectionInfo *sections) {
    unsigned short i;
    unsigned char raw[64];

    if (header->shnum > OBJDUMP_MAX_SECTIONS || header->shentsize < 64U) {
        return -1;
    }

    for (i = 0U; i < header->shnum; ++i) {
        unsigned long long offset = header->shoff + ((unsigned long long)i * (unsigned long long)header->shentsize);
        if (read_region(fd, offset, raw, sizeof(raw)) != 0) {
            return -1;
        }
        sections[i].name = read_u32_le_local(raw + 0);
        sections[i].type = read_u32_le_local(raw + 4);
        sections[i].flags = read_u64_le_local(raw + 8);
        sections[i].addr = read_u64_le_local(raw + 16);
        sections[i].offset = read_u64_le_local(raw + 24);
        sections[i].size = read_u64_le_local(raw + 32);
        sections[i].link = read_u32_le_local(raw + 40);
        sections[i].entsize = read_u64_le_local(raw + 56);
    }
    return 0;
}

static int load_name_table(int fd,
                           const ElfHeaderInfo *header,
                           const ElfSectionInfo *sections,
                           char *buffer,
                           size_t buffer_capacity,
                           size_t *size_out) {
    const ElfSectionInfo *section;
    size_t to_read;

    *size_out = 0U;
    if (header->shstrndx >= header->shnum) {
        return 0;
    }

    section = &sections[header->shstrndx];
    to_read = (size_t)(section->size < (unsigned long long)(buffer_capacity - 1U) ? section->size : (unsigned long long)(buffer_capacity - 1U));
    if (to_read == 0U) {
        return 0;
    }
    if (read_region(fd, section->offset, (unsigned char *)buffer, to_read) != 0) {
        return -1;
    }
    buffer[to_read] = '\0';
    *size_out = to_read;
    return 0;
}

static const char *name_from_table(const char *table, size_t table_size, unsigned int offset) {
    if (table == 0 || offset >= table_size) {
        return "";
    }
    return table + offset;
}

static void print_file_header(const char *path, const ElfHeaderInfo *header) {
    rt_write_cstr(1, "\n");
    rt_write_line(1, path);
    rt_write_cstr(1, "file format elf64-");
    rt_write_line(1, machine_name(header->machine));
    rt_write_cstr(1, "architecture: ");
    rt_write_cstr(1, machine_name(header->machine));
    rt_write_cstr(1, ", type: ");
    rt_write_line(1, type_name(header->type));
    rt_write_cstr(1, "start address ");
    write_hex_value(header->entry);
    rt_write_char(1, '\n');
}

static void print_macho_file_header(const char *path, const MachHeaderInfo *header) {
    rt_write_cstr(1, "\n");
    rt_write_line(1, path);
    rt_write_line(1, "file format mach-o-64");
    rt_write_cstr(1, "architecture: ");
    rt_write_cstr(1, macho_machine_name(header->cputype));
    rt_write_cstr(1, ", type: ");
    rt_write_line(1, macho_type_name(header->filetype));
    rt_write_cstr(1, "load commands: ");
    rt_write_uint(1, (unsigned long long)header->ncmds);
    rt_write_cstr(1, ", flags: ");
    write_hex_value((unsigned long long)header->flags);
    rt_write_char(1, '\n');
}

static void print_section_table(const ElfHeaderInfo *header, const ElfSectionInfo *sections, const char *names, size_t names_size) {
    unsigned short i;

    rt_write_line(1, "Sections:");
    for (i = 0U; i < header->shnum; ++i) {
        rt_write_cstr(1, "  ");
        rt_write_uint(1, i);
        rt_write_cstr(1, " ");
        rt_write_cstr(1, name_from_table(names, names_size, sections[i].name));
        rt_write_cstr(1, " ");
        rt_write_cstr(1, section_type_name(sections[i].type));
        rt_write_cstr(1, " addr=");
        write_hex_value(sections[i].addr);
        rt_write_cstr(1, " off=");
        write_hex_value(sections[i].offset);
        rt_write_cstr(1, " size=");
        write_hex_value(sections[i].size);
        rt_write_char(1, '\n');
    }
}

static void dump_section_bytes(int fd, const char *name, const ElfSectionInfo *section) {
    unsigned char buffer[16];
    unsigned long long remaining = section->size;
    unsigned long long current_offset = section->offset;
    unsigned long long display_addr = section->addr;

    if (remaining == 0ULL) {
        return;
    }

    rt_write_cstr(1, "Contents of section ");
    rt_write_line(1, name);

    while (remaining > 0ULL) {
        size_t chunk = remaining > 16ULL ? 16U : (size_t)remaining;
        size_t i;
        if (read_region(fd, current_offset, buffer, chunk) != 0) {
            break;
        }

        write_hex_value(display_addr);
        rt_write_cstr(1, " ");
        for (i = 0U; i < 16U; ++i) {
            if (i < chunk) {
                write_hex_byte(buffer[i]);
            } else {
                rt_write_cstr(1, "  ");
            }
            if (i != 15U) {
                rt_write_char(1, ' ');
            }
        }
        rt_write_char(1, '\n');

        current_offset += (unsigned long long)chunk;
        display_addr += (unsigned long long)chunk;
        remaining -= (unsigned long long)chunk;
    }
}

static void print_symbols(int fd, const ElfHeaderInfo *header, const ElfSectionInfo *sections, const char *names, size_t names_size) {
    unsigned short i;

    for (i = 0U; i < header->shnum; ++i) {
        if (sections[i].type == 2U || sections[i].type == 11U) {
            char strings[OBJDUMP_NAME_TABLE_CAPACITY];
            size_t string_size = 0U;
            unsigned long long count;
            unsigned long long index;
            unsigned char entry[24];

            if (sections[i].entsize == 0ULL || sections[i].link >= header->shnum) {
                continue;
            }

            if (sections[sections[i].link].size > 0ULL) {
                size_t to_read = (size_t)(sections[sections[i].link].size < (unsigned long long)(sizeof(strings) - 1U) ? sections[sections[i].link].size : (unsigned long long)(sizeof(strings) - 1U));
                if (read_region(fd, sections[sections[i].link].offset, (unsigned char *)strings, to_read) == 0) {
                    strings[to_read] = '\0';
                    string_size = to_read;
                }
            }

            rt_write_cstr(1, "SYMBOL TABLE: ");
            rt_write_line(1, name_from_table(names, names_size, sections[i].name));

            count = sections[i].size / sections[i].entsize;
            for (index = 0ULL; index < count; ++index) {
                unsigned int st_name;
                unsigned long long st_value;
                if (read_region(fd, sections[i].offset + (index * sections[i].entsize), entry, sizeof(entry)) != 0) {
                    break;
                }
                st_name = read_u32_le_local(entry + 0);
                st_value = read_u64_le_local(entry + 8);
                write_hex_value(st_value);
                rt_write_cstr(1, " ");
                rt_write_line(1, name_from_table(strings, string_size, st_name));
            }
        }
    }
}

int main(int argc, char **argv) {
    int show_file = 0;
    int show_sections = 0;
    int show_contents = 0;
    int show_symbols = 0;
    int argi = 1;
    int exit_code = 0;
    int i;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-f") == 0) {
            show_file = 1;
        } else if (rt_strcmp(argv[argi], "-h") == 0) {
            show_sections = 1;
        } else if (rt_strcmp(argv[argi], "-s") == 0) {
            show_contents = 1;
        } else if (rt_strcmp(argv[argi], "-t") == 0) {
            show_symbols = 1;
        } else {
            tool_write_usage("objdump", "[-f] [-h] [-s] [-t] file ...");
            return 1;
        }
        argi += 1;
    }

    if (!show_file && !show_sections && !show_contents && !show_symbols) {
        show_file = 1;
        show_sections = 1;
    }

    if (argi >= argc) {
        tool_write_usage("objdump", "[-f] [-h] [-s] [-t] file ...");
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        int fd = platform_open_read(argv[i]);
        ElfHeaderInfo header;
        MachHeaderInfo macho;
        ElfSectionInfo sections[OBJDUMP_MAX_SECTIONS];
        char names[OBJDUMP_NAME_TABLE_CAPACITY];
        size_t names_size = 0U;
        unsigned short section_index;

        if (fd < 0) {
            rt_write_cstr(2, "objdump: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (parse_elf_header(fd, &header) != 0 || load_sections(fd, &header, sections) != 0 ||
            load_name_table(fd, &header, sections, names, sizeof(names), &names_size) != 0) {
            if (parse_macho_header(fd, &macho) == 0) {
                if (show_file) {
                    print_macho_file_header(argv[i], &macho);
                }
                if (show_sections) {
                    rt_write_line(1, "Section dumping for Mach-O inputs is not implemented yet.");
                }
                if (show_contents) {
                    rt_write_line(1, "Raw section contents for Mach-O inputs are not implemented yet.");
                }
                if (show_symbols) {
                    rt_write_line(1, "Symbol dumping for Mach-O inputs is not implemented yet.");
                }
                platform_close(fd);
                continue;
            }

            rt_write_cstr(2, "objdump: unsupported or invalid object file ");
            rt_write_line(2, argv[i]);
            platform_close(fd);
            exit_code = 1;
            continue;
        }

        if (show_file) {
            print_file_header(argv[i], &header);
        }
        if (show_sections) {
            print_section_table(&header, sections, names, names_size);
        }
        if (show_contents) {
            for (section_index = 0U; section_index < header.shnum; ++section_index) {
                if (sections[section_index].size > 0ULL && sections[section_index].type != 8U) {
                    dump_section_bytes(fd, name_from_table(names, names_size, sections[section_index].name), &sections[section_index]);
                }
            }
        }
        if (show_symbols) {
            print_symbols(fd, &header, sections, names, names_size);
        }

        platform_close(fd);
    }

    return exit_code;
}
