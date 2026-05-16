static void expack_write_elf_header(unsigned char *header, unsigned long long file_size) {
    unsigned long long entry = EXPACK_ELF_BASE_VADDR + EXPACK_ELF_CODE_OFFSET;

    memset(header, 0, EXPACK_ELF_CODE_OFFSET);
    header[0] = 0x7fU;
    header[1] = 'E';
    header[2] = 'L';
    header[3] = 'F';
    header[4] = 2U;
    header[5] = 1U;
    header[6] = 1U;
    expack_store_u16_le(header + 16, 2U);
    expack_store_u16_le(header + 18, 62U);
    archive_store_u32_le(header + 20, 1U);
    archive_store_u64_le(header + 24, entry);
    archive_store_u64_le(header + 32, EXPACK_ELF_HEADER_SIZE);
    expack_store_u16_le(header + 52, EXPACK_ELF_HEADER_SIZE);
    expack_store_u16_le(header + 54, EXPACK_ELF_PHDR_SIZE);
    expack_store_u16_le(header + 56, 1U);

    archive_store_u32_le(header + EXPACK_ELF_HEADER_SIZE + 0, 1U);
    archive_store_u32_le(header + EXPACK_ELF_HEADER_SIZE + 4, 5U);
    archive_store_u64_le(header + EXPACK_ELF_HEADER_SIZE + 8, 0ULL);
    archive_store_u64_le(header + EXPACK_ELF_HEADER_SIZE + 16, EXPACK_ELF_BASE_VADDR);
    archive_store_u64_le(header + EXPACK_ELF_HEADER_SIZE + 24, EXPACK_ELF_BASE_VADDR);
    archive_store_u64_le(header + EXPACK_ELF_HEADER_SIZE + 32, file_size);
    archive_store_u64_le(header + EXPACK_ELF_HEADER_SIZE + 40, file_size);
    archive_store_u64_le(header + EXPACK_ELF_HEADER_SIZE + 48, 0x1000ULL);
}

static int expack_candidate_size_offsets(const ExpackCandidate *candidate, unsigned int *original_size_offset_out, unsigned int *payload_size_offset_out) {
    if (candidate->codec == EXPACK_CODEC_LZSS) {
        *original_size_offset_out = EXPACK_LZSS_ORIGINAL_SIZE_OFFSET;
        *payload_size_offset_out = EXPACK_LZSS_PAYLOAD_SIZE_OFFSET;
        return 0;
    }
    if (candidate->codec == EXPACK_CODEC_ZERO_RUN) {
        *original_size_offset_out = EXPACK_ZERO_ORIGINAL_SIZE_OFFSET;
        *payload_size_offset_out = EXPACK_ZERO_PAYLOAD_SIZE_OFFSET;
        return 0;
    }
    if (candidate->codec == EXPACK_CODEC_BYTE_RUN) {
        *original_size_offset_out = EXPACK_BYTE_RUN_ORIGINAL_SIZE_OFFSET;
        *payload_size_offset_out = EXPACK_BYTE_RUN_PAYLOAD_SIZE_OFFSET;
        return 0;
    }
    if (candidate->codec == EXPACK_CODEC_LZREP) {
        *original_size_offset_out = EXPACK_LZREP_ORIGINAL_SIZE_OFFSET;
        *payload_size_offset_out = EXPACK_LZREP_PAYLOAD_SIZE_OFFSET;
        return 0;
    }
    if (candidate->codec == EXPACK_CODEC_LZSS_BCJ) {
        *original_size_offset_out = EXPACK_LZSS_BCJ_ORIGINAL_SIZE_OFFSET;
        *payload_size_offset_out = EXPACK_LZSS_BCJ_PAYLOAD_SIZE_OFFSET;
        return 0;
    }
    return -1;
}

static int expack_patch_stub(unsigned char *stub, const ExpackCandidate *candidate, size_t original_size) {
    unsigned int original_size_offset;
    unsigned int payload_size_offset;

    if (expack_candidate_size_offsets(candidate, &original_size_offset, &payload_size_offset) != 0) {
        return -1;
    }
    if (candidate->codec == EXPACK_CODEC_LZSS) {
        stub[EXPACK_LZSS_STUB_LENGTH_SHIFT_OFFSET] = candidate->lzss_profile->length_shift;
        stub[EXPACK_LZSS_STUB_LENGTH_MASK_OFFSET] = candidate->lzss_profile->length_mask;
    }
    archive_store_u64_le(stub + original_size_offset, (unsigned long long)original_size);
    archive_store_u64_le(stub + payload_size_offset, (unsigned long long)candidate->payload_size);
    return 0;
}

static int expack_write_packed_elf(const char *output_path, const ExpackCandidate *candidate, size_t original_size) {
    unsigned char header[EXPACK_ELF_CODE_OFFSET];
    unsigned char stub[sizeof(expack_lzss_bcj_stub_x86_64)];
    unsigned long long file_size = (unsigned long long)sizeof(header) + (unsigned long long)candidate->stub_size + (unsigned long long)candidate->payload_size;
    int output_fd;

    expack_write_elf_header(header, file_size);
    memcpy(stub, candidate->stub, candidate->stub_size);
    if (expack_patch_stub(stub, candidate, original_size) != 0) {
        return -1;
    }

    output_fd = platform_open_write(output_path, 0755U);
    if (output_fd < 0) {
        return -1;
    }
    if (rt_write_all(output_fd, header, sizeof(header)) != 0 || rt_write_all(output_fd, stub, candidate->stub_size) != 0 ||
        rt_write_all(output_fd, candidate->payload, candidate->payload_size) != 0) {
        platform_close(output_fd);
        return -1;
    }
    if (platform_close(output_fd) != 0) {
        return -1;
    }
    return 0;
}

static void expack_store_pe_section_name(unsigned char *field, const char *name) {
    size_t index = 0U;

    memset(field, 0, 8U);
    while (index < 8U && name[index] != '\0') {
        field[index] = (unsigned char)name[index];
        index += 1U;
    }
}

static void expack_write_pe_section(unsigned char *section, const char *name, unsigned int virtual_size, unsigned int virtual_address, unsigned int raw_size, unsigned int raw_offset, unsigned int characteristics) {
    expack_store_pe_section_name(section, name);
    archive_store_u32_le(section + 8U, virtual_size);
    archive_store_u32_le(section + 12U, virtual_address);
    archive_store_u32_le(section + 16U, raw_size);
    archive_store_u32_le(section + 20U, raw_offset);
    archive_store_u32_le(section + 24U, 0U);
    archive_store_u32_le(section + 28U, 0U);
    expack_store_u16_le(section + 32U, 0U);
    expack_store_u16_le(section + 34U, 0U);
    archive_store_u32_le(section + 36U, characteristics);
}

static void expack_write_pe_container_metadata(unsigned char *metadata, const ExpackInputFormat *format, const ExpackCandidate *candidate, size_t original_size) {
    memset(metadata, 0, EXPACK_PE_CONTAINER_METADATA_SIZE);
    memcpy(metadata, "EXPACKP1", 8U);
    archive_store_u32_le(metadata + 8U, EXPACK_PE_CONTAINER_VERSION);
    archive_store_u32_le(metadata + 12U, candidate->codec);
    archive_store_u64_le(metadata + 16U, (unsigned long long)original_size);
    archive_store_u64_le(metadata + 24U, (unsigned long long)candidate->payload_size);
    archive_store_u32_le(metadata + 32U, format->info.pe.entry_rva);
    archive_store_u32_le(metadata + 36U, format->info.pe.section_count);
    archive_store_u64_le(metadata + 40U, format->info.pe.image_base);
}

static int expack_write_pe_container(const ExpackInputFormat *format, const char *output_path, const ExpackCandidate *candidate, size_t original_size) {
    static const unsigned char text_stub[] = { 0x31U, 0xc0U, 0xc3U };
    unsigned int optional_header_size = 0xf0U;
    unsigned int pe_header_offset = 0x80U;
    unsigned int section_count = 2U;
    unsigned long long expack_virtual_size64 = EXPACK_PE_CONTAINER_METADATA_SIZE + (unsigned long long)candidate->payload_size;
    unsigned long long expack_raw_size64 = expack_align_up_u64(expack_virtual_size64, EXPACK_PE_CONTAINER_FILE_ALIGNMENT);
    unsigned long long expack_raw_offset64 = EXPACK_PE_CONTAINER_TEXT_RAW_OFFSET + EXPACK_PE_CONTAINER_TEXT_RAW_SIZE;
    unsigned long long file_size64 = expack_raw_offset64 + expack_raw_size64;
    unsigned long long size_of_image64 = expack_align_up_u64(EXPACK_PE_CONTAINER_EXPACK_RVA + expack_virtual_size64, EXPACK_PE_CONTAINER_SECTION_ALIGNMENT);
    unsigned int expack_virtual_size;
    unsigned int expack_raw_size;
    unsigned int expack_raw_offset;
    unsigned int file_size;
    unsigned int size_of_image;
    unsigned char *output_data;
    unsigned char *pe;
    unsigned char *optional;
    unsigned char *sections;
    unsigned int subsystem;
    int output_fd;

    if (expack_virtual_size64 > 0xffffffffULL || expack_raw_size64 > 0xffffffffULL || expack_raw_offset64 > 0xffffffffULL ||
        file_size64 > 0xffffffffULL || size_of_image64 > 0xffffffffULL) {
        return -1;
    }
    expack_virtual_size = (unsigned int)expack_virtual_size64;
    expack_raw_size = (unsigned int)expack_raw_size64;
    expack_raw_offset = (unsigned int)expack_raw_offset64;
    file_size = (unsigned int)file_size64;
    size_of_image = (unsigned int)size_of_image64;
    output_data = (unsigned char *)rt_malloc(file_size == 0U ? 1U : file_size);
    if (output_data == 0) {
        return -1;
    }
    memset(output_data, 0, file_size);
    output_data[0] = 'M';
    output_data[1] = 'Z';
    archive_store_u32_le(output_data + 0x3cU, pe_header_offset);
    pe = output_data + pe_header_offset;
    pe[0] = 'P';
    pe[1] = 'E';
    pe[2] = 0U;
    pe[3] = 0U;
    expack_store_u16_le(pe + 4U, 0x8664U);
    expack_store_u16_le(pe + 6U, (unsigned short)section_count);
    archive_store_u32_le(pe + 8U, 0U);
    archive_store_u32_le(pe + 12U, 0U);
    archive_store_u32_le(pe + 16U, 0U);
    expack_store_u16_le(pe + 20U, (unsigned short)optional_header_size);
    expack_store_u16_le(pe + 22U, 0x0022U);

    optional = pe + 24U;
    expack_store_u16_le(optional, 0x20bU);
    optional[2] = 14U;
    optional[3] = 0U;
    archive_store_u32_le(optional + 4U, EXPACK_PE_CONTAINER_TEXT_RAW_SIZE);
    archive_store_u32_le(optional + 8U, expack_raw_size);
    archive_store_u32_le(optional + 12U, 0U);
    archive_store_u32_le(optional + 16U, EXPACK_PE_CONTAINER_TEXT_RVA);
    archive_store_u32_le(optional + 20U, EXPACK_PE_CONTAINER_TEXT_RVA);
    archive_store_u64_le(optional + 24U, EXPACK_PE_CONTAINER_IMAGE_BASE);
    archive_store_u32_le(optional + 32U, EXPACK_PE_CONTAINER_SECTION_ALIGNMENT);
    archive_store_u32_le(optional + 36U, EXPACK_PE_CONTAINER_FILE_ALIGNMENT);
    expack_store_u16_le(optional + 40U, 6U);
    expack_store_u16_le(optional + 42U, 0U);
    expack_store_u16_le(optional + 48U, 6U);
    expack_store_u16_le(optional + 50U, 0U);
    archive_store_u32_le(optional + 56U, size_of_image);
    archive_store_u32_le(optional + 60U, EXPACK_PE_CONTAINER_HEADERS_SIZE);
    subsystem = format->info.pe.subsystem == 0U ? 3U : format->info.pe.subsystem;
    expack_store_u16_le(optional + 68U, (unsigned short)subsystem);
    expack_store_u16_le(optional + 70U, 0x8160U);
    archive_store_u64_le(optional + 72U, 0x100000ULL);
    archive_store_u64_le(optional + 80U, 0x1000ULL);
    archive_store_u64_le(optional + 88U, 0x100000ULL);
    archive_store_u64_le(optional + 96U, 0x1000ULL);
    archive_store_u32_le(optional + 108U, 16U);

    sections = optional + optional_header_size;
    expack_write_pe_section(sections, ".text", sizeof(text_stub), EXPACK_PE_CONTAINER_TEXT_RVA, EXPACK_PE_CONTAINER_TEXT_RAW_SIZE, EXPACK_PE_CONTAINER_TEXT_RAW_OFFSET, 0x60000020U);
    expack_write_pe_section(sections + 40U, ".expack", expack_virtual_size, EXPACK_PE_CONTAINER_EXPACK_RVA, expack_raw_size, expack_raw_offset, 0x40000040U);
    memcpy(output_data + EXPACK_PE_CONTAINER_TEXT_RAW_OFFSET, text_stub, sizeof(text_stub));
    expack_write_pe_container_metadata(output_data + expack_raw_offset, format, candidate, original_size);
    memcpy(output_data + expack_raw_offset + EXPACK_PE_CONTAINER_METADATA_SIZE, candidate->payload, candidate->payload_size);

    output_fd = platform_open_write(output_path, 0755U);
    if (output_fd < 0) {
        rt_free(output_data);
        return -1;
    }
    if (rt_write_all(output_fd, output_data, file_size) != 0) {
        platform_close(output_fd);
        rt_free(output_data);
        return -1;
    }
    rt_free(output_data);
    if (platform_close(output_fd) != 0) {
        return -1;
    }
    return 0;
}

static int expack_macho_container_subtype(unsigned int cputype, unsigned int *subtype_out) {
    if (cputype == EXPACK_MACHO_CPU_ARM64) {
        *subtype_out = 0U;
        return 0;
    }
    if (cputype == EXPACK_MACHO_CPU_X86_64) {
        *subtype_out = 3U;
        return 0;
    }
    return -1;
}

typedef struct {
    const unsigned char *stub;
    size_t stub_size;
    unsigned int payload_address_offset;
    unsigned int payload_size_offset;
    unsigned int original_size_offset;
} ExpackMachoRunnerDescriptor;

static int expack_macho_container_runner(unsigned int cputype, unsigned int codec, ExpackMachoRunnerDescriptor *runner_out) {
    static const unsigned char arm64_raw_runner_stub[] = {
        0xf5, 0x03, 0x01, 0xaa, 0xf6, 0x03, 0x02, 0xaa, 0xff, 0x03, 0x01, 0xd1,
        0xf7, 0x03, 0x00, 0x91, 0x45, 0x07, 0x00, 0x10, 0x26, 0x03, 0x80, 0xd2,
        0xa7, 0x14, 0x40, 0x38, 0xe7, 0x16, 0x00, 0x38, 0xc6, 0x04, 0x00, 0xf1,
        0xa1, 0xff, 0xff, 0x54, 0xf7, 0x03, 0x00, 0x91, 0x90, 0x02, 0x80, 0xd2,
        0x01, 0x10, 0x00, 0xd4, 0xe1, 0x42, 0x00, 0x91, 0x02, 0x01, 0x80, 0xd2,
        0x03, 0xfc, 0x5c, 0xd3, 0x63, 0x0c, 0x40, 0x92, 0x64, 0x06, 0x00, 0x30,
        0x83, 0x68, 0x63, 0x38, 0x23, 0x14, 0x00, 0x38, 0x00, 0xec, 0x7c, 0xd3,
        0x42, 0x04, 0x00, 0xf1, 0x21, 0xff, 0xff, 0x54, 0xe0, 0x03, 0x17, 0xaa,
        0x21, 0xc0, 0x81, 0xd2, 0x02, 0x38, 0x80, 0xd2, 0xb0, 0x00, 0x80, 0xd2,
        0x01, 0x10, 0x00, 0xd4, 0x62, 0x03, 0x00, 0x54, 0xf3, 0x03, 0x00, 0xaa,
        0x89, 0x03, 0x00, 0x10, 0x38, 0x01, 0x40, 0xf9, 0x38, 0x01, 0x18, 0x8b,
        0x69, 0x03, 0x00, 0x10, 0x39, 0x01, 0x40, 0xf9, 0x99, 0x01, 0x00, 0xb4,
        0xe0, 0x03, 0x13, 0xaa, 0xe1, 0x03, 0x18, 0xaa, 0xe2, 0x03, 0x19, 0xaa,
        0x90, 0x00, 0x80, 0xd2,
        0x01, 0x10, 0x00, 0xd4, 0xc2, 0x01, 0x00, 0x54, 0x1f, 0x04, 0x00, 0xf1,
        0x8b, 0x01, 0x00, 0x54, 0x18, 0x03, 0x00, 0x8b, 0x39, 0x03, 0x00, 0xcb,
        0xf5, 0xff, 0xff, 0x17, 0xe0, 0x03, 0x13, 0xaa, 0xd0, 0x00, 0x80, 0xd2,
        0x01, 0x10, 0x00, 0xd4, 0xe0, 0x03, 0x17, 0xaa, 0xe1, 0x03, 0x15, 0xaa,
        0xe2, 0x03, 0x16, 0xaa, 0x70, 0x07, 0x80, 0xd2, 0x01, 0x10, 0x00, 0xd4,
        0xe0, 0x0f, 0x80, 0xd2, 0x30, 0x00, 0x80, 0xd2, 0x01, 0x10, 0x00, 0xd4,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x2f, 0x74, 0x6d, 0x70,
        0x2f, 0x65, 0x78, 0x70, 0x61, 0x63, 0x6b, 0x2d, 0x72, 0x75, 0x6e, 0x2d,
        0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x00, 0x30, 0x31, 0x32,
        0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65,
        0x66
    };
    static const unsigned char arm64_lzrep_runner_stub[] = {
#include "macho_arm64_lzrep_runner.inc"
    };
    static const unsigned char x86_64_exit_stub[] = {
        0xb8, 0x01, 0x00, 0x00, 0x02,
        0xbf, 0x7f, 0x00, 0x00, 0x00,
        0x0f, 0x05
    };

    runner_out->stub = 0;
    runner_out->stub_size = 0U;
    runner_out->payload_address_offset = (unsigned int)-1;
    runner_out->payload_size_offset = (unsigned int)-1;
    runner_out->original_size_offset = (unsigned int)-1;
    if (cputype == EXPACK_MACHO_CPU_ARM64 && codec == EXPACK_CODEC_RAW) {
        runner_out->stub = arm64_raw_runner_stub;
        runner_out->stub_size = sizeof(arm64_raw_runner_stub);
        runner_out->payload_address_offset = EXPACK_MACHO_ARM64_RUNNER_PAYLOAD_ADDRESS_OFFSET;
        runner_out->payload_size_offset = EXPACK_MACHO_ARM64_RUNNER_PAYLOAD_SIZE_OFFSET;
        return 0;
    }
    if (cputype == EXPACK_MACHO_CPU_ARM64 && codec == EXPACK_CODEC_LZREP) {
        runner_out->stub = arm64_lzrep_runner_stub;
        runner_out->stub_size = sizeof(arm64_lzrep_runner_stub);
        runner_out->payload_address_offset = EXPACK_MACHO_ARM64_LZREP_RUNNER_PAYLOAD_ADDRESS_OFFSET;
        runner_out->payload_size_offset = EXPACK_MACHO_ARM64_LZREP_RUNNER_PAYLOAD_SIZE_OFFSET;
        runner_out->original_size_offset = EXPACK_MACHO_ARM64_LZREP_RUNNER_ORIGINAL_SIZE_OFFSET;
        return 0;
    }
    if (cputype == EXPACK_MACHO_CPU_X86_64) {
        runner_out->stub = x86_64_exit_stub;
        runner_out->stub_size = sizeof(x86_64_exit_stub);
        return 0;
    }
    return -1;
}

static int expack_patch_macho_runner(const ExpackMachoRunnerDescriptor *runner, unsigned char *stub, size_t original_size, const ExpackCandidate *candidate) {
    if (runner->payload_address_offset != (unsigned int)-1) {
        if (runner->payload_address_offset + 8U > runner->stub_size || runner->payload_size_offset + 8U > runner->stub_size) {
            return -1;
        }
        archive_store_u64_le(stub + runner->payload_address_offset, (unsigned long long)(runner->stub_size + EXPACK_MACHO_CONTAINER_METADATA_SIZE - runner->payload_address_offset));
        archive_store_u64_le(stub + runner->payload_size_offset, (unsigned long long)candidate->payload_size);
    }
    if (runner->original_size_offset != (unsigned int)-1) {
        if (runner->original_size_offset + 8U > runner->stub_size) {
            return -1;
        }
        archive_store_u64_le(stub + runner->original_size_offset, (unsigned long long)original_size);
    }
    return 0;
}

static void expack_write_macho_section(unsigned char *section, const char *section_name, const char *segment_name, unsigned long long address, unsigned long long size, unsigned int offset, unsigned int alignment, unsigned int flags) {
    expack_store_fixed_name(section, section_name);
    expack_store_fixed_name(section + 16U, segment_name);
    archive_store_u64_le(section + 32U, address);
    archive_store_u64_le(section + 40U, size);
    archive_store_u32_le(section + 48U, offset);
    archive_store_u32_le(section + 52U, alignment);
    archive_store_u32_le(section + 56U, 0U);
    archive_store_u32_le(section + 60U, 0U);
    archive_store_u32_le(section + 64U, flags);
    archive_store_u32_le(section + 68U, 0U);
    archive_store_u32_le(section + 72U, 0U);
    archive_store_u32_le(section + 76U, 0U);
}

static void expack_write_macho_segment64(unsigned char *segment, const char *segment_name, unsigned long long vmaddr, unsigned long long vmsize, unsigned long long fileoff, unsigned long long filesize, unsigned int maxprot, unsigned int initprot, unsigned int nsects, unsigned int flags) {
    archive_store_u32_le(segment + 0U, EXPACK_MACHO_LC_SEGMENT_64);
    archive_store_u32_le(segment + 4U, 72U + 80U * nsects);
    expack_store_fixed_name(segment + 8U, segment_name);
    archive_store_u64_le(segment + 24U, vmaddr);
    archive_store_u64_le(segment + 32U, vmsize);
    archive_store_u64_le(segment + 40U, fileoff);
    archive_store_u64_le(segment + 48U, filesize);
    archive_store_u32_le(segment + 56U, maxprot);
    archive_store_u32_le(segment + 60U, initprot);
    archive_store_u32_le(segment + 64U, nsects);
    archive_store_u32_le(segment + 68U, flags);
}

static void expack_store_u32_be(unsigned char *out, unsigned int value) {
    out[0] = (unsigned char)((value >> 24U) & 0xffU);
    out[1] = (unsigned char)((value >> 16U) & 0xffU);
    out[2] = (unsigned char)((value >> 8U) & 0xffU);
    out[3] = (unsigned char)(value & 0xffU);
}

typedef struct {
    CryptoSha256Context context;
    unsigned char *hashes;
    size_t hash_count;
    size_t hash_index;
    size_t page_used;
} ExpackMachoPageHasher;

static int expack_macho_page_hasher_init(ExpackMachoPageHasher *hasher, size_t hash_count) {
    hasher->hashes = (unsigned char *)rt_malloc(hash_count * CRYPTO_SHA256_DIGEST_SIZE);
    if (hasher->hashes == 0 && hash_count != 0U) {
        return -1;
    }
    hasher->hash_count = hash_count;
    hasher->hash_index = 0U;
    hasher->page_used = 0U;
    crypto_sha256_init(&hasher->context);
    return 0;
}

static int expack_macho_page_hasher_update(ExpackMachoPageHasher *hasher, const unsigned char *data, size_t size) {
    size_t offset = 0U;

    while (offset < size) {
        size_t space = EXPACK_MACHO_CODE_SIGNATURE_PAGE_SIZE - hasher->page_used;
        size_t chunk = size - offset;
        if (chunk > space) {
            chunk = space;
        }
        crypto_sha256_update(&hasher->context, data + offset, chunk);
        hasher->page_used += chunk;
        offset += chunk;
        if (hasher->page_used == EXPACK_MACHO_CODE_SIGNATURE_PAGE_SIZE) {
            if (hasher->hash_index >= hasher->hash_count) {
                return -1;
            }
            crypto_sha256_final(&hasher->context, hasher->hashes + hasher->hash_index * CRYPTO_SHA256_DIGEST_SIZE);
            ++hasher->hash_index;
            hasher->page_used = 0U;
            crypto_sha256_init(&hasher->context);
        }
    }
    return 0;
}

static int expack_macho_page_hasher_finish(ExpackMachoPageHasher *hasher) {
    if (hasher->page_used != 0U) {
        if (hasher->hash_index >= hasher->hash_count) {
            return -1;
        }
        crypto_sha256_final(&hasher->context, hasher->hashes + hasher->hash_index * CRYPTO_SHA256_DIGEST_SIZE);
        ++hasher->hash_index;
        hasher->page_used = 0U;
    }
    return hasher->hash_index == hasher->hash_count ? 0 : -1;
}

static int expack_macho_page_hasher_update_zeroes(ExpackMachoPageHasher *hasher, unsigned long long size) {
    static const unsigned char zero_pad[64] = {0U};
    while (size != 0ULL) {
        size_t chunk = size > (unsigned long long)sizeof(zero_pad) ? sizeof(zero_pad) : (size_t)size;
        if (expack_macho_page_hasher_update(hasher, zero_pad, chunk) != 0) {
            return -1;
        }
        size -= (unsigned long long)chunk;
    }
    return 0;
}

static unsigned char *expack_make_macho_code_signature(const unsigned char *header, size_t header_size, const unsigned char *stub, size_t stub_size, const unsigned char *metadata, size_t metadata_size, const unsigned char *payload, size_t payload_size, unsigned long long code_limit, size_t *signature_size_out) {
    const unsigned int superblob_header_size = 20U;
    const unsigned int code_directory_fixed_size = 88U;
    const unsigned int hash_offset = code_directory_fixed_size + EXPACK_MACHO_CODE_DIRECTORY_IDENT_SIZE;
    unsigned int code_slots;
    unsigned int code_directory_size;
    unsigned int signature_size;
    unsigned char *signature;
    ExpackMachoPageHasher hasher;
    unsigned long long written_size = (unsigned long long)header_size + (unsigned long long)stub_size + (unsigned long long)metadata_size + (unsigned long long)payload_size;

    if (code_limit == 0ULL || code_limit > 0xffffffffULL || (code_limit % EXPACK_MACHO_CODE_SIGNATURE_PAGE_SIZE) != 0ULL || written_size > code_limit) {
        return 0;
    }
    code_slots = (unsigned int)(code_limit / EXPACK_MACHO_CODE_SIGNATURE_PAGE_SIZE);
    if (code_slots == 0U || code_slots > ((unsigned int)-1 - hash_offset) / CRYPTO_SHA256_DIGEST_SIZE) {
        return 0;
    }
    code_directory_size = hash_offset + code_slots * CRYPTO_SHA256_DIGEST_SIZE;
    signature_size = superblob_header_size + code_directory_size;
    signature = (unsigned char *)rt_malloc(signature_size);
    if (signature == 0) {
        return 0;
    }
    if (expack_macho_page_hasher_init(&hasher, code_slots) != 0) {
        rt_free(signature);
        return 0;
    }
    if (expack_macho_page_hasher_update(&hasher, header, header_size) != 0 ||
        expack_macho_page_hasher_update(&hasher, stub, stub_size) != 0 ||
        expack_macho_page_hasher_update(&hasher, metadata, metadata_size) != 0 ||
        expack_macho_page_hasher_update(&hasher, payload, payload_size) != 0 ||
        expack_macho_page_hasher_update_zeroes(&hasher, code_limit - written_size) != 0 ||
        expack_macho_page_hasher_finish(&hasher) != 0) {
        rt_free(hasher.hashes);
        rt_free(signature);
        return 0;
    }

    memset(signature, 0, signature_size);
    expack_store_u32_be(signature + 0U, 0xfade0cc0U);
    expack_store_u32_be(signature + 4U, signature_size);
    expack_store_u32_be(signature + 8U, 1U);
    expack_store_u32_be(signature + 12U, 0U);
    expack_store_u32_be(signature + 16U, superblob_header_size);

    expack_store_u32_be(signature + superblob_header_size + 0U, 0xfade0c02U);
    expack_store_u32_be(signature + superblob_header_size + 4U, code_directory_size);
    expack_store_u32_be(signature + superblob_header_size + 8U, 0x20400U);
    expack_store_u32_be(signature + superblob_header_size + 12U, 0x2U);
    expack_store_u32_be(signature + superblob_header_size + 16U, hash_offset);
    expack_store_u32_be(signature + superblob_header_size + 20U, code_directory_fixed_size);
    expack_store_u32_be(signature + superblob_header_size + 24U, 0U);
    expack_store_u32_be(signature + superblob_header_size + 28U, code_slots);
    expack_store_u32_be(signature + superblob_header_size + 32U, (unsigned int)code_limit);
    signature[superblob_header_size + 36U] = CRYPTO_SHA256_DIGEST_SIZE;
    signature[superblob_header_size + 37U] = 2U;
    signature[superblob_header_size + 39U] = 14U;
    memcpy(signature + superblob_header_size + code_directory_fixed_size, EXPACK_MACHO_CODE_DIRECTORY_IDENT, EXPACK_MACHO_CODE_DIRECTORY_IDENT_SIZE);
    memcpy(signature + superblob_header_size + hash_offset, hasher.hashes, (size_t)code_slots * CRYPTO_SHA256_DIGEST_SIZE);
    rt_free(hasher.hashes);
    *signature_size_out = signature_size;
    return signature;
}

static void expack_write_macho_container_metadata(unsigned char *metadata, const ExpackCandidate *candidate, size_t original_size) {
    memset(metadata, 0, EXPACK_MACHO_CONTAINER_METADATA_SIZE);
    memcpy(metadata, "EXPACKM1", 8U);
    archive_store_u32_le(metadata + 8U, EXPACK_MACHO_CONTAINER_VERSION);
    archive_store_u32_le(metadata + 12U, candidate->codec);
    archive_store_u64_le(metadata + 16U, (unsigned long long)original_size);
    archive_store_u64_le(metadata + 24U, (unsigned long long)candidate->payload_size);
    archive_store_u32_le(metadata + 32U, candidate->codec == EXPACK_CODEC_LZSS ? candidate->lzss_profile->profile_id : 0xffffffffU);
}

static int expack_write_macho_container(const ExpackInputFormat *format, const char *output_path, const ExpackCandidate *candidate, size_t original_size) {
    enum { header_size = 32U, pagezero_command_size = 72U, text_command_size = 72U + 80U + 80U, linkedit_command_size = 72U, dylinker_command_size = 32U, build_version_command_size = 32U, main_command_size = 24U, code_signature_command_size = 16U };
    unsigned char header[header_size + pagezero_command_size + text_command_size + linkedit_command_size + dylinker_command_size + build_version_command_size + main_command_size + code_signature_command_size];
    static const unsigned char zero_pad[64] = {0U};
    unsigned char metadata[EXPACK_MACHO_CONTAINER_METADATA_SIZE];
    unsigned char *signature;
    unsigned char *patched_stub;
    const unsigned char *stub;
    ExpackMachoRunnerDescriptor runner;
    size_t stub_size;
    size_t signature_size;
    unsigned int subtype;
    unsigned int commands_size = pagezero_command_size + text_command_size + linkedit_command_size + dylinker_command_size + build_version_command_size + main_command_size + code_signature_command_size;
    unsigned int code_offset = header_size + commands_size;
    unsigned int payload_offset;
    unsigned long long payload_size = (unsigned long long)EXPACK_MACHO_CONTAINER_METADATA_SIZE + (unsigned long long)candidate->payload_size;
    unsigned long long file_size;
    unsigned long long text_file_size;
    int output_fd;

    if (expack_macho_container_subtype(format->info.macho.cputype, &subtype) != 0) {
        return -1;
    }
    if (expack_macho_container_runner(format->info.macho.cputype, candidate->codec, &runner) != 0) {
        return -1;
    }
    stub = runner.stub;
    stub_size = runner.stub_size;
    if (stub == 0 || stub_size == 0U || stub_size > (size_t)((unsigned int)-1 - code_offset)) {
        return -1;
    }
    patched_stub = (unsigned char *)rt_malloc(stub_size);
    if (patched_stub == 0) {
        return -1;
    }
    memcpy(patched_stub, stub, stub_size);
    stub = patched_stub;
    payload_offset = code_offset + (unsigned int)stub_size;
    file_size = (unsigned long long)payload_offset + payload_size;
    text_file_size = expack_align_up_u64(file_size, 0x4000ULL);

    memset(header, 0, sizeof(header));
    archive_store_u32_le(header + 0U, 0xfeedfacfU);
    archive_store_u32_le(header + 4U, format->info.macho.cputype);
    archive_store_u32_le(header + 8U, subtype);
    archive_store_u32_le(header + 12U, EXPACK_MACHO_TYPE_EXECUTE);
    archive_store_u32_le(header + 16U, 7U);
    archive_store_u32_le(header + 20U, commands_size);
    archive_store_u32_le(header + 24U, EXPACK_MACHO_FLAG_NOUNDEFS | EXPACK_MACHO_FLAG_DYLDLINK | EXPACK_MACHO_FLAG_TWOLEVEL | EXPACK_MACHO_FLAG_PIE);

    expack_write_macho_segment64(header + 32U, "__PAGEZERO", 0ULL, EXPACK_MACHO_CONTAINER_BASE, 0ULL, 0ULL, 0U, 0U, 0U, 0U);
    expack_write_macho_segment64(header + 104U, "__TEXT", EXPACK_MACHO_CONTAINER_BASE, text_file_size, 0ULL, text_file_size, EXPACK_MACHO_VM_PROT_READ | EXPACK_MACHO_VM_PROT_EXECUTE, EXPACK_MACHO_VM_PROT_READ | EXPACK_MACHO_VM_PROT_EXECUTE, 2U, 0U);
    expack_write_macho_section(header + 176U, "__text", "__TEXT", EXPACK_MACHO_CONTAINER_BASE + (unsigned long long)code_offset, (unsigned long long)stub_size, code_offset, 2U, 0x80000400U);
    expack_write_macho_section(header + 256U, "__expack", "__TEXT", EXPACK_MACHO_CONTAINER_BASE + (unsigned long long)payload_offset, payload_size, payload_offset, 3U, 0U);
    expack_write_macho_segment64(header + 336U, "__LINKEDIT", EXPACK_MACHO_CONTAINER_BASE + text_file_size, 0x4000ULL, text_file_size, 0ULL, EXPACK_MACHO_VM_PROT_READ, EXPACK_MACHO_VM_PROT_READ, 0U, 0U);

    archive_store_u32_le(header + 408U, EXPACK_MACHO_LC_LOAD_DYLINKER);
    archive_store_u32_le(header + 412U, dylinker_command_size);
    archive_store_u32_le(header + 416U, 12U);
    memcpy(header + 420U, "/usr/lib/dyld", 14U);

    archive_store_u32_le(header + 440U, EXPACK_MACHO_LC_BUILD_VERSION);
    archive_store_u32_le(header + 444U, build_version_command_size);
    archive_store_u32_le(header + 448U, 1U);
    archive_store_u32_le(header + 452U, 0x000b0000U);
    archive_store_u32_le(header + 456U, 0x000b0000U);
    archive_store_u32_le(header + 460U, 1U);
    archive_store_u32_le(header + 464U, 3U);

    archive_store_u32_le(header + 472U, EXPACK_MACHO_LC_MAIN);
    archive_store_u32_le(header + 476U, main_command_size);
    archive_store_u64_le(header + 480U, code_offset);

    archive_store_u32_le(header + 496U, EXPACK_MACHO_LC_CODE_SIGNATURE);
    archive_store_u32_le(header + 500U, code_signature_command_size);
    archive_store_u32_le(header + 504U, (unsigned int)text_file_size);

    expack_write_macho_container_metadata(metadata, candidate, original_size);
    if (expack_patch_macho_runner(&runner, patched_stub, original_size, candidate) != 0) {
        rt_free(patched_stub);
        return -1;
    }
    signature = expack_make_macho_code_signature(header, sizeof(header), stub, stub_size, metadata, sizeof(metadata), candidate->payload, candidate->payload_size, text_file_size, &signature_size);
    if (signature == 0 || signature_size > (size_t)((unsigned int)-1)) {
        rt_free(patched_stub);
        return -1;
    }
    archive_store_u32_le(header + 384U, (unsigned int)signature_size);
    archive_store_u32_le(header + 508U, (unsigned int)signature_size);
    rt_free(signature);
    signature = expack_make_macho_code_signature(header, sizeof(header), stub, stub_size, metadata, sizeof(metadata), candidate->payload, candidate->payload_size, text_file_size, &signature_size);
    if (signature == 0 || signature_size > (size_t)((unsigned int)-1)) {
        rt_free(patched_stub);
        return -1;
    }

    output_fd = platform_open_write(output_path, 0755U);
    if (output_fd < 0) {
        rt_free(signature);
        rt_free(patched_stub);
        return -1;
    }
    if (rt_write_all(output_fd, header, sizeof(header)) != 0 || rt_write_all(output_fd, stub, stub_size) != 0 ||
        rt_write_all(output_fd, metadata, sizeof(metadata)) != 0 || rt_write_all(output_fd, candidate->payload, candidate->payload_size) != 0) {
        platform_close(output_fd);
        rt_free(signature);
        rt_free(patched_stub);
        return -1;
    }
    while (file_size < text_file_size) {
        size_t chunk = (size_t)(text_file_size - file_size);
        if (chunk > sizeof(zero_pad)) {
            chunk = sizeof(zero_pad);
        }
        if (rt_write_all(output_fd, zero_pad, chunk) != 0) {
            platform_close(output_fd);
            rt_free(signature);
            rt_free(patched_stub);
            return -1;
        }
        file_size += (unsigned long long)chunk;
    }
    if (rt_write_all(output_fd, signature, signature_size) != 0) {
        platform_close(output_fd);
        rt_free(signature);
        rt_free(patched_stub);
        return -1;
    }
    rt_free(signature);
    rt_free(patched_stub);
    if (platform_close(output_fd) != 0) {
        return -1;
    }
    return 0;
}

static int expack_backend_can_write_packed(const ExpackInputFormat *format) {
    (void)format;
    return 1;
}

static int expack_backend_cannot_write_packed(const ExpackInputFormat *format) {
    (void)format;
    return 0;
}

static int expack_backend_cannot_write_container(const ExpackInputFormat *format, unsigned int output_kind) {
    (void)format;
    (void)output_kind;
    return 0;
}

static int expack_elf_write_packed(const ExpackInputFormat *format, const char *output_path, const ExpackCandidate *candidate, size_t original_size) {
    (void)format;
    return expack_write_packed_elf(output_path, candidate, original_size);
}

static int expack_macho_can_write_container(const ExpackInputFormat *format, unsigned int output_kind) {
    return format->kind == EXPACK_FORMAT_MACHO && output_kind == EXPACK_OUTPUT_KIND_MACHO_CONTAINER;
}

static int expack_macho_prepare_container_candidate(const ExpackInputFormat *format, const ExpackImage *image, ExpackCandidate *candidate) {
    if (format->info.macho.cputype == EXPACK_MACHO_CPU_ARM64 && candidate->codec != EXPACK_CODEC_LZREP) {
        return expack_make_raw_candidate(image->data, image->size, candidate);
    }
    return 0;
}

static int expack_macho_write_container_backend(const ExpackInputFormat *format, const char *output_path, const ExpackCandidate *candidate, size_t original_size) {
    return expack_write_macho_container(format, output_path, candidate, original_size);
}

static int expack_pe_can_write_container(const ExpackInputFormat *format, unsigned int output_kind) {
    return format->kind == EXPACK_FORMAT_PE_COFF && output_kind == EXPACK_OUTPUT_KIND_PE_CONTAINER;
}

static int expack_pe_write_container_backend(const ExpackInputFormat *format, const char *output_path, const ExpackCandidate *candidate, size_t original_size) {
    return expack_write_pe_container(format, output_path, candidate, original_size);
}

static void expack_macho_write_container_success(const char *output_path, const ExpackCandidate *candidate) {
    rt_write_cstr(1, "expack: wrote Mach-O prototype container ");
    rt_write_cstr(1, output_path);
    rt_write_cstr(1, " with codec ");
    expack_write_candidate_name(candidate);
    rt_write_cstr(1, "\n");
}

static void expack_pe_write_container_success(const char *output_path, const ExpackCandidate *candidate) {
    rt_write_cstr(1, "expack: wrote PE/COFF prototype container ");
    rt_write_cstr(1, output_path);
    rt_write_cstr(1, " with codec ");
    expack_write_candidate_name(candidate);
    rt_write_cstr(1, "\n");
}

static const ExpackOutputBackend expack_output_backends[] = {
    {
        EXPACK_FORMAT_ELF64_X86_64,
        EXPACK_OUTPUT_KIND_ELF_PACKED,
        "cannot write packed output for this executable format",
        expack_backend_can_write_packed,
        expack_backend_cannot_write_container,
        0,
        expack_elf_write_packed,
        0,
        0
    },
    {
        EXPACK_FORMAT_MACHO,
        EXPACK_OUTPUT_KIND_MACHO_CONTAINER,
        "Mach-O compression analysis is supported, but writing compressed runnable Mach-O output needs a native decoder backend",
        expack_backend_cannot_write_packed,
        expack_macho_can_write_container,
        expack_macho_prepare_container_candidate,
        0,
        expack_macho_write_container_backend,
        expack_macho_write_container_success
    },
    {
        EXPACK_FORMAT_PE_COFF,
        EXPACK_OUTPUT_KIND_PE_CONTAINER,
        "PE/COFF compression analysis is supported, but writing packed output needs a PE/COFF container backend",
        expack_backend_cannot_write_packed,
        expack_pe_can_write_container,
        0,
        0,
        expack_pe_write_container_backend,
        expack_pe_write_container_success
    }
};

static const ExpackOutputBackend *expack_find_output_backend(const ExpackInputFormat *format) {
    size_t index;

    for (index = 0U; index < sizeof(expack_output_backends) / sizeof(expack_output_backends[0]); ++index) {
        if (expack_output_backends[index].format_kind == format->kind) {
            return &expack_output_backends[index];
        }
    }
    return 0;
}

static int expack_write_packed_output(const ExpackOutputBackend *backend, const ExpackInputFormat *format, const char *output_path, const ExpackCandidate *candidate, size_t original_size) {
    if (backend == 0 || backend->write_packed == 0) {
        return -1;
    }
    return backend->write_packed(format, output_path, candidate, original_size);
}

static int expack_write_container_output(const ExpackOutputBackend *backend, const ExpackInputFormat *format, const char *output_path, const ExpackCandidate *candidate, size_t original_size, unsigned int output_kind) {
    if (backend == 0 || backend->write_container == 0 || backend->can_write_container == 0 || !backend->can_write_container(format, output_kind)) {
        return -1;
    }
    return backend->write_container(format, output_path, candidate, original_size);
}

