#include "linker_internal.h"

#include "crypto/sha256.h"

#define MACHO64_MAGIC               0xfeedfacfU
#define MACHO_CPU_TYPE_ARM64        0x0100000cU
#define MACHO_FILETYPE_OBJECT       1U
#define MACHO_FILETYPE_EXECUTE      2U
#define MACHO_LC_SEGMENT_64         0x19U
#define MACHO_LC_SYMTAB             0x2U
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
#define MACHO_N_TYPE_MASK           0x0eU
#define MACHO_N_SECT                0x0eU
#define MACHO_ARM64_RELOC_UNSIGNED  0U
#define MACHO_ARM64_RELOC_BRANCH26  2U
#define MACHO_ARM64_RELOC_PAGE21    3U
#define MACHO_ARM64_RELOC_PAGEOFF12 4U
#define MACHO_TEXT_SECTION_FLAGS    0x80000400U
#define MACHO_CODE_DIRECTORY_IDENT  "newlinker"
#define MACHO_CODE_DIRECTORY_IDENT_SIZE 10U

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
    uint64_t out_offset;
    uint64_t out_addr;
} MachoInputSection;

typedef struct {
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
    MachoInputSection *cstring_section;
    MachoInputSection *data_section;
    uint64_t entry_value;
    int entry_found;
} MachoInputObject;

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

    if (code_limit == 0ULL || (code_limit % MACHO_PAGE_SIZE) != 0ULL || code_limit > 0xffffffffULL || image_size > code_limit) {
        return 0;
    }
    code_slots = (uint32_t)(code_limit / MACHO_PAGE_SIZE);
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
        macho_page_hasher_update_zeroes(&hasher, code_limit - (uint64_t)image_size) != 0 ||
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
    if (macho_name_equals(section->segname, "__TEXT") && macho_name_equals(section->sectname, "__text")) {
        return 1;
    }
    if (macho_name_equals(section->segname, "__TEXT") && macho_name_equals(section->sectname, "__cstring")) {
        return 1;
    }
    if (macho_name_equals(section->segname, "__DATA") && macho_name_equals(section->sectname, "__data")) {
        return 1;
    }
    return section->size == 0ULL;
}

static int macho_section_has_file_bytes(const MachoInputSection *section) {
    return section->size != 0ULL && section->offset != 0U;
}

static int macho_symbol_value(MachoInputObject *object, uint32_t symbol_index, int external, uint64_t *value_out, char *error_out, size_t error_size) {
    const unsigned char *symbol;
    unsigned char type;
    unsigned char sect;
    uint64_t value;
    MachoInputSection *section;

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
            set_link_error(error_out, error_size, "undefined Mach-O arm64 symbol", name);
            return -1;
        }
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

    section = macho_section_by_ordinal(object, symbol_index);
    if (section == 0) {
        set_link_error(error_out, error_size, "invalid Mach-O section relocation target", "");
        return -1;
    }
    *value_out = section->out_addr;
    return 0;
}

static int macho_apply_arm64_relocations(MachoInputObject *object, unsigned char *image, size_t image_size, char *error_out, size_t error_size) {
    size_t section_index;
    for (section_index = 0; section_index < object->section_count; ++section_index) {
        MachoInputSection *section = &object->sections[section_index];
        uint32_t reloc_index;

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
            if (macho_symbol_value(object, symbol_index, external, &target, error_out, error_size) != 0) {
                return -1;
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
                    set_link_error(error_out, error_size, "only ADD-immediate Mach-O arm64 pageoff relocations are supported", section->sectname);
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

static int macho_parse_input_object(MachoInputObject *object, const char *path, const char *entry_symbol,
                                    char *error_out, size_t error_size) {
    uint32_t cputype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint64_t load_offset;
    uint32_t command_index;
    uint32_t section_ordinal = 1U;
    size_t i;

    memset(object, 0, sizeof(*object));
    if (read_file_alloc(path, LINKER_MAX_OBJECT_SIZE, &object->file, &object->size, error_out, error_size) != 0) {
        return -1;
    }
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
                object->relocation_count += section->nreloc;
                if (section->offset != 0U && !range_valid(section->offset, section->size, object->size)) {
                    set_link_error(error_out, error_size, "Mach-O section extends past end of file", path);
                    return -1;
                }
                if (section->nreloc != 0U && !range_valid(section->reloff, (uint64_t)section->nreloc * MACHO_RELOC_SIZE, object->size)) {
                    set_link_error(error_out, error_size, "Mach-O relocation table extends past end of file", path);
                    return -1;
                }
                if (macho_name_equals(section->segname, "__TEXT") && macho_name_equals(section->sectname, "__text")) {
                    object->text_section = section;
                } else if (macho_name_equals(section->segname, "__TEXT") && macho_name_equals(section->sectname, "__cstring")) {
                    object->cstring_section = section;
                } else if (macho_name_equals(section->segname, "__DATA") && macho_name_equals(section->sectname, "__data")) {
                    object->data_section = section;
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
    if (object->text_section == 0 || object->symoff == 0U || object->stroff == 0U) {
        set_link_error(error_out, error_size, "Mach-O arm64 object is missing __TEXT,__text or symbols", path);
        return -1;
    }
    if (object->text_section->offset == 0U || !range_valid(object->text_section->offset, object->text_section->size, object->size)) {
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
        if ((type & MACHO_N_TYPE_MASK) == MACHO_N_SECT && sect == object->text_section->ordinal && rt_strcmp(name, entry_symbol) == 0) {
            object->entry_value = value;
            object->entry_found = 1;
            break;
        }
    }
    if (!object->entry_found || object->entry_value < object->text_section->addr || object->entry_value - object->text_section->addr >= object->text_section->size) {
        set_link_error(error_out, error_size, "Mach-O arm64 entry symbol was not found in __TEXT,__text", entry_symbol);
        return -1;
    }
    return 0;
}

static int macho_write_executable(MachoInputObject *object, const char *output_path, char *error_out, size_t error_size) {
    enum { header_size = 32U, pagezero_command_size = 72U, segment_command_size = 72U, section_command_size = 80U, linkedit_command_size = 72U, dylinker_command_size = 32U, build_version_command_size = 32U, main_command_size = 24U, code_signature_command_size = 16U };
    MachoInputSection *text_section = object->text_section;
    MachoInputSection *cstring_section = object->cstring_section;
    MachoInputSection *data_section = object->data_section;
    uint32_t text_section_count = cstring_section != 0 && cstring_section->size != 0ULL ? 2U : 1U;
    uint32_t data_section_count = data_section != 0 && data_section->size != 0ULL ? 1U : 0U;
    uint32_t text_command_size = segment_command_size + section_command_size * text_section_count;
    uint32_t data_command_size = data_section_count != 0U ? segment_command_size + section_command_size * data_section_count : 0U;
    uint32_t commands_size = pagezero_command_size + text_command_size + data_command_size + linkedit_command_size + dylinker_command_size + build_version_command_size + main_command_size + code_signature_command_size;
    uint32_t ncmds = data_section_count != 0U ? 8U : 7U;
    uint64_t header_bytes = (uint64_t)header_size + commands_size;
    uint64_t cursor = header_bytes;
    uint64_t text_file_size;
    uint64_t data_fileoff = 0ULL;
    uint64_t data_file_size = 0ULL;
    uint64_t signature_offset;
    uint64_t entryoff;
    unsigned char *signature;
    size_t signature_size;
    unsigned char *image;
    uint64_t command_offset;
    int output_fd;

    if (header_bytes > 0xffffffffULL || text_section->size > 0xffffffffULL || (cstring_section != 0 && cstring_section->size > 0xffffffffULL) || (data_section != 0 && data_section->size > 0xffffffffULL)) {
        set_link_error(error_out, error_size, "Mach-O text section is too large", output_path);
        return -1;
    }

    if (text_section->align >= 32U || (cstring_section != 0 && cstring_section->align >= 32U) || (data_section != 0 && data_section->align >= 32U)) {
        set_link_error(error_out, error_size, "unsupported Mach-O section alignment", output_path);
        return -1;
    }

    cursor = macho_align_up(cursor, 1ULL << text_section->align);
    text_section->out_offset = cursor;
    text_section->out_addr = MACHO_BASE_ADDRESS + cursor;
    cursor += text_section->size;
    if (cstring_section != 0 && cstring_section->size != 0ULL) {
        cursor = macho_align_up(cursor, 1ULL << cstring_section->align);
        cstring_section->out_offset = cursor;
        cstring_section->out_addr = MACHO_BASE_ADDRESS + cursor;
        cursor += cstring_section->size;
    }
    text_file_size = macho_align_up(cursor, MACHO_PAGE_SIZE);
    if (data_section_count != 0U) {
        data_fileoff = text_file_size;
        data_section->out_offset = data_fileoff;
        data_section->out_addr = MACHO_BASE_ADDRESS + data_fileoff;
        data_file_size = macho_align_up(data_section->size, MACHO_PAGE_SIZE);
        signature_offset = data_fileoff + data_file_size;
    } else {
        signature_offset = text_file_size;
    }
    if (signature_offset > 0xffffffffULL) {
        set_link_error(error_out, error_size, "Mach-O output is too large", output_path);
        return -1;
    }
    signature_size = 20U + 88U + MACHO_CODE_DIRECTORY_IDENT_SIZE + (size_t)(signature_offset / MACHO_PAGE_SIZE) * CRYPTO_SHA256_DIGEST_SIZE;
    image = (unsigned char *)rt_malloc((size_t)signature_offset);
    if (image == 0) {
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
    macho_write_segment64(image + command_offset, "__TEXT", MACHO_BASE_ADDRESS, text_file_size, 0ULL, text_file_size, MACHO_VM_PROT_READ | MACHO_VM_PROT_EXECUTE, MACHO_VM_PROT_READ | MACHO_VM_PROT_EXECUTE, text_section_count, 0U);
    macho_write_section64(image + command_offset + 72U, "__text", "__TEXT", text_section->out_addr, text_section->size, (uint32_t)text_section->out_offset, text_section->align, text_section->flags != 0U ? text_section->flags : MACHO_TEXT_SECTION_FLAGS);
    if (cstring_section != 0 && cstring_section->size != 0ULL) {
        macho_write_section64(image + command_offset + 152U, "__cstring", "__TEXT", cstring_section->out_addr, cstring_section->size, (uint32_t)cstring_section->out_offset, cstring_section->align, cstring_section->flags);
    }
    command_offset += text_command_size;
    if (data_section_count != 0U) {
        macho_write_segment64(image + command_offset, "__DATA", MACHO_BASE_ADDRESS + data_fileoff, data_file_size, data_fileoff, data_file_size, MACHO_VM_PROT_READ | MACHO_VM_PROT_WRITE, MACHO_VM_PROT_READ | MACHO_VM_PROT_WRITE, data_section_count, 0U);
        macho_write_section64(image + command_offset + 72U, "__data", "__DATA", data_section->out_addr, data_section->size, (uint32_t)data_section->out_offset, data_section->align, data_section->flags);
        command_offset += data_command_size;
    }
    macho_write_segment64(image + command_offset, "__LINKEDIT", MACHO_BASE_ADDRESS + signature_offset, MACHO_PAGE_SIZE, signature_offset, (uint64_t)signature_size, MACHO_VM_PROT_READ, MACHO_VM_PROT_READ, 0U, 0U);
    command_offset += linkedit_command_size;

    write_u32(image + command_offset + 0U, MACHO_LC_LOAD_DYLINKER);
    write_u32(image + command_offset + 4U, dylinker_command_size);
    write_u32(image + command_offset + 8U, 12U);
    memcpy(image + command_offset + 12U, "/usr/lib/dyld", 14U);
    command_offset += dylinker_command_size;

    write_u32(image + command_offset + 0U, MACHO_LC_BUILD_VERSION);
    write_u32(image + command_offset + 4U, build_version_command_size);
    write_u32(image + command_offset + 8U, 1U);
    write_u32(image + command_offset + 12U, 0x000b0000U);
    write_u32(image + command_offset + 16U, 0x000b0000U);
    write_u32(image + command_offset + 20U, 1U);
    write_u32(image + command_offset + 24U, 3U);
    command_offset += build_version_command_size;

    entryoff = text_section->out_offset + (object->entry_value - text_section->addr);
    write_u32(image + command_offset + 0U, MACHO_LC_MAIN);
    write_u32(image + command_offset + 4U, main_command_size);
    write_u64(image + command_offset + 8U, entryoff);
    command_offset += main_command_size;

    write_u32(image + command_offset + 0U, MACHO_LC_CODE_SIGNATURE);
    write_u32(image + command_offset + 4U, code_signature_command_size);
    write_u32(image + command_offset + 8U, (uint32_t)signature_offset);
    write_u32(image + command_offset + 12U, (uint32_t)signature_size);

    memcpy(image + text_section->out_offset, object->file + text_section->offset, (size_t)text_section->size);
    if (cstring_section != 0 && cstring_section->size != 0ULL) {
        memcpy(image + cstring_section->out_offset, object->file + cstring_section->offset, (size_t)cstring_section->size);
    }
    if (data_section_count != 0U) {
        memcpy(image + data_section->out_offset, object->file + data_section->offset, (size_t)data_section->size);
    }
    if (macho_apply_arm64_relocations(object, image, (size_t)signature_offset, error_out, error_size) != 0) {
        rt_free(image);
        return -1;
    }

    signature = macho_make_code_signature(image, (size_t)signature_offset, signature_offset, &signature_size);
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
    return 0;
}

int compiler_link_macho64_aarch64_static_options(const char *const *object_paths,
                                                 size_t object_count,
                                                 const char *output_path,
                                                 const CompilerLinkerOptions *options,
                                                 char *error_out,
                                                 size_t error_size) {
    MachoInputObject object;
    const char *entry_symbol = "_start";
    int result;

    if (error_out != 0 && error_size > 0U) {
        error_out[0] = '\0';
    }
    if (options != 0 && options->entry_symbol != 0 && options->entry_symbol[0] != '\0') {
        entry_symbol = options->entry_symbol;
    }
    if (object_count != 1U) {
        set_link_error(error_out, error_size, "Mach-O arm64 linker currently accepts exactly one relocatable object", output_path);
        return -1;
    }
    if (ends_with_text(object_paths[0], ".a")) {
        set_link_error(error_out, error_size, "Mach-O arm64 archive linking is not implemented yet", object_paths[0]);
        return -1;
    }
    if (macho_parse_input_object(&object, object_paths[0], entry_symbol, error_out, error_size) != 0) {
        if (object.file != 0) {
            rt_free(object.file);
        }
        return -1;
    }
    result = macho_write_executable(&object, output_path, error_out, error_size);
    rt_free(object.file);
    return result;
}