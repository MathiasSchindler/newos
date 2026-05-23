#include "linker_internal.h"

LinkObject linker_objects[LINKER_MAX_OBJECTS];

static const char *linker_entry_symbol(const CompilerLinkerOptions *options) {
    if (options != 0 && options->entry_symbol != 0 && options->entry_symbol[0] != '\0') {
        return options->entry_symbol;
    }
    return "_start";
}

int compiler_link_elf64_x86_64_static_options(const char *const *object_paths,
                                              size_t object_count,
                                              const char *output_path,
                                              const CompilerLinkerOptions *options,
                                              char *error_out,
                                              size_t error_size) {
    uint64_t text_size;
    uint64_t data_size;
    uint64_t bss_size;
    uint64_t text_file_offset;
    uint64_t data_file_offset;
    uint64_t bss_vaddr_offset;
    uint64_t file_size;
    uint64_t memory_size;
    int has_writable_segment;
    uint64_t header_size;
    uint64_t padding_size;
    uint64_t text_alignment;
    uint64_t data_alignment;
    uint64_t bss_alignment;
    uint64_t entry;
    int start_index;
    int fd;
    size_t loaded_object_count = 0U;
    size_t i;
    unsigned char *output;
    int tiny = options != 0 && options->tiny != 0;
    int gc_sections = options != 0 && options->gc_sections != 0;
    const char *entry_symbol = linker_entry_symbol(options);
    const char *lto_cc = (options != 0 && options->lto_cc != 0 && options->lto_cc[0] != '\0') ? options->lto_cc : 0;
    const char *lto_ir_path = 0;
    char lto_prelink_path[COMPILER_PATH_CAPACITY];
    const char *lto_prelink_single[1];
    int did_lto_prelink = 0;
    int lto_ir_found = 0;

    if (error_out != 0 && error_size > 0U) {
        error_out[0] = '\0';
    }
#if !COMPILER_LINKER_ENABLE_REPORTING
    if (options != 0 && (options->stats || options->print_gc_sections || (options->map_path != 0 && options->map_path[0] != '\0') || (options->why_live != 0 && options->why_live[0] != '\0'))) {
        set_link_error(error_out, error_size, "linker reporting is not available in this build", output_path);
        return -1;
    }
#endif
    reset_merge_string_pool();
#if COMPILER_LINKER_ENABLE_CONST_MERGE
    reset_merge_const_pool();
#endif
    if (object_count == 0 || object_count > LINKER_MAX_OBJECTS) {
        set_link_error(error_out, error_size, "invalid object count for native linker", "");
        return -1;
    }
    {
        size_t out_len;
        for (i = 0; i < object_count && !lto_ir_found; ++i) {
            if (!ends_with_text(object_paths[i], ".a")) {
                unsigned char *probe = 0;
                size_t probe_size = 0;
                char probe_err[256];
                if (read_file_alloc(object_paths[i], LINKER_MAX_OBJECT_SIZE, &probe, &probe_size, probe_err, sizeof(probe_err)) == 0) {
                    lto_ir_found = detect_lto_ir(probe, probe_size);
                    if (lto_ir_found) {
                        lto_ir_path = object_paths[i];
                    }
                    rt_free(probe);
                }
            }
        }
        if (lto_ir_found) {
            if (lto_cc == 0) {
                set_link_error(error_out, error_size, "GCC LTO IR object; add --lto-cc=gcc to enable transparent prelink", lto_ir_path != 0 ? lto_ir_path : "");
                return -1;
            }
            out_len = rt_strlen(output_path);
            if (out_len + 16U >= sizeof(lto_prelink_path)) {
                set_link_error(error_out, error_size, "output path too long for LTO prelink temp file", output_path);
                return -1;
            }
            rt_copy_string(lto_prelink_path, sizeof(lto_prelink_path), output_path);
            rt_copy_string(lto_prelink_path + out_len, sizeof(lto_prelink_path) - out_len, ".lto-prelink.o");
            if (run_gcc_lto_prelink(object_paths, object_count, entry_symbol, lto_cc, lto_prelink_path, error_out, error_size) != 0) {
                return -1;
            }
            lto_prelink_single[0] = lto_prelink_path;
            object_paths = lto_prelink_single;
            object_count = 1U;
            did_lto_prelink = 1;
        }
    }
    for (i = 0; i < object_count; ++i) {
        if (ends_with_text(object_paths[i], ".a")) {
            if (load_archive(object_paths[i], linker_objects, &loaded_object_count, error_out, error_size) != 0) {
                return -1;
            }
        } else {
            if (loaded_object_count >= LINKER_MAX_OBJECTS) {
                set_link_error(error_out, error_size, "too many objects for native linker", object_paths[i]);
                return -1;
            }
            if (load_object(&linker_objects[loaded_object_count], object_paths[i], error_out, error_size) != 0) {
                return -1;
            }
            loaded_object_count += 1U;
        }
    }
    object_count = loaded_object_count;
    if (object_count == 0U) {
        set_link_error(error_out, error_size, "invalid object count for native linker", "");
        return -1;
    }
    if (collect_defined_symbol_owners(linker_objects, object_count, error_out, error_size) != 0) {
        return -1;
    }
    if (gc_sections) {
        if (mark_live_sections(linker_objects, object_count, entry_symbol, error_out, error_size) != 0) {
            return -1;
        }
    } else {
        if (mark_live_objects(linker_objects, object_count, entry_symbol, error_out, error_size) != 0) {
            return -1;
        }
        mark_all_sections_in_live_objects(linker_objects, object_count);
    }
    if (merge_string_sections(linker_objects, object_count, error_out, error_size) != 0) {
        return -1;
    }
#if COMPILER_LINKER_ENABLE_CONST_MERGE
    if (merge_const_sections(linker_objects, object_count, error_out, error_size) != 0) {
        return -1;
    }
#endif
    if (options != 0 && options->icf_safe) {
        fold_identical_sections(linker_objects, object_count);
    }

    layout_objects(linker_objects, object_count, &text_size, &data_size, &bss_size);
    has_writable_segment = data_size != 0 || bss_size != 0;
    header_size = ELF64_EHDR_SIZE + ((uint64_t)((tiny || !has_writable_segment) ? 1U : 2U) * ELF64_PHDR_SIZE);
    text_alignment = max_live_section_alignment(linker_objects, object_count, LINK_SECTION_TEXT);
    data_alignment = max_live_section_alignment(linker_objects, object_count, LINK_SECTION_DATA);
    bss_alignment = max_live_section_alignment(linker_objects, object_count, LINK_SECTION_BSS);
    text_file_offset = align_u64(header_size, tiny ? text_alignment : (text_alignment > 16ULL ? text_alignment : 16ULL));
    if (has_writable_segment) {
        data_file_offset = align_u64(text_file_offset + text_size, tiny ? data_alignment : (data_alignment > 0x1000ULL ? data_alignment : 0x1000ULL));
        bss_vaddr_offset = align_u64(data_file_offset + data_size, bss_alignment > 8ULL ? bss_alignment : 8ULL);
        file_size = data_file_offset + data_size;
        memory_size = bss_vaddr_offset + bss_size;
    } else {
        data_file_offset = text_file_offset + text_size;
        bss_vaddr_offset = data_file_offset;
        file_size = data_file_offset;
        memory_size = file_size;
    }
    if (file_size > LINKER_MAX_OUTPUT || memory_size > LINKER_MAX_MEMORY) {
        set_link_error(error_out, error_size, "linked executable exceeds native linker capacity", output_path);
        return -1;
    }
    padding_size = text_file_offset - header_size;
    if (has_writable_segment && data_file_offset > text_file_offset + text_size) {
        padding_size += data_file_offset - (text_file_offset + text_size);
    }
    output = (unsigned char *)rt_malloc((size_t)file_size);
    if (output == 0) {
        set_link_error(error_out, error_size, "failed to allocate linker output", output_path);
        return -1;
    }
    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!linker_objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < linker_objects[i].section_count; ++section_index) {
            LinkSection *section = &linker_objects[i].sections[section_index];

            if (section->kind == LINK_SECTION_TEXT) {
                section->out_offset += text_file_offset;
            } else if (section->kind == LINK_SECTION_DATA) {
                section->out_offset += data_file_offset;
            } else if (section->kind == LINK_SECTION_BSS) {
                section->out_offset += bss_vaddr_offset;
            }
        }
    }

    rt_memset(output, 0, (size_t)file_size);
    copy_sections(linker_objects, object_count, output);
    if (collect_globals(linker_objects, object_count, error_out, error_size) != 0) {
        rt_free(output);
        return -1;
    }
    start_index = linker_find_global(entry_symbol);
    if (start_index < 0) {
        set_link_error(error_out, error_size, "undefined entry symbol", entry_symbol);
        rt_free(output);
        return -1;
    }
    entry = linker_globals[start_index].value;
    if (apply_relocations(linker_objects, object_count, output, error_out, error_size) != 0) {
        rt_free(output);
        return -1;
    }
    file_size = trim_trailing_zero_bytes(output, file_size, (!tiny && has_writable_segment) ? data_file_offset : header_size);
    write_elf_header(output, entry, text_file_offset, text_size, data_file_offset, data_size, bss_size, file_size, memory_size, tiny);

    fd = platform_open_write(output_path, 0755U);
    if (fd < 0) {
        set_link_error(error_out, error_size, "failed to open output executable", output_path);
        rt_free(output);
        return -1;
    }
    if (rt_write_all(fd, output, (size_t)file_size) != 0) {
        (void)platform_close(fd);
        set_link_error(error_out, error_size, "failed to write output executable", output_path);
        rt_free(output);
        return -1;
    }
    if (platform_close(fd) != 0) {
        set_link_error(error_out, error_size, "failed to close output executable", output_path);
        rt_free(output);
        return -1;
    }
    rt_free(output);
    (void)platform_change_mode(output_path, 0755U);
#if COMPILER_LINKER_ENABLE_REPORTING
    if (options != 0 && options->map_path != 0 && options->map_path[0] != '\0') {
        if (write_link_map(options->map_path,
                           linker_objects,
                           object_count,
                           output_path,
                           entry_symbol,
                           entry,
                           text_size,
                           data_size,
                           bss_size,
                           file_size,
                           memory_size,
                           tiny,
                           gc_sections,
                           error_out,
                           error_size) != 0) {
            return -1;
        }
    }
    if (options != 0 && options->stats) {
        if (write_link_stats(1,
                             linker_objects,
                             object_count,
                             text_size,
                             data_size,
                             bss_size,
                             file_size,
                             memory_size,
                             header_size,
                             padding_size,
                             tiny,
                             gc_sections) != 0) {
            set_link_error(error_out, error_size, "failed to write linker stats", output_path);
            return -1;
        }
    }
    if (options != 0 && options->print_gc_sections) {
        if (write_gc_sections(1, linker_objects, object_count) != 0) {
            set_link_error(error_out, error_size, "failed to write discarded section report", output_path);
            return -1;
        }
    }
    if (options != 0 && options->why_live != 0 && options->why_live[0] != '\0') {
        if (write_why_live(1, linker_objects, object_count, options->why_live) != 0) {
            set_link_error(error_out, error_size, "failed to write why-live report", options->why_live);
            return -1;
        }
    }
#endif
    if (did_lto_prelink) {
        platform_remove_file(lto_prelink_path);
    }
    return 0;
}

int compiler_link_elf64_x86_64_static(const char *const *object_paths, size_t object_count, const char *output_path, char *error_out, size_t error_size) {
    return compiler_link_elf64_x86_64_static_options(object_paths, object_count, output_path, 0, error_out, error_size);
}
