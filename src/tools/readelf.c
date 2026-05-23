#include "platform.h"
#include "runtime.h"
#include "tool_util.h"
#include "archive_util.h"

#define READELF_MAX_SECTIONS 256U
#define READELF_MAX_PROGRAM_HEADERS 128U
#define READELF_NAME_TABLE_CAPACITY 65536U

#define ELF_SHT_DYNAMIC 6U
#define ELF_SHT_NOTE 7U
#define ELF_SHT_RELA 4U
#define ELF_SHT_REL 9U

#define ELF_PT_LOAD 1U
#define ELF_PT_DYNAMIC 2U
#define ELF_PT_INTERP 3U
#define ELF_PT_NOTE 4U
#define ELF_PT_PHDR 6U

#define ELF_DT_NULL 0LL
#define ELF_DT_NEEDED 1LL
#define ELF_DT_PLTRELSZ 2LL
#define ELF_DT_STRTAB 5LL
#define ELF_DT_SYMTAB 6LL
#define ELF_DT_RELA 7LL
#define ELF_DT_RELASZ 8LL
#define ELF_DT_RELAENT 9LL
#define ELF_DT_STRSZ 10LL
#define ELF_DT_SYMENT 11LL
#define ELF_DT_INIT 12LL
#define ELF_DT_FINI 13LL
#define ELF_DT_SONAME 14LL
#define ELF_DT_RPATH 15LL
#define ELF_DT_SYMBOLIC 16LL
#define ELF_DT_REL 17LL
#define ELF_DT_RELSZ 18LL
#define ELF_DT_RELENT 19LL
#define ELF_DT_PLTREL 20LL
#define ELF_DT_DEBUG 21LL
#define ELF_DT_TEXTREL 22LL
#define ELF_DT_JMPREL 23LL
#define ELF_DT_BIND_NOW 24LL
#define ELF_DT_INIT_ARRAY 25LL
#define ELF_DT_FINI_ARRAY 26LL
#define ELF_DT_INIT_ARRAYSZ 27LL
#define ELF_DT_FINI_ARRAYSZ 28LL
#define ELF_DT_RUNPATH 29LL
#define ELF_DT_FLAGS 30LL
#define ELF_DT_GNU_HASH 0x6ffffef5LL
#define ELF_DT_FLAGS_1 0x6ffffffbLL
#define ELF_DT_VERNEED 0x6ffffffeLL
#define ELF_DT_VERNEEDNUM 0x6fffffffLL

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
    unsigned char ident[16];
    unsigned short type;
    unsigned short machine;
    unsigned int version;
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

typedef struct {
    unsigned int type;
    unsigned int flags;
    unsigned long long offset;
    unsigned long long vaddr;
    unsigned long long paddr;
    unsigned long long filesz;
    unsigned long long memsz;
    unsigned long long align;
} ElfProgramInfo;

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

static const char *macho_type_name(unsigned int type) {
    if (type == 1U) return "OBJECT";
    if (type == 2U) return "EXECUTE";
    if (type == 6U) return "DYLIB";
    if (type == 8U) return "BUNDLE";
    return "UNKNOWN";
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

static const char *macho_machine_name(unsigned int cputype) {
    unsigned int family = cputype & 0x00ffffffU;

    if (family == 7U) return "Advanced Micro Devices X86-64";
    if (family == 12U) return "AArch64";
    return "Unknown";
}

static const char *elf_section_type_name(unsigned int type) {
    if (type == 0U) return "NULL";
    if (type == 1U) return "PROGBITS";
    if (type == 2U) return "SYMTAB";
    if (type == 3U) return "STRTAB";
    if (type == 4U) return "RELA";
    if (type == 5U) return "HASH";
    if (type == 6U) return "DYNAMIC";
    if (type == 7U) return "NOTE";
    if (type == 8U) return "NOBITS";
    if (type == 9U) return "REL";
    if (type == 11U) return "DYNSYM";
    if (type == 14U) return "INIT_ARRAY";
    if (type == 15U) return "FINI_ARRAY";
    if (type == 0x6ffffff6U) return "GNU_HASH";
    if (type == 0x6fffffffU) return "VERNEED";
    if (type == 0x6ffffffeU) return "VERSYM";
    return "OTHER";
}

static const char *elf_program_type_name(unsigned int type) {
    if (type == 0U) return "NULL";
    if (type == ELF_PT_LOAD) return "LOAD";
    if (type == ELF_PT_DYNAMIC) return "DYNAMIC";
    if (type == ELF_PT_INTERP) return "INTERP";
    if (type == ELF_PT_NOTE) return "NOTE";
    if (type == 5U) return "SHLIB";
    if (type == ELF_PT_PHDR) return "PHDR";
    if (type == 0x6474e550U) return "GNU_EH_FRAME";
    if (type == 0x6474e551U) return "GNU_STACK";
    if (type == 0x6474e552U) return "GNU_RELRO";
    if (type == 0x6474e553U) return "GNU_PROPERTY";
    return "OTHER";
}

static const char *elf_osabi_name(unsigned int osabi) {
    if (osabi == 0U) return "UNIX - System V";
    if (osabi == 3U) return "UNIX - GNU";
    if (osabi == 6U) return "UNIX - Solaris";
    if (osabi == 9U) return "FreeBSD";
    return "Unknown";
}

static const char *elf_dynamic_tag_name(long long tag) {
    if (tag == ELF_DT_NULL) return "NULL";
    if (tag == ELF_DT_NEEDED) return "NEEDED";
    if (tag == ELF_DT_PLTRELSZ) return "PLTRELSZ";
    if (tag == ELF_DT_STRTAB) return "STRTAB";
    if (tag == ELF_DT_SYMTAB) return "SYMTAB";
    if (tag == ELF_DT_RELA) return "RELA";
    if (tag == ELF_DT_RELASZ) return "RELASZ";
    if (tag == ELF_DT_RELAENT) return "RELAENT";
    if (tag == ELF_DT_STRSZ) return "STRSZ";
    if (tag == ELF_DT_SYMENT) return "SYMENT";
    if (tag == ELF_DT_INIT) return "INIT";
    if (tag == ELF_DT_FINI) return "FINI";
    if (tag == ELF_DT_SONAME) return "SONAME";
    if (tag == ELF_DT_RPATH) return "RPATH";
    if (tag == ELF_DT_SYMBOLIC) return "SYMBOLIC";
    if (tag == ELF_DT_REL) return "REL";
    if (tag == ELF_DT_RELSZ) return "RELSZ";
    if (tag == ELF_DT_RELENT) return "RELENT";
    if (tag == ELF_DT_PLTREL) return "PLTREL";
    if (tag == ELF_DT_DEBUG) return "DEBUG";
    if (tag == ELF_DT_TEXTREL) return "TEXTREL";
    if (tag == ELF_DT_JMPREL) return "JMPREL";
    if (tag == ELF_DT_BIND_NOW) return "BIND_NOW";
    if (tag == ELF_DT_INIT_ARRAY) return "INIT_ARRAY";
    if (tag == ELF_DT_FINI_ARRAY) return "FINI_ARRAY";
    if (tag == ELF_DT_INIT_ARRAYSZ) return "INIT_ARRAYSZ";
    if (tag == ELF_DT_FINI_ARRAYSZ) return "FINI_ARRAYSZ";
    if (tag == ELF_DT_RUNPATH) return "RUNPATH";
    if (tag == ELF_DT_FLAGS) return "FLAGS";
    if (tag == ELF_DT_GNU_HASH) return "GNU_HASH";
    if (tag == ELF_DT_FLAGS_1) return "FLAGS_1";
    if (tag == ELF_DT_VERNEED) return "VERNEED";
    if (tag == ELF_DT_VERNEEDNUM) return "VERNEEDNUM";
    return "OTHER";
}

static const char *elf_x86_64_relocation_name(unsigned int type) {
    if (type == 0U) return "R_X86_64_NONE";
    if (type == 1U) return "R_X86_64_64";
    if (type == 2U) return "R_X86_64_PC32";
    if (type == 6U) return "R_X86_64_GLOB_DAT";
    if (type == 7U) return "R_X86_64_JUMP_SLOT";
    if (type == 8U) return "R_X86_64_RELATIVE";
    if (type == 10U) return "R_X86_64_32";
    if (type == 11U) return "R_X86_64_32S";
    if (type == 42U) return "R_X86_64_REX_GOTPCRELX";
    return "R_X86_64_OTHER";
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

static void write_flags_rwx(unsigned int flags) {
    rt_write_char(1, (flags & 4U) != 0U ? 'R' : '-');
    rt_write_char(1, (flags & 2U) != 0U ? 'W' : '-');
    rt_write_char(1, (flags & 1U) != 0U ? 'E' : '-');
}

static void write_section_flags(unsigned long long flags) {
    if ((flags & 1ULL) != 0ULL) rt_write_char(1, 'W');
    if ((flags & 2ULL) != 0ULL) rt_write_char(1, 'A');
    if ((flags & 4ULL) != 0ULL) rt_write_char(1, 'X');
    if ((flags & 0x10ULL) != 0ULL) rt_write_char(1, 'M');
    if ((flags & 0x20ULL) != 0ULL) rt_write_char(1, 'S');
    if ((flags & 0x40ULL) != 0ULL) rt_write_char(1, 'I');
    if ((flags & 0x80ULL) != 0ULL) rt_write_char(1, 'L');
    if ((flags & 0x100ULL) != 0ULL) rt_write_char(1, 'O');
    if ((flags & 0x200ULL) != 0ULL) rt_write_char(1, 'G');
    if ((flags & 0x400ULL) != 0ULL) rt_write_char(1, 'T');
    if (flags == 0ULL) rt_write_char(1, '-');
}

static unsigned long long align4_u64(unsigned long long value) {
    return (value + 3ULL) & ~3ULL;
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

    for (size_t i = 0U; i < sizeof(info->ident); ++i) {
        info->ident[i] = header[i];
    }
    info->type = read_u16_le(header + 16);
    info->machine = read_u16_le(header + 18);
    info->version = read_u32_le_local(header + 20);
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

static int load_program_headers(int fd, const ElfHeaderInfo *header, ElfProgramInfo *programs) {
    unsigned char raw[56];
    unsigned short i;

    if (header->phnum == 0U) {
        return 0;
    }
    if (header->phnum > READELF_MAX_PROGRAM_HEADERS || header->phentsize < 56U) {
        return -1;
    }
    for (i = 0U; i < header->phnum; ++i) {
        unsigned long long offset = header->phoff + ((unsigned long long)i * (unsigned long long)header->phentsize);
        if (read_region(fd, offset, raw, sizeof(raw)) != 0) {
            return -1;
        }
        programs[i].type = read_u32_le_local(raw + 0);
        programs[i].flags = read_u32_le_local(raw + 4);
        programs[i].offset = read_u64_le_local(raw + 8);
        programs[i].vaddr = read_u64_le_local(raw + 16);
        programs[i].paddr = read_u64_le_local(raw + 24);
        programs[i].filesz = read_u64_le_local(raw + 32);
        programs[i].memsz = read_u64_le_local(raw + 40);
        programs[i].align = read_u64_le_local(raw + 48);
    }
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
    unsigned char raw[64];
    unsigned short i;

    if (header->shnum == 0U) {
        return 0;
    }
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
    size_t i;

    rt_write_line(1, "ELF Header:");
    rt_write_cstr(1, "  Magic: ");
    for (i = 0U; i < sizeof(info->ident); ++i) {
        write_hex_value((unsigned long long)info->ident[i]);
        if (i + 1U < sizeof(info->ident)) {
            rt_write_char(1, ' ');
        }
    }
    rt_write_char(1, '\n');
    rt_write_line(1, "  Class: ELF64");
    rt_write_line(1, "  Data: 2's complement, little endian");
    rt_write_cstr(1, "  Version: ");
    rt_write_uint(1, (unsigned long long)info->ident[6]);
    rt_write_line(1, " (current)");
    rt_write_cstr(1, "  OS/ABI: ");
    rt_write_line(1, elf_osabi_name((unsigned int)info->ident[7]));
    rt_write_cstr(1, "  ABI Version: ");
    rt_write_uint(1, (unsigned long long)info->ident[8]);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  Type: ");
    rt_write_line(1, elf_type_name(info->type));
    rt_write_cstr(1, "  Machine: ");
    rt_write_line(1, elf_machine_name(info->machine));
    rt_write_cstr(1, "  Object file version: ");
    write_hex_value((unsigned long long)info->version);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  Entry point: ");
    write_hex_value(info->entry);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  Program headers: ");
    rt_write_uint(1, info->phnum);
    rt_write_cstr(1, " at ");
    write_hex_value(info->phoff);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  Program header offset: ");
    write_hex_value(info->phoff);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  Flags: ");
    write_hex_value((unsigned long long)info->flags);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  ELF header size: ");
    rt_write_uint(1, info->ehsize);
    rt_write_line(1, " bytes");
    rt_write_cstr(1, "  Program header entry size: ");
    rt_write_uint(1, info->phentsize);
    rt_write_line(1, " bytes");
    rt_write_cstr(1, "  Program header count: ");
    rt_write_uint(1, info->phnum);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  Section headers: ");
    rt_write_uint(1, info->shnum);
    rt_write_cstr(1, " at ");
    write_hex_value(info->shoff);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  Section header offset: ");
    write_hex_value(info->shoff);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  Section header entry size: ");
    rt_write_uint(1, info->shentsize);
    rt_write_line(1, " bytes");
    rt_write_cstr(1, "  Section header count: ");
    rt_write_uint(1, info->shnum);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  Section header string table index: ");
    rt_write_uint(1, info->shstrndx);
    rt_write_char(1, '\n');
}

static void print_macho_header(const MachHeaderInfo *info) {
    rt_write_line(1, "Mach-O Header:");
    rt_write_cstr(1, "  Type: ");
    rt_write_line(1, macho_type_name(info->filetype));
    rt_write_cstr(1, "  Machine: ");
    rt_write_line(1, macho_machine_name(info->cputype));
    rt_write_cstr(1, "  Load commands: ");
    rt_write_uint(1, (unsigned long long)info->ncmds);
    rt_write_cstr(1, " (");
    rt_write_uint(1, (unsigned long long)info->sizeofcmds);
    rt_write_line(1, " bytes)");
    rt_write_cstr(1, "  Flags: ");
    write_hex_value((unsigned long long)info->flags);
    rt_write_char(1, '\n');
}

static void print_sections(const ElfHeaderInfo *header, const ElfSectionInfo *sections, const char *names, size_t names_size) {
    unsigned short i;
    (void)header;

    rt_write_line(1, "Section Headers:");
    if (header->shnum == 0U) {
        rt_write_line(1, "  (none)");
        return;
    }
    for (i = 0U; i < header->shnum; ++i) {
        const char *name = name_from_table(names, names_size, sections[i].name);
        rt_write_cstr(1, "  [");
        rt_write_uint(1, i);
        rt_write_cstr(1, "] ");
        rt_write_cstr(1, name);
        rt_write_cstr(1, " type=");
        rt_write_cstr(1, elf_section_type_name(sections[i].type));
        rt_write_cstr(1, " addr=");
        write_hex_value(sections[i].addr);
        rt_write_cstr(1, " off=");
        write_hex_value(sections[i].offset);
        rt_write_cstr(1, " size=");
        write_hex_value(sections[i].size);
        rt_write_cstr(1, " entsize=");
        write_hex_value(sections[i].entsize);
        rt_write_cstr(1, " flags=");
        write_section_flags(sections[i].flags);
        rt_write_cstr(1, " link=");
        rt_write_uint(1, sections[i].link);
        rt_write_char(1, '\n');
    }
}

static void print_program_headers(int fd, const ElfHeaderInfo *header, const ElfProgramInfo *programs) {
    unsigned short i;

    rt_write_line(1, "Program Headers:");
    if (header->phnum == 0U) {
        rt_write_line(1, "  (none)");
        return;
    }
    for (i = 0U; i < header->phnum; ++i) {
        rt_write_cstr(1, "  ");
        rt_write_cstr(1, elf_program_type_name(programs[i].type));
        rt_write_cstr(1, " off=");
        write_hex_value(programs[i].offset);
        rt_write_cstr(1, " vaddr=");
        write_hex_value(programs[i].vaddr);
        rt_write_cstr(1, " paddr=");
        write_hex_value(programs[i].paddr);
        rt_write_cstr(1, " filesz=");
        write_hex_value(programs[i].filesz);
        rt_write_cstr(1, " memsz=");
        write_hex_value(programs[i].memsz);
        rt_write_cstr(1, " flags=");
        write_flags_rwx(programs[i].flags);
        rt_write_cstr(1, " align=");
        write_hex_value(programs[i].align);
        if (programs[i].type == ELF_PT_INTERP && programs[i].filesz > 0ULL && programs[i].filesz < 256ULL) {
            char interp[256];
            size_t to_read = (size_t)programs[i].filesz;
            if (read_region(fd, programs[i].offset, (unsigned char *)interp, to_read) == 0) {
                interp[to_read < sizeof(interp) ? to_read : sizeof(interp) - 1U] = '\0';
                rt_write_cstr(1, " interp=");
                rt_write_cstr(1, interp);
            }
        }
        rt_write_char(1, '\n');
    }
}

static size_t load_section_text(int fd, const ElfSectionInfo *section, char *buffer, size_t buffer_capacity) {
    size_t to_read;

    if (buffer_capacity == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    to_read = (size_t)(section->size < (unsigned long long)(buffer_capacity - 1U) ? section->size : (unsigned long long)(buffer_capacity - 1U));
    if (to_read == 0U) {
        return 0U;
    }
    if (read_region(fd, section->offset, (unsigned char *)buffer, to_read) != 0) {
        return 0U;
    }
    buffer[to_read] = '\0';
    return to_read;
}

static const char *dynamic_string_at(const char *strings, size_t strings_size, unsigned long long offset) {
    if (offset >= (unsigned long long)strings_size) {
        return "";
    }
    return strings + offset;
}

static void print_dynamic(int fd, const ElfHeaderInfo *header, const ElfSectionInfo *sections, const char *section_names, size_t section_names_size) {
    unsigned short i;
    int found = 0;

    for (i = 0U; i < header->shnum; ++i) {
        if (sections[i].type == ELF_SHT_DYNAMIC) {
            const ElfSectionInfo *dynamic = &sections[i];
            char strings[READELF_NAME_TABLE_CAPACITY];
            size_t strings_size = 0U;
            unsigned long long count;
            unsigned long long index;
            unsigned char entry[16];

            found = 1;
            if (dynamic->link < header->shnum) {
                strings_size = load_section_text(fd, &sections[dynamic->link], strings, sizeof(strings));
            }
            rt_write_cstr(1, "Dynamic section '");
            rt_write_cstr(1, name_from_table(section_names, section_names_size, dynamic->name));
            rt_write_line(1, "':");
            count = dynamic->entsize != 0ULL ? dynamic->size / dynamic->entsize : dynamic->size / 16ULL;
            for (index = 0ULL; index < count; ++index) {
                long long tag;
                unsigned long long value;

                if (read_region(fd, dynamic->offset + (index * (dynamic->entsize != 0ULL ? dynamic->entsize : 16ULL)), entry, sizeof(entry)) != 0) {
                    break;
                }
                tag = (long long)read_u64_le_local(entry + 0);
                value = read_u64_le_local(entry + 8);
                rt_write_cstr(1, "  ");
                rt_write_cstr(1, elf_dynamic_tag_name(tag));
                rt_write_cstr(1, " ");
                if ((tag == ELF_DT_NEEDED || tag == ELF_DT_SONAME || tag == ELF_DT_RPATH || tag == ELF_DT_RUNPATH) && strings_size != 0U) {
                    rt_write_cstr(1, dynamic_string_at(strings, strings_size, value));
                } else {
                    write_hex_value(value);
                }
                rt_write_char(1, '\n');
            }
        }
    }
    if (!found) {
        rt_write_line(1, "There is no dynamic section in this file.");
    }
}

static const char *symbol_name_from_table(int fd,
                                         const ElfHeaderInfo *header,
                                         const ElfSectionInfo *sections,
                                         unsigned int symtab_index,
                                         unsigned long long symbol_index,
                                         char *strings,
                                         size_t strings_size,
                                         unsigned char *symbol_entry) {
    const ElfSectionInfo *symtab;
    unsigned int name_offset;

    if (symtab_index >= header->shnum) {
        return "";
    }
    symtab = &sections[symtab_index];
    if (symtab->entsize == 0ULL || symbol_index >= symtab->size / symtab->entsize) {
        return "";
    }
    if (read_region(fd, symtab->offset + (symbol_index * symtab->entsize), symbol_entry, 24U) != 0) {
        return "";
    }
    name_offset = read_u32_le_local(symbol_entry + 0);
    (void)strings;
    return name_from_table(strings, strings_size, name_offset);
}

static void print_relocations(int fd, const ElfHeaderInfo *header, const ElfSectionInfo *sections, const char *section_names, size_t section_names_size) {
    unsigned short i;
    int found = 0;

    for (i = 0U; i < header->shnum; ++i) {
        if (sections[i].type == ELF_SHT_RELA || sections[i].type == ELF_SHT_REL) {
            const ElfSectionInfo *reloc = &sections[i];
            char strings[READELF_NAME_TABLE_CAPACITY];
            size_t strings_size = 0U;
            unsigned long long entry_size = reloc->entsize != 0ULL ? reloc->entsize : (reloc->type == ELF_SHT_RELA ? 24ULL : 16ULL);
            unsigned long long count = entry_size != 0ULL ? reloc->size / entry_size : 0ULL;
            unsigned long long index;
            unsigned char entry[24];
            unsigned char symbol_entry[24];

            found = 1;
            if (reloc->link < header->shnum && sections[reloc->link].link < header->shnum) {
                strings_size = load_section_text(fd, &sections[sections[reloc->link].link], strings, sizeof(strings));
            } else {
                strings[0] = '\0';
            }
            rt_write_cstr(1, "Relocation section '");
            rt_write_cstr(1, name_from_table(section_names, section_names_size, reloc->name));
            rt_write_line(1, "':");
            for (index = 0ULL; index < count; ++index) {
                unsigned long long offset;
                unsigned long long info;
                unsigned long long symbol_index;
                unsigned int type;
                long long addend = 0LL;
                const char *symbol_name;

                if (read_region(fd, reloc->offset + (index * entry_size), entry, (size_t)(entry_size < sizeof(entry) ? entry_size : sizeof(entry))) != 0) {
                    break;
                }
                offset = read_u64_le_local(entry + 0);
                info = read_u64_le_local(entry + 8);
                symbol_index = info >> 32;
                type = (unsigned int)(info & 0xffffffffULL);
                if (reloc->type == ELF_SHT_RELA) {
                    addend = (long long)read_u64_le_local(entry + 16);
                }
                symbol_name = symbol_name_from_table(fd, header, sections, reloc->link, symbol_index, strings, strings_size, symbol_entry);
                rt_write_cstr(1, "  offset=");
                write_hex_value(offset);
                rt_write_cstr(1, " type=");
                if (header->machine == 62U) {
                    rt_write_cstr(1, elf_x86_64_relocation_name(type));
                } else {
                    rt_write_uint(1, type);
                }
                rt_write_cstr(1, " sym=");
                if (symbol_name[0] != '\0') {
                    rt_write_cstr(1, symbol_name);
                } else {
                    rt_write_uint(1, symbol_index);
                }
                if (reloc->type == ELF_SHT_RELA) {
                    rt_write_cstr(1, " addend=");
                    rt_write_int(1, addend);
                }
                rt_write_char(1, '\n');
            }
        }
    }
    if (!found) {
        rt_write_line(1, "There are no relocations in this file.");
    }
}

static const char *note_type_name(const char *owner, unsigned int type) {
    if (rt_strcmp(owner, "GNU") == 0) {
        if (type == 1U) return "NT_GNU_ABI_TAG";
        if (type == 3U) return "NT_GNU_BUILD_ID";
        if (type == 5U) return "NT_GNU_PROPERTY_TYPE_0";
    }
    return "NOTE";
}

static void print_notes(int fd, const ElfHeaderInfo *header, const ElfSectionInfo *sections, const char *section_names, size_t section_names_size) {
    unsigned short i;
    int found = 0;

    for (i = 0U; i < header->shnum; ++i) {
        if (sections[i].type == ELF_SHT_NOTE) {
            unsigned char buffer[READELF_NAME_TABLE_CAPACITY];
            unsigned long long offset = 0ULL;
            size_t loaded;

            found = 1;
            loaded = (size_t)(sections[i].size < (unsigned long long)sizeof(buffer) ? sections[i].size : (unsigned long long)sizeof(buffer));
            if (loaded != 0U && read_region(fd, sections[i].offset, buffer, loaded) != 0) {
                continue;
            }
            rt_write_cstr(1, "Notes in section '");
            rt_write_cstr(1, name_from_table(section_names, section_names_size, sections[i].name));
            rt_write_line(1, "':");
            while (offset + 12ULL <= (unsigned long long)loaded) {
                unsigned int namesz = read_u32_le_local(buffer + offset + 0ULL);
                unsigned int descsz = read_u32_le_local(buffer + offset + 4ULL);
                unsigned int type = read_u32_le_local(buffer + offset + 8ULL);
                unsigned long long name_offset = offset + 12ULL;
                unsigned long long desc_offset = name_offset + align4_u64((unsigned long long)namesz);
                char owner[32];
                size_t owner_len;
                size_t copy_index;

                if (desc_offset + align4_u64((unsigned long long)descsz) > (unsigned long long)loaded) {
                    break;
                }
                owner_len = namesz < sizeof(owner) ? (size_t)namesz : sizeof(owner) - 1U;
                for (copy_index = 0U; copy_index < owner_len; ++copy_index) {
                    owner[copy_index] = (char)buffer[name_offset + copy_index];
                    if (owner[copy_index] == '\0') {
                        break;
                    }
                }
                owner[owner_len] = '\0';
                rt_write_cstr(1, "  owner=");
                rt_write_cstr(1, owner);
                rt_write_cstr(1, " type=");
                rt_write_cstr(1, note_type_name(owner, type));
                rt_write_cstr(1, " descsz=");
                rt_write_uint(1, descsz);
                rt_write_char(1, '\n');
                offset = desc_offset + align4_u64((unsigned long long)descsz);
            }
        }
    }
    if (!found) {
        rt_write_line(1, "No notes found in this file.");
    }
}

static void print_symbols(int fd, const ElfHeaderInfo *header, const ElfSectionInfo *sections, const char *section_names, size_t section_names_size) {
    unsigned short i;
    int found = 0;
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
            found = 1;

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
    if (!found) {
        rt_write_line(1, "No symbol table is available.");
    }
}

int main(int argc, char **argv) {
    int show_header_flag = 0;
    int show_programs_flag = 0;
    int show_sections_flag = 0;
    int show_dynamic_flag = 0;
    int show_relocations_flag = 0;
    int show_symbols_flag = 0;
    int show_notes_flag = 0;
    int argi = 1;
    int exit_code = 0;
    int i;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--file-header") == 0) {
            show_header_flag = 1;
        } else if (rt_strcmp(argv[argi], "-l") == 0 || rt_strcmp(argv[argi], "-lW") == 0 || rt_strcmp(argv[argi], "--program-headers") == 0) {
            show_programs_flag = 1;
        } else if (rt_strcmp(argv[argi], "-S") == 0 || rt_strcmp(argv[argi], "--sections") == 0) {
            show_sections_flag = 1;
        } else if (rt_strcmp(argv[argi], "-d") == 0 || rt_strcmp(argv[argi], "--dynamic") == 0) {
            show_dynamic_flag = 1;
        } else if (rt_strcmp(argv[argi], "-r") == 0 || rt_strcmp(argv[argi], "--relocs") == 0 || rt_strcmp(argv[argi], "--relocations") == 0) {
            show_relocations_flag = 1;
        } else if (rt_strcmp(argv[argi], "-s") == 0 || rt_strcmp(argv[argi], "--symbols") == 0) {
            show_symbols_flag = 1;
        } else if (rt_strcmp(argv[argi], "-n") == 0 || rt_strcmp(argv[argi], "--notes") == 0) {
            show_notes_flag = 1;
        } else if (rt_strcmp(argv[argi], "-a") == 0 || rt_strcmp(argv[argi], "-all") == 0 || rt_strcmp(argv[argi], "--all") == 0) {
            show_header_flag = 1;
            show_programs_flag = 1;
            show_sections_flag = 1;
            show_dynamic_flag = 1;
            show_relocations_flag = 1;
            show_symbols_flag = 1;
            show_notes_flag = 1;
        } else if (rt_strcmp(argv[argi], "-W") == 0 || rt_strcmp(argv[argi], "--wide") == 0) {
        } else {
            tool_write_usage("readelf", "[-a] [-h] [-l] [-S] [-d] [-r] [-s] [-n] file ...");
            return 1;
        }
        argi += 1;
    }

    if (!show_header_flag && !show_programs_flag && !show_sections_flag && !show_dynamic_flag && !show_relocations_flag && !show_symbols_flag && !show_notes_flag) {
        show_header_flag = 1;
    }

    if (argi >= argc) {
        tool_write_usage("readelf", "[-a] [-h] [-l] [-S] [-d] [-r] [-s] [-n] file ...");
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        int fd = platform_open_read(argv[i]);
        ElfHeaderInfo header;
        MachHeaderInfo macho;
        ElfProgramInfo programs[READELF_MAX_PROGRAM_HEADERS];
        ElfSectionInfo sections[READELF_MAX_SECTIONS];
        char names[READELF_NAME_TABLE_CAPACITY];
        size_t names_size = 0U;

        if (fd < 0) {
            rt_write_cstr(2, "readelf: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (parse_elf_header(fd, &header) != 0 || load_program_headers(fd, &header, programs) != 0 || load_sections(fd, &header, sections) != 0 ||
            load_name_table(fd, &header, sections, names, sizeof(names), &names_size) != 0) {
            if (parse_macho_header(fd, &macho) == 0) {
                rt_write_cstr(1, "File: ");
                rt_write_line(1, argv[i]);
                if (show_header_flag) {
                    print_macho_header(&macho);
                }
                if (show_programs_flag) {
                    rt_write_line(1, "Program header dumping for Mach-O inputs is not implemented yet.");
                }
                if (show_sections_flag) {
                    rt_write_line(1, "Section header dumping for Mach-O inputs is not implemented yet.");
                }
                if (show_dynamic_flag) {
                    rt_write_line(1, "Dynamic-section dumping for Mach-O inputs is not implemented yet.");
                }
                if (show_relocations_flag) {
                    rt_write_line(1, "Relocation dumping for Mach-O inputs is not implemented yet.");
                }
                if (show_symbols_flag) {
                    rt_write_line(1, "Symbol dumping for Mach-O inputs is not implemented yet.");
                }
                if (show_notes_flag) {
                    rt_write_line(1, "Note dumping for Mach-O inputs is not implemented yet.");
                }
                platform_close(fd);
                continue;
            }

            rt_write_cstr(2, "readelf: unsupported or invalid object file ");
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
        if (show_programs_flag) {
            print_program_headers(fd, &header, programs);
        }
        if (show_sections_flag) {
            print_sections(&header, sections, names, names_size);
        }
        if (show_dynamic_flag) {
            print_dynamic(fd, &header, sections, names, names_size);
        }
        if (show_relocations_flag) {
            print_relocations(fd, &header, sections, names, names_size);
        }
        if (show_symbols_flag) {
            print_symbols(fd, &header, sections, names, names_size);
        }
        if (show_notes_flag) {
            print_notes(fd, &header, sections, names, names_size);
        }
        platform_close(fd);
    }

    return exit_code;
}
