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
#define READELF_MAX_MACHO_FAT_ARCHES 16U
#define READELF_MAX_CODE_SIGNATURE_SLOTS 16U

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
#define MACHO_LC_DYSYMTAB 0xbU
#define MACHO_LC_UUID 0x1bU
#define MACHO_LC_DYLD_INFO 0x22U
#define MACHO_LC_DYLD_INFO_ONLY 0x80000022U
#define MACHO_LC_FUNCTION_STARTS 0x26U
#define MACHO_LC_DATA_IN_CODE 0x29U
#define MACHO_LC_SOURCE_VERSION 0x2aU
#define MACHO_LC_DYLD_EXPORTS_TRIE 0x80000033U
#define MACHO_LC_DYLD_CHAINED_FIXUPS 0x80000034U

#define MACHO_VM_PROT_READ 1U
#define MACHO_VM_PROT_WRITE 2U
#define MACHO_VM_PROT_EXECUTE 4U

#define MACHO_DYLD_CHAINED_PTR_ARM64E 1U
#define MACHO_DYLD_CHAINED_PTR_64 2U
#define MACHO_DYLD_CHAINED_PTR_64_OFFSET 6U
#define MACHO_DYLD_CHAINED_PTR_ARM64E_KERNEL 7U
#define MACHO_DYLD_CHAINED_PTR_ARM64E_USERLAND 9U
#define MACHO_DYLD_CHAINED_PTR_ARM64E_USERLAND24 12U

#define MACHO_FAT_MAGIC 0xcafebabeU
#define MACHO_FAT_MAGIC_64 0xcafebabfU
#define MACHO_CPU_ARCH_ABI64 0x01000000U
#define MACHO_CPU_TYPE_X86_64 0x01000007U
#define MACHO_CPU_TYPE_ARM64 0x0100000cU
#define MACHO_CPU_SUBTYPE_MASK 0xff000000U

#define MACHO_CODE_SIGNATURE_SUPERBLOB 0xfade0cc0U
#define MACHO_CODE_SIGNATURE_CODEDIRECTORY 0xfade0c02U
#define MACHO_CODE_SIGNATURE_SLOT_CODEDIRECTORY 0U
#define MACHO_CODE_SIGNATURE_SLOT_REQUIREMENTS 2U
#define MACHO_CODE_SIGNATURE_SLOT_CMS_SIGNATURE 0x10000U
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
    unsigned int reserved1;
    unsigned int reserved2;
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
    int present;
} MachLinkeditDataInfo;

typedef struct {
    unsigned int cputype;
    unsigned int cpusubtype;
    unsigned long long offset;
    unsigned long long size;
    unsigned int align;
} MachFatArchInfo;

typedef struct {
    unsigned int magic;
    unsigned int arch_count;
    int is_64;
    MachFatArchInfo arches[READELF_MAX_MACHO_FAT_ARCHES];
} MachFatInfo;

typedef struct {
    unsigned int type;
    unsigned int offset;
    unsigned int length;
    unsigned int magic;
} MachCodeSignatureSlotInfo;

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
    unsigned int slot_count;
    MachCodeSignatureSlotInfo slots[READELF_MAX_CODE_SIGNATURE_SLOTS];
    int present;
    int has_code_directory;
    int structure_valid;
    int hashes_verified;
    char identifier[128];
    char cdhash[41];
    char cdhash_full[65];
    char message[160];
} MachCodeSignatureInfo;

typedef struct {
    char format[16];
    char sha256[65];
    char load_commands_sha256[65];
    char layout_sha256[65];
    char code_signature_cdhash[41];
    unsigned long long entry;
    unsigned int machine;
    unsigned int type;
    unsigned int flags;
    unsigned int segment_count;
    unsigned int section_count;
    unsigned int load_command_count;
    unsigned int dylib_count;
    unsigned int fixup_count;
    unsigned int symbol_count;
    unsigned int relocation_count;
    int code_signature_present;
    int code_signature_verified;
} BinaryCompareSummary;

static int readelf_json;
static unsigned long long readelf_object_base;
static unsigned long long readelf_object_size;

static int read_region(int fd, unsigned long long offset, unsigned char *buffer, size_t size);
static const char *macho_symbol_name_at(int fd, const MachSymtabInfo *symtab, unsigned int symbol_index, char *strings, size_t string_size);
static int json_macho_section_event(const char *path, unsigned int index, const MachSectionInfo *section);

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

static unsigned long long read_u64_be_local(const unsigned char *bytes) {
    return ((unsigned long long)read_u32_be_local(bytes) << 32U) |
           (unsigned long long)read_u32_be_local(bytes + 4);
}

static const char *macho_hash_type_name(unsigned int type) {
    if (type == MACHO_CODE_SIGNATURE_HASH_SHA256) return "sha256";
    return "unknown";
}

static const char *macho_cpu_subtype_name(unsigned int cputype, unsigned int cpusubtype) {
    unsigned int subtype = cpusubtype & ~MACHO_CPU_SUBTYPE_MASK;
    if ((cputype & 0x00ffffffU) == 12U) {
        if (subtype == 0U) return "ALL";
        if (subtype == 1U) return "V8";
        if (subtype == 2U) return "E";
    }
    if ((cputype & 0x00ffffffU) == 7U) {
        if (subtype == 3U) return "X86_64_ALL";
        if (subtype == 8U) return "X86_64_H";
    }
    return "UNKNOWN";
}

static unsigned int macho_cpu_capabilities(unsigned int cpusubtype) {
    return (cpusubtype & MACHO_CPU_SUBTYPE_MASK) >> 24U;
}

static const char *macho_short_arch_name(unsigned int cputype, unsigned int cpusubtype) {
    unsigned int subtype = cpusubtype & ~MACHO_CPU_SUBTYPE_MASK;
    if (cputype == MACHO_CPU_TYPE_X86_64) return "x86_64";
    if (cputype == MACHO_CPU_TYPE_ARM64 && subtype == 2U) return "arm64e";
    if (cputype == MACHO_CPU_TYPE_ARM64) return "arm64";
    return "unknown";
}

static const char *macho_code_signature_slot_name(unsigned int type) {
    if (type == MACHO_CODE_SIGNATURE_SLOT_CODEDIRECTORY) return "CodeDirectory";
    if (type == MACHO_CODE_SIGNATURE_SLOT_REQUIREMENTS) return "Requirements";
    if (type == MACHO_CODE_SIGNATURE_SLOT_CMS_SIGNATURE) return "CMS Signature";
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
    if (readelf_object_size != 0ULL) file_size = (long long)readelf_object_size;
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
    if (command == MACHO_LC_DYSYMTAB) return "LC_DYSYMTAB";
    if (command == MACHO_LC_LOAD_DYLINKER) return "LC_LOAD_DYLINKER";
    if (command == MACHO_LC_LOAD_DYLIB) return "LC_LOAD_DYLIB";
    if (command == MACHO_LC_ID_DYLIB) return "LC_ID_DYLIB";
    if (command == MACHO_LC_UUID) return "LC_UUID";
    if (command == MACHO_LC_CODE_SIGNATURE) return "LC_CODE_SIGNATURE";
    if (command == MACHO_LC_DYLD_INFO) return "LC_DYLD_INFO";
    if (command == MACHO_LC_DYLD_INFO_ONLY) return "LC_DYLD_INFO_ONLY";
    if (command == MACHO_LC_FUNCTION_STARTS) return "LC_FUNCTION_STARTS";
    if (command == MACHO_LC_DATA_IN_CODE) return "LC_DATA_IN_CODE";
    if (command == MACHO_LC_SOURCE_VERSION) return "LC_SOURCE_VERSION";
    if (command == MACHO_LC_DYLD_EXPORTS_TRIE) return "LC_DYLD_EXPORTS_TRIE";
    if (command == MACHO_LC_DYLD_CHAINED_FIXUPS) return "LC_DYLD_CHAINED_FIXUPS";
    if (command == MACHO_LC_MAIN) return "LC_MAIN";
    if (command == MACHO_LC_BUILD_VERSION) return "LC_BUILD_VERSION";
    return "LC_OTHER";
}

static const char *macho_build_tool_name(unsigned int tool) {
    if (tool == 1U) return "clang";
    if (tool == 2U) return "swift";
    if (tool == 3U) return "ld";
    return "unknown";
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

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, optimize("O0")))
#endif
static void write_elf_program_type_name(unsigned int type) {
    char token[16];
    size_t length = 0U;

    if (type == 0U) {
        token[length++] = 'N'; token[length++] = 'U'; token[length++] = 'L'; token[length++] = 'L';
    } else if (type == ELF_PT_LOAD) {
        token[length++] = 'L'; token[length++] = 'O'; token[length++] = 'A'; token[length++] = 'D';
    } else if (type == ELF_PT_DYNAMIC) {
        token[length++] = 'D'; token[length++] = 'Y'; token[length++] = 'N'; token[length++] = 'A'; token[length++] = 'M'; token[length++] = 'I'; token[length++] = 'C';
    } else if (type == ELF_PT_INTERP) {
        token[length++] = 'I'; token[length++] = 'N'; token[length++] = 'T'; token[length++] = 'E'; token[length++] = 'R'; token[length++] = 'P';
    } else if (type == ELF_PT_NOTE) {
        token[length++] = 'N'; token[length++] = 'O'; token[length++] = 'T'; token[length++] = 'E';
    } else if (type == 5U) {
        token[length++] = 'S'; token[length++] = 'H'; token[length++] = 'L'; token[length++] = 'I'; token[length++] = 'B';
    } else if (type == ELF_PT_PHDR) {
        token[length++] = 'P'; token[length++] = 'H'; token[length++] = 'D'; token[length++] = 'R';
    } else if (type == 0x6474e550U) {
        token[length++] = 'G'; token[length++] = 'N'; token[length++] = 'U'; token[length++] = '_'; token[length++] = 'E'; token[length++] = 'H'; token[length++] = '_'; token[length++] = 'F'; token[length++] = 'R'; token[length++] = 'A'; token[length++] = 'M'; token[length++] = 'E';
    } else if (type == 0x6474e551U) {
        token[length++] = 'G'; token[length++] = 'N'; token[length++] = 'U'; token[length++] = '_'; token[length++] = 'S'; token[length++] = 'T'; token[length++] = 'A'; token[length++] = 'C'; token[length++] = 'K';
    } else if (type == 0x6474e552U) {
        token[length++] = 'G'; token[length++] = 'N'; token[length++] = 'U'; token[length++] = '_'; token[length++] = 'R'; token[length++] = 'E'; token[length++] = 'L'; token[length++] = 'R'; token[length++] = 'O';
    } else if (type == 0x6474e553U) {
        token[length++] = 'G'; token[length++] = 'N'; token[length++] = 'U'; token[length++] = '_'; token[length++] = 'P'; token[length++] = 'R'; token[length++] = 'O'; token[length++] = 'P'; token[length++] = 'E'; token[length++] = 'R'; token[length++] = 'T'; token[length++] = 'Y';
    } else {
        token[length++] = 'O'; token[length++] = 'T'; token[length++] = 'H'; token[length++] = 'E'; token[length++] = 'R';
    }
    (void)rt_write_all(1, token, length);
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
    unsigned long long absolute_offset = readelf_object_base + offset;
    if (readelf_object_size != 0ULL && ((unsigned long long)size > readelf_object_size || offset > readelf_object_size - (unsigned long long)size)) {
        return -1;
    }
    if (platform_seek(fd, (long long)absolute_offset, PLATFORM_SEEK_SET) < 0) {
        return -1;
    }
    return archive_read_exact(fd, buffer, size);
}

static int read_file_region(int fd, unsigned long long offset, unsigned char *buffer, size_t size) {
    if (platform_seek(fd, (long long)offset, PLATFORM_SEEK_SET) < 0) {
        return -1;
    }
    return archive_read_exact(fd, buffer, size);
}

static void set_object_window(unsigned long long base, unsigned long long size) {
    readelf_object_base = base;
    readelf_object_size = size;
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

static int parse_macho_fat_header(int fd, MachFatInfo *fat) {
    unsigned char header[8];
    unsigned int index;
    long long file_size;

    file_size = platform_seek(fd, 0, PLATFORM_SEEK_END);
    if (file_size < 8) return -1;
    if (read_file_region(fd, 0ULL, header, sizeof(header)) != 0) return -1;
    fat->magic = read_u32_be_local(header + 0);
    if (fat->magic != MACHO_FAT_MAGIC && fat->magic != MACHO_FAT_MAGIC_64) return -1;
    fat->arch_count = read_u32_be_local(header + 4);
    fat->is_64 = fat->magic == MACHO_FAT_MAGIC_64;
    if (fat->arch_count > READELF_MAX_MACHO_FAT_ARCHES) return -1;
    for (index = 0U; index < fat->arch_count; ++index) {
        unsigned char raw[32];
        unsigned long long entry_offset = 8ULL + (unsigned long long)index * (fat->is_64 ? 32ULL : 20ULL);
        if (read_file_region(fd, entry_offset, raw, fat->is_64 ? 32U : 20U) != 0) return -1;
        fat->arches[index].cputype = read_u32_be_local(raw + 0);
        fat->arches[index].cpusubtype = read_u32_be_local(raw + 4);
        fat->arches[index].offset = fat->is_64 ? read_u64_be_local(raw + 8) : (unsigned long long)read_u32_be_local(raw + 8);
        fat->arches[index].size = fat->is_64 ? read_u64_be_local(raw + 16) : (unsigned long long)read_u32_be_local(raw + 12);
        fat->arches[index].align = fat->is_64 ? read_u32_be_local(raw + 24) : read_u32_be_local(raw + 16);
        if (fat->arches[index].offset > (unsigned long long)file_size || fat->arches[index].size > (unsigned long long)file_size || fat->arches[index].offset + fat->arches[index].size > (unsigned long long)file_size) {
            return -1;
        }
    }
    return 0;
}

static int macho_fat_choose_slice(const MachFatInfo *fat, unsigned int *index_out) {
    unsigned int index;
    for (index = 0U; index < fat->arch_count; ++index) {
        if (fat->arches[index].cputype == MACHO_CPU_TYPE_ARM64) {
            *index_out = index;
            return 0;
        }
    }
    if (fat->arch_count != 0U) {
        *index_out = 0U;
        return 0;
    }
    return -1;
}

static void print_macho_fat_header(const MachFatInfo *fat) {
    unsigned int index;
    rt_write_line(1, "Mach-O Universal Binary:");
    rt_write_cstr(1, "  Magic: ");
    write_hex_value((unsigned long long)fat->magic);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  Architectures: ");
    rt_write_uint(1, (unsigned long long)fat->arch_count);
    rt_write_char(1, '\n');
    for (index = 0U; index < fat->arch_count; ++index) {
        rt_write_cstr(1, "  [");
        rt_write_uint(1, (unsigned long long)index);
        rt_write_cstr(1, "] ");
        rt_write_cstr(1, macho_short_arch_name(fat->arches[index].cputype, fat->arches[index].cpusubtype));
        rt_write_cstr(1, " cputype=");
        rt_write_uint(1, (unsigned long long)fat->arches[index].cputype);
        rt_write_cstr(1, " cpusubtype=");
        rt_write_uint(1, (unsigned long long)(fat->arches[index].cpusubtype & ~MACHO_CPU_SUBTYPE_MASK));
        rt_write_cstr(1, " subtype_name=");
        rt_write_cstr(1, macho_cpu_subtype_name(fat->arches[index].cputype, fat->arches[index].cpusubtype));
        rt_write_cstr(1, " capabilities=");
        write_hex_value((unsigned long long)macho_cpu_capabilities(fat->arches[index].cpusubtype));
        rt_write_cstr(1, " offset=");
        write_hex_value(fat->arches[index].offset);
        rt_write_cstr(1, " size=");
        write_hex_value(fat->arches[index].size);
        rt_write_cstr(1, " align=2^");
        rt_write_uint(1, (unsigned long long)fat->arches[index].align);
        rt_write_char(1, '\n');
    }
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
                sections[section_count].reserved1 = read_u32_le_local(raw + 68);
                sections[section_count].reserved2 = read_u32_le_local(raw + 72);
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

static int load_macho_linkedit_data_command(int fd, const MachHeaderInfo *header, unsigned int wanted_command, MachLinkeditDataInfo *info) {
    unsigned int command_index;
    unsigned long long command_offset = 32ULL;

    info->dataoff = 0U;
    info->datasize = 0U;
    info->present = 0;
    if (header->ncmds > READELF_MAX_MACHO_COMMANDS) return -1;
    for (command_index = 0U; command_index < header->ncmds; ++command_index) {
        unsigned char command_data[16];
        unsigned int command;
        unsigned int command_size;
        if (read_region(fd, command_offset, command_data, 8U) != 0) return -1;
        command = read_u32_le_local(command_data + 0);
        command_size = read_u32_le_local(command_data + 4);
        if (command_size < 8U) return -1;
        if (command == wanted_command && command_size >= 16U) {
            if (read_region(fd, command_offset, command_data, sizeof(command_data)) != 0) return -1;
            info->dataoff = read_u32_le_local(command_data + 8);
            info->datasize = read_u32_le_local(command_data + 12);
            info->present = 1;
            return 0;
        }
        command_offset += (unsigned long long)command_size;
    }
    return 0;
}

static int load_macho_main_entryoff(int fd, const MachHeaderInfo *header, unsigned long long *entryoff_out) {
    unsigned int command_index;
    unsigned long long command_offset = 32ULL;

    *entryoff_out = 0ULL;
    if (header->ncmds > READELF_MAX_MACHO_COMMANDS) return -1;
    for (command_index = 0U; command_index < header->ncmds; ++command_index) {
        unsigned char command_data[24];
        unsigned int command;
        unsigned int command_size;
        if (read_region(fd, command_offset, command_data, 8U) != 0) return -1;
        command = read_u32_le_local(command_data + 0);
        command_size = read_u32_le_local(command_data + 4);
        if (command_size < 8U) return -1;
        if (command == MACHO_LC_MAIN && command_size >= 24U) {
            if (read_region(fd, command_offset, command_data, sizeof(command_data)) != 0) return -1;
            *entryoff_out = read_u64_le_local(command_data + 8);
            return 0;
        }
        command_offset += (unsigned long long)command_size;
    }
    return 0;
}

static unsigned int count_macho_dylibs(int fd, const MachHeaderInfo *header) {
    unsigned int command_index;
    unsigned long long command_offset = 32ULL;
    unsigned int count = 0U;
    if (header->ncmds > READELF_MAX_MACHO_COMMANDS) return 0U;
    for (command_index = 0U; command_index < header->ncmds; ++command_index) {
        unsigned char command_data[8];
        unsigned int command;
        unsigned int command_size;
        if (read_region(fd, command_offset, command_data, sizeof(command_data)) != 0) return count;
        command = read_u32_le_local(command_data + 0);
        command_size = read_u32_le_local(command_data + 4);
        if (command_size < 8U) return count;
        if (command == MACHO_LC_LOAD_DYLIB) count += 1U;
        command_offset += (unsigned long long)command_size;
    }
    return count;
}

static const char *macho_pointer_format_name(unsigned int pointer_format) {
    if (pointer_format == MACHO_DYLD_CHAINED_PTR_ARM64E) return "arm64e authenticated/rebase-bind";
    if (pointer_format == MACHO_DYLD_CHAINED_PTR_64) return "64-bit rebase-bind";
    if (pointer_format == MACHO_DYLD_CHAINED_PTR_64_OFFSET) return "64-bit offset rebase-bind";
    if (pointer_format == MACHO_DYLD_CHAINED_PTR_ARM64E_KERNEL) return "arm64e kernel authenticated";
    if (pointer_format == MACHO_DYLD_CHAINED_PTR_ARM64E_USERLAND) return "arm64e userland authenticated";
    if (pointer_format == MACHO_DYLD_CHAINED_PTR_ARM64E_USERLAND24) return "arm64e userland24 authenticated";
    return "unknown";
}

static void write_macho_prot(unsigned int prot) {
    rt_write_char(1, (prot & MACHO_VM_PROT_READ) != 0U ? 'r' : '-');
    rt_write_char(1, (prot & MACHO_VM_PROT_WRITE) != 0U ? 'w' : '-');
    rt_write_char(1, (prot & MACHO_VM_PROT_EXECUTE) != 0U ? 'x' : '-');
}

static void sha_update_u32(CryptoSha256Context *sha, unsigned int value) {
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xffU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xffU);
    crypto_sha256_update(sha, bytes, sizeof(bytes));
}

static void sha_update_u64(CryptoSha256Context *sha, unsigned long long value) {
    unsigned char bytes[8];
    unsigned int i;
    for (i = 0U; i < 8U; ++i) bytes[i] = (unsigned char)((value >> (i * 8U)) & 0xffU);
    crypto_sha256_update(sha, bytes, sizeof(bytes));
}

static int hash_macho_load_commands(int fd, const MachHeaderInfo *header, char *out, size_t out_size) {
    CryptoSha256Context sha;
    unsigned char digest[CRYPTO_SHA256_DIGEST_SIZE];
    unsigned long long remaining = (unsigned long long)header->sizeofcmds;
    unsigned long long offset = 32ULL;

    crypto_sha256_init(&sha);
    while (remaining != 0ULL) {
        unsigned char buffer[512];
        size_t chunk = remaining > (unsigned long long)sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        if (read_region(fd, offset, buffer, chunk) != 0) return -1;
        crypto_sha256_update(&sha, buffer, chunk);
        offset += (unsigned long long)chunk;
        remaining -= (unsigned long long)chunk;
    }
    crypto_sha256_final(&sha, digest);
    bytes_to_hex(digest, sizeof(digest), out, out_size);
    return 0;
}

static void hash_macho_layout(const MachSegmentInfo *segments, unsigned int segment_count, const MachSectionInfo *sections, unsigned int section_count, char *out, size_t out_size) {
    CryptoSha256Context sha;
    unsigned char digest[CRYPTO_SHA256_DIGEST_SIZE];
    unsigned int i;

    crypto_sha256_init(&sha);
    for (i = 0U; i < segment_count; ++i) {
        crypto_sha256_update(&sha, (const unsigned char *)segments[i].name, rt_strlen(segments[i].name));
        sha_update_u64(&sha, segments[i].vmaddr);
        sha_update_u64(&sha, segments[i].vmsize);
        sha_update_u64(&sha, segments[i].fileoff);
        sha_update_u64(&sha, segments[i].filesize);
        sha_update_u32(&sha, segments[i].initprot);
        sha_update_u32(&sha, segments[i].flags);
    }
    for (i = 0U; i < section_count; ++i) {
        crypto_sha256_update(&sha, (const unsigned char *)sections[i].segment, rt_strlen(sections[i].segment));
        crypto_sha256_update(&sha, (const unsigned char *)sections[i].section, rt_strlen(sections[i].section));
        sha_update_u64(&sha, sections[i].addr);
        sha_update_u64(&sha, sections[i].size);
        sha_update_u32(&sha, sections[i].offset);
        sha_update_u32(&sha, sections[i].flags);
    }
    crypto_sha256_final(&sha, digest);
    bytes_to_hex(digest, sizeof(digest), out, out_size);
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
    signature->slot_count = 0U;
    signature->present = 0;
    signature->has_code_directory = 0;
    signature->structure_valid = 0;
    signature->hashes_verified = 0;
    signature->identifier[0] = '\0';
    signature->cdhash[0] = '\0';
    signature->cdhash_full[0] = '\0';
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
    unsigned char raw[44];
    long long file_size;
    unsigned int index;

    if (load_macho_code_signature_command(fd, header, signature) != 0) {
        return -1;
    }
    if (!signature->present) {
        return 0;
    }
    file_size = platform_seek(fd, 0, PLATFORM_SEEK_END);
    if (readelf_object_size != 0ULL) file_size = (long long)readelf_object_size;
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
        if (signature->slot_count < READELF_MAX_CODE_SIGNATURE_SLOTS && blob_offset <= signature->superblob_length - 8U && read_region(fd, (unsigned long long)signature->dataoff + (unsigned long long)blob_offset, raw, 8U) == 0) {
            signature->slots[signature->slot_count].type = slot_type;
            signature->slots[signature->slot_count].offset = blob_offset;
            signature->slots[signature->slot_count].magic = read_u32_be_local(raw + 0);
            signature->slots[signature->slot_count].length = read_u32_be_local(raw + 4);
            signature->slot_count += 1U;
        }
        if (slot_type == MACHO_CODE_SIGNATURE_SLOT_CODEDIRECTORY) {
            signature->code_directory_offset = blob_offset;
        }
    }
    if (signature->superblob_length < 44U || signature->code_directory_offset == 0U || signature->code_directory_offset > signature->superblob_length - 44U) {
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
    if (signature->code_directory_length == 0U || signature->code_directory_length > signature->superblob_length ||
        signature->code_directory_offset > signature->superblob_length - signature->code_directory_length ||
        (signature->hash_size != 0U && signature->n_code_slots > (0xffffffffU / signature->hash_size)) ||
        signature->hash_offset > signature->code_directory_length ||
        (signature->hash_size != 0U && signature->n_code_slots * signature->hash_size > signature->code_directory_length - signature->hash_offset) ||
        signature->code_limit > signature->dataoff) {
        rt_copy_string(signature->message, sizeof(signature->message), "invalid CodeDirectory bounds");
        return 0;
    }
    signature->structure_valid = 1;
    if (signature->hash_type == MACHO_CODE_SIGNATURE_HASH_SHA256) {
        CryptoSha256Context sha;
        unsigned char digest[CRYPTO_SHA256_DIGEST_SIZE];
        unsigned int remaining = signature->code_directory_length;
        unsigned long long digest_offset = (unsigned long long)signature->dataoff + (unsigned long long)signature->code_directory_offset;
        crypto_sha256_init(&sha);
        while (remaining != 0U) {
            unsigned char buffer[512];
            size_t chunk = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
            if (read_region(fd, digest_offset, buffer, chunk) != 0) return -1;
            crypto_sha256_update(&sha, buffer, chunk);
            digest_offset += (unsigned long long)chunk;
            remaining -= (unsigned int)chunk;
        }
        crypto_sha256_final(&sha, digest);
        bytes_to_hex(digest, sizeof(digest), signature->cdhash_full, sizeof(signature->cdhash_full));
        bytes_to_hex(digest, 20U, signature->cdhash, sizeof(signature->cdhash));
    }
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
    if (info->shnum == 0U) {
        rt_write_line(1, " bytes (ignored; no section headers)");
    } else {
        rt_write_line(1, " bytes");
    }
    rt_write_cstr(1, "  Section header count: ");
    rt_write_uint(1, info->shnum);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "  Section header string table index: ");
    if (info->shnum == 0U) {
        rt_write_line(1, "ignored");
    } else {
        rt_write_uint(1, info->shstrndx);
        rt_write_char(1, '\n');
    }
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
    rt_write_cstr(1, " (");
    rt_write_cstr(1, macho_cpu_subtype_name(info->cputype, info->cpusubtype));
    rt_write_cstr(1, ", capabilities=");
    write_hex_value((unsigned long long)macho_cpu_capabilities(info->cpusubtype));
    rt_write_char(1, ')');
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

static void print_macho_source_version(unsigned long long version) {
    rt_write_uint(1, (version >> 40U) & 0xffffffULL);
    rt_write_char(1, '.');
    rt_write_uint(1, (version >> 30U) & 0x3ffULL);
    rt_write_char(1, '.');
    rt_write_uint(1, (version >> 20U) & 0x3ffULL);
    rt_write_char(1, '.');
    rt_write_uint(1, (version >> 10U) & 0x3ffULL);
    rt_write_char(1, '.');
    rt_write_uint(1, version & 0x3ffULL);
}

static void write_uuid_text(const unsigned char *uuid) {
    char text[37];
    static const char digits[] = "0123456789abcdef";
    size_t i;
    size_t out = 0U;
    for (i = 0U; i < 16U; ++i) {
        if (i == 4U || i == 6U || i == 8U || i == 10U) text[out++] = '-';
        text[out++] = digits[(uuid[i] >> 4U) & 0x0fU];
        text[out++] = digits[uuid[i] & 0x0fU];
    }
    text[out] = '\0';
    rt_write_cstr(1, text);
}

static void append_uint_text(char *text, size_t text_size, unsigned long long value) {
    char reversed[32];
    char digits[32];
    size_t count = 0U;
    size_t i;
    if (text_size == 0U) return;
    if (value == 0ULL) {
        rt_copy_string(text + rt_strlen(text), text_size - rt_strlen(text), "0");
        return;
    }
    while (value != 0ULL && count < sizeof(reversed)) {
        reversed[count++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    for (i = 0U; i < count; ++i) digits[i] = reversed[count - 1U - i];
    digits[count] = '\0';
    rt_copy_string(text + rt_strlen(text), text_size - rt_strlen(text), digits);
}

static int json_field_version_triplet(const char *name, unsigned int version) {
    char text[32];
    text[0] = '\0';
    append_uint_text(text, sizeof(text), (unsigned long long)((version >> 16U) & 0xffffU));
    rt_copy_string(text + rt_strlen(text), sizeof(text) - rt_strlen(text), ".");
    append_uint_text(text, sizeof(text), (unsigned long long)((version >> 8U) & 0xffU));
    rt_copy_string(text + rt_strlen(text), sizeof(text) - rt_strlen(text), ".");
    append_uint_text(text, sizeof(text), (unsigned long long)(version & 0xffU));
    return json_field_string(name, text);
}

static int json_field_source_version(const char *name, unsigned long long version) {
    char text[64];
    text[0] = '\0';
    append_uint_text(text, sizeof(text), (version >> 40U) & 0xffffffULL);
    rt_copy_string(text + rt_strlen(text), sizeof(text) - rt_strlen(text), ".");
    append_uint_text(text, sizeof(text), (version >> 30U) & 0x3ffULL);
    rt_copy_string(text + rt_strlen(text), sizeof(text) - rt_strlen(text), ".");
    append_uint_text(text, sizeof(text), (version >> 20U) & 0x3ffULL);
    rt_copy_string(text + rt_strlen(text), sizeof(text) - rt_strlen(text), ".");
    append_uint_text(text, sizeof(text), (version >> 10U) & 0x3ffULL);
    rt_copy_string(text + rt_strlen(text), sizeof(text) - rt_strlen(text), ".");
    append_uint_text(text, sizeof(text), version & 0x3ffULL);
    return json_field_string(name, text);
}

static int json_field_uuid(const char *name, const unsigned char *uuid) {
    char text[37];
    static const char digits[] = "0123456789abcdef";
    size_t i;
    size_t out = 0U;
    for (i = 0U; i < 16U; ++i) {
        if (i == 4U || i == 6U || i == 8U || i == 10U) text[out++] = '-';
        text[out++] = digits[(uuid[i] >> 4U) & 0x0fU];
        text[out++] = digits[uuid[i] & 0x0fU];
    }
    text[out] = '\0';
    return json_field_string(name, text);
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
            rt_write_cstr(1, " maxprot=");
            write_hex_value((unsigned long long)read_u32_le_local(command_data + 56));
            rt_write_cstr(1, " initprot=");
            write_hex_value((unsigned long long)read_u32_le_local(command_data + 60));
            rt_write_cstr(1, " nsects=");
            rt_write_uint(1, (unsigned long long)read_u32_le_local(command_data + 64));
            rt_write_cstr(1, " flags=");
            write_hex_value((unsigned long long)read_u32_le_local(command_data + 68));
        } else if (command == MACHO_LC_SYMTAB && command_size >= 24U && read_region(fd, command_offset, command_data, 24U) == 0) {
            rt_write_cstr(1, " symoff=");
            write_hex_value((unsigned long long)read_u32_le_local(command_data + 8));
            rt_write_cstr(1, " nsyms=");
            rt_write_uint(1, (unsigned long long)read_u32_le_local(command_data + 12));
            rt_write_cstr(1, " stroff=");
            write_hex_value((unsigned long long)read_u32_le_local(command_data + 16));
            rt_write_cstr(1, " strsize=");
            write_hex_value((unsigned long long)read_u32_le_local(command_data + 20));
        } else if (command == MACHO_LC_DYSYMTAB && command_size >= 80U && read_region(fd, command_offset, command_data, 80U) == 0) {
            rt_write_cstr(1, " ilocalsym="); rt_write_uint(1, (unsigned long long)read_u32_le_local(command_data + 8));
            rt_write_cstr(1, " nlocalsym="); rt_write_uint(1, (unsigned long long)read_u32_le_local(command_data + 12));
            rt_write_cstr(1, " iextdefsym="); rt_write_uint(1, (unsigned long long)read_u32_le_local(command_data + 16));
            rt_write_cstr(1, " nextdefsym="); rt_write_uint(1, (unsigned long long)read_u32_le_local(command_data + 20));
            rt_write_cstr(1, " iundefsym="); rt_write_uint(1, (unsigned long long)read_u32_le_local(command_data + 24));
            rt_write_cstr(1, " nundefsym="); rt_write_uint(1, (unsigned long long)read_u32_le_local(command_data + 28));
            rt_write_cstr(1, " indirectsymoff="); write_hex_value((unsigned long long)read_u32_le_local(command_data + 56));
            rt_write_cstr(1, " nindirectsyms="); rt_write_uint(1, (unsigned long long)read_u32_le_local(command_data + 60));
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
        } else if ((command == MACHO_LC_DYLD_CHAINED_FIXUPS || command == MACHO_LC_DYLD_EXPORTS_TRIE || command == MACHO_LC_FUNCTION_STARTS || command == MACHO_LC_DATA_IN_CODE) && command_size >= 16U && read_region(fd, command_offset, command_data, 16U) == 0) {
            rt_write_cstr(1, " dataoff=");
            write_hex_value((unsigned long long)read_u32_le_local(command_data + 8));
            rt_write_cstr(1, " datasize=");
            write_hex_value((unsigned long long)read_u32_le_local(command_data + 12));
        } else if ((command == MACHO_LC_DYLD_INFO || command == MACHO_LC_DYLD_INFO_ONLY) && command_size >= 48U && read_region(fd, command_offset, command_data, 48U) == 0) {
            rt_write_cstr(1, " rebase_off="); write_hex_value((unsigned long long)read_u32_le_local(command_data + 8));
            rt_write_cstr(1, " rebase_size="); write_hex_value((unsigned long long)read_u32_le_local(command_data + 12));
            rt_write_cstr(1, " bind_off="); write_hex_value((unsigned long long)read_u32_le_local(command_data + 16));
            rt_write_cstr(1, " bind_size="); write_hex_value((unsigned long long)read_u32_le_local(command_data + 20));
            rt_write_cstr(1, " export_off="); write_hex_value((unsigned long long)read_u32_le_local(command_data + 40));
            rt_write_cstr(1, " export_size="); write_hex_value((unsigned long long)read_u32_le_local(command_data + 44));
        } else if (command == MACHO_LC_UUID && command_size >= 24U && read_region(fd, command_offset, command_data, 24U) == 0) {
            rt_write_cstr(1, " uuid=");
            write_uuid_text(command_data + 8);
        } else if (command == MACHO_LC_SOURCE_VERSION && command_size >= 16U && read_region(fd, command_offset, command_data, 16U) == 0) {
            rt_write_cstr(1, " version=");
            print_macho_source_version(read_u64_le_local(command_data + 8));
        } else if (command == MACHO_LC_BUILD_VERSION && command_size >= 24U && read_region(fd, command_offset, command_data, 24U) == 0) {
            unsigned int platform = read_u32_le_local(command_data + 8);
            unsigned int ntools = read_u32_le_local(command_data + 20);
            rt_write_cstr(1, " platform=");
            rt_write_cstr(1, macho_platform_name(platform));
            rt_write_cstr(1, " minos=");
            print_macho_version(read_u32_le_local(command_data + 12));
            rt_write_cstr(1, " sdk=");
            print_macho_version(read_u32_le_local(command_data + 16));
            rt_write_cstr(1, " ntools=");
            rt_write_uint(1, (unsigned long long)ntools);
            if (ntools > 0U && command_size >= 32U && read_region(fd, command_offset, command_data, 32U) == 0) {
                rt_write_cstr(1, " tool=");
                rt_write_cstr(1, macho_build_tool_name(read_u32_le_local(command_data + 24)));
                rt_write_cstr(1, " tool_version=");
                print_macho_version(read_u32_le_local(command_data + 28));
            }
        } else if ((command == MACHO_LC_LOAD_DYLINKER || command == MACHO_LC_LOAD_DYLIB || command == MACHO_LC_ID_DYLIB) && command_size > 12U && command_size <= sizeof(command_data) && read_region(fd, command_offset, command_data, command_size) == 0) {
            unsigned int name_offset = read_u32_le_local(command_data + 8);
            if (name_offset < command_size) {
                char path[128];
                copy_fixed_name(path, sizeof(path), command_data + name_offset, (size_t)(command_size - name_offset));
                rt_write_cstr(1, command == MACHO_LC_LOAD_DYLINKER ? " path=" : " name=");
                rt_write_cstr(1, path);
            }
            if (command == MACHO_LC_LOAD_DYLIB || command == MACHO_LC_ID_DYLIB) {
                rt_write_cstr(1, " current_version=");
                print_macho_version(read_u32_le_local(command_data + 16));
                rt_write_cstr(1, " compatibility_version=");
                print_macho_version(read_u32_le_local(command_data + 20));
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
        rt_write_cstr(1, " reserved1=");
        write_hex_value((unsigned long long)sections[i].reserved1);
        rt_write_cstr(1, " reserved2=");
        write_hex_value((unsigned long long)sections[i].reserved2);
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
    if (signature->slot_count != 0U) {
        unsigned int slot_index;
        rt_write_line(1, "  SuperBlob slots:");
        for (slot_index = 0U; slot_index < signature->slot_count; ++slot_index) {
            rt_write_cstr(1, "    [");
            rt_write_uint(1, (unsigned long long)slot_index);
            rt_write_cstr(1, "] type=");
            rt_write_cstr(1, macho_code_signature_slot_name(signature->slots[slot_index].type));
            rt_write_cstr(1, "(");
            write_hex_value((unsigned long long)signature->slots[slot_index].type);
            rt_write_cstr(1, ") offset=");
            write_hex_value((unsigned long long)signature->slots[slot_index].offset);
            rt_write_cstr(1, " length=");
            rt_write_uint(1, (unsigned long long)signature->slots[slot_index].length);
            rt_write_cstr(1, " magic=");
            write_hex_value((unsigned long long)signature->slots[slot_index].magic);
            rt_write_char(1, '\n');
        }
    }
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
    if (signature->cdhash[0] != '\0') {
        rt_write_cstr(1, "  CDHash: ");
        rt_write_line(1, signature->cdhash);
        rt_write_cstr(1, "  CDHashFull: ");
        rt_write_line(1, signature->cdhash_full);
    }
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

static void print_macho_signature_details(int fd, const MachCodeSignatureInfo *signature) {
    unsigned int slot_index;

    print_macho_code_signature(signature);
    if (!signature->has_code_directory) return;
    rt_write_line(1, "Mach-O Signature Details:");
    rt_write_cstr(1, "  CodeDirectory hash offset=");
    write_hex_value((unsigned long long)signature->hash_offset);
    rt_write_cstr(1, " ident offset=");
    write_hex_value((unsigned long long)signature->ident_offset);
    rt_write_char(1, '\n');
    if (signature->n_special_slots != 0U && signature->hash_type == MACHO_CODE_SIGNATURE_HASH_SHA256 && signature->hash_size == CRYPTO_SHA256_DIGEST_SIZE) {
        rt_write_line(1, "  Special slot hashes:");
        for (slot_index = 1U; slot_index <= signature->n_special_slots; ++slot_index) {
            unsigned long long slot_offset;
            unsigned char hash[CRYPTO_SHA256_DIGEST_SIZE];
            char hash_text[65];
            if (signature->hash_offset < slot_index * signature->hash_size) break;
            slot_offset = (unsigned long long)signature->dataoff + (unsigned long long)signature->code_directory_offset + (unsigned long long)signature->hash_offset - ((unsigned long long)slot_index * (unsigned long long)signature->hash_size);
            if (read_region(fd, slot_offset, hash, sizeof(hash)) != 0) break;
            bytes_to_hex(hash, sizeof(hash), hash_text, sizeof(hash_text));
            rt_write_cstr(1, "    -");
            rt_write_uint(1, (unsigned long long)slot_index);
            rt_write_cstr(1, ": ");
            rt_write_line(1, hash_text);
        }
    }
    for (slot_index = 0U; slot_index < signature->slot_count; ++slot_index) {
        if (signature->slots[slot_index].type == MACHO_CODE_SIGNATURE_SLOT_CMS_SIGNATURE) {
            rt_write_cstr(1, "  CMS signature blob: offset=");
            write_hex_value((unsigned long long)signature->slots[slot_index].offset);
            rt_write_cstr(1, " length=");
            rt_write_uint(1, (unsigned long long)signature->slots[slot_index].length);
            rt_write_line(1, " (certificate authority and signed-time parsing not implemented)");
        }
        if (signature->slots[slot_index].type == MACHO_CODE_SIGNATURE_SLOT_REQUIREMENTS) {
            unsigned char raw[12];
            unsigned long long blob_offset = (unsigned long long)signature->dataoff + (unsigned long long)signature->slots[slot_index].offset;
            rt_write_cstr(1, "  Requirements blob: offset=");
            write_hex_value((unsigned long long)signature->slots[slot_index].offset);
            rt_write_cstr(1, " length=");
            rt_write_uint(1, (unsigned long long)signature->slots[slot_index].length);
            if (signature->slots[slot_index].length >= 12U && read_region(fd, blob_offset, raw, sizeof(raw)) == 0) {
                rt_write_cstr(1, " count=");
                rt_write_uint(1, (unsigned long long)read_u32_be_local(raw + 8));
            }
            rt_write_char(1, '\n');
        }
    }
}

static int json_macho_signature_detail_event(const char *path, const MachCodeSignatureInfo *signature) {
    if (tool_json_begin_event(1, "readelf", "stdout", "macho_signature_detail") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
    if (rt_write_cstr(1, "\"file\":") != 0 || tool_json_write_string(1, path) != 0) return -1;
    if (json_field_bool("present", signature->present) != 0) return -1;
    if (json_field_string("identifier", signature->identifier) != 0) return -1;
    if (json_field_uint("code_directory_version", signature->code_directory_version) != 0) return -1;
    if (json_field_uint("code_directory_flags", signature->code_directory_flags) != 0) return -1;
    if (json_field_string("cdhash", signature->cdhash) != 0) return -1;
    if (json_field_string("cdhash_full", signature->cdhash_full) != 0) return -1;
    if (json_field_uint("special_slots", signature->n_special_slots) != 0) return -1;
    if (json_field_uint("code_slots", signature->n_code_slots) != 0) return -1;
    if (json_field_uint("hash_offset", signature->hash_offset) != 0) return -1;
    if (json_field_uint("ident_offset", signature->ident_offset) != 0) return -1;
    if (json_field_bool("cms_parsed", 0) != 0) return -1;
    if (json_field_string("cms_note", "CMS blob is sized and located; certificate chain and signed time are not parsed") != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int json_macho_map_segment_event(const char *path, unsigned int index, const MachSegmentInfo *segment) {
    if (tool_json_begin_event(1, "readelf", "stdout", "macho_map_segment") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
    if (rt_write_cstr(1, "\"file\":") != 0 || tool_json_write_string(1, path) != 0) return -1;
    if (json_field_uint("index", index) != 0) return -1;
    if (json_field_string("segment", segment->name) != 0) return -1;
    if (json_field_uint("file_offset", segment->fileoff) != 0) return -1;
    if (json_field_uint("file_size", segment->filesize) != 0) return -1;
    if (json_field_uint("vmaddr", segment->vmaddr) != 0) return -1;
    if (json_field_uint("vmsize", segment->vmsize) != 0) return -1;
    if (json_field_uint("initprot", segment->initprot) != 0) return -1;
    if (json_field_uint("maxprot", segment->maxprot) != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static void print_macho_map(const MachSegmentInfo *segments, unsigned int segment_count, const MachSectionInfo *sections, unsigned int section_count, unsigned long long slice_base) {
    unsigned int segment_index;
    unsigned int section_index;

    rt_write_line(1, "Mach-O File/VM Map:");
    rt_write_cstr(1, "  Slice file base: ");
    write_hex_value(slice_base);
    rt_write_char(1, '\n');
    for (segment_index = 0U; segment_index < segment_count; ++segment_index) {
        const MachSegmentInfo *segment = &segments[segment_index];
        rt_write_cstr(1, "  Segment ");
        rt_write_cstr(1, segment->name[0] != '\0' ? segment->name : "(none)");
        rt_write_cstr(1, " file=[");
        write_hex_value(segment->fileoff);
        rt_write_cstr(1, ".. ");
        write_hex_value(segment->fileoff + segment->filesize);
        rt_write_cstr(1, ") vm=[");
        write_hex_value(segment->vmaddr);
        rt_write_cstr(1, ".. ");
        write_hex_value(segment->vmaddr + segment->vmsize);
        rt_write_cstr(1, ") prot=");
        write_macho_prot(segment->initprot);
        rt_write_cstr(1, " max=");
        write_macho_prot(segment->maxprot);
        if (segment->filesize == 0ULL && segment->vmsize != 0ULL) rt_write_cstr(1, " zero-fill");
        rt_write_char(1, '\n');
        for (section_index = 0U; section_index < section_count; ++section_index) {
            const MachSectionInfo *section = &sections[section_index];
            if (rt_strcmp(section->segment, segment->name) != 0) continue;
            rt_write_cstr(1, "    Section ");
            rt_write_cstr(1, section->section);
            rt_write_cstr(1, " file=");
            write_hex_value((unsigned long long)section->offset);
            rt_write_cstr(1, " vm=");
            write_hex_value(section->addr);
            rt_write_cstr(1, " size=");
            write_hex_value(section->size);
            rt_write_cstr(1, " type=");
            rt_write_cstr(1, macho_section_type_name(section->flags & 0xffU));
            rt_write_char(1, '\n');
        }
    }
}

static int json_macho_map_events(const char *path, const MachSegmentInfo *segments, unsigned int segment_count, const MachSectionInfo *sections, unsigned int section_count) {
    unsigned int i;
    for (i = 0U; i < segment_count; ++i) {
        if (json_macho_map_segment_event(path, i, &segments[i]) != 0) return -1;
    }
    for (i = 0U; i < section_count; ++i) {
        if (json_macho_section_event(path, i, &sections[i]) != 0) return -1;
    }
    return 0;
}

static int print_macho_fixups(int fd, const MachHeaderInfo *header, const char *path) {
    MachLinkeditDataInfo chained;
    MachLinkeditDataInfo exports_trie;
    unsigned char raw[32];
    unsigned int fixups_version;
    unsigned int starts_offset;
    unsigned int imports_offset;
    unsigned int symbols_offset;
    unsigned int imports_count;
    unsigned int imports_format;
    unsigned int symbols_format;
    unsigned int segment_count;
    unsigned int segment_index;
    unsigned int page_start_count = 0U;

    (void)path;
    if (load_macho_linkedit_data_command(fd, header, MACHO_LC_DYLD_CHAINED_FIXUPS, &chained) != 0) return -1;
    if (load_macho_linkedit_data_command(fd, header, MACHO_LC_DYLD_EXPORTS_TRIE, &exports_trie) != 0) return -1;
    if (!chained.present) {
        rt_write_line(1, "Mach-O Chained Fixups: (none)");
        return 0;
    }
    rt_write_line(1, "Mach-O Chained Fixups:");
    rt_write_cstr(1, "  dataoff=");
    write_hex_value((unsigned long long)chained.dataoff);
    rt_write_cstr(1, " datasize=");
    write_hex_value((unsigned long long)chained.datasize);
    rt_write_char(1, '\n');
    if (chained.datasize < 28U || read_region(fd, (unsigned long long)chained.dataoff, raw, 28U) != 0) return -1;
    fixups_version = read_u32_le_local(raw + 0);
    starts_offset = read_u32_le_local(raw + 4);
    imports_offset = read_u32_le_local(raw + 8);
    symbols_offset = read_u32_le_local(raw + 12);
    imports_count = read_u32_le_local(raw + 16);
    imports_format = read_u32_le_local(raw + 20);
    symbols_format = read_u32_le_local(raw + 24);
    rt_write_cstr(1, "  Header: version="); rt_write_uint(1, (unsigned long long)fixups_version);
    rt_write_cstr(1, " starts_offset="); write_hex_value((unsigned long long)starts_offset);
    rt_write_cstr(1, " imports_offset="); write_hex_value((unsigned long long)imports_offset);
    rt_write_cstr(1, " symbols_offset="); write_hex_value((unsigned long long)symbols_offset);
    rt_write_cstr(1, " imports_count="); rt_write_uint(1, (unsigned long long)imports_count);
    rt_write_cstr(1, " imports_format="); rt_write_uint(1, (unsigned long long)imports_format);
    rt_write_cstr(1, " symbols_format="); rt_write_uint(1, (unsigned long long)symbols_format);
    rt_write_char(1, '\n');
    if (starts_offset + 4U > chained.datasize || read_region(fd, (unsigned long long)chained.dataoff + starts_offset, raw, 4U) != 0) return 0;
    segment_count = read_u32_le_local(raw);
    rt_write_cstr(1, "  Segment starts: ");
    rt_write_uint(1, (unsigned long long)segment_count);
    rt_write_char(1, '\n');
    for (segment_index = 0U; segment_index < segment_count && segment_index < READELF_MAX_MACHO_SEGMENTS; ++segment_index) {
        unsigned int info_offset;
        unsigned long long info_base;
        unsigned int size;
        unsigned int page_size;
        unsigned int pointer_format;
        unsigned long long segment_offset;
        unsigned int page_count;
        unsigned int page_index;
        if (starts_offset + 4U + ((segment_index + 1U) * 4U) > chained.datasize) break;
        if (read_region(fd, (unsigned long long)chained.dataoff + starts_offset + 4ULL + ((unsigned long long)segment_index * 4ULL), raw, 4U) != 0) break;
        info_offset = read_u32_le_local(raw);
        if (info_offset == 0U) continue;
        info_base = (unsigned long long)chained.dataoff + starts_offset + (unsigned long long)info_offset;
        if (read_region(fd, info_base, raw, 24U) != 0) break;
        size = read_u32_le_local(raw + 0);
        page_size = read_u16_le(raw + 4);
        pointer_format = read_u16_le(raw + 6);
        segment_offset = read_u64_le_local(raw + 8);
        page_count = read_u16_le(raw + 20);
        rt_write_cstr(1, "    ["); rt_write_uint(1, (unsigned long long)segment_index); rt_write_cstr(1, "] size="); rt_write_uint(1, (unsigned long long)size);
        rt_write_cstr(1, " page_size="); rt_write_uint(1, (unsigned long long)page_size);
        rt_write_cstr(1, " pointer_format="); rt_write_cstr(1, macho_pointer_format_name(pointer_format));
        rt_write_cstr(1, " segment_offset="); write_hex_value(segment_offset);
        rt_write_cstr(1, " page_count="); rt_write_uint(1, (unsigned long long)page_count);
        rt_write_char(1, '\n');
        for (page_index = 0U; page_index < page_count && page_index < 16U; ++page_index) {
            unsigned char page_raw[2];
            unsigned int start;
            unsigned long long chain_fileoff;
            unsigned int chain_index = 0U;
            if (22ULL + ((unsigned long long)(page_index + 1U) * 2ULL) > (unsigned long long)size) break;
            if (read_region(fd, info_base + 22ULL + ((unsigned long long)page_index * 2ULL), page_raw, sizeof(page_raw)) != 0) break;
            start = read_u16_le(page_raw);
            if (start != 0xffffU) page_start_count += 1U;
            rt_write_cstr(1, "      page["); rt_write_uint(1, (unsigned long long)page_index); rt_write_cstr(1, "] start="); write_hex_value((unsigned long long)start); rt_write_char(1, '\n');
            if (start == 0xffffU) continue;
            if ((start & 0x8000U) != 0U) {
                rt_write_line(1, "        multi-start overflow entries are not yet expanded");
                continue;
            }
            chain_fileoff = segment_offset + ((unsigned long long)page_index * (unsigned long long)page_size) + (unsigned long long)start;
            while (chain_index < 256U) {
                unsigned char pointer_raw[8];
                unsigned long long raw_pointer;
                unsigned int bind;
                unsigned int auth;
                unsigned long long next;
                if (chain_fileoff + 8ULL > readelf_object_size && readelf_object_size != 0ULL) break;
                if (read_region(fd, chain_fileoff, pointer_raw, sizeof(pointer_raw)) != 0) break;
                raw_pointer = read_u64_le_local(pointer_raw);
                bind = (unsigned int)((raw_pointer >> 62U) & 1ULL);
                auth = (unsigned int)((raw_pointer >> 63U) & 1ULL);
                next = (raw_pointer >> 51U) & 0x7ffULL;
                rt_write_cstr(1, "        fixup fileoff="); write_hex_value(chain_fileoff);
                rt_write_cstr(1, " raw="); write_hex_value(raw_pointer);
                rt_write_cstr(1, bind ? " bind" : " rebase");
                rt_write_cstr(1, auth ? " authenticated" : " unauthenticated");
                if (bind) {
                    unsigned int ordinal = (unsigned int)(raw_pointer & 0xffffULL);
                    rt_write_cstr(1, " ordinal="); rt_write_uint(1, (unsigned long long)ordinal);
                    if (imports_format == 1U && ordinal < imports_count) {
                        unsigned char import_raw[4];
                        unsigned int import_entry;
                        unsigned int name_offset;
                        char import_name[128];
                        if (read_region(fd, (unsigned long long)chained.dataoff + (unsigned long long)imports_offset + ((unsigned long long)ordinal * 4ULL), import_raw, sizeof(import_raw)) == 0) {
                            import_entry = read_u32_le_local(import_raw);
                            name_offset = import_entry >> 9U;
                            if (macho_copy_cstring_from_blob(fd,
                                                             (unsigned long long)chained.dataoff + (unsigned long long)symbols_offset + (unsigned long long)name_offset,
                                                             (unsigned long long)chained.datasize,
                                                             import_name,
                                                             sizeof(import_name)) == 0 && import_name[0] != '\0') {
                                rt_write_cstr(1, " symbol="); rt_write_cstr(1, import_name);
                            }
                        }
                    }
                } else if (auth) {
                    rt_write_cstr(1, " target="); write_hex_value(raw_pointer & 0xffffffffULL);
                    rt_write_cstr(1, " key="); rt_write_uint(1, (raw_pointer >> 49U) & 3ULL);
                } else {
                    rt_write_cstr(1, " target="); write_hex_value(raw_pointer & 0x7ffffffffffULL);
                }
                rt_write_cstr(1, " next="); write_hex_value(next);
                rt_write_char(1, '\n');
                if (next == 0ULL) break;
                chain_fileoff += next * (pointer_format == MACHO_DYLD_CHAINED_PTR_ARM64E ||
                                         pointer_format == MACHO_DYLD_CHAINED_PTR_ARM64E_KERNEL ||
                                         pointer_format == MACHO_DYLD_CHAINED_PTR_ARM64E_USERLAND ||
                                         pointer_format == MACHO_DYLD_CHAINED_PTR_ARM64E_USERLAND24 ? 8ULL : 4ULL);
                chain_index += 1U;
            }
        }
        if (page_count > 16U) rt_write_line(1, "      ...");
    }
    if (exports_trie.present) {
        rt_write_cstr(1, "  Exports trie: dataoff="); write_hex_value((unsigned long long)exports_trie.dataoff);
        rt_write_cstr(1, " datasize="); write_hex_value((unsigned long long)exports_trie.datasize);
        rt_write_char(1, '\n');
    }
    rt_write_cstr(1, "  Summary: fixup_pages=");
    rt_write_uint(1, (unsigned long long)page_start_count);
    rt_write_line(1, " walked chain entries are printed under each page start.");
    return 0;
}

static int json_macho_fixup_event(int fd, const char *path, const MachHeaderInfo *header) {
    MachLinkeditDataInfo chained;
    unsigned char raw[28];
    if (load_macho_linkedit_data_command(fd, header, MACHO_LC_DYLD_CHAINED_FIXUPS, &chained) != 0) return -1;
    if (tool_json_begin_event(1, "readelf", "stdout", "macho_fixups") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
    if (rt_write_cstr(1, "\"file\":") != 0 || tool_json_write_string(1, path) != 0) return -1;
    if (json_field_bool("present", chained.present) != 0) return -1;
    if (json_field_uint("dataoff", chained.dataoff) != 0) return -1;
    if (json_field_uint("datasize", chained.datasize) != 0) return -1;
    if (chained.present && chained.datasize >= 28U && read_region(fd, (unsigned long long)chained.dataoff, raw, sizeof(raw)) == 0) {
        if (json_field_uint("version", read_u32_le_local(raw + 0)) != 0) return -1;
        if (json_field_uint("starts_offset", read_u32_le_local(raw + 4)) != 0) return -1;
        if (json_field_uint("imports_offset", read_u32_le_local(raw + 8)) != 0) return -1;
        if (json_field_uint("symbols_offset", read_u32_le_local(raw + 12)) != 0) return -1;
        if (json_field_uint("imports_count", read_u32_le_local(raw + 16)) != 0) return -1;
        if (json_field_uint("imports_format", read_u32_le_local(raw + 20)) != 0) return -1;
        if (json_field_uint("symbols_format", read_u32_le_local(raw + 24)) != 0) return -1;
    }
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static unsigned long long parse_address_arg(const char *text, int *ok) {
    unsigned long long value = 0ULL;
    size_t index = 0U;
    int base = 10;

    *ok = 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        index = 2U;
    }
    if (text[index] == '\0') return 0ULL;
    for (; text[index] != '\0'; ++index) {
        unsigned int digit;
        char ch = text[index];
        if (ch >= '0' && ch <= '9') digit = (unsigned int)(ch - '0');
        else if (base == 16 && ch >= 'a' && ch <= 'f') digit = 10U + (unsigned int)(ch - 'a');
        else if (base == 16 && ch >= 'A' && ch <= 'F') digit = 10U + (unsigned int)(ch - 'A');
        else return 0ULL;
        if (digit >= (unsigned int)base) return 0ULL;
        value = (value * (unsigned long long)base) + (unsigned long long)digit;
    }
    *ok = 1;
    return value;
}

static void find_nearest_macho_symbol(int fd, const MachSymtabInfo *symtab, unsigned long long address, char *name_out, size_t name_size, unsigned long long *symbol_value_out) {
    char strings[READELF_NAME_TABLE_CAPACITY];
    size_t string_size = 0U;
    unsigned int index;
    unsigned long long best_value = 0ULL;
    const char *best_name = "";

    name_out[0] = '\0';
    *symbol_value_out = 0ULL;
    if (symtab->symoff == 0U || symtab->nsyms == 0U || symtab->stroff == 0U || symtab->strsize == 0U) return;
    {
        size_t to_read = symtab->strsize < (unsigned int)(sizeof(strings) - 1U) ? (size_t)symtab->strsize : sizeof(strings) - 1U;
        if (read_region(fd, (unsigned long long)symtab->stroff, (unsigned char *)strings, to_read) != 0) return;
        strings[to_read] = '\0';
        string_size = to_read;
    }
    for (index = 0U; index < symtab->nsyms; ++index) {
        unsigned char entry[16];
        unsigned int strx;
        unsigned long long value;
        const char *name;
        if (read_region(fd, (unsigned long long)symtab->symoff + ((unsigned long long)index * 16ULL), entry, sizeof(entry)) != 0) break;
        value = read_u64_le_local(entry + 8);
        if (value == 0ULL || value > address || value < best_value) continue;
        strx = read_u32_le_local(entry + 0);
        name = name_from_table(strings, string_size, strx);
        if (name[0] == '\0') continue;
        best_value = value;
        best_name = name;
    }
    if (best_name[0] != '\0') {
        rt_copy_string(name_out, name_size, best_name);
        *symbol_value_out = best_value;
    }
}

static int find_macho_function_start(int fd, const MachHeaderInfo *header, const MachSegmentInfo *segments, unsigned int segment_count, unsigned long long address, unsigned long long *function_start_out) {
    MachLinkeditDataInfo starts;
    unsigned long long text_base = 0ULL;
    unsigned long long current;
    unsigned long long offset = 0ULL;
    unsigned int segment_index;

    *function_start_out = 0ULL;
    for (segment_index = 0U; segment_index < segment_count; ++segment_index) {
        if (rt_strcmp(segments[segment_index].name, "__TEXT") == 0) {
            text_base = segments[segment_index].vmaddr;
            break;
        }
    }
    if (load_macho_linkedit_data_command(fd, header, MACHO_LC_FUNCTION_STARTS, &starts) != 0 || !starts.present || starts.datasize == 0U || text_base == 0ULL) return -1;
    current = text_base;
    while (offset < (unsigned long long)starts.datasize) {
        unsigned long long delta = 0ULL;
        unsigned int shift = 0U;
        while (offset < (unsigned long long)starts.datasize) {
            unsigned char byte;
            if (read_region(fd, (unsigned long long)starts.dataoff + offset, &byte, 1U) != 0) return -1;
            offset += 1ULL;
            delta |= ((unsigned long long)(byte & 0x7fU)) << shift;
            if ((byte & 0x80U) == 0U) break;
            shift += 7U;
            if (shift >= 63U) return -1;
        }
        if (delta == 0ULL) break;
        current += delta;
        if (current <= address) *function_start_out = current;
        else break;
    }
    return *function_start_out != 0ULL ? 0 : -1;
}

static void print_macho_explain_address(int fd,
                                        const MachHeaderInfo *header,
                                        const MachSegmentInfo *segments,
                                        unsigned int segment_count,
                                        const MachSectionInfo *sections,
                                        unsigned int section_count,
                                        const MachSymtabInfo *symtab,
                                        const MachCodeSignatureInfo *signature,
                                        unsigned long long address) {
    const MachSegmentInfo *matched_segment = 0;
    const MachSectionInfo *matched_section = 0;
    unsigned long long vm_address = address;
    unsigned long long file_offset = 0ULL;
    unsigned int index;
    int matched_by_file = 0;
    char symbol_name[128];
    unsigned long long symbol_value;
    unsigned long long function_start;

    rt_write_line(1, "Mach-O Address Explanation:");
    rt_write_cstr(1, "  query: ");
    write_hex_value(address);
    rt_write_char(1, '\n');
    for (index = 0U; index < segment_count; ++index) {
        if (address >= segments[index].vmaddr && address < segments[index].vmaddr + segments[index].vmsize) {
            matched_segment = &segments[index];
            file_offset = segments[index].fileoff + (address - segments[index].vmaddr);
            vm_address = address;
            break;
        }
    }
    if (matched_segment == 0) {
        for (index = 0U; index < segment_count; ++index) {
            if (address >= segments[index].fileoff && address < segments[index].fileoff + segments[index].filesize) {
                matched_segment = &segments[index];
                file_offset = address;
                vm_address = segments[index].vmaddr + (address - segments[index].fileoff);
                matched_by_file = 1;
                break;
            }
        }
    }
    if (matched_segment == 0) {
        rt_write_line(1, "  No segment contains this VM address or file offset.");
        return;
    }
    rt_write_cstr(1, "  interpreted-as: ");
    rt_write_line(1, matched_by_file ? "file offset" : "VM address");
    rt_write_cstr(1, "  segment: ");
    rt_write_line(1, matched_segment->name);
    rt_write_cstr(1, "  vmaddr: ");
    write_hex_value(vm_address);
    rt_write_cstr(1, " file-offset: ");
    write_hex_value(file_offset);
    rt_write_cstr(1, " protection: ");
    write_macho_prot(matched_segment->initprot);
    rt_write_char(1, '\n');
    for (index = 0U; index < section_count; ++index) {
        if (vm_address >= sections[index].addr && vm_address < sections[index].addr + sections[index].size) {
            matched_section = &sections[index];
            break;
        }
    }
    if (matched_section != 0) {
        rt_write_cstr(1, "  section: ");
        rt_write_cstr(1, matched_section->segment);
        rt_write_char(1, ',');
        rt_write_line(1, matched_section->section);
        rt_write_cstr(1, "  section-offset: ");
        write_hex_value(vm_address - matched_section->addr);
        rt_write_cstr(1, " type: ");
        rt_write_line(1, macho_section_type_name(matched_section->flags & 0xffU));
    }
    find_nearest_macho_symbol(fd, symtab, vm_address, symbol_name, sizeof(symbol_name), &symbol_value);
    if (symbol_name[0] != '\0') {
        rt_write_cstr(1, "  nearest-symbol: ");
        rt_write_cstr(1, symbol_name);
        rt_write_cstr(1, " + ");
        write_hex_value(vm_address - symbol_value);
        rt_write_char(1, '\n');
    }
    if (find_macho_function_start(fd, header, segments, segment_count, vm_address, &function_start) == 0) {
        rt_write_cstr(1, "  function-start: ");
        write_hex_value(function_start);
        rt_write_cstr(1, " + ");
        write_hex_value(vm_address - function_start);
        rt_write_char(1, '\n');
    }
    if (signature->present && signature->has_code_directory && file_offset < (unsigned long long)signature->code_limit && signature->page_size_log2 < 31U) {
        unsigned long long page_size = 1ULL << signature->page_size_log2;
        rt_write_cstr(1, "  signed-by: CodeDirectory slot ");
        rt_write_uint(1, file_offset / page_size);
        rt_write_cstr(1, signature->hashes_verified ? " (verified)" : " (not verified)");
        rt_write_char(1, '\n');
    }
}

static int json_macho_explain_address_event(const char *path,
                                            const MachSegmentInfo *segments,
                                            unsigned int segment_count,
                                            const MachSectionInfo *sections,
                                            unsigned int section_count,
                                            unsigned long long address) {
    unsigned int index;
    const MachSegmentInfo *matched_segment = 0;
    const MachSectionInfo *matched_section = 0;
    unsigned long long vm_address = address;
    unsigned long long file_offset = 0ULL;
    int matched_by_file = 0;
    for (index = 0U; index < segment_count; ++index) {
        if (address >= segments[index].vmaddr && address < segments[index].vmaddr + segments[index].vmsize) {
            matched_segment = &segments[index];
            file_offset = segments[index].fileoff + (address - segments[index].vmaddr);
            break;
        }
    }
    if (matched_segment == 0) {
        for (index = 0U; index < segment_count; ++index) {
            if (address >= segments[index].fileoff && address < segments[index].fileoff + segments[index].filesize) {
                matched_segment = &segments[index];
                file_offset = address;
                vm_address = segments[index].vmaddr + (address - segments[index].fileoff);
                matched_by_file = 1;
                break;
            }
        }
    }
    if (matched_segment != 0) {
        for (index = 0U; index < section_count; ++index) {
            if (vm_address >= sections[index].addr && vm_address < sections[index].addr + sections[index].size) {
                matched_section = &sections[index];
                break;
            }
        }
    }
    if (tool_json_begin_event(1, "readelf", "stdout", "macho_address_explanation") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
    if (rt_write_cstr(1, "\"file\":") != 0 || tool_json_write_string(1, path) != 0) return -1;
    if (json_field_uint("query", address) != 0) return -1;
    if (json_field_bool("matched", matched_segment != 0) != 0) return -1;
    if (matched_segment != 0) {
        if (json_field_string("interpreted_as", matched_by_file ? "file_offset" : "vm_address") != 0) return -1;
        if (json_field_string("segment", matched_segment->name) != 0) return -1;
        if (json_field_uint("vmaddr", vm_address) != 0) return -1;
        if (json_field_uint("file_offset", file_offset) != 0) return -1;
        if (matched_section != 0) {
            if (json_field_string("section_segment", matched_section->segment) != 0) return -1;
            if (json_field_string("section", matched_section->section) != 0) return -1;
        }
    }
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int json_macho_header_event(const char *path, const MachHeaderInfo *info) {
    if (tool_json_begin_event(1, "readelf", "stdout", "macho_header") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
    if (rt_write_cstr(1, "\"file\":") != 0 || tool_json_write_string(1, path) != 0) return -1;
    if (json_field_uint("magic", info->magic) != 0) return -1;
    if (json_field_string("type", macho_type_name(info->filetype)) != 0) return -1;
    if (json_field_string("machine", macho_machine_name(info->cputype)) != 0) return -1;
    if (json_field_string("arch", macho_short_arch_name(info->cputype, info->cpusubtype)) != 0) return -1;
    if (json_field_uint("cputype", info->cputype) != 0) return -1;
    if (json_field_uint("cpusubtype", info->cpusubtype) != 0) return -1;
    if (json_field_string("cpusubtype_name", macho_cpu_subtype_name(info->cputype, info->cpusubtype)) != 0) return -1;
    if (json_field_uint("cpu_capabilities", macho_cpu_capabilities(info->cpusubtype)) != 0) return -1;
    if (json_field_uint("ncmds", info->ncmds) != 0) return -1;
    if (json_field_uint("sizeofcmds", info->sizeofcmds) != 0) return -1;
    if (json_field_uint("flags", info->flags) != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int json_macho_fat_arch_events(const char *path, const MachFatInfo *fat) {
    unsigned int index;
    for (index = 0U; index < fat->arch_count; ++index) {
        if (tool_json_begin_event(1, "readelf", "stdout", "macho_fat_arch") != 0) return -1;
        if (rt_write_cstr(1, ",\"data\":{") != 0) return -1;
        if (rt_write_cstr(1, "\"file\":") != 0 || tool_json_write_string(1, path) != 0) return -1;
        if (json_field_uint("index", index) != 0) return -1;
        if (json_field_string("arch", macho_short_arch_name(fat->arches[index].cputype, fat->arches[index].cpusubtype)) != 0) return -1;
        if (json_field_uint("cputype", fat->arches[index].cputype) != 0) return -1;
        if (json_field_uint("cpusubtype", fat->arches[index].cpusubtype) != 0) return -1;
        if (json_field_string("cpusubtype_name", macho_cpu_subtype_name(fat->arches[index].cputype, fat->arches[index].cpusubtype)) != 0) return -1;
        if (json_field_uint("cpu_capabilities", macho_cpu_capabilities(fat->arches[index].cpusubtype)) != 0) return -1;
        if (json_field_uint("offset", fat->arches[index].offset) != 0) return -1;
        if (json_field_uint("size", fat->arches[index].size) != 0) return -1;
        if (json_field_uint("align", fat->arches[index].align) != 0) return -1;
        if (rt_write_char(1, '}') != 0 || tool_json_end_event(1) != 0) return -1;
    }
    return 0;
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
    if (json_field_uint("reserved1", section->reserved1) != 0) return -1;
    if (json_field_uint("reserved2", section->reserved2) != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int json_macho_code_signature_event(const char *path, const MachCodeSignatureInfo *signature) {
    unsigned int slot_index;
    unsigned int cms_size = 0U;
    unsigned int requirements_size = 0U;
    for (slot_index = 0U; slot_index < signature->slot_count; ++slot_index) {
        if (signature->slots[slot_index].type == MACHO_CODE_SIGNATURE_SLOT_CMS_SIGNATURE) cms_size = signature->slots[slot_index].length;
        if (signature->slots[slot_index].type == MACHO_CODE_SIGNATURE_SLOT_REQUIREMENTS) requirements_size = signature->slots[slot_index].length;
    }
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
    if (json_field_string("cdhash", signature->cdhash) != 0) return -1;
    if (json_field_string("cdhash_full", signature->cdhash_full) != 0) return -1;
    if (json_field_uint("superblob_count", signature->superblob_count) != 0) return -1;
    if (json_field_uint("slot_count", signature->slot_count) != 0) return -1;
    if (json_field_uint("cms_signature_size", cms_size) != 0) return -1;
    if (json_field_uint("requirements_size", requirements_size) != 0) return -1;
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
            if (json_field_uint("maxprot", read_u32_le_local(command_data + 56)) != 0) return -1;
            if (json_field_uint("initprot", read_u32_le_local(command_data + 60)) != 0) return -1;
            if (json_field_uint("nsects", read_u32_le_local(command_data + 64)) != 0) return -1;
            if (json_field_uint("flags", read_u32_le_local(command_data + 68)) != 0) return -1;
        } else if (command == MACHO_LC_SYMTAB && command_size >= 24U && read_region(fd, command_offset, command_data, 24U) == 0) {
            if (json_field_uint("symoff", read_u32_le_local(command_data + 8)) != 0) return -1;
            if (json_field_uint("nsyms", read_u32_le_local(command_data + 12)) != 0) return -1;
            if (json_field_uint("stroff", read_u32_le_local(command_data + 16)) != 0) return -1;
            if (json_field_uint("strsize", read_u32_le_local(command_data + 20)) != 0) return -1;
        } else if (command == MACHO_LC_DYSYMTAB && command_size >= 80U && read_region(fd, command_offset, command_data, 80U) == 0) {
            if (json_field_uint("ilocalsym", read_u32_le_local(command_data + 8)) != 0) return -1;
            if (json_field_uint("nlocalsym", read_u32_le_local(command_data + 12)) != 0) return -1;
            if (json_field_uint("iextdefsym", read_u32_le_local(command_data + 16)) != 0) return -1;
            if (json_field_uint("nextdefsym", read_u32_le_local(command_data + 20)) != 0) return -1;
            if (json_field_uint("iundefsym", read_u32_le_local(command_data + 24)) != 0) return -1;
            if (json_field_uint("nundefsym", read_u32_le_local(command_data + 28)) != 0) return -1;
            if (json_field_uint("indirectsymoff", read_u32_le_local(command_data + 56)) != 0) return -1;
            if (json_field_uint("nindirectsyms", read_u32_le_local(command_data + 60)) != 0) return -1;
        } else if (command == MACHO_LC_MAIN && command_size >= 24U && read_region(fd, command_offset, command_data, 24U) == 0) {
            if (json_field_uint("entryoff", read_u64_le_local(command_data + 8)) != 0) return -1;
            if (json_field_uint("stacksize", read_u64_le_local(command_data + 16)) != 0) return -1;
        } else if (command == MACHO_LC_CODE_SIGNATURE && command_size >= 16U && read_region(fd, command_offset, command_data, 16U) == 0) {
            if (json_field_uint("dataoff", read_u32_le_local(command_data + 8)) != 0) return -1;
            if (json_field_uint("datasize", read_u32_le_local(command_data + 12)) != 0) return -1;
        } else if ((command == MACHO_LC_DYLD_CHAINED_FIXUPS || command == MACHO_LC_DYLD_EXPORTS_TRIE || command == MACHO_LC_FUNCTION_STARTS || command == MACHO_LC_DATA_IN_CODE) && command_size >= 16U && read_region(fd, command_offset, command_data, 16U) == 0) {
            if (json_field_uint("dataoff", read_u32_le_local(command_data + 8)) != 0) return -1;
            if (json_field_uint("datasize", read_u32_le_local(command_data + 12)) != 0) return -1;
        } else if ((command == MACHO_LC_DYLD_INFO || command == MACHO_LC_DYLD_INFO_ONLY) && command_size >= 48U && read_region(fd, command_offset, command_data, 48U) == 0) {
            if (json_field_uint("rebase_off", read_u32_le_local(command_data + 8)) != 0) return -1;
            if (json_field_uint("rebase_size", read_u32_le_local(command_data + 12)) != 0) return -1;
            if (json_field_uint("bind_off", read_u32_le_local(command_data + 16)) != 0) return -1;
            if (json_field_uint("bind_size", read_u32_le_local(command_data + 20)) != 0) return -1;
            if (json_field_uint("export_off", read_u32_le_local(command_data + 40)) != 0) return -1;
            if (json_field_uint("export_size", read_u32_le_local(command_data + 44)) != 0) return -1;
        } else if (command == MACHO_LC_UUID && command_size >= 24U && read_region(fd, command_offset, command_data, 24U) == 0) {
            if (json_field_uuid("uuid", command_data + 8) != 0) return -1;
        } else if (command == MACHO_LC_SOURCE_VERSION && command_size >= 16U && read_region(fd, command_offset, command_data, 16U) == 0) {
            if (json_field_uint("raw_version", read_u64_le_local(command_data + 8)) != 0) return -1;
            if (json_field_source_version("version", read_u64_le_local(command_data + 8)) != 0) return -1;
        } else if (command == MACHO_LC_BUILD_VERSION && command_size >= 24U && read_region(fd, command_offset, command_data, 24U) == 0) {
            if (json_field_string("platform", macho_platform_name(read_u32_le_local(command_data + 8))) != 0) return -1;
            if (json_field_uint("minos_raw", read_u32_le_local(command_data + 12)) != 0) return -1;
            if (json_field_version_triplet("minos", read_u32_le_local(command_data + 12)) != 0) return -1;
            if (json_field_uint("sdk_raw", read_u32_le_local(command_data + 16)) != 0) return -1;
            if (json_field_version_triplet("sdk", read_u32_le_local(command_data + 16)) != 0) return -1;
            if (json_field_uint("ntools", read_u32_le_local(command_data + 20)) != 0) return -1;
            if (read_u32_le_local(command_data + 20) > 0U && command_size >= 32U && read_region(fd, command_offset, command_data, 32U) == 0) {
                if (json_field_string("tool", macho_build_tool_name(read_u32_le_local(command_data + 24))) != 0) return -1;
                if (json_field_version_triplet("tool_version", read_u32_le_local(command_data + 28)) != 0) return -1;
            }
        } else if ((command == MACHO_LC_LOAD_DYLINKER || command == MACHO_LC_LOAD_DYLIB || command == MACHO_LC_ID_DYLIB) && command_size > 12U && command_size <= sizeof(command_data) && read_region(fd, command_offset, command_data, command_size) == 0) {
            unsigned int name_offset = read_u32_le_local(command_data + 8);
            if (name_offset < command_size) {
                char path[128];
                copy_fixed_name(path, sizeof(path), command_data + name_offset, (size_t)(command_size - name_offset));
                if (json_field_string(command == MACHO_LC_LOAD_DYLINKER ? "path" : "name", path) != 0) return -1;
            }
            if (command == MACHO_LC_LOAD_DYLIB || command == MACHO_LC_ID_DYLIB) {
                if (json_field_version_triplet("current_version", read_u32_le_local(command_data + 16)) != 0) return -1;
                if (json_field_version_triplet("compatibility_version", read_u32_le_local(command_data + 20)) != 0) return -1;
            }
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
        write_elf_program_type_name(programs[i].type);
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

static unsigned int count_macho_fixup_pages(int fd, const MachHeaderInfo *header) {
    MachLinkeditDataInfo chained;
    unsigned char raw[32];
    unsigned int starts_offset;
    unsigned int segment_count;
    unsigned int segment_index;
    unsigned int count = 0U;

    if (load_macho_linkedit_data_command(fd, header, MACHO_LC_DYLD_CHAINED_FIXUPS, &chained) != 0 || !chained.present || chained.datasize < 28U) return 0U;
    if (read_region(fd, (unsigned long long)chained.dataoff, raw, 28U) != 0) return 0U;
    starts_offset = read_u32_le_local(raw + 4);
    if (starts_offset + 4U > chained.datasize || read_region(fd, (unsigned long long)chained.dataoff + starts_offset, raw, 4U) != 0) return 0U;
    segment_count = read_u32_le_local(raw);
    for (segment_index = 0U; segment_index < segment_count && segment_index < READELF_MAX_MACHO_SEGMENTS; ++segment_index) {
        unsigned int info_offset;
        unsigned long long info_base;
        unsigned int size;
        unsigned int page_count;
        unsigned int page_index;
        if (starts_offset + 4U + ((segment_index + 1U) * 4U) > chained.datasize) break;
        if (read_region(fd, (unsigned long long)chained.dataoff + starts_offset + 4ULL + ((unsigned long long)segment_index * 4ULL), raw, 4U) != 0) break;
        info_offset = read_u32_le_local(raw);
        if (info_offset == 0U) continue;
        info_base = (unsigned long long)chained.dataoff + starts_offset + (unsigned long long)info_offset;
        if (read_region(fd, info_base, raw, 24U) != 0) break;
        size = read_u32_le_local(raw + 0);
        page_count = read_u16_le(raw + 20);
        for (page_index = 0U; page_index < page_count; ++page_index) {
            unsigned char page_raw[2];
            if (22ULL + ((unsigned long long)(page_index + 1U) * 2ULL) > (unsigned long long)size) break;
            if (read_region(fd, info_base + 22ULL + ((unsigned long long)page_index * 2ULL), page_raw, sizeof(page_raw)) != 0) break;
            if (read_u16_le(page_raw) != 0xffffU) count += 1U;
        }
    }
    return count;
}

static int fill_macho_compare_summary(int fd, const MachHeaderInfo *macho_header, BinaryCompareSummary *summary) {
    MachSectionInfo macho_sections[READELF_MAX_SECTIONS];
    MachSegmentInfo macho_segments[READELF_MAX_MACHO_SEGMENTS];
    MachSymtabInfo macho_symtab;
    MachCodeSignatureInfo signature;
    unsigned int macho_section_count = 0U;
    unsigned int macho_segment_count = 0U;
    unsigned long long entryoff = 0ULL;
    unsigned int segment_index;

    if (load_macho_sections(fd, macho_header, macho_sections, &macho_section_count) != 0 ||
        load_macho_segments(fd, macho_header, macho_segments, &macho_segment_count) != 0 ||
        load_macho_symtab(fd, macho_header, &macho_symtab) != 0 ||
        inspect_macho_code_signature(fd, macho_header, &signature) != 0) {
        return -1;
    }
    rt_copy_string(summary->format, sizeof(summary->format), "macho");
    if (hash_fd_sha256_hex(fd, summary->sha256, sizeof(summary->sha256)) != 0) summary->sha256[0] = '\0';
    if (hash_macho_load_commands(fd, macho_header, summary->load_commands_sha256, sizeof(summary->load_commands_sha256)) != 0) summary->load_commands_sha256[0] = '\0';
    hash_macho_layout(macho_segments, macho_segment_count, macho_sections, macho_section_count, summary->layout_sha256, sizeof(summary->layout_sha256));
    (void)load_macho_main_entryoff(fd, macho_header, &entryoff);
    summary->entry = entryoff;
    for (segment_index = 0U; segment_index < macho_segment_count; ++segment_index) {
        if (entryoff >= macho_segments[segment_index].fileoff && entryoff < macho_segments[segment_index].fileoff + macho_segments[segment_index].filesize) {
            summary->entry = macho_segments[segment_index].vmaddr + (entryoff - macho_segments[segment_index].fileoff);
            break;
        }
    }
    summary->machine = macho_header->cputype;
    summary->type = macho_header->filetype;
    summary->flags = macho_header->flags;
    summary->segment_count = macho_segment_count;
    summary->section_count = macho_section_count;
    summary->load_command_count = macho_header->ncmds;
    summary->dylib_count = count_macho_dylibs(fd, macho_header);
    summary->fixup_count = count_macho_fixup_pages(fd, macho_header);
    summary->symbol_count = macho_symtab.nsyms;
    summary->relocation_count = count_macho_relocations(macho_sections, macho_section_count);
    summary->code_signature_present = signature.present;
    summary->code_signature_verified = signature.hashes_verified;
    rt_copy_string(summary->code_signature_cdhash, sizeof(summary->code_signature_cdhash), signature.cdhash);
    return 0;
}

static int load_compare_summary(const char *path, BinaryCompareSummary *summary) {
    int fd = platform_open_read(path);
    ElfHeaderInfo elf_header;
    ElfProgramInfo programs[READELF_MAX_PROGRAM_HEADERS];
    ElfSectionInfo elf_sections[READELF_MAX_SECTIONS];
    MachHeaderInfo macho_header;
    char names[READELF_NAME_TABLE_CAPACITY];
    size_t names_size = 0U;

    (void)programs;
    summary->sha256[0] = '\0';
    summary->load_commands_sha256[0] = '\0';
    summary->layout_sha256[0] = '\0';
    summary->code_signature_cdhash[0] = '\0';
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
        summary->load_command_count = 0U;
        summary->dylib_count = 0U;
        summary->fixup_count = 0U;
        summary->symbol_count = count_elf_symbols(&elf_header, elf_sections);
        summary->relocation_count = count_elf_relocations(&elf_header, elf_sections);
        summary->code_signature_present = 0;
        summary->code_signature_verified = 0;
        platform_close(fd);
        return 0;
    }
    if (parse_macho_header(fd, &macho_header) == 0 && fill_macho_compare_summary(fd, &macho_header, summary) == 0) {
        platform_close(fd);
        return 0;
    }
    {
        MachFatInfo fat;
        unsigned int slice_index = 0U;
        if (parse_macho_fat_header(fd, &fat) == 0 && macho_fat_choose_slice(&fat, &slice_index) == 0) {
            set_object_window(fat.arches[slice_index].offset, fat.arches[slice_index].size);
            if (parse_macho_header(fd, &macho_header) == 0 && fill_macho_compare_summary(fd, &macho_header, summary) == 0) {
                set_object_window(0ULL, 0ULL);
                platform_close(fd);
                return 0;
            }
            set_object_window(0ULL, 0ULL);
        }
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

static int compare_files(const char *left_path, const char *right_path, int deep) {
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
    if (deep) {
        if (compare_field_uint("load_commands", left.load_command_count, right.load_command_count, &differences) != 0) return 2;
        if (compare_field_uint("dylibs", left.dylib_count, right.dylib_count, &differences) != 0) return 2;
        if (compare_field_uint("fixup_pages", left.fixup_count, right.fixup_count, &differences) != 0) return 2;
        if (compare_field_string("load_commands_sha256", left.load_commands_sha256, right.load_commands_sha256, &differences) != 0) return 2;
        if (compare_field_string("layout_sha256", left.layout_sha256, right.layout_sha256, &differences) != 0) return 2;
        if (compare_field_string("code_signature_cdhash", left.code_signature_cdhash, right.code_signature_cdhash, &differences) != 0) return 2;
    }
    if (compare_field_string("sha256", left.sha256, right.sha256, &differences) != 0) return 2;
    if (readelf_json) {
        if (tool_json_begin_event(1, "readelf", "stdout", "compare_summary") != 0) return 2;
        if (rt_write_cstr(1, ",\"data\":{") != 0) return 2;
        if (rt_write_cstr(1, "\"left_file\":") != 0 || tool_json_write_string(1, left_path) != 0) return 2;
        if (json_field_string("right_file", right_path) != 0) return 2;
        if (json_field_bool("deep", deep) != 0) return 2;
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
    int show_macho_map_flag = 0;
    int show_macho_fixups_flag = 0;
    int show_signature_details_flag = 0;
    int explain_address_flag = 0;
    unsigned long long explain_address = 0ULL;
    int compare_mode = 0;
    int compare_deep = 0;
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
        } else if (rt_strcmp(argv[argi], "--deep") == 0) {
            compare_deep = 1;
        } else if (rt_strcmp(argv[argi], "--macho-map") == 0) {
            show_macho_map_flag = 1;
        } else if (rt_strcmp(argv[argi], "--macho-fixups") == 0) {
            show_macho_fixups_flag = 1;
        } else if (rt_strcmp(argv[argi], "--signature-details") == 0) {
            show_signature_details_flag = 1;
            show_notes_flag = 1;
        } else if (rt_strcmp(argv[argi], "--explain-address") == 0) {
            int ok;
            if (argi + 1 >= argc) {
                tool_write_usage("readelf", "--explain-address ADDRESS file ...");
                return 1;
            }
            explain_address = parse_address_arg(argv[argi + 1], &ok);
            if (!ok) {
                tool_write_error("readelf", "invalid address: ", argv[argi + 1]);
                return 1;
            }
            explain_address_flag = 1;
            argi += 1;
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
            show_macho_map_flag = 1;
            show_macho_fixups_flag = 1;
        } else if (rt_strcmp(argv[argi], "-W") == 0 || rt_strcmp(argv[argi], "--wide") == 0) {
        } else {
            tool_write_usage("readelf", "[-a] [-h] [-l] [-S] [-d] [-r] [-s] [-n] [--macho-map] [--macho-fixups] [--signature-details] [--explain-address addr] [--json] [--compare [--deep] left right] file ...");
            return 1;
        }
        argi += 1;
    }

    if (compare_mode) {
        if (argc - argi != 2) {
            tool_write_usage("readelf", "--compare [--json] left right");
            return 1;
        }
        return compare_files(argv[argi], argv[argi + 1], compare_deep);
    }

    if (!show_header_flag && !show_programs_flag && !show_sections_flag && !show_dynamic_flag && !show_relocations_flag && !show_symbols_flag && !show_notes_flag && !show_macho_map_flag && !show_macho_fixups_flag && !explain_address_flag) {
        show_header_flag = 1;
    }

    if (argi >= argc) {
        tool_write_usage("readelf", "[-a] [-h] [-l] [-S] [-d] [-r] [-s] [-n] [--macho-map] [--macho-fixups] [--signature-details] [--explain-address addr] [--json] file ...");
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        int fd = platform_open_read(argv[i]);
        ElfHeaderInfo header;
        MachHeaderInfo macho;
        MachSectionInfo macho_sections[READELF_MAX_SECTIONS];
        MachSegmentInfo macho_segments[READELF_MAX_MACHO_SEGMENTS];
        MachSymtabInfo macho_symtab;
        unsigned int macho_section_count = 0U;
        unsigned int macho_segment_count = 0U;
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

        set_object_window(0ULL, 0ULL);

        if (parse_elf_header(fd, &header) != 0 || load_program_headers(fd, &header, programs) != 0 || load_sections(fd, &header, sections) != 0 ||
            load_name_table(fd, &header, sections, names, sizeof(names), &names_size) != 0) {
            if (parse_macho_header(fd, &macho) == 0) {
                int macho_sections_ok = load_macho_sections(fd, &macho, macho_sections, &macho_section_count) == 0;
                int macho_segments_ok = load_macho_segments(fd, &macho, macho_segments, &macho_segment_count) == 0;
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
                if (show_macho_map_flag) {
                    if (macho_segments_ok && macho_sections_ok) {
                        if (readelf_json) (void)json_macho_map_events(argv[i], macho_segments, macho_segment_count, macho_sections, macho_section_count);
                        else print_macho_map(macho_segments, macho_segment_count, macho_sections, macho_section_count, readelf_object_base);
                    } else if (!readelf_json) rt_write_line(1, "Mach-O map is not available.");
                }
                if (show_macho_fixups_flag) {
                    if (readelf_json) (void)json_macho_fixup_event(fd, argv[i], &macho);
                    else (void)print_macho_fixups(fd, &macho, argv[i]);
                }
                if (explain_address_flag) {
                    if (macho_segments_ok && macho_sections_ok && macho_symtab_ok) {
                        if (readelf_json) (void)json_macho_explain_address_event(argv[i], macho_segments, macho_segment_count, macho_sections, macho_section_count, explain_address);
                        else print_macho_explain_address(fd, &macho, macho_segments, macho_segment_count, macho_sections, macho_section_count, &macho_symtab, &macho_signature, explain_address);
                    } else if (!readelf_json) rt_write_line(1, "Mach-O address explanation is not available.");
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
                    if (readelf_json) {
                        (void)json_macho_code_signature_event(argv[i], &macho_signature);
                        if (show_signature_details_flag) (void)json_macho_signature_detail_event(argv[i], &macho_signature);
                    } else if (show_signature_details_flag) print_macho_signature_details(fd, &macho_signature);
                    else print_macho_code_signature(&macho_signature);
                }
                platform_close(fd);
                continue;
            }

            {
                MachFatInfo fat;
                unsigned int slice_index = 0U;
                if (parse_macho_fat_header(fd, &fat) == 0 && macho_fat_choose_slice(&fat, &slice_index) == 0) {
                    if (!readelf_json) {
                        rt_write_cstr(1, "File: ");
                        rt_write_line(1, argv[i]);
                    }
                    if (show_header_flag) {
                        if (readelf_json) (void)json_macho_fat_arch_events(argv[i], &fat);
                        else print_macho_fat_header(&fat);
                    }
                    set_object_window(fat.arches[slice_index].offset, fat.arches[slice_index].size);
                    if (parse_macho_header(fd, &macho) == 0) {
                        int macho_sections_ok = load_macho_sections(fd, &macho, macho_sections, &macho_section_count) == 0;
                        int macho_segments_ok = load_macho_segments(fd, &macho, macho_segments, &macho_segment_count) == 0;
                        int macho_symtab_ok = load_macho_symtab(fd, &macho, &macho_symtab) == 0;
                        MachCodeSignatureInfo macho_signature;
                        (void)inspect_macho_code_signature(fd, &macho, &macho_signature);
                        if (!readelf_json) {
                            rt_write_cstr(1, "Selected Mach-O slice: ");
                            rt_write_cstr(1, macho_short_arch_name(fat.arches[slice_index].cputype, fat.arches[slice_index].cpusubtype));
                            rt_write_cstr(1, " offset=");
                            write_hex_value(fat.arches[slice_index].offset);
                            rt_write_cstr(1, " size=");
                            write_hex_value(fat.arches[slice_index].size);
                            rt_write_char(1, '\n');
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
                                    for (section_index = 0U; section_index < macho_section_count; ++section_index) (void)json_macho_section_event(argv[i], section_index, &macho_sections[section_index]);
                                } else print_macho_sections(macho_sections, macho_section_count);
                            } else if (!readelf_json) rt_write_line(1, "Mach-O section table is not available.");
                        }
                        if (show_dynamic_flag && !readelf_json) rt_write_line(1, "Mach-O inputs do not have ELF dynamic sections.");
                        if (show_relocations_flag) {
                            if (load_macho_sections(fd, &macho, macho_sections, &macho_section_count) == 0 && load_macho_symtab(fd, &macho, &macho_symtab) == 0) {
                                if (readelf_json) (void)json_macho_relocations(fd, argv[i], macho_sections, macho_section_count, &macho_symtab);
                                else print_macho_relocations(fd, macho_sections, macho_section_count, &macho_symtab);
                            }
                        }
                        if (show_macho_map_flag) {
                            if (macho_segments_ok && macho_sections_ok) {
                                if (readelf_json) (void)json_macho_map_events(argv[i], macho_segments, macho_segment_count, macho_sections, macho_section_count);
                                else print_macho_map(macho_segments, macho_segment_count, macho_sections, macho_section_count, readelf_object_base);
                            } else if (!readelf_json) rt_write_line(1, "Mach-O map is not available.");
                        }
                        if (show_macho_fixups_flag) {
                            if (readelf_json) (void)json_macho_fixup_event(fd, argv[i], &macho);
                            else (void)print_macho_fixups(fd, &macho, argv[i]);
                        }
                        if (explain_address_flag) {
                            if (macho_segments_ok && macho_sections_ok && macho_symtab_ok) {
                                if (readelf_json) (void)json_macho_explain_address_event(argv[i], macho_segments, macho_segment_count, macho_sections, macho_section_count, explain_address);
                                else print_macho_explain_address(fd, &macho, macho_segments, macho_segment_count, macho_sections, macho_section_count, &macho_symtab, &macho_signature, explain_address);
                            } else if (!readelf_json) rt_write_line(1, "Mach-O address explanation is not available.");
                        }
                        if (show_symbols_flag) {
                            if (macho_symtab_ok) {
                                if (readelf_json) (void)json_macho_symbols(fd, argv[i], &macho_symtab);
                                else print_macho_symbols(fd, &macho_symtab);
                            } else if (!readelf_json) rt_write_line(1, "Mach-O symbol table is not available.");
                        }
                        if (show_notes_flag) {
                            if (readelf_json) {
                                (void)json_macho_code_signature_event(argv[i], &macho_signature);
                                if (show_signature_details_flag) (void)json_macho_signature_detail_event(argv[i], &macho_signature);
                            } else if (show_signature_details_flag) print_macho_signature_details(fd, &macho_signature);
                            else print_macho_code_signature(&macho_signature);
                        }
                        set_object_window(0ULL, 0ULL);
                        platform_close(fd);
                        continue;
                    }
                    set_object_window(0ULL, 0ULL);
                }
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
