#include "linker_internal.h"

static int sections_have_same_bytes(const LinkObject *left_object, const LinkSection *left, const LinkObject *right_object, const LinkSection *right) {
    if (left->size != right->size || left->type != right->type || left->size == 0ULL) {
        return 0;
    }
    if (left->type == SHT_NOBITS || right->type == SHT_NOBITS) {
        return 0;
    }
    return memcmp(left_object->file + left->offset, right_object->file + right->offset, (size_t)left->size) == 0;
}

static int relocation_symbols_equivalent(const LinkObject *left_object,
                                         const LinkSection *left_section,
                                         const unsigned char *left_symbol,
                                         const LinkObject *right_object,
                                         const LinkSection *right_section,
                                         const unsigned char *right_symbol) {
    uint16_t left_shndx = read_u16(left_symbol + 6);
    uint16_t right_shndx = read_u16(right_symbol + 6);
    unsigned int left_bind = (unsigned int)(left_symbol[4] >> 4U);
    unsigned int right_bind = (unsigned int)(right_symbol[4] >> 4U);

    if (left_shndx == SHN_UNDEF || right_shndx == SHN_UNDEF) {
        return left_shndx == SHN_UNDEF && right_shndx == SHN_UNDEF && rt_strcmp(symbol_name(left_object, left_symbol), symbol_name(right_object, right_symbol)) == 0;
    }
    if (left_shndx == SHN_ABS || right_shndx == SHN_ABS) {
        return left_shndx == SHN_ABS && right_shndx == SHN_ABS && read_u64(left_symbol + 8) == read_u64(right_symbol + 8);
    }
    if (left_bind == STB_GLOBAL || right_bind == STB_GLOBAL) {
        return left_bind == STB_GLOBAL && right_bind == STB_GLOBAL && rt_strcmp(symbol_name(left_object, left_symbol), symbol_name(right_object, right_symbol)) == 0;
    }
    if (left_shndx == left_section->index && right_shndx == right_section->index) {
        return read_u64(left_symbol + 8) == read_u64(right_symbol + 8);
    }
    return 0;
}

static int sections_have_equivalent_relocations(const LinkObject *left_object,
                                                const LinkSection *left,
                                                const LinkObject *right_object,
                                                const LinkSection *right) {
    const LinkRelaSection *left_rela = find_rela_section_const(left_object, left->index);
    const LinkRelaSection *right_rela = find_rela_section_const(right_object, right->index);
    uint64_t left_count;
    uint64_t right_count;
    uint64_t reloc_index;

    if (left_rela == 0 || left_rela->size == 0ULL) {
        return right_rela == 0 || right_rela->size == 0ULL;
    }
    if (right_rela == 0 || right_rela->size == 0ULL || left_rela->entsize == 0ULL || right_rela->entsize == 0ULL) {
        return 0;
    }
    left_count = left_rela->size / left_rela->entsize;
    right_count = right_rela->size / right_rela->entsize;
    if (left_count != right_count) {
        return 0;
    }
    for (reloc_index = 0ULL; reloc_index < left_count; ++reloc_index) {
        const unsigned char *left_reloc = left_object->file + left_rela->offset + (reloc_index * left_rela->entsize);
        const unsigned char *right_reloc = right_object->file + right_rela->offset + (reloc_index * right_rela->entsize);
        uint64_t left_info = read_u64(left_reloc + 8);
        uint64_t right_info = read_u64(right_reloc + 8);
        uint32_t left_symbol_index = (uint32_t)(left_info >> 32U);
        uint32_t right_symbol_index = (uint32_t)(right_info >> 32U);
        const unsigned char *left_symbol = symbol_entry(left_object, left_symbol_index);
        const unsigned char *right_symbol = symbol_entry(right_object, right_symbol_index);

        if (read_u64(left_reloc + 0) != read_u64(right_reloc + 0) || (uint32_t)left_info != (uint32_t)right_info || read_i64(left_reloc + 16) != read_i64(right_reloc + 16)) {
            return 0;
        }
        if (left_symbol == 0 || right_symbol == 0 || !relocation_symbols_equivalent(left_object, left, left_symbol, right_object, right, right_symbol)) {
            return 0;
        }
    }
    return 1;
}

static int section_alignment_satisfies(uint64_t master_align, uint64_t folded_align, uint64_t addend) {
    if (folded_align <= 1ULL) {
        return 1;
    }
    if (master_align <= 1ULL) {
        return 0;
    }
    return master_align >= folded_align && (master_align % folded_align) == 0ULL && (addend % folded_align) == 0ULL;
}

static int section_is_readonly_data(const LinkSection *section) {
    return section->kind == LINK_SECTION_TEXT && (section->flags & SHF_EXECINSTR) == 0ULL && section->type == SHT_PROGBITS;
}

static int sections_have_suffix_bytes(const LinkObject *folded_object,
                                      const LinkSection *folded,
                                      const LinkObject *master_object,
                                      const LinkSection *master,
                                      uint64_t *addend_out) {
    uint64_t addend;

    if (!section_is_readonly_data(folded) || !section_is_readonly_data(master) || folded->size == 0ULL || folded->size >= master->size) {
        return 0;
    }
    if (section_has_relocations(folded_object, folded->index) || section_has_relocations(master_object, master->index)) {
        return 0;
    }
    if (folded->type == SHT_NOBITS || master->type == SHT_NOBITS) {
        return 0;
    }
    addend = master->size - folded->size;
    if (!section_alignment_satisfies(master->align, folded->align, addend)) {
        return 0;
    }
    if (memcmp(folded_object->file + folded->offset, master_object->file + master->offset + addend, (size_t)folded->size) != 0) {
        return 0;
    }
    *addend_out = addend;
    return 1;
}

void fold_identical_sections(LinkObject *objects, size_t object_count) {
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            LinkSection *section = &objects[i].sections[section_index];
            size_t master_object_index;

            if (!section->live || section->folded || section->kind != LINK_SECTION_TEXT) {
                continue;
            }
            for (master_object_index = 0; master_object_index <= i; ++master_object_index) {
                size_t master_section_index;

                for (master_section_index = 0; master_section_index < objects[master_object_index].section_count; ++master_section_index) {
                    LinkSection *master = &objects[master_object_index].sections[master_section_index];

                    if (master_object_index == i && master_section_index >= section_index) {
                        break;
                    }
                    if (!master->live || master->folded || master->kind != LINK_SECTION_TEXT) {
                        continue;
                    }
                    if (sections_have_same_bytes(&objects[i], section, &objects[master_object_index], master) &&
                        section_alignment_satisfies(master->align, section->align, 0ULL) &&
                        sections_have_equivalent_relocations(&objects[i], section, &objects[master_object_index], master)) {
                        section->folded = 1;
                        section->fold_object_index = master_object_index;
                        section->fold_section_index = master_section_index;
                        section->fold_addend = 0ULL;
                        rt_copy_string(section->why, sizeof(section->why), "identical section folded");
                        break;
                    }
                    if (section_is_readonly_data(section) && section_is_readonly_data(master)) {
                        uint64_t fold_addend;

                        if (sections_have_suffix_bytes(&objects[i], section, &objects[master_object_index], master, &fold_addend)) {
                            section->folded = 1;
                            section->fold_object_index = master_object_index;
                            section->fold_section_index = master_section_index;
                            section->fold_addend = fold_addend;
                            rt_copy_string(section->why, sizeof(section->why), "read-only suffix folded");
                            break;
                        }
                    }
                    if (section->folded) {
                        break;
                    }
                }
                if (section->folded) {
                    break;
                }
            }
        }
    }
}
