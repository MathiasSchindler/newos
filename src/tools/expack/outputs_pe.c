#include "internal.h"

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