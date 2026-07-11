#include "linker_internal.h"

unsigned char *linker_merge_string_pool;
uint64_t       linker_merge_string_pool_size;
uint64_t       linker_merge_string_pool_capacity;
int            linker_merge_string_pool_active;
size_t         linker_merge_master_object_index;
size_t         linker_merge_master_section_index;
uint64_t       linker_merge_master_input_size;
#if COMPILER_LINKER_ENABLE_CONST_MERGE
unsigned char *linker_merge_const_pool;
uint64_t       linker_merge_const_pool_size;
uint64_t       linker_merge_const_pool_capacity;
int            linker_merge_const_pool_active;
size_t         linker_merge_const_master_object_index;
size_t         linker_merge_const_master_section_index;
uint64_t       linker_merge_const_master_input_size;
#endif

void reset_merge_string_pool(void) {
    if (linker_merge_string_pool != 0) {
        rt_free(linker_merge_string_pool);
    }
    linker_merge_string_pool = 0;
    linker_merge_string_pool_size = 0ULL;
    linker_merge_string_pool_capacity = 0ULL;
    linker_merge_string_pool_active = 0;
    linker_merge_master_object_index = 0U;
    linker_merge_master_section_index = 0U;
    linker_merge_master_input_size = 0ULL;
}

#if COMPILER_LINKER_ENABLE_CONST_MERGE
void reset_merge_const_pool(void) {
    if (linker_merge_const_pool != 0) {
        rt_free(linker_merge_const_pool);
    }
    linker_merge_const_pool = 0;
    linker_merge_const_pool_size = 0ULL;
    linker_merge_const_pool_capacity = 0ULL;
    linker_merge_const_pool_active = 0;
    linker_merge_const_master_object_index = 0U;
    linker_merge_const_master_section_index = 0U;
    linker_merge_const_master_input_size = 0ULL;
}
#endif

static uint64_t section_entry_size(const LinkObject *object, const LinkSection *section) {
    const unsigned char *header = section_header(object, section->index);

    return header != 0 ? read_u64(header + 56) : 0ULL;
}

static int section_is_mergeable_string(const LinkObject *object, const LinkSection *section) {
    uint64_t entsize = section_entry_size(object, section);

    return section->live && !section->folded && section->kind == LINK_SECTION_TEXT && section->type == SHT_PROGBITS &&
           (section->flags & SHF_EXECINSTR) == 0ULL && (section->flags & SHF_MERGE) != 0ULL && (section->flags & SHF_STRINGS) != 0ULL &&
           entsize == 1ULL && section->align <= 1ULL && !section_has_relocations(object, section->index);
}

#if COMPILER_LINKER_ENABLE_CONST_MERGE
static int merge_const_section_references_supported(const LinkObject *object, const LinkSection *section, uint64_t entsize) {
    uint32_t symbol_count = (uint32_t)(object->symtab_size / object->symtab_entsize);
    uint32_t symbol_index;
    size_t rela_index;

    for (symbol_index = 0; symbol_index < symbol_count; ++symbol_index) {
        const unsigned char *symbol = symbol_entry(object, symbol_index);

        if (symbol != 0 && read_u16(symbol + 6) == section->index) {
            uint64_t value = read_u64(symbol + 8);

            if (value > section->size || (value % entsize) != 0ULL) {
                return 0;
            }
        }
    }
    for (rela_index = 0; rela_index < object->rela_section_count; ++rela_index) {
        const LinkRelaSection *rela = &object->rela_sections[rela_index];
        uint64_t entry_count;
        uint64_t reloc_index;

        if (rela->entsize == 0ULL) {
            continue;
        }
        entry_count = rela->size / rela->entsize;
        for (reloc_index = 0ULL; reloc_index < entry_count; ++reloc_index) {
            const unsigned char *reloc = object->file + rela->offset + (reloc_index * rela->entsize);
            uint64_t info = read_u64(reloc + 8);
            uint32_t symbol_index_from_reloc = (uint32_t)(info >> 32U);
            uint32_t type = (uint32_t)info;
            const unsigned char *symbol = symbol_entry(object, symbol_index_from_reloc);

            if (symbol != 0 && read_u16(symbol + 6) == section->index) {
                int64_t relocation_bias = (type == R_X86_64_PC32 || type == R_X86_64_PLT32) ? -4LL : 0LL;
                int64_t const_addend = read_i64(reloc + 16) - relocation_bias;
                uint64_t symbol_offset = read_u64(symbol + 8);
                uint64_t input_offset;

                if (const_addend < 0 || (uint64_t)const_addend > section->size || symbol_offset > section->size - (uint64_t)const_addend) {
                    return 0;
                }
                input_offset = symbol_offset + (uint64_t)const_addend;
                if (input_offset >= section->size || (input_offset % entsize) != 0ULL) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int section_is_mergeable_const(const LinkObject *object, const LinkSection *section) {
    uint64_t entsize = section_entry_size(object, section);

    return section->live && !section->folded && section->kind == LINK_SECTION_TEXT && section->type == SHT_PROGBITS &&
           (section->flags & SHF_EXECINSTR) == 0ULL && (section->flags & SHF_MERGE) != 0ULL && (section->flags & SHF_STRINGS) == 0ULL &&
           entsize > 1ULL && section->size != 0ULL && (section->size % entsize) == 0ULL && !section_has_relocations(object, section->index) &&
           merge_const_section_references_supported(object, section, entsize);
}
#endif

int section_uses_merge_string_pool(const LinkSection *section) {
    return linker_merge_string_pool_active && (section->flags & SHF_MERGE) != 0ULL && (section->flags & SHF_STRINGS) != 0ULL &&
           ((section->folded && section->fold_object_index == linker_merge_master_object_index && section->fold_section_index == linker_merge_master_section_index) ||
            (!section->folded && section == &linker_objects[linker_merge_master_object_index].sections[linker_merge_master_section_index]));
}

uint64_t merge_string_input_size(const LinkSection *section) {
    if (linker_merge_string_pool_active && section == &linker_objects[linker_merge_master_object_index].sections[linker_merge_master_section_index]) {
        return linker_merge_master_input_size;
    }
    return section->size;
}

#if COMPILER_LINKER_ENABLE_CONST_MERGE
int section_uses_merge_const_pool(const LinkSection *section) {
    return linker_merge_const_pool_active && (section->flags & SHF_MERGE) != 0ULL && (section->flags & SHF_STRINGS) == 0ULL &&
           ((section->folded && section->fold_object_index == linker_merge_const_master_object_index && section->fold_section_index == linker_merge_const_master_section_index) ||
            (!section->folded && section == &linker_objects[linker_merge_const_master_object_index].sections[linker_merge_const_master_section_index]));
}

uint64_t merge_const_input_size(const LinkSection *section) {
    if (linker_merge_const_pool_active && section == &linker_objects[linker_merge_const_master_object_index].sections[linker_merge_const_master_section_index]) {
        return linker_merge_const_master_input_size;
    }
    return section->size;
}
#endif

static uint64_t merge_string_length_at(const LinkObject *object, const LinkSection *section, uint64_t offset) {
    uint64_t length = 0ULL;
    uint64_t input_size = merge_string_input_size(section);

    if (offset >= input_size || section->type == SHT_NOBITS) {
        return LINKER_UNPLACED_OFFSET;
    }
    while (offset + length < input_size) {
        if (object->file[section->offset + offset + length] == 0U) {
            return length + 1ULL;
        }
        length += 1ULL;
    }
    return LINKER_UNPLACED_OFFSET;
}

static int merge_string_pool_find(const unsigned char *bytes, uint64_t length, uint64_t *offset_out) {
    uint64_t offset;

    if (length == 0ULL || linker_merge_string_pool_size < length) {
        return 0;
    }
    for (offset = 0ULL; offset + length <= linker_merge_string_pool_size; ++offset) {
        if (memcmp(linker_merge_string_pool + offset, bytes, (size_t)length) == 0) {
            *offset_out = offset;
            return 1;
        }
    }
    return 0;
}

static int merge_string_pool_append(const unsigned char *bytes, uint64_t length, uint64_t *offset_out, char *error_out, size_t error_size) {
    uint64_t offset;

    if (merge_string_pool_find(bytes, length, &offset)) {
        *offset_out = offset;
        return 0;
    }
    if (linker_merge_string_pool_size + length > linker_merge_string_pool_capacity) {
        uint64_t next_capacity = linker_merge_string_pool_capacity != 0ULL ? linker_merge_string_pool_capacity : 256ULL;
        unsigned char *resized;

        while (next_capacity < linker_merge_string_pool_size + length) {
            next_capacity *= 2ULL;
        }
        resized = (unsigned char *)rt_realloc(linker_merge_string_pool, (size_t)next_capacity);
        if (resized == 0) {
            set_link_error(error_out, error_size, "failed to allocate merge string pool", "");
            return -1;
        }
        linker_merge_string_pool = resized;
        linker_merge_string_pool_capacity = next_capacity;
    }
    offset = linker_merge_string_pool_size;
    memcpy(linker_merge_string_pool + offset, bytes, (size_t)length);
    linker_merge_string_pool_size += length;
    *offset_out = offset;
    return 0;
}

#if COMPILER_LINKER_ENABLE_CONST_MERGE
static int merge_const_pool_find(const unsigned char *bytes, uint64_t length, uint64_t alignment, uint64_t *offset_out) {
    uint64_t offset;

    if (length == 0ULL || linker_merge_const_pool_size < length) {
        return 0;
    }
    if (alignment == 0ULL) {
        alignment = 1ULL;
    }
    for (offset = 0ULL; offset + length <= linker_merge_const_pool_size; ++offset) {
        if ((offset % alignment) == 0ULL && memcmp(linker_merge_const_pool + offset, bytes, (size_t)length) == 0) {
            *offset_out = offset;
            return 1;
        }
    }
    return 0;
}

static int merge_const_pool_append(const unsigned char *bytes, uint64_t length, uint64_t alignment, uint64_t *offset_out, char *error_out, size_t error_size) {
    uint64_t offset;

    if (alignment == 0ULL) {
        alignment = 1ULL;
    }
    if (merge_const_pool_find(bytes, length, alignment, &offset)) {
        *offset_out = offset;
        return 0;
    }
    offset = align_u64(linker_merge_const_pool_size, alignment);
    if (offset + length > linker_merge_const_pool_capacity) {
        uint64_t next_capacity = linker_merge_const_pool_capacity != 0ULL ? linker_merge_const_pool_capacity : 128ULL;
        unsigned char *resized;

        while (next_capacity < offset + length) {
            next_capacity *= 2ULL;
        }
        resized = (unsigned char *)rt_realloc(linker_merge_const_pool, (size_t)next_capacity);
        if (resized == 0) {
            set_link_error(error_out, error_size, "failed to allocate merge constant pool", "");
            return -1;
        }
        linker_merge_const_pool = resized;
        linker_merge_const_pool_capacity = next_capacity;
    }
    while (linker_merge_const_pool_size < offset) {
        linker_merge_const_pool[linker_merge_const_pool_size++] = 0U;
    }
    memcpy(linker_merge_const_pool + offset, bytes, (size_t)length);
    linker_merge_const_pool_size = offset + length;
    *offset_out = offset;
    return 0;
}
#endif

int translate_merge_string_offset(const LinkObject *object, const LinkSection *section, uint64_t input_offset, uint64_t *output_offset_out) {
    uint64_t length = merge_string_length_at(object, section, input_offset);

    if (length == LINKER_UNPLACED_OFFSET || !linker_merge_string_pool_active) {
        return 0;
    }
    return merge_string_pool_find(object->file + section->offset + input_offset, length, output_offset_out);
}

#if COMPILER_LINKER_ENABLE_CONST_MERGE
int translate_merge_const_offset(const LinkObject *object, const LinkSection *section, uint64_t input_offset, uint64_t *output_offset_out) {
    uint64_t entsize = section_entry_size(object, section);
    uint64_t input_size = merge_const_input_size(section);

    if (!linker_merge_const_pool_active || entsize <= 1ULL || input_offset >= input_size || (input_offset % entsize) != 0ULL) {
        return 0;
    }
    return merge_const_pool_find(object->file + section->offset + input_offset, entsize, section->input_align, output_offset_out);
}
#endif

static int append_merge_string_record(LinkMergeStringRecord **records,
                                      size_t *count,
                                      size_t *capacity,
                                      size_t object_index,
                                      size_t section_index,
                                      uint64_t offset,
                                      uint64_t length,
                                      char *error_out,
                                      size_t error_size) {
    if (*count == *capacity) {
        size_t next_capacity = *capacity != 0U ? *capacity * 2U : 256U;
        LinkMergeStringRecord *resized = (LinkMergeStringRecord *)rt_realloc(*records, next_capacity * sizeof(**records));

        if (resized == 0) {
            set_link_error(error_out, error_size, "failed to allocate merge string records", "");
            return -1;
        }
        *records = resized;
        *capacity = next_capacity;
    }
    (*records)[*count].object_index = object_index;
    (*records)[*count].section_index = section_index;
    (*records)[*count].offset = offset;
    (*records)[*count].length = length;
    (*records)[*count].emitted = 0;
    *count += 1U;
    return 0;
}

int merge_string_sections(LinkObject *objects, size_t object_count, char *error_out, size_t error_size) {
    LinkMergeStringRecord *records = 0;
    size_t record_count = 0U;
    size_t record_capacity = 0U;
    size_t i;
    int have_master = 0;

    reset_merge_string_pool();
    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            LinkSection *section = &objects[i].sections[section_index];
            uint64_t offset;

            if (!section_is_mergeable_string(&objects[i], section)) {
                continue;
            }
            if (!have_master) {
                linker_merge_master_object_index = i;
                linker_merge_master_section_index = section_index;
                linker_merge_master_input_size = section->size;
                have_master = 1;
            }
            for (offset = 0ULL; offset < section->size;) {
                uint64_t length = merge_string_length_at(&objects[i], section, offset);

                if (length == LINKER_UNPLACED_OFFSET) {
                    rt_free(records);
                    reset_merge_string_pool();
                    return 0;
                }
                if (append_merge_string_record(&records, &record_count, &record_capacity, i, section_index, offset, length, error_out, error_size) != 0) {
                    rt_free(records);
                    reset_merge_string_pool();
                    return -1;
                }
                offset += length;
            }
        }
    }
    if (!have_master) {
        rt_free(records);
        return 0;
    }
    for (;;) {
        LinkMergeStringRecord *best = 0;
        size_t record_index;
        uint64_t merged_offset;

        for (record_index = 0U; record_index < record_count; ++record_index) {
            if (!records[record_index].emitted && (best == 0 || records[record_index].length > best->length)) {
                best = &records[record_index];
            }
        }
        if (best == 0) {
            break;
        }
        if (merge_string_pool_append(objects[best->object_index].file + objects[best->object_index].sections[best->section_index].offset + best->offset, best->length, &merged_offset, error_out, error_size) != 0) {
            rt_free(records);
            reset_merge_string_pool();
            return -1;
        }
        best->emitted = 1;
    }
    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            LinkSection *section = &objects[i].sections[section_index];

            if (!section_is_mergeable_string(&objects[i], section)) {
                continue;
            }
            if (i == linker_merge_master_object_index && section_index == linker_merge_master_section_index) {
                section->size = linker_merge_string_pool_size;
                rt_copy_string(section->why, sizeof(section->why), "merge string pool");
            } else {
                section->folded = 1;
                section->fold_object_index = linker_merge_master_object_index;
                section->fold_section_index = linker_merge_master_section_index;
                section->fold_addend = 0ULL;
                rt_copy_string(section->why, sizeof(section->why), "merge string folded");
            }
        }
    }
    linker_merge_string_pool_active = 1;
    rt_free(records);
    return 0;
}

#if COMPILER_LINKER_ENABLE_CONST_MERGE
int merge_const_sections(LinkObject *objects, size_t object_count, char *error_out, size_t error_size) {
    size_t i;
    int have_master = 0;

    reset_merge_const_pool();
    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            LinkSection *section = &objects[i].sections[section_index];
            uint64_t entsize;
            uint64_t offset;

            if (!section_is_mergeable_const(&objects[i], section)) {
                continue;
            }
            entsize = section_entry_size(&objects[i], section);
            if (!have_master) {
                linker_merge_const_master_object_index = i;
                linker_merge_const_master_section_index = section_index;
                linker_merge_const_master_input_size = section->size;
                have_master = 1;
            }
            if (section->align > objects[linker_merge_const_master_object_index].sections[linker_merge_const_master_section_index].align) {
                objects[linker_merge_const_master_object_index].sections[linker_merge_const_master_section_index].align = section->align;
            }
            for (offset = 0ULL; offset < section->size; offset += entsize) {
                uint64_t merged_offset;

                if (merge_const_pool_append(objects[i].file + section->offset + offset, entsize, section->align, &merged_offset, error_out, error_size) != 0) {
                    reset_merge_const_pool();
                    return -1;
                }
            }
        }
    }
    if (!have_master) {
        return 0;
    }
    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            LinkSection *section = &objects[i].sections[section_index];

            if (!section_is_mergeable_const(&objects[i], section)) {
                continue;
            }
            if (i == linker_merge_const_master_object_index && section_index == linker_merge_const_master_section_index) {
                section->size = linker_merge_const_pool_size;
                rt_copy_string(section->why, sizeof(section->why), "merge constant pool");
            } else {
                section->folded = 1;
                section->fold_object_index = linker_merge_const_master_object_index;
                section->fold_section_index = linker_merge_const_master_section_index;
                section->fold_addend = 0ULL;
                rt_copy_string(section->why, sizeof(section->why), "merge constant folded");
            }
        }
    }
    linker_merge_const_pool_active = 1;
    return 0;
}
#endif
