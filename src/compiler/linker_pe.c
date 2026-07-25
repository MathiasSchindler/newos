#include "linker_internal.h"

#define PE_COFF_MACHINE_ARM64 0xaa64U
#define PE_COFF_HEADER_SIZE 20U
#define PE_COFF_SECTION_SIZE 40U
#define PE_COFF_SYMBOL_SIZE 18U
#define PE_COFF_RELOCATION_SIZE 10U
#define PE_MAX_INPUT_OBJECTS 64U
#define PE_MAX_IMPORT_DEFINITIONS 512U
#define PE_MAX_IMPORTS 256U
#define PE_MAX_IMPORT_GROUPS 16U
#define PE_IMPORT_NAME_CAPACITY 128U
#define PE_IMAGE_BASE 0x140000000ULL
#define PE_SECTION_ALIGNMENT 0x1000U
#define PE_FILE_ALIGNMENT 0x200U
#define PE_HEADERS_SIZE 0x200U
#define PE_SIGNATURE_OFFSET 0x80U
#define PE_OPTIONAL_HEADER_SIZE 0xf0U
#define PE_TEXT_RVA 0x1000U
#define PE_SCN_CNT_CODE 0x00000020U
#define PE_SCN_CNT_INITIALIZED_DATA 0x00000040U
#define PE_SCN_CNT_UNINITIALIZED_DATA 0x00000080U
#define PE_SCN_LNK_REMOVE 0x00000800U
#define PE_SCN_LNK_COMDAT 0x00001000U
#define PE_SCN_MEM_EXECUTE 0x20000000U
#define PE_SCN_MEM_READ 0x40000000U
#define PE_SCN_MEM_WRITE 0x80000000U
#define PE_SYM_CLASS_EXTERNAL 2U
#define PE_SYM_CLASS_STATIC 3U
#define PE_SYM_CLASS_WEAK_EXTERNAL 105U
#define PE_SYM_UNDEFINED 0
#define PE_SYM_ABSOLUTE (-1)
#define PE_WEAK_SEARCH_ALIAS 3U
#define PE_COMDAT_SELECT_ASSOCIATIVE 5U
#define PE_REL_ARM64_ADDR32 0x0001U
#define PE_REL_ARM64_ADDR32NB 0x0002U
#define PE_REL_ARM64_BRANCH26 0x0003U
#define PE_REL_ARM64_PAGEBASE_REL21 0x0004U
#define PE_REL_ARM64_REL21 0x0005U
#define PE_REL_ARM64_PAGEOFFSET_12A 0x0006U
#define PE_REL_ARM64_PAGEOFFSET_12L 0x0007U
#define PE_REL_ARM64_ADDR64 0x000eU
#define PE_REL_ARM64_REL32 0x0011U

typedef enum {
    PE_SECTION_NONE = 0,
    PE_SECTION_TEXT,
    PE_SECTION_RDATA,
    PE_SECTION_DATA,
    PE_SECTION_BSS
} PeSectionClass;

typedef struct {
    char name[COMPILER_PATH_CAPACITY];
    uint32_t ordinal;
    uint32_t raw_size;
    uint32_t raw_offset;
    uint32_t reloc_offset;
    uint16_t reloc_count;
    uint32_t characteristics;
    uint64_t alignment;
    uint64_t class_offset;
    uint64_t output_rva;
    uint64_t output_file_offset;
    uint32_t associative_ordinal;
    PeSectionClass section_class;
    int live;
} PeInputSection;

typedef struct {
    char path[COMPILER_PATH_CAPACITY];
    unsigned char *file;
    size_t size;
    PeInputSection *sections;
    size_t section_count;
    uint32_t symbol_offset;
    uint32_t symbol_count;
    uint32_t string_offset;
    uint32_t string_size;
} PeInputObject;

typedef struct {
    char name[COMPILER_PATH_CAPACITY];
    uint64_t value;
    uint32_t rva;
} PeGlobalSymbol;

typedef struct {
    char dll[PE_IMPORT_NAME_CAPACITY];
    char name[PE_IMPORT_NAME_CAPACITY];
} PeImportDefinition;

typedef struct {
    char dll[PE_IMPORT_NAME_CAPACITY];
    char name[PE_IMPORT_NAME_CAPACITY];
    size_t group_index;
    size_t slot_index;
    uint64_t iat_offset;
    uint64_t ilt_offset;
    uint64_t hint_name_offset;
} PeImport;

typedef struct {
    char dll[PE_IMPORT_NAME_CAPACITY];
    size_t import_count;
    uint64_t iat_offset;
    uint64_t ilt_offset;
    uint64_t name_offset;
} PeImportGroup;

typedef struct {
    PeInputObject objects[PE_MAX_INPUT_OBJECTS];
    size_t object_count;
    PeGlobalSymbol globals[LINKER_MAX_GLOBALS];
    size_t global_count;
    PeImportDefinition import_definitions[PE_MAX_IMPORT_DEFINITIONS];
    size_t import_definition_count;
    PeImport imports[PE_MAX_IMPORTS];
    size_t import_count;
    PeImportGroup import_groups[PE_MAX_IMPORT_GROUPS];
    size_t import_group_count;
    uint64_t text_size;
    uint64_t data_raw_size;
    uint64_t data_virtual_size;
    uint32_t text_rva;
    uint32_t data_rva;
    uint32_t text_file_offset;
    uint32_t data_file_offset;
    uint32_t idata_rva;
    uint32_t idata_file_offset;
    uint64_t idata_size;
    uint64_t iat_size;
    uint32_t entry_rva;
} PeLinkImage;

static int pe_text_starts_with(const char *text, const char *prefix) {
    while (*prefix != '\0') {
        if (*text++ != *prefix++) return 0;
    }
    return 1;
}

static int pe_text_ends_with(const char *text, const char *suffix) {
    size_t text_length = rt_strlen(text);
    size_t suffix_length = rt_strlen(suffix);
    return suffix_length <= text_length && rt_strcmp(text + text_length - suffix_length, suffix) == 0;
}

static uint64_t pe_section_alignment(uint32_t characteristics) {
    uint32_t encoded = (characteristics >> 20U) & 0xfU;
    if (encoded == 0U || encoded > 14U) return 1ULL;
    return 1ULL << (encoded - 1U);
}

static int pe_copy_bounded_name(char *out, size_t out_size, const unsigned char *text, size_t available) {
    size_t length = 0U;
    while (length < available && text[length] != 0U) length += 1U;
    if (length == available || length + 1U > out_size) return -1;
    memcpy(out, text, length);
    out[length] = '\0';
    return 0;
}

static int pe_parse_decimal(const unsigned char *text, size_t size, uint32_t *value_out) {
    uint32_t value = 0U;
    size_t index = 0U;
    if (size == 0U) return -1;
    while (index < size && text[index] != 0U) {
        unsigned int digit;
        if (text[index] < '0' || text[index] > '9') return -1;
        digit = (unsigned int)(text[index] - '0');
        if (value > (0xffffffffU - digit) / 10U) return -1;
        value = value * 10U + digit;
        index += 1U;
    }
    *value_out = value;
    return 0;
}

static char *pe_trim_text(char *text) {
    char *end;
    while (*text == ' ' || *text == '\t' || *text == '\r') text += 1;
    end = text + rt_strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end -= 1;
    *end = '\0';
    return text;
}

static int pe_load_import_definitions(PeLinkImage *link, const char *path, char *error_out, size_t error_size) {
    unsigned char *file = 0;
    size_t file_size = 0U;
    char *text;
    char *line;
    char dll[PE_IMPORT_NAME_CAPACITY];
    int in_exports = 0;

    dll[0] = '\0';
    if (read_file_alloc(path, LINKER_MAX_OBJECT_SIZE, &file, &file_size, error_out, error_size) != 0) return -1;
    text = (char *)rt_malloc(file_size + 1U);
    if (text == 0) {
        rt_free(file);
        set_link_error(error_out, error_size, "out of memory for PE import definitions", path);
        return -1;
    }
    memcpy(text, file, file_size);
    text[file_size] = '\0';
    rt_free(file);
    line = text;
    while (*line != '\0') {
        char *next = line;
        char *trimmed;
        while (*next != '\0' && *next != '\n') next += 1;
        if (*next == '\n') *next++ = '\0';
        trimmed = pe_trim_text(line);
        if (pe_text_starts_with(trimmed, "LIBRARY")) {
            trimmed = pe_trim_text(trimmed + 7U);
            if (trimmed[0] == '\0' || rt_strlen(trimmed) >= sizeof(dll)) {
                rt_free(text);
                set_link_error(error_out, error_size, "invalid PE import library name", path);
                return -1;
            }
            rt_copy_string(dll, sizeof(dll), trimmed);
            in_exports = 0;
        } else if (rt_strcmp(trimmed, "EXPORTS") == 0) {
            in_exports = 1;
        } else if (in_exports && trimmed[0] != '\0' && trimmed[0] != ';') {
            char *end = trimmed;
            PeImportDefinition *definition;
            while (*end != '\0' && *end != ' ' && *end != '\t' && *end != '=' && *end != '@') end += 1;
            *end = '\0';
            if (dll[0] == '\0' || trimmed[0] == '\0' || rt_strlen(trimmed) >= PE_IMPORT_NAME_CAPACITY ||
                link->import_definition_count >= PE_MAX_IMPORT_DEFINITIONS) {
                rt_free(text);
                set_link_error(error_out, error_size, "invalid or excessive PE import definition", path);
                return -1;
            }
            definition = &link->import_definitions[link->import_definition_count++];
            rt_copy_string(definition->dll, sizeof(definition->dll), dll);
            rt_copy_string(definition->name, sizeof(definition->name), trimmed);
        }
        line = next;
    }
    rt_free(text);
    if (dll[0] == '\0') {
        set_link_error(error_out, error_size, "missing PE import library declaration", path);
        return -1;
    }
    return 0;
}

static int pe_read_name(const PeInputObject *object, const unsigned char field[8], char *out, size_t out_size) {
    if (field[0] == '/' && field[1] >= '0' && field[1] <= '9') {
        uint32_t offset;
        if (pe_parse_decimal(field + 1U, 7U, &offset) != 0 || offset < 4U || offset >= object->string_size) return -1;
        return pe_copy_bounded_name(out, out_size, object->file + object->string_offset + offset, object->string_size - offset);
    }
    {
        size_t length = 0U;
        while (length < 8U && field[length] != 0U) length += 1U;
        if (length + 1U > out_size) return -1;
        memcpy(out, field, length);
        out[length] = '\0';
    }
    return 0;
}

static int pe_symbol_name(const PeInputObject *object, const unsigned char *symbol, char *out, size_t out_size) {
    if (read_u32(symbol) == 0U) {
        uint32_t offset = read_u32(symbol + 4U);
        if (offset < 4U || offset >= object->string_size) return -1;
        return pe_copy_bounded_name(out, out_size, object->file + object->string_offset + offset, object->string_size - offset);
    }
    return pe_read_name(object, symbol, out, out_size);
}

static PeSectionClass pe_classify_section(const char *name, uint32_t characteristics) {
    if ((characteristics & PE_SCN_LNK_REMOVE) != 0U || pe_text_starts_with(name, ".debug")) return PE_SECTION_NONE;
    if ((characteristics & PE_SCN_CNT_CODE) != 0U || (characteristics & PE_SCN_MEM_EXECUTE) != 0U) return PE_SECTION_TEXT;
    if ((characteristics & PE_SCN_CNT_UNINITIALIZED_DATA) != 0U) return PE_SECTION_BSS;
    if ((characteristics & PE_SCN_MEM_WRITE) != 0U) return PE_SECTION_DATA;
    if ((characteristics & PE_SCN_CNT_INITIALIZED_DATA) != 0U || (characteristics & PE_SCN_MEM_READ) != 0U) return PE_SECTION_RDATA;
    return PE_SECTION_NONE;
}

static PeInputSection *pe_section_by_ordinal(PeInputObject *object, uint32_t ordinal) {
    size_t index;
    for (index = 0U; index < object->section_count; ++index) {
        if (object->sections[index].ordinal == ordinal) return &object->sections[index];
    }
    return 0;
}

static const PeInputSection *pe_section_by_ordinal_const(const PeInputObject *object, uint32_t ordinal) {
    size_t index;
    for (index = 0U; index < object->section_count; ++index) {
        if (object->sections[index].ordinal == ordinal) return &object->sections[index];
    }
    return 0;
}

static int pe_read_comdat_associations(PeInputObject *object, char *error_out, size_t error_size) {
    uint32_t symbol_index = 0U;
    while (symbol_index < object->symbol_count) {
        const unsigned char *symbol = object->file + object->symbol_offset + (uint64_t)symbol_index * PE_COFF_SYMBOL_SIZE;
        int section_number = (int)(short)read_u16(symbol + 12U);
        unsigned int storage_class = symbol[16];
        unsigned int aux_count = symbol[17];
        if (symbol_index + 1U + aux_count > object->symbol_count) {
            set_link_error(error_out, error_size, "invalid ARM64 COFF auxiliary symbol count", object->path);
            return -1;
        }
        if (storage_class == PE_SYM_CLASS_STATIC && section_number > 0 && aux_count != 0U) {
            PeInputSection *section = pe_section_by_ordinal(object, (uint32_t)section_number);
            const unsigned char *aux = symbol + PE_COFF_SYMBOL_SIZE;
            if (section != 0 && (section->characteristics & PE_SCN_LNK_COMDAT) != 0U &&
                aux[14] == PE_COMDAT_SELECT_ASSOCIATIVE) {
                uint32_t ordinal = read_u16(aux + 12U) | ((uint32_t)read_u16(aux + 16U) << 16U);
                if (ordinal == 0U || pe_section_by_ordinal(object, ordinal) == 0) {
                    set_link_error(error_out, error_size, "invalid associative ARM64 COFF COMDAT", section->name);
                    return -1;
                }
                section->associative_ordinal = ordinal;
            }
        }
        symbol_index += 1U + aux_count;
    }
    return 0;
}

static int pe_load_object(PeInputObject *object, const char *path, char *error_out, size_t error_size) {
    uint16_t section_count;
    uint16_t optional_size;
    uint64_t section_table_offset;
    uint64_t string_offset;
    uint32_t string_size;
    uint16_t section_index;

    memset(object, 0, sizeof(*object));
    rt_copy_string(object->path, sizeof(object->path), path);
    if (read_file_alloc(path, LINKER_MAX_OBJECT_SIZE, &object->file, &object->size, error_out, error_size) != 0) return -1;
    if (object->size < PE_COFF_HEADER_SIZE || read_u16(object->file) != PE_COFF_MACHINE_ARM64) {
        set_link_error(error_out, error_size, "unsupported ARM64 COFF object", path);
        return -1;
    }
    section_count = read_u16(object->file + 2U);
    object->symbol_offset = read_u32(object->file + 8U);
    object->symbol_count = read_u32(object->file + 12U);
    optional_size = read_u16(object->file + 16U);
    section_table_offset = PE_COFF_HEADER_SIZE + optional_size;
    if (section_count == 0U ||
        !range_valid(section_table_offset, (uint64_t)section_count * PE_COFF_SECTION_SIZE, object->size) ||
        !range_valid(object->symbol_offset, (uint64_t)object->symbol_count * PE_COFF_SYMBOL_SIZE + 4ULL, object->size)) {
        set_link_error(error_out, error_size, "invalid ARM64 COFF object", path);
        return -1;
    }
    string_offset = (uint64_t)object->symbol_offset + (uint64_t)object->symbol_count * PE_COFF_SYMBOL_SIZE;
    string_size = read_u32(object->file + string_offset);
    if (string_size < 4U || !range_valid(string_offset, string_size, object->size)) {
        set_link_error(error_out, error_size, "invalid ARM64 COFF string table", path);
        return -1;
    }
    object->string_offset = (uint32_t)string_offset;
    object->string_size = string_size;
    object->sections = (PeInputSection *)rt_malloc((size_t)section_count * sizeof(*object->sections));
    if (object->sections == 0) {
        set_link_error(error_out, error_size, "out of memory for ARM64 COFF sections", path);
        return -1;
    }
    memset(object->sections, 0, (size_t)section_count * sizeof(*object->sections));
    for (section_index = 0U; section_index < section_count; ++section_index) {
        const unsigned char *header = object->file + section_table_offset + (uint64_t)section_index * PE_COFF_SECTION_SIZE;
        PeInputSection *section = &object->sections[object->section_count++];
        section->ordinal = (uint32_t)section_index + 1U;
        section->raw_size = read_u32(header + 16U);
        section->raw_offset = read_u32(header + 20U);
        section->reloc_offset = read_u32(header + 24U);
        section->reloc_count = read_u16(header + 32U);
        section->characteristics = read_u32(header + 36U);
        section->alignment = pe_section_alignment(section->characteristics);
        section->live = 1;
        if (pe_read_name(object, header, section->name, sizeof(section->name)) != 0) {
            set_link_error(error_out, error_size, "invalid ARM64 COFF section name", path);
            return -1;
        }
        section->section_class = pe_classify_section(section->name, section->characteristics);
        if (section->section_class != PE_SECTION_BSS && section->raw_size != 0U && !range_valid(section->raw_offset, section->raw_size, object->size)) {
            set_link_error(error_out, error_size, "invalid ARM64 COFF section data", section->name);
            return -1;
        }
        if (section->reloc_count != 0U && !range_valid(section->reloc_offset, (uint64_t)section->reloc_count * PE_COFF_RELOCATION_SIZE, object->size)) {
            set_link_error(error_out, error_size, "invalid ARM64 COFF relocations", section->name);
            return -1;
        }
        if (section->section_class == PE_SECTION_NONE && section->reloc_count != 0U) {
            set_link_error(error_out, error_size, "unsupported relocated ARM64 COFF section", section->name);
            return -1;
        }
    }
    return pe_read_comdat_associations(object, error_out, error_size);
}

static PeInputSection *pe_find_defined_symbol_section(PeLinkImage *link, const char *name) {
    size_t object_index;
    for (object_index = 0U; object_index < link->object_count; ++object_index) {
        PeInputObject *object = &link->objects[object_index];
        uint32_t symbol_index = 0U;
        while (symbol_index < object->symbol_count) {
            const unsigned char *symbol = object->file + object->symbol_offset + (uint64_t)symbol_index * PE_COFF_SYMBOL_SIZE;
            int section_number = (int)(short)read_u16(symbol + 12U);
            unsigned int storage_class = symbol[16];
            unsigned int aux_count = symbol[17];
            if (storage_class == PE_SYM_CLASS_EXTERNAL && section_number > 0) {
                char symbol_name[COMPILER_PATH_CAPACITY];
                if (pe_symbol_name(object, symbol, symbol_name, sizeof(symbol_name)) == 0 && rt_strcmp(symbol_name, name) == 0) {
                    return pe_section_by_ordinal(object, (uint32_t)section_number);
                }
            }
            symbol_index += 1U + aux_count;
        }
    }
    for (object_index = 0U; object_index < link->object_count; ++object_index) {
        PeInputObject *object = &link->objects[object_index];
        uint32_t symbol_index = 0U;
        while (symbol_index < object->symbol_count) {
            const unsigned char *symbol = object->file + object->symbol_offset + (uint64_t)symbol_index * PE_COFF_SYMBOL_SIZE;
            unsigned int storage_class = symbol[16];
            unsigned int aux_count = symbol[17];
            if (storage_class == PE_SYM_CLASS_WEAK_EXTERNAL && aux_count == 1U &&
                read_u32(symbol + PE_COFF_SYMBOL_SIZE + 4U) == PE_WEAK_SEARCH_ALIAS) {
                char symbol_name[COMPILER_PATH_CAPACITY];
                uint32_t linked_index = read_u32(symbol + PE_COFF_SYMBOL_SIZE);
                if (linked_index < object->symbol_count && pe_symbol_name(object, symbol, symbol_name, sizeof(symbol_name)) == 0 &&
                    rt_strcmp(symbol_name, name) == 0) {
                    const unsigned char *linked = object->file + object->symbol_offset + (uint64_t)linked_index * PE_COFF_SYMBOL_SIZE;
                    int section_number = (int)(short)read_u16(linked + 12U);
                    return section_number > 0 ? pe_section_by_ordinal(object, (uint32_t)section_number) : 0;
                }
            }
            symbol_index += 1U + aux_count;
        }
    }
    return 0;
}

static PeInputSection *pe_relocation_target_section(PeLinkImage *link, PeInputObject *object, uint32_t symbol_index) {
    const unsigned char *symbol;
    int section_number;
    char name[COMPILER_PATH_CAPACITY];
    if (symbol_index >= object->symbol_count) return 0;
    symbol = object->file + object->symbol_offset + (uint64_t)symbol_index * PE_COFF_SYMBOL_SIZE;
    section_number = (int)(short)read_u16(symbol + 12U);
    if (section_number > 0) return pe_section_by_ordinal(object, (uint32_t)section_number);
    if (section_number != PE_SYM_UNDEFINED || pe_symbol_name(object, symbol, name, sizeof(name)) != 0) return 0;
    return pe_find_defined_symbol_section(link, name);
}

static int pe_mark_live_sections(PeLinkImage *link, const char *entry_symbol, char *error_out, size_t error_size) {
    PeInputSection *entry_section;
    size_t object_index;
    int changed;
    for (object_index = 0U; object_index < link->object_count; ++object_index) {
        PeInputObject *object = &link->objects[object_index];
        size_t section_index;
        for (section_index = 0U; section_index < object->section_count; ++section_index) object->sections[section_index].live = 0;
    }
    entry_section = pe_find_defined_symbol_section(link, entry_symbol);
    if (entry_section == 0 || entry_section->section_class == PE_SECTION_NONE) {
        set_link_error(error_out, error_size, "missing PE32+ ARM64 entry symbol", entry_symbol);
        return -1;
    }
    entry_section->live = 1;
    do {
        changed = 0;
        for (object_index = 0U; object_index < link->object_count; ++object_index) {
            PeInputObject *object = &link->objects[object_index];
            size_t section_index;
            for (section_index = 0U; section_index < object->section_count; ++section_index) {
                PeInputSection *section = &object->sections[section_index];
                uint16_t reloc_index;
                if (section->associative_ordinal != 0U) {
                    PeInputSection *parent = pe_section_by_ordinal(object, section->associative_ordinal);
                    if (!section->live && parent != 0 && parent->live) {
                        section->live = 1;
                        changed = 1;
                    }
                }
                if (!section->live) continue;
                for (reloc_index = 0U; reloc_index < section->reloc_count; ++reloc_index) {
                    const unsigned char *reloc = object->file + section->reloc_offset + (uint64_t)reloc_index * PE_COFF_RELOCATION_SIZE;
                    PeInputSection *target = pe_relocation_target_section(link, object, read_u32(reloc + 4U));
                    if (target != 0 && target->section_class != PE_SECTION_NONE && !target->live) {
                        target->live = 1;
                        changed = 1;
                    }
                }
            }
        }
    } while (changed);
    return 0;
}

static int pe_layout_sections(PeLinkImage *link, char *error_out, size_t error_size) {
    uint64_t text_size = 0ULL;
    uint64_t data_size = 0ULL;
    uint64_t bss_size;
    size_t object_index;
    size_t section_index;

    for (object_index = 0U; object_index < link->object_count; ++object_index) {
        PeInputObject *object = &link->objects[object_index];
        for (section_index = 0U; section_index < object->section_count; ++section_index) {
            PeInputSection *section = &object->sections[section_index];
            if (!section->live) continue;
            if (section->section_class == PE_SECTION_TEXT || section->section_class == PE_SECTION_RDATA) {
                text_size = align_u64(text_size, section->alignment);
                section->class_offset = text_size;
                text_size += section->raw_size;
            } else if (section->section_class == PE_SECTION_DATA) {
                data_size = align_u64(data_size, section->alignment);
                section->class_offset = data_size;
                data_size += section->raw_size;
            }
        }
    }
    bss_size = data_size;
    for (object_index = 0U; object_index < link->object_count; ++object_index) {
        PeInputObject *object = &link->objects[object_index];
        for (section_index = 0U; section_index < object->section_count; ++section_index) {
            PeInputSection *section = &object->sections[section_index];
            if (!section->live) continue;
            if (section->section_class == PE_SECTION_BSS) {
                bss_size = align_u64(bss_size, section->alignment);
                section->class_offset = bss_size;
                bss_size += section->raw_size;
            }
        }
    }
    if (text_size == 0ULL || text_size > 0xffffffffULL || data_size > 0xffffffffULL || bss_size > 0xffffffffULL) {
        set_link_error(error_out, error_size, "PE32+ ARM64 image sections are too large", "");
        return -1;
    }
    link->text_size = text_size;
    link->data_raw_size = data_size;
    link->data_virtual_size = bss_size;
    link->text_rva = PE_TEXT_RVA;
    link->text_file_offset = PE_HEADERS_SIZE;
    if (bss_size != 0ULL) {
        link->data_rva = (uint32_t)align_u64((uint64_t)link->text_rva + text_size, PE_SECTION_ALIGNMENT);
        link->data_file_offset = (uint32_t)align_u64((uint64_t)link->text_file_offset + text_size, PE_FILE_ALIGNMENT);
    }
    for (object_index = 0U; object_index < link->object_count; ++object_index) {
        PeInputObject *object = &link->objects[object_index];
        for (section_index = 0U; section_index < object->section_count; ++section_index) {
            PeInputSection *section = &object->sections[section_index];
            if (!section->live) continue;
            if (section->section_class == PE_SECTION_TEXT || section->section_class == PE_SECTION_RDATA) {
                section->output_rva = (uint64_t)link->text_rva + section->class_offset;
                section->output_file_offset = (uint64_t)link->text_file_offset + section->class_offset;
            } else if (section->section_class == PE_SECTION_DATA) {
                section->output_rva = (uint64_t)link->data_rva + section->class_offset;
                section->output_file_offset = (uint64_t)link->data_file_offset + section->class_offset;
            } else if (section->section_class == PE_SECTION_BSS) {
                section->output_rva = (uint64_t)link->data_rva + section->class_offset;
            }
        }
    }
    return 0;
}

static int pe_find_global(const PeLinkImage *link, const char *name, uint64_t *value_out, uint32_t *rva_out) {
    size_t index;
    for (index = 0U; index < link->global_count; ++index) {
        if (rt_strcmp(link->globals[index].name, name) == 0) {
            *value_out = link->globals[index].value;
            if (rva_out != 0) *rva_out = link->globals[index].rva;
            return 0;
        }
    }
    return -1;
}

static int pe_collect_globals(PeLinkImage *link, char *error_out, size_t error_size) {
    size_t object_index;
    for (object_index = 0U; object_index < link->object_count; ++object_index) {
        PeInputObject *object = &link->objects[object_index];
        uint32_t symbol_index = 0U;
        while (symbol_index < object->symbol_count) {
            const unsigned char *symbol = object->file + object->symbol_offset + (uint64_t)symbol_index * PE_COFF_SYMBOL_SIZE;
            int section_number = (int)(short)read_u16(symbol + 12U);
            unsigned int storage_class = symbol[16];
            unsigned int aux_count = symbol[17];
            if (symbol_index + 1U + aux_count > object->symbol_count) {
                set_link_error(error_out, error_size, "invalid ARM64 COFF auxiliary symbol count", object->path);
                return -1;
            }
            if (storage_class == PE_SYM_CLASS_EXTERNAL && section_number > 0) {
                PeInputSection *section = pe_section_by_ordinal(object, (uint32_t)section_number);
                char name[COMPILER_PATH_CAPACITY];
                uint64_t section_offset;
                uint64_t value;
                uint64_t existing_value;
                if (section == 0 || !section->live || section->section_class == PE_SECTION_NONE) {
                    symbol_index += 1U + aux_count;
                    continue;
                }
                if (pe_symbol_name(object, symbol, name, sizeof(name)) != 0) {
                    set_link_error(error_out, error_size, "invalid defined ARM64 COFF symbol", object->path);
                    return -1;
                }
                section_offset = read_u32(symbol + 8U);
                if (section_offset > section->raw_size) {
                    set_link_error(error_out, error_size, "ARM64 COFF symbol lies outside its section", name);
                    return -1;
                }
                if (pe_find_global(link, name, &existing_value, 0) == 0) {
                    set_link_error(error_out, error_size, "duplicate ARM64 COFF symbol", name);
                    return -1;
                }
                if (link->global_count >= LINKER_MAX_GLOBALS) {
                    set_link_error(error_out, error_size, "too many ARM64 COFF symbols", name);
                    return -1;
                }
                value = PE_IMAGE_BASE + section->output_rva + section_offset;
                rt_copy_string(link->globals[link->global_count].name, sizeof(link->globals[link->global_count].name), name);
                link->globals[link->global_count].value = value;
                link->globals[link->global_count].rva = (uint32_t)(section->output_rva + section_offset);
                link->global_count += 1U;
            }
            symbol_index += 1U + aux_count;
        }
    }
    for (object_index = 0U; object_index < link->object_count; ++object_index) {
        PeInputObject *object = &link->objects[object_index];
        uint32_t symbol_index = 0U;
        while (symbol_index < object->symbol_count) {
            const unsigned char *symbol = object->file + object->symbol_offset + (uint64_t)symbol_index * PE_COFF_SYMBOL_SIZE;
            unsigned int storage_class = symbol[16];
            unsigned int aux_count = symbol[17];
            if (storage_class == PE_SYM_CLASS_WEAK_EXTERNAL && aux_count == 1U &&
                read_u32(symbol + PE_COFF_SYMBOL_SIZE + 4U) == PE_WEAK_SEARCH_ALIAS) {
                uint32_t linked_index = read_u32(symbol + PE_COFF_SYMBOL_SIZE);
                char name[COMPILER_PATH_CAPACITY];
                uint64_t existing_value;
                if (pe_symbol_name(object, symbol, name, sizeof(name)) != 0 || linked_index >= object->symbol_count) {
                    set_link_error(error_out, error_size, "invalid ARM64 COFF weak external", object->path);
                    return -1;
                }
                if (pe_find_global(link, name, &existing_value, 0) != 0) {
                    const unsigned char *linked = object->file + object->symbol_offset + (uint64_t)linked_index * PE_COFF_SYMBOL_SIZE;
                    int linked_section_number = (int)(short)read_u16(linked + 12U);
                    PeInputSection *section = linked_section_number > 0
                        ? pe_section_by_ordinal(object, (uint32_t)linked_section_number)
                        : 0;
                    uint64_t section_offset = read_u32(linked + 8U);
                    if (section == 0 || !section->live) {
                        symbol_index += 1U + aux_count;
                        continue;
                    }
                    if (section->section_class == PE_SECTION_NONE || section_offset > section->raw_size ||
                        link->global_count >= LINKER_MAX_GLOBALS) {
                        set_link_error(error_out, error_size, "invalid ARM64 COFF weak alias target", name);
                        return -1;
                    }
                    rt_copy_string(link->globals[link->global_count].name, sizeof(link->globals[link->global_count].name), name);
                    link->globals[link->global_count].value = PE_IMAGE_BASE + section->output_rva + section_offset;
                    link->globals[link->global_count].rva = (uint32_t)(section->output_rva + section_offset);
                    link->global_count += 1U;
                }
            }
            symbol_index += 1U + aux_count;
        }
    }
    return 0;
}

static const PeImportDefinition *pe_find_import_definition(const PeLinkImage *link, const char *name) {
    size_t index;
    for (index = 0U; index < link->import_definition_count; ++index) {
        if (rt_strcmp(link->import_definitions[index].name, name) == 0) return &link->import_definitions[index];
    }
    return 0;
}

static int pe_import_already_added(const PeLinkImage *link, const char *dll, const char *name) {
    size_t index;
    for (index = 0U; index < link->import_count; ++index) {
        if (rt_strcmp(link->imports[index].dll, dll) == 0 && rt_strcmp(link->imports[index].name, name) == 0) return 1;
    }
    return 0;
}

static int pe_collect_imports(PeLinkImage *link, char *error_out, size_t error_size) {
    size_t object_index;
    for (object_index = 0U; object_index < link->object_count; ++object_index) {
        PeInputObject *object = &link->objects[object_index];
        size_t section_index;
        for (section_index = 0U; section_index < object->section_count; ++section_index) {
            PeInputSection *section = &object->sections[section_index];
            uint16_t reloc_index;
            if (!section->live) continue;
            for (reloc_index = 0U; reloc_index < section->reloc_count; ++reloc_index) {
                const unsigned char *reloc = object->file + section->reloc_offset + (uint64_t)reloc_index * PE_COFF_RELOCATION_SIZE;
                uint32_t symbol_index = read_u32(reloc + 4U);
                const unsigned char *symbol;
                int section_number;
                unsigned int storage_class;
                char symbol_name[COMPILER_PATH_CAPACITY];
                uint64_t existing_value;
                uint32_t existing_rva;
                if (symbol_index >= object->symbol_count) {
                    set_link_error(error_out, error_size, "invalid ARM64 COFF import relocation symbol", object->path);
                    return -1;
                }
                symbol = object->file + object->symbol_offset + (uint64_t)symbol_index * PE_COFF_SYMBOL_SIZE;
                section_number = (int)(short)read_u16(symbol + 12U);
                storage_class = symbol[16];
                if (storage_class != PE_SYM_CLASS_EXTERNAL || section_number != PE_SYM_UNDEFINED) continue;
                if (pe_symbol_name(object, symbol, symbol_name, sizeof(symbol_name)) != 0) {
                    set_link_error(error_out, error_size, "invalid undefined ARM64 COFF symbol", object->path);
                    return -1;
                }
                if (pe_find_global(link, symbol_name, &existing_value, &existing_rva) != 0 && pe_text_starts_with(symbol_name, "__imp_")) {
                    const char *import_name = symbol_name + 6U;
                    const PeImportDefinition *definition = pe_find_import_definition(link, import_name);
                    if (definition != 0 && !pe_import_already_added(link, definition->dll, import_name)) {
                        PeImport *import;
                        if (link->import_count >= PE_MAX_IMPORTS) {
                            set_link_error(error_out, error_size, "too many PE imports", import_name);
                            return -1;
                        }
                        import = &link->imports[link->import_count++];
                        rt_copy_string(import->dll, sizeof(import->dll), definition->dll);
                        rt_copy_string(import->name, sizeof(import->name), import_name);
                    }
                }
            }
        }
    }
    return 0;
}

static size_t pe_import_group_for(PeLinkImage *link, const char *dll, char *error_out, size_t error_size) {
    size_t index;
    for (index = 0U; index < link->import_group_count; ++index) {
        if (rt_strcmp(link->import_groups[index].dll, dll) == 0) return index;
    }
    if (link->import_group_count >= PE_MAX_IMPORT_GROUPS) {
        set_link_error(error_out, error_size, "too many PE import libraries", dll);
        return LINKER_NO_INDEX;
    }
    index = link->import_group_count++;
    rt_copy_string(link->import_groups[index].dll, sizeof(link->import_groups[index].dll), dll);
    return index;
}

static int pe_layout_imports(PeLinkImage *link, char *error_out, size_t error_size) {
    uint64_t offset;
    uint64_t iat_start;
    uint64_t idata_offset;
    size_t index;

    if (link->import_count == 0U) return 0;
    for (index = 0U; index < link->import_count; ++index) {
        size_t group_index = pe_import_group_for(link, link->imports[index].dll, error_out, error_size);
        if (group_index == LINKER_NO_INDEX) return -1;
        link->imports[index].group_index = group_index;
        link->imports[index].slot_index = link->import_groups[group_index].import_count++;
    }
    offset = ((uint64_t)link->import_group_count + 1ULL) * 20ULL;
    offset = align_u64(offset, 8ULL);
    iat_start = offset;
    for (index = 0U; index < link->import_group_count; ++index) {
        link->import_groups[index].iat_offset = offset;
        offset += ((uint64_t)link->import_groups[index].import_count + 1ULL) * 8ULL;
    }
    link->iat_size = offset - iat_start;
    for (index = 0U; index < link->import_group_count; ++index) {
        link->import_groups[index].ilt_offset = offset;
        offset += ((uint64_t)link->import_groups[index].import_count + 1ULL) * 8ULL;
    }
    for (index = 0U; index < link->import_group_count; ++index) {
        link->import_groups[index].name_offset = offset;
        offset += rt_strlen(link->import_groups[index].dll) + 1U;
    }
    offset = align_u64(offset, 2ULL);
    for (index = 0U; index < link->import_count; ++index) {
        PeImport *import = &link->imports[index];
        PeImportGroup *group = &link->import_groups[import->group_index];
        import->iat_offset = group->iat_offset + (uint64_t)import->slot_index * 8ULL;
        import->ilt_offset = group->ilt_offset + (uint64_t)import->slot_index * 8ULL;
        import->hint_name_offset = offset;
        offset += 2ULL + rt_strlen(import->name) + 1ULL;
        offset = align_u64(offset, 2ULL);
    }
    link->idata_size = offset;
    idata_offset = align_u64(link->text_size, 8ULL);
    if (idata_offset + link->idata_size > 0xffffffffULL) {
        set_link_error(error_out, error_size, "PE32+ ARM64 text and imports are too large", "");
        return -1;
    }
    link->idata_rva = link->text_rva + (uint32_t)idata_offset;
    link->idata_file_offset = link->text_file_offset + (uint32_t)idata_offset;
    link->text_size = idata_offset + link->idata_size;
    if (link->data_virtual_size != 0ULL) {
        size_t object_index;
        uint64_t data_rva = align_u64((uint64_t)link->text_rva + link->text_size, PE_SECTION_ALIGNMENT);
        uint64_t data_file_offset = align_u64((uint64_t)link->text_file_offset + link->text_size, PE_FILE_ALIGNMENT);
        if (data_rva > 0xffffffffULL || data_file_offset > 0xffffffffULL) {
            set_link_error(error_out, error_size, "PE32+ ARM64 data layout is too large", "");
            return -1;
        }
        link->data_rva = (uint32_t)data_rva;
        link->data_file_offset = (uint32_t)data_file_offset;
        for (object_index = 0U; object_index < link->object_count; ++object_index) {
            PeInputObject *object = &link->objects[object_index];
            size_t section_index;
            for (section_index = 0U; section_index < object->section_count; ++section_index) {
                PeInputSection *section = &object->sections[section_index];
                if (!section->live) continue;
                if (section->section_class == PE_SECTION_DATA) {
                    section->output_rva = (uint64_t)link->data_rva + section->class_offset;
                    section->output_file_offset = (uint64_t)link->data_file_offset + section->class_offset;
                } else if (section->section_class == PE_SECTION_BSS) {
                    section->output_rva = (uint64_t)link->data_rva + section->class_offset;
                }
            }
        }
    }
    return 0;
}

static int pe_collect_import_globals(PeLinkImage *link, char *error_out, size_t error_size) {
    size_t index;
    for (index = 0U; index < link->import_count; ++index) {
        char symbol_name[PE_IMPORT_NAME_CAPACITY + 8U];
        PeImport *import = &link->imports[index];
        uint64_t existing_value;
        rt_copy_string(symbol_name, sizeof(symbol_name), "__imp_");
        rt_copy_string(symbol_name + 6U, sizeof(symbol_name) - 6U, import->name);
        if (pe_find_global(link, symbol_name, &existing_value, 0) == 0 || link->global_count >= LINKER_MAX_GLOBALS) {
            set_link_error(error_out, error_size, "duplicate or excessive PE import symbol", symbol_name);
            return -1;
        }
        rt_copy_string(link->globals[link->global_count].name, sizeof(link->globals[link->global_count].name), symbol_name);
        link->globals[link->global_count].rva = link->idata_rva + (uint32_t)import->iat_offset;
        link->globals[link->global_count].value = PE_IMAGE_BASE + link->globals[link->global_count].rva;
        link->global_count += 1U;
    }
    return 0;
}

static int pe_symbol_value(PeLinkImage *link, size_t object_index, uint32_t symbol_index,
                           uint64_t *value_out, uint32_t *rva_out, char *error_out, size_t error_size) {
    PeInputObject *object = &link->objects[object_index];
    const unsigned char *symbol;
    int section_number;
    char name[COMPILER_PATH_CAPACITY];
    if (symbol_index >= object->symbol_count) {
        set_link_error(error_out, error_size, "invalid ARM64 COFF relocation symbol index", object->path);
        return -1;
    }
    symbol = object->file + object->symbol_offset + (uint64_t)symbol_index * PE_COFF_SYMBOL_SIZE;
    section_number = (int)(short)read_u16(symbol + 12U);
    if (section_number > 0) {
        const PeInputSection *section = pe_section_by_ordinal_const(object, (uint32_t)section_number);
        uint64_t section_offset = read_u32(symbol + 8U);
        if (section == 0 || section->section_class == PE_SECTION_NONE || section_offset > section->raw_size) {
            set_link_error(error_out, error_size, "invalid ARM64 COFF relocation symbol", object->path);
            return -1;
        }
        *rva_out = (uint32_t)(section->output_rva + section_offset);
        *value_out = PE_IMAGE_BASE + *rva_out;
        return 0;
    }
    if (section_number == PE_SYM_ABSOLUTE) {
        *rva_out = read_u32(symbol + 8U);
        *value_out = *rva_out;
        return 0;
    }
    if (section_number != PE_SYM_UNDEFINED || pe_symbol_name(object, symbol, name, sizeof(name)) != 0 ||
        pe_find_global(link, name, value_out, rva_out) != 0) {
        set_link_error(error_out, error_size, "undefined ARM64 COFF symbol", pe_symbol_name(object, symbol, name, sizeof(name)) == 0 ? name : "");
        return -1;
    }
    return 0;
}

static int pe_apply_relocations(PeLinkImage *link, unsigned char *output, size_t output_size, char *error_out, size_t error_size) {
    size_t object_index;
    for (object_index = 0U; object_index < link->object_count; ++object_index) {
        PeInputObject *object = &link->objects[object_index];
        size_t section_index;
        for (section_index = 0U; section_index < object->section_count; ++section_index) {
            PeInputSection *section = &object->sections[section_index];
            uint16_t reloc_index;
            if (!section->live || section->reloc_count == 0U) continue;
            for (reloc_index = 0U; reloc_index < section->reloc_count; ++reloc_index) {
                const unsigned char *reloc = object->file + section->reloc_offset + (uint64_t)reloc_index * PE_COFF_RELOCATION_SIZE;
                uint32_t offset = read_u32(reloc);
                uint32_t symbol_index = read_u32(reloc + 4U);
                uint16_t type = read_u16(reloc + 8U);
                uint64_t target;
                uint32_t target_rva;
                uint64_t place;
                unsigned char *patch;
                uint32_t instruction;
                if (section->section_class == PE_SECTION_BSS || section->section_class == PE_SECTION_NONE ||
                    offset + 4U > section->raw_size || !range_valid(section->output_file_offset + offset, 4U, output_size)) {
                    set_link_error(error_out, error_size, "invalid ARM64 COFF relocation offset", section->name);
                    return -1;
                }
                if (pe_symbol_value(link, object_index, symbol_index, &target, &target_rva, error_out, error_size) != 0) return -1;
                place = PE_IMAGE_BASE + section->output_rva + offset;
                patch = output + section->output_file_offset + offset;
                if (type == PE_REL_ARM64_BRANCH26) {
                    int64_t delta = (int64_t)target - (int64_t)place;
                    int64_t immediate;
                    if ((delta & 3LL) != 0LL) {
                        set_link_error(error_out, error_size, "unaligned ARM64 COFF branch relocation", section->name);
                        return -1;
                    }
                    immediate = delta >> 2;
                    if (immediate < -(1LL << 25) || immediate >= (1LL << 25)) {
                        set_link_error(error_out, error_size, "ARM64 COFF branch relocation is out of range", section->name);
                        return -1;
                    }
                    instruction = read_u32(patch);
                    write_u32(patch, (instruction & 0xfc000000U) | ((uint32_t)immediate & 0x03ffffffU));
                } else if (type == PE_REL_ARM64_PAGEBASE_REL21) {
                    int64_t delta = (int64_t)(target & ~0xfffULL) - (int64_t)(place & ~0xfffULL);
                    int64_t immediate = delta >> 12;
                    if ((delta & 0xfffLL) != 0LL || immediate < -(1LL << 20) || immediate >= (1LL << 20)) {
                        set_link_error(error_out, error_size, "ARM64 COFF page relocation is out of range", section->name);
                        return -1;
                    }
                    instruction = read_u32(patch);
                    instruction = (instruction & ~0x60ffffe0U) |
                                  (((uint32_t)immediate & 3U) << 29U) |
                                  ((((uint32_t)immediate >> 2U) & 0x7ffffU) << 5U);
                    write_u32(patch, instruction);
                } else if (type == PE_REL_ARM64_REL21) {
                    int64_t delta = (int64_t)target - (int64_t)place;
                    if (delta < -(1LL << 20) || delta >= (1LL << 20)) {
                        set_link_error(error_out, error_size, "ARM64 COFF relative relocation is out of range", section->name);
                        return -1;
                    }
                    instruction = read_u32(patch);
                    instruction = (instruction & ~0x60ffffe0U) |
                                  (((uint32_t)delta & 3U) << 29U) |
                                  ((((uint32_t)delta >> 2U) & 0x7ffffU) << 5U);
                    write_u32(patch, instruction);
                } else if (type == PE_REL_ARM64_PAGEOFFSET_12A) {
                    uint32_t page_offset = (uint32_t)(target & 0xfffULL);
                    instruction = read_u32(patch);
                    if ((instruction & 0x1f000000U) != 0x11000000U || (instruction & 0x00400000U) != 0U) {
                        set_link_error(error_out, error_size, "unsupported ARM64 COFF add page-offset instruction", section->name);
                        return -1;
                    }
                    write_u32(patch, (instruction & ~0x003ffc00U) | (page_offset << 10U));
                } else if (type == PE_REL_ARM64_PAGEOFFSET_12L) {
                    uint32_t page_offset = (uint32_t)(target & 0xfffULL);
                    uint32_t scale;
                    instruction = read_u32(patch);
                    if ((instruction & 0x3b000000U) != 0x39000000U) {
                        set_link_error(error_out, error_size, "unsupported ARM64 COFF load/store page-offset instruction", section->name);
                        return -1;
                    }
                    scale = (instruction >> 30U) & 3U;
                    if ((instruction & 0x04000000U) != 0U && ((instruction >> 22U) & 3U) == 3U) scale = 4U;
                    if ((page_offset & ((1U << scale) - 1U)) != 0U) {
                        set_link_error(error_out, error_size, "unaligned ARM64 COFF load/store relocation", section->name);
                        return -1;
                    }
                    write_u32(patch, (instruction & ~0x003ffc00U) | ((page_offset >> scale) << 10U));
                } else if (type == PE_REL_ARM64_ADDR64) {
                    if (offset + 8U > section->raw_size || !range_valid(section->output_file_offset + offset, 8U, output_size)) {
                        set_link_error(error_out, error_size, "invalid ARM64 COFF 64-bit relocation", section->name);
                        return -1;
                    }
                    write_u64(patch, target + read_u64(patch));
                } else if (type == PE_REL_ARM64_ADDR32NB) {
                    write_u32(patch, target_rva + read_u32(patch));
                } else if (type == PE_REL_ARM64_ADDR32) {
                    uint64_t value = target + read_u32(patch);
                    if (value > 0xffffffffULL) {
                        set_link_error(error_out, error_size, "ARM64 COFF 32-bit address relocation is out of range", section->name);
                        return -1;
                    }
                    write_u32(patch, (uint32_t)value);
                } else if (type == PE_REL_ARM64_REL32) {
                    int64_t value = (int64_t)target + (int32_t)read_u32(patch) - (int64_t)(place + 4ULL);
                    if (value < -0x80000000LL || value > 0x7fffffffLL) {
                        set_link_error(error_out, error_size, "ARM64 COFF 32-bit relative relocation is out of range", section->name);
                        return -1;
                    }
                    write_u32(patch, (uint32_t)value);
                } else {
                    set_link_error(error_out, error_size, "unsupported ARM64 COFF relocation type", section->name);
                    return -1;
                }
            }
        }
    }
    return 0;
}

static void pe_write_section_header(unsigned char *header, const char *name, uint32_t virtual_size, uint32_t rva,
                                    uint32_t raw_size, uint32_t raw_offset, uint32_t characteristics) {
    size_t index;
    memset(header, 0, PE_COFF_SECTION_SIZE);
    for (index = 0U; index < 8U && name[index] != '\0'; ++index) header[index] = (unsigned char)name[index];
    write_u32(header + 8U, virtual_size);
    write_u32(header + 12U, rva);
    write_u32(header + 16U, raw_size);
    write_u32(header + 20U, raw_offset);
    write_u32(header + 36U, characteristics);
}

static void pe_write_import_data(const PeLinkImage *link, unsigned char *output) {
    unsigned char *idata = output + link->idata_file_offset;
    size_t index;
    for (index = 0U; index < link->import_group_count; ++index) {
        const PeImportGroup *group = &link->import_groups[index];
        unsigned char *descriptor = idata + index * 20U;
        write_u32(descriptor, link->idata_rva + (uint32_t)group->ilt_offset);
        write_u32(descriptor + 12U, link->idata_rva + (uint32_t)group->name_offset);
        write_u32(descriptor + 16U, link->idata_rva + (uint32_t)group->iat_offset);
        memcpy(idata + group->name_offset, group->dll, rt_strlen(group->dll) + 1U);
    }
    for (index = 0U; index < link->import_count; ++index) {
        const PeImport *import = &link->imports[index];
        uint64_t hint_name_rva = (uint64_t)link->idata_rva + import->hint_name_offset;
        write_u64(idata + import->iat_offset, hint_name_rva);
        write_u64(idata + import->ilt_offset, hint_name_rva);
        write_u16(idata + import->hint_name_offset, 0U);
        memcpy(idata + import->hint_name_offset + 2U, import->name, rt_strlen(import->name) + 1U);
    }
}

static int pe_write_image(PeLinkImage *link, const char *output_path, char *error_out, size_t error_size) {
    uint32_t text_raw_size = (uint32_t)align_u64(link->text_size, PE_FILE_ALIGNMENT);
    uint32_t data_raw_size = (uint32_t)align_u64(link->data_raw_size, PE_FILE_ALIGNMENT);
    uint16_t output_section_count = 1U + (link->data_virtual_size != 0ULL ? 1U : 0U);
    uint32_t file_size = link->data_raw_size != 0ULL
        ? link->data_file_offset + data_raw_size
        : link->text_file_offset + text_raw_size;
    uint32_t image_size = link->data_virtual_size != 0ULL
        ? (uint32_t)align_u64((uint64_t)link->data_rva + link->data_virtual_size, PE_SECTION_ALIGNMENT)
        : (uint32_t)align_u64((uint64_t)link->text_rva + link->text_size, PE_SECTION_ALIGNMENT);
    unsigned char *output;
    unsigned char *coff;
    unsigned char *optional;
    unsigned char *section_headers;
    size_t object_index;
    int fd;

    if (file_size > LINKER_MAX_OUTPUT) {
        set_link_error(error_out, error_size, "PE32+ ARM64 output is too large", output_path);
        return -1;
    }
    output = (unsigned char *)rt_malloc(file_size);
    if (output == 0) {
        set_link_error(error_out, error_size, "out of memory for PE32+ ARM64 output", output_path);
        return -1;
    }
    memset(output, 0, file_size);
    output[0] = 'M';
    output[1] = 'Z';
    write_u16(output + 2U, 0x78U);
    write_u16(output + 4U, 1U);
    write_u16(output + 8U, 4U);
    write_u16(output + 10U, 0U);
    write_u16(output + 12U, 0xffffU);
    write_u16(output + 16U, 0xb8U);
    write_u16(output + 24U, 0x40U);
    write_u32(output + 60U, PE_SIGNATURE_OFFSET);
    memcpy(output + PE_SIGNATURE_OFFSET, "PE\0\0", 4U);
    coff = output + PE_SIGNATURE_OFFSET + 4U;
    write_u16(coff, PE_COFF_MACHINE_ARM64);
    write_u16(coff + 2U, output_section_count);
    write_u16(coff + 16U, PE_OPTIONAL_HEADER_SIZE);
    write_u16(coff + 18U, 0x0022U);
    optional = coff + PE_COFF_HEADER_SIZE;
    write_u16(optional, 0x020bU);
    optional[2] = 1U;
    write_u32(optional + 4U, text_raw_size);
    write_u32(optional + 8U, data_raw_size);
    write_u32(optional + 12U, (uint32_t)(link->data_virtual_size - link->data_raw_size));
    write_u32(optional + 16U, link->entry_rva);
    write_u32(optional + 20U, link->text_rva);
    write_u64(optional + 24U, PE_IMAGE_BASE);
    write_u32(optional + 32U, PE_SECTION_ALIGNMENT);
    write_u32(optional + 36U, PE_FILE_ALIGNMENT);
    write_u16(optional + 40U, 6U);
    write_u16(optional + 48U, 6U);
    write_u32(optional + 56U, image_size);
    write_u32(optional + 60U, PE_HEADERS_SIZE);
    write_u16(optional + 68U, 3U);
    write_u16(optional + 70U, 0x8160U);
    write_u64(optional + 72U, 8ULL * 1024ULL * 1024ULL);
    write_u64(optional + 80U, 4096ULL);
    write_u64(optional + 88U, 1024ULL * 1024ULL);
    write_u64(optional + 96U, 4096ULL);
    write_u32(optional + 108U, 16U);
    if (link->idata_size != 0ULL) {
        uint32_t iat_offset = (uint32_t)align_u64(((uint64_t)link->import_group_count + 1ULL) * 20ULL, 8ULL);
        write_u32(optional + 120U, link->idata_rva);
        write_u32(optional + 124U, ((uint32_t)link->import_group_count + 1U) * 20U);
        write_u32(optional + 208U, link->idata_rva + iat_offset);
        write_u32(optional + 212U, (uint32_t)link->iat_size);
    }
    section_headers = optional + PE_OPTIONAL_HEADER_SIZE;
    pe_write_section_header(section_headers, ".text", (uint32_t)link->text_size, link->text_rva,
                            text_raw_size, link->text_file_offset, 0x60000020U);
    if (link->data_virtual_size != 0ULL) {
        pe_write_section_header(section_headers + PE_COFF_SECTION_SIZE, ".data", (uint32_t)link->data_virtual_size,
                                link->data_rva, data_raw_size, link->data_file_offset,
                                link->data_virtual_size > link->data_raw_size ? 0xc00000c0U : 0xc0000040U);
    }
    for (object_index = 0U; object_index < link->object_count; ++object_index) {
        PeInputObject *object = &link->objects[object_index];
        size_t section_index;
        for (section_index = 0U; section_index < object->section_count; ++section_index) {
            PeInputSection *section = &object->sections[section_index];
            if (section->live && (section->section_class == PE_SECTION_TEXT || section->section_class == PE_SECTION_RDATA || section->section_class == PE_SECTION_DATA) && section->raw_size != 0U) {
                memcpy(output + section->output_file_offset, object->file + section->raw_offset, section->raw_size);
            }
        }
    }
    if (link->idata_size != 0ULL) pe_write_import_data(link, output);
    if (pe_apply_relocations(link, output, file_size, error_out, error_size) != 0) {
        rt_free(output);
        return -1;
    }
    fd = platform_open_write(output_path, 0755U);
    if (fd < 0 || rt_write_all(fd, output, file_size) != 0 || platform_close(fd) != 0) {
        if (fd >= 0) (void)platform_close(fd);
        rt_free(output);
        set_link_error(error_out, error_size, "failed to write PE32+ ARM64 output", output_path);
        return -1;
    }
    rt_free(output);
    return 0;
}

int compiler_link_pe32plus_aarch64_static_options(const char *const *object_paths,
                                                  size_t object_count,
                                                  const char *output_path,
                                                  const CompilerLinkerOptions *options,
                                                  char *error_out,
                                                  size_t error_size) {
    PeLinkImage *link;
    const char *entry_symbol = options != 0 && options->entry_symbol != 0 ? options->entry_symbol : "mainCRTStartup";
    size_t object_index;
    uint64_t entry_value;
    int result = -1;

    if (object_paths == 0 || object_count == 0U || object_count > PE_MAX_INPUT_OBJECTS + PE_MAX_IMPORT_GROUPS || output_path == 0) {
        set_link_error(error_out, error_size, "invalid PE32+ ARM64 link arguments", "");
        return -1;
    }
    link = (PeLinkImage *)rt_malloc(sizeof(*link));
    if (link == 0) {
        set_link_error(error_out, error_size, "out of memory for PE32+ ARM64 link", "");
        return -1;
    }
    memset(link, 0, sizeof(*link));
    for (object_index = 0U; object_index < object_count; ++object_index) {
        if (pe_text_ends_with(object_paths[object_index], ".def")) {
            if (pe_load_import_definitions(link, object_paths[object_index], error_out, error_size) != 0) goto cleanup;
        } else {
            if (link->object_count >= PE_MAX_INPUT_OBJECTS) {
                set_link_error(error_out, error_size, "too many ARM64 COFF input objects", object_paths[object_index]);
                goto cleanup;
            }
            if (pe_load_object(&link->objects[link->object_count], object_paths[object_index], error_out, error_size) != 0) {
                link->object_count += 1U;
                goto cleanup;
            }
            link->object_count += 1U;
        }
    }
    if (link->object_count == 0U ||
        (options != 0 && options->gc_sections != 0 && pe_mark_live_sections(link, entry_symbol, error_out, error_size) != 0) ||
        pe_layout_sections(link, error_out, error_size) != 0 ||
        pe_collect_globals(link, error_out, error_size) != 0 || pe_collect_imports(link, error_out, error_size) != 0 ||
        pe_layout_imports(link, error_out, error_size) != 0) goto cleanup;
    link->global_count = 0U;
    if (pe_collect_globals(link, error_out, error_size) != 0 ||
        pe_collect_import_globals(link, error_out, error_size) != 0) goto cleanup;
    if (pe_find_global(link, entry_symbol, &entry_value, &link->entry_rva) != 0) {
        set_link_error(error_out, error_size, "missing PE32+ ARM64 entry symbol", entry_symbol);
        goto cleanup;
    }
    if (pe_write_image(link, output_path, error_out, error_size) != 0) goto cleanup;
    result = 0;

cleanup:
    for (object_index = 0U; object_index < link->object_count; ++object_index) {
        rt_free(link->objects[object_index].sections);
        rt_free(link->objects[object_index].file);
    }
    rt_free(link);
    return result;
}