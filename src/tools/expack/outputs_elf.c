#include "internal.h"

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
    unsigned char *stub;
    unsigned long long file_size = (unsigned long long)sizeof(header) + (unsigned long long)candidate->stub_size + (unsigned long long)candidate->payload_size;
    int output_fd;

    stub = (unsigned char *)rt_malloc(candidate->stub_size == 0U ? 1U : candidate->stub_size);
    if (stub == 0) {
        return -1;
    }
    expack_write_elf_header(header, file_size);
    memcpy(stub, candidate->stub, candidate->stub_size);
    if (expack_patch_stub(stub, candidate, original_size) != 0) {
        rt_free(stub);
        return -1;
    }

    output_fd = platform_open_write(output_path, 0755U);
    if (output_fd < 0) {
        rt_free(stub);
        return -1;
    }
    if (rt_write_all(output_fd, header, sizeof(header)) != 0 || rt_write_all(output_fd, stub, candidate->stub_size) != 0 ||
        rt_write_all(output_fd, candidate->payload, candidate->payload_size) != 0) {
        platform_close(output_fd);
        rt_free(stub);
        return -1;
    }
    rt_free(stub);
    if (platform_close(output_fd) != 0) {
        return -1;
    }
    return 0;
}