#include "platform.h"
#include "runtime.h"
#include "tool_util.h"
#include "archive_util.h"

#define NM_MAX_SECTIONS 512U
#define NM_MAX_SYMBOLS 8192U
#define NM_NAME_TABLE_CAPACITY 65536U

#define ELF_SHT_SYMTAB 2U
#define ELF_SHT_STRTAB 3U
#define ELF_SHT_NOBITS 8U
#define ELF_SHT_DYNSYM 11U
#define ELF_SHN_UNDEF 0U
#define ELF_SHN_ABS 0xfff1U

typedef struct {
    unsigned int name;
    unsigned int type;
    unsigned long long flags;
    unsigned long long addr;
    unsigned long long offset;
    unsigned long long size;
    unsigned int link;
    unsigned long long entsize;
} NmSection;

typedef struct {
    unsigned long long value;
    unsigned long long size;
    unsigned char info;
    unsigned short shndx;
    char type;
    char name[NM_NAME_TABLE_CAPACITY / 256U];
} NmSymbol;

static NmSection nm_sections[NM_MAX_SECTIONS];
static NmSymbol nm_symbols[NM_MAX_SYMBOLS];
static char nm_string_table[NM_NAME_TABLE_CAPACITY];
static size_t nm_symbol_count;
static int nm_sort_enabled = 1;
static int nm_sort_numeric = 1;
static int nm_undefined_only;
static int nm_external_only;
static int nm_json;

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

static void write_hex16(unsigned long long value) {
    static const char digits[] = "0123456789abcdef";
    int shift;

    for (shift = 60; shift >= 0; shift -= 4) {
        rt_write_char(1, digits[(value >> (unsigned int)shift) & 0xfULL]);
    }
}

static const char *string_at(const char *table, size_t table_size, unsigned int offset) {
    if ((size_t)offset >= table_size) {
        return "";
    }
    return table + offset;
}

static char section_symbol_type(const NmSection *section, unsigned char info, unsigned short shndx) {
    unsigned int bind = (unsigned int)(info >> 4U);
    char type;

    if (shndx == ELF_SHN_UNDEF) {
        type = 'U';
    } else if (shndx == ELF_SHN_ABS) {
        type = 'A';
    } else if (section == 0) {
        type = '?';
    } else if (section->type == ELF_SHT_NOBITS) {
        type = 'B';
    } else if ((section->flags & 4ULL) != 0ULL) {
        type = 'T';
    } else if ((section->flags & 1ULL) != 0ULL) {
        type = 'D';
    } else {
        type = 'R';
    }
    if (bind == 0U && type >= 'A' && type <= 'Z') {
        type = (char)(type - 'A' + 'a');
    }
    return type;
}

static int compare_symbols(const void *left_ptr, const void *right_ptr) {
    const NmSymbol *left = (const NmSymbol *)left_ptr;
    const NmSymbol *right = (const NmSymbol *)right_ptr;

    if (nm_sort_numeric && left->value != right->value) {
        return left->value < right->value ? -1 : 1;
    }
    return rt_strcmp(left->name, right->name);
}

static const char *symbol_bind_name(unsigned char info) {
    unsigned int bind = (unsigned int)(info >> 4U);

    if (bind == 0U) return "local";
    if (bind == 1U) return "global";
    if (bind == 2U) return "weak";
    return "other";
}

static int add_symbol(const char *name, unsigned long long value, unsigned long long size, unsigned char info, unsigned short shndx, char type) {
    unsigned int bind = (unsigned int)(info >> 4U);

    if (name == 0 || name[0] == '\0') {
        return 0;
    }
    if (nm_undefined_only && shndx != ELF_SHN_UNDEF) {
        return 0;
    }
    if (nm_external_only && bind == 0U) {
        return 0;
    }
    if (nm_symbol_count >= NM_MAX_SYMBOLS) {
        return -1;
    }
    nm_symbols[nm_symbol_count].value = value;
    nm_symbols[nm_symbol_count].size = size;
    nm_symbols[nm_symbol_count].info = info;
    nm_symbols[nm_symbol_count].shndx = shndx;
    nm_symbols[nm_symbol_count].type = type;
    rt_copy_string(nm_symbols[nm_symbol_count].name, sizeof(nm_symbols[nm_symbol_count].name), name);
    nm_symbol_count += 1U;
    return 0;
}

static int write_json_symbol(const char *path, const NmSymbol *symbol) {
    char type_text[2];

    type_text[0] = symbol->type;
    type_text[1] = '\0';
    if (tool_json_begin_event(1, "nm", "stdout", "symbol") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{\"file\":") != 0) return -1;
    if (tool_json_write_string(1, path) != 0) return -1;
    if (rt_write_cstr(1, ",\"name\":") != 0) return -1;
    if (tool_json_write_string(1, symbol->name) != 0) return -1;
    if (rt_write_cstr(1, ",\"type\":") != 0) return -1;
    if (tool_json_write_string(1, type_text) != 0) return -1;
    if (rt_write_cstr(1, ",\"bind\":") != 0) return -1;
    if (tool_json_write_string(1, symbol_bind_name(symbol->info)) != 0) return -1;
    if (rt_write_cstr(1, ",\"defined\":") != 0) return -1;
    if (rt_write_cstr(1, symbol->shndx == ELF_SHN_UNDEF ? "false" : "true") != 0) return -1;
    if (rt_write_cstr(1, ",\"value\":") != 0) return -1;
    if (symbol->shndx == ELF_SHN_UNDEF) {
        if (rt_write_cstr(1, "null") != 0) return -1;
    } else if (rt_write_uint(1, symbol->value) != 0) {
        return -1;
    }
    if (rt_write_cstr(1, ",\"size\":") != 0) return -1;
    if (rt_write_uint(1, symbol->size) != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int load_symbols_from_table(int fd, const NmSection *symtab, const char *strings, size_t string_size) {
    unsigned long long count;
    unsigned long long index;

    if (symtab->entsize < 24ULL || symtab->entsize == 0ULL) {
        return -1;
    }
    count = symtab->size / symtab->entsize;
    for (index = 0ULL; index < count; ++index) {
        unsigned char raw[24];
        unsigned int name_offset;
        unsigned char info;
        unsigned short shndx;
        unsigned long long value;
        unsigned long long size;
        const NmSection *section = 0;

        if (read_region(fd, symtab->offset + (index * symtab->entsize), raw, sizeof(raw)) != 0) {
            return -1;
        }
        name_offset = read_u32_le(raw + 0);
        info = raw[4];
        shndx = read_u16_le(raw + 6);
        value = read_u64_le(raw + 8);
        size = read_u64_le(raw + 16);
        if (shndx != ELF_SHN_UNDEF && shndx != ELF_SHN_ABS && shndx < NM_MAX_SECTIONS) {
            section = &nm_sections[shndx];
        }
        if (add_symbol(string_at(strings, string_size, name_offset), value, size, info, shndx, section_symbol_type(section, info, shndx)) != 0) {
            return -1;
        }
    }
    return 0;
}

static int nm_file(const char *path) {
    int fd = platform_open_read(path);
    unsigned char header[64];
    unsigned long long shoff;
    unsigned short shentsize;
    unsigned short shnum;
    unsigned short shstrndx;
    unsigned short i;
    int want_dynsym = 1;
    int saw_symbols = 0;

    nm_symbol_count = 0U;
    if (fd < 0) {
        tool_write_error("nm", "cannot open ", path);
        return 1;
    }
    if (read_region(fd, 0ULL, header, sizeof(header)) != 0 ||
        header[0] != 0x7fU || header[1] != 'E' || header[2] != 'L' || header[3] != 'F' ||
        header[4] != 2U || header[5] != 1U) {
        platform_close(fd);
        tool_write_error("nm", "unsupported file: ", path);
        return 1;
    }
    shoff = read_u64_le(header + 40);
    shentsize = read_u16_le(header + 58);
    shnum = read_u16_le(header + 60);
    shstrndx = read_u16_le(header + 62);
    if (shnum == 0U || shnum > NM_MAX_SECTIONS || shentsize < 64U || shstrndx >= shnum) {
        platform_close(fd);
        tool_write_error("nm", "invalid section table: ", path);
        return 1;
    }
    for (i = 0U; i < shnum; ++i) {
        unsigned char raw[64];
        if (read_region(fd, shoff + ((unsigned long long)i * shentsize), raw, sizeof(raw)) != 0) {
            platform_close(fd);
            return 1;
        }
        nm_sections[i].name = read_u32_le(raw + 0);
        nm_sections[i].type = read_u32_le(raw + 4);
        nm_sections[i].flags = read_u64_le(raw + 8);
        nm_sections[i].addr = read_u64_le(raw + 16);
        nm_sections[i].offset = read_u64_le(raw + 24);
        nm_sections[i].size = read_u64_le(raw + 32);
        nm_sections[i].link = read_u32_le(raw + 40);
        nm_sections[i].entsize = read_u64_le(raw + 56);
    }
    for (i = 0U; i < shnum; ++i) {
        if (nm_sections[i].type == ELF_SHT_SYMTAB) {
            want_dynsym = 0;
            break;
        }
    }
    for (i = 0U; i < shnum; ++i) {
        if ((nm_sections[i].type == ELF_SHT_SYMTAB || (want_dynsym && nm_sections[i].type == ELF_SHT_DYNSYM)) && nm_sections[i].link < shnum) {
            const NmSection *strtab = &nm_sections[nm_sections[i].link];
            size_t read_size = (size_t)(strtab->size < (unsigned long long)sizeof(nm_string_table) ? strtab->size : (unsigned long long)sizeof(nm_string_table));
            if (strtab->type != ELF_SHT_STRTAB || read_region(fd, strtab->offset, (unsigned char *)nm_string_table, read_size) != 0) {
                continue;
            }
            if (read_size > 0U) {
                nm_string_table[read_size - 1U] = '\0';
            }
            if (load_symbols_from_table(fd, &nm_sections[i], nm_string_table, read_size) == 0) {
                saw_symbols = 1;
            }
        }
    }
    platform_close(fd);
    if (!saw_symbols || nm_symbol_count == 0U) {
        tool_write_error("nm", "no symbols: ", path);
        return 1;
    }
    if (nm_sort_enabled) {
        rt_sort(nm_symbols, nm_symbol_count, sizeof(nm_symbols[0]), compare_symbols);
    }
    for (i = 0U; i < nm_symbol_count; ++i) {
        if (nm_json) {
            if (write_json_symbol(path, &nm_symbols[i]) != 0) {
                return 1;
            }
            continue;
        }
        if (nm_symbols[i].shndx == ELF_SHN_UNDEF) {
            rt_write_cstr(1, "                 ");
        } else {
            write_hex16(nm_symbols[i].value);
            rt_write_char(1, ' ');
        }
        rt_write_char(1, nm_symbols[i].type);
        rt_write_char(1, ' ');
        rt_write_line(1, nm_symbols[i].name);
    }
    return 0;
}

static void print_usage(void) {
    tool_write_usage("nm", "[-n] [-p] [-u] [-g] [--json] FILE ...");
}

int main(int argc, char **argv) {
    int argi = 1;
    int status = 0;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "--help") == 0 || rt_strcmp(argv[argi], "-h") == 0) {
            print_usage();
            return 0;
        } else if (rt_strcmp(argv[argi], "-n") == 0 || rt_strcmp(argv[argi], "--numeric-sort") == 0) {
            nm_sort_numeric = 1;
        } else if (rt_strcmp(argv[argi], "-p") == 0 || rt_strcmp(argv[argi], "--no-sort") == 0) {
            nm_sort_enabled = 0;
        } else if (rt_strcmp(argv[argi], "-u") == 0 || rt_strcmp(argv[argi], "--undefined-only") == 0) {
            nm_undefined_only = 1;
        } else if (rt_strcmp(argv[argi], "-g") == 0 || rt_strcmp(argv[argi], "--extern-only") == 0) {
            nm_external_only = 1;
        } else if (rt_strcmp(argv[argi], "--json") == 0) {
            nm_json = 1;
            tool_json_set_enabled(1);
        } else {
            tool_write_error("nm", "unknown option: ", argv[argi]);
            return 1;
        }
        argi += 1;
    }
    if (argi >= argc) {
        print_usage();
        return 1;
    }
    for (; argi < argc; ++argi) {
        if (!nm_json && argc - argi > 1) {
            rt_write_cstr(1, argv[argi]);
            rt_write_line(1, ":");
        }
        if (nm_file(argv[argi]) != 0) {
            status = 1;
        }
    }
    return status;
}
