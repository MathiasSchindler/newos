#include "internal.h"

#include "pe_runner_template.inc"

typedef struct {
    const unsigned char *data;
    size_t size;
    unsigned int metadata_offset_patch;
} ExpackPeRunnerTemplate;

static int expack_pe_runner_for_codec(unsigned int codec, ExpackPeRunnerTemplate *runner_out) {
    if (codec == EXPACK_CODEC_LZSS) {
        runner_out->data = expack_pe_lzss_runner_template;
        runner_out->size = sizeof(expack_pe_lzss_runner_template);
        runner_out->metadata_offset_patch = EXPACK_PE_LZSS_RUNNER_TEMPLATE_METADATA_OFFSET_PATCH;
        return 0;
    }
    if (codec == EXPACK_CODEC_LZREP) {
        runner_out->data = expack_pe_lzrep_runner_template;
        runner_out->size = sizeof(expack_pe_lzrep_runner_template);
        runner_out->metadata_offset_patch = EXPACK_PE_LZREP_RUNNER_TEMPLATE_METADATA_OFFSET_PATCH;
        return 0;
    }
    if (codec == EXPACK_CODEC_LZSS_BCJ) {
        runner_out->data = expack_pe_lzss_bcj_runner_template;
        runner_out->size = sizeof(expack_pe_lzss_bcj_runner_template);
        runner_out->metadata_offset_patch = EXPACK_PE_LZSS_BCJ_RUNNER_TEMPLATE_METADATA_OFFSET_PATCH;
        return 0;
    }
    return -1;
}

static unsigned long long expack_pe_runner_size_for_codec(unsigned int codec) {
    ExpackPeRunnerTemplate runner;
    if (expack_pe_runner_for_codec(codec, &runner) != 0) {
        return EXPACK_CANDIDATE_UNSUPPORTED_SIZE;
    }
    return (unsigned long long)runner.size;
}

static void expack_write_pe_container_metadata(unsigned char *metadata, const ExpackInputFormat *format, const ExpackCandidate *candidate, size_t original_size) {
    (void)format;
    memset(metadata, 0, EXPACK_PE_CONTAINER_METADATA_SIZE);
    memcpy(metadata, "EXPACKP1", 8U);
    archive_store_u64_le(metadata + 8U, (unsigned long long)original_size);
    archive_store_u64_le(metadata + 16U, (unsigned long long)candidate->payload_size);
}

static int expack_write_pe_container(const ExpackInputFormat *format, const char *output_path, const ExpackCandidate *candidate, size_t original_size) {
    unsigned char metadata[EXPACK_PE_CONTAINER_METADATA_SIZE];
    ExpackPeRunnerTemplate runner_template;
    unsigned char *runner;
    int output_fd;

    if (!((candidate->codec == EXPACK_CODEC_RAW && original_size == candidate->payload_size) || candidate->codec == EXPACK_CODEC_LZSS || candidate->codec == EXPACK_CODEC_LZREP || candidate->codec == EXPACK_CODEC_LZSS_BCJ)) {
        return -1;
    }
    if (candidate->codec == EXPACK_CODEC_LZSS && (candidate->lzss_profile == 0 || candidate->lzss_profile->profile_id != COMPRESSION_LZSS_PROFILE_WIDE_WINDOW)) {
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
    if (expack_pe_runner_for_codec(candidate->codec, &runner_template) != 0 || runner_template.metadata_offset_patch + 8U > runner_template.size) {
        return -1;
    }
    runner = (unsigned char *)rt_malloc(runner_template.size);
    if (runner == 0) {
        return -1;
    }
    memcpy(runner, runner_template.data, runner_template.size);
    archive_store_u64_le(runner + runner_template.metadata_offset_patch, (unsigned long long)runner_template.size);
    expack_write_pe_container_metadata(metadata, format, candidate, original_size);

    output_fd = platform_open_write(output_path, 0755U);
    if (output_fd < 0) {
        rt_free(runner);
        return -1;
    }
    if (rt_write_all(output_fd, runner, runner_template.size) != 0 ||
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