#include "platform.h"
#include "runtime.h"
#include "tool_util.h"
#include "archive_util.h"

#define READELF_MAX_SECTIONS 256U
#define READELF_NAME_TABLE_CAPACITY 65536U

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
    unsigned int flags;
    unsigned short ehsize;
    unsigned short phentsize;
    unsigned short phnum;
    unsigned short shentsize;
    unsigned short shnum;
    unsigned short shstrndx;
} ElfHeaderInfo;

static unsigned short read_u16_le(const unsigned char *bytes) {
    return (unsigned short)bytes[0] | (unsigned short)((unsigned short)bytes[1] << 8);
}

static unsigned int read_u32_le_local(const unsigned char *bytes) {
    return archive_read_u32_le(bytes);
}

static unsigned long long read_u64_le_local(const unsigned char *bytes) {
    return archive_read_u64_le(bytes);
}

static const char *elf_type_name(unsigned short type) {
    if (type == 1U) return "REL (Relocatable)";
    if (type == 2U) return "EXEC (Executable)";
    if (type == 3U) return "DYN (Shared object)";
    if (type == 4U) return "CORE";
    return "UNKNOWN";
}

static const char *elf_machine_name(unsigned short machine) {
    if (machine == 62U) return "Advanced Micro Devices X86-64";
    if (machine == 183U) return "AArch64";
    return "Unknown";
}

static const char *elf_section_type_name(unsigned int type) {
    if (type == 0U) return "NULL";
    if (type == 1U) return "PROGBITS";
    if (type == 2U) return "SYMTAB";
    if (type == 3U) return "STRTAB";
    if (type == 4U) return "RELA";
    if (type == 8U) return "NOBITS";
    if (type == 9U) return "REL";
    if (type == 11U) return "DYNSYM";
    return "OTHER";
}

static const char *elf_symbol_bind_name(unsigned int bind) {
    if (bind == 0U) return "LOCAL";
    if (bind == 1U) return "GLOBAL";
    if (bind == 2U) return "WEAK";
    return "OTHER";
}

static const char *elf_symbol_type_name(unsigned int type) {
    if (type == 0U) return "NOTYPE";
    if (type == 1U) return "OBJECT";
    if (type == 2U) return "FUNC";
    if (type == 3U) return "SECTION";
    if (type == 4U) return "FILE";
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

static int read_region(int fd, unsigned long long offset, unsigned char *buffer, size_t size) {
    if (platform_seek(fd, (long long)offset, PLATFORM_SEEK_SET) < 0) {
        return -1;
    }
    return archive_read_exact(fd, buffer, size);
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
    info->flags = read_u32_le_local(header + 48);
    info->ehsize = read_u16_le(header + 52);
    info->phentsize = read_u16_le(header + 54);
    info->phnum = read_u16_le(header + 56);
    info->shentsize = read_u16_le(header + 58);
    info->shnum = read_u16_le(header + 60);
    info->shstrndx = read_u16_le(header + 62);
    return 0;
}

static int load_sections(int fd, const ElfHeaderInfo *header, ElfSectionInfo *sections) {
    unsigned char raw[64];
    unsigned short i;

    if (header->shnum > READELF_MAX_SECTIONS || header->shentsize < 64U) {
        return -1;
    }

    for (i = 0U; i < header->shnum; ++i) {
        unsigned long long offset = header->shoff + ((unsigned long long)i * (unsigned long long)header->shentsize);
        if (read_region(fd, offset, raw, 64U) != 0) {
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

static void print_header(const ElfHeaderInfo *info) {
    rt_write_line(1, "ELF Header:");
    rt_write_cstr(1, "  Type: ");
    rt_write_line(1, elf_type_name(info->type));
    rt_write_cstr(1, "  Machine: ");
    rt_write_line(1, elf_machine_name(info->machine));
    rt_write_cstr(1, "  Entry point: ");
    write_hex_value(info->entry);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  Program headers: ");
    rt_write_uint(1, info->phnum);
    rt_write_cstr(1, " at ");
    write_hex_value(info->phoff);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  Section headers: ");
    rt_write_uint(1, info->shnum);
    rt_write_cstr(1, " at ");
    write_hex_value(info->shoff);
    rt_write_char(1, '\n');
}

static void print_sections(const ElfHeaderInfo *header, const ElfSectionInfo *sections, const char *names, size_t names_size) {
    unsigned short i;
    (void)header;

    rt_write_line(1, "Section Headers:");
    for (i = 0U; i < header->shnum; ++i) {
        const char *name = name_from_table(names, names_size, sections[i].name);
        rt_write_cstr(1, "  [");
        rt_write_uint(1, i);
        rt_write_cstr(1, "] ");
        rt_write_cstr(1, name);
        rt_write_cstr(1, " type=");
        rt_write_cstr(1, elf_section_type_name(sections[i].type));
        rt_write_cstr(1, " off=");
        write_hex_value(sections[i].offset);
        rt_write_cstr(1, " size=");
        write_hex_value(sections[i].size);
        rt_write_char(1, '\n');
    }
}

static void print_symbols(int fd, const ElfHeaderInfo *header, const ElfSectionInfo *sections, const char *section_names, size_t section_names_size) {
    unsigned short i;
    (void)section_names;
    (void)section_names_size;

    for (i = 0U; i < header->shnum; ++i) {
        if (sections[i].type == 2U || sections[i].type == 11U) {
            const ElfSectionInfo *symtab = &sections[i];
            char strings[READELF_NAME_TABLE_CAPACITY];
            size_t string_size = 0U;
            unsigned long long count;
            unsigned long long index;
            unsigned char entry[24];

            if (symtab->entsize == 0ULL || symtab->link >= header->shnum) {
                continue;
            }

            if (sections[symtab->link].size > 0ULL) {
                size_t to_read = (size_t)(sections[symtab->link].size < (unsigned long long)(sizeof(strings) - 1U) ? sections[symtab->link].size : (unsigned long long)(sizeof(strings) - 1U));
                if (read_region(fd, sections[symtab->link].offset, (unsigned char *)strings, to_read) == 0) {
                    strings[to_read] = '\0';
                    string_size = to_read;
                }
            }

            rt_write_cstr(1, "Symbol table '");
            rt_write_cstr(1, name_from_table(section_names, section_names_size, symtab->name));
            rt_write_line(1, "':");

            count = symtab->size / symtab->entsize;
            for (index = 0ULL; index < count; ++index) {
                unsigned int st_name;
                unsigned int st_bind;
                unsigned int st_type;
                unsigned short st_shndx;
                unsigned long long st_value;
                unsigned long long st_size;
                const char *name;

                if (read_region(fd, symtab->offset + (index * symtab->entsize), entry, sizeof(entry)) != 0) {
                    break;
                }

                st_name = read_u32_le_local(entry + 0);
                st_bind = (unsigned int)(entry[4] >> 4);
                st_type = (unsigned int)(entry[4] & 0x0fU);
                st_shndx = read_u16_le(entry + 6);
                st_value = read_u64_le_local(entry + 8);
                st_size = read_u64_le_local(entry + 16);
                name = name_from_table(strings, string_size, st_name);

                rt_write_cstr(1, "  [");
                rt_write_uint(1, index);
                rt_write_cstr(1, "] ");
                rt_write_cstr(1, name);
                rt_write_cstr(1, " value=");
                write_hex_value(st_value);
                rt_write_cstr(1, " size=");
                rt_write_uint(1, st_size);
                rt_write_cstr(1, " bind=");
                rt_write_cstr(1, elf_symbol_bind_name(st_bind));
                rt_write_cstr(1, " type=");
                rt_write_cstr(1, elf_symbol_type_name(st_type));
                rt_write_cstr(1, " shndx=");
                rt_write_uint(1, st_shndx);
                rt_write_char(1, '\n');
            }
        }
    }
}

int main(int argc, char **argv) {
    int show_header_flag = 0;
    int show_sections_flag = 0;
    int show_symbols_flag = 0;
    int argi = 1;
    int exit_code = 0;
    int i;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--file-header") == 0) {
            show_header_flag = 1;
        } else if (rt_strcmp(argv[argi], "-S") == 0 || rt_strcmp(argv[argi], "--sections") == 0) {
            show_sections_flag = 1;
        } else if (rt_strcmp(argv[argi], "-s") == 0 || rt_strcmp(argv[argi], "--symbols") == 0) {
            show_symbols_flag = 1;
        } else {
            tool_write_usage("readelf", "[-h] [-S] [-s] file ...");
            return 1;
        }
        argi += 1;
    }

    if (!show_header_flag && !show_sections_flag && !show_symbols_flag) {
        show_header_flag = 1;
    }

    if (argi >= argc) {
        tool_write_usage("readelf", "[-h] [-S] [-s] file ...");
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        int fd = platform_open_read(argv[i]);
        ElfHeaderInfo header;
        ElfSectionInfo sections[READELF_MAX_SECTIONS];
        char names[READELF_NAME_TABLE_CAPACITY];
        size_t names_size = 0U;

        if (fd < 0) {
            rt_write_cstr(2, "readelf: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (parse_elf_header(fd, &header) != 0 || load_sections(fd, &header, sections) != 0 || load_name_table(fd, &header, sections, names, sizeof(names), &names_size) != 0) {
            rt_write_cstr(2, "readelf: unsupported or invalid ELF file ");
            rt_write_line(2, argv[i]);
            platform_close(fd);
            exit_code = 1;
            continue;
        }

        rt_write_cstr(1, "File: ");
        rt_write_line(1, argv[i]);
        if (show_header_flag) {
            print_header(&header);
        }
        if (show_sections_flag) {
            print_sections(&header, sections, names, names_size);
        }
        if (show_symbols_flag) {
            print_symbols(fd, &header, sections, names, names_size);
        }
        platform_close(fd);
    }

    return exit_code;
}
