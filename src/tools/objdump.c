#include "platform.h"
#include "runtime.h"
#include "tool_util.h"
#include "archive_util.h"

#define OBJDUMP_MAX_SECTIONS 256U
#define OBJDUMP_NAME_TABLE_CAPACITY 65536U
#define OBJDUMP_MACHO_MAX_COMMANDS 256U

#define MACHO_LC_SEGMENT_64 0x19U
#define MACHO_LC_SYMTAB 0x2U
#define MACHO_ARM64_RELOC_UNSIGNED 0U
#define MACHO_ARM64_RELOC_SUBTRACTOR 1U
#define MACHO_ARM64_RELOC_BRANCH26 2U
#define MACHO_ARM64_RELOC_PAGE21 3U
#define MACHO_ARM64_RELOC_PAGEOFF12 4U
#define MACHO_ARM64_RELOC_GOT_LOAD_PAGE21 5U
#define MACHO_ARM64_RELOC_GOT_LOAD_PAGEOFF12 6U
#define MACHO_ARM64_RELOC_POINTER_TO_GOT 7U
#define MACHO_ARM64_RELOC_TLVP_LOAD_PAGE21 8U
#define MACHO_ARM64_RELOC_TLVP_LOAD_PAGEOFF12 9U
#define MACHO_ARM64_RELOC_ADDEND 10U
#define MACHO_S_ZEROFILL 1U
#define MACHO_S_CSTRING_LITERALS 2U
#define MACHO_S_4BYTE_LITERALS 3U
#define MACHO_S_8BYTE_LITERALS 4U
#define MACHO_S_SYMBOL_STUBS 8U
#define MACHO_S_GB_ZEROFILL 12U
#define MACHO_S_16BYTE_LITERALS 14U
#define MACHO_S_THREAD_LOCAL_ZEROFILL 18U
#define MACHO_FAT_MAGIC_LE 0xbebafecaU
#define MACHO_FAT_MAGIC_64_LE 0xbfbafecaU
#define MACHO_CPU_TYPE_ARM64 0x0100000cU

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

typedef struct {
    char segment[17];
    char section[17];
    unsigned long long addr;
    unsigned long long size;
    unsigned int offset;
    unsigned int reloff;
    unsigned int nreloc;
    unsigned int flags;
} MachSectionInfo;

typedef struct {
    unsigned int symoff;
    unsigned int nsyms;
    unsigned int stroff;
    unsigned int strsize;
} MachSymtabInfo;

typedef struct {
    unsigned short machine;
    unsigned short section_count;
    unsigned int timestamp;
    unsigned short optional_size;
    unsigned short characteristics;
    unsigned short optional_magic;
    unsigned short subsystem;
    unsigned long long entry;
    unsigned long long image_base;
} PeHeaderInfo;

typedef struct {
    char name[9];
    unsigned int virtual_size;
    unsigned int virtual_address;
    unsigned int raw_size;
    unsigned int raw_offset;
    unsigned int characteristics;
} PeSectionInfo;

static int objdump_json;
static unsigned long long objdump_object_base;

static int json_field_string(const char *name, const char *value) {
    if (rt_write_cstr(1, ",\"") != 0) return -1;
    if (rt_write_cstr(1, name) != 0) return -1;
    if (rt_write_cstr(1, "\":") != 0) return -1;
    return tool_json_write_string(1, value != 0 ? value : "");
}

static int json_field_uint(const char *name, unsigned long long value) {
    if (rt_write_cstr(1, ",\"") != 0) return -1;
    if (rt_write_cstr(1, name) != 0) return -1;
    if (rt_write_cstr(1, "\":") != 0) return -1;
    return rt_write_uint(1, value);
}

static int json_field_bool(const char *name, int value) {
    if (rt_write_cstr(1, ",\"") != 0) return -1;
    if (rt_write_cstr(1, name) != 0) return -1;
    if (rt_write_cstr(1, "\":") != 0) return -1;
    return rt_write_cstr(1, value ? "true" : "false");
}

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

static const char *macho_section_type_name(unsigned int type) {
    if (type == 0U) return "regular";
    if (type == MACHO_S_ZEROFILL) return "zerofill";
    if (type == MACHO_S_CSTRING_LITERALS) return "cstring";
    if (type == MACHO_S_4BYTE_LITERALS) return "literal4";
    if (type == MACHO_S_8BYTE_LITERALS) return "literal8";
    if (type == MACHO_S_SYMBOL_STUBS) return "stubs";
    if (type == MACHO_S_GB_ZEROFILL) return "gb-zerofill";
    if (type == MACHO_S_16BYTE_LITERALS) return "literal16";
    if (type == MACHO_S_THREAD_LOCAL_ZEROFILL) return "tlv-zerofill";
    return "other";
}

static const char *macho_arm64_relocation_name(unsigned int type) {
    if (type == MACHO_ARM64_RELOC_UNSIGNED) return "UNSIGNED";
    if (type == MACHO_ARM64_RELOC_SUBTRACTOR) return "SUBTRACTOR";
    if (type == MACHO_ARM64_RELOC_BRANCH26) return "BRANCH26";
    if (type == MACHO_ARM64_RELOC_PAGE21) return "PAGE21";
    if (type == MACHO_ARM64_RELOC_PAGEOFF12) return "PAGEOFF12";
    if (type == MACHO_ARM64_RELOC_GOT_LOAD_PAGE21) return "GOT_LOAD_PAGE21";
    if (type == MACHO_ARM64_RELOC_GOT_LOAD_PAGEOFF12) return "GOT_LOAD_PAGEOFF12";
    if (type == MACHO_ARM64_RELOC_POINTER_TO_GOT) return "POINTER_TO_GOT";
    if (type == MACHO_ARM64_RELOC_TLVP_LOAD_PAGE21) return "TLVP_LOAD_PAGE21";
    if (type == MACHO_ARM64_RELOC_TLVP_LOAD_PAGEOFF12) return "TLVP_LOAD_PAGEOFF12";
    if (type == MACHO_ARM64_RELOC_ADDEND) return "ADDEND";
    return "UNKNOWN";
}

static int macho_section_is_zerofill(const MachSectionInfo *section) {
    unsigned int type = section->flags & 0xffU;

    return type == MACHO_S_ZEROFILL || type == MACHO_S_GB_ZEROFILL || type == MACHO_S_THREAD_LOCAL_ZEROFILL;
}

static const char *pe_machine_name(unsigned short machine) {
    if (machine == 0x014cU) return "i386";
    if (machine == 0x8664U) return "x86-64";
    if (machine == 0xaa64U) return "aarch64";
    if (machine == 0x01c0U) return "arm";
    if (machine == 0x01c4U) return "armv7";
    return "unknown";
}

static const char *pe_subsystem_name(unsigned short subsystem) {
    if (subsystem == 1U) return "native";
    if (subsystem == 2U) return "windows-gui";
    if (subsystem == 3U) return "console";
    if (subsystem == 9U) return "windows-ce";
    if (subsystem == 10U) return "efi-application";
    if (subsystem == 11U) return "efi-boot-service-driver";
    if (subsystem == 12U) return "efi-runtime-driver";
    if (subsystem == 14U) return "xbox";
    return "unknown";
}

static const char *pe_type_name(unsigned short characteristics) {
    if ((characteristics & 0x2000U) != 0U) return "dll";
    if ((characteristics & 0x0002U) != 0U) return "executable";
    return "object";
}

static void copy_fixed_name(char *dest, size_t dest_size, const unsigned char *src, size_t src_size) {
    size_t i;

    if (dest_size == 0U) return;
    for (i = 0U; i + 1U < dest_size && i < src_size && src[i] != 0U; ++i) {
        unsigned char ch = src[i];
        dest[i] = (ch >= 32U && ch <= 126U) ? (char)ch : '?';
    }
    dest[i] = '\0';
}

static int read_region(int fd, unsigned long long offset, unsigned char *buffer, size_t size) {
    if (platform_seek(fd, (long long)(objdump_object_base + offset), PLATFORM_SEEK_SET) < 0) {
        return -1;
    }
    return archive_read_exact(fd, buffer, size);
}

static int read_file_region(int fd, unsigned long long offset, unsigned char *buffer, size_t size) {
    if (platform_seek(fd, (long long)offset, PLATFORM_SEEK_SET) < 0) return -1;
    return archive_read_exact(fd, buffer, size);
}

static unsigned int read_u32_be_local(const unsigned char *bytes) {
    return ((unsigned int)bytes[0] << 24U) |
           ((unsigned int)bytes[1] << 16U) |
           ((unsigned int)bytes[2] << 8U) |
           (unsigned int)bytes[3];
}

static int select_macho_fat_slice(int fd, unsigned long long *offset_out) {
    unsigned char header[8];
    unsigned int magic;
    unsigned int arch_count;
    unsigned int index;
    *offset_out = 0ULL;
    if (read_file_region(fd, 0ULL, header, sizeof(header)) != 0) return -1;
    magic = read_u32_le_local(header + 0);
    if (magic != MACHO_FAT_MAGIC_LE && magic != MACHO_FAT_MAGIC_64_LE) return -1;
    arch_count = read_u32_be_local(header + 4);
    if (arch_count > 32U) return -1;
    for (index = 0U; index < arch_count; ++index) {
        unsigned char raw[32];
        unsigned int entry_size = magic == MACHO_FAT_MAGIC_64_LE ? 32U : 20U;
        unsigned long long entry = 8ULL + (unsigned long long)index * (unsigned long long)entry_size;
        unsigned int cputype;
        if (read_file_region(fd, entry, raw, entry_size) != 0) return -1;
        cputype = read_u32_be_local(raw + 0);
        if (cputype == MACHO_CPU_TYPE_ARM64 || index == 0U) {
            *offset_out = magic == MACHO_FAT_MAGIC_64_LE ? (((unsigned long long)read_u32_be_local(raw + 8) << 32U) | (unsigned long long)read_u32_be_local(raw + 12)) : (unsigned long long)read_u32_be_local(raw + 8);
            if (cputype == MACHO_CPU_TYPE_ARM64) return 0;
        }
    }
    return *offset_out != 0ULL ? 0 : -1;
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

static int load_macho_sections(int fd, const MachHeaderInfo *header, MachSectionInfo *sections, unsigned int *section_count_out) {
    unsigned int command_index;
    unsigned long long command_offset = 32ULL;
    unsigned int section_count = 0U;

    *section_count_out = 0U;
    if (header->ncmds > OBJDUMP_MACHO_MAX_COMMANDS) {
        return -1;
    }

    for (command_index = 0U; command_index < header->ncmds; ++command_index) {
        unsigned char command_header[8];
        unsigned int command;
        unsigned int command_size;

        if (read_region(fd, command_offset, command_header, sizeof(command_header)) != 0) {
            return -1;
        }
        command = read_u32_le_local(command_header + 0);
        command_size = read_u32_le_local(command_header + 4);
        if (command_size < 8U) {
            return -1;
        }

        if (command == MACHO_LC_SEGMENT_64 && command_size >= 72U) {
            unsigned char segment[72];
            unsigned int nsects;
            unsigned int section_index;
            char segment_name[17];

            if (read_region(fd, command_offset, segment, sizeof(segment)) != 0) {
                return -1;
            }
            copy_fixed_name(segment_name, sizeof(segment_name), segment + 8, 16U);
            nsects = read_u32_le_local(segment + 64);
            if (72U + nsects * 80U > command_size) {
                return -1;
            }
            for (section_index = 0U; section_index < nsects; ++section_index) {
                unsigned char raw[80];
                unsigned long long section_offset = command_offset + 72ULL + ((unsigned long long)section_index * 80ULL);

                if (section_count >= OBJDUMP_MAX_SECTIONS) {
                    return -1;
                }
                if (read_region(fd, section_offset, raw, sizeof(raw)) != 0) {
                    return -1;
                }
                copy_fixed_name(sections[section_count].section, sizeof(sections[section_count].section), raw + 0, 16U);
                copy_fixed_name(sections[section_count].segment, sizeof(sections[section_count].segment), raw + 16, 16U);
                if (sections[section_count].segment[0] == '\0') {
                    rt_copy_string(sections[section_count].segment, sizeof(sections[section_count].segment), segment_name);
                }
                sections[section_count].addr = read_u64_le_local(raw + 32);
                sections[section_count].size = read_u64_le_local(raw + 40);
                sections[section_count].offset = read_u32_le_local(raw + 48);
                sections[section_count].reloff = read_u32_le_local(raw + 56);
                sections[section_count].nreloc = read_u32_le_local(raw + 60);
                sections[section_count].flags = read_u32_le_local(raw + 64);
                section_count += 1U;
            }
        }

        command_offset += (unsigned long long)command_size;
    }

    *section_count_out = section_count;
    return 0;
}

static int load_macho_symtab(int fd, const MachHeaderInfo *header, MachSymtabInfo *symtab) {
    unsigned int command_index;
    unsigned long long command_offset = 32ULL;

    symtab->symoff = 0U;
    symtab->nsyms = 0U;
    symtab->stroff = 0U;
    symtab->strsize = 0U;
    if (header->ncmds > OBJDUMP_MACHO_MAX_COMMANDS) {
        return -1;
    }

    for (command_index = 0U; command_index < header->ncmds; ++command_index) {
        unsigned char command_header[24];
        unsigned int command;
        unsigned int command_size;

        if (read_region(fd, command_offset, command_header, 8U) != 0) {
            return -1;
        }
        command = read_u32_le_local(command_header + 0);
        command_size = read_u32_le_local(command_header + 4);
        if (command_size < 8U) {
            return -1;
        }
        if (command == MACHO_LC_SYMTAB && command_size >= 24U) {
            if (read_region(fd, command_offset, command_header, sizeof(command_header)) != 0) {
                return -1;
            }
            symtab->symoff = read_u32_le_local(command_header + 8);
            symtab->nsyms = read_u32_le_local(command_header + 12);
            symtab->stroff = read_u32_le_local(command_header + 16);
            symtab->strsize = read_u32_le_local(command_header + 20);
            return 0;
        }
        command_offset += (unsigned long long)command_size;
    }
    return 0;
}

static int parse_pe_header(int fd, PeHeaderInfo *info) {
    unsigned char dos[64];
    unsigned char coff[24];
    unsigned int pe_offset;
    unsigned long long optional_offset;

    if (read_region(fd, 0ULL, dos, sizeof(dos)) != 0) {
        return -1;
    }
    if (dos[0] != 'M' || dos[1] != 'Z') {
        return -1;
    }
    pe_offset = read_u32_le_local(dos + 0x3cU);
    if (read_region(fd, (unsigned long long)pe_offset, coff, sizeof(coff)) != 0) {
        return -1;
    }
    if (!(coff[0] == 'P' && coff[1] == 'E' && coff[2] == 0U && coff[3] == 0U)) {
        return -1;
    }

    info->machine = read_u16_le(coff + 4);
    info->section_count = read_u16_le(coff + 6);
    info->timestamp = read_u32_le_local(coff + 8);
    info->optional_size = read_u16_le(coff + 20);
    info->characteristics = read_u16_le(coff + 22);
    info->optional_magic = 0U;
    info->subsystem = 0U;
    info->entry = 0ULL;
    info->image_base = 0ULL;

    optional_offset = (unsigned long long)pe_offset + 24ULL;
    if (info->optional_size >= 70U) {
        unsigned char optional[72];
        if (read_region(fd, optional_offset, optional, sizeof(optional)) != 0) {
            return -1;
        }
        info->optional_magic = read_u16_le(optional + 0);
        info->entry = (unsigned long long)read_u32_le_local(optional + 16);
        if (info->optional_magic == 0x020bU) {
            info->image_base = read_u64_le_local(optional + 24);
        } else if (info->optional_magic == 0x010bU) {
            info->image_base = (unsigned long long)read_u32_le_local(optional + 28);
        }
        info->subsystem = read_u16_le(optional + 68);
    } else if (info->optional_size >= 2U) {
        unsigned char magic[2];
        if (read_region(fd, optional_offset, magic, sizeof(magic)) != 0) {
            return -1;
        }
        info->optional_magic = read_u16_le(magic);
    }

    return 0;
}

static int load_pe_sections(int fd, const PeHeaderInfo *header, PeSectionInfo *sections) {
    unsigned char dos[64];
    unsigned int pe_offset;
    unsigned long long section_offset;
    unsigned short i;

    if (header->section_count > OBJDUMP_MAX_SECTIONS) {
        return -1;
    }
    if (read_region(fd, 0ULL, dos, sizeof(dos)) != 0) {
        return -1;
    }
    pe_offset = read_u32_le_local(dos + 0x3cU);
    section_offset = (unsigned long long)pe_offset + 24ULL + (unsigned long long)header->optional_size;

    for (i = 0U; i < header->section_count; ++i) {
        unsigned char raw[40];
        if (read_region(fd, section_offset + ((unsigned long long)i * 40ULL), raw, sizeof(raw)) != 0) {
            return -1;
        }
        copy_fixed_name(sections[i].name, sizeof(sections[i].name), raw + 0, 8U);
        sections[i].virtual_size = read_u32_le_local(raw + 8);
        sections[i].virtual_address = read_u32_le_local(raw + 12);
        sections[i].raw_size = read_u32_le_local(raw + 16);
        sections[i].raw_offset = read_u32_le_local(raw + 20);
        sections[i].characteristics = read_u32_le_local(raw + 36);
    }
    return 0;
}

static int load_sections(int fd, const ElfHeaderInfo *header, ElfSectionInfo *sections) {
    unsigned short i;
    unsigned char raw[64];

    if (header->shnum > OBJDUMP_MAX_SECTIONS) {
        return -1;
    }
    if (header->shnum == 0) {
        return 0;
    }
    if (header->shentsize < 64U) {
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

static void print_pe_file_header(const char *path, const PeHeaderInfo *header) {
    rt_write_cstr(1, "\n");
    rt_write_line(1, path);
    rt_write_cstr(1, "file format ");
    if (header->optional_magic == 0x020bU) rt_write_line(1, "pei-x86-64");
    else if (header->optional_magic == 0x010bU) rt_write_line(1, "pei-i386");
    else rt_write_line(1, "pe-coff");
    rt_write_cstr(1, "architecture: ");
    rt_write_cstr(1, pe_machine_name(header->machine));
    rt_write_cstr(1, ", type: ");
    rt_write_line(1, pe_type_name(header->characteristics));
    if (header->subsystem != 0U) {
        rt_write_cstr(1, "subsystem: ");
        rt_write_line(1, pe_subsystem_name(header->subsystem));
    }
    rt_write_cstr(1, "start address ");
    write_hex_value(header->entry);
    rt_write_cstr(1, ", image base ");
    write_hex_value(header->image_base);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "timestamp: ");
    rt_write_uint(1, (unsigned long long)header->timestamp);
    rt_write_cstr(1, ", characteristics: ");
    write_hex_value((unsigned long long)header->characteristics);
    rt_write_char(1, '\n');
}

static int json_file_header_event(const char *path, const char *format, const char *architecture, const char *type, unsigned long long entry, unsigned long long flags) {
    if (tool_json_begin_event(1, "objdump", "stdout", "file_header") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{\"file\":") != 0) return -1;
    if (tool_json_write_string(1, path) != 0) return -1;
    if (json_field_string("format", format) != 0) return -1;
    if (json_field_string("architecture", architecture) != 0) return -1;
    if (json_field_string("type", type) != 0) return -1;
    if (json_field_uint("entry", entry) != 0) return -1;
    if (json_field_uint("flags", flags) != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int json_section_event(const char *path, const char *format, unsigned int index, const char *name, unsigned long long addr, unsigned long long offset, unsigned long long size, const char *type, unsigned long long flags) {
    if (tool_json_begin_event(1, "objdump", "stdout", "section") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{\"file\":") != 0) return -1;
    if (tool_json_write_string(1, path) != 0) return -1;
    if (json_field_string("format", format) != 0) return -1;
    if (json_field_uint("index", index) != 0) return -1;
    if (json_field_string("name", name) != 0) return -1;
    if (json_field_uint("addr", addr) != 0) return -1;
    if (json_field_uint("offset", offset) != 0) return -1;
    if (json_field_uint("size", size) != 0) return -1;
    if (json_field_string("type", type) != 0) return -1;
    if (json_field_uint("flags", flags) != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int json_macho_section_table(const char *path, const MachSectionInfo *sections, unsigned int section_count) {
    unsigned int i;
    for (i = 0U; i < section_count; ++i) {
        char name[40];
        rt_copy_string(name, sizeof(name), sections[i].segment);
        if (rt_strlen(name) + 2U < sizeof(name)) {
            rt_copy_string(name + rt_strlen(name), sizeof(name) - rt_strlen(name), ",");
            rt_copy_string(name + rt_strlen(name), sizeof(name) - rt_strlen(name), sections[i].section);
        }
        if (json_section_event(path, "mach-o-64", i, name, sections[i].addr, (unsigned long long)sections[i].offset, sections[i].size, macho_section_type_name(sections[i].flags & 0xffU), (unsigned long long)sections[i].flags) != 0) return -1;
    }
    return 0;
}

static int json_elf_section_table(const char *path, const ElfHeaderInfo *header, const ElfSectionInfo *sections, const char *names, size_t names_size) {
    unsigned short i;
    for (i = 0U; i < header->shnum; ++i) {
        if (json_section_event(path, "elf64", i, name_from_table(names, names_size, sections[i].name), sections[i].addr, sections[i].offset, sections[i].size, section_type_name(sections[i].type), sections[i].flags) != 0) return -1;
    }
    return 0;
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

static void print_macho_section_table(const MachSectionInfo *sections, unsigned int section_count) {
    unsigned int i;

    rt_write_line(1, "Sections:");
    for (i = 0U; i < section_count; ++i) {
        rt_write_cstr(1, "  ");
        rt_write_uint(1, (unsigned long long)i);
        rt_write_cstr(1, " ");
        rt_write_cstr(1, sections[i].segment);
        rt_write_cstr(1, ",");
        rt_write_cstr(1, sections[i].section);
        rt_write_cstr(1, " addr=");
        write_hex_value(sections[i].addr);
        rt_write_cstr(1, " off=");
        write_hex_value((unsigned long long)sections[i].offset);
        rt_write_cstr(1, " size=");
        write_hex_value(sections[i].size);
        rt_write_cstr(1, " type=");
        rt_write_cstr(1, macho_section_type_name(sections[i].flags & 0xffU));
        rt_write_cstr(1, " flags=");
        write_hex_value((unsigned long long)sections[i].flags);
        rt_write_char(1, '\n');
    }
}

static void print_pe_section_table(const PeHeaderInfo *header, const PeSectionInfo *sections) {
    unsigned short i;

    rt_write_line(1, "Sections:");
    for (i = 0U; i < header->section_count; ++i) {
        rt_write_cstr(1, "  ");
        rt_write_uint(1, (unsigned long long)i);
        rt_write_cstr(1, " ");
        rt_write_cstr(1, sections[i].name);
        rt_write_cstr(1, " vaddr=");
        write_hex_value((unsigned long long)sections[i].virtual_address);
        rt_write_cstr(1, " vsize=");
        write_hex_value((unsigned long long)sections[i].virtual_size);
        rt_write_cstr(1, " off=");
        write_hex_value((unsigned long long)sections[i].raw_offset);
        rt_write_cstr(1, " size=");
        write_hex_value((unsigned long long)sections[i].raw_size);
        rt_write_cstr(1, " flags=");
        write_hex_value((unsigned long long)sections[i].characteristics);
        rt_write_char(1, '\n');
    }
}

static void dump_bytes_range(int fd, const char *name, unsigned long long offset, unsigned long long size, unsigned long long addr) {
    unsigned char buffer[16];
    unsigned long long remaining = size;
    unsigned long long current_offset = offset;
    unsigned long long display_addr = addr;

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

static void dump_section_bytes(int fd, const char *name, const ElfSectionInfo *section) {
    dump_bytes_range(fd, name, section->offset, section->size, section->addr);
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

static void print_macho_symbols(int fd, const MachSymtabInfo *symtab) {
    char strings[OBJDUMP_NAME_TABLE_CAPACITY];
    size_t string_size = 0U;
    unsigned int index;

    if (symtab->symoff == 0U || symtab->nsyms == 0U || symtab->stroff == 0U || symtab->strsize == 0U) {
        rt_write_line(1, "No Mach-O symbol table is available.");
        return;
    }

    if (symtab->strsize > 0U) {
        size_t to_read = symtab->strsize < (unsigned int)(sizeof(strings) - 1U) ? (size_t)symtab->strsize : sizeof(strings) - 1U;
        if (read_region(fd, (unsigned long long)symtab->stroff, (unsigned char *)strings, to_read) == 0) {
            strings[to_read] = '\0';
            string_size = to_read;
        }
    }

    rt_write_line(1, "SYMBOL TABLE: LC_SYMTAB");
    for (index = 0U; index < symtab->nsyms; ++index) {
        unsigned char entry[16];
        unsigned int strx;
        unsigned int type;
        unsigned int sect;
        unsigned long long value;
        const char *name;

        if (read_region(fd, (unsigned long long)symtab->symoff + ((unsigned long long)index * 16ULL), entry, sizeof(entry)) != 0) {
            break;
        }
        strx = read_u32_le_local(entry + 0);
        type = (unsigned int)entry[4];
        sect = (unsigned int)entry[5];
        value = read_u64_le_local(entry + 8);
        name = name_from_table(strings, string_size, strx);
        write_hex_value(value);
        rt_write_cstr(1, " type=");
        write_hex_value((unsigned long long)type);
        rt_write_cstr(1, " sect=");
        rt_write_uint(1, (unsigned long long)sect);
        rt_write_char(1, ' ');
        rt_write_line(1, name);
    }
}

static int json_symbol_event(const char *path, const char *format, unsigned long long value, const char *name, unsigned int type, unsigned int section) {
    if (tool_json_begin_event(1, "objdump", "stdout", "symbol") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{\"file\":") != 0) return -1;
    if (tool_json_write_string(1, path) != 0) return -1;
    if (json_field_string("format", format) != 0) return -1;
    if (json_field_string("name", name) != 0) return -1;
    if (json_field_uint("value", value) != 0) return -1;
    if (json_field_uint("type", type) != 0) return -1;
    if (json_field_uint("section", section) != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int json_macho_symbols(int fd, const char *path, const MachSymtabInfo *symtab) {
    char strings[OBJDUMP_NAME_TABLE_CAPACITY];
    size_t string_size = 0U;
    unsigned int index;

    if (symtab->symoff == 0U || symtab->nsyms == 0U || symtab->stroff == 0U || symtab->strsize == 0U) return 0;
    if (symtab->strsize > 0U) {
        size_t to_read = symtab->strsize < (unsigned int)(sizeof(strings) - 1U) ? (size_t)symtab->strsize : sizeof(strings) - 1U;
        if (read_region(fd, (unsigned long long)symtab->stroff, (unsigned char *)strings, to_read) == 0) {
            strings[to_read] = '\0';
            string_size = to_read;
        }
    }
    for (index = 0U; index < symtab->nsyms; ++index) {
        unsigned char entry[16];
        unsigned int strx;
        if (read_region(fd, (unsigned long long)symtab->symoff + ((unsigned long long)index * 16ULL), entry, sizeof(entry)) != 0) return -1;
        strx = read_u32_le_local(entry + 0);
        if (json_symbol_event(path, "mach-o-64", read_u64_le_local(entry + 8), name_from_table(strings, string_size, strx), (unsigned int)entry[4], (unsigned int)entry[5]) != 0) return -1;
    }
    return 0;
}

static const char *macho_symbol_name_at(int fd, const MachSymtabInfo *symtab, unsigned int symbol_index, char *strings, size_t string_size) {
    unsigned char entry[16];
    unsigned int strx;

    if (symtab == 0 || symbol_index >= symtab->nsyms || string_size == 0U) {
        return "";
    }
    if (read_region(fd, (unsigned long long)symtab->symoff + ((unsigned long long)symbol_index * 16ULL), entry, sizeof(entry)) != 0) {
        return "";
    }
    strx = read_u32_le_local(entry + 0);
    return name_from_table(strings, string_size, strx);
}

static void print_macho_relocations(int fd, const MachSectionInfo *sections, unsigned int section_count, const MachSymtabInfo *symtab) {
    char strings[OBJDUMP_NAME_TABLE_CAPACITY];
    size_t string_size = 0U;
    unsigned int section_index;
    int saw_relocations = 0;

    if (symtab != 0 && symtab->stroff != 0U && symtab->strsize != 0U) {
        size_t to_read = symtab->strsize < (unsigned int)(sizeof(strings) - 1U) ? (size_t)symtab->strsize : sizeof(strings) - 1U;
        if (read_region(fd, (unsigned long long)symtab->stroff, (unsigned char *)strings, to_read) == 0) {
            strings[to_read] = '\0';
            string_size = to_read;
        }
    }

    for (section_index = 0U; section_index < section_count; ++section_index) {
        const MachSectionInfo *section = &sections[section_index];
        unsigned int reloc_index;
        if (section->nreloc == 0U || section->reloff == 0U) {
            continue;
        }
        saw_relocations = 1;
        rt_write_cstr(1, "RELOCATION RECORDS FOR [");
        rt_write_cstr(1, section->segment);
        rt_write_char(1, ',');
        rt_write_cstr(1, section->section);
        rt_write_line(1, "]:");
        for (reloc_index = 0U; reloc_index < section->nreloc; ++reloc_index) {
            unsigned char raw[8];
            unsigned int address;
            unsigned int info;
            unsigned int symbolnum;
            unsigned int pcrel;
            unsigned int length;
            unsigned int external;
            unsigned int type;
            if (read_region(fd, (unsigned long long)section->reloff + ((unsigned long long)reloc_index * 8ULL), raw, sizeof(raw)) != 0) {
                break;
            }
            address = read_u32_le_local(raw + 0);
            info = read_u32_le_local(raw + 4);
            symbolnum = info & 0x00ffffffU;
            pcrel = (info >> 24U) & 1U;
            length = (info >> 25U) & 3U;
            external = (info >> 27U) & 1U;
            type = (info >> 28U) & 0x0fU;
            write_hex_value((unsigned long long)address);
            rt_write_cstr(1, " ");
            rt_write_cstr(1, macho_arm64_relocation_name(type));
            rt_write_cstr(1, " length=");
            rt_write_uint(1, (unsigned long long)length);
            rt_write_cstr(1, " pcrel=");
            rt_write_uint(1, (unsigned long long)pcrel);
            rt_write_cstr(1, " extern=");
            rt_write_uint(1, (unsigned long long)external);
            rt_write_cstr(1, " ");
            if (external) {
                const char *name = macho_symbol_name_at(fd, symtab, symbolnum, strings, string_size);
                if (name[0] != '\0') rt_write_cstr(1, name);
                else {
                    rt_write_cstr(1, "symbol[");
                    rt_write_uint(1, (unsigned long long)symbolnum);
                    rt_write_char(1, ']');
                }
            } else {
                rt_write_cstr(1, "section[");
                rt_write_uint(1, (unsigned long long)symbolnum);
                rt_write_char(1, ']');
            }
            rt_write_char(1, '\n');
        }
    }
    if (!saw_relocations) {
        rt_write_line(1, "No Mach-O relocations are available.");
    }
}

static int json_relocation_event(const char *path, const char *segment, const char *section, unsigned int offset, const char *type, unsigned int length, unsigned int pcrel, unsigned int external, const char *symbol, unsigned int section_ordinal) {
    if (tool_json_begin_event(1, "objdump", "stdout", "relocation") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{\"file\":") != 0) return -1;
    if (tool_json_write_string(1, path) != 0) return -1;
    if (json_field_string("format", "mach-o-64") != 0) return -1;
    if (json_field_string("segment", segment) != 0) return -1;
    if (json_field_string("section", section) != 0) return -1;
    if (json_field_uint("offset", offset) != 0) return -1;
    if (json_field_string("type", type) != 0) return -1;
    if (json_field_uint("length", length) != 0) return -1;
    if (json_field_bool("pcrel", pcrel != 0U) != 0) return -1;
    if (json_field_bool("external", external != 0U) != 0) return -1;
    if (external != 0U) {
        if (json_field_string("symbol", symbol) != 0) return -1;
    } else if (json_field_uint("section_ordinal", section_ordinal) != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int json_macho_relocations(int fd, const char *path, const MachSectionInfo *sections, unsigned int section_count, const MachSymtabInfo *symtab) {
    char strings[OBJDUMP_NAME_TABLE_CAPACITY];
    size_t string_size = 0U;
    unsigned int section_index;

    if (symtab != 0 && symtab->stroff != 0U && symtab->strsize != 0U) {
        size_t to_read = symtab->strsize < (unsigned int)(sizeof(strings) - 1U) ? (size_t)symtab->strsize : sizeof(strings) - 1U;
        if (read_region(fd, (unsigned long long)symtab->stroff, (unsigned char *)strings, to_read) == 0) {
            strings[to_read] = '\0';
            string_size = to_read;
        }
    }
    for (section_index = 0U; section_index < section_count; ++section_index) {
        unsigned int reloc_index;
        for (reloc_index = 0U; reloc_index < sections[section_index].nreloc; ++reloc_index) {
            unsigned char raw[8];
            unsigned int address;
            unsigned int info;
            unsigned int symbolnum;
            unsigned int external;
            unsigned int type;
            const char *symbol = "";
            if (read_region(fd, (unsigned long long)sections[section_index].reloff + ((unsigned long long)reloc_index * 8ULL), raw, sizeof(raw)) != 0) return -1;
            address = read_u32_le_local(raw + 0);
            info = read_u32_le_local(raw + 4);
            symbolnum = info & 0x00ffffffU;
            external = (info >> 27U) & 1U;
            type = (info >> 28U) & 0x0fU;
            if (external) symbol = macho_symbol_name_at(fd, symtab, symbolnum, strings, string_size);
            if (json_relocation_event(path, sections[section_index].segment, sections[section_index].section, address, macho_arm64_relocation_name(type), (info >> 25U) & 3U, (info >> 24U) & 1U, external, symbol, symbolnum) != 0) return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    int show_file = 0;
    int show_sections = 0;
    int show_contents = 0;
    int show_symbols = 0;
    int show_relocations = 0;
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
        } else if (rt_strcmp(argv[argi], "-r") == 0) {
            show_relocations = 1;
        } else if (rt_strcmp(argv[argi], "--json") == 0) {
            objdump_json = 1;
            tool_json_set_enabled(1);
        } else {
            tool_write_usage("objdump", "[-f] [-h] [-s] [-t] [-r] [--json] file ...");
            return 1;
        }
        argi += 1;
    }

    if (!show_file && !show_sections && !show_contents && !show_symbols && !show_relocations) {
        show_file = 1;
        show_sections = 1;
    }

    if (argi >= argc) {
        tool_write_usage("objdump", "[-f] [-h] [-s] [-t] [-r] [--json] file ...");
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        int fd = platform_open_read(argv[i]);
        ElfHeaderInfo header;
        MachHeaderInfo macho;
        MachSectionInfo macho_sections[OBJDUMP_MAX_SECTIONS];
        MachSymtabInfo macho_symtab;
        unsigned int macho_section_count = 0U;
        PeHeaderInfo pe;
        PeSectionInfo pe_sections[OBJDUMP_MAX_SECTIONS];
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
        objdump_object_base = 0ULL;

        if (parse_elf_header(fd, &header) != 0 || load_sections(fd, &header, sections) != 0 ||
            load_name_table(fd, &header, sections, names, sizeof(names), &names_size) != 0) {
            unsigned long long macho_slice_offset;
            if (select_macho_fat_slice(fd, &macho_slice_offset) == 0) {
                objdump_object_base = macho_slice_offset;
            }
            if (parse_macho_header(fd, &macho) == 0 && load_macho_sections(fd, &macho, macho_sections, &macho_section_count) == 0 && load_macho_symtab(fd, &macho, &macho_symtab) == 0) {
                if (show_file) {
                    if (objdump_json) (void)json_file_header_event(argv[i], "mach-o-64", macho_machine_name(macho.cputype), macho_type_name(macho.filetype), 0ULL, (unsigned long long)macho.flags);
                    else print_macho_file_header(argv[i], &macho);
                }
                if (show_sections) {
                    if (objdump_json) (void)json_macho_section_table(argv[i], macho_sections, macho_section_count);
                    else print_macho_section_table(macho_sections, macho_section_count);
                }
                if (show_contents && !objdump_json) {
                    unsigned int macho_index;
                    for (macho_index = 0U; macho_index < macho_section_count; ++macho_index) {
                        char section_name[40];
                        if (macho_section_is_zerofill(&macho_sections[macho_index]) || macho_sections[macho_index].offset == 0U) {
                            continue;
                        }
                        rt_copy_string(section_name, sizeof(section_name), macho_sections[macho_index].segment);
                        if (rt_strlen(section_name) + 2U < sizeof(section_name)) {
                            rt_copy_string(section_name + rt_strlen(section_name), sizeof(section_name) - rt_strlen(section_name), ",");
                            rt_copy_string(section_name + rt_strlen(section_name), sizeof(section_name) - rt_strlen(section_name), macho_sections[macho_index].section);
                        }
                        dump_bytes_range(fd, section_name, (unsigned long long)macho_sections[macho_index].offset, macho_sections[macho_index].size, macho_sections[macho_index].addr);
                    }
                }
                if (show_symbols) {
                    if (objdump_json) (void)json_macho_symbols(fd, argv[i], &macho_symtab);
                    else print_macho_symbols(fd, &macho_symtab);
                }
                if (show_relocations) {
                    if (objdump_json) (void)json_macho_relocations(fd, argv[i], macho_sections, macho_section_count, &macho_symtab);
                    else print_macho_relocations(fd, macho_sections, macho_section_count, &macho_symtab);
                }
                platform_close(fd);
                continue;
            }

            if (parse_pe_header(fd, &pe) == 0 && load_pe_sections(fd, &pe, pe_sections) == 0) {
                if (show_file) {
                    print_pe_file_header(argv[i], &pe);
                }
                if (show_sections) {
                    print_pe_section_table(&pe, pe_sections);
                }
                if (show_contents && !objdump_json) {
                    unsigned short pe_index;
                    for (pe_index = 0U; pe_index < pe.section_count; ++pe_index) {
                        if (pe_sections[pe_index].raw_size > 0U) {
                            dump_bytes_range(fd, pe_sections[pe_index].name,
                                             (unsigned long long)pe_sections[pe_index].raw_offset,
                                             (unsigned long long)pe_sections[pe_index].raw_size,
                                             pe.image_base + (unsigned long long)pe_sections[pe_index].virtual_address);
                        }
                    }
                }
                if (show_symbols) {
                    rt_write_line(1, "Symbol dumping for PE/COFF inputs is not implemented yet.");
                }
                if (show_relocations) {
                    rt_write_line(1, "Relocation dumping for PE/COFF inputs is not implemented yet.");
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
            if (objdump_json) (void)json_file_header_event(argv[i], "elf64", machine_name(header.machine), type_name(header.type), header.entry, 0ULL);
            else print_file_header(argv[i], &header);
        }
        if (show_sections) {
            if (objdump_json) (void)json_elf_section_table(argv[i], &header, sections, names, names_size);
            else print_section_table(&header, sections, names, names_size);
        }
        if (show_contents) {
            if (!objdump_json) for (section_index = 0U; section_index < header.shnum; ++section_index) {
                if (sections[section_index].size > 0ULL && sections[section_index].type != 8U) {
                    dump_section_bytes(fd, name_from_table(names, names_size, sections[section_index].name), &sections[section_index]);
                }
            }
        }
        if (show_symbols) {
            if (!objdump_json) print_symbols(fd, &header, sections, names, names_size);
        }
        if (show_relocations) {
            if (!objdump_json) rt_write_line(1, "Relocation dumping for ELF inputs is available in readelf -r.");
        }

        platform_close(fd);
    }

    return exit_code;
}
