#include "linker_internal.h"

const unsigned char *section_header(const LinkObject *object, uint16_t index) {
    uint64_t offset;

    if (index == 0 || index >= object->shnum || object->shentsize == 0) {
        return 0;
    }
    offset = object->shoff + (uint64_t)index * object->shentsize;
    if (!range_valid(offset, ELF64_SHDR_SIZE, object->size)) {
        return 0;
    }
    return object->file + offset;
}

const char *section_name(const LinkObject *object, uint16_t index) {
    const unsigned char *shstr_hdr;
    uint64_t shstr_off;
    uint64_t shstr_size;
    const unsigned char *shdr;
    uint32_t name_off;

    if (object->shstrndx == 0 || object->shstrndx >= object->shnum) {
        return "";
    }
    shstr_hdr = object->file + object->shoff + (uint64_t)object->shstrndx * object->shentsize;
    shstr_off = read_u64(shstr_hdr + 24);
    shstr_size = read_u64(shstr_hdr + 32);
    if (shstr_size == 0 || !range_valid(shstr_off, shstr_size, object->size)) {
        return "";
    }
    shdr = section_header(object, index);
    if (shdr == 0) {
        return "";
    }
    name_off = read_u32(shdr + 0);
    if (name_off >= shstr_size) {
        return "";
    }
    return (const char *)(object->file + shstr_off + name_off);
}

int section_is(const LinkObject *object, uint16_t index, const char *name) {
    return rt_strcmp(section_name(object, index), name) == 0;
}

LinkSection *find_link_section(LinkObject *object, uint16_t index) {
    size_t i;

    for (i = 0; i < object->section_count; ++i) {
        if (object->sections[i].index == index) {
            return &object->sections[i];
        }
    }
    return 0;
}

const LinkSection *find_link_section_const(const LinkObject *object, uint16_t index) {
    size_t i;

    for (i = 0; i < object->section_count; ++i) {
        if (object->sections[i].index == index) {
            return &object->sections[i];
        }
    }
    return 0;
}

const unsigned char *symbol_entry(const LinkObject *object, uint32_t index) {
    uint64_t offset;

    if (object->symtab_entsize == 0 || (uint64_t)(index + 1U) * object->symtab_entsize > object->symtab_size) {
        return 0;
    }
    offset = object->symtab_offset + ((uint64_t)index * object->symtab_entsize);
    if (!range_valid(offset, ELF64_SYM_SIZE, object->size)) {
        return 0;
    }
    return object->file + offset;
}

const char *symbol_name(const LinkObject *object, const unsigned char *symbol) {
    uint32_t name_offset = read_u32(symbol + 0);

    if (name_offset >= object->strtab_size) {
        return "";
    }
    return (const char *)(object->file + object->strtab_offset + name_offset);
}

int section_has_relocations(const LinkObject *object, uint16_t section_index) {
    size_t rela_index;

    for (rela_index = 0; rela_index < object->rela_section_count; ++rela_index) {
        if (object->rela_sections[rela_index].target_index == section_index && object->rela_sections[rela_index].size != 0ULL) {
            return 1;
        }
    }
    return 0;
}

const LinkRelaSection *find_rela_section_const(const LinkObject *object, uint16_t section_index) {
    size_t rela_index;

    for (rela_index = 0; rela_index < object->rela_section_count; ++rela_index) {
        if (object->rela_sections[rela_index].target_index == section_index) {
            return &object->rela_sections[rela_index];
        }
    }
    return 0;
}
