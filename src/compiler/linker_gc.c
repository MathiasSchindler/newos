#include "linker_internal.h"

static int find_defined_global_object(LinkObject *objects, size_t object_count, const char *name) {
    int indexed = find_defined_symbol_owner(name);
    size_t i;

    if (indexed >= 0) {
        return indexed;
    }

    for (i = 0; i < object_count; ++i) {
        LinkObject *object = &objects[i];
        uint32_t count = (uint32_t)(object->symtab_size / object->symtab_entsize);
        uint32_t symbol_index;

        for (symbol_index = 0; symbol_index < count; ++symbol_index) {
            const unsigned char *symbol = symbol_entry(object, symbol_index);
            if (symbol == 0) {
                continue;
            }
            if ((symbol[4] >> 4U) == STB_GLOBAL && read_u16(symbol + 6) != SHN_UNDEF && rt_strcmp(symbol_name(object, symbol), name) == 0) {
                return (int)i;
            }
        }
    }
    return -1;
}

static int find_defined_global_symbol(LinkObject *objects,
                                      size_t object_count,
                                      const char *name,
                                      size_t *object_index_out,
                                      const unsigned char **symbol_out) {
    int owner_index = find_defined_global_object(objects, object_count, name);
    uint32_t count;
    uint32_t symbol_index;
    LinkObject *object;

    if (owner_index < 0) {
        return -1;
    }
    object = &objects[owner_index];
    count = (uint32_t)(object->symtab_size / object->symtab_entsize);
    for (symbol_index = 0; symbol_index < count; ++symbol_index) {
        const unsigned char *symbol = symbol_entry(object, symbol_index);

        if (symbol == 0) {
            continue;
        }
        if ((symbol[4] >> 4U) == STB_GLOBAL && read_u16(symbol + 6) != SHN_UNDEF && rt_strcmp(symbol_name(object, symbol), name) == 0) {
            *object_index_out = (size_t)owner_index;
            *symbol_out = symbol;
            return 0;
        }
    }
    return -1;
}

static int mark_symbol_section_live(LinkObject *objects,
                                    size_t object_count,
                                    size_t object_index,
                                    const unsigned char *symbol,
                                    const char *reason,
                                    size_t parent_object_index,
                                    size_t parent_section_index,
                                    int *changed_out,
                                    size_t *queue_objects,
                                    size_t *queue_sections,
                                    size_t queue_capacity,
                                    size_t *queue_tail,
                                    char *error_out,
                                    size_t error_size) {
    LinkObject *object = &objects[object_index];
    uint16_t shndx = read_u16(symbol + 6);
    LinkSection *section;
    const char *name;
    const unsigned char *definition;
    size_t definition_object_index;

    if (shndx == SHN_ABS) {
        return 0;
    }
    if (shndx == SHN_UNDEF) {
        name = symbol_name(object, symbol);
        if (find_defined_global_symbol(objects, object_count, name, &definition_object_index, &definition) != 0) {
            set_link_error(error_out, error_size, "undefined symbol", name);
            return -1;
        }
        return mark_symbol_section_live(objects, object_count, definition_object_index, definition, name, parent_object_index, parent_section_index, changed_out, queue_objects, queue_sections, queue_capacity, queue_tail, error_out, error_size);
    }
    section = find_link_section(object, shndx);
    if (section == 0) {
        set_link_error(error_out, error_size, "unsupported symbol section", object->path);
        return -1;
    }
    object->live = 1;
    if (!section->live) {
        section->live = 1;
        rt_copy_string(section->why, sizeof(section->why), reason != 0 && reason[0] != '\0' ? reason : symbol_name(object, symbol));
        section->parent_object_index = parent_object_index;
        section->parent_section_index = parent_section_index;
        if (changed_out != 0) {
            *changed_out = 1;
        }
        if (queue_objects != 0 && queue_sections != 0 && queue_tail != 0) {
            if (*queue_tail >= queue_capacity) {
                set_link_error(error_out, error_size, "too many live sections for native linker", object->path);
                return -1;
            }
            queue_objects[*queue_tail] = object_index;
            queue_sections[*queue_tail] = (size_t)(section - object->sections);
            *queue_tail += 1U;
        }
    }
    return 0;
}

static int mark_relocation_dependencies(LinkObject *objects,
                                        size_t object_count,
                                        LinkObject *object,
                                        uint64_t rela_offset,
                                        uint64_t rela_size,
                                        uint64_t rela_entsize,
                                        size_t *queue,
                                        size_t *queue_tail,
                                        int *changed_out,
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
        uint64_t info = read_u64(reloc + 8);
        uint32_t symbol_index = (uint32_t)(info >> 32U);
        const unsigned char *symbol = symbol_entry(object, symbol_index);
        int owner_index;
        const char *name;

        if (symbol == 0) {
            set_link_error(error_out, error_size, "invalid relocation in object", object->path);
            return -1;
        }
        if (read_u16(symbol + 6) != SHN_UNDEF) {
            continue;
        }
        name = symbol_name(object, symbol);
        owner_index = find_defined_global_object(objects, object_count, name);
        if (owner_index < 0) {
            set_link_error(error_out, error_size, "undefined symbol", name);
            return -1;
        }
        if (!objects[owner_index].live) {
            objects[owner_index].live = 1;
            if (queue != 0 && queue_tail != 0 && *queue_tail < LINKER_MAX_OBJECTS) {
                queue[*queue_tail] = (size_t)owner_index;
                *queue_tail += 1U;
            }
            *changed_out = 1;
        }
    }
    return 0;
}

int mark_live_objects(LinkObject *objects, size_t object_count, const char *entry_symbol, char *error_out, size_t error_size) {
    int root_index = find_defined_global_object(objects, object_count, entry_symbol);
    size_t queue[LINKER_MAX_OBJECTS];
    size_t queue_head = 0U;
    size_t queue_tail = 0U;

    if (root_index < 0) {
        set_link_error(error_out, error_size, "undefined entry symbol", entry_symbol);
        return -1;
    }
    objects[root_index].live = 1;
    queue[queue_tail++] = (size_t)root_index;

    while (queue_head < queue_tail) {
        size_t i = queue[queue_head++];
        LinkObject *object = &objects[i];
        size_t rela_index;
        int changed = 0;

        (void)changed;
        if (!object->live) {
            continue;
        }
        for (rela_index = 0; rela_index < object->rela_section_count; ++rela_index) {
            LinkRelaSection *rela = &object->rela_sections[rela_index];

            if (mark_relocation_dependencies(objects, object_count, object, rela->offset, rela->size, rela->entsize, queue, &queue_tail, &changed, error_out, error_size) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

int mark_live_sections(LinkObject *objects, size_t object_count, const char *entry_symbol, char *error_out, size_t error_size) {
    const unsigned char *entry_symbol_entry;
    size_t entry_object_index;
    size_t queue_capacity = 0U;
    size_t *queue_objects;
    size_t *queue_sections;
    size_t queue_head = 0U;
    size_t queue_tail = 0U;
    size_t i;
    int changed;

    for (i = 0; i < object_count; ++i) {
        queue_capacity += objects[i].section_count;
    }
    queue_objects = (size_t *)rt_malloc_array(queue_capacity == 0U ? 1U : queue_capacity, sizeof(*queue_objects));
    queue_sections = (size_t *)rt_malloc_array(queue_capacity == 0U ? 1U : queue_capacity, sizeof(*queue_sections));
    if (queue_objects == 0 || queue_sections == 0) {
        rt_free(queue_objects);
        rt_free(queue_sections);
        set_link_error(error_out, error_size, "failed to allocate linker live-section queue", entry_symbol);
        return -1;
    }

    if (find_defined_global_symbol(objects, object_count, entry_symbol, &entry_object_index, &entry_symbol_entry) != 0) {
        rt_free(queue_objects);
        rt_free(queue_sections);
        set_link_error(error_out, error_size, "undefined entry symbol", entry_symbol);
        return -1;
    }
    changed = 0;
    if (mark_symbol_section_live(objects, object_count, entry_object_index, entry_symbol_entry, "entry", LINKER_NO_INDEX, LINKER_NO_INDEX, &changed, queue_objects, queue_sections, queue_capacity, &queue_tail, error_out, error_size) != 0) {
        rt_free(queue_objects);
        rt_free(queue_sections);
        return -1;
    }

    while (queue_head < queue_tail) {
        size_t object_index = queue_objects[queue_head];
        size_t section_index = queue_sections[queue_head];
        LinkObject *object;
        LinkSection *target;
        const LinkRelaSection *rela;
        uint64_t entry_count;
        uint64_t reloc_index;

        queue_head += 1U;
        if (object_index >= object_count) {
            continue;
        }
        object = &objects[object_index];
        if (section_index >= object->section_count) {
            continue;
        }
        target = &object->sections[section_index];
        rela = find_rela_section_const(object, target->index);
        if (rela == 0 || rela->size == 0ULL) {
            continue;
        }
        entry_count = rela->size / rela->entsize;
        for (reloc_index = 0; reloc_index < entry_count; ++reloc_index) {
            const unsigned char *reloc = object->file + rela->offset + (reloc_index * rela->entsize);
            uint64_t info = read_u64(reloc + 8);
            uint32_t symbol_index = (uint32_t)(info >> 32U);
            uint32_t type = (uint32_t)info;
            const unsigned char *symbol = symbol_entry(object, symbol_index);

            if (symbol == 0) {
                rt_free(queue_objects);
                rt_free(queue_sections);
                set_link_error(error_out, error_size, "invalid relocation in object", object->path);
                return -1;
            }
            if (type == R_X86_64_NONE) {
                continue;
            }
            changed = 0;
            if (mark_symbol_section_live(objects, object_count, object_index, symbol, symbol_name(object, symbol), object_index, section_index, &changed, queue_objects, queue_sections, queue_capacity, &queue_tail, error_out, error_size) != 0) {
                rt_free(queue_objects);
                rt_free(queue_sections);
                return -1;
            }
        }
    }
    rt_free(queue_objects);
    rt_free(queue_sections);
    return 0;
}

void mark_all_sections_in_live_objects(LinkObject *objects, size_t object_count) {
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            objects[i].sections[section_index].live = 1;
            objects[i].sections[section_index].parent_object_index = LINKER_NO_INDEX;
            objects[i].sections[section_index].parent_section_index = LINKER_NO_INDEX;
            rt_copy_string(objects[i].sections[section_index].why, sizeof(objects[i].sections[section_index].why), "live object");
        }
    }
}
