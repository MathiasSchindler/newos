#include "linker_internal.h"

#define MACHO64_MAGIC          0xfeedfacfU
#define MACHO_CPU_TYPE_ARM64   0x0100000cU
#define MACHO_FILETYPE_OBJECT  1U
#define MACHO_LC_SEGMENT_64    0x19U
#define MACHO_LC_SYMTAB        0x2U
#define MACHO_HEADER64_SIZE    32U
#define MACHO_SECTION64_SIZE   80U
#define MACHO_NLIST64_SIZE     16U
#define MACHO_RELOC_SIZE       8U

static int looks_like_macho64_object(const unsigned char *file, size_t size) {
    return size >= MACHO_HEADER64_SIZE && read_u32(file) == MACHO64_MAGIC;
}

static int parse_macho64_aarch64_object_boundary(const unsigned char *file, size_t size, const char *path, char *error_out, size_t error_size) {
    uint32_t cputype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint64_t load_offset;
    uint32_t command_index;
    int saw_segment = 0;
    int saw_symtab = 0;
    uint64_t section_count = 0ULL;

    cputype = read_u32(file + 4);
    filetype = read_u32(file + 12);
    ncmds = read_u32(file + 16);
    sizeofcmds = read_u32(file + 20);
    if (cputype != MACHO_CPU_TYPE_ARM64 || filetype != MACHO_FILETYPE_OBJECT) {
        set_link_error(error_out, error_size, "unsupported Mach-O object variant", path);
        return -1;
    }
    if (!range_valid(MACHO_HEADER64_SIZE, sizeofcmds, size)) {
        set_link_error(error_out, error_size, "invalid Mach-O load command table", path);
        return -1;
    }
    load_offset = MACHO_HEADER64_SIZE;
    for (command_index = 0; command_index < ncmds; ++command_index) {
        uint64_t command_offset = load_offset;
        uint32_t command;
        uint32_t command_size;

        if (!range_valid(command_offset, 8U, size) || command_offset + 8U > MACHO_HEADER64_SIZE + sizeofcmds) {
            set_link_error(error_out, error_size, "invalid Mach-O load command", path);
            return -1;
        }
        command = read_u32(file + command_offset);
        command_size = read_u32(file + command_offset + 4U);
        if (command_size < 8U || !range_valid(command_offset, command_size, size) || command_offset + command_size > MACHO_HEADER64_SIZE + sizeofcmds) {
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
            nsects = read_u32(file + command_offset + 64U);
            minimum_command_size = 72ULL + ((uint64_t)nsects * MACHO_SECTION64_SIZE);
            if (command_size < minimum_command_size) {
                set_link_error(error_out, error_size, "invalid Mach-O section table", path);
                return -1;
            }
            saw_segment = 1;
            section_count += nsects;
            for (section_index = 0; section_index < nsects; ++section_index) {
                uint64_t section_offset = command_offset + 72ULL + ((uint64_t)section_index * MACHO_SECTION64_SIZE);
                uint64_t section_size = read_u64(file + section_offset + 40U);
                uint32_t file_offset = read_u32(file + section_offset + 48U);
                uint32_t reloff = read_u32(file + section_offset + 56U);
                uint32_t nreloc = read_u32(file + section_offset + 60U);

                if (file_offset != 0U && !range_valid(file_offset, section_size, size)) {
                    set_link_error(error_out, error_size, "Mach-O section extends past end of file", path);
                    return -1;
                }
                if (nreloc != 0U && !range_valid(reloff, (uint64_t)nreloc * MACHO_RELOC_SIZE, size)) {
                    set_link_error(error_out, error_size, "Mach-O relocation table extends past end of file", path);
                    return -1;
                }
            }
        } else if (command == MACHO_LC_SYMTAB) {
            uint32_t symoff;
            uint32_t nsyms;
            uint32_t stroff;
            uint32_t strsize;

            if (command_size < 24U) {
                set_link_error(error_out, error_size, "invalid Mach-O symbol table command", path);
                return -1;
            }
            symoff = read_u32(file + command_offset + 8U);
            nsyms = read_u32(file + command_offset + 12U);
            stroff = read_u32(file + command_offset + 16U);
            strsize = read_u32(file + command_offset + 20U);
            if (!range_valid(symoff, (uint64_t)nsyms * MACHO_NLIST64_SIZE, size) || !range_valid(stroff, strsize, size)) {
                set_link_error(error_out, error_size, "Mach-O symbol table extends past end of file", path);
                return -1;
            }
            saw_symtab = 1;
        }
        load_offset += command_size;
    }
    if (load_offset != MACHO_HEADER64_SIZE + sizeofcmds || !saw_segment || !saw_symtab || section_count == 0ULL) {
        set_link_error(error_out, error_size, "incomplete Mach-O arm64 relocatable object", path);
        return -1;
    }
    set_link_error(error_out, error_size, "Mach-O arm64 relocatable object is recognized, but Mach-O linking is not implemented yet", path);
    return -1;
}

static int grow_sections(LinkObject *object) {
    size_t new_cap;
    LinkSection *new_sections;

    if (object->section_count < object->section_capacity) {
        return 0;
    }
    new_cap = object->section_capacity == 0U ? 32U : object->section_capacity * 2U;
    new_sections = (LinkSection *)rt_realloc(object->sections, new_cap * sizeof(LinkSection));
    if (new_sections == 0) {
        return -1;
    }
    object->sections = new_sections;
    object->section_capacity = new_cap;
    return 0;
}

static int grow_rela_sections(LinkObject *object) {
    size_t new_cap;
    LinkRelaSection *new_rela;

    if (object->rela_section_count < object->rela_section_capacity) {
        return 0;
    }
    new_cap = object->rela_section_capacity == 0U ? 32U : object->rela_section_capacity * 2U;
    new_rela = (LinkRelaSection *)rt_realloc(object->rela_sections, new_cap * sizeof(LinkRelaSection));
    if (new_rela == 0) {
        return -1;
    }
    object->rela_sections = new_rela;
    object->rela_section_capacity = new_cap;
    return 0;
}

static void remember_section(LinkObject *object, uint16_t index) {
    const unsigned char *section = section_header(object, index);
    uint32_t type;
    uint64_t flags;

    if (section == 0) {
        return;
    }
    type = read_u32(section + 4);
    flags = read_u64(section + 8);
    if ((flags & SHF_ALLOC) != 0ULL && (type == SHT_PROGBITS || type == SHT_NOBITS)) {
        LinkSection *link_section;
        if (grow_sections(object) != 0) {
            return;
        }
        link_section = &object->sections[object->section_count++];

        link_section->index = index;
        link_section->type = type;
        link_section->flags = flags;
        link_section->offset = read_u64(section + 24);
        link_section->size = read_u64(section + 32);
        link_section->align = read_u64(section + 48);
        link_section->input_align = link_section->align;
        link_section->out_offset = 0ULL;
        link_section->live = 0;
        link_section->folded = 0;
        link_section->fold_object_index = 0U;
        link_section->fold_section_index = 0U;
        link_section->fold_addend = 0ULL;
        link_section->parent_object_index = LINKER_NO_INDEX;
        link_section->parent_section_index = LINKER_NO_INDEX;
        link_section->why[0] = '\0';
        if (type == SHT_NOBITS) {
            link_section->kind = LINK_SECTION_BSS;
        } else if ((flags & SHF_WRITE) != 0ULL) {
            link_section->kind = LINK_SECTION_DATA;
        } else {
            link_section->kind = LINK_SECTION_TEXT;
        }
    }
    if (type == SHT_RELA) {
        LinkRelaSection *rela;
        if (grow_rela_sections(object) != 0) {
            return;
        }
        rela = &object->rela_sections[object->rela_section_count++];

        rela->index = index;
        rela->target_index = (uint16_t)read_u32(section + 44);
        rela->offset = read_u64(section + 24);
        rela->size = read_u64(section + 32);
        rela->entsize = read_u64(section + 56);
    }
    if (section_is(object, index, ".text")) {
        object->text_index = index;
        object->text_offset = read_u64(section + 24);
        object->text_size = read_u64(section + 32);
    } else if (section_is(object, index, ".data")) {
        object->data_index = index;
        object->data_offset = read_u64(section + 24);
        object->data_size = read_u64(section + 32);
    } else if (section_is(object, index, ".bss")) {
        object->bss_index = index;
        object->bss_size = read_u64(section + 32);
    } else if (type == SHT_SYMTAB) {
        object->symtab_index = index;
        object->symtab_offset = read_u64(section + 24);
        object->symtab_size = read_u64(section + 32);
        object->symtab_entsize = read_u64(section + 56);
    } else if (type == SHT_STRTAB && section_is(object, index, ".strtab")) {
        object->strtab_index = index;
        object->strtab_offset = read_u64(section + 24);
        object->strtab_size = read_u64(section + 32);
    } else if (type == SHT_RELA && section_is(object, index, ".rela.text")) {
        object->rela_text_index = index;
        object->rela_text_offset = read_u64(section + 24);
        object->rela_text_size = read_u64(section + 32);
        object->rela_text_entsize = read_u64(section + 56);
    } else if (type == SHT_RELA && section_is(object, index, ".rela.data")) {
        object->rela_data_index = index;
        object->rela_data_offset = read_u64(section + 24);
        object->rela_data_size = read_u64(section + 32);
        object->rela_data_entsize = read_u64(section + 56);
    }
}

static int parse_loaded_object(LinkObject *object, const char *path, unsigned char *file, size_t size, char *error_out, size_t error_size) {
    uint16_t section_index;
    size_t link_section_index;
    size_t rela_section_index;

    rt_memset(object, 0, sizeof(*object));
    rt_copy_string(object->path, sizeof(object->path), path);
    object->file = file;
    object->size = size;
    if (looks_like_macho64_object(object->file, object->size)) {
        return parse_macho64_aarch64_object_boundary(object->file, object->size, path, error_out, error_size);
    }
    if (object->size < ELF64_EHDR_SIZE || object->file[0] != 0x7fU || object->file[1] != 'E' || object->file[2] != 'L' || object->file[3] != 'F' ||
        object->file[4] != ELFCLASS64 || object->file[5] != ELFDATA2LSB || read_u16(object->file + 16) != ET_REL || read_u16(object->file + 18) != EM_X86_64) {
        set_link_error(error_out, error_size, "unsupported object format", path);
        return -1;
    }
    object->shoff = read_u64(object->file + 40);
    object->shentsize = read_u16(object->file + 58);
    object->shnum = read_u16(object->file + 60);
    object->shstrndx = read_u16(object->file + 62);
    if (object->shnum == 0 || object->shstrndx >= object->shnum || !range_valid(object->shoff, (uint64_t)object->shnum * object->shentsize, object->size)) {
        set_link_error(error_out, error_size, "invalid section table in object", path);
        return -1;
    }

    for (section_index = 1U; section_index < object->shnum; ++section_index) {
        remember_section(object, section_index);
    }
    if (object->section_count == 0 || object->symtab_index == 0 || object->strtab_index == 0 || object->symtab_entsize < ELF64_SYM_SIZE) {
        for (section_index = 1U; section_index < object->shnum; ++section_index) {
            if (rt_strncmp(section_name(object, section_index), ".gnu.lto_", 9) == 0) {
                object->is_lto_ir = 1;
                set_link_error(error_out, error_size, "GCC LTO IR object; add --lto-cc=gcc to enable transparent prelink", path);
                return -1;
            }
        }
        set_link_error(error_out, error_size, "object is missing required linker sections", path);
        return -1;
    }
    for (link_section_index = 0U; link_section_index < object->section_count; ++link_section_index) {
        if (object->sections[link_section_index].type != SHT_NOBITS && !range_valid(object->sections[link_section_index].offset, object->sections[link_section_index].size, object->size)) {
            set_link_error(error_out, error_size, "object section extends past end of file", path);
            return -1;
        }
    }
    if (!range_valid(object->symtab_offset, object->symtab_size, object->size) ||
        !range_valid(object->strtab_offset, object->strtab_size, object->size)) {
        set_link_error(error_out, error_size, "object section extends past end of file", path);
        return -1;
    }
    for (rela_section_index = 0U; rela_section_index < object->rela_section_count; ++rela_section_index) {
        if (!range_valid(object->rela_sections[rela_section_index].offset, object->rela_sections[rela_section_index].size, object->size)) {
            set_link_error(error_out, error_size, "object section extends past end of file", path);
            return -1;
        }
        if (object->rela_sections[rela_section_index].entsize < ELF64_RELA_SIZE) {
            set_link_error(error_out, error_size, "unsupported relocation entry size", path);
            return -1;
        }
    }
    return 0;
}

int read_file_alloc(const char *path, size_t capacity, unsigned char **file_out, size_t *size_out, char *error_out, size_t error_size) {
    int fd;
    long bytes_read;
    size_t total = 0;
    unsigned char *file;

    fd = platform_open_read(path);
    if (fd < 0) {
        set_link_error(error_out, error_size, "failed to open object", path);
        return -1;
    }
    file = (unsigned char *)rt_malloc(capacity);
    if (file == 0) {
        (void)platform_close(fd);
        set_link_error(error_out, error_size, "failed to allocate linker input buffer", path);
        return -1;
    }
    while ((bytes_read = platform_read(fd, file + total, capacity - total)) > 0) {
        total += (size_t)bytes_read;
        if (total == capacity) {
            break;
        }
    }
    (void)platform_close(fd);
    if (bytes_read < 0) {
        rt_free(file);
        set_link_error(error_out, error_size, "failed to read linker input", path);
        return -1;
    }
    if (total == capacity) {
        rt_free(file);
        set_link_error(error_out, error_size, "linker input exceeds native linker capacity", path);
        return -1;
    }
    *file_out = file;
    *size_out = total;
    return 0;
}

int load_object(LinkObject *object, const char *path, char *error_out, size_t error_size) {
    unsigned char *file;
    size_t size;

    if (read_file_alloc(path, LINKER_MAX_OBJECT_SIZE, &file, &size, error_out, error_size) != 0) {
        return -1;
    }
    return parse_loaded_object(object, path, file, size, error_out, error_size);
}

static unsigned long long parse_ar_decimal_field(const unsigned char *field, size_t field_size) {
    unsigned long long value = 0U;
    size_t i = 0U;

    while (i < field_size && field[i] == ' ') {
        i += 1U;
    }
    while (i < field_size && field[i] >= '0' && field[i] <= '9') {
        value = value * 10U + (unsigned long long)(field[i] - '0');
        i += 1U;
    }
    return value;
}

static void copy_ar_trimmed_name(char *buffer, size_t buffer_size, const unsigned char *field) {
    size_t i = 0U;
    size_t last_non_space = 0U;

    while (i < 16U && i + 1U < buffer_size) {
        buffer[i] = (char)field[i];
        if (field[i] != ' ') {
            last_non_space = i + 1U;
        }
        i += 1U;
    }
    buffer[last_non_space] = '\0';
}

static void copy_ar_string_table_name(char *buffer, size_t buffer_size, const unsigned char *strings, size_t strings_size, size_t offset) {
    size_t i = 0U;

    if (strings == 0 || offset >= strings_size) {
        buffer[0] = '\0';
        return;
    }
    while (offset + i < strings_size && i + 1U < buffer_size) {
        char ch = (char)strings[offset + i];
        if (ch == '/' || ch == '\n') {
            break;
        }
        buffer[i] = ch;
        i += 1U;
    }
    buffer[i] = '\0';
}

int load_archive(const char *path, LinkObject *objects, size_t *object_count, char *error_out, size_t error_size) {
    unsigned char *archive;
    const unsigned char *string_table = 0;
    size_t archive_size;
    size_t string_table_size = 0U;
    size_t offset = 8U;
    size_t loaded = 0U;

    if (read_file_alloc(path, LINKER_MAX_ARCHIVE_SIZE, &archive, &archive_size, error_out, error_size) != 0) {
        return -1;
    }
    if (archive_size < 8U || archive[0] != '!' || archive[1] != '<' || archive[2] != 'a' || archive[3] != 'r' ||
        archive[4] != 'c' || archive[5] != 'h' || archive[6] != '>' || archive[7] != '\n') {
        rt_free(archive);
        set_link_error(error_out, error_size, "unsupported archive format", path);
        return -1;
    }

    while (offset + LINKER_AR_HEADER_SIZE <= archive_size) {
        const unsigned char *header = archive + offset;
        size_t payload_offset = offset + LINKER_AR_HEADER_SIZE;
        unsigned long long payload_size_value = parse_ar_decimal_field(header + 48, 10U);
        size_t payload_size = (size_t)payload_size_value;
        size_t data_offset = payload_offset;
        size_t data_size = payload_size;
        size_t next_offset;
        char member_name[COMPILER_PATH_CAPACITY];
        char object_name[COMPILER_PATH_CAPACITY];

        if (header[58] != '`' || header[59] != '\n' || payload_size_value > (unsigned long long)(archive_size - payload_offset)) {
            rt_free(archive);
            set_link_error(error_out, error_size, "invalid archive member", path);
            return -1;
        }
        next_offset = payload_offset + payload_size + ((payload_size & 1U) != 0U ? 1U : 0U);
        if (next_offset > archive_size) {
            rt_free(archive);
            set_link_error(error_out, error_size, "invalid archive member size", path);
            return -1;
        }

        copy_ar_trimmed_name(member_name, sizeof(member_name), header);
        if (rt_strcmp(member_name, "//") == 0) {
            string_table = archive + payload_offset;
            string_table_size = payload_size;
            offset = next_offset;
            continue;
        }
        if (rt_strcmp(member_name, "/") == 0 || rt_strcmp(member_name, "__.SYMDEF") == 0 || rt_strcmp(member_name, "__.SYMDEF SORTED") == 0) {
            offset = next_offset;
            continue;
        }
        if (member_name[0] == '/' && member_name[1] >= '0' && member_name[1] <= '9') {
            size_t string_offset = 0U;
            size_t i = 1U;
            while (member_name[i] >= '0' && member_name[i] <= '9') {
                string_offset = (string_offset * 10U) + (size_t)(member_name[i] - '0');
                i += 1U;
            }
            copy_ar_string_table_name(member_name, sizeof(member_name), string_table, string_table_size, string_offset);
        } else if (member_name[0] == '#' && member_name[1] == '1' && member_name[2] == '/') {
            unsigned long long name_length_value = parse_ar_decimal_field((const unsigned char *)member_name + 3, rt_strlen(member_name + 3));
            size_t name_length = (size_t)name_length_value;
            size_t copy_length;

            if (name_length_value > payload_size) {
                rt_free(archive);
                set_link_error(error_out, error_size, "invalid archive member name", path);
                return -1;
            }
            copy_length = name_length + 1U < sizeof(member_name) ? name_length : sizeof(member_name) - 1U;
            memcpy(member_name, archive + payload_offset, copy_length);
            member_name[copy_length] = '\0';
            data_offset = payload_offset + name_length;
            data_size = payload_size - name_length;
        }

        if (!ends_with_text(member_name, ".o") && !(data_size >= ELF64_EHDR_SIZE && archive[data_offset] == 0x7fU && archive[data_offset + 1U] == 'E')) {
            offset = next_offset;
            continue;
        }
        if (*object_count >= LINKER_MAX_OBJECTS) {
            rt_free(archive);
            set_link_error(error_out, error_size, "too many objects for native linker", path);
            return -1;
        }
        if (data_size > LINKER_MAX_OBJECT_SIZE) {
            rt_free(archive);
            set_link_error(error_out, error_size, "archive member exceeds native linker capacity", member_name);
            return -1;
        }
        rt_copy_string(object_name, sizeof(object_name), path);
        if (rt_strlen(object_name) + rt_strlen(member_name) + 3U < sizeof(object_name)) {
            size_t used = rt_strlen(object_name);
            object_name[used++] = '(';
            rt_copy_string(object_name + used, sizeof(object_name) - used, member_name);
            used = rt_strlen(object_name);
            object_name[used++] = ')';
            object_name[used] = '\0';
        }
        if (parse_loaded_object(&objects[*object_count], object_name, archive + data_offset, data_size, error_out, error_size) != 0) {
            rt_free(archive);
            return -1;
        }
        *object_count += 1U;
        loaded += 1U;
        offset = next_offset;
    }
    if (loaded == 0U) {
        rt_free(archive);
        set_link_error(error_out, error_size, "archive contains no supported objects", path);
        return -1;
    }
    return 0;
}
