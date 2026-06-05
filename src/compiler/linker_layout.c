#include "linker_internal.h"

static uint64_t section_trailing_zero_bytes(const LinkObject *object, const LinkSection *section) {
    uint64_t count = 0ULL;

    if (section->type == SHT_NOBITS || section->size == 0ULL) {
        return 0ULL;
    }
    while (count < section->size && object->file[section->offset + section->size - count - 1ULL] == 0U) {
        count += 1ULL;
    }
    return count;
}

static int layout_section_is_better(const LinkObject *candidate_object,
                                    const LinkSection *candidate,
                                    const LinkObject *best_object,
                                    const LinkSection *best,
                                    LinkSectionKind kind) {
    if (best == 0) {
        return 1;
    }
    if (candidate->align != best->align) {
        return candidate->align > best->align;
    }
    if (kind == LINK_SECTION_DATA) {
        uint64_t candidate_zero_tail = section_trailing_zero_bytes(candidate_object, candidate);
        uint64_t best_zero_tail = section_trailing_zero_bytes(best_object, best);

        if (candidate_zero_tail != best_zero_tail) {
            return candidate_zero_tail < best_zero_tail;
        }
    }
    return candidate->size > best->size;
}

typedef struct {
    LinkObject *object;
    LinkSection *section;
    size_t object_index;
    size_t section_index;
    uint64_t zero_tail;
} LinkLayoutEntry;

static LinkSectionKind layout_sort_kind;

static int compare_layout_entries(const void *left_ptr, const void *right_ptr) {
    const LinkLayoutEntry *left = (const LinkLayoutEntry *)left_ptr;
    const LinkLayoutEntry *right = (const LinkLayoutEntry *)right_ptr;

    if (left->section->align != right->section->align) {
        return left->section->align > right->section->align ? -1 : 1;
    }
    if (layout_sort_kind == LINK_SECTION_DATA && left->zero_tail != right->zero_tail) {
        return left->zero_tail < right->zero_tail ? -1 : 1;
    }
    if (left->section->size != right->section->size) {
        return left->section->size > right->section->size ? -1 : 1;
    }
    if (left->object_index != right->object_index) {
        return left->object_index < right->object_index ? -1 : 1;
    }
    if (left->section_index != right->section_index) {
        return left->section_index < right->section_index ? -1 : 1;
    }
    return 0;
}

static uint64_t layout_sections_of_kind_slow(LinkObject *objects, size_t object_count, LinkSectionKind kind) {
    uint64_t size = 0ULL;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            LinkSection *section = &objects[i].sections[section_index];

            if (section->live && !section->folded && section->kind == kind) {
                section->out_offset = LINKER_UNPLACED_OFFSET;
            }
        }
    }
    for (;;) {
        LinkSection *best = 0;
        LinkObject *best_object = 0;
        size_t best_object_index = 0U;
        size_t best_section_index = 0U;

        for (i = 0; i < object_count; ++i) {
            size_t section_index;

            if (!objects[i].live) {
                continue;
            }
            for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
                LinkSection *section = &objects[i].sections[section_index];

                if (!section->live || section->folded || section->kind != kind || section->out_offset != LINKER_UNPLACED_OFFSET) {
                    continue;
                }
                if (layout_section_is_better(&objects[i], section, best_object, best, kind)) {
                    best = section;
                    best_object = &objects[i];
                    best_object_index = i;
                    best_section_index = section_index;
                }
            }
        }
        if (best == 0) {
            break;
        }
        (void)best_object_index;
        (void)best_section_index;
        size = align_u64(size, best->align);
        best->out_offset = size;
        size += best->size;
    }
    return size;
}

static uint64_t layout_sections_of_kind(LinkObject *objects, size_t object_count, LinkSectionKind kind) {
    LinkLayoutEntry *entries;
    size_t entry_count = 0U;
    size_t entry_index = 0U;
    size_t i;
    uint64_t size = 0ULL;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            LinkSection *section = &objects[i].sections[section_index];

            if (section->live && !section->folded && section->kind == kind) {
                section->out_offset = LINKER_UNPLACED_OFFSET;
                entry_count += 1U;
            }
        }
    }
    if (entry_count == 0U) {
        return 0ULL;
    }
    entries = (LinkLayoutEntry *)rt_malloc_array(entry_count, sizeof(*entries));
    if (entries == 0) {
        return layout_sections_of_kind_slow(objects, object_count, kind);
    }
    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            LinkSection *section = &objects[i].sections[section_index];

            if (!section->live || section->folded || section->kind != kind) {
                continue;
            }
            entries[entry_index].object = &objects[i];
            entries[entry_index].section = section;
            entries[entry_index].object_index = i;
            entries[entry_index].section_index = section_index;
            entries[entry_index].zero_tail = kind == LINK_SECTION_DATA ? section_trailing_zero_bytes(&objects[i], section) : 0ULL;
            entry_index += 1U;
        }
    }
    layout_sort_kind = kind;
    rt_sort(entries, entry_count, sizeof(*entries), compare_layout_entries);
    for (entry_index = 0U; entry_index < entry_count; ++entry_index) {
        LinkSection *section = entries[entry_index].section;

        size = align_u64(size, section->align);
        section->out_offset = size;
        size += section->size;
    }
    rt_free(entries);
    return size;
}

void layout_objects(LinkObject *objects, size_t object_count, uint64_t *text_size_out, uint64_t *data_size_out, uint64_t *bss_size_out) {
    *text_size_out = layout_sections_of_kind(objects, object_count, LINK_SECTION_TEXT);
    *data_size_out = layout_sections_of_kind(objects, object_count, LINK_SECTION_DATA);
    *bss_size_out = layout_sections_of_kind(objects, object_count, LINK_SECTION_BSS);
}

uint64_t max_live_section_alignment(const LinkObject *objects, size_t object_count, LinkSectionKind kind) {
    uint64_t alignment = 1ULL;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            const LinkSection *section = &objects[i].sections[section_index];

            if (section->live && !section->folded && section->kind == kind && section->align > alignment) {
                alignment = section->align;
            }
        }
    }
    return alignment;
}

void copy_sections(LinkObject *objects, size_t object_count, unsigned char *output) {
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            const LinkSection *section = &objects[i].sections[section_index];

            if (!section->live || section->folded || section->kind == LINK_SECTION_BSS || section->size == 0ULL) {
                continue;
            }
            if (linker_merge_string_pool_active && section == &linker_objects[linker_merge_master_object_index].sections[linker_merge_master_section_index]) {
                memcpy(output + section->out_offset, linker_merge_string_pool, (size_t)section->size);
#if COMPILER_LINKER_ENABLE_CONST_MERGE
            } else if (linker_merge_const_pool_active && section == &linker_objects[linker_merge_const_master_object_index].sections[linker_merge_const_master_section_index]) {
                memcpy(output + section->out_offset, linker_merge_const_pool, (size_t)section->size);
#endif
            } else {
                memcpy(output + section->out_offset, objects[i].file + section->offset, (size_t)section->size);
            }
        }
    }
}

static void write_load_header(unsigned char *output, uint64_t program_header_offset, uint16_t index, uint32_t flags, uint64_t offset, uint64_t file_size, uint64_t memory_size, uint64_t alignment) {
    unsigned char *program = output + program_header_offset + ((uint64_t)index * ELF64_PHDR_SIZE);

    write_u32(program + 0, PT_LOAD);
    write_u32(program + 4, flags);
    write_u64(program + 8, offset);
    write_u64(program + 16, LINKER_BASE_VADDR + offset);
    write_u64(program + 24, LINKER_BASE_VADDR + offset);
    write_u64(program + 32, file_size);
    write_u64(program + 40, memory_size);
    write_u64(program + 48, alignment);
}

void write_elf_header(unsigned char *output,
                      uint64_t entry,
                      uint64_t text_file_offset,
                      uint64_t text_size,
                      uint64_t data_file_offset,
                      uint64_t data_size,
                      uint64_t bss_size,
                      uint64_t file_size,
                      uint64_t memory_size,
                      int tiny) {
    uint16_t program_count = tiny ? 1U : (data_size != 0 || bss_size != 0 ? 2U : 1U);
    uint64_t segment_alignment = tiny ? 1U : 0x1000U;
    uint64_t program_header_offset = tiny ? ELF64_EHDR_SIZE - ELF64_TINY_PHDR_OVERLAP : ELF64_EHDR_SIZE;

    output[0] = 0x7fU;
    output[1] = 'E';
    output[2] = 'L';
    output[3] = 'F';
    output[4] = ELFCLASS64;
    output[5] = ELFDATA2LSB;
    output[6] = EV_CURRENT;
    output[7] = 0U;
    write_u16(output + 16, ET_EXEC);
    write_u16(output + 18, EM_X86_64);
    write_u32(output + 20, EV_CURRENT);
    write_u64(output + 24, entry);
    write_u64(output + 32, program_header_offset);
    write_u64(output + 40, 0U);
    write_u32(output + 48, 0U);
    write_u16(output + 52, ELF64_EHDR_SIZE);
    write_u16(output + 54, ELF64_PHDR_SIZE);
    write_u16(output + 56, program_count);
    write_u16(output + 58, 0U);
    write_u16(output + 60, 0U);
    write_u16(output + 62, 0U);

    if (tiny) {
        uint32_t flags = PF_R | PF_X;
        if (data_size != 0 || bss_size != 0) {
            flags |= PF_W;
        }
        write_load_header(output, program_header_offset, 0U, flags, 0U, file_size, memory_size, segment_alignment);
        return;
    }

    write_load_header(output, program_header_offset, 0U, PF_R | PF_X, 0U, file_size < text_file_offset + text_size ? file_size : text_file_offset + text_size, text_file_offset + text_size, segment_alignment);
    if (program_count > 1U) {
        uint64_t data_file_size = 0ULL;

        if (file_size > data_file_offset) {
            data_file_size = file_size - data_file_offset;
            if (data_file_size > data_size) {
                data_file_size = data_size;
            }
        }
        write_load_header(output, program_header_offset, 1U, PF_R | PF_W, data_file_offset, data_file_size, data_size + bss_size, segment_alignment);
    }
}
