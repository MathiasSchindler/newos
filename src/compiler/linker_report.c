#include "linker_internal.h"

#if COMPILER_LINKER_ENABLE_REPORTING
static int linker_write_hex64(int fd, uint64_t value) {
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

static uint64_t count_live_objects(const LinkObject *objects, size_t object_count) {
    uint64_t count = 0;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        if (objects[i].live) {
            count += 1ULL;
        }
    }
    return count;
}

static uint64_t count_live_sections(const LinkObject *objects, size_t object_count) {
    uint64_t count = 0;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            if (objects[i].sections[section_index].live) {
                count += 1ULL;
            }
        }
    }
    return count;
}

static uint64_t count_folded_section_bytes(const LinkObject *objects, size_t object_count) {
    uint64_t count = 0;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            if (objects[i].sections[section_index].live && objects[i].sections[section_index].folded) {
                count += objects[i].sections[section_index].size;
            }
        }
    }
    return count;
}

static uint64_t count_discarded_section_bytes(const LinkObject *objects, size_t object_count) {
    uint64_t count = 0;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            if (!objects[i].sections[section_index].live) {
                count += objects[i].sections[section_index].size;
            }
        }
    }
    return count;
}

static uint64_t count_discarded_kind_bytes(const LinkObject *objects, size_t object_count, LinkSectionKind kind) {
    uint64_t count = 0ULL;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            const LinkSection *section = &objects[i].sections[section_index];
            if (!section->live && section->kind == kind) {
                count += section->size;
            }
        }
    }
    return count;
}

static void count_fold_reason(const LinkObject *objects, size_t object_count, const char *reason, uint64_t *sections_out, uint64_t *bytes_out) {
    size_t i;

    *sections_out = 0ULL;
    *bytes_out = 0ULL;
    for (i = 0; i < object_count; ++i) {
        size_t section_index;
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            const LinkSection *section = &objects[i].sections[section_index];
            if (section->live && section->folded && rt_strcmp(section->why, reason) == 0) {
                *sections_out += 1ULL;
                *bytes_out += section->size;
            }
        }
    }
}

static uint64_t merge_string_input_bytes(const LinkObject *objects, size_t object_count) {
    uint64_t bytes = 0ULL;
    size_t i;

    if (!linker_merge_string_pool_active) {
        return 0ULL;
    }
    for (i = 0; i < object_count; ++i) {
        size_t section_index;
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            const LinkSection *section = &objects[i].sections[section_index];
            if (rt_strcmp(section->why, "merge string pool") == 0) {
                bytes += linker_merge_master_input_size;
            } else if (rt_strcmp(section->why, "merge string folded") == 0) {
                bytes += section->size;
            }
        }
    }
    return bytes;
}

#if COMPILER_LINKER_ENABLE_CONST_MERGE
static uint64_t merge_const_input_bytes(const LinkObject *objects, size_t object_count) {
    uint64_t bytes = 0ULL;
    size_t i;

    if (!linker_merge_const_pool_active) {
        return 0ULL;
    }
    for (i = 0; i < object_count; ++i) {
        size_t section_index;
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            const LinkSection *section = &objects[i].sections[section_index];
            if (rt_strcmp(section->why, "merge constant pool") == 0) {
                bytes += linker_merge_const_master_input_size;
            } else if (rt_strcmp(section->why, "merge constant folded") == 0) {
                bytes += section->size;
            }
        }
    }
    return bytes;
}
#endif

static uint64_t count_ordered_sections(const LinkObject *objects, size_t object_count) {
    uint64_t count = 0ULL;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            const LinkSection *section = &objects[i].sections[section_index];
            if (section->live && !section->folded && (section->flags & SHF_EXECINSTR) != 0ULL && section->layout_rank != LINKER_UNPLACED_OFFSET) {
                count += 1ULL;
            }
        }
    }
    return count;
}

static uint64_t count_total_sections(const LinkObject *objects, size_t object_count) {
    uint64_t count = 0;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        count += (uint64_t)objects[i].section_count;
    }
    return count;
}

static uint64_t count_live_relocations(const LinkObject *objects, size_t object_count) {
    uint64_t count = 0;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t rela_index;

        for (rela_index = 0; rela_index < objects[i].rela_section_count; ++rela_index) {
            const LinkRelaSection *rela = &objects[i].rela_sections[rela_index];
            const LinkSection *target = find_link_section_const(&objects[i], rela->target_index);

            if (target != 0 && target->live && rela->entsize != 0) {
                count += rela->size / rela->entsize;
            }
        }
    }
    return count;
}

int write_link_stats(int fd,
                     LinkObject *objects,
                     size_t object_count,
                     uint64_t text_size,
                     uint64_t data_size,
                     uint64_t bss_size,
                     uint64_t file_size,
                     uint64_t memory_size,
                     uint64_t header_size,
                     uint64_t padding_size,
                     uint64_t text_file_offset,
                     uint64_t data_file_offset,
                     int tiny,
                     int gc_sections,
                     int call_graph_order,
                     int symbol_ordering,
                     int profile_ordering,
                     int has_writable_segment) {
    uint64_t exact_sections;
    uint64_t exact_bytes;
    uint64_t suffix_sections;
    uint64_t suffix_bytes;
    uint64_t equivalence_sections;
    uint64_t equivalence_bytes;
    uint64_t executable_suffix_sections;
    uint64_t executable_suffix_bytes;
    uint64_t short_jump_sites;
    uint64_t short_jump_bytes;
    uint64_t rx_page_slack = 0ULL;
    uint64_t rx_bytes_to_smaller_file = 0ULL;
    uint64_t string_input = merge_string_input_bytes(objects, object_count);
    uint64_t string_output = linker_merge_string_pool_active ? linker_merge_string_pool_size : 0ULL;
#if COMPILER_LINKER_ENABLE_CONST_MERGE
    uint64_t const_input = merge_const_input_bytes(objects, object_count);
    uint64_t const_output = linker_merge_const_pool_active ? linker_merge_const_pool_size : 0ULL;
#else
    uint64_t const_input = 0ULL;
    uint64_t const_output = 0ULL;
#endif

    count_fold_reason(objects, object_count, "identical section folded", &exact_sections, &exact_bytes);
    count_fold_reason(objects, object_count, "read-only suffix folded", &suffix_sections, &suffix_bytes);
    count_fold_reason(objects, object_count, "equivalence-class ICF folded", &equivalence_sections, &equivalence_bytes);
    count_executable_suffix_fold_candidates(objects, object_count, &executable_suffix_sections, &executable_suffix_bytes);
    count_short_jump_relaxation_candidates(objects, object_count, &short_jump_sites, &short_jump_bytes);
    if (!tiny && has_writable_segment && data_file_offset >= text_file_offset + text_size) {
        uint64_t text_end = text_file_offset + text_size;
        rx_page_slack = data_file_offset - text_end;
        if (data_file_offset >= 8192ULL && text_end > data_file_offset - 4096ULL) {
            rx_bytes_to_smaller_file = text_end - (data_file_offset - 4096ULL);
        }
    }
    if (rt_write_cstr(fd, "linker stats\nobjects live/total: ") != 0) return -1;
    if (rt_write_uint(fd, count_live_objects(objects, object_count)) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, (unsigned long long)object_count) != 0) return -1;
    if (rt_write_cstr(fd, "\nsections live/total: ") != 0) return -1;
    if (rt_write_uint(fd, count_live_sections(objects, object_count)) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, count_total_sections(objects, object_count)) != 0) return -1;
    if (rt_write_cstr(fd, "\nrelocations applied: ") != 0) return -1;
    if (rt_write_uint(fd, count_live_relocations(objects, object_count)) != 0) return -1;
    if (rt_write_cstr(fd, "\nfolded/discarded bytes: ") != 0) return -1;
    if (rt_write_uint(fd, count_folded_section_bytes(objects, object_count)) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, count_discarded_section_bytes(objects, object_count)) != 0) return -1;
    if (rt_write_cstr(fd, "\ngc discarded text/data/bss: ") != 0) return -1;
    if (rt_write_uint(fd, count_discarded_kind_bytes(objects, object_count, LINK_SECTION_TEXT)) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, count_discarded_kind_bytes(objects, object_count, LINK_SECTION_DATA)) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, count_discarded_kind_bytes(objects, object_count, LINK_SECTION_BSS)) != 0) return -1;
    if (rt_write_cstr(fd, "\nicf exact sections/bytes: ") != 0) return -1;
    if (rt_write_uint(fd, exact_sections) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, exact_bytes) != 0) return -1;
    if (rt_write_cstr(fd, "\nicf suffix sections/bytes: ") != 0) return -1;
    if (rt_write_uint(fd, suffix_sections) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, suffix_bytes) != 0) return -1;
    if (rt_write_cstr(fd, "\nicf equivalence sections/bytes: ") != 0) return -1;
    if (rt_write_uint(fd, equivalence_sections) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, equivalence_bytes) != 0) return -1;
    if (rt_write_cstr(fd, "\nexecutable suffix candidates/bytes: ") != 0) return -1;
    if (rt_write_uint(fd, executable_suffix_sections) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, executable_suffix_bytes) != 0) return -1;
    if (rt_write_cstr(fd, "\nx86 short-jump candidates/bytes: ") != 0) return -1;
    if (rt_write_uint(fd, short_jump_sites) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, short_jump_bytes) != 0) return -1;
    if (rt_write_cstr(fd, "\nmerge strings input/output/saved: ") != 0) return -1;
    if (rt_write_uint(fd, string_input) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, string_output) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, string_input >= string_output ? string_input - string_output : 0ULL) != 0) return -1;
    if (rt_write_cstr(fd, "\nmerge constants input/output/saved: ") != 0) return -1;
    if (rt_write_uint(fd, const_input) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, const_output) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, const_input >= const_output ? const_input - const_output : 0ULL) != 0) return -1;
    if (rt_write_cstr(fd, "\ntext/data/bss: ") != 0) return -1;
    if (rt_write_uint(fd, text_size) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, data_size) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, bss_size) != 0) return -1;
    if (rt_write_cstr(fd, "\nfile/memory: ") != 0) return -1;
    if (rt_write_uint(fd, file_size) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, memory_size) != 0) return -1;
    if (rt_write_cstr(fd, "\nheaders/padding: ") != 0) return -1;
    if (rt_write_uint(fd, header_size) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, padding_size) != 0) return -1;
    if (rt_write_cstr(fd, "\nrx page slack/bytes-to-smaller-file: ") != 0) return -1;
    if (rt_write_uint(fd, rx_page_slack) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, rx_bytes_to_smaller_file) != 0) return -1;
    if (rt_write_cstr(fd, "\nprofile nodes matched/total: ") != 0) return -1;
    if (rt_write_uint(fd, linker_profile_nodes_matched) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, linker_profile_nodes_total) != 0) return -1;
    if (rt_write_cstr(fd, "\nprofile edges matched/total: ") != 0) return -1;
    if (rt_write_uint(fd, linker_profile_edges_matched) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, linker_profile_edges_total) != 0) return -1;
    if (rt_write_cstr(fd, "\nprofile ordered sections/bytes: ") != 0) return -1;
    if (rt_write_uint(fd, linker_profile_sections_ordered) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, linker_profile_bytes_ordered) != 0) return -1;
    if (rt_write_cstr(fd, "\npolicy: ") != 0) return -1;
    if (rt_write_cstr(fd, tiny ? "tiny" : "page-aligned") != 0) return -1;
    if (rt_write_cstr(fd, gc_sections ? " gc-sections\n" : " object-gc\n") != 0) return -1;
    if (rt_write_cstr(fd, "ordering: ") != 0) return -1;
    if (profile_ordering) {
        if (rt_write_cstr(fd, "profile") != 0) return -1;
    } else if (symbol_ordering) {
        if (rt_write_cstr(fd, "symbol-file") != 0) return -1;
    } else if (call_graph_order) {
        if (rt_write_cstr(fd, "call-graph") != 0) return -1;
    } else if (rt_write_cstr(fd, "alignment-size") != 0) return -1;
    if ((profile_ordering || symbol_ordering) && call_graph_order && rt_write_cstr(fd, "+call-graph") != 0) return -1;
    if (rt_write_cstr(fd, " ordered-sections=") != 0) return -1;
    if (rt_write_uint(fd, (call_graph_order || symbol_ordering || profile_ordering) ? count_ordered_sections(objects, object_count) : 0ULL) != 0) return -1;
    if (rt_write_cstr(fd, "\nsegment permissions: ") != 0) return -1;
    if (rt_write_cstr(fd, tiny && has_writable_segment ? "rwx" : (has_writable_segment ? "rx+rw" : "rx")) != 0) return -1;
    if (rt_write_cstr(fd, "\n") != 0) return -1;
    return 0;
}

int write_link_map(const char *path,
                   const LinkObject *objects,
                   size_t object_count,
                   const char *output_path,
                   const char *entry_symbol,
                   uint64_t entry,
                   uint64_t text_size,
                   uint64_t data_size,
                   uint64_t bss_size,
                   uint64_t file_size,
                   uint64_t memory_size,
                   int tiny,
                   int gc_sections,
                   char *error_out,
                   size_t error_size) {
    int fd = platform_open_write(path, 0644U);
    size_t i;

    if (fd < 0) {
        set_link_error(error_out, error_size, "failed to open map file", path);
        return -1;
    }
    if (rt_write_cstr(fd, "newos linker map\noutput: ") != 0 || rt_write_cstr(fd, output_path) != 0 ||
        rt_write_cstr(fd, "\nentry: ") != 0 || rt_write_cstr(fd, entry_symbol) != 0 || rt_write_cstr(fd, " ") != 0 || linker_write_hex64(fd, entry) != 0 ||
        rt_write_cstr(fd, "\npolicy: ") != 0 || rt_write_cstr(fd, tiny ? "tiny" : "page-aligned") != 0 || rt_write_cstr(fd, gc_sections ? " gc-sections\n" : " object-gc\n") != 0 ||
        rt_write_cstr(fd, "text/data/bss/file/memory: ") != 0 || rt_write_uint(fd, text_size) != 0 || rt_write_cstr(fd, "/") != 0 ||
        rt_write_uint(fd, data_size) != 0 || rt_write_cstr(fd, "/") != 0 || rt_write_uint(fd, bss_size) != 0 || rt_write_cstr(fd, "/") != 0 ||
        rt_write_uint(fd, file_size) != 0 || rt_write_cstr(fd, "/") != 0 || rt_write_uint(fd, memory_size) != 0 || rt_write_cstr(fd, "\n\nLive sections:\n") != 0) {
        (void)platform_close(fd);
        set_link_error(error_out, error_size, "failed to write map file", path);
        return -1;
    }
    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            const LinkSection *section = &objects[i].sections[section_index];

            if (!section->live) {
                continue;
            }
            if (linker_write_hex64(fd, LINKER_BASE_VADDR + (section->folded ? objects[section->fold_object_index].sections[section->fold_section_index].out_offset + section->fold_addend : section->out_offset)) != 0 || rt_write_cstr(fd, " ") != 0 ||
                rt_write_uint(fd, section->size) != 0 || rt_write_cstr(fd, " ") != 0 || rt_write_cstr(fd, section_name(&objects[i], section->index)) != 0 ||
                rt_write_cstr(fd, " ") != 0 || rt_write_cstr(fd, objects[i].path) != 0 || rt_write_cstr(fd, " why=") != 0 ||
                rt_write_cstr(fd, section->why[0] != '\0' ? section->why : "live") != 0 ||
                (section->folded && (rt_write_cstr(fd, " folded-to=") != 0 || rt_write_cstr(fd, objects[section->fold_object_index].path) != 0 || rt_write_cstr(fd, ":") != 0 || rt_write_cstr(fd, section_name(&objects[section->fold_object_index], objects[section->fold_object_index].sections[section->fold_section_index].index)) != 0)) ||
                rt_write_cstr(fd, "\n") != 0) {
                (void)platform_close(fd);
                set_link_error(error_out, error_size, "failed to write map file", path);
                return -1;
            }
        }
    }
    if (platform_close(fd) != 0) {
        set_link_error(error_out, error_size, "failed to close map file", path);
        return -1;
    }
    return 0;
}

static int write_section_label(int fd, const LinkObject *objects, size_t object_index, size_t section_index) {
    const LinkSection *section;

    if (object_index == LINKER_NO_INDEX || section_index == LINKER_NO_INDEX || section_index >= objects[object_index].section_count) {
        return rt_write_cstr(fd, "<root>");
    }
    section = &objects[object_index].sections[section_index];
    if (rt_write_cstr(fd, section_name(&objects[object_index], section->index)) != 0 || rt_write_cstr(fd, " in ") != 0 || rt_write_cstr(fd, objects[object_index].path) != 0) {
        return -1;
    }
    return 0;
}

static int write_live_chain(int fd, const LinkObject *objects, size_t object_count, size_t object_index, size_t section_index) {
    size_t depth;
    size_t max_depth = 0;
    size_t di;

    for (di = 0; di < object_count; ++di) {
        max_depth += objects[di].section_count;
    }
    if (max_depth == 0) {
        max_depth = 1;
    }
    if (rt_write_cstr(fd, "chain: ") != 0) {
        return -1;
    }
    for (depth = 0; depth < max_depth; ++depth) {
        const LinkSection *section;

        if (object_index == LINKER_NO_INDEX || section_index == LINKER_NO_INDEX || object_index >= object_count || section_index >= objects[object_index].section_count) {
            break;
        }
        section = &objects[object_index].sections[section_index];
        if (depth != 0 && rt_write_cstr(fd, " <- ") != 0) {
            return -1;
        }
        if (write_section_label(fd, objects, object_index, section_index) != 0) {
            return -1;
        }
        if (section->parent_object_index == LINKER_NO_INDEX || section->parent_section_index == LINKER_NO_INDEX) {
            break;
        }
        object_index = section->parent_object_index;
        section_index = section->parent_section_index;
    }
    if (rt_write_cstr(fd, "\n") != 0) {
        return -1;
    }
    return 0;
}

int write_gc_sections(int fd, const LinkObject *objects, size_t object_count) {
    size_t i;

    if (rt_write_cstr(fd, "discarded sections\n") != 0) {
        return -1;
    }
    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            const LinkSection *section = &objects[i].sections[section_index];

            if (section->live) {
                continue;
            }
            if (rt_write_uint(fd, section->size) != 0 || rt_write_cstr(fd, " ") != 0 || rt_write_cstr(fd, section_name(&objects[i], section->index)) != 0 ||
                rt_write_cstr(fd, " ") != 0 || rt_write_cstr(fd, objects[i].path) != 0 || rt_write_cstr(fd, "\n") != 0) {
                return -1;
            }
        }
    }
    return 0;
}

int write_why_live(int fd, const LinkObject *objects, size_t object_count, const char *query) {
    int found = 0;
    size_t i;

    if (query == 0 || query[0] == '\0') {
        return 0;
    }
    if (rt_write_cstr(fd, "why-live ") != 0 || rt_write_cstr(fd, query) != 0 || rt_write_cstr(fd, "\n") != 0) {
        return -1;
    }
    for (i = 0; i < object_count; ++i) {
        uint32_t symbol_count = (uint32_t)(objects[i].symtab_size / objects[i].symtab_entsize);
        uint32_t symbol_index;
        size_t section_index;

        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            const LinkSection *section = &objects[i].sections[section_index];

            if (section->live && rt_strcmp(section_name(&objects[i], section->index), query) == 0) {
                found = 1;
                if (rt_write_cstr(fd, "section ") != 0 || rt_write_cstr(fd, query) != 0 || rt_write_cstr(fd, " in ") != 0 ||
                    rt_write_cstr(fd, objects[i].path) != 0 || rt_write_cstr(fd, " because ") != 0 || rt_write_cstr(fd, section->why) != 0 || rt_write_cstr(fd, "\n") != 0) {
                    return -1;
                }
                if (write_live_chain(fd, objects, object_count, i, section_index) != 0) {
                    return -1;
                }
            }
        }
        for (symbol_index = 0; symbol_index < symbol_count; ++symbol_index) {
            const unsigned char *symbol = symbol_entry(&objects[i], symbol_index);
            const LinkSection *section;
            uint16_t shndx;

            if (symbol == 0 || rt_strcmp(symbol_name(&objects[i], symbol), query) != 0) {
                continue;
            }
            shndx = read_u16(symbol + 6);
            section = shndx != SHN_ABS && shndx != SHN_UNDEF ? find_link_section_const(&objects[i], shndx) : 0;
            if (section != 0 && section->live) {
                size_t live_section_index;

                found = 1;
                if (rt_write_cstr(fd, "symbol ") != 0 || rt_write_cstr(fd, query) != 0 || rt_write_cstr(fd, " in ") != 0 ||
                    rt_write_cstr(fd, section_name(&objects[i], section->index)) != 0 || rt_write_cstr(fd, " from ") != 0 ||
                    rt_write_cstr(fd, objects[i].path) != 0 || rt_write_cstr(fd, " because ") != 0 || rt_write_cstr(fd, section->why) != 0 || rt_write_cstr(fd, "\n") != 0) {
                    return -1;
                }
                live_section_index = (size_t)(section - objects[i].sections);
                if (write_live_chain(fd, objects, object_count, i, live_section_index) != 0) {
                    return -1;
                }
            }
        }
    }
    if (!found && rt_write_cstr(fd, "not live\n") != 0) {
        return -1;
    }
    return 0;
}
#endif
