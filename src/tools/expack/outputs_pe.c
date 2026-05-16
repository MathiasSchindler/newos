#include "internal.h"

#include "pe_runner_template.inc"

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
    unsigned char metadata[EXPACK_PE_CONTAINER_METADATA_SIZE];
    unsigned char *runner;
    int output_fd;

    if (!((candidate->codec == EXPACK_CODEC_RAW && original_size == candidate->payload_size) || candidate->codec == EXPACK_CODEC_LZREP)) {
        return -1;
    }
    if (candidate->codec == EXPACK_CODEC_RAW) {
        output_fd = platform_open_write(output_path, 0755U);
        if (output_fd < 0) {
            return -1;
        }
        if (rt_write_all(output_fd, candidate->payload, candidate->payload_size) != 0) {
            platform_close(output_fd);
            return -1;
        }
        if (platform_close(output_fd) != 0) {
            return -1;
        }
        return 0;
    }
    if (EXPACK_PE_RUNNER_METADATA_OFFSET_PATCH + 8U > sizeof(expack_pe_runner_template)) {
        return -1;
    }
    runner = (unsigned char *)rt_malloc(sizeof(expack_pe_runner_template));
    if (runner == 0) {
        return -1;
    }
    memcpy(runner, expack_pe_runner_template, sizeof(expack_pe_runner_template));
    archive_store_u64_le(runner + EXPACK_PE_RUNNER_METADATA_OFFSET_PATCH, (unsigned long long)sizeof(expack_pe_runner_template));
    expack_write_pe_container_metadata(metadata, format, candidate, original_size);

    output_fd = platform_open_write(output_path, 0755U);
    if (output_fd < 0) {
        rt_free(runner);
        return -1;
    }
    if (rt_write_all(output_fd, runner, sizeof(expack_pe_runner_template)) != 0 ||
        rt_write_all(output_fd, metadata, sizeof(metadata)) != 0 ||
        rt_write_all(output_fd, candidate->payload, candidate->payload_size) != 0) {
        platform_close(output_fd);
        rt_free(runner);
        return -1;
    }
    rt_free(runner);
    if (platform_close(output_fd) != 0) {
        return -1;
    }
    return 0;
}