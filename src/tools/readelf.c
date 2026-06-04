#include "platform.h"
#include "runtime.h"
#include "tool_util.h"
#include "archive_util.h"
#include "crypto/sha256.h"

#define READELF_MAX_SECTIONS 256U
#define READELF_MAX_PROGRAM_HEADERS 128U
#define READELF_NAME_TABLE_CAPACITY 65536U
#define READELF_MAX_MACHO_COMMANDS 256U
#define READELF_MAX_MACHO_SEGMENTS 128U

#define ELF_SHT_DYNAMIC 6U
#define ELF_SHT_NOTE 7U
#define ELF_SHT_RELA 4U
#define ELF_SHT_REL 9U

#define ELF_PT_LOAD 1U
#define ELF_PT_DYNAMIC 2U
#define ELF_PT_INTERP 3U
#define ELF_PT_NOTE 4U
#define ELF_PT_PHDR 6U

#define MACHO_LC_SEGMENT_64 0x19U
#define MACHO_LC_SYMTAB 0x2U
#define MACHO_LC_LOAD_DYLINKER 0xeU
#define MACHO_LC_CODE_SIGNATURE 0x1dU
#define MACHO_LC_MAIN 0x80000028U
#define MACHO_LC_BUILD_VERSION 0x32U
#define MACHO_LC_LOAD_DYLIB 0xcU
#define MACHO_LC_ID_DYLIB 0xdU
#define MACHO_LC_DYLD_INFO 0x22U
#define MACHO_LC_DYLD_INFO_ONLY 0x80000022U
#define MACHO_LC_DYLD_CHAINED_FIXUPS 0x80000034U

#define MACHO_CODE_SIGNATURE_SUPERBLOB 0xfade0cc0U
#define MACHO_CODE_SIGNATURE_CODEDIRECTORY 0xfade0c02U
#define MACHO_CODE_SIGNATURE_SLOT_CODEDIRECTORY 0U
#define MACHO_CODE_SIGNATURE_HASH_SHA256 2U

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

typedef struct {
    char segment[17];
    char section[17];
    unsigned long long addr;
    unsigned long long size;
    unsigned int offset;
    unsigned int align;
    unsigned int reloff;
    unsigned int nreloc;
    unsigned int flags;
} MachSectionInfo;

typedef struct {
    char name[17];
    unsigned long long vmaddr;
    unsigned long long vmsize;
    unsigned long long fileoff;
    unsigned long long filesize;
    unsigned int maxprot;
    unsigned int initprot;
    unsigned int nsects;
    unsigned int flags;
} MachSegmentInfo;

typedef struct {
    unsigned int symoff;
    unsigned int nsyms;
    unsigned int stroff;
    unsigned int strsize;
} MachSymtabInfo;

typedef struct {
    unsigned int dataoff;
    unsigned int datasize;
    unsigned int superblob_magic;
    unsigned int superblob_length;
    unsigned int superblob_count;
    unsigned int code_directory_offset;
    unsigned int code_directory_length;
    unsigned int code_directory_version;
    unsigned int code_directory_flags;
    unsigned int hash_offset;
    unsigned int ident_offset;
    unsigned int n_special_slots;
    unsigned int n_code_slots;
    unsigned int code_limit;
    unsigned int hash_size;
    unsigned int hash_type;
    unsigned int page_size_log2;
    unsigned int hash_slots_checked;
    unsigned int hash_mismatches;
    int present;
    int has_code_directory;
    int structure_valid;
    int hashes_verified;
    char identifier[128];
    char message[160];
} MachCodeSignatureInfo;

typedef struct {
    char format[16];
    char sha256[65];
    unsigned long long entry;
    unsigned int machine;
    unsigned int type;
    unsigned int flags;
    unsigned int segment_count;
    unsigned int section_count;
    unsigned int symbol_count;
    unsigned int relocation_count;
    int code_signature_present;
    int code_signature_verified;
} BinaryCompareSummary;

static int readelf_json;

static int read_region(int fd, unsigned long long offset, unsigned char *buffer, size_t size);
static const char *macho_symbol_name_at(int fd, const MachSymtabInfo *symtab, unsigned int symbol_index, char *strings, size_t string_size);

static unsigned short read_u16_le(const unsigned char *bytes) {
    return (unsigned short)bytes[0] | (unsigned short)((unsigned short)bytes[1] << 8);
}

static unsigned int read_u32_le_local(const unsigned char *bytes) {
    return archive_read_u32_le(bytes);
}

static unsigned int read_u32_be_local(const unsigned char *bytes) {
    return ((unsigned int)bytes[0] << 24U) |
           ((unsigned int)bytes[1] << 16U) |
           ((unsigned int)bytes[2] << 8U) |
           (unsigned int)bytes[3];
}

static unsigned long long read_u64_le_local(const unsigned char *bytes) {
    return archive_read_u64_le(bytes);
}

static const char *macho_hash_type_name(unsigned int type) {
    if (type == MACHO_CODE_SIGNATURE_HASH_SHA256) return "sha256";
    return "unknown";
}

static int bytes_equal(const unsigned char *left, const unsigned char *right, size_t size) {
    size_t index;
    for (index = 0U; index < size; ++index) {
        if (left[index] != right[index]) {
            return 0;
        }
    }
    return 1;
}

static void bytes_to_hex(const unsigned char *bytes, size_t size, char *out, size_t out_size) {
    static const char digits[] = "0123456789abcdef";
    size_t index;
    size_t out_index = 0U;

    if (out_size == 0U) return;
    for (index = 0U; index < size && out_index + 2U < out_size; ++index) {
        out[out_index++] = digits[(bytes[index] >> 4U) & 0x0fU];
        out[out_index++] = digits[bytes[index] & 0x0fU];
    }
    out[out_index] = '\0';
}

static int hash_fd_sha256_hex(int fd, char *out, size_t out_size) {
    CryptoSha256Context sha;
    unsigned char digest[CRYPTO_SHA256_DIGEST_SIZE];
    unsigned long long offset = 0ULL;
    long long file_size;

    file_size = platform_seek(fd, 0, PLATFORM_SEEK_END);
    if (file_size < 0) {
        return -1;
    }
    crypto_sha256_init(&sha);
    while (offset < (unsigned long long)file_size) {
        unsigned char buffer[2048];
        size_t chunk = (unsigned long long)file_size - offset > (unsigned long long)sizeof(buffer) ? sizeof(buffer) : (size_t)((unsigned long long)file_size - offset);
        if (read_region(fd, offset, buffer, chunk) != 0) {
            return -1;
        }
        crypto_sha256_update(&sha, buffer, chunk);
        offset += (unsigned long long)chunk;
    }
    crypto_sha256_final(&sha, digest);
    bytes_to_hex(digest, sizeof(digest), out, out_size);
    return 0;
}

static const char *macho_type_name(unsigned int type) {
    if (type == 1U) return "OBJECT";
    if (type == 2U) return "EXECUTE";
    if (type == 6U) return "DYLIB";
    if (type == 8U) return "BUNDLE";
    return "UNKNOWN";
}

static const char *macho_command_name(unsigned int command) {
    if (command == MACHO_LC_SEGMENT_64) return "LC_SEGMENT_64";
    if (command == MACHO_LC_SYMTAB) return "LC_SYMTAB";
    if (command == MACHO_LC_LOAD_DYLINKER) return "LC_LOAD_DYLINKER";
    if (command == MACHO_LC_CODE_SIGNATURE) return "LC_CODE_SIGNATURE";
    if (command == MACHO_LC_MAIN) return "LC_MAIN";
    if (command == MACHO_LC_BUILD_VERSION) return "LC_BUILD_VERSION";
    return "LC_OTHER";
}

static const char *macho_platform_name(unsigned int platform) {
    if (platform == 1U) return "macOS";
    if (platform == 2U) return "iOS";
    if (platform == 3U) return "tvOS";
    if (platform == 4U) return "watchOS";
    return "unknown";
}

static const char *macho_section_type_name(unsigned int type) {
    if (type == 0U) return "REGULAR";
    if (type == MACHO_S_ZEROFILL) return "ZEROFILL";
    if (type == MACHO_S_CSTRING_LITERALS) return "CSTRING";
    if (type == MACHO_S_4BYTE_LITERALS) return "LITERAL4";
    if (type == MACHO_S_8BYTE_LITERALS) return "LITERAL8";
    if (type == MACHO_S_SYMBOL_STUBS) return "STUBS";
    if (type == MACHO_S_GB_ZEROFILL) return "GB_ZEROFILL";
    if (type == MACHO_S_16BYTE_LITERALS) return "LITERAL16";
    if (type == MACHO_S_THREAD_LOCAL_ZEROFILL) return "TLV_ZEROFILL";
    return "OTHER";
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

static void copy_fixed_name(char *dest, size_t dest_size, const unsigned char *src, size_t src_size) {
    size_t i;

    if (dest_size == 0U) return;
    for (i = 0U; i + 1U < dest_size && i < src_size && src[i] != 0U; ++i) {
        unsigned char ch = src[i];
        dest[i] = (ch >= 32U && ch <= 126U) ? (char)ch : '?';
    }
    dest[i] = '\0';
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

static int load_macho_sections(int fd, const MachHeaderInfo *header, MachSectionInfo *sections, unsigned int *section_count_out) {
    unsigned int command_index;
    unsigned long long command_offset = 32ULL;
    unsigned int section_count = 0U;

    *section_count_out = 0U;
    if (header->ncmds > READELF_MAX_MACHO_COMMANDS) {
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

                if (section_count >= READELF_MAX_SECTIONS) {
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
                sections[section_count].align = read_u32_le_local(raw + 52);
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
    if (header->ncmds > READELF_MAX_MACHO_COMMANDS) {
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

static int load_macho_segments(int fd, const MachHeaderInfo *header, MachSegmentInfo *segments, unsigned int *segment_count_out) {
    unsigned int command_index;
    unsigned long long command_offset = 32ULL;
    unsigned int segment_count = 0U;

    *segment_count_out = 0U;
    if (header->ncmds > READELF_MAX_MACHO_COMMANDS) {
        return -1;
    }
    for (command_index = 0U; command_index < header->ncmds; ++command_index) {
        unsigned char command_data[72];
        unsigned int command;
        unsigned int command_size;

        if (read_region(fd, command_offset, command_data, 8U) != 0) {
            return -1;
        }
        command = read_u32_le_local(command_data + 0);
        command_size = read_u32_le_local(command_data + 4);
        if (command_size < 8U) {
            return -1;
        }
        if (command == MACHO_LC_SEGMENT_64 && command_size >= 72U) {
            if (segment_count >= READELF_MAX_MACHO_SEGMENTS || read_region(fd, command_offset, command_data, sizeof(command_data)) != 0) {
                return -1;
            }
            copy_fixed_name(segments[segment_count].name, sizeof(segments[segment_count].name), command_data + 8, 16U);
            segments[segment_count].vmaddr = read_u64_le_local(command_data + 24);
            segments[segment_count].vmsize = read_u64_le_local(command_data + 32);
            segments[segment_count].fileoff = read_u64_le_local(command_data + 40);
            segments[segment_count].filesize = read_u64_le_local(command_data + 48);
            segments[segment_count].maxprot = read_u32_le_local(command_data + 56);
            segments[segment_count].initprot = read_u32_le_local(command_data + 60);
            segments[segment_count].nsects = read_u32_le_local(command_data + 64);
            segments[segment_count].flags = read_u32_le_local(command_data + 68);
            segment_count += 1U;
        }
        command_offset += (unsigned long long)command_size;
    }
    *segment_count_out = segment_count;
    return 0;
}

static void macho_signature_init(MachCodeSignatureInfo *signature) {
    signature->dataoff = 0U;
    signature->datasize = 0U;
    signature->superblob_magic = 0U;
    signature->superblob_length = 0U;
    signature->superblob_count = 0U;
    signature->code_directory_offset = 0U;
    signature->code_directory_length = 0U;
    signature->code_directory_version = 0U;
    signature->code_directory_flags = 0U;
    signature->hash_offset = 0U;
    signature->ident_offset = 0U;
    signature->n_special_slots = 0U;
    signature->n_code_slots = 0U;
    signature->code_limit = 0U;
    signature->hash_size = 0U;
    signature->hash_type = 0U;
    signature->page_size_log2 = 0U;
    signature->hash_slots_checked = 0U;
    signature->hash_mismatches = 0U;
    signature->present = 0;
    signature->has_code_directory = 0;
    signature->structure_valid = 0;
    signature->hashes_verified = 0;
    signature->identifier[0] = '\0';
    rt_copy_string(signature->message, sizeof(signature->message), "no code signature");
}

static int load_macho_code_signature_command(int fd, const MachHeaderInfo *header, MachCodeSignatureInfo *signature) {
    unsigned int command_index;
    unsigned long long command_offset = 32ULL;

    macho_signature_init(signature);
    if (header->ncmds > READELF_MAX_MACHO_COMMANDS) {
        return -1;
    }
    for (command_index = 0U; command_index < header->ncmds; ++command_index) {
        unsigned char command_data[16];
        unsigned int command;
        unsigned int command_size;

        if (read_region(fd, command_offset, command_data, 8U) != 0) {
            return -1;
        }
        command = read_u32_le_local(command_data + 0);
        command_size = read_u32_le_local(command_data + 4);
        if (command_size < 8U) {
            return -1;
        }
        if (command == MACHO_LC_CODE_SIGNATURE && command_size >= 16U) {
            if (read_region(fd, command_offset, command_data, sizeof(command_data)) != 0) {
                return -1;
            }
            signature->present = 1;
            signature->dataoff = read_u32_le_local(command_data + 8);
            signature->datasize = read_u32_le_local(command_data + 12);
            rt_copy_string(signature->message, sizeof(signature->message), "code signature command present");
            return 0;
        }
        command_offset += (unsigned long long)command_size;
    }
    return 0;
}

static int macho_copy_cstring_from_blob(int fd, unsigned long long offset, unsigned long long max_length, char *out, size_t out_size) {
    size_t index;

    if (out_size == 0U) return -1;
    out[0] = '\0';
    for (index = 0U; index + 1U < out_size && (unsigned long long)index < max_length; ++index) {
        unsigned char ch;
        if (read_region(fd, offset + (unsigned long long)index, &ch, 1U) != 0) {
            return -1;
        }
        if (ch == 0U) {
            out[index] = '\0';
            return 0;
        }
        out[index] = (ch >= 32U && ch <= 126U) ? (char)ch : '?';
    }
    out[index < out_size ? index : out_size - 1U] = '\0';
    return 0;
}

static int macho_verify_code_directory_hashes(int fd, const MachCodeSignatureInfo *signature, unsigned int *checked_out, unsigned int *mismatches_out) {
    unsigned long long page_size;
    unsigned long long slot;
    unsigned char expected[CRYPTO_SHA256_DIGEST_SIZE];
    unsigned char actual[CRYPTO_SHA256_DIGEST_SIZE];

    *checked_out = 0U;
    *mismatches_out = 0U;
    if (!signature->has_code_directory || signature->hash_type != MACHO_CODE_SIGNATURE_HASH_SHA256 || signature->hash_size != CRYPTO_SHA256_DIGEST_SIZE || signature->page_size_log2 >= 31U) {
        return -1;
    }
    page_size = 1ULL << signature->page_size_log2;
    if (page_size == 0ULL || signature->code_limit == 0U) {
        return -1;
    }
    for (slot = 0ULL; slot < (unsigned long long)signature->n_code_slots; ++slot) {
        unsigned long long page_offset = slot * page_size;
        unsigned long long remaining;
        unsigned long long hash_offset;
        CryptoSha256Context sha;

        if (page_offset >= (unsigned long long)signature->code_limit) {
            break;
        }
        remaining = (unsigned long long)signature->code_limit - page_offset;
        if (remaining > page_size) {
            remaining = page_size;
        }
        crypto_sha256_init(&sha);
        while (remaining != 0ULL) {
            unsigned char buffer[1024];
            size_t chunk = remaining > (unsigned long long)sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
            if (read_region(fd, page_offset, buffer, chunk) != 0) {
                return -1;
            }
            crypto_sha256_update(&sha, buffer, chunk);
            page_offset += (unsigned long long)chunk;
            remaining -= (unsigned long long)chunk;
        }
        crypto_sha256_final(&sha, actual);
        hash_offset = (unsigned long long)signature->dataoff + (unsigned long long)signature->code_directory_offset + (unsigned long long)signature->hash_offset + (slot * (unsigned long long)signature->hash_size);
        if (read_region(fd, hash_offset, expected, sizeof(expected)) != 0) {
            return -1;
        }
        *checked_out += 1U;
        if (!bytes_equal(expected, actual, sizeof(expected))) {
            *mismatches_out += 1U;
        }
    }
    return *checked_out == signature->n_code_slots ? 0 : -1;
}

static int inspect_macho_code_signature(int fd, const MachHeaderInfo *header, MachCodeSignatureInfo *signature) {
    unsigned char raw[32];
    long long file_size;
    unsigned int index;

    if (load_macho_code_signature_command(fd, header, signature) != 0) {
        return -1;
    }
    if (!signature->present) {
        return 0;
    }
    file_size = platform_seek(fd, 0, PLATFORM_SEEK_END);
    if (file_size < 0 || (unsigned long long)signature->dataoff + (unsigned long long)signature->datasize > (unsigned long long)file_size || signature->datasize < 12U) {
        rt_copy_string(signature->message, sizeof(signature->message), "invalid code signature range");
        return 0;
    }
    if (read_region(fd, (unsigned long long)signature->dataoff, raw, 12U) != 0) {
        return -1;
    }
    signature->superblob_magic = read_u32_be_local(raw + 0);
    signature->superblob_length = read_u32_be_local(raw + 4);
    signature->superblob_count = read_u32_be_local(raw + 8);
    if (signature->superblob_magic != MACHO_CODE_SIGNATURE_SUPERBLOB || signature->superblob_length > signature->datasize || signature->superblob_length < 12U) {
        rt_copy_string(signature->message, sizeof(signature->message), "invalid code signature SuperBlob");
        return 0;
    }
    for (index = 0U; index < signature->superblob_count; ++index) {
        unsigned long long entry_offset = (unsigned long long)signature->dataoff + 12ULL + ((unsigned long long)index * 8ULL);
        unsigned int slot_type;
        unsigned int blob_offset;
        if (12ULL + ((unsigned long long)(index + 1U) * 8ULL) > (unsigned long long)signature->superblob_length || read_region(fd, entry_offset, raw, 8U) != 0) {
            rt_copy_string(signature->message, sizeof(signature->message), "truncated code signature index");
            return 0;
        }
        slot_type = read_u32_be_local(raw + 0);
        blob_offset = read_u32_be_local(raw + 4);
        if (slot_type == MACHO_CODE_SIGNATURE_SLOT_CODEDIRECTORY) {
            signature->code_directory_offset = blob_offset;
            break;
        }
    }
    if (signature->code_directory_offset == 0U || signature->code_directory_offset + 44U > signature->superblob_length) {
        rt_copy_string(signature->message, sizeof(signature->message), "code signature has no CodeDirectory");
        return 0;
    }
    if (read_region(fd, (unsigned long long)signature->dataoff + signature->code_directory_offset, raw, 44U) != 0) {
        return -1;
    }
    if (read_u32_be_local(raw + 0) != MACHO_CODE_SIGNATURE_CODEDIRECTORY) {
        rt_copy_string(signature->message, sizeof(signature->message), "invalid CodeDirectory magic");
        return 0;
    }
    signature->has_code_directory = 1;
    signature->code_directory_length = read_u32_be_local(raw + 4);
    signature->code_directory_version = read_u32_be_local(raw + 8);
    signature->code_directory_flags = read_u32_be_local(raw + 12);
    signature->hash_offset = read_u32_be_local(raw + 16);
    signature->ident_offset = read_u32_be_local(raw + 20);
    signature->n_special_slots = read_u32_be_local(raw + 24);
    signature->n_code_slots = read_u32_be_local(raw + 28);
    signature->code_limit = read_u32_be_local(raw + 32);
    signature->hash_size = (unsigned int)raw[36];
    signature->hash_type = (unsigned int)raw[37];
    signature->page_size_log2 = (unsigned int)raw[39];
    if (signature->code_directory_length == 0U || signature->code_directory_offset + signature->code_directory_length > signature->superblob_length ||
        signature->hash_offset + (signature->n_code_slots * signature->hash_size) > signature->code_directory_length ||
        signature->code_limit > signature->dataoff) {
        rt_copy_string(signature->message, sizeof(signature->message), "invalid CodeDirectory bounds");
        return 0;
    }
    signature->structure_valid = 1;
    if (signature->ident_offset < signature->code_directory_length) {
        (void)macho_copy_cstring_from_blob(fd,
                                          (unsigned long long)signature->dataoff + signature->code_directory_offset + signature->ident_offset,
                                          (unsigned long long)signature->code_directory_length - signature->ident_offset,
                                          signature->identifier,
                                          sizeof(signature->identifier));
    }
    if (macho_verify_code_directory_hashes(fd, signature, &signature->hash_slots_checked, &signature->hash_mismatches) == 0 && signature->hash_mismatches == 0U) {
        signature->hashes_verified = 1;
        rt_copy_string(signature->message, sizeof(signature->message), "CodeDirectory SHA-256 hashes verified");
    } else if (signature->hash_slots_checked != 0U) {
        rt_copy_string(signature->message, sizeof(signature->message), "CodeDirectory SHA-256 hash mismatch");
    } else {
        rt_copy_string(signature->message, sizeof(signature->message), "CodeDirectory hashes were not verified");
    }
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
    rt_write_cstr(1, "  Magic: ");
    write_hex_value((unsigned long long)info->magic);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  Type: ");
    rt_write_line(1, macho_type_name(info->filetype));
    rt_write_cstr(1, "  Machine: ");
    rt_write_line(1, macho_machine_name(info->cputype));
    rt_write_cstr(1, "  CPU subtype: ");
    write_hex_value((unsigned long long)info->cpusubtype);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  Load commands: ");
    rt_write_uint(1, (unsigned long long)info->ncmds);
    rt_write_cstr(1, " (");
    rt_write_uint(1, (unsigned long long)info->sizeofcmds);
    rt_write_line(1, " bytes)");
    rt_write_cstr(1, "  Flags: ");
    write_hex_value((unsigned long long)info->flags);
    rt_write_char(1, '\n');
}

static void print_macho_version(unsigned int version) {
    rt_write_uint(1, (unsigned long long)((version >> 16U) & 0xffffU));
    rt_write_char(1, '.');
    rt_write_uint(1, (unsigned long long)((version >> 8U) & 0xffU));
    rt_write_char(1, '.');
    rt_write_uint(1, (unsigned long long)(version & 0xffU));
}

static void print_macho_load_commands(int fd, const MachHeaderInfo *header) {
    unsigned int command_index;
    unsigned long long command_offset = 32ULL;

    rt_write_line(1, "Mach-O Load Commands:");
    if (header->ncmds == 0U) {
        rt_write_line(1, "  (none)");
        return;
    }
    if (header->ncmds > READELF_MAX_MACHO_COMMANDS) {
        rt_write_line(1, "  invalid load-command count");
        return;
    }

    for (command_index = 0U; command_index < header->ncmds; ++command_index) {
        unsigned char command_data[128];
        unsigned int command;
        unsigned int command_size;

        if (read_region(fd, command_offset, command_data, 8U) != 0) {
            rt_write_line(1, "  truncated load-command table");
            return;
        }
        command = read_u32_le_local(command_data + 0);
        command_size = read_u32_le_local(command_data + 4);
        if (command_size < 8U) {
            rt_write_line(1, "  invalid load-command size");
            return;
        }

        rt_write_cstr(1, "  [");
        rt_write_uint(1, (unsigned long long)command_index);
        rt_write_cstr(1, "] ");
        rt_write_cstr(1, macho_command_name(command));
        rt_write_cstr(1, " cmdsize=");
        rt_write_uint(1, (unsigned long long)command_size);

        if (command == MACHO_LC_SEGMENT_64 && command_size >= 72U && read_region(fd, command_offset, command_data, 72U) == 0) {
            char segment_name[17];
            copy_fixed_name(segment_name, sizeof(segment_name), command_data + 8, 16U);
            rt_write_cstr(1, " segname=");
            rt_write_cstr(1, segment_name[0] != '\0' ? segment_name : "(none)");
            rt_write_cstr(1, " vmaddr=");
            write_hex_value(read_u64_le_local(command_data + 24));
            rt_write_cstr(1, " vmsize=");
            write_hex_value(read_u64_le_local(command_data + 32));
            rt_write_cstr(1, " fileoff=");
            write_hex_value(read_u64_le_local(command_data + 40));
            rt_write_cstr(1, " filesize=");
            write_hex_value(read_u64_le_local(command_data + 48));
            rt_write_cstr(1, " initprot=");
            write_hex_value((unsigned long long)read_u32_le_local(command_data + 60));
            rt_write_cstr(1, " nsects=");
            rt_write_uint(1, (unsigned long long)read_u32_le_local(command_data + 64));
        } else if (command == MACHO_LC_SYMTAB && command_size >= 24U && read_region(fd, command_offset, command_data, 24U) == 0) {
            rt_write_cstr(1, " symoff=");
            write_hex_value((unsigned long long)read_u32_le_local(command_data + 8));
            rt_write_cstr(1, " nsyms=");
            rt_write_uint(1, (unsigned long long)read_u32_le_local(command_data + 12));
            rt_write_cstr(1, " stroff=");
            write_hex_value((unsigned long long)read_u32_le_local(command_data + 16));
            rt_write_cstr(1, " strsize=");
            write_hex_value((unsigned long long)read_u32_le_local(command_data + 20));
        } else if (command == MACHO_LC_MAIN && command_size >= 24U && read_region(fd, command_offset, command_data, 24U) == 0) {
            rt_write_cstr(1, " entryoff=");
            write_hex_value(read_u64_le_local(command_data + 8));
            rt_write_cstr(1, " stacksize=");
            write_hex_value(read_u64_le_local(command_data + 16));
        } else if (command == MACHO_LC_CODE_SIGNATURE && command_size >= 16U && read_region(fd, command_offset, command_data, 16U) == 0) {
            rt_write_cstr(1, " dataoff=");
            write_hex_value((unsigned long long)read_u32_le_local(command_data + 8));
            rt_write_cstr(1, " datasize=");
            write_hex_value((unsigned long long)read_u32_le_local(command_data + 12));
        } else if (command == MACHO_LC_BUILD_VERSION && command_size >= 24U && read_region(fd, command_offset, command_data, 24U) == 0) {
            unsigned int platform = read_u32_le_local(command_data + 8);
            rt_write_cstr(1, " platform=");
            rt_write_cstr(1, macho_platform_name(platform));
            rt_write_cstr(1, " minos=");
            print_macho_version(read_u32_le_local(command_data + 12));
            rt_write_cstr(1, " sdk=");
            print_macho_version(read_u32_le_local(command_data + 16));
            rt_write_cstr(1, " ntools=");
            rt_write_uint(1, (unsigned long long)read_u32_le_local(command_data + 20));
        } else if (command == MACHO_LC_LOAD_DYLINKER && command_size > 12U && command_size <= sizeof(command_data) && read_region(fd, command_offset, command_data, command_size) == 0) {
            unsigned int name_offset = read_u32_le_local(command_data + 8);
            if (name_offset < command_size) {
                char path[128];
                copy_fixed_name(path, sizeof(path), command_data + name_offset, (size_t)(command_size - name_offset));
                rt_write_cstr(1, " path=");
                rt_write_cstr(1, path);
            }
        } else {
            rt_write_cstr(1, " cmd=");
            write_hex_value((unsigned long long)command);
        }
        rt_write_char(1, '\n');
        command_offset += (unsigned long long)command_size;
    }
}

static void print_macho_sections(const MachSectionInfo *sections, unsigned int section_count) {
    unsigned int i;

    rt_write_line(1, "Mach-O Sections:");
    if (section_count == 0U) {
        rt_write_line(1, "  (none)");
        return;
    }
    for (i = 0U; i < section_count; ++i) {
        rt_write_cstr(1, "  [");
        rt_write_uint(1, (unsigned long long)i);
        rt_write_cstr(1, "] ");
        rt_write_cstr(1, sections[i].segment);
        rt_write_cstr(1, ",");
        rt_write_cstr(1, sections[i].section);
        rt_write_cstr(1, " type=");
        rt_write_cstr(1, macho_section_type_name(sections[i].flags & 0xffU));
        rt_write_cstr(1, " addr=");
        write_hex_value(sections[i].addr);
        rt_write_cstr(1, " off=");
        write_hex_value((unsigned long long)sections[i].offset);
        rt_write_cstr(1, " size=");
        write_hex_value(sections[i].size);
        rt_write_cstr(1, " align=2^");
        rt_write_uint(1, (unsigned long long)sections[i].align);
        rt_write_cstr(1, " flags=");
        write_hex_value((unsigned long long)sections[i].flags);
        rt_write_char(1, '\n');
    }
}

static void print_macho_symbols(int fd, const MachSymtabInfo *symtab) {
    char strings[READELF_NAME_TABLE_CAPACITY];
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

    rt_write_line(1, "Mach-O Symbol Table:");
    for (index = 0U; index < symtab->nsyms; ++index) {
        unsigned char entry[16];
        unsigned int strx;
        unsigned int type;
        unsigned int sect;
        unsigned int desc;
        unsigned long long value;
        const char *name;

        if (read_region(fd, (unsigned long long)symtab->symoff + ((unsigned long long)index * 16ULL), entry, sizeof(entry)) != 0) {
            break;
        }
        strx = read_u32_le_local(entry + 0);
        type = (unsigned int)entry[4];
        sect = (unsigned int)entry[5];
        desc = (unsigned int)read_u16_le(entry + 6);
        value = read_u64_le_local(entry + 8);
        name = name_from_table(strings, string_size, strx);

        rt_write_cstr(1, "  [");
        rt_write_uint(1, (unsigned long long)index);
        rt_write_cstr(1, "] ");
        rt_write_cstr(1, name);
        rt_write_cstr(1, " value=");
        write_hex_value(value);
        rt_write_cstr(1, " type=");
        write_hex_value((unsigned long long)type);
        rt_write_cstr(1, " sect=");
        rt_write_uint(1, (unsigned long long)sect);
        rt_write_cstr(1, " desc=");
        write_hex_value((unsigned long long)desc);
        rt_write_char(1, '\n');
    }
}

static void print_macho_code_signature(const MachCodeSignatureInfo *signature) {
    rt_write_line(1, "Mach-O Code Signature:");
    if (!signature->present) {
        rt_write_line(1, "  (none)");
        return;
    }
    rt_write_cstr(1, "  dataoff=");
    write_hex_value((unsigned long long)signature->dataoff);
    rt_write_cstr(1, " datasize=");
    write_hex_value((unsigned long long)signature->datasize);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  SuperBlob: magic=");
    write_hex_value((unsigned long long)signature->superblob_magic);
    rt_write_cstr(1, " length=");
    rt_write_uint(1, (unsigned long long)signature->superblob_length);
    rt_write_cstr(1, " count=");
    rt_write_uint(1, (unsigned long long)signature->superblob_count);
    rt_write_char(1, '\n');
    if (!signature->has_code_directory) {
        rt_write_cstr(1, "  ");
        rt_write_line(1, signature->message);
        return;
    }
    rt_write_cstr(1, "  CodeDirectory: offset=");
    write_hex_value((unsigned long long)signature->code_directory_offset);
    rt_write_cstr(1, " length=");
    rt_write_uint(1, (unsigned long long)signature->code_directory_length);
    rt_write_cstr(1, " version=");
    write_hex_value((unsigned long long)signature->code_directory_version);
    rt_write_cstr(1, " flags=");
    write_hex_value((unsigned long long)signature->code_directory_flags);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  Identifier: ");
    rt_write_line(1, signature->identifier[0] != '\0' ? signature->identifier : "(none)");
    rt_write_cstr(1, "  Hashes: type=");
    rt_write_cstr(1, macho_hash_type_name(signature->hash_type));
    rt_write_cstr(1, " size=");
    rt_write_uint(1, (unsigned long long)signature->hash_size);
    rt_write_cstr(1, " page-size=2^");
    rt_write_uint(1, (unsigned long long)signature->page_size_log2);
    rt_write_cstr(1, " code-slots=");
    rt_write_uint(1, (unsigned long long)signature->n_code_slots);
    rt_write_cstr(1, " special-slots=");
    rt_write_uint(1, (unsigned long long)signature->n_special_slots);
    rt_write_cstr(1, " code-limit=");
    write_hex_value((unsigned long long)signature->code_limit);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  Verification: ");
    rt_write_cstr(1, signature->hashes_verified ? "ok" : (signature->structure_valid ? "failed" : "invalid"));
    rt_write_cstr(1, " checked=");
    rt_write_uint(1, (unsigned long long)signature->hash_slots_checked);
    rt_write_cstr(1, " mismatches=");
    rt_write_uint(1, (unsigned long long)signature->hash_mismatches);
    rt_write_cstr(1, " message=");
    rt_write_line(1, signature->message);
}

static int json_macho_header_event(const char *path, const MachHeaderInfo *info) {
    if (tool_json_begin_event(1, "readelf", "stdout", "macho_header") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
    if (rt_write_cstr(1, "\"file\":") != 0 || tool_json_write_string(1, path) != 0) return -1;
    if (json_field_uint("magic", info->magic) != 0) return -1;
    if (json_field_string("type", macho_type_name(info->filetype)) != 0) return -1;
    if (json_field_string("machine", macho_machine_name(info->cputype)) != 0) return -1;
    if (json_field_uint("cputype", info->cputype) != 0) return -1;
    if (json_field_uint("cpusubtype", info->cpusubtype) != 0) return -1;
    if (json_field_uint("ncmds", info->ncmds) != 0) return -1;
    if (json_field_uint("sizeofcmds", info->sizeofcmds) != 0) return -1;
    if (json_field_uint("flags", info->flags) != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int json_macho_section_event(const char *path, unsigned int index, const MachSectionInfo *section) {
    if (tool_json_begin_event(1, "readelf", "stdout", "macho_section") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
    if (rt_write_cstr(1, "\"file\":") != 0 || tool_json_write_string(1, path) != 0) return -1;
    if (json_field_uint("index", index) != 0) return -1;
    if (json_field_string("segment", section->segment) != 0) return -1;
    if (json_field_string("section", section->section) != 0) return -1;
    if (json_field_string("type", macho_section_type_name(section->flags & 0xffU)) != 0) return -1;
    if (json_field_uint("addr", section->addr) != 0) return -1;
    if (json_field_uint("offset", section->offset) != 0) return -1;
    if (json_field_uint("size", section->size) != 0) return -1;
    if (json_field_uint("align", section->align) != 0) return -1;
    if (json_field_uint("reloff", section->reloff) != 0) return -1;
    if (json_field_uint("nreloc", section->nreloc) != 0) return -1;
    if (json_field_uint("flags", section->flags) != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int json_macho_code_signature_event(const char *path, const MachCodeSignatureInfo *signature) {
    if (tool_json_begin_event(1, "readelf", "stdout", "macho_code_signature") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
    if (rt_write_cstr(1, "\"file\":") != 0 || tool_json_write_string(1, path) != 0) return -1;
    if (json_field_bool("present", signature->present) != 0) return -1;
    if (json_field_uint("dataoff", signature->dataoff) != 0) return -1;
    if (json_field_uint("datasize", signature->datasize) != 0) return -1;
    if (json_field_bool("has_code_directory", signature->has_code_directory) != 0) return -1;
    if (json_field_bool("structure_valid", signature->structure_valid) != 0) return -1;
    if (json_field_bool("hashes_verified", signature->hashes_verified) != 0) return -1;
    if (json_field_string("identifier", signature->identifier) != 0) return -1;
    if (json_field_uint("code_limit", signature->code_limit) != 0) return -1;
    if (json_field_string("hash_type", macho_hash_type_name(signature->hash_type)) != 0) return -1;
    if (json_field_uint("hash_size", signature->hash_size) != 0) return -1;
    if (json_field_uint("page_size_log2", signature->page_size_log2) != 0) return -1;
    if (json_field_uint("code_slots", signature->n_code_slots) != 0) return -1;
    if (json_field_uint("checked", signature->hash_slots_checked) != 0) return -1;
    if (json_field_uint("mismatches", signature->hash_mismatches) != 0) return -1;
    if (json_field_string("message", signature->message) != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int json_elf_header_event(const char *path, const ElfHeaderInfo *info) {
    if (tool_json_begin_event(1, "readelf", "stdout", "elf_header") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
    if (rt_write_cstr(1, "\"file\":") != 0 || tool_json_write_string(1, path) != 0) return -1;
    if (json_field_string("type", elf_type_name(info->type)) != 0) return -1;
    if (json_field_string("machine", elf_machine_name(info->machine)) != 0) return -1;
    if (json_field_uint("entry", info->entry) != 0) return -1;
    if (json_field_uint("program_header_offset", info->phoff) != 0) return -1;
    if (json_field_uint("section_header_offset", info->shoff) != 0) return -1;
    if (json_field_uint("flags", info->flags) != 0) return -1;
    if (json_field_uint("program_header_count", info->phnum) != 0) return -1;
    if (json_field_uint("section_header_count", info->shnum) != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int json_macho_load_commands(int fd, const char *path, const MachHeaderInfo *header) {
    unsigned int command_index;
    unsigned long long command_offset = 32ULL;

    if (header->ncmds > READELF_MAX_MACHO_COMMANDS) return -1;
    for (command_index = 0U; command_index < header->ncmds; ++command_index) {
        unsigned char command_data[128];
        unsigned int command;
        unsigned int command_size;
        if (read_region(fd, command_offset, command_data, 8U) != 0) return -1;
        command = read_u32_le_local(command_data + 0);
        command_size = read_u32_le_local(command_data + 4);
        if (command_size < 8U) return -1;
        if (tool_json_begin_event(1, "readelf", "stdout", "macho_load_command") != 0) return -1;
        if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
        if (rt_write_cstr(1, "\"file\":") != 0 || tool_json_write_string(1, path) != 0) return -1;
        if (json_field_uint("index", command_index) != 0) return -1;
        if (json_field_uint("offset", command_offset) != 0) return -1;
        if (json_field_uint("command", command) != 0) return -1;
        if (json_field_string("name", macho_command_name(command)) != 0) return -1;
        if (json_field_uint("cmdsize", command_size) != 0) return -1;
        if (command == MACHO_LC_SEGMENT_64 && command_size >= 72U && read_region(fd, command_offset, command_data, 72U) == 0) {
            char segment_name[17];
            copy_fixed_name(segment_name, sizeof(segment_name), command_data + 8, 16U);
            if (json_field_string("segment", segment_name) != 0) return -1;
            if (json_field_uint("vmaddr", read_u64_le_local(command_data + 24)) != 0) return -1;
            if (json_field_uint("vmsize", read_u64_le_local(command_data + 32)) != 0) return -1;
            if (json_field_uint("fileoff", read_u64_le_local(command_data + 40)) != 0) return -1;
            if (json_field_uint("filesize", read_u64_le_local(command_data + 48)) != 0) return -1;
            if (json_field_uint("initprot", read_u32_le_local(command_data + 60)) != 0) return -1;
            if (json_field_uint("nsects", read_u32_le_local(command_data + 64)) != 0) return -1;
        } else if (command == MACHO_LC_MAIN && command_size >= 24U && read_region(fd, command_offset, command_data, 24U) == 0) {
            if (json_field_uint("entryoff", read_u64_le_local(command_data + 8)) != 0) return -1;
            if (json_field_uint("stacksize", read_u64_le_local(command_data + 16)) != 0) return -1;
        } else if (command == MACHO_LC_CODE_SIGNATURE && command_size >= 16U && read_region(fd, command_offset, command_data, 16U) == 0) {
            if (json_field_uint("dataoff", read_u32_le_local(command_data + 8)) != 0) return -1;
            if (json_field_uint("datasize", read_u32_le_local(command_data + 12)) != 0) return -1;
        } else if (command == MACHO_LC_BUILD_VERSION && command_size >= 24U && read_region(fd, command_offset, command_data, 24U) == 0) {
            if (json_field_string("platform", macho_platform_name(read_u32_le_local(command_data + 8))) != 0) return -1;
            if (json_field_uint("minos", read_u32_le_local(command_data + 12)) != 0) return -1;
            if (json_field_uint("sdk", read_u32_le_local(command_data + 16)) != 0) return -1;
        }
        if (rt_write_char(1, '}') != 0 || tool_json_end_event(1) != 0) return -1;
        command_offset += (unsigned long long)command_size;
    }
    return 0;
}

static int json_macho_symbols(int fd, const char *path, const MachSymtabInfo *symtab) {
    char strings[READELF_NAME_TABLE_CAPACITY];
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
        if (tool_json_begin_event(1, "readelf", "stdout", "macho_symbol") != 0) return -1;
        if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
        if (rt_write_cstr(1, "\"file\":") != 0 || tool_json_write_string(1, path) != 0) return -1;
        if (json_field_uint("index", index) != 0) return -1;
        if (json_field_string("name", name_from_table(strings, string_size, strx)) != 0) return -1;
        if (json_field_uint("value", read_u64_le_local(entry + 8)) != 0) return -1;
        if (json_field_uint("type", (unsigned int)entry[4]) != 0) return -1;
        if (json_field_uint("section", (unsigned int)entry[5]) != 0) return -1;
        if (json_field_uint("desc", (unsigned int)read_u16_le(entry + 6)) != 0) return -1;
        if (rt_write_char(1, '}') != 0 || tool_json_end_event(1) != 0) return -1;
    }
    return 0;
}

static int json_macho_relocations(int fd, const char *path, const MachSectionInfo *sections, unsigned int section_count, const MachSymtabInfo *symtab) {
    char strings[READELF_NAME_TABLE_CAPACITY];
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
            if (read_region(fd, (unsigned long long)sections[section_index].reloff + ((unsigned long long)reloc_index * 8ULL), raw, sizeof(raw)) != 0) return -1;
            address = read_u32_le_local(raw + 0);
            info = read_u32_le_local(raw + 4);
            symbolnum = info & 0x00ffffffU;
            external = (info >> 27U) & 1U;
            type = (info >> 28U) & 0x0fU;
            if (tool_json_begin_event(1, "readelf", "stdout", "macho_relocation") != 0) return -1;
            if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
            if (rt_write_cstr(1, "\"file\":") != 0 || tool_json_write_string(1, path) != 0) return -1;
            if (json_field_string("segment", sections[section_index].segment) != 0) return -1;
            if (json_field_string("section", sections[section_index].section) != 0) return -1;
            if (json_field_uint("offset", address) != 0) return -1;
            if (json_field_string("type", macho_arm64_relocation_name(type)) != 0) return -1;
            if (json_field_uint("length", (info >> 25U) & 3U) != 0) return -1;
            if (json_field_bool("pcrel", ((info >> 24U) & 1U) != 0U) != 0) return -1;
            if (json_field_bool("external", external != 0U) != 0) return -1;
            if (external != 0U) {
                if (json_field_string("symbol", macho_symbol_name_at(fd, symtab, symbolnum, strings, string_size)) != 0) return -1;
            } else if (json_field_uint("section_ordinal", symbolnum) != 0) return -1;
            if (rt_write_char(1, '}') != 0 || tool_json_end_event(1) != 0) return -1;
        }
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
    char strings[READELF_NAME_TABLE_CAPACITY];
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
        unsigned int reloc_index;
        const MachSectionInfo *section = &sections[section_index];

        if (section->nreloc == 0U || section->reloff == 0U) {
            continue;
        }
        saw_relocations = 1;
        rt_write_cstr(1, "Relocation section '");
        rt_write_cstr(1, section->segment);
        rt_write_char(1, ',');
        rt_write_cstr(1, section->section);
        rt_write_cstr(1, "' at offset ");
        write_hex_value((unsigned long long)section->reloff);
        rt_write_cstr(1, " contains ");
        rt_write_uint(1, (unsigned long long)section->nreloc);
        rt_write_line(1, " entries:");
        rt_write_line(1, "  Offset      Type                         Length PCRel Extern Symbol/Section");
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

            rt_write_cstr(1, "  ");
            write_hex_value((unsigned long long)address);
            rt_write_cstr(1, "  ");
            rt_write_cstr(1, macho_arm64_relocation_name(type));
            rt_write_cstr(1, "  ");
            rt_write_uint(1, (unsigned long long)length);
            rt_write_cstr(1, "      ");
            rt_write_uint(1, (unsigned long long)pcrel);
            rt_write_cstr(1, "     ");
            rt_write_uint(1, (unsigned long long)external);
            rt_write_cstr(1, "      ");
            if (external) {
                const char *name = macho_symbol_name_at(fd, symtab, symbolnum, strings, string_size);
                if (name[0] != '\0') {
                    rt_write_cstr(1, name);
                } else {
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
        rt_write_line(1, "There are no relocations in this Mach-O file.");
    }
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

static unsigned int count_elf_symbols(const ElfHeaderInfo *header, const ElfSectionInfo *sections) {
    unsigned short i;
    unsigned int count = 0U;
    for (i = 0U; i < header->shnum; ++i) {
        if ((sections[i].type == 2U || sections[i].type == 11U) && sections[i].entsize != 0ULL) {
            count += (unsigned int)(sections[i].size / sections[i].entsize);
        }
    }
    return count;
}

static unsigned int count_elf_relocations(const ElfHeaderInfo *header, const ElfSectionInfo *sections) {
    unsigned short i;
    unsigned int count = 0U;
    for (i = 0U; i < header->shnum; ++i) {
        if ((sections[i].type == ELF_SHT_RELA || sections[i].type == ELF_SHT_REL)) {
            unsigned long long entry_size = sections[i].entsize != 0ULL ? sections[i].entsize : (sections[i].type == ELF_SHT_RELA ? 24ULL : 16ULL);
            if (entry_size != 0ULL) count += (unsigned int)(sections[i].size / entry_size);
        }
    }
    return count;
}

static unsigned int count_macho_relocations(const MachSectionInfo *sections, unsigned int section_count) {
    unsigned int i;
    unsigned int count = 0U;
    for (i = 0U; i < section_count; ++i) {
        count += sections[i].nreloc;
    }
    return count;
}

static int load_compare_summary(const char *path, BinaryCompareSummary *summary) {
    int fd = platform_open_read(path);
    ElfHeaderInfo elf_header;
    ElfProgramInfo programs[READELF_MAX_PROGRAM_HEADERS];
    ElfSectionInfo elf_sections[READELF_MAX_SECTIONS];
    MachHeaderInfo macho_header;
    MachSectionInfo macho_sections[READELF_MAX_SECTIONS];
    MachSegmentInfo macho_segments[READELF_MAX_MACHO_SEGMENTS];
    MachSymtabInfo macho_symtab;
    MachCodeSignatureInfo signature;
    char names[READELF_NAME_TABLE_CAPACITY];
    size_t names_size = 0U;
    unsigned int macho_section_count = 0U;
    unsigned int macho_segment_count = 0U;

    (void)programs;
    if (fd < 0) {
        tool_write_error("readelf", "cannot open ", path);
        return -1;
    }
    if (parse_elf_header(fd, &elf_header) == 0 && load_program_headers(fd, &elf_header, programs) == 0 &&
        load_sections(fd, &elf_header, elf_sections) == 0 && load_name_table(fd, &elf_header, elf_sections, names, sizeof(names), &names_size) == 0) {
        rt_copy_string(summary->format, sizeof(summary->format), "elf");
        if (hash_fd_sha256_hex(fd, summary->sha256, sizeof(summary->sha256)) != 0) summary->sha256[0] = '\0';
        summary->entry = elf_header.entry;
        summary->machine = elf_header.machine;
        summary->type = elf_header.type;
        summary->flags = elf_header.flags;
        summary->segment_count = elf_header.phnum;
        summary->section_count = elf_header.shnum;
        summary->symbol_count = count_elf_symbols(&elf_header, elf_sections);
        summary->relocation_count = count_elf_relocations(&elf_header, elf_sections);
        summary->code_signature_present = 0;
        summary->code_signature_verified = 0;
        platform_close(fd);
        return 0;
    }
    if (parse_macho_header(fd, &macho_header) == 0 && load_macho_sections(fd, &macho_header, macho_sections, &macho_section_count) == 0 &&
        load_macho_segments(fd, &macho_header, macho_segments, &macho_segment_count) == 0 && load_macho_symtab(fd, &macho_header, &macho_symtab) == 0 &&
        inspect_macho_code_signature(fd, &macho_header, &signature) == 0) {
        rt_copy_string(summary->format, sizeof(summary->format), "macho");
        if (hash_fd_sha256_hex(fd, summary->sha256, sizeof(summary->sha256)) != 0) summary->sha256[0] = '\0';
        summary->entry = 0ULL;
        summary->machine = macho_header.cputype;
        summary->type = macho_header.filetype;
        summary->flags = macho_header.flags;
        summary->segment_count = macho_segment_count;
        summary->section_count = macho_section_count;
        summary->symbol_count = macho_symtab.nsyms;
        summary->relocation_count = count_macho_relocations(macho_sections, macho_section_count);
        summary->code_signature_present = signature.present;
        summary->code_signature_verified = signature.hashes_verified;
        platform_close(fd);
        return 0;
    }
    platform_close(fd);
    tool_write_error("readelf", "unsupported or invalid object file ", path);
    return -1;
}

static int compare_field_string(const char *name, const char *left, const char *right, int *differences) {
    if (rt_strcmp(left, right) == 0) return 0;
    *differences += 1;
    if (readelf_json) {
        if (tool_json_begin_event(1, "readelf", "stdout", "compare_difference") != 0) return -1;
        if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
        if (rt_write_cstr(1, "\"field\":") != 0 || tool_json_write_string(1, name) != 0) return -1;
        if (json_field_string("left", left) != 0 || json_field_string("right", right) != 0) return -1;
        if (rt_write_char(1, '}') != 0) return -1;
        return tool_json_end_event(1);
    }
    rt_write_cstr(1, name);
    rt_write_cstr(1, ": ");
    rt_write_cstr(1, left);
    rt_write_cstr(1, " != ");
    rt_write_line(1, right);
    return 0;
}

static int compare_field_uint(const char *name, unsigned long long left, unsigned long long right, int *differences) {
    if (left == right) return 0;
    *differences += 1;
    if (readelf_json) {
        if (tool_json_begin_event(1, "readelf", "stdout", "compare_difference") != 0) return -1;
        if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
        if (rt_write_cstr(1, "\"field\":") != 0 || tool_json_write_string(1, name) != 0) return -1;
        if (json_field_uint("left", left) != 0 || json_field_uint("right", right) != 0) return -1;
        if (rt_write_char(1, '}') != 0) return -1;
        return tool_json_end_event(1);
    }
    rt_write_cstr(1, name);
    rt_write_cstr(1, ": ");
    rt_write_uint(1, left);
    rt_write_cstr(1, " != ");
    rt_write_uint(1, right);
    rt_write_char(1, '\n');
    return 0;
}

static int compare_files(const char *left_path, const char *right_path) {
    BinaryCompareSummary left;
    BinaryCompareSummary right;
    int differences = 0;

    if (load_compare_summary(left_path, &left) != 0 || load_compare_summary(right_path, &right) != 0) {
        return 2;
    }
    if (!readelf_json) {
        rt_write_cstr(1, "Comparing ");
        rt_write_cstr(1, left_path);
        rt_write_cstr(1, " and ");
        rt_write_line(1, right_path);
    }
    if (compare_field_string("format", left.format, right.format, &differences) != 0) return 2;
    if (compare_field_uint("machine", left.machine, right.machine, &differences) != 0) return 2;
    if (compare_field_uint("type", left.type, right.type, &differences) != 0) return 2;
    if (compare_field_uint("flags", left.flags, right.flags, &differences) != 0) return 2;
    if (compare_field_uint("entry", left.entry, right.entry, &differences) != 0) return 2;
    if (compare_field_uint("segments", left.segment_count, right.segment_count, &differences) != 0) return 2;
    if (compare_field_uint("sections", left.section_count, right.section_count, &differences) != 0) return 2;
    if (compare_field_uint("symbols", left.symbol_count, right.symbol_count, &differences) != 0) return 2;
    if (compare_field_uint("relocations", left.relocation_count, right.relocation_count, &differences) != 0) return 2;
    if (compare_field_uint("code_signature_present", (unsigned long long)left.code_signature_present, (unsigned long long)right.code_signature_present, &differences) != 0) return 2;
    if (compare_field_uint("code_signature_verified", (unsigned long long)left.code_signature_verified, (unsigned long long)right.code_signature_verified, &differences) != 0) return 2;
    if (compare_field_string("sha256", left.sha256, right.sha256, &differences) != 0) return 2;
    if (readelf_json) {
        if (tool_json_begin_event(1, "readelf", "stdout", "compare_summary") != 0) return 2;
        if (rt_write_cstr(1, ",\"data\":{") != 0) return 2;
        if (rt_write_cstr(1, "\"left_file\":") != 0 || tool_json_write_string(1, left_path) != 0) return 2;
        if (json_field_string("right_file", right_path) != 0) return 2;
        if (json_field_uint("differences", (unsigned long long)differences) != 0) return 2;
        if (json_field_bool("equal", differences == 0) != 0) return 2;
        if (rt_write_char(1, '}') != 0 || tool_json_end_event(1) != 0) return 2;
    } else if (differences == 0) {
        rt_write_line(1, "No structural differences found.");
    }
    return differences == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    int show_header_flag = 0;
    int show_programs_flag = 0;
    int show_sections_flag = 0;
    int show_dynamic_flag = 0;
    int show_relocations_flag = 0;
    int show_symbols_flag = 0;
    int show_notes_flag = 0;
    int compare_mode = 0;
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
        } else if (rt_strcmp(argv[argi], "--compare") == 0) {
            compare_mode = 1;
        } else if (rt_strcmp(argv[argi], "--json") == 0) {
            readelf_json = 1;
            tool_json_set_enabled(1);
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
            tool_write_usage("readelf", "[-a] [-h] [-l] [-S] [-d] [-r] [-s] [-n] [--json] [--compare left right] file ...");
            return 1;
        }
        argi += 1;
    }

    if (compare_mode) {
        if (argc - argi != 2) {
            tool_write_usage("readelf", "--compare [--json] left right");
            return 1;
        }
        return compare_files(argv[argi], argv[argi + 1]);
    }

    if (!show_header_flag && !show_programs_flag && !show_sections_flag && !show_dynamic_flag && !show_relocations_flag && !show_symbols_flag && !show_notes_flag) {
        show_header_flag = 1;
    }

    if (argi >= argc) {
        tool_write_usage("readelf", "[-a] [-h] [-l] [-S] [-d] [-r] [-s] [-n] [--json] file ...");
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        int fd = platform_open_read(argv[i]);
        ElfHeaderInfo header;
        MachHeaderInfo macho;
        MachSectionInfo macho_sections[READELF_MAX_SECTIONS];
        MachSymtabInfo macho_symtab;
        unsigned int macho_section_count = 0U;
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
                int macho_sections_ok = load_macho_sections(fd, &macho, macho_sections, &macho_section_count) == 0;
                int macho_symtab_ok = load_macho_symtab(fd, &macho, &macho_symtab) == 0;
                MachCodeSignatureInfo macho_signature;
                (void)inspect_macho_code_signature(fd, &macho, &macho_signature);
                if (!readelf_json) {
                    rt_write_cstr(1, "File: ");
                    rt_write_line(1, argv[i]);
                }
                if (show_header_flag) {
                    if (readelf_json) (void)json_macho_header_event(argv[i], &macho);
                    else print_macho_header(&macho);
                }
                if (show_programs_flag) {
                    if (readelf_json) (void)json_macho_load_commands(fd, argv[i], &macho);
                    else print_macho_load_commands(fd, &macho);
                }
                if (show_sections_flag) {
                    if (macho_sections_ok) {
                        if (readelf_json) {
                            unsigned int section_index;
                            for (section_index = 0U; section_index < macho_section_count; ++section_index) {
                                (void)json_macho_section_event(argv[i], section_index, &macho_sections[section_index]);
                            }
                        } else {
                            print_macho_sections(macho_sections, macho_section_count);
                        }
                    } else {
                        if (!readelf_json) rt_write_line(1, "Mach-O section table is not available.");
                    }
                }
                if (show_dynamic_flag) {
                    if (!readelf_json) rt_write_line(1, "Mach-O inputs do not have ELF dynamic sections.");
                }
                if (show_relocations_flag) {
                    if (load_macho_sections(fd, &macho, macho_sections, &macho_section_count) == 0 && load_macho_symtab(fd, &macho, &macho_symtab) == 0) {
                        if (readelf_json) (void)json_macho_relocations(fd, argv[i], macho_sections, macho_section_count, &macho_symtab);
                        else print_macho_relocations(fd, macho_sections, macho_section_count, &macho_symtab);
                    }
                }
                if (show_symbols_flag) {
                    if (macho_symtab_ok) {
                        if (readelf_json) (void)json_macho_symbols(fd, argv[i], &macho_symtab);
                        else print_macho_symbols(fd, &macho_symtab);
                    } else if (!readelf_json) {
                        rt_write_line(1, "Mach-O symbol table is not available.");
                    }
                }
                if (show_notes_flag) {
                    if (readelf_json) (void)json_macho_code_signature_event(argv[i], &macho_signature);
                    else print_macho_code_signature(&macho_signature);
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

        if (!readelf_json) {
            rt_write_cstr(1, "File: ");
            rt_write_line(1, argv[i]);
        }
        if (show_header_flag) {
            if (readelf_json) (void)json_elf_header_event(argv[i], &header);
            else print_header(&header);
        }
        if (show_programs_flag) {
            if (!readelf_json) print_program_headers(fd, &header, programs);
        }
        if (show_sections_flag) {
            if (!readelf_json) print_sections(&header, sections, names, names_size);
        }
        if (show_dynamic_flag) {
            if (!readelf_json) print_dynamic(fd, &header, sections, names, names_size);
        }
        if (show_relocations_flag) {
            if (!readelf_json) print_relocations(fd, &header, sections, names, names_size);
        }
        if (show_symbols_flag) {
            if (!readelf_json) print_symbols(fd, &header, sections, names, names_size);
        }
        if (show_notes_flag) {
            if (!readelf_json) print_notes(fd, &header, sections, names, names_size);
        }
        platform_close(fd);
    }

    return exit_code;
}
