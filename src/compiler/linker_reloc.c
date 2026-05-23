#include "linker_internal.h"

static int symbol_value(const LinkObject *object, const unsigned char *symbol, uint64_t *value_out, char *error_out, size_t error_size) {
    uint16_t shndx = read_u16(symbol + 6);
    uint64_t value = read_u64(symbol + 8);
    const LinkSection *section;
    const char *name;
    int global_index;

    if (shndx == SHN_ABS) {
        *value_out = value;
        return 0;
    }
    if (shndx != SHN_UNDEF) {
        section = find_link_section_const(object, shndx);
        if (section != 0) {
            if (section_uses_merge_string_pool(section)) {
                const LinkSection *master = &linker_objects[linker_merge_master_object_index].sections[linker_merge_master_section_index];
                uint64_t merged_offset;

                if (translate_merge_string_offset(object, section, value, &merged_offset)) {
                    *value_out = LINKER_BASE_VADDR + master->out_offset + merged_offset;
                    return 0;
                }
            }
#if COMPILER_LINKER_ENABLE_CONST_MERGE
            if (section_uses_merge_const_pool(section)) {
                const LinkSection *master = &linker_objects[linker_merge_const_master_object_index].sections[linker_merge_const_master_section_index];
                uint64_t merged_offset;

                if (translate_merge_const_offset(object, section, value, &merged_offset)) {
                    *value_out = LINKER_BASE_VADDR + master->out_offset + merged_offset;
                    return 0;
                }
            }
#endif
            if (section->folded) {
                const LinkSection *folded = &linker_objects[section->fold_object_index].sections[section->fold_section_index];
                *value_out = LINKER_BASE_VADDR + folded->out_offset + section->fold_addend + value;
                return 0;
            }
            *value_out = LINKER_BASE_VADDR + section->out_offset + value;
            return 0;
        }
        set_link_error(error_out, error_size, "unsupported symbol section", object->path);
        return -1;
    }

    name = symbol_name(object, symbol);
    global_index = linker_find_global(name);
    if (global_index < 0) {
        set_link_error(error_out, error_size, "undefined symbol", name);
        return -1;
    }
    *value_out = linker_globals[global_index].value;
    return 0;
}

static int relocation_symbol_value(const LinkObject *object,
                                   const unsigned char *symbol,
                                   uint32_t type,
                                   int64_t addend,
                                   uint64_t *value_out,
                                   int64_t *addend_out,
                                   char *error_out,
                                   size_t error_size) {
    uint16_t shndx = read_u16(symbol + 6);

    *addend_out = addend;
    if (shndx != SHN_UNDEF && shndx != SHN_ABS) {
        const LinkSection *section = find_link_section_const(object, shndx);

        if (section != 0 && section_uses_merge_string_pool(section)) {
            uint64_t symbol_offset = read_u64(symbol + 8);
            uint64_t input_offset;
            uint64_t merged_offset;
            const LinkSection *master = &linker_objects[linker_merge_master_object_index].sections[linker_merge_master_section_index];
            uint64_t input_size = merge_string_input_size(section);
            int64_t relocation_bias = (type == R_X86_64_PC32 || type == R_X86_64_PLT32) ? -4LL : 0LL;
            int64_t string_addend = addend - relocation_bias;

            if (string_addend < 0 || (uint64_t)string_addend > input_size || symbol_offset > input_size - (uint64_t)string_addend) {
                set_link_error(error_out, error_size, "invalid merge string relocation", object->path);
                return -1;
            }
            input_offset = symbol_offset + (uint64_t)string_addend;
            if (!translate_merge_string_offset(object, section, input_offset, &merged_offset)) {
                set_link_error(error_out, error_size, "invalid merge string relocation", object->path);
                return -1;
            }
            *value_out = LINKER_BASE_VADDR + master->out_offset + merged_offset;
            *addend_out = relocation_bias;
            return 0;
        }
#if COMPILER_LINKER_ENABLE_CONST_MERGE
        if (section != 0 && section_uses_merge_const_pool(section)) {
            uint64_t symbol_offset = read_u64(symbol + 8);
            uint64_t input_offset;
            uint64_t merged_offset;
            const LinkSection *master = &linker_objects[linker_merge_const_master_object_index].sections[linker_merge_const_master_section_index];
            uint64_t input_size = merge_const_input_size(section);
            int64_t relocation_bias = (type == R_X86_64_PC32 || type == R_X86_64_PLT32) ? -4LL : 0LL;
            int64_t const_addend = addend - relocation_bias;

            if (const_addend < 0 || (uint64_t)const_addend > input_size || symbol_offset > input_size - (uint64_t)const_addend) {
                set_link_error(error_out, error_size, "invalid merge constant relocation", object->path);
                return -1;
            }
            input_offset = symbol_offset + (uint64_t)const_addend;
            if (!translate_merge_const_offset(object, section, input_offset, &merged_offset)) {
                set_link_error(error_out, error_size, "invalid merge constant relocation", object->path);
                return -1;
            }
            *value_out = LINKER_BASE_VADDR + master->out_offset + merged_offset;
            *addend_out = relocation_bias;
            return 0;
        }
#endif
    }
    return symbol_value(object, symbol, value_out, error_out, error_size);
}

int collect_globals(LinkObject *objects, size_t object_count, char *error_out, size_t error_size) {
    size_t i;

    linker_global_count = 0;
    for (i = 0; i < object_count; ++i) {
        LinkObject *object = &objects[i];
        uint32_t symbol_count = (uint32_t)(object->symtab_size / object->symtab_entsize);
        uint32_t symbol_index;

        if (!object->live) {
            continue;
        }
        for (symbol_index = 0; symbol_index < symbol_count; ++symbol_index) {
            const unsigned char *symbol = symbol_entry(object, symbol_index);
            unsigned int bind;
            uint16_t shndx;
            uint64_t value;

            if (symbol == 0) {
                set_link_error(error_out, error_size, "invalid symbol table", object->path);
                return -1;
            }
            bind = (unsigned int)(symbol[4] >> 4U);
            shndx = read_u16(symbol + 6);
            if (bind != STB_GLOBAL || shndx == SHN_UNDEF) {
                continue;
            }
            if (shndx != SHN_ABS) {
                const LinkSection *section = find_link_section_const(object, shndx);
                if (section == 0 || !section->live) {
                    continue;
                }
            }
            if (symbol_value(object, symbol, &value, error_out, error_size) != 0) {
                return -1;
            }
            if (linker_add_global(symbol_name(object, symbol), value, error_out, error_size) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int apply_relocation_table(LinkObject *object,
                                  uint64_t rela_offset,
                                  uint64_t rela_size,
                                  uint64_t rela_entsize,
                                  uint64_t target_size,
                                  uint64_t out_target_offset,
                                  int is_text,
                                  unsigned char *output,
                                  char *error_out,
                                  size_t error_size) {
    uint64_t entry_count;
    uint64_t reloc_index;

    if (rela_size == 0) {
        return 0;
    }
    entry_count = rela_size / rela_entsize;
    for (reloc_index = 0; reloc_index < entry_count; ++reloc_index) {
        const unsigned char *reloc = object->file + rela_offset + (reloc_index * rela_entsize);
        uint64_t offset = read_u64(reloc + 0);
        uint64_t info = read_u64(reloc + 8);
        int64_t addend = read_i64(reloc + 16);
        uint32_t symbol_index = (uint32_t)(info >> 32U);
        uint32_t type = (uint32_t)info;
        const unsigned char *symbol = symbol_entry(object, symbol_index);
        uint64_t symbol_addr;
        int64_t relocation_addend;
        uint64_t place_addr;
        uint64_t patch_offset;

        if (symbol == 0) {
            set_link_error(error_out, error_size, "invalid relocation in object", object->path);
            return -1;
        }
        if (type == R_X86_64_NONE) {
            continue;
        } else if (type == R_X86_64_PC32 || type == R_X86_64_PLT32) {
            int64_t patched;

            (void)is_text;
            if (offset + 4ULL > target_size) {
                set_link_error(error_out, error_size, "invalid relocation in object", object->path);
                return -1;
            }
            if (relocation_symbol_value(object, symbol, type, addend, &symbol_addr, &relocation_addend, error_out, error_size) != 0) {
                return -1;
            }
            place_addr = LINKER_BASE_VADDR + out_target_offset + offset;
            patched = (int64_t)symbol_addr + relocation_addend - (int64_t)place_addr;
            if (patched < -2147483648LL || patched > 2147483647LL) {
                set_link_error(error_out, error_size, "relocation is out of range", object->path);
                return -1;
            }
            patch_offset = out_target_offset + offset;
            write_u32(output + patch_offset, (uint32_t)(int32_t)patched);
        } else if (type == R_X86_64_32 || type == R_X86_64_32S) {
            int64_t patched;

            if (offset + 4ULL > target_size) {
                set_link_error(error_out, error_size, "invalid relocation in object", object->path);
                return -1;
            }
            if (relocation_symbol_value(object, symbol, type, addend, &symbol_addr, &relocation_addend, error_out, error_size) != 0) {
                return -1;
            }
            patched = (int64_t)symbol_addr + relocation_addend;
            if (type == R_X86_64_32 && (patched < 0 || patched > 4294967295LL)) {
                set_link_error(error_out, error_size, "relocation is out of range", object->path);
                return -1;
            }
            if (type == R_X86_64_32S && (patched < -2147483648LL || patched > 2147483647LL)) {
                set_link_error(error_out, error_size, "relocation is out of range", object->path);
                return -1;
            }
            patch_offset = out_target_offset + offset;
            write_u32(output + patch_offset, (uint32_t)(int32_t)patched);
        } else if (type == R_X86_64_64) {
            uint64_t patched;

            if (offset + 8ULL > target_size) {
                set_link_error(error_out, error_size, "invalid relocation in object", object->path);
                return -1;
            }
            if (relocation_symbol_value(object, symbol, type, addend, &symbol_addr, &relocation_addend, error_out, error_size) != 0) {
                return -1;
            }
            patched = (uint64_t)((int64_t)symbol_addr + relocation_addend);
            patch_offset = out_target_offset + offset;
            write_u64(output + patch_offset, patched);
        } else {
            set_link_error(error_out, error_size, "unsupported x86_64 relocation in object", object->path);
            return -1;
        }
    }
    return 0;
}

int apply_relocations(LinkObject *objects, size_t object_count, unsigned char *output, char *error_out, size_t error_size) {
    size_t i;

    for (i = 0; i < object_count; ++i) {
        LinkObject *object = &objects[i];
        size_t rela_index;

        if (!object->live) {
            continue;
        }
        for (rela_index = 0; rela_index < object->rela_section_count; ++rela_index) {
            LinkRelaSection *rela = &object->rela_sections[rela_index];
            LinkSection *target = find_link_section(object, rela->target_index);

            if (target == 0) {
                set_link_error(error_out, error_size, "relocation targets unsupported section", object->path);
                return -1;
            }
            if (!target->live || target->folded) {
                continue;
            }
            if (target->kind == LINK_SECTION_BSS) {
                continue;
            }
            if (apply_relocation_table(object, rela->offset, rela->size, rela->entsize, target->size, target->out_offset, target->kind == LINK_SECTION_TEXT, output, error_out, error_size) != 0) {
                return -1;
            }
        }
    }
    return 0;
}
