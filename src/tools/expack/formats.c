#include "internal.h"

static int expack_validate_elf64_x86_64(const unsigned char *data, size_t size, const char **message_out) {
    unsigned short type;
    unsigned short machine;

    if (size < EXPACK_ELF_HEADER_SIZE) {
        *message_out = "input is too small for an ELF header";
        return -1;
    }
    if (!(data[0] == 0x7fU && data[1] == 'E' && data[2] == 'L' && data[3] == 'F')) {
        *message_out = "input is not an ELF file";
        return -1;
    }
    if (data[4] != 2U || data[5] != 1U || data[6] != 1U) {
        *message_out = "only ELF64 little-endian version-1 files are supported";
        return -1;
    }

    type = expack_read_u16_le(data + 16);
    machine = expack_read_u16_le(data + 18);
    if (!(type == 2U || type == 3U)) {
        *message_out = "only executable and PIE ELF files are supported";
        return -1;
    }
    if (machine != 62U) {
        *message_out = "only x86-64 ELF inputs are supported by these stubs";
        return -1;
    }

    return 0;
}

static int expack_has_macho_magic(const unsigned char *data, size_t size) {
    if (size < 4U) {
        return 0;
    }
    return (data[0] == 0xfeU && data[1] == 0xedU && data[2] == 0xfaU && (data[3] == 0xceU || data[3] == 0xcfU)) ||
           ((data[0] == 0xceU || data[0] == 0xcfU) && data[1] == 0xfaU && data[2] == 0xedU && data[3] == 0xfeU);
}

static int expack_has_pe_coff_magic(const unsigned char *data, size_t size) {
    return size >= 2U && data[0] == 'M' && data[1] == 'Z';
}

static const char *expack_pe_machine_name(unsigned int machine) {
    if (machine == 0x8664U) return "PE/COFF PE32+ x86-64";
    return "PE/COFF";
}

static int expack_validate_pe_coff(const unsigned char *data, size_t size, ExpackInputFormat *format_out, const char **message_out) {
    unsigned int pe_offset;
    unsigned int machine;
    unsigned int section_count;
    unsigned int optional_header_size;
    unsigned int optional_magic;
    unsigned int section_index;
    size_t pe_header_offset;
    size_t optional_header_offset;
    size_t section_table_offset;
    size_t section_table_size;

    if (size < 0x40U) {
        *message_out = "input is too small for a PE/COFF DOS header";
        return -1;
    }
    pe_offset = archive_read_u32_le(data + 0x3cU);
    if (pe_offset > size || 24U > size - pe_offset) {
        *message_out = "invalid PE/COFF header offset";
        return -1;
    }
    pe_header_offset = (size_t)pe_offset;
    if (!(data[pe_header_offset] == 'P' && data[pe_header_offset + 1U] == 'E' && data[pe_header_offset + 2U] == 0U && data[pe_header_offset + 3U] == 0U)) {
        *message_out = "invalid PE/COFF signature";
        return -1;
    }
    machine = expack_read_u16_le(data + pe_header_offset + 4U);
    section_count = expack_read_u16_le(data + pe_header_offset + 6U);
    optional_header_size = expack_read_u16_le(data + pe_header_offset + 20U);
    optional_header_offset = pe_header_offset + 24U;
    if (machine != 0x8664U) {
        *message_out = "only PE32+ x86-64 inputs are supported today";
        return -1;
    }
    if (section_count == 0U || section_count > 96U) {
        *message_out = "invalid PE/COFF section count";
        return -1;
    }
    if (optional_header_size < 112U || optional_header_size > size - optional_header_offset) {
        *message_out = "invalid PE/COFF optional-header range";
        return -1;
    }
    optional_magic = expack_read_u16_le(data + optional_header_offset);
    if (optional_magic != 0x20bU) {
        *message_out = "only PE32+ optional headers are supported today";
        return -1;
    }
    section_table_offset = optional_header_offset + (size_t)optional_header_size;
    section_table_size = (size_t)section_count * 40U;
    if (section_table_offset > size || section_table_size > size - section_table_offset) {
        *message_out = "invalid PE/COFF section-table range";
        return -1;
    }
    for (section_index = 0U; section_index < section_count; ++section_index) {
        size_t section_offset = section_table_offset + (size_t)section_index * 40U;
        unsigned int raw_size = archive_read_u32_le(data + section_offset + 16U);
        unsigned int raw_offset = archive_read_u32_le(data + section_offset + 20U);

        if (raw_size != 0U && ((size_t)raw_offset > size || (size_t)raw_size > size - (size_t)raw_offset)) {
            *message_out = "invalid PE/COFF section raw-data range";
            return -1;
        }
    }

    format_out->kind = EXPACK_FORMAT_PE_COFF;
    format_out->name = expack_pe_machine_name(machine);
    format_out->preprocess_error = "PE/COFF preprocessing failed";
    format_out->allow_x86_bcj = 1;
    format_out->info.pe.machine = machine;
    format_out->info.pe.section_count = section_count;
    format_out->info.pe.entry_rva = archive_read_u32_le(data + optional_header_offset + 16U);
    format_out->info.pe.image_base = archive_read_u64_le(data + optional_header_offset + 24U);
    format_out->info.pe.section_alignment = archive_read_u32_le(data + optional_header_offset + 32U);
    format_out->info.pe.file_alignment = archive_read_u32_le(data + optional_header_offset + 36U);
    format_out->info.pe.size_of_image = archive_read_u32_le(data + optional_header_offset + 56U);
    format_out->info.pe.size_of_headers = archive_read_u32_le(data + optional_header_offset + 60U);
    format_out->info.pe.subsystem = expack_read_u16_le(data + optional_header_offset + 68U);
    return 0;
}

static const char *expack_macho_machine_name(unsigned int cputype) {
    if (cputype == EXPACK_MACHO_CPU_X86_64) return "Mach-O 64-bit x86-64";
    if (cputype == EXPACK_MACHO_CPU_ARM64) return "Mach-O 64-bit arm64";
    return "Mach-O 64-bit";
}

static int expack_validate_macho64_le(const unsigned char *data, size_t size, ExpackInputFormat *format_out, const char **message_out) {
    unsigned int cputype;
    unsigned int filetype;
    unsigned int ncmds;
    unsigned int sizeofcmds;
    unsigned int command_index;
    size_t command_offset = EXPACK_MACHO_HEADER64_SIZE;

    if (size < EXPACK_MACHO_HEADER64_SIZE) {
        *message_out = "input is too small for a Mach-O 64-bit header";
        return -1;
    }
    if (archive_read_u32_le(data) != 0xfeedfacfU) {
        *message_out = "only Mach-O 64-bit little-endian files are supported";
        return -1;
    }
    cputype = archive_read_u32_le(data + 4U);
    filetype = archive_read_u32_le(data + 12U);
    ncmds = archive_read_u32_le(data + 16U);
    sizeofcmds = archive_read_u32_le(data + 20U);
    if (filetype != EXPACK_MACHO_TYPE_EXECUTE) {
        *message_out = "only Mach-O executable files are supported";
        return -1;
    }
    if (!(cputype == EXPACK_MACHO_CPU_X86_64 || cputype == EXPACK_MACHO_CPU_ARM64)) {
        *message_out = "only Mach-O x86-64 and arm64 inputs are supported";
        return -1;
    }
    if (ncmds > 1024U || sizeofcmds > size - EXPACK_MACHO_HEADER64_SIZE) {
        *message_out = "invalid Mach-O load-command table";
        return -1;
    }

    format_out->info.macho.code_signature_offset = 0U;
    format_out->info.macho.code_signature_size = 0U;
    for (command_index = 0U; command_index < ncmds; ++command_index) {
        unsigned int command;
        unsigned int command_size;

        if (command_offset > size || 8U > size - command_offset) {
            *message_out = "invalid Mach-O load-command range";
            return -1;
        }
        command = archive_read_u32_le(data + command_offset);
        command_size = archive_read_u32_le(data + command_offset + 4U);
        if (command_size < 8U || command_size > size - command_offset) {
            *message_out = "invalid Mach-O load-command range";
            return -1;
        }
        if (command == EXPACK_MACHO_LC_CODE_SIGNATURE && command_size >= 16U) {
            format_out->info.macho.code_signature_offset = archive_read_u32_le(data + command_offset + 8U);
            format_out->info.macho.code_signature_size = archive_read_u32_le(data + command_offset + 12U);
            if (format_out->info.macho.code_signature_offset > size || format_out->info.macho.code_signature_size > size - format_out->info.macho.code_signature_offset) {
                *message_out = "invalid Mach-O code-signature range";
                return -1;
            }
        }
        command_offset += (size_t)command_size;
    }
    if (command_offset > EXPACK_MACHO_HEADER64_SIZE + (size_t)sizeofcmds) {
        *message_out = "invalid Mach-O load-command table";
        return -1;
    }

    format_out->kind = EXPACK_FORMAT_MACHO;
    format_out->name = expack_macho_machine_name(cputype);
    format_out->preprocess_error = "Mach-O preprocessing failed";
    format_out->allow_x86_bcj = cputype == EXPACK_MACHO_CPU_X86_64;
    format_out->info.macho.cputype = cputype;
    return 0;
}

static int expack_detect_input_format(const unsigned char *data, size_t size, ExpackInputFormat *format_out, const char **message_out) {
    if (size >= 4U && data[0] == 0x7fU && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        if (expack_validate_elf64_x86_64(data, size, message_out) != 0) {
            return -1;
        }
        format_out->kind = EXPACK_FORMAT_ELF64_X86_64;
        format_out->name = "ELF64 x86-64";
        format_out->preprocess_error = "ELF preprocessing failed";
        format_out->allow_x86_bcj = 1;
        return 0;
    }
    if (expack_has_macho_magic(data, size)) {
        return expack_validate_macho64_le(data, size, format_out, message_out);
    }
    if (expack_has_pe_coff_magic(data, size)) {
        return expack_validate_pe_coff(data, size, format_out, message_out);
    }
    *message_out = "input is not a supported executable format";
    return -1;
}

static int expack_range_end(size_t offset, size_t length, size_t limit, size_t *end_out) {
    if (offset > limit || length > limit - offset) {
        return -1;
    }
    *end_out = offset + length;
    return 0;
}

static void expack_clear_section_header_fields(unsigned char *data, size_t size) {
    if (size >= EXPACK_ELF_HEADER_SIZE) {
        archive_store_u64_le(data + 40U, 0ULL);
        expack_store_u16_le(data + 58U, 0U);
        expack_store_u16_le(data + 60U, 0U);
        expack_store_u16_le(data + 62U, 0U);
    }
}

static unsigned long long expack_align_to_segment_mod(unsigned long long value, unsigned long long vaddr, unsigned long long align) {
    unsigned long long target;
    unsigned long long current;

    if (align <= 1ULL) {
        return value;
    }
    target = vaddr % align;
    current = value % align;
    if (current <= target) {
        return value + (target - current);
    }
    return value + (align - current) + target;
}

static int expack_find_canonical_load_offset(const ExpackLoadRange *loads, unsigned int load_count, unsigned long long old_offset, unsigned long long size, unsigned long long *new_offset_out) {
    unsigned int index;

    for (index = 0U; index < load_count; ++index) {
        unsigned long long load_end = loads[index].old_offset + loads[index].file_size;
        if (old_offset >= loads[index].old_offset && size <= load_end - old_offset) {
            *new_offset_out = loads[index].new_offset + (old_offset - loads[index].old_offset);
            return 0;
        }
    }
    return -1;
}

static int expack_make_elf_exec_image(const unsigned char *input_data, size_t input_size, ExpackImage *image_out) {
    unsigned long long phoff64 = archive_read_u64_le(input_data + 32U);
    unsigned short phentsize = expack_read_u16_le(input_data + 54U);
    unsigned short phnum = expack_read_u16_le(input_data + 56U);
    size_t phoff;
    size_t phdr_size;
    unsigned long long needed_size64 = EXPACK_ELF_HEADER_SIZE;
    unsigned long long canonical_cursor = 0ULL;
    unsigned short index;
    unsigned int load_count = 0U;
    ExpackLoadRange *loads;
    unsigned char *image_data;

    image_out->data = 0;
    image_out->size = 0U;
    image_out->changed = 0;
    if (phoff64 > (unsigned long long)((size_t)-1)) {
        return -1;
    }
    phoff = (size_t)phoff64;
    if (phentsize < EXPACK_ELF_PHDR_SIZE || phnum == 0U || phnum > ((size_t)-1) / (size_t)phentsize) {
        return -1;
    }
    phdr_size = (size_t)phentsize * (size_t)phnum;
    if (phoff > input_size || phdr_size > input_size - phoff) {
        return -1;
    }
    loads = (ExpackLoadRange *)rt_malloc((size_t)phnum * sizeof(*loads));
    if (loads == 0) {
        return -1;
    }

    for (index = 0U; index < phnum; ++index) {
        size_t phdr_offset = phoff + (size_t)index * (size_t)phentsize;
        const unsigned char *phdr = input_data + phdr_offset;
        unsigned int type = archive_read_u32_le(phdr);
        unsigned long long file_offset64 = archive_read_u64_le(phdr + 8U);
        unsigned long long vaddr64 = archive_read_u64_le(phdr + 16U);
        unsigned long long file_size64 = archive_read_u64_le(phdr + 32U);
        unsigned long long align64 = archive_read_u64_le(phdr + 48U);
        unsigned long long new_offset64;

        if (file_offset64 > (unsigned long long)((size_t)-1) || file_size64 > (unsigned long long)((size_t)-1) ||
            (size_t)file_offset64 > input_size || (size_t)file_size64 > input_size - (size_t)file_offset64) {
            rt_free(loads);
            return -1;
        }
        if (type != 1U || file_size64 == 0ULL) {
            continue;
        }
        new_offset64 = expack_align_to_segment_mod(canonical_cursor, vaddr64, align64);
        if (file_size64 > 0xffffffffffffffffULL - new_offset64) {
            rt_free(loads);
            return -1;
        }
        loads[load_count].old_offset = file_offset64;
        loads[load_count].new_offset = new_offset64;
        loads[load_count].file_size = file_size64;
        load_count += 1U;
        canonical_cursor = new_offset64 + file_size64;
        if (canonical_cursor > needed_size64) {
            needed_size64 = canonical_cursor;
        }
    }

    if (needed_size64 > (unsigned long long)((size_t)-1)) {
        rt_free(loads);
        return -1;
    }
    if (phoff > (size_t)needed_size64 || phdr_size > (size_t)needed_size64 - phoff) {
        rt_free(loads);
        return -1;
    }
    image_data = (unsigned char *)rt_malloc((size_t)needed_size64 == 0U ? 1U : (size_t)needed_size64);
    if (image_data == 0) {
        rt_free(loads);
        return -1;
    }
    memset(image_data, 0, (size_t)needed_size64);
    memcpy(image_data, input_data, EXPACK_ELF_HEADER_SIZE);
    memcpy(image_data + phoff, input_data + phoff, phdr_size);
    for (index = 0U; index < phnum; ++index) {
        size_t phdr_offset = phoff + (size_t)index * (size_t)phentsize;
        const unsigned char *input_phdr = input_data + phdr_offset;
        unsigned int type = archive_read_u32_le(input_phdr);
        unsigned long long file_offset64 = archive_read_u64_le(input_phdr + 8U);
        unsigned long long file_size64 = archive_read_u64_le(input_phdr + 32U);
        unsigned long long new_offset64;
        size_t end;

        if (type != 1U || file_size64 == 0ULL) {
            continue;
        }
        if (expack_find_canonical_load_offset(loads, load_count, file_offset64, file_size64, &new_offset64) != 0) {
            continue;
        }
        if (new_offset64 > needed_size64 || file_size64 > needed_size64 - new_offset64 || expack_range_end((size_t)file_offset64, (size_t)file_size64, input_size, &end) != 0) {
            rt_free(loads);
            rt_free(image_data);
            return -1;
        }
        memcpy(image_data + (size_t)new_offset64, input_data + (size_t)file_offset64, (size_t)file_size64);
    }
    for (index = 0U; index < phnum; ++index) {
        size_t phdr_offset = phoff + (size_t)index * (size_t)phentsize;
        unsigned char *image_phdr = image_data + phdr_offset;
        const unsigned char *input_phdr = input_data + phdr_offset;
        unsigned long long file_offset64 = archive_read_u64_le(input_phdr + 8U);
        unsigned long long file_size64 = archive_read_u64_le(input_phdr + 32U);
        unsigned long long new_offset64;

        if (file_size64 != 0ULL && expack_find_canonical_load_offset(loads, load_count, file_offset64, file_size64, &new_offset64) == 0) {
            archive_store_u64_le(image_phdr + 8U, new_offset64);
        }
    }
    expack_clear_section_header_fields(image_data, (size_t)needed_size64);

    image_out->data = image_data;
    image_out->size = (size_t)needed_size64;
    image_out->changed = (size_t)needed_size64 != input_size || memcmp(image_data, input_data, input_size) != 0;
    rt_free(loads);
    return 0;
}

static int expack_make_exact_exec_image(const unsigned char *input_data, size_t input_size, ExpackImage *image_out) {
    image_out->data = (unsigned char *)input_data;
    image_out->size = input_size;
    image_out->changed = 0;
    return 0;
}

static void expack_release_exec_image(ExpackImage *image, const unsigned char *input_data) {
    if (image->data != 0 && image->data != input_data) {
        rt_free(image->data);
    }
    image->data = 0;
    image->size = 0U;
    image->changed = 0;
}

static int expack_make_exec_image(const ExpackInputFormat *format, const unsigned char *input_data, size_t input_size, ExpackImage *image_out) {
    if (format->kind == EXPACK_FORMAT_ELF64_X86_64) {
        return expack_make_elf_exec_image(input_data, input_size, image_out);
    }
    if (format->kind == EXPACK_FORMAT_MACHO) {
        return expack_make_exact_exec_image(input_data, input_size, image_out);
    }
    if (format->kind == EXPACK_FORMAT_PE_COFF) {
        return expack_make_exact_exec_image(input_data, input_size, image_out);
    }
    image_out->data = 0;
    image_out->size = 0U;
    image_out->changed = 0;
    return -1;
}

