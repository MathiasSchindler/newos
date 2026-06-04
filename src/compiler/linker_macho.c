#include "linker_internal.h"

#include "crypto/sha256.h"

#define MACHO64_MAGIC               0xfeedfacfU
#define MACHO_CPU_TYPE_ARM64        0x0100000cU
#define MACHO_FILETYPE_OBJECT       1U
#define MACHO_FILETYPE_EXECUTE      2U
#define MACHO_LC_SEGMENT_64         0x19U
#define MACHO_LC_SYMTAB             0x2U
#define MACHO_LC_DYLD_INFO_ONLY     0x80000022U
#define MACHO_LC_LOAD_DYLINKER      0xeU
#define MACHO_LC_BUILD_VERSION      0x32U
#define MACHO_LC_MAIN               0x80000028U
#define MACHO_LC_CODE_SIGNATURE     0x1dU
#define MACHO_HEADER64_SIZE         32U
#define MACHO_SECTION64_SIZE        80U
#define MACHO_NLIST64_SIZE          16U
#define MACHO_RELOC_SIZE            8U
#define MACHO_BASE_ADDRESS          0x100000000ULL
#define MACHO_PAGE_SIZE             0x4000ULL
#define MACHO_MAX_SECTIONS          128U
#define MACHO_VM_PROT_READ          1U
#define MACHO_VM_PROT_WRITE         2U
#define MACHO_VM_PROT_EXECUTE       4U
#define MACHO_FLAG_NOUNDEFS         0x1U
#define MACHO_FLAG_DYLDLINK         0x4U
#define MACHO_FLAG_TWOLEVEL         0x80U
#define MACHO_FLAG_PIE              0x200000U
#define MACHO_N_UNDF                0x00U
#define MACHO_N_STAB                0xe0U
#define MACHO_N_TYPE_MASK           0x0eU
#define MACHO_N_SECT                0x0eU
#define MACHO_N_EXT                 0x01U
#define MACHO_ARM64_RELOC_UNSIGNED  0U
#define MACHO_ARM64_RELOC_BRANCH26  2U
#define MACHO_ARM64_RELOC_PAGE21    3U
#define MACHO_ARM64_RELOC_PAGEOFF12 4U
#define MACHO_ARM64_RELOC_ADDEND    10U
#define MACHO_SECTION_TYPE_MASK     0xffU
#define MACHO_S_ZEROFILL            0x1U
#define MACHO_TEXT_SECTION_FLAGS    0x80000400U
#define MACHO_CODE_DIRECTORY_IDENT  "newlinker"
#define MACHO_CODE_DIRECTORY_IDENT_SIZE 10U
#define MACHO_REBASE_TYPE_POINTER   1U
#define MACHO_REBASE_OPCODE_DONE    0x00U
#define MACHO_REBASE_OPCODE_SET_TYPE_IMM 0x10U
#define MACHO_REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB 0x20U
#define MACHO_REBASE_OPCODE_DO_REBASE_IMM_TIMES 0x50U
#define MACHO_MAX_REBASES           8192U

typedef enum {
    MACHO_SECTION_CLASS_NONE = 0,
    MACHO_SECTION_CLASS_TEXT,
    MACHO_SECTION_CLASS_CONST,
    MACHO_SECTION_CLASS_CSTRING,
    MACHO_SECTION_CLASS_DATA,
    MACHO_SECTION_CLASS_BSS
} MachoSectionClass;

typedef struct {
    char sectname[17];
    char segname[17];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t ordinal;
    MachoSectionClass section_class;
    uint64_t out_offset;
    uint64_t out_addr;
} MachoInputSection;

typedef struct {
    char path[COMPILER_PATH_CAPACITY];
    unsigned char *file;
    size_t size;
    MachoInputSection sections[MACHO_MAX_SECTIONS];
    size_t section_count;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
    uint32_t relocation_count;
    MachoInputSection *text_section;
    MachoInputSection *const_section;
    MachoInputSection *cstring_section;
    MachoInputSection *data_section;
    uint64_t entry_value;
    int entry_found;
} MachoInputObject;

typedef struct {
    char name[COMPILER_PATH_CAPACITY];
    size_t object_index;
    uint32_t symbol_index;
    uint64_t value;
} MachoGlobalSymbol;

typedef struct {
    unsigned char segment_index;
    uint64_t segment_offset;
} MachoRebaseEntry;

typedef struct {
    const char *section_name;
    const char *segment_name;
    MachoSectionClass section_class;
    uint32_t flags;
    uint32_t align;
    uint64_t size;
    uint64_t out_offset;
    uint64_t out_addr;
} MachoOutputSection;

typedef struct {
    MachoInputObject *objects;
    size_t object_count;
    MachoGlobalSymbol globals[LINKER_MAX_GLOBALS];
    size_t global_count;
    MachoRebaseEntry rebases[MACHO_MAX_REBASES];
    size_t rebase_count;
    MachoOutputSection text;
    MachoOutputSection constant;
    MachoOutputSection cstring;
    MachoOutputSection data;
    MachoOutputSection bss;
    uint64_t entryoff;
    int entry_found;
} MachoLinkImage;

typedef struct {
    CryptoSha256Context context;
    unsigned char *hashes;
    size_t hash_count;
    size_t hash_index;
    size_t page_used;
} MachoPageHasher;

static uint64_t macho_align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static void macho_store_fixed_name(unsigned char *out, const char *name) {
    size_t i;
    memset(out, 0, 16U);
    for (i = 0; i < 16U && name[i] != '\0'; ++i) {
        out[i] = (unsigned char)name[i];
    }
}

static void macho_copy_fixed_name(char *out, const unsigned char *name) {
    size_t i;
    for (i = 0; i < 16U && name[i] != '\0'; ++i) {
        out[i] = (char)name[i];
    }
    out[i] = '\0';
}

static int macho_name_equals(const char *name, const char *expected) {
    return rt_strcmp(name, expected) == 0;
}

static int macho_write_hex64(int fd, uint64_t value) {
    char buffer[18];
    const char *digits = "0123456789abcdef";
    int i;

    buffer[0] = '0';
    buffer[1] = 'x';
    for (i = 0; i < 16; ++i) {
        buffer[2 + i] = digits[(value >> (uint64_t)((15 - i) * 4)) & 0xfU];
    }
    return rt_write_all(fd, buffer, sizeof(buffer));
}

static int64_t macho_sign_extend_24(uint32_t value) {
    uint32_t bits = value & 0x00ffffffU;
    if ((bits & 0x00800000U) != 0U) {
        return (int64_t)(int32_t)(bits | 0xff000000U);
    }
    return (int64_t)bits;
}

static MachoSectionClass macho_classify_section(const char *segment_name, const char *section_name, uint32_t flags) {
    if (macho_name_equals(segment_name, "__TEXT") && macho_name_equals(section_name, "__text")) {
        return MACHO_SECTION_CLASS_TEXT;
    }
    if ((macho_name_equals(segment_name, "__TEXT") || macho_name_equals(segment_name, "__DATA")) && macho_name_equals(section_name, "__const")) {
        return MACHO_SECTION_CLASS_CONST;
    }
    if (macho_name_equals(segment_name, "__TEXT") &&
        (macho_name_equals(section_name, "__literal4") || macho_name_equals(section_name, "__literal8") || macho_name_equals(section_name, "__literal16"))) {
        return MACHO_SECTION_CLASS_CONST;
    }
    if (macho_name_equals(segment_name, "__TEXT") && macho_name_equals(section_name, "__cstring")) {
        return MACHO_SECTION_CLASS_CSTRING;
    }
    if (macho_name_equals(segment_name, "__DATA") && macho_name_equals(section_name, "__data")) {
        return MACHO_SECTION_CLASS_DATA;
    }
    if (macho_name_equals(segment_name, "__DATA") &&
        (macho_name_equals(section_name, "__bss") || macho_name_equals(section_name, "__common")) &&
        (flags & MACHO_SECTION_TYPE_MASK) == MACHO_S_ZEROFILL) {
        return MACHO_SECTION_CLASS_BSS;
    }
    return MACHO_SECTION_CLASS_NONE;
}

static void macho_store_u32_be(unsigned char *out, uint32_t value) {
    out[0] = (unsigned char)((value >> 24U) & 0xffU);
    out[1] = (unsigned char)((value >> 16U) & 0xffU);
    out[2] = (unsigned char)((value >> 8U) & 0xffU);
    out[3] = (unsigned char)(value & 0xffU);
}

static int macho_page_hasher_init(MachoPageHasher *hasher, size_t hash_count) {
    hasher->hashes = (unsigned char *)rt_malloc(hash_count * CRYPTO_SHA256_DIGEST_SIZE);
    if (hasher->hashes == 0 && hash_count != 0U) {
        return -1;
    }
    hasher->hash_count = hash_count;
    hasher->hash_index = 0U;
    hasher->page_used = 0U;
    crypto_sha256_init(&hasher->context);
    return 0;
}

static int macho_page_hasher_update(MachoPageHasher *hasher, const unsigned char *data, size_t size) {
    size_t offset = 0U;
    while (offset < size) {
        size_t space = (size_t)MACHO_PAGE_SIZE - hasher->page_used;
        size_t chunk = size - offset;
        if (chunk > space) {
            chunk = space;
        }
        crypto_sha256_update(&hasher->context, data + offset, chunk);
        hasher->page_used += chunk;
        offset += chunk;
        if (hasher->page_used == (size_t)MACHO_PAGE_SIZE) {
            if (hasher->hash_index >= hasher->hash_count) {
                return -1;
            }
            crypto_sha256_final(&hasher->context, hasher->hashes + hasher->hash_index * CRYPTO_SHA256_DIGEST_SIZE);
            hasher->hash_index += 1U;
            hasher->page_used = 0U;
            crypto_sha256_init(&hasher->context);
        }
    }
    return 0;
}

static int macho_page_hasher_update_zeroes(MachoPageHasher *hasher, uint64_t size) {
    static const unsigned char zeroes[64] = {0U};
    while (size != 0ULL) {
        size_t chunk = size > (uint64_t)sizeof(zeroes) ? sizeof(zeroes) : (size_t)size;
        if (macho_page_hasher_update(hasher, zeroes, chunk) != 0) {
            return -1;
        }
        size -= (uint64_t)chunk;
    }
    return 0;
}

static int macho_page_hasher_finish(MachoPageHasher *hasher) {
    if (hasher->page_used != 0U) {
        if (hasher->hash_index >= hasher->hash_count) {
            return -1;
        }
        crypto_sha256_final(&hasher->context, hasher->hashes + hasher->hash_index * CRYPTO_SHA256_DIGEST_SIZE);
        hasher->hash_index += 1U;
        hasher->page_used = 0U;
    }
    return hasher->hash_index == hasher->hash_count ? 0 : -1;
}

static unsigned char *macho_make_code_signature(const unsigned char *image, size_t image_size,
                                                uint64_t code_limit, size_t *signature_size_out) {
    const uint32_t superblob_header_size = 20U;
    const uint32_t code_directory_fixed_size = 88U;
    const uint32_t hash_offset = code_directory_fixed_size + MACHO_CODE_DIRECTORY_IDENT_SIZE;
    uint32_t code_slots;
    uint32_t code_directory_size;
    uint32_t signature_size;
    unsigned char *signature;
    MachoPageHasher hasher;

    if (code_limit == 0ULL || code_limit > 0xffffffffULL || image_size > code_limit) {
        return 0;
    }
    code_slots = (uint32_t)((code_limit + MACHO_PAGE_SIZE - 1ULL) / MACHO_PAGE_SIZE);
    code_directory_size = hash_offset + code_slots * CRYPTO_SHA256_DIGEST_SIZE;
    signature_size = superblob_header_size + code_directory_size;
    signature = (unsigned char *)rt_malloc(signature_size);
    if (signature == 0) {
        return 0;
    }
    if (macho_page_hasher_init(&hasher, code_slots) != 0) {
        rt_free(signature);
        return 0;
    }
    if (macho_page_hasher_update(&hasher, image, image_size) != 0 ||
        (code_limit > (uint64_t)image_size && macho_page_hasher_update_zeroes(&hasher, code_limit - (uint64_t)image_size) != 0) ||
        macho_page_hasher_finish(&hasher) != 0) {
        rt_free(hasher.hashes);
        rt_free(signature);
        return 0;
    }

    memset(signature, 0, signature_size);
    macho_store_u32_be(signature + 0U, 0xfade0cc0U);
    macho_store_u32_be(signature + 4U, signature_size);
    macho_store_u32_be(signature + 8U, 1U);
    macho_store_u32_be(signature + 12U, 0U);
    macho_store_u32_be(signature + 16U, superblob_header_size);
    macho_store_u32_be(signature + superblob_header_size + 0U, 0xfade0c02U);
    macho_store_u32_be(signature + superblob_header_size + 4U, code_directory_size);
    macho_store_u32_be(signature + superblob_header_size + 8U, 0x20400U);
    macho_store_u32_be(signature + superblob_header_size + 12U, 0x2U);
    macho_store_u32_be(signature + superblob_header_size + 16U, hash_offset);
    macho_store_u32_be(signature + superblob_header_size + 20U, code_directory_fixed_size);
    macho_store_u32_be(signature + superblob_header_size + 24U, 0U);
    macho_store_u32_be(signature + superblob_header_size + 28U, code_slots);
    macho_store_u32_be(signature + superblob_header_size + 32U, (uint32_t)code_limit);
    signature[superblob_header_size + 36U] = CRYPTO_SHA256_DIGEST_SIZE;
    signature[superblob_header_size + 37U] = 2U;
    signature[superblob_header_size + 39U] = 14U;
    memcpy(signature + superblob_header_size + code_directory_fixed_size, MACHO_CODE_DIRECTORY_IDENT, MACHO_CODE_DIRECTORY_IDENT_SIZE);
    memcpy(signature + superblob_header_size + hash_offset, hasher.hashes, (size_t)code_slots * CRYPTO_SHA256_DIGEST_SIZE);
    rt_free(hasher.hashes);
    *signature_size_out = signature_size;
    return signature;
}

static MachoInputSection *macho_section_by_ordinal(MachoInputObject *object, uint32_t ordinal) {
    size_t i;
    for (i = 0; i < object->section_count; ++i) {
        if (object->sections[i].ordinal == ordinal) {
            return &object->sections[i];
        }
    }
    return 0;
}

static int macho_section_is_supported_payload(const MachoInputSection *section) {
    if (section->section_class != MACHO_SECTION_CLASS_NONE) {
        return 1;
    }
    return section->size == 0ULL;
}

static int macho_section_has_file_bytes(const MachoInputSection *section) {
    return section->size != 0ULL && section->offset != 0U && section->section_class != MACHO_SECTION_CLASS_BSS;
}

static int macho_find_global_symbol(MachoLinkImage *link, const char *name, uint64_t *value_out) {
    size_t i;
    for (i = 0; i < link->global_count; ++i) {
        if (rt_strcmp(link->globals[i].name, name) == 0) {
            *value_out = link->globals[i].value;
            return 0;
        }
    }
    return -1;
}

static int macho_symbol_value_in_object(MachoInputObject *object, const char *name, unsigned char type, unsigned char sect,
                                        uint64_t value, uint64_t *value_out, char *error_out, size_t error_size) {
    MachoInputSection *section;

    if ((type & MACHO_N_TYPE_MASK) != MACHO_N_SECT) {
        set_link_error(error_out, error_size, "unsupported Mach-O relocation symbol kind", name);
        return -1;
    }
    section = macho_section_by_ordinal(object, sect);
    if (section == 0 || value < section->addr || value - section->addr > section->size) {
        set_link_error(error_out, error_size, "invalid Mach-O relocation symbol value", name);
        return -1;
    }
    *value_out = section->out_addr + (value - section->addr);
    return 0;
}

static int macho_symbol_value(MachoLinkImage *link, size_t object_index, uint32_t symbol_index, int external,
                              uint64_t *value_out, char *error_out, size_t error_size) {
    MachoInputObject *object = &link->objects[object_index];
    const unsigned char *symbol;
    unsigned char type;
    unsigned char sect;
    uint64_t value;

    if (external) {
        uint32_t strx;
        const char *name;
        if (symbol_index >= object->nsyms) {
            set_link_error(error_out, error_size, "invalid Mach-O relocation symbol index", "");
            return -1;
        }
        symbol = object->file + object->symoff + (uint64_t)symbol_index * MACHO_NLIST64_SIZE;
        strx = read_u32(symbol);
        type = symbol[4];
        sect = symbol[5];
        value = read_u64(symbol + 8U);
        name = strx < object->strsize ? (const char *)(object->file + object->stroff + strx) : "";
        if ((type & MACHO_N_TYPE_MASK) == MACHO_N_UNDF) {
            if (macho_find_global_symbol(link, name, value_out) == 0) {
                return 0;
            }
            set_link_error(error_out, error_size, "undefined Mach-O arm64 symbol", name);
            return -1;
        }
        return macho_symbol_value_in_object(object, name, type, sect, value, value_out, error_out, error_size);
    }

    {
        MachoInputSection *section = macho_section_by_ordinal(object, symbol_index);
        if (section == 0) {
            set_link_error(error_out, error_size, "invalid Mach-O section relocation target", "");
            return -1;
        }
        *value_out = section->out_addr;
    }
    return 0;
}

static int macho_apply_arm64_relocations(MachoLinkImage *link, size_t object_index, unsigned char *image, size_t image_size, char *error_out, size_t error_size) {
    MachoInputObject *object = &link->objects[object_index];
    size_t section_index;
    for (section_index = 0; section_index < object->section_count; ++section_index) {
        MachoInputSection *section = &object->sections[section_index];
        uint32_t reloc_index;
        int has_addend = 0;
        uint64_t addend_offset = 0ULL;
        int64_t addend = 0;

        if (section->nreloc == 0U) {
            continue;
        }
        if (section->out_offset == 0ULL) {
            set_link_error(error_out, error_size, "Mach-O relocation targets unsupported section", section->sectname);
            return -1;
        }
        for (reloc_index = 0; reloc_index < section->nreloc; ++reloc_index) {
            const unsigned char *reloc = object->file + section->reloff + (uint64_t)reloc_index * MACHO_RELOC_SIZE;
            uint32_t address_word = read_u32(reloc);
            uint32_t info = read_u32(reloc + 4U);
            uint32_t symbol_index = info & 0x00ffffffU;
            int pcrel = (int)((info >> 24U) & 1U);
            uint32_t length = (info >> 25U) & 3U;
            int external = (int)((info >> 27U) & 1U);
            uint32_t type = (info >> 28U) & 0xfU;
            uint64_t offset;
            uint64_t place;
            uint64_t target;
            unsigned char *patch;
            uint32_t instruction;

            if ((address_word & 0x80000000U) != 0U) {
                set_link_error(error_out, error_size, "scattered Mach-O arm64 relocations are not supported", section->sectname);
                return -1;
            }
            offset = (uint64_t)address_word;
            if (offset + 4ULL > section->size || !range_valid(section->out_offset + offset, 4U, image_size)) {
                set_link_error(error_out, error_size, "invalid Mach-O arm64 relocation offset", section->sectname);
                return -1;
            }
            place = section->out_addr + offset;
            patch = image + section->out_offset + offset;

            if (type == MACHO_ARM64_RELOC_ADDEND) {
                if (pcrel || length != 2U || external) {
                    set_link_error(error_out, error_size, "invalid Mach-O arm64 addend relocation", section->sectname);
                    return -1;
                }
                has_addend = 1;
                addend_offset = offset;
                addend = macho_sign_extend_24(symbol_index);
                continue;
            }
            if (macho_symbol_value(link, object_index, symbol_index, external, &target, error_out, error_size) != 0) {
                return -1;
            }
            if (has_addend) {
                int64_t adjusted_target;
                if (offset != addend_offset || (type != MACHO_ARM64_RELOC_PAGE21 && type != MACHO_ARM64_RELOC_PAGEOFF12)) {
                    set_link_error(error_out, error_size, "unpaired Mach-O arm64 addend relocation", section->sectname);
                    return -1;
                }
                adjusted_target = (int64_t)target + addend;
                if (adjusted_target < 0) {
                    set_link_error(error_out, error_size, "Mach-O arm64 addend relocation is out of range", section->sectname);
                    return -1;
                }
                target = (uint64_t)adjusted_target;
                has_addend = 0;
                addend = 0;
            }

            if (type == MACHO_ARM64_RELOC_BRANCH26) {
                int64_t delta;
                int64_t imm26;
                if (!pcrel || length != 2U) {
                    set_link_error(error_out, error_size, "invalid Mach-O arm64 branch relocation", section->sectname);
                    return -1;
                }
                delta = (int64_t)target - (int64_t)place;
                if ((delta & 3LL) != 0LL) {
                    set_link_error(error_out, error_size, "unaligned Mach-O arm64 branch relocation", section->sectname);
                    return -1;
                }
                imm26 = delta >> 2;
                if (imm26 < -(1LL << 25) || imm26 >= (1LL << 25)) {
                    set_link_error(error_out, error_size, "Mach-O arm64 branch relocation is out of range", section->sectname);
                    return -1;
                }
                instruction = read_u32(patch);
                instruction = (instruction & 0xfc000000U) | ((uint32_t)imm26 & 0x03ffffffU);
                write_u32(patch, instruction);
            } else if (type == MACHO_ARM64_RELOC_PAGE21) {
                int64_t page_delta = (int64_t)(target & ~0xfffULL) - (int64_t)(place & ~0xfffULL);
                int64_t page_imm = page_delta >> 12;
                uint32_t immlo;
                uint32_t immhi;
                if (!pcrel || length != 2U || (page_delta & 0xfffLL) != 0LL) {
                    set_link_error(error_out, error_size, "invalid Mach-O arm64 page relocation", section->sectname);
                    return -1;
                }
                if (page_imm < -(1LL << 20) || page_imm >= (1LL << 20)) {
                    set_link_error(error_out, error_size, "Mach-O arm64 page relocation is out of range", section->sectname);
                    return -1;
                }
                immlo = (uint32_t)page_imm & 3U;
                immhi = ((uint32_t)page_imm >> 2U) & 0x7ffffU;
                instruction = read_u32(patch);
                instruction = (instruction & ~0x60ffffe0U) | (immlo << 29U) | (immhi << 5U);
                write_u32(patch, instruction);
            } else if (type == MACHO_ARM64_RELOC_PAGEOFF12) {
                uint32_t pageoff = (uint32_t)(target & 0xfffULL);
                if (pcrel || length != 2U) {
                    set_link_error(error_out, error_size, "invalid Mach-O arm64 pageoff relocation", section->sectname);
                    return -1;
                }
                instruction = read_u32(patch);
                if ((instruction & 0x7f000000U) != 0x11000000U) {
                    if ((instruction & 0x3b000000U) == 0x39000000U) {
                        uint32_t scale = (instruction >> 30U) & 3U;
                        if ((instruction & 0x04000000U) != 0U && ((instruction >> 22U) & 3U) == 3U) {
                            scale = 4U;
                        }
                        if ((pageoff & ((1U << scale) - 1U)) != 0U) {
                            set_link_error(error_out, error_size, "unaligned Mach-O arm64 load/store pageoff relocation", section->sectname);
                            return -1;
                        }
                        instruction = (instruction & ~0x003ffc00U) | ((pageoff >> scale) << 10U);
                        write_u32(patch, instruction);
                        continue;
                    }
                    set_link_error(error_out, error_size, "unsupported Mach-O arm64 pageoff relocation instruction", section->sectname);
                    return -1;
                }
                instruction = (instruction & ~0x003ffc00U) | (pageoff << 10U);
                write_u32(patch, instruction);
            } else if (type == MACHO_ARM64_RELOC_UNSIGNED) {
                if (pcrel) {
                    set_link_error(error_out, error_size, "PC-relative unsigned Mach-O arm64 relocation is not supported", section->sectname);
                    return -1;
                }
                if (length == 3U) {
                    if (offset + 8ULL > section->size) {
                        set_link_error(error_out, error_size, "invalid Mach-O arm64 unsigned relocation", section->sectname);
                        return -1;
                    }
                    write_u64(patch, target + read_u64(patch));
                } else if (length == 2U) {
                    uint64_t patched = target + read_u32(patch);
                    if (patched > 0xffffffffULL) {
                        set_link_error(error_out, error_size, "Mach-O arm64 unsigned relocation is out of range", section->sectname);
                        return -1;
                    }
                    write_u32(patch, (uint32_t)patched);
                } else {
                    set_link_error(error_out, error_size, "unsupported Mach-O arm64 unsigned relocation width", section->sectname);
                    return -1;
                }
            } else {
                set_link_error(error_out, error_size, "unsupported Mach-O arm64 relocation type", section->sectname);
                return -1;
            }
        }
        if (has_addend) {
            set_link_error(error_out, error_size, "unpaired Mach-O arm64 addend relocation", section->sectname);
            return -1;
        }
    }
    return 0;
}

static void macho_write_section64(unsigned char *section, const char *section_name, const char *segment_name,
                                  uint64_t address, uint64_t size, uint32_t offset, uint32_t alignment, uint32_t flags) {
    macho_store_fixed_name(section, section_name);
    macho_store_fixed_name(section + 16U, segment_name);
    write_u64(section + 32U, address);
    write_u64(section + 40U, size);
    write_u32(section + 48U, offset);
    write_u32(section + 52U, alignment);
    write_u32(section + 56U, 0U);
    write_u32(section + 60U, 0U);
    write_u32(section + 64U, flags);
    write_u32(section + 68U, 0U);
    write_u32(section + 72U, 0U);
    write_u32(section + 76U, 0U);
}

static void macho_write_segment64(unsigned char *segment, const char *segment_name, uint64_t vmaddr, uint64_t vmsize,
                                  uint64_t fileoff, uint64_t filesize, uint32_t maxprot, uint32_t initprot,
                                  uint32_t nsects, uint32_t flags) {
    write_u32(segment + 0U, MACHO_LC_SEGMENT_64);
    write_u32(segment + 4U, 72U + 80U * nsects);
    macho_store_fixed_name(segment + 8U, segment_name);
    write_u64(segment + 24U, vmaddr);
    write_u64(segment + 32U, vmsize);
    write_u64(segment + 40U, fileoff);
    write_u64(segment + 48U, filesize);
    write_u32(segment + 56U, maxprot);
    write_u32(segment + 60U, initprot);
    write_u32(segment + 64U, nsects);
    write_u32(segment + 68U, flags);
}

static int macho_parse_loaded_object(MachoInputObject *object, const char *path, unsigned char *file, size_t size,
                                     const char *entry_symbol, char *error_out, size_t error_size) {
    uint32_t cputype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint64_t load_offset;
    uint32_t command_index;
    uint32_t section_ordinal = 1U;
    size_t i;

    memset(object, 0, sizeof(*object));
    rt_copy_string(object->path, sizeof(object->path), path);
    object->file = file;
    object->size = size;
    if (object->size < MACHO_HEADER64_SIZE || read_u32(object->file) != MACHO64_MAGIC) {
        set_link_error(error_out, error_size, "unsupported Mach-O object format", path);
        return -1;
    }
    cputype = read_u32(object->file + 4U);
    filetype = read_u32(object->file + 12U);
    ncmds = read_u32(object->file + 16U);
    sizeofcmds = read_u32(object->file + 20U);
    if (cputype != MACHO_CPU_TYPE_ARM64 || filetype != MACHO_FILETYPE_OBJECT || !range_valid(MACHO_HEADER64_SIZE, sizeofcmds, object->size)) {
        set_link_error(error_out, error_size, "unsupported Mach-O arm64 relocatable object", path);
        return -1;
    }
    load_offset = MACHO_HEADER64_SIZE;
    for (command_index = 0; command_index < ncmds; ++command_index) {
        uint64_t command_offset = load_offset;
        uint32_t command;
        uint32_t command_size;

        if (!range_valid(command_offset, 8U, object->size) || command_offset + 8U > MACHO_HEADER64_SIZE + sizeofcmds) {
            set_link_error(error_out, error_size, "invalid Mach-O load command", path);
            return -1;
        }
        command = read_u32(object->file + command_offset);
        command_size = read_u32(object->file + command_offset + 4U);
        if (command_size < 8U || !range_valid(command_offset, command_size, object->size) || command_offset + command_size > MACHO_HEADER64_SIZE + sizeofcmds) {
            set_link_error(error_out, error_size, "invalid Mach-O load command", path);
            return -1;
        }
        if (command == MACHO_LC_SEGMENT_64) {
            uint32_t nsects;
            uint64_t minimum_command_size;
            uint32_t section_index;

            if (command_size < 72U) {
                set_link_error(error_out, error_size, "invalid Mach-O segment command", path);
                return -1;
            }
            nsects = read_u32(object->file + command_offset + 64U);
            minimum_command_size = 72ULL + ((uint64_t)nsects * MACHO_SECTION64_SIZE);
            if (command_size < minimum_command_size || object->section_count + nsects > MACHO_MAX_SECTIONS) {
                set_link_error(error_out, error_size, "invalid Mach-O section table", path);
                return -1;
            }
            for (section_index = 0; section_index < nsects; ++section_index) {
                uint64_t section_offset = command_offset + 72ULL + ((uint64_t)section_index * MACHO_SECTION64_SIZE);
                MachoInputSection *section = &object->sections[object->section_count++];
                macho_copy_fixed_name(section->sectname, object->file + section_offset);
                macho_copy_fixed_name(section->segname, object->file + section_offset + 16U);
                section->addr = read_u64(object->file + section_offset + 32U);
                section->size = read_u64(object->file + section_offset + 40U);
                section->offset = read_u32(object->file + section_offset + 48U);
                section->align = read_u32(object->file + section_offset + 52U);
                section->reloff = read_u32(object->file + section_offset + 56U);
                section->nreloc = read_u32(object->file + section_offset + 60U);
                section->flags = read_u32(object->file + section_offset + 64U);
                section->ordinal = section_ordinal++;
                section->section_class = macho_classify_section(section->segname, section->sectname, section->flags);
                object->relocation_count += section->nreloc;
                if (macho_section_has_file_bytes(section) && !range_valid(section->offset, section->size, object->size)) {
                    set_link_error(error_out, error_size, "Mach-O section extends past end of file", path);
                    return -1;
                }
                if (section->nreloc != 0U && !range_valid(section->reloff, (uint64_t)section->nreloc * MACHO_RELOC_SIZE, object->size)) {
                    set_link_error(error_out, error_size, "Mach-O relocation table extends past end of file", path);
                    return -1;
                }
                if (macho_name_equals(section->segname, "__TEXT") && macho_name_equals(section->sectname, "__text")) {
                    object->text_section = section;
                } else if ((macho_name_equals(section->segname, "__TEXT") || macho_name_equals(section->segname, "__DATA")) && macho_name_equals(section->sectname, "__const")) {
                    object->const_section = section;
                } else if (macho_name_equals(section->segname, "__TEXT") && macho_name_equals(section->sectname, "__cstring")) {
                    object->cstring_section = section;
                } else if (macho_name_equals(section->segname, "__DATA") && macho_name_equals(section->sectname, "__data")) {
                    object->data_section = section;
                } else if (macho_name_equals(section->segname, "__DATA") &&
                           (macho_name_equals(section->sectname, "__bss") || macho_name_equals(section->sectname, "__common"))) {
                    /* BSS is handled by the section class and written as a zero-fill output section. */
                }
            }
        } else if (command == MACHO_LC_SYMTAB) {
            if (command_size < 24U) {
                set_link_error(error_out, error_size, "invalid Mach-O symbol table command", path);
                return -1;
            }
            object->symoff = read_u32(object->file + command_offset + 8U);
            object->nsyms = read_u32(object->file + command_offset + 12U);
            object->stroff = read_u32(object->file + command_offset + 16U);
            object->strsize = read_u32(object->file + command_offset + 20U);
            if (!range_valid(object->symoff, (uint64_t)object->nsyms * MACHO_NLIST64_SIZE, object->size) || !range_valid(object->stroff, object->strsize, object->size)) {
                set_link_error(error_out, error_size, "Mach-O symbol table extends past end of file", path);
                return -1;
            }
        }
        load_offset += command_size;
    }
    if (object->symoff == 0U || object->stroff == 0U) {
        set_link_error(error_out, error_size, "Mach-O arm64 object is missing symbols", path);
        return -1;
    }
    if (object->text_section != 0 && macho_section_has_file_bytes(object->text_section) && !range_valid(object->text_section->offset, object->text_section->size, object->size)) {
        set_link_error(error_out, error_size, "Mach-O __TEXT,__text section extends past end of file", path);
        return -1;
    }
    for (i = 0; i < object->section_count; ++i) {
        const MachoInputSection *section = &object->sections[i];
        if (!macho_section_is_supported_payload(section)) {
            set_link_error(error_out, error_size, "unsupported Mach-O arm64 section in current backend", section->sectname);
            return -1;
        }
        if (macho_section_has_file_bytes(section) && !range_valid(section->offset, section->size, object->size)) {
            set_link_error(error_out, error_size, "Mach-O section extends past end of file", section->sectname);
            return -1;
        }
    }
    for (i = 0; i < object->nsyms; ++i) {
        const unsigned char *symbol = object->file + object->symoff + i * MACHO_NLIST64_SIZE;
        uint32_t strx = read_u32(symbol);
        unsigned char type = symbol[4];
        unsigned char sect = symbol[5];
        uint64_t value = read_u64(symbol + 8U);
        const char *name;
        if (strx >= object->strsize) {
            continue;
        }
        name = (const char *)(object->file + object->stroff + strx);
        if (object->text_section != 0 && (type & MACHO_N_TYPE_MASK) == MACHO_N_SECT && sect == object->text_section->ordinal && rt_strcmp(name, entry_symbol) == 0) {
            object->entry_value = value;
            object->entry_found = 1;
            break;
        }
        if (object->text_section != 0 && rt_strcmp(entry_symbol, "_start") == 0 && (type & MACHO_N_TYPE_MASK) == MACHO_N_SECT && sect == object->text_section->ordinal && rt_strcmp(name, "__start") == 0) {
            object->entry_value = value;
            object->entry_found = 1;
            break;
        }
    }
    return 0;
}

static int macho_parse_input_object(MachoInputObject *object, const char *path, const char *entry_symbol,
                                    char *error_out, size_t error_size) {
    unsigned char *file;
    size_t size;

    if (read_file_alloc(path, LINKER_MAX_OBJECT_SIZE, &file, &size, error_out, error_size) != 0) {
        memset(object, 0, sizeof(*object));
        return -1;
    }
    return macho_parse_loaded_object(object, path, file, size, entry_symbol, error_out, error_size);
}

static MachoOutputSection *macho_output_section_for_class(MachoLinkImage *link, MachoSectionClass section_class) {
    if (section_class == MACHO_SECTION_CLASS_TEXT) {
        return &link->text;
    }
    if (section_class == MACHO_SECTION_CLASS_CONST) {
        return &link->constant;
    }
    if (section_class == MACHO_SECTION_CLASS_CSTRING) {
        return &link->cstring;
    }
    if (section_class == MACHO_SECTION_CLASS_DATA) {
        return &link->data;
    }
    if (section_class == MACHO_SECTION_CLASS_BSS) {
        return &link->bss;
    }
    return 0;
}

static void macho_init_output_sections(MachoLinkImage *link) {
    memset(&link->text, 0, sizeof(link->text));
    memset(&link->constant, 0, sizeof(link->constant));
    memset(&link->cstring, 0, sizeof(link->cstring));
    memset(&link->data, 0, sizeof(link->data));
    memset(&link->bss, 0, sizeof(link->bss));
    link->text.section_name = "__text";
    link->text.segment_name = "__TEXT";
    link->text.section_class = MACHO_SECTION_CLASS_TEXT;
    link->text.flags = MACHO_TEXT_SECTION_FLAGS;
    link->constant.section_name = "__const";
    link->constant.segment_name = "__TEXT";
    link->constant.section_class = MACHO_SECTION_CLASS_CONST;
    link->cstring.section_name = "__cstring";
    link->cstring.segment_name = "__TEXT";
    link->cstring.section_class = MACHO_SECTION_CLASS_CSTRING;
    link->data.section_name = "__data";
    link->data.segment_name = "__DATA";
    link->data.section_class = MACHO_SECTION_CLASS_DATA;
    link->bss.section_name = "__bss";
    link->bss.segment_name = "__DATA";
    link->bss.section_class = MACHO_SECTION_CLASS_BSS;
    link->bss.flags = MACHO_S_ZEROFILL;
}

static int macho_compute_output_section_layout(MachoLinkImage *link, MachoSectionClass section_class, char *error_out, size_t error_size) {
    MachoOutputSection *output = macho_output_section_for_class(link, section_class);
    uint64_t cursor = 0ULL;
    size_t object_index;

    if (output == 0) {
        return 0;
    }
    for (object_index = 0; object_index < link->object_count; ++object_index) {
        MachoInputObject *object = &link->objects[object_index];
        size_t section_index;
        for (section_index = 0; section_index < object->section_count; ++section_index) {
            MachoInputSection *section = &object->sections[section_index];
            uint64_t alignment;
            if (section->section_class != section_class || section->size == 0ULL) {
                continue;
            }
            if (section->align >= 32U) {
                set_link_error(error_out, error_size, "unsupported Mach-O section alignment", section->sectname);
                return -1;
            }
            if (section->align > output->align) {
                output->align = section->align;
            }
            if (output->flags == 0U) {
                output->flags = section->flags;
            }
            alignment = 1ULL << section->align;
            cursor = macho_align_up(cursor, alignment);
            section->out_offset = cursor;
            section->out_addr = cursor;
            cursor += section->size;
        }
    }
    output->size = cursor;
    return 0;
}

static void macho_apply_output_section_base(MachoLinkImage *link, MachoSectionClass section_class, uint64_t file_offset, uint64_t vm_addr) {
    MachoOutputSection *output = macho_output_section_for_class(link, section_class);
    size_t object_index;

    if (output == 0 || output->size == 0ULL) {
        return;
    }
    output->out_offset = file_offset;
    output->out_addr = vm_addr;
    for (object_index = 0; object_index < link->object_count; ++object_index) {
        MachoInputObject *object = &link->objects[object_index];
        size_t section_index;
        for (section_index = 0; section_index < object->section_count; ++section_index) {
            MachoInputSection *section = &object->sections[section_index];
            if (section->section_class == section_class && section->size != 0ULL) {
                section->out_offset += file_offset;
                section->out_addr += vm_addr;
            }
        }
    }
}

static int macho_write_uleb128(unsigned char *buffer, size_t buffer_size, size_t *offset, uint64_t value) {
    do {
        unsigned char byte = (unsigned char)(value & 0x7fU);
        value >>= 7U;
        if (value != 0ULL) {
            byte |= 0x80U;
        }
        if (*offset >= buffer_size) {
            return -1;
        }
        buffer[*offset] = byte;
        *offset += 1U;
    } while (value != 0ULL);
    return 0;
}

static int macho_section_rebase_segment(const MachoInputSection *section, uint64_t data_vmaddr, unsigned char *segment_index_out, uint64_t *segment_vmaddr_out) {
    if (section->section_class == MACHO_SECTION_CLASS_TEXT || section->section_class == MACHO_SECTION_CLASS_CONST || section->section_class == MACHO_SECTION_CLASS_CSTRING) {
        *segment_index_out = 1U;
        *segment_vmaddr_out = MACHO_BASE_ADDRESS;
        return 0;
    }
    if (section->section_class == MACHO_SECTION_CLASS_DATA) {
        *segment_index_out = 2U;
        *segment_vmaddr_out = data_vmaddr;
        return 0;
    }
    return -1;
}

static int macho_add_rebase_entry(MachoLinkImage *link, unsigned char segment_index, uint64_t segment_offset, char *error_out, size_t error_size) {
    if (link->rebase_count >= MACHO_MAX_REBASES) {
        set_link_error(error_out, error_size, "too many Mach-O rebases", "");
        return -1;
    }
    link->rebases[link->rebase_count].segment_index = segment_index;
    link->rebases[link->rebase_count].segment_offset = segment_offset;
    link->rebase_count += 1U;
    return 0;
}

static int macho_collect_rebase_entries(MachoLinkImage *link, uint64_t data_vmaddr, char *error_out, size_t error_size) {
    size_t object_index;
    link->rebase_count = 0U;
    for (object_index = 0; object_index < link->object_count; ++object_index) {
        MachoInputObject *object = &link->objects[object_index];
        size_t section_index;
        for (section_index = 0; section_index < object->section_count; ++section_index) {
            MachoInputSection *section = &object->sections[section_index];
            uint32_t reloc_index;
            if (section->nreloc == 0U || section->section_class == MACHO_SECTION_CLASS_BSS) {
                continue;
            }
            for (reloc_index = 0U; reloc_index < section->nreloc; ++reloc_index) {
                const unsigned char *reloc = object->file + section->reloff + (uint64_t)reloc_index * MACHO_RELOC_SIZE;
                uint32_t address_word = read_u32(reloc);
                uint32_t info = read_u32(reloc + 4U);
                uint32_t type = (info >> 28U) & 0xfU;
                uint32_t length = (info >> 25U) & 3U;
                int pcrel = (int)((info >> 24U) & 1U);
                uint64_t offset = (uint64_t)(address_word & 0x7fffffffU);
                unsigned char segment_index;
                uint64_t segment_vmaddr;
                if ((address_word & 0x80000000U) != 0U || type != MACHO_ARM64_RELOC_UNSIGNED || length != 3U || pcrel) {
                    continue;
                }
                if (offset + 8ULL > section->size) {
                    set_link_error(error_out, error_size, "invalid Mach-O rebase relocation offset", section->sectname);
                    return -1;
                }
                if (macho_section_rebase_segment(section, data_vmaddr, &segment_index, &segment_vmaddr) != 0) {
                    continue;
                }
                if (section->out_addr + offset < segment_vmaddr) {
                    set_link_error(error_out, error_size, "invalid Mach-O rebase address", section->sectname);
                    return -1;
                }
                if (macho_add_rebase_entry(link, segment_index, section->out_addr + offset - segment_vmaddr, error_out, error_size) != 0) {
                    return -1;
                }
            }
        }
    }
    return 0;
}

static unsigned char *macho_make_rebase_info(const MachoLinkImage *link, size_t *size_out) {
    size_t capacity = link->rebase_count == 0U ? 0U : 2U + link->rebase_count * 12U;
    unsigned char *buffer;
    size_t offset = 0U;
    size_t i;

    if (link->rebase_count == 0U) {
        *size_out = 0U;
        return 0;
    }
    buffer = (unsigned char *)rt_malloc(capacity);
    if (buffer == 0) {
        return 0;
    }
    buffer[offset++] = (unsigned char)(MACHO_REBASE_OPCODE_SET_TYPE_IMM | MACHO_REBASE_TYPE_POINTER);
    for (i = 0U; i < link->rebase_count; ++i) {
        if (offset >= capacity) {
            rt_free(buffer);
            return 0;
        }
        buffer[offset++] = (unsigned char)(MACHO_REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | (link->rebases[i].segment_index & 0xfU));
        if (macho_write_uleb128(buffer, capacity, &offset, link->rebases[i].segment_offset) != 0) {
            rt_free(buffer);
            return 0;
        }
        if (offset >= capacity) {
            rt_free(buffer);
            return 0;
        }
        buffer[offset++] = (unsigned char)(MACHO_REBASE_OPCODE_DO_REBASE_IMM_TIMES | 1U);
    }
    if (offset >= capacity) {
        rt_free(buffer);
        return 0;
    }
    buffer[offset++] = MACHO_REBASE_OPCODE_DONE;
    *size_out = offset;
    return buffer;
}

static int macho_add_global_symbol(MachoLinkImage *link, const char *name, size_t object_index, uint32_t symbol_index, uint64_t value, char *error_out, size_t error_size) {
    size_t i;
    for (i = 0; i < link->global_count; ++i) {
        if (rt_strcmp(link->globals[i].name, name) == 0) {
            set_link_error(error_out, error_size, "duplicate Mach-O arm64 symbol", name);
            return -1;
        }
    }
    if (link->global_count >= LINKER_MAX_GLOBALS) {
        set_link_error(error_out, error_size, "too many Mach-O arm64 symbols", name);
        return -1;
    }
    rt_copy_string(link->globals[link->global_count].name, sizeof(link->globals[link->global_count].name), name);
    link->globals[link->global_count].object_index = object_index;
    link->globals[link->global_count].symbol_index = symbol_index;
    link->globals[link->global_count].value = value;
    link->global_count += 1U;
    return 0;
}

static int macho_collect_global_symbols(MachoLinkImage *link, const char *entry_symbol, char *error_out, size_t error_size) {
    size_t object_index;

    link->global_count = 0U;
    link->entry_found = 0;
    for (object_index = 0; object_index < link->object_count; ++object_index) {
        MachoInputObject *object = &link->objects[object_index];
        size_t symbol_index;
        for (symbol_index = 0; symbol_index < object->nsyms; ++symbol_index) {
            const unsigned char *symbol = object->file + object->symoff + symbol_index * MACHO_NLIST64_SIZE;
            uint32_t strx = read_u32(symbol);
            unsigned char type = symbol[4];
            unsigned char sect = symbol[5];
            uint64_t value = read_u64(symbol + 8U);
            const char *name;
            uint64_t runtime_value;
            if (strx >= object->strsize || (type & MACHO_N_EXT) == 0U || (type & MACHO_N_TYPE_MASK) != MACHO_N_SECT) {
                continue;
            }
            name = (const char *)(object->file + object->stroff + strx);
            if (macho_symbol_value_in_object(object, name, type, sect, value, &runtime_value, error_out, error_size) != 0) {
                return -1;
            }
            if (macho_add_global_symbol(link, name, object_index, (uint32_t)symbol_index, runtime_value, error_out, error_size) != 0) {
                return -1;
            }
            if (rt_strcmp(name, entry_symbol) == 0 || (rt_strcmp(entry_symbol, "_start") == 0 && rt_strcmp(name, "__start") == 0)) {
                link->entryoff = runtime_value - MACHO_BASE_ADDRESS;
                link->entry_found = 1;
            }
        }
        if (!link->entry_found && object->entry_found && object->text_section != 0) {
            link->entryoff = object->text_section->out_offset + (object->entry_value - object->text_section->addr);
            link->entry_found = 1;
        }
    }
    if (!link->entry_found) {
        set_link_error(error_out, error_size, "Mach-O arm64 entry symbol was not found in __TEXT,__text", entry_symbol);
        return -1;
    }
    return 0;
}

static void macho_copy_input_sections(MachoLinkImage *link, unsigned char *image, MachoSectionClass section_class) {
    size_t object_index;
    for (object_index = 0; object_index < link->object_count; ++object_index) {
        MachoInputObject *object = &link->objects[object_index];
        size_t section_index;
        for (section_index = 0; section_index < object->section_count; ++section_index) {
            MachoInputSection *section = &object->sections[section_index];
            if (section->section_class == section_class && macho_section_has_file_bytes(section)) {
                memcpy(image + section->out_offset, object->file + section->offset, (size_t)section->size);
            }
        }
    }
}

static uint64_t macho_count_input_sections(const MachoLinkImage *link) {
    uint64_t count = 0ULL;
    size_t object_index;
    for (object_index = 0; object_index < link->object_count; ++object_index) {
        count += (uint64_t)link->objects[object_index].section_count;
    }
    return count;
}

static int macho_write_stats(const MachoLinkImage *link,
                             uint64_t header_bytes,
                             uint64_t text_payload_size,
                             uint64_t text_file_size,
                             uint64_t text_vm_size,
                             uint64_t data_file_size,
                             uint64_t data_vm_size,
                             uint64_t signature_offset,
                             uint64_t code_limit,
                             uint64_t signature_size,
                             int compact) {
    if (rt_write_cstr(1, "Mach-O linker stats\nobjects: ") != 0) return -1;
    if (rt_write_uint(1, (unsigned long long)link->object_count) != 0) return -1;
    if (rt_write_cstr(1, "\nsections: ") != 0) return -1;
    if (rt_write_uint(1, macho_count_input_sections(link)) != 0) return -1;
    if (rt_write_cstr(1, "\ntext/const/cstring/data/bss payload: ") != 0) return -1;
    if (rt_write_uint(1, link->text.size) != 0 || rt_write_cstr(1, "/") != 0 ||
        rt_write_uint(1, link->constant.size) != 0 || rt_write_cstr(1, "/") != 0 ||
        rt_write_uint(1, link->cstring.size) != 0 || rt_write_cstr(1, "/") != 0 ||
        rt_write_uint(1, link->data.size) != 0 || rt_write_cstr(1, "/") != 0 ||
        rt_write_uint(1, link->bss.size) != 0) return -1;
    if (rt_write_cstr(1, "\nheaders/text-file/text-vm: ") != 0) return -1;
    if (rt_write_uint(1, header_bytes) != 0 || rt_write_cstr(1, "/") != 0 ||
        rt_write_uint(1, text_file_size) != 0 || rt_write_cstr(1, "/") != 0 ||
        rt_write_uint(1, text_vm_size) != 0) return -1;
    if (rt_write_cstr(1, "\ntext-payload/data-file/data-vm: ") != 0) return -1;
    if (rt_write_uint(1, text_payload_size) != 0 || rt_write_cstr(1, "/") != 0 ||
        rt_write_uint(1, data_file_size) != 0 || rt_write_cstr(1, "/") != 0 ||
        rt_write_uint(1, data_vm_size) != 0) return -1;
    if (rt_write_cstr(1, "\nsignature-offset/code-limit/signature/file: ") != 0) return -1;
    if (rt_write_uint(1, signature_offset) != 0 || rt_write_cstr(1, "/") != 0 ||
        rt_write_uint(1, code_limit) != 0 || rt_write_cstr(1, "/") != 0 ||
        rt_write_uint(1, signature_size) != 0 || rt_write_cstr(1, "/") != 0 ||
        rt_write_uint(1, signature_offset + signature_size) != 0) return -1;
    if (rt_write_cstr(1, "\nfile-padding-not-written: ") != 0) return -1;
    if (rt_write_uint(1, code_limit > signature_offset ? code_limit - signature_offset : 0ULL) != 0) return -1;
    if (rt_write_cstr(1, "\npolicy: ") != 0) return -1;
    if (rt_write_cstr(1, compact ? "compact" : "page-aligned") != 0) return -1;
    return rt_write_cstr(1, "\n");
}

static uint64_t macho_symbol_approx_size(const MachoInputObject *object, uint32_t symbol_index, const MachoInputSection *section, uint64_t value) {
    uint64_t next_value = section->addr + section->size;
    uint32_t i;

    for (i = 0U; i < object->nsyms; ++i) {
        const unsigned char *candidate;
        unsigned char type;
        unsigned char sect;
        uint64_t candidate_value;
        if (i == symbol_index) {
            continue;
        }
        candidate = object->file + object->symoff + (uint64_t)i * MACHO_NLIST64_SIZE;
        type = candidate[4];
        sect = candidate[5];
        if ((type & MACHO_N_STAB) != 0U || (type & MACHO_N_TYPE_MASK) != MACHO_N_SECT || sect != section->ordinal) {
            continue;
        }
        candidate_value = read_u64(candidate + 8U);
        if (candidate_value > value && candidate_value < next_value) {
            next_value = candidate_value;
        }
    }
    return next_value > value ? next_value - value : 0ULL;
}

static int macho_write_map_output_section(int fd, const MachoOutputSection *section) {
    if (section->size == 0ULL) {
        return 0;
    }
    return rt_write_cstr(fd, "section ") != 0 ||
           macho_write_hex64(fd, section->out_addr) != 0 ||
           rt_write_cstr(fd, " ") != 0 ||
           rt_write_uint(fd, section->size) != 0 ||
           rt_write_cstr(fd, " ") != 0 ||
           rt_write_cstr(fd, section->segment_name) != 0 ||
           rt_write_cstr(fd, ",") != 0 ||
           rt_write_cstr(fd, section->section_name) != 0 ||
           rt_write_cstr(fd, "\n") != 0 ? -1 : 0;
}

static int macho_write_map(const char *path,
                           const MachoLinkImage *link,
                           const char *output_path,
                           const char *entry_symbol,
                           int compact,
                           int gc_sections,
                           uint64_t file_size,
                           char *error_out,
                           size_t error_size) {
    int fd = platform_open_write(path, 0644U);
    size_t object_index;

    if (fd < 0) {
        set_link_error(error_out, error_size, "failed to open Mach-O map file", path);
        return -1;
    }
    if (rt_write_cstr(fd, "newos macho linker map\noutput: ") != 0 || rt_write_cstr(fd, output_path) != 0 ||
        rt_write_cstr(fd, "\nentry: ") != 0 || rt_write_cstr(fd, entry_symbol) != 0 || rt_write_cstr(fd, " ") != 0 ||
        macho_write_hex64(fd, MACHO_BASE_ADDRESS + link->entryoff) != 0 ||
        rt_write_cstr(fd, "\npolicy: ") != 0 || rt_write_cstr(fd, compact ? "compact" : "page-aligned") != 0 ||
        rt_write_cstr(fd, gc_sections ? " gc-sections\n" : " object-gc\n") != 0 ||
        rt_write_cstr(fd, "text/const/cstring/data/bss/file: ") != 0 || rt_write_uint(fd, link->text.size) != 0 ||
        rt_write_cstr(fd, "/") != 0 || rt_write_uint(fd, link->constant.size) != 0 ||
        rt_write_cstr(fd, "/") != 0 || rt_write_uint(fd, link->cstring.size) != 0 ||
        rt_write_cstr(fd, "/") != 0 || rt_write_uint(fd, link->data.size) != 0 ||
        rt_write_cstr(fd, "/") != 0 || rt_write_uint(fd, link->bss.size) != 0 ||
        rt_write_cstr(fd, "/") != 0 || rt_write_uint(fd, file_size) != 0 ||
        rt_write_cstr(fd, "\n\nFinal sections:\n") != 0) {
        (void)platform_close(fd);
        set_link_error(error_out, error_size, "failed to write Mach-O map file", path);
        return -1;
    }
    if (macho_write_map_output_section(fd, &link->text) != 0 ||
        macho_write_map_output_section(fd, &link->constant) != 0 ||
        macho_write_map_output_section(fd, &link->cstring) != 0 ||
        macho_write_map_output_section(fd, &link->data) != 0 ||
        macho_write_map_output_section(fd, &link->bss) != 0 ||
        rt_write_cstr(fd, "\nInput sections:\n") != 0) {
        (void)platform_close(fd);
        set_link_error(error_out, error_size, "failed to write Mach-O map file", path);
        return -1;
    }
    for (object_index = 0; object_index < link->object_count; ++object_index) {
        const MachoInputObject *object = &link->objects[object_index];
        size_t section_index;
        for (section_index = 0; section_index < object->section_count; ++section_index) {
            const MachoInputSection *section = &object->sections[section_index];
            if (section->section_class == MACHO_SECTION_CLASS_NONE || section->size == 0ULL) {
                continue;
            }
            if (rt_write_cstr(fd, "input-section ") != 0 || macho_write_hex64(fd, section->out_addr) != 0 ||
                rt_write_cstr(fd, " ") != 0 || rt_write_uint(fd, section->size) != 0 ||
                rt_write_cstr(fd, " ") != 0 || rt_write_cstr(fd, section->segname) != 0 ||
                rt_write_cstr(fd, ",") != 0 || rt_write_cstr(fd, section->sectname) != 0 ||
                rt_write_cstr(fd, " ") != 0 || rt_write_cstr(fd, object->path) != 0 || rt_write_cstr(fd, "\n") != 0) {
                (void)platform_close(fd);
                set_link_error(error_out, error_size, "failed to write Mach-O map file", path);
                return -1;
            }
        }
    }
    if (rt_write_cstr(fd, "\nSymbols:\n") != 0) {
        (void)platform_close(fd);
        set_link_error(error_out, error_size, "failed to write Mach-O map file", path);
        return -1;
    }
    for (object_index = 0; object_index < link->object_count; ++object_index) {
        const MachoInputObject *object = &link->objects[object_index];
        uint32_t symbol_index;
        for (symbol_index = 0U; symbol_index < object->nsyms; ++symbol_index) {
            const unsigned char *symbol = object->file + object->symoff + (uint64_t)symbol_index * MACHO_NLIST64_SIZE;
            uint32_t strx = read_u32(symbol);
            unsigned char type = symbol[4];
            unsigned char sect = symbol[5];
            uint64_t value = read_u64(symbol + 8U);
            const MachoInputSection *section = macho_section_by_ordinal((MachoInputObject *)object, sect);
            uint64_t runtime_value;
            uint64_t approx_size;
            const char *name;
            if (strx >= object->strsize || (type & MACHO_N_STAB) != 0U || (type & MACHO_N_TYPE_MASK) != MACHO_N_SECT || section == 0 || section->section_class == MACHO_SECTION_CLASS_NONE || value < section->addr || value >= section->addr + section->size) {
                continue;
            }
            name = (const char *)(object->file + object->stroff + strx);
            if (name[0] == '\0') {
                continue;
            }
            if (name[0] == 'L' || rt_strncmp(name, "ltmp", 4U) == 0) {
                continue;
            }
            runtime_value = section->out_addr + (value - section->addr);
            approx_size = macho_symbol_approx_size(object, symbol_index, section, value);
            if (rt_write_cstr(fd, "symbol ") != 0 || macho_write_hex64(fd, runtime_value) != 0 ||
                rt_write_cstr(fd, " ") != 0 || rt_write_uint(fd, approx_size) != 0 ||
                rt_write_cstr(fd, " ") != 0 || rt_write_cstr(fd, section->segname) != 0 ||
                rt_write_cstr(fd, ",") != 0 || rt_write_cstr(fd, section->sectname) != 0 ||
                rt_write_cstr(fd, " ") != 0 || rt_write_cstr(fd, name) != 0 ||
                rt_write_cstr(fd, " ") != 0 || rt_write_cstr(fd, object->path) != 0 || rt_write_cstr(fd, "\n") != 0) {
                (void)platform_close(fd);
                set_link_error(error_out, error_size, "failed to write Mach-O map file", path);
                return -1;
            }
        }
    }
    if (platform_close(fd) != 0) {
        set_link_error(error_out, error_size, "failed to close Mach-O map file", path);
        return -1;
    }
    return 0;
}

static int macho_apply_all_relocations(MachoLinkImage *link, unsigned char *image, size_t image_size, char *error_out, size_t error_size) {
    size_t object_index;
    for (object_index = 0; object_index < link->object_count; ++object_index) {
        if (macho_apply_arm64_relocations(link, object_index, image, image_size, error_out, error_size) != 0) {
            return -1;
        }
    }
    return 0;
}

static int macho_write_executable(MachoLinkImage *link, const char *entry_symbol, const char *output_path, const CompilerLinkerOptions *options, char *error_out, size_t error_size) {
    enum { header_size = 32U, pagezero_command_size = 72U, segment_command_size = 72U, section_command_size = 80U, linkedit_command_size = 72U, dyld_info_command_size = 48U, dylinker_command_size = 32U, build_version_command_size = 32U, main_command_size = 24U, code_signature_command_size = 16U };
    uint32_t text_section_count;
    uint32_t data_section_count;
    uint32_t text_command_size;
    uint32_t data_command_size;
    uint32_t build_version_size;
    uint32_t commands_size;
    uint32_t ncmds;
    uint64_t header_bytes;
    uint64_t cursor;
    uint64_t text_payload_size;
    uint64_t text_file_size;
    uint64_t text_vm_size;
    uint64_t data_fileoff = 0ULL;
    uint64_t data_file_size = 0ULL;
    uint64_t data_vmaddr = 0ULL;
    uint64_t data_vm_size = 0ULL;
    uint64_t linkedit_data_offset;
    uint64_t rebase_info_offset = 0ULL;
    size_t rebase_info_size = 0U;
    uint64_t signature_offset;
    uint64_t code_limit;
    uint64_t linkedit_vmaddr;
    uint64_t linkedit_fileoff;
    uint64_t linkedit_file_size;
    uint64_t linkedit_vm_size;
    unsigned char *rebase_info = 0;
    unsigned char *signature;
    size_t signature_size;
    unsigned char *image;
    uint64_t command_offset;
    uint32_t section_command_index;
    int output_fd;
    int compact = options != 0 && (options->tiny || options->macho_compact);

    macho_init_output_sections(link);
    if (macho_compute_output_section_layout(link, MACHO_SECTION_CLASS_TEXT, error_out, error_size) != 0 ||
        macho_compute_output_section_layout(link, MACHO_SECTION_CLASS_CONST, error_out, error_size) != 0 ||
        macho_compute_output_section_layout(link, MACHO_SECTION_CLASS_CSTRING, error_out, error_size) != 0 ||
        macho_compute_output_section_layout(link, MACHO_SECTION_CLASS_DATA, error_out, error_size) != 0 ||
        macho_compute_output_section_layout(link, MACHO_SECTION_CLASS_BSS, error_out, error_size) != 0) {
        return -1;
    }
    if (link->text.size == 0ULL) {
        set_link_error(error_out, error_size, "Mach-O arm64 output has no __TEXT,__text section", output_path);
        return -1;
    }
    text_section_count = 1U + (link->constant.size != 0ULL ? 1U : 0U) + (link->cstring.size != 0ULL ? 1U : 0U);
    data_section_count = (link->data.size != 0ULL ? 1U : 0U) + (link->bss.size != 0ULL ? 1U : 0U);
    text_command_size = segment_command_size + section_command_size * text_section_count;
    data_command_size = data_section_count != 0U ? segment_command_size + section_command_size * data_section_count : 0U;
    build_version_size = compact ? 24U : build_version_command_size;
    commands_size = pagezero_command_size + text_command_size + data_command_size + linkedit_command_size + dyld_info_command_size + dylinker_command_size + build_version_size + main_command_size + code_signature_command_size;
    ncmds = data_section_count != 0U ? 9U : 8U;
    header_bytes = (uint64_t)header_size + commands_size;
    cursor = macho_align_up(header_bytes, 1ULL << link->text.align);
    macho_apply_output_section_base(link, MACHO_SECTION_CLASS_TEXT, cursor, MACHO_BASE_ADDRESS + cursor);
    cursor += link->text.size;
    if (link->constant.size != 0ULL) {
        cursor = macho_align_up(cursor, 1ULL << link->constant.align);
        macho_apply_output_section_base(link, MACHO_SECTION_CLASS_CONST, cursor, MACHO_BASE_ADDRESS + cursor);
        cursor += link->constant.size;
    }
    if (link->cstring.size != 0ULL) {
        cursor = macho_align_up(cursor, 1ULL << link->cstring.align);
        macho_apply_output_section_base(link, MACHO_SECTION_CLASS_CSTRING, cursor, MACHO_BASE_ADDRESS + cursor);
        cursor += link->cstring.size;
    }
    text_payload_size = cursor;
    text_file_size = macho_align_up(cursor, MACHO_PAGE_SIZE);
    text_vm_size = macho_align_up(cursor, MACHO_PAGE_SIZE);
    if (data_section_count != 0U) {
        uint64_t data_cursor;
        uint64_t data_payload_end;
        data_fileoff = text_file_size;
        data_vmaddr = MACHO_BASE_ADDRESS + text_vm_size;
        data_cursor = data_fileoff;
        if (link->data.size != 0ULL) {
            data_cursor = macho_align_up(data_cursor, 1ULL << link->data.align);
            macho_apply_output_section_base(link, MACHO_SECTION_CLASS_DATA, data_cursor, data_vmaddr + (data_cursor - data_fileoff));
            data_cursor += link->data.size;
            data_file_size = macho_align_up(data_cursor - data_fileoff, MACHO_PAGE_SIZE);
        }
        data_payload_end = data_fileoff + data_file_size;
        if (link->bss.size != 0ULL) {
            uint64_t bss_addr_cursor = data_vmaddr + (data_file_size != 0ULL ? data_file_size : 0ULL);
            bss_addr_cursor = macho_align_up(bss_addr_cursor, 1ULL << link->bss.align);
            macho_apply_output_section_base(link, MACHO_SECTION_CLASS_BSS, 0ULL, bss_addr_cursor);
            data_vm_size = (bss_addr_cursor - data_vmaddr) + link->bss.size;
        } else {
            data_vm_size = data_file_size;
        }
        if (data_vm_size < data_file_size) {
            data_vm_size = data_file_size;
        }
        data_vm_size = macho_align_up(data_vm_size, MACHO_PAGE_SIZE);
        linkedit_data_offset = data_payload_end;
    } else {
        linkedit_data_offset = text_file_size;
    }
    if (macho_collect_rebase_entries(link, data_vmaddr, error_out, error_size) != 0) {
        return -1;
    }
    rebase_info = macho_make_rebase_info(link, &rebase_info_size);
    if (link->rebase_count != 0U && rebase_info == 0) {
        set_link_error(error_out, error_size, "failed to create Mach-O rebase info", output_path);
        return -1;
    }
    if (rebase_info_size != 0U) {
        rebase_info_offset = linkedit_data_offset;
    }
    signature_offset = linkedit_data_offset + (uint64_t)rebase_info_size;
    code_limit = signature_offset;
    linkedit_vmaddr = data_section_count != 0U ? data_vmaddr + data_vm_size : MACHO_BASE_ADDRESS + text_vm_size;
    if (macho_collect_global_symbols(link, entry_symbol, error_out, error_size) != 0) {
        rt_free(rebase_info);
        return -1;
    }
    if (header_bytes > 0xffffffffULL || signature_offset > 0xffffffffULL || link->text.size > 0xffffffffULL || link->constant.size > 0xffffffffULL || link->cstring.size > 0xffffffffULL || link->data.size > 0xffffffffULL || link->bss.size > 0xffffffffULL) {
        set_link_error(error_out, error_size, "Mach-O output is too large", output_path);
        rt_free(rebase_info);
        return -1;
    }
    signature_size = 20U + 88U + MACHO_CODE_DIRECTORY_IDENT_SIZE + (size_t)((code_limit + MACHO_PAGE_SIZE - 1ULL) / MACHO_PAGE_SIZE) * CRYPTO_SHA256_DIGEST_SIZE;
    image = (unsigned char *)rt_malloc((size_t)signature_offset);
    if (image == 0) {
        rt_free(rebase_info);
        set_link_error(error_out, error_size, "out of memory while writing Mach-O output", output_path);
        return -1;
    }
    memset(image, 0, (size_t)signature_offset);

    write_u32(image + 0U, MACHO64_MAGIC);
    write_u32(image + 4U, MACHO_CPU_TYPE_ARM64);
    write_u32(image + 8U, 0U);
    write_u32(image + 12U, MACHO_FILETYPE_EXECUTE);
    write_u32(image + 16U, ncmds);
    write_u32(image + 20U, commands_size);
    write_u32(image + 24U, MACHO_FLAG_NOUNDEFS | MACHO_FLAG_DYLDLINK | MACHO_FLAG_TWOLEVEL | MACHO_FLAG_PIE);

    command_offset = 32ULL;
    macho_write_segment64(image + command_offset, "__PAGEZERO", 0ULL, MACHO_BASE_ADDRESS, 0ULL, 0ULL, 0U, 0U, 0U, 0U);
    command_offset += pagezero_command_size;
    macho_write_segment64(image + command_offset, "__TEXT", MACHO_BASE_ADDRESS, text_vm_size, 0ULL, text_file_size, MACHO_VM_PROT_READ | MACHO_VM_PROT_EXECUTE, MACHO_VM_PROT_READ | MACHO_VM_PROT_EXECUTE, text_section_count, 0U);
    section_command_index = 0U;
    macho_write_section64(image + command_offset + 72U + 80U * section_command_index++, "__text", "__TEXT", link->text.out_addr, link->text.size, (uint32_t)link->text.out_offset, link->text.align, link->text.flags != 0U ? link->text.flags : MACHO_TEXT_SECTION_FLAGS);
    if (link->constant.size != 0ULL) {
        macho_write_section64(image + command_offset + 72U + 80U * section_command_index++, "__const", "__TEXT", link->constant.out_addr, link->constant.size, (uint32_t)link->constant.out_offset, link->constant.align, link->constant.flags);
    }
    if (link->cstring.size != 0ULL) {
        macho_write_section64(image + command_offset + 72U + 80U * section_command_index++, "__cstring", "__TEXT", link->cstring.out_addr, link->cstring.size, (uint32_t)link->cstring.out_offset, link->cstring.align, link->cstring.flags);
    }
    command_offset += text_command_size;
    if (data_section_count != 0U) {
        macho_write_segment64(image + command_offset, "__DATA", data_vmaddr, data_vm_size, data_fileoff, data_file_size, MACHO_VM_PROT_READ | MACHO_VM_PROT_WRITE, MACHO_VM_PROT_READ | MACHO_VM_PROT_WRITE, data_section_count, 0U);
        section_command_index = 0U;
        if (link->data.size != 0ULL) {
            macho_write_section64(image + command_offset + 72U + 80U * section_command_index++, "__data", "__DATA", link->data.out_addr, link->data.size, (uint32_t)link->data.out_offset, link->data.align, link->data.flags);
        }
        if (link->bss.size != 0ULL) {
            macho_write_section64(image + command_offset + 72U + 80U * section_command_index++, "__bss", "__DATA", link->bss.out_addr, link->bss.size, 0U, link->bss.align, link->bss.flags != 0U ? link->bss.flags : MACHO_S_ZEROFILL);
        }
        command_offset += data_command_size;
    }
    linkedit_fileoff = rebase_info_size != 0U ? linkedit_data_offset : signature_offset;
    linkedit_file_size = (signature_offset - linkedit_fileoff) + (uint64_t)signature_size;
    linkedit_vm_size = macho_align_up(linkedit_file_size, MACHO_PAGE_SIZE);
    macho_write_segment64(image + command_offset, "__LINKEDIT", linkedit_vmaddr, linkedit_vm_size, linkedit_fileoff, linkedit_file_size, MACHO_VM_PROT_READ, MACHO_VM_PROT_READ, 0U, 0U);
    command_offset += linkedit_command_size;

    write_u32(image + command_offset + 0U, MACHO_LC_DYLD_INFO_ONLY);
    write_u32(image + command_offset + 4U, dyld_info_command_size);
    write_u32(image + command_offset + 8U, (uint32_t)rebase_info_offset);
    write_u32(image + command_offset + 12U, (uint32_t)rebase_info_size);
    write_u32(image + command_offset + 16U, 0U);
    write_u32(image + command_offset + 20U, 0U);
    write_u32(image + command_offset + 24U, 0U);
    write_u32(image + command_offset + 28U, 0U);
    write_u32(image + command_offset + 32U, 0U);
    write_u32(image + command_offset + 36U, 0U);
    write_u32(image + command_offset + 40U, 0U);
    write_u32(image + command_offset + 44U, 0U);
    command_offset += dyld_info_command_size;

    write_u32(image + command_offset + 0U, MACHO_LC_LOAD_DYLINKER);
    write_u32(image + command_offset + 4U, dylinker_command_size);
    write_u32(image + command_offset + 8U, 12U);
    memcpy(image + command_offset + 12U, "/usr/lib/dyld", 14U);
    command_offset += dylinker_command_size;

    write_u32(image + command_offset + 0U, MACHO_LC_BUILD_VERSION);
    write_u32(image + command_offset + 4U, build_version_size);
    write_u32(image + command_offset + 8U, 1U);
    write_u32(image + command_offset + 12U, 0x000b0000U);
    write_u32(image + command_offset + 16U, 0x000b0000U);
    write_u32(image + command_offset + 20U, compact ? 0U : 1U);
    if (!compact) {
        write_u32(image + command_offset + 24U, 3U);
    }
    command_offset += build_version_size;

    write_u32(image + command_offset + 0U, MACHO_LC_MAIN);
    write_u32(image + command_offset + 4U, main_command_size);
    write_u64(image + command_offset + 8U, link->entryoff);
    command_offset += main_command_size;

    write_u32(image + command_offset + 0U, MACHO_LC_CODE_SIGNATURE);
    write_u32(image + command_offset + 4U, code_signature_command_size);
    write_u32(image + command_offset + 8U, (uint32_t)signature_offset);
    write_u32(image + command_offset + 12U, (uint32_t)signature_size);

    macho_copy_input_sections(link, image, MACHO_SECTION_CLASS_TEXT);
    macho_copy_input_sections(link, image, MACHO_SECTION_CLASS_CONST);
    macho_copy_input_sections(link, image, MACHO_SECTION_CLASS_CSTRING);
    macho_copy_input_sections(link, image, MACHO_SECTION_CLASS_DATA);
    if (macho_apply_all_relocations(link, image, (size_t)signature_offset, error_out, error_size) != 0) {
        rt_free(rebase_info);
        rt_free(image);
        return -1;
    }
    if (rebase_info_size != 0U) {
        memcpy(image + rebase_info_offset, rebase_info, rebase_info_size);
    }
    rt_free(rebase_info);

    signature = macho_make_code_signature(image, (size_t)signature_offset, code_limit, &signature_size);
    if (signature == 0) {
        rt_free(image);
        set_link_error(error_out, error_size, "failed to create Mach-O ad-hoc code signature", output_path);
        return -1;
    }
    output_fd = platform_open_write(output_path, 0755U);
    if (output_fd < 0) {
        rt_free(image);
        rt_free(signature);
        set_link_error(error_out, error_size, "failed to create Mach-O output", output_path);
        return -1;
    }
    if (rt_write_all(output_fd, image, (size_t)signature_offset) != 0) {
        platform_close(output_fd);
        rt_free(image);
        rt_free(signature);
        set_link_error(error_out, error_size, "failed to write Mach-O output", output_path);
        return -1;
    }
    if (rt_write_all(output_fd, signature, signature_size) != 0) {
        platform_close(output_fd);
        rt_free(image);
        rt_free(signature);
        set_link_error(error_out, error_size, "failed to write Mach-O code signature", output_path);
        return -1;
    }
    rt_free(image);
    rt_free(signature);
    if (platform_close(output_fd) != 0) {
        set_link_error(error_out, error_size, "failed to close Mach-O output", output_path);
        return -1;
    }
    if (options != 0 && options->map_path != 0 && options->map_path[0] != '\0') {
        if (macho_write_map(options->map_path, link, output_path, entry_symbol, compact, options->gc_sections, signature_offset + (uint64_t)signature_size, error_out, error_size) != 0) {
            return -1;
        }
    }
    if (options != 0 && options->stats) {
        if (macho_write_stats(link, header_bytes, text_payload_size, text_file_size, text_vm_size, data_file_size, data_vm_size, signature_offset, code_limit, (uint64_t)signature_size, compact) != 0) {
            set_link_error(error_out, error_size, "failed to write Mach-O linker stats", output_path);
            return -1;
        }
    }
    return 0;
}

static void macho_free_link_image(MachoLinkImage *link) {
    size_t object_index;
    if (link->objects == 0) {
        return;
    }
    for (object_index = 0; object_index < link->object_count; ++object_index) {
        rt_free(link->objects[object_index].file);
        link->objects[object_index].file = 0;
    }
    rt_free(link->objects);
    link->objects = 0;
    link->object_count = 0U;
}

static unsigned long long macho_parse_ar_decimal_field(const unsigned char *field, size_t field_size) {
    unsigned long long value = 0U;
    size_t i = 0U;
    while (i < field_size && field[i] == ' ') {
        i += 1U;
    }
    while (i < field_size && field[i] >= '0' && field[i] <= '9') {
        value = value * 10U + (unsigned long long)(field[i] - '0');
        i += 1U;
    }
    return value;
}

static void macho_copy_ar_trimmed_name(char *buffer, size_t buffer_size, const unsigned char *field) {
    size_t i = 0U;
    size_t last_non_space = 0U;
    while (i < 16U && i + 1U < buffer_size) {
        buffer[i] = (char)field[i];
        if (field[i] != ' ') {
            last_non_space = i + 1U;
        }
        i += 1U;
    }
    buffer[last_non_space] = '\0';
}

static void macho_copy_ar_string_table_name(char *buffer, size_t buffer_size, const unsigned char *strings, size_t strings_size, size_t offset) {
    size_t i = 0U;
    if (strings == 0 || offset >= strings_size) {
        buffer[0] = '\0';
        return;
    }
    while (offset + i < strings_size && i + 1U < buffer_size) {
        char ch = (char)strings[offset + i];
        if (ch == '/' || ch == '\n') {
            break;
        }
        buffer[i] = ch;
        i += 1U;
    }
    buffer[i] = '\0';
}

static int macho_add_loaded_object(MachoLinkImage *link, const char *path, unsigned char *file, size_t size,
                                   const char *entry_symbol, char *error_out, size_t error_size) {
    if (link->object_count >= LINKER_MAX_OBJECTS) {
        rt_free(file);
        set_link_error(error_out, error_size, "too many objects for Mach-O arm64 linker", path);
        return -1;
    }
    if (macho_parse_loaded_object(&link->objects[link->object_count], path, file, size, entry_symbol, error_out, error_size) != 0) {
        rt_free(file);
        link->objects[link->object_count].file = 0;
        return -1;
    }
    link->object_count += 1U;
    return 0;
}

static int macho_add_input_object(MachoLinkImage *link, const char *path, const char *entry_symbol, char *error_out, size_t error_size) {
    if (link->object_count >= LINKER_MAX_OBJECTS) {
        set_link_error(error_out, error_size, "too many objects for Mach-O arm64 linker", path);
        return -1;
    }
    if (macho_parse_input_object(&link->objects[link->object_count], path, entry_symbol, error_out, error_size) != 0) {
        if (link->objects[link->object_count].file != 0) {
            rt_free(link->objects[link->object_count].file);
            link->objects[link->object_count].file = 0;
        }
        return -1;
    }
    link->object_count += 1U;
    return 0;
}

static int macho_load_archive(MachoLinkImage *link, const char *path, const char *entry_symbol, char *error_out, size_t error_size) {
    unsigned char *archive;
    const unsigned char *string_table = 0;
    size_t archive_size;
    size_t string_table_size = 0U;
    size_t offset = 8U;
    size_t loaded = 0U;

    if (read_file_alloc(path, LINKER_MAX_ARCHIVE_SIZE, &archive, &archive_size, error_out, error_size) != 0) {
        return -1;
    }
    if (archive_size < 8U || archive[0] != '!' || archive[1] != '<' || archive[2] != 'a' || archive[3] != 'r' || archive[4] != 'c' || archive[5] != 'h' || archive[6] != '>' || archive[7] != '\n') {
        rt_free(archive);
        set_link_error(error_out, error_size, "unsupported archive format", path);
        return -1;
    }
    while (offset + LINKER_AR_HEADER_SIZE <= archive_size) {
        const unsigned char *header = archive + offset;
        size_t payload_offset = offset + LINKER_AR_HEADER_SIZE;
        unsigned long long payload_size_value = macho_parse_ar_decimal_field(header + 48, 10U);
        size_t payload_size = (size_t)payload_size_value;
        size_t data_offset = payload_offset;
        size_t data_size = payload_size;
        size_t next_offset;
        char member_name[COMPILER_PATH_CAPACITY];
        char object_name[COMPILER_PATH_CAPACITY];
        unsigned char *member_file;

        if (header[58] != '`' || header[59] != '\n' || payload_size_value > (unsigned long long)(archive_size - payload_offset)) {
            rt_free(archive);
            set_link_error(error_out, error_size, "invalid archive member", path);
            return -1;
        }
        next_offset = payload_offset + payload_size + ((payload_size & 1U) != 0U ? 1U : 0U);
        if (next_offset > archive_size) {
            rt_free(archive);
            set_link_error(error_out, error_size, "invalid archive member size", path);
            return -1;
        }
        macho_copy_ar_trimmed_name(member_name, sizeof(member_name), header);
        if (rt_strcmp(member_name, "//") == 0) {
            string_table = archive + payload_offset;
            string_table_size = payload_size;
            offset = next_offset;
            continue;
        }
        if (rt_strcmp(member_name, "/") == 0 || rt_strcmp(member_name, "__.SYMDEF") == 0 || rt_strcmp(member_name, "__.SYMDEF SORTED") == 0) {
            offset = next_offset;
            continue;
        }
        if (member_name[0] == '/' && member_name[1] >= '0' && member_name[1] <= '9') {
            size_t string_offset = 0U;
            size_t i = 1U;
            while (member_name[i] >= '0' && member_name[i] <= '9') {
                string_offset = string_offset * 10U + (size_t)(member_name[i] - '0');
                i += 1U;
            }
            macho_copy_ar_string_table_name(member_name, sizeof(member_name), string_table, string_table_size, string_offset);
        } else if (member_name[0] == '#' && member_name[1] == '1' && member_name[2] == '/') {
            unsigned long long name_length_value = macho_parse_ar_decimal_field((const unsigned char *)member_name + 3, rt_strlen(member_name + 3));
            size_t name_length = (size_t)name_length_value;
            size_t copy_length;
            if (name_length_value > payload_size) {
                rt_free(archive);
                set_link_error(error_out, error_size, "invalid archive member name", path);
                return -1;
            }
            copy_length = name_length + 1U < sizeof(member_name) ? name_length : sizeof(member_name) - 1U;
            memcpy(member_name, archive + payload_offset, copy_length);
            member_name[copy_length] = '\0';
            data_offset = payload_offset + name_length;
            data_size = payload_size - name_length;
        }
        if (!ends_with_text(member_name, ".o") && !(data_size >= MACHO_HEADER64_SIZE && read_u32(archive + data_offset) == MACHO64_MAGIC)) {
            offset = next_offset;
            continue;
        }
        if (data_size > LINKER_MAX_OBJECT_SIZE) {
            rt_free(archive);
            set_link_error(error_out, error_size, "archive member exceeds native linker capacity", member_name);
            return -1;
        }
        member_file = (unsigned char *)rt_malloc(data_size);
        if (member_file == 0) {
            rt_free(archive);
            set_link_error(error_out, error_size, "failed to allocate archive member", member_name);
            return -1;
        }
        memcpy(member_file, archive + data_offset, data_size);
        rt_copy_string(object_name, sizeof(object_name), path);
        if (rt_strlen(object_name) + rt_strlen(member_name) + 3U < sizeof(object_name)) {
            size_t used = rt_strlen(object_name);
            object_name[used++] = '(';
            rt_copy_string(object_name + used, sizeof(object_name) - used, member_name);
            used = rt_strlen(object_name);
            object_name[used++] = ')';
            object_name[used] = '\0';
        }
        if (macho_add_loaded_object(link, object_name, member_file, data_size, entry_symbol, error_out, error_size) != 0) {
            rt_free(archive);
            return -1;
        }
        loaded += 1U;
        offset = next_offset;
    }
    rt_free(archive);
    if (loaded == 0U) {
        set_link_error(error_out, error_size, "archive contains no supported Mach-O arm64 objects", path);
        return -1;
    }
    return 0;
}

int compiler_link_macho64_aarch64_static_options(const char *const *object_paths,
                                                 size_t object_count,
                                                 const char *output_path,
                                                 const CompilerLinkerOptions *options,
                                                 char *error_out,
                                                 size_t error_size) {
    MachoLinkImage link;
    const char *entry_symbol = "_start";
    const char *lto_cc = 0;
    const char *lto_link_paths[LINKER_MAX_OBJECTS + 1U];
    LinkLtoKind input_lto_kinds[LINKER_MAX_OBJECTS];
    char lto_prelink_path[COMPILER_PATH_CAPACITY];
    int did_lto_prelink = 0;
    size_t input_index;
    size_t original_object_count;
    int result;

    memset(&link, 0, sizeof(link));
    if (error_out != 0 && error_size > 0U) {
        error_out[0] = '\0';
    }
    if (options != 0 && options->entry_symbol != 0 && options->entry_symbol[0] != '\0') {
        entry_symbol = options->entry_symbol;
    }
    if (options != 0) {
        lto_cc = options->lto_cc;
    }
    if (object_count == 0U) {
        set_link_error(error_out, error_size, "no Mach-O arm64 input objects", output_path);
        return -1;
    }
    if (object_count > LINKER_MAX_OBJECTS) {
        set_link_error(error_out, error_size, "too many objects for Mach-O arm64 linker", output_path);
        return -1;
    }
    original_object_count = object_count;
    for (input_index = 0; input_index < LINKER_MAX_OBJECTS; ++input_index) {
        input_lto_kinds[input_index] = LINK_LTO_NONE;
    }
    for (input_index = 0; input_index < object_count; ++input_index) {
        unsigned char *probe = 0;
        size_t probe_size = 0U;
        LinkLtoKind lto_kind;
        if (ends_with_text(object_paths[input_index], ".a")) {
            continue;
        }
        if (read_file_alloc(object_paths[input_index], LINKER_MAX_OBJECT_SIZE, &probe, &probe_size, error_out, error_size) != 0) {
            return -1;
        }
        lto_kind = detect_lto_ir_kind(probe, probe_size);
        rt_free(probe);
        input_lto_kinds[input_index] = lto_kind;
        if (lto_kind != LINK_LTO_NONE) {
            size_t out_len;
            size_t link_count = 0U;
            size_t link_index;
            if (lto_kind != LINK_LTO_LLVM) {
                set_link_error(error_out, error_size, "Mach-O arm64 LTO requires LLVM/Clang bitcode", object_paths[input_index]);
                return -1;
            }
            for (link_index = input_index + 1U; link_index < object_count; ++link_index) {
                unsigned char *later_probe = 0;
                size_t later_probe_size = 0U;
                if (ends_with_text(object_paths[link_index], ".a")) {
                    input_lto_kinds[link_index] = LINK_LTO_NONE;
                    continue;
                }
                if (read_file_alloc(object_paths[link_index], LINKER_MAX_OBJECT_SIZE, &later_probe, &later_probe_size, error_out, error_size) != 0) {
                    return -1;
                }
                input_lto_kinds[link_index] = detect_lto_ir_kind(later_probe, later_probe_size);
                rt_free(later_probe);
                if (input_lto_kinds[link_index] != LINK_LTO_NONE && input_lto_kinds[link_index] != LINK_LTO_LLVM) {
                    set_link_error(error_out, error_size, "Mach-O arm64 LTO requires LLVM/Clang bitcode", object_paths[link_index]);
                    return -1;
                }
            }
            if (lto_cc == 0 || lto_cc[0] == '\0') {
                set_link_error(error_out, error_size, "Mach-O arm64 LLVM/Clang LTO input; add --lto-cc=clang", object_paths[input_index]);
                return -1;
            }
            out_len = rt_strlen(output_path);
            if (out_len + 16U >= sizeof(lto_prelink_path)) {
                set_link_error(error_out, error_size, "LTO prelink output path is too long", output_path);
                return -1;
            }
            rt_copy_string(lto_prelink_path, sizeof(lto_prelink_path), output_path);
            rt_copy_string(lto_prelink_path + out_len, sizeof(lto_prelink_path) - out_len, ".lto-prelink.o");
            if (run_clang_lto_prelink_macho64_aarch64(object_paths, object_count, entry_symbol, lto_cc, lto_prelink_path, options != 0 && options->gc_sections, error_out, error_size) != 0) {
                return -1;
            }
            for (link_index = 0; link_index < original_object_count; ++link_index) {
                if (input_lto_kinds[link_index] == LINK_LTO_NONE) {
                    lto_link_paths[link_count++] = object_paths[link_index];
                }
            }
            lto_link_paths[link_count++] = lto_prelink_path;
            object_paths = lto_link_paths;
            object_count = link_count;
            did_lto_prelink = 1;
            break;
        }
    }
    link.objects = (MachoInputObject *)rt_malloc(LINKER_MAX_OBJECTS * sizeof(MachoInputObject));
    if (link.objects == 0) {
        set_link_error(error_out, error_size, "failed to allocate Mach-O link state", output_path);
        return -1;
    }
    memset(link.objects, 0, LINKER_MAX_OBJECTS * sizeof(MachoInputObject));
    for (input_index = 0; input_index < object_count; ++input_index) {
        if (ends_with_text(object_paths[input_index], ".a")) {
            if (macho_load_archive(&link, object_paths[input_index], entry_symbol, error_out, error_size) != 0) {
                macho_free_link_image(&link);
                if (did_lto_prelink) {
                    platform_remove_file(lto_prelink_path);
                }
                return -1;
            }
        } else if (macho_add_input_object(&link, object_paths[input_index], entry_symbol, error_out, error_size) != 0) {
            macho_free_link_image(&link);
            if (did_lto_prelink) {
                platform_remove_file(lto_prelink_path);
            }
            return -1;
        }
    }
    if (link.object_count == 0U) {
        macho_free_link_image(&link);
        if (did_lto_prelink) {
            platform_remove_file(lto_prelink_path);
        }
        set_link_error(error_out, error_size, "no Mach-O arm64 input objects", output_path);
        return -1;
    }
    result = macho_write_executable(&link, entry_symbol, output_path, options, error_out, error_size);
    macho_free_link_image(&link);
    if (did_lto_prelink) {
        platform_remove_file(lto_prelink_path);
    }
    return result;
}